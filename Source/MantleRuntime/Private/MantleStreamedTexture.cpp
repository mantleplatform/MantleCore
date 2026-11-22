// Copyright Joshua Eger. All Rights Reserved.
#include "MantleStreamedTexture.h"

#include "Engine/Texture.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "MantleCookedPacking.h"
#include "MantleRuntime.h"
#include "Streaming/TextureMipDataProvider.h"
#include "TextureResource.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleStreamedTexturePrivate
{
	TAtomic<int64> GMipsServed{ 0 };
	TAtomic<int64> GBytesServed{ 0 };
	TAtomic<int64> GPackedBytesRead{ 0 };
	TAtomic<int64> GFailed{ 0 };
	TAtomic<int64> GRequests{ 0 };
	TAtomic<int64> GSumRequestUs{ 0 };
	TAtomic<int64> GWorstRequestUs{ 0 };

	/**
	 * One stream-in of one texture: reads the wanted mips from the file and inflates them into the engine's buffers. GetMips runs on the update's
	 * worker thread (ETickThread::Async), so the read is done there, synchronously; nothing is left running when it returns, so a cancel has nothing to stop.
	 */
	class FCachedMipProvider final : public FTextureMipDataProvider
	{
	public:
		FCachedMipProvider(const UTexture* Texture, const TSharedRef<const FMantleTextureFileTable>& InTable)
			: FTextureMipDataProvider(Texture, ETickState::Init, ETickThread::Async)
			, Table(InTable)
			, TextureName(Texture ? Texture->GetFName() : NAME_None)
		{
		}

		virtual void Init(const FTextureUpdateContext& Context, const FTextureUpdateSyncOptions& SyncOptions) override
		{
			AdvanceTo(ETickState::GetMips, ETickThread::Async);
		}

		virtual int32 GetMips(const FTextureUpdateContext& Context, int32 StartingMipIndex, const FTextureMipInfoArray& MipInfos, const FTextureUpdateSyncOptions& SyncOptions) override
		{
			const double Start = FPlatformTime::Seconds();
			bOk = ReadMips(Context, StartingMipIndex, MipInfos);
			const int64 Us = static_cast<int64>((FPlatformTime::Seconds() - Start) * 1.0e6);
			++GRequests;
			GSumRequestUs += Us;
			int64 Worst = GWorstRequestUs.Load();
			while (Us > Worst && !GWorstRequestUs.CompareExchange(Worst, Us))
			{
			}
			if (!bOk)
			{
				++GFailed;
			}
			AdvanceTo(ETickState::PollMips, ETickThread::Async);
			// Every wanted mip is this provider's: a failure is reported by PollMips (the update is cancelled), not passed to the next provider, which has no file to read.
			return CurrentFirstLODIdx;
		}

		virtual bool PollMips(const FTextureUpdateSyncOptions& SyncOptions) override
		{
			AdvanceTo(ETickState::Done, ETickThread::None);
			return bOk;
		}

		virtual void CleanUp(const FTextureUpdateSyncOptions& SyncOptions) override
		{
			AdvanceTo(ETickState::Done, ETickThread::None);
		}

		virtual void Cancel(const FTextureUpdateSyncOptions& SyncOptions) override
		{
		}

		virtual ETickThread GetCancelThread() const override
		{
			return ETickThread::None;
		}

	private:
		bool Fail(const FString& Reason)
		{
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_TEXTURE_STREAM %s: %s (%s)"), *TextureName.ToString(), *Reason, *Table->Path);
			return false;
		}

		bool ReadMips(const FTextureUpdateContext& Context, const int32 StartingMipIndex, const FTextureMipInfoArray& MipInfos)
		{
			// The view starts at the resource's first mip (the asset LOD bias): find where it is in the texture's own mips.
			FTexturePlatformData* const* PlatformData = Context.Texture ? const_cast<UTexture*>(Context.Texture)->GetRunningPlatformData() : nullptr;
			if (!PlatformData || !*PlatformData || Context.MipsView.Num() == 0)
			{
				return Fail(TEXT("the texture has no mips"));
			}
			int32 ViewStart = INDEX_NONE;
			for (int32 Index = 0; Index < (*PlatformData)->Mips.Num(); ++Index)
			{
				if (&(*PlatformData)->Mips[Index] == Context.MipsView[0])
				{
					ViewStart = Index;
					break;
				}
			}
			if (ViewStart == INDEX_NONE)
			{
				return Fail(TEXT("the streamed mips are not the texture's"));
			}

			TUniquePtr<IFileHandle> File(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Table->Path));
			if (!File.IsValid())
			{
				return Fail(TEXT("the file could not be opened"));
			}
			if (File->Size() != Table->FileSize)
			{
				return Fail(FString::Printf(TEXT("the file is %lld bytes, its table says %lld"), File->Size(), Table->FileSize));
			}

			const FPixelFormatInfo& Info = GPixelFormats[Table->Format];
			TArray<uint8> Packed;
			TArray<uint8> Rows;
			for (int32 MipIndex = StartingMipIndex; MipIndex < CurrentFirstLODIdx; ++MipIndex)
			{
				const int32 FileMip = Table->FirstMip + ViewStart + MipIndex;
				if (!Table->Mips.IsValidIndex(FileMip) || !MipInfos.IsValidIndex(MipIndex))
				{
					return Fail(FString::Printf(TEXT("mip %d is not in the file"), FileMip));
				}
				const FMantleTextureFileMip& Entry = Table->Mips[FileMip];
				const FTextureMipInfo& Target = MipInfos[MipIndex];
				if (!Target.DestData || Target.SizeX != static_cast<uint32>(Entry.Width) || Target.SizeY != static_cast<uint32>(Entry.Height)
					|| (Target.DataSize != 0 && Target.DataSize < static_cast<uint64>(Entry.Size)))
				{
					return Fail(FString::Printf(TEXT("mip %d is %u x %u (%llu bytes) in the engine and %d x %d (%lld bytes) in the file"),
						FileMip, Target.SizeX, Target.SizeY, Target.DataSize, Entry.Width, Entry.Height, Entry.Size));
				}
				Packed.SetNumUninitialized(static_cast<int32>(Entry.Packed), EAllowShrinking::No);
				if (!File->Seek(Entry.Offset) || !File->Read(Packed.GetData(), Entry.Packed))
				{
					return Fail(FString::Printf(TEXT("mip %d could not be read"), FileMip));
				}
				GPackedBytesRead += Entry.Packed;

				// The engine's buffer is tightly packed unless it says a row pitch (a locked GPU mip on an RHI without async creation).
				const int64 TightPitch = static_cast<int64>(FMath::DivideAndRoundUp(Entry.Width, Info.BlockSizeX)) * Info.BlockBytes;
				const int64 BlockRows = FMath::DivideAndRoundUp(Entry.Height, Info.BlockSizeY);
				FString Reason;
				if (Target.RowPitch == 0 || Target.RowPitch == TightPitch)
				{
					if (!MantleCookedPacking::InflateExact(Packed, static_cast<uint8*>(Target.DestData), Entry.Size, &Reason))
					{
						return Fail(FString::Printf(TEXT("mip %d is %s"), FileMip, *Reason));
					}
				}
				else
				{
					Rows.SetNumUninitialized(static_cast<int32>(Entry.Size), EAllowShrinking::No);
					if (!MantleCookedPacking::InflateExact(Packed, Rows.GetData(), Entry.Size, &Reason))
					{
						return Fail(FString::Printf(TEXT("mip %d is %s"), FileMip, *Reason));
					}
					for (int64 Row = 0; Row < BlockRows; ++Row)
					{
						FMemory::Memcpy(static_cast<uint8*>(Target.DestData) + Row * Target.RowPitch, Rows.GetData() + Row * TightPitch, TightPitch);
					}
				}
				++GMipsServed;
				GBytesServed += Entry.Size;
			}
			return true;
		}

		TSharedRef<const FMantleTextureFileTable> Table;
		FName TextureName;
		bool bOk = false;
	};
}

