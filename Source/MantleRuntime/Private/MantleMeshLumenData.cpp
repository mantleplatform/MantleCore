// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMeshLumenData.h"

#include "DistanceFieldAtlas.h"
#include "MantleAssetStore.h"
#include "MeshCardBuild.h"
#include "Misc/EngineVersion.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleMeshLumenDataPrivate
{
	constexpr uint32 FormatVersion = 1;

	/** No mesh's file comes near this; a count past it is a damaged file, not a big mesh. */
	constexpr int64 MaxStreamedMipBytes = 256LL * 1024 * 1024;

	void SetReason(FString* OutReason, const FString& Reason)
	{
		if (OutReason)
		{
			*OutReason = Reason;
		}
	}
}

FMantleMeshLumenData::FMantleMeshLumenData() = default;
FMantleMeshLumenData::~FMantleMeshLumenData() = default;
FMantleMeshLumenData::FMantleMeshLumenData(FMantleMeshLumenData&&) = default;
FMantleMeshLumenData& FMantleMeshLumenData::operator=(FMantleMeshLumenData&&) = default;

int64 FMantleMeshLumenData::GetAllocatedBytes() const
{
	int64 Bytes = 0;
	if (DistanceField)
	{
		Bytes += static_cast<int64>(DistanceField->GetResourceSizeBytes()) + DistanceField->StreamableMips.GetBulkDataSize();
	}
	if (Cards)
	{
		Bytes += static_cast<int64>(Cards->GetResourceSizeBytes());
	}
	return Bytes;
}

FString FMantleMeshLumenFile::EngineTag()
{
	return FEngineVersion::Current().ToString(EVersionComponent::Changelist);
}

TArray<uint8> FMantleMeshLumenFile::Write(const FMantleContentHash& MeshHash, const FDistanceFieldVolumeData& DistanceField, const FCardRepresentationData& Cards)
{
	using namespace MantleMeshLumenDataPrivate;

	TArray<uint8> Bytes;
	if (!DistanceField.IsValid())
	{
		return Bytes;
	}
	FMemoryWriter Writer(Bytes, /*bIsPersistent*/ true);
	uint8 Magic[4] = { 'M', 'L', 'D', '1' };
	Writer.Serialize(Magic, 4);
	uint32 Version = FormatVersion;
	Writer << Version;
	FString Engine = EngineTag();
	FString Mesh = MeshHash.ToString();
	Writer << Engine << Mesh;

	// FDistanceFieldVolumeData::Serialize's fields, the streamed mips as plain bytes: its bulk data needs a package to be read back.
	FDistanceFieldVolumeData& Field = const_cast<FDistanceFieldVolumeData&>(DistanceField);
	Writer << Field.LocalSpaceMeshBounds << Field.bMostlyTwoSided << Field.Mips << Field.AlwaysLoadedMip;
	TArray<uint8> Streamed;
	if (const int64 StreamedBytes = Field.StreamableMips.GetBulkDataSize(); StreamedBytes > 0)
	{
		Streamed.Append(static_cast<const uint8*>(Field.StreamableMips.LockReadOnly()), StreamedBytes);
		Field.StreamableMips.Unlock();
	}
	Writer << Streamed;
	Writer << const_cast<FCardRepresentationData&>(Cards);
	return Bytes;
}

bool FMantleMeshLumenFile::Read(const TConstArrayView<uint8> Bytes, const FMantleContentHash& MeshHash, FMantleMeshLumenData& OutData, FString* OutReason)
{
	using namespace MantleMeshLumenDataPrivate;

	OutData = FMantleMeshLumenData();
	if (Bytes.Num() < 8 || FMemory::Memcmp(Bytes.GetData(), "MLD1", 4) != 0)
	{
		SetReason(OutReason, TEXT("is not an MLD1"));
		return false;
	}
	// FMemoryReader takes a TArray; the view's bytes are only read.
	const TArray<uint8> Copy(Bytes.GetData(), Bytes.Num());
	FMemoryReader Reader(Copy, /*bIsPersistent*/ true);
	Reader.Seek(4);
	uint32 Version = 0;
	Reader << Version;
	if (Version != FormatVersion)
	{
		SetReason(OutReason, FString::Printf(TEXT("is MLD1 version %u (this client reads %u)"), Version, FormatVersion));
		return false;
	}
	FString Engine;
	FString Mesh;
	Reader << Engine << Mesh;
	if (Reader.IsError() || Engine != EngineTag())
	{
		SetReason(OutReason, FString::Printf(TEXT("was baked by engine %s (this client is %s)"), *Engine, *EngineTag()));
		return false;
	}
	if (Mesh != MeshHash.ToString())
	{
		SetReason(OutReason, FString::Printf(TEXT("belongs to mesh %s"), *Mesh));
		return false;
	}

	TUniquePtr<FDistanceFieldVolumeData> Field = MakeUnique<FDistanceFieldVolumeData>();
	Reader << Field->LocalSpaceMeshBounds << Field->bMostlyTwoSided << Field->Mips << Field->AlwaysLoadedMip;
	int32 StreamedBytes = 0;
	Reader << StreamedBytes;
	if (Reader.IsError() || StreamedBytes < 0 || StreamedBytes > MaxStreamedMipBytes || StreamedBytes > Reader.TotalSize() - Reader.Tell())
	{
		SetReason(OutReason, TEXT("has a damaged distance field"));
		return false;
	}
	if (StreamedBytes > 0)
	{
		Field->StreamableMips.Lock(LOCK_READ_WRITE);
		void* Data = Field->StreamableMips.Realloc(StreamedBytes);
		Reader.Serialize(Data, StreamedBytes);
		Field->StreamableMips.Unlock();
	}
	TUniquePtr<FCardRepresentationData> Cards = MakeUnique<FCardRepresentationData>();
	Reader << *Cards;
	if (Reader.IsError() || Reader.Tell() != Reader.TotalSize() || !Field->IsValid())
	{
		SetReason(OutReason, TEXT("is damaged"));
		return false;
	}
	OutData.DistanceField = MoveTemp(Field);
	OutData.Cards = MoveTemp(Cards);
	return true;
}