FMantleFilePin::FMantleFilePin(const TSharedRef<FMantleAssetStore>& InStore, const FMantleContentHash& InHash)
	: Store(InStore)
	, Hash(InHash)
{
	Store->Pin(Hash);
}

FMantleFilePin::~FMantleFilePin()
{
	Store->Unpin(Hash);
}

FTextureMipDataProvider* UMantleCachedMipsFactory::AllocateMipDataProvider(UTexture* Asset)
{
	if (!Table.IsValid())
	{
		return nullptr;
	}
	return new MantleStreamedTexturePrivate::FCachedMipProvider(Asset, Table.ToSharedRef());
}

FMantleStreamedMipStats UMantleCachedMipsFactory::GetStats()
{
	using namespace MantleStreamedTexturePrivate;
	FMantleStreamedMipStats Stats;
	Stats.MipsServed = GMipsServed.Load();
	Stats.BytesServed = GBytesServed.Load();
	Stats.PackedBytesRead = GPackedBytesRead.Load();
	Stats.Failed = GFailed.Load();
	Stats.Requests = GRequests.Load();
	Stats.MeanRequestMs = Stats.Requests > 0 ? GSumRequestUs.Load() / 1000.0 / static_cast<double>(Stats.Requests) : 0.0;
	Stats.WorstRequestMs = GWorstRequestUs.Load() / 1000.0;
	return Stats;
}

void UMantleCachedMipsFactory::ResetStats()
{
	using namespace MantleStreamedTexturePrivate;
	GMipsServed = 0;
	GBytesServed = 0;
	GPackedBytesRead = 0;
	GFailed = 0;
	GRequests = 0;
	GSumRequestUs = 0;
	GWorstRequestUs = 0;
}
