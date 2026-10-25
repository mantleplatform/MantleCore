// Copyright Joshua Eger. All Rights Reserved.
#include "MantleRuntimeTextureBuilder.h"

#include "Engine/Texture2D.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "MantleRuntime.h"
#include "Misc/CoreMisc.h"
#include "Modules/ModuleManager.h"
#include "RHIGlobals.h"
#include "TextureResource.h"
#include "UObject/Package.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleRuntimeTextureBuilderPrivate
{
	/** No texture Mantle builds is larger than this on a side, whatever the caller allows (the engine's mip sizes are 16-bit). */
	constexpr int32 HardMaxDimension = 8192;

	/** Enough levels for HardMaxDimension down to 1. */
	constexpr int32 MaxMips = 14;

	constexpr uint8 PngSignature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	bool RefuseTexture(FString* OutError, const FString& Error)
	{
		if (OutError)
		{
			*OutError = Error;
		}
		return false;
	}

	uint32 ReadBigEndian32(const uint8* Bytes)
	{
		return (static_cast<uint32>(Bytes[0]) << 24) | (static_cast<uint32>(Bytes[1]) << 16) | (static_cast<uint32>(Bytes[2]) << 8) | static_cast<uint32>(Bytes[3]);
	}

	/**
	 * Walks a PNG's chunk list before libpng sees it: the signature, IHDR first, every chunk's length
	 * inside the file, and IEND. The engine's PNG reader does not stop on a short read (it logs and lets
	 * libpng continue on an unfilled buffer), so a truncated download is refused here instead. Gives
	 * the IHDR's width and height.
	 */
	bool CheckPngStructure(const TConstArrayView<uint8> Bytes, int64& OutWidth, int64& OutHeight, FString* OutError)
	{
		if (Bytes.Num() < 8 + 8 + 13 + 4 || FMemory::Memcmp(Bytes.GetData(), PngSignature, 8) != 0)
		{
			return RefuseTexture(OutError, TEXT("That texture is not a complete PNG."));
		}
		int64 Offset = 8;
		bool bFirst = true;
		while (true)
		{
			if (Offset + 8 > Bytes.Num())
			{
				return RefuseTexture(OutError, TEXT("That PNG is truncated (it has no end chunk)."));
			}
			const int64 Length = ReadBigEndian32(Bytes.GetData() + Offset);
			const uint8* Type = Bytes.GetData() + Offset + 4;
			if (Length > 0x7FFFFFFF || Offset + 8 + Length + 4 > Bytes.Num())
			{
				return RefuseTexture(OutError, TEXT("That PNG is truncated or corrupt (a chunk runs past the end)."));
			}
			if (bFirst)
			{
				if (FMemory::Memcmp(Type, "IHDR", 4) != 0 || Length != 13)
				{
					return RefuseTexture(OutError, TEXT("That PNG is corrupt (it does not start with a header chunk)."));
				}
				OutWidth = ReadBigEndian32(Type + 4);
				OutHeight = ReadBigEndian32(Type + 8);
				bFirst = false;
			}
			if (FMemory::Memcmp(Type, "IEND", 4) == 0)
			{
				return true;
			}
			Offset += 8 + Length + 4;
		}
	}

	/** sRGB byte to linear light. */
	const float* SRGBToLinearTable()
	{
		static const TArray<float> Table = []
		{
			TArray<float> Values;
			Values.SetNumUninitialized(256);
			for (int32 Index = 0; Index < 256; ++Index)
			{
				const float C = Index / 255.0f;
				Values[Index] = C <= 0.04045f ? C / 12.92f : FMath::Pow((C + 0.055f) / 1.055f, 2.4f);
			}
			return Values;
		}();
		return Table.GetData();
	}

	/** Linear light (0..1) to an sRGB byte, through a table fine enough for the darks. */
	uint8 LinearToSRGBByte(const float Linear)
	{
		constexpr int32 Steps = 16384;
		static const TArray<uint8> Table = []
		{
			TArray<uint8> Values;
			Values.SetNumUninitialized(Steps + 1);
			for (int32 Index = 0; Index <= Steps; ++Index)
			{
				const float L = Index / static_cast<float>(Steps);
				const float C = L <= 0.0031308f ? L * 12.92f : 1.055f * FMath::Pow(L, 1.0f / 2.4f) - 0.055f;
				Values[Index] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32(C * 255.0f), 0, 255));
			}
			return Values;
		}();
		return Table[FMath::Clamp(FMath::RoundToInt32(Linear * Steps), 0, Steps)];
	}

	/** The next level of a BGRA8 chain: a 2x2 box per texel (edge texels repeated on odd sizes), per role. */
	void DownsampleBGRA8(const FMantleTextureMip& Source, const EMantleTextureRole Role, FMantleTextureMip& Out)
	{
		Out.Width = FMath::Max(1, Source.Width / 2);
		Out.Height = FMath::Max(1, Source.Height / 2);
		Out.Bytes.SetNumUninitialized(static_cast<int64>(Out.Width) * Out.Height * 4);
		const uint8* In = Source.Bytes.GetData();
		uint8* Dest = Out.Bytes.GetData();
		const float* ToLinear = SRGBToLinearTable();

		for (int32 Y = 0; Y < Out.Height; ++Y)
		{
			const int32 Y0 = FMath::Min(Y * 2, Source.Height - 1);
			const int32 Y1 = FMath::Min(Y * 2 + 1, Source.Height - 1);
			for (int32 X = 0; X < Out.Width; ++X)
			{
				const int32 X0 = FMath::Min(X * 2, Source.Width - 1);
				const int32 X1 = FMath::Min(X * 2 + 1, Source.Width - 1);
				const uint8* Texels[4] =
				{
					In + (static_cast<int64>(Y0) * Source.Width + X0) * 4,
					In + (static_cast<int64>(Y0) * Source.Width + X1) * 4,
					In + (static_cast<int64>(Y1) * Source.Width + X0) * 4,
					In + (static_cast<int64>(Y1) * Source.Width + X1) * 4,
				};
				uint8* Texel = Dest + (static_cast<int64>(Y) * Out.Width + X) * 4;

				// Alpha (byte 3 of B, G, R, A) is linear coverage in every role.
				Texel[3] = static_cast<uint8>((Texels[0][3] + Texels[1][3] + Texels[2][3] + Texels[3][3] + 2) / 4);

				if (Role == EMantleTextureRole::Color)
				{
					for (int32 Channel = 0; Channel < 3; ++Channel)
					{
						const float Linear = (ToLinear[Texels[0][Channel]] + ToLinear[Texels[1][Channel]] + ToLinear[Texels[2][Channel]] + ToLinear[Texels[3][Channel]]) * 0.25f;
						Texel[Channel] = LinearToSRGBByte(Linear);
					}
				}
				else if (Role == EMantleTextureRole::Normal)
				{
					// B, G, R hold z, y, x encoded as (n + 1) / 2.
					FVector3f Sum = FVector3f::ZeroVector;
					for (const uint8* Source4 : Texels)
					{
						Sum += FVector3f(Source4[2] / 127.5f - 1.0f, Source4[1] / 127.5f - 1.0f, Source4[0] / 127.5f - 1.0f);
					}
					const FVector3f Normal = Sum.GetSafeNormal(UE_SMALL_NUMBER, FVector3f(0.0f, 0.0f, 1.0f));
					Texel[2] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32((Normal.X + 1.0f) * 127.5f), 0, 255));
					Texel[1] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32((Normal.Y + 1.0f) * 127.5f), 0, 255));
					Texel[0] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32((Normal.Z + 1.0f) * 127.5f), 0, 255));
				}
				else
				{
					for (int32 Channel = 0; Channel < 3; ++Channel)
					{
						Texel[Channel] = static_cast<uint8>((Texels[0][Channel] + Texels[1][Channel] + Texels[2][Channel] + Texels[3][Channel] + 2) / 4);
					}
				}
			}
		}
	}

	uint32 ReadLittleEndian32(const uint8* Bytes)
	{
		return static_cast<uint32>(Bytes[0]) | (static_cast<uint32>(Bytes[1]) << 8) | (static_cast<uint32>(Bytes[2]) << 16) | (static_cast<uint32>(Bytes[3]) << 24);
	}

	// MTX1, the cook worker's block-compressed texture (Fab F9, #343; backend/internal/cook/texturecook.go,
	// BACKEND.md "The MTX1 format"). Little-endian: magic, version, format, flags, width, height,
	// sourceWidth, sourceHeight, mipCount, then mipCount x {width, height, offset, size}, then the
	// block data, mip 0 first and contiguous.
	constexpr uint32 Mtx1Version = 1;
	constexpr int64 Mtx1HeaderBytes = 36;
	constexpr int64 Mtx1MipEntryBytes = 16;
	constexpr uint32 Mtx1FlagSRGB = 1;
	/** The worker's MaxTextureDimension: no MTX1 mip 0 is larger on a side. */
	constexpr uint32 Mtx1MaxDimension = 2048;

	/** The pixel format of an MTX1 format code (1 BC1, 3 BC3, 4 BC4, 5 BC5, all UNORM); PF_Unknown for any other. */
	EPixelFormat Mtx1PixelFormat(const uint32 Code)
	{
		switch (Code)
		{
		case 1: return PF_DXT1;
		case 3: return PF_DXT5;
		case 4: return PF_BC4;
		case 5: return PF_BC5;
		default: return PF_Unknown;
		}
	}

	/**
	 * Reads an MTX1 container into Out without decoding a texel. Every field is checked against the
	 * format's rules and every mip's size and offset against the buffer before anything is allocated:
	 * the version, a known format, no unknown flags, sRGB only on BC1 and BC3, mip 0 a multiple of 4 and
	 * at most 2048 (and Params.MaxDimension), the source size padding to it by less than a block, the
	 * full chain to 1 x 1, each mip halving with exactly ceil(w/4) x ceil(h/4) blocks at the next offset,
	 * and nothing after the last mip. The same rules as the worker's own reader (cook.DecodeTexture).
	 */
	bool DecodeMtx1(const TConstArrayView<uint8> Bytes, const FMantleTextureBuildParams& Params, FMantleDecodedTexture& Out, FString* OutError)
	{
		const int64 Size = Bytes.Num();
		if (Size < Mtx1HeaderBytes)
		{
			return RefuseTexture(OutError, TEXT("That MTX1 texture is truncated (its header is incomplete)."));
		}
		const uint8* Data = Bytes.GetData();
		auto Field = [Data](const int64 Offset) { return ReadLittleEndian32(Data + Offset); };

		const uint32 Version = Field(4);
		if (Version != Mtx1Version)
		{
			return RefuseTexture(OutError, FString::Printf(TEXT("That MTX1 texture is version %u; this build of Mantle reads version %u."), Version, Mtx1Version));
		}
		const uint32 FormatCode = Field(8);
		const EPixelFormat Format = Mtx1PixelFormat(FormatCode);
		if (Format == PF_Unknown)
		{
			return RefuseTexture(OutError, FString::Printf(TEXT("That MTX1 texture has an unknown block format (%u)."), FormatCode));
		}
		const uint32 Flags = Field(12);
		if ((Flags & ~Mtx1FlagSRGB) != 0)
		{
			return RefuseTexture(OutError, FString::Printf(TEXT("That MTX1 texture has unknown flags (0x%x)."), Flags));
		}
		const bool bSRGB = (Flags & Mtx1FlagSRGB) != 0;
		if (bSRGB && Format != PF_DXT1 && Format != PF_DXT5)
		{
			return RefuseTexture(OutError, TEXT("That MTX1 texture is marked sRGB, which only BC1 and BC3 can be."));
		}

		const uint32 Width = Field(16);
		const uint32 Height = Field(20);
		const uint32 SourceWidth = Field(24);
		const uint32 SourceHeight = Field(28);
		const uint32 MipCount = Field(32);
		if (Width < 4 || Height < 4 || Width > Mtx1MaxDimension || Height > Mtx1MaxDimension || Width % 4 != 0 || Height % 4 != 0)
		{
			return RefuseTexture(OutError, FString::Printf(TEXT("That MTX1 texture's top mip is %u x %u; it must be a multiple of 4 and at most %u."),
				Width, Height, Mtx1MaxDimension));
		}
		const uint32 MaxDimension = static_cast<uint32>(FMath::Clamp(Params.MaxDimension, 1, HardMaxDimension));
		if (Width > MaxDimension || Height > MaxDimension)
		{
			return RefuseTexture(OutError, FString::Printf(TEXT("That MTX1 texture is %u x %u; the limit is %u on a side."), Width, Height, MaxDimension));
		}
		// Width and Height are at least 4 here; the additions cannot wrap (both fields were read as uint32 below 2^31 + 4).
		if (SourceWidth > Width || static_cast<uint64>(SourceWidth) + 4 <= Width || SourceHeight > Height || static_cast<uint64>(SourceHeight) + 4 <= Height)
		{
			return RefuseTexture(OutError, FString::Printf(TEXT("That MTX1 texture's source size %u x %u does not pad to %u x %u."),
				SourceWidth, SourceHeight, Width, Height));
		}
		const int32 ExpectedMips = FMantleRuntimeTextureBuilder::ComputeFullMipCount(static_cast<int32>(Width), static_cast<int32>(Height));
		if (MipCount != static_cast<uint32>(ExpectedMips))
		{
			return RefuseTexture(OutError, FString::Printf(TEXT("That MTX1 texture has %u mips; a %u x %u chain has %d."), MipCount, Width, Height, ExpectedMips));
		}
		const int64 TableEnd = Mtx1HeaderBytes + static_cast<int64>(MipCount) * Mtx1MipEntryBytes;
		if (Size < TableEnd)
		{
			return RefuseTexture(OutError, TEXT("That MTX1 texture is truncated (its mip table is incomplete)."));
		}

		const int32 KeptMips = Params.Mips == EMantleTextureMips::None ? 1 : ExpectedMips;
		const int64 Footprint = FMantleRuntimeTextureBuilder::ComputeGpuBytes(Format, static_cast<int32>(Width), static_cast<int32>(Height), KeptMips);
		if (Footprint < 0 || Footprint > Params.MaxGpuBytes)
		{
			return RefuseTexture(OutError, FString::Printf(TEXT("That %u x %u MTX1 texture needs %lld bytes of texture memory; the budget is %lld."),
				Width, Height, Footprint, Params.MaxGpuBytes));
		}

		// Every mip entry against the chain the header promises and against the buffer, before any copy.
		int64 Offset = TableEnd;
		for (uint32 Mip = 0; Mip < MipCount; ++Mip)
		{
			const int64 Entry = Mtx1HeaderBytes + static_cast<int64>(Mip) * Mtx1MipEntryBytes;
			const uint32 MipWidth = Field(Entry);
			const uint32 MipHeight = Field(Entry + 4);
			const uint32 MipOffset = Field(Entry + 8);
			const uint32 MipSize = Field(Entry + 12);
			const uint32 WantWidth = FMath::Max(1u, Width >> Mip);
			const uint32 WantHeight = FMath::Max(1u, Height >> Mip);
			if (MipWidth != WantWidth || MipHeight != WantHeight)
			{
				return RefuseTexture(OutError, FString::Printf(TEXT("That MTX1 texture's mip %u is %u x %u; the chain needs %u x %u."),
					Mip, MipWidth, MipHeight, WantWidth, WantHeight));
			}
			const int64 WantSize = FMantleRuntimeTextureBuilder::ComputeMipBytes(Format, static_cast<int32>(WantWidth), static_cast<int32>(WantHeight));
			if (WantSize < 0 || static_cast<int64>(MipSize) != WantSize)
			{
				return RefuseTexture(OutError, FString::Printf(TEXT("That MTX1 texture's mip %u holds %u bytes; %u x %u in its format is %lld."),
					Mip, MipSize, WantWidth, WantHeight, WantSize));
			}
			if (static_cast<int64>(MipOffset) != Offset)
			{
				return RefuseTexture(OutError, FString::Printf(TEXT("That MTX1 texture's mip %u starts at byte %u; it must follow the one before, at %lld."),
					Mip, MipOffset, Offset));
			}
			if (Offset + WantSize > Size)
			{
				return RefuseTexture(OutError, FString::Printf(TEXT("That MTX1 texture is truncated (mip %u runs past the end)."), Mip));
			}
			Offset += WantSize;
		}
		if (Offset != Size)
		{
			return RefuseTexture(OutError, FString::Printf(TEXT("That MTX1 texture has %lld bytes after its last mip."), Size - Offset));
		}

		// Everything checked: only now is anything allocated.
		Out.Format = Format;
		Out.bSRGB = bSRGB;
		Out.Role = Format == PF_BC5 ? EMantleTextureRole::Normal : (bSRGB ? EMantleTextureRole::Color : EMantleTextureRole::Data);
		Out.SourceWidth = static_cast<int32>(SourceWidth);
		Out.SourceHeight = static_cast<int32>(SourceHeight);
		Out.Mips.Reserve(KeptMips);
		for (int32 Mip = 0; Mip < KeptMips; ++Mip)
		{
			const int64 Entry = Mtx1HeaderBytes + static_cast<int64>(Mip) * Mtx1MipEntryBytes;
			FMantleTextureMip& Level = Out.Mips.AddDefaulted_GetRef();
			Level.Width = static_cast<int32>(Field(Entry));
			Level.Height = static_cast<int32>(Field(Entry + 4));
			Level.Bytes.Append(Data + Field(Entry + 8), static_cast<int64>(Field(Entry + 12)));
		}
		return true;
	}

	IImageWrapperModule* GetImageWrapperModule()
	{
		static const FName ModuleName(TEXT("ImageWrapper"));
		IImageWrapperModule* Module = FModuleManager::GetModulePtr<IImageWrapperModule>(ModuleName);
		if (!Module && IsInGameThread())
		{
			Module = FModuleManager::LoadModulePtr<IImageWrapperModule>(ModuleName);
		}
		return Module;
	}
}

int64 FMantleDecodedTexture::GetGpuBytes() const
{
	int64 Bytes = 0;
	for (const FMantleTextureMip& Mip : Mips)
	{
		Bytes += Mip.Bytes.Num();
	}
	return Bytes;
}

bool FMantleRuntimeTextureBuilder::IsSupportedFormat(const EPixelFormat Format)
{
	switch (Format)
	{
	case PF_B8G8R8A8:
	case PF_DXT1:
	case PF_DXT5:
	case PF_BC4:
	case PF_BC5:
		return true;
	default:
		return false;
	}
}

int64 FMantleRuntimeTextureBuilder::ComputeMipBytes(const EPixelFormat Format, const int32 Width, const int32 Height)
{
	if (!IsSupportedFormat(Format) || Width < 1 || Height < 1)
	{
		return -1;
	}
	const FPixelFormatInfo& Info = GPixelFormats[Format];
	const int64 BlocksX = FMath::DivideAndRoundUp(Width, Info.BlockSizeX);
	const int64 BlocksY = FMath::DivideAndRoundUp(Height, Info.BlockSizeY);
	return BlocksX * BlocksY * Info.BlockBytes;
}

int64 FMantleRuntimeTextureBuilder::ComputeGpuBytes(const EPixelFormat Format, int32 Width, int32 Height, const int32 NumMips)
{
	int64 Bytes = 0;
	for (int32 Mip = 0; Mip < NumMips; ++Mip)
	{
		const int64 MipBytes = ComputeMipBytes(Format, Width, Height);
		if (MipBytes < 0)
		{
			return -1;
		}
		Bytes += MipBytes;
		Width = FMath::Max(1, Width / 2);
		Height = FMath::Max(1, Height / 2);
	}
	return Bytes;
}

int32 FMantleRuntimeTextureBuilder::ComputeFullMipCount(int32 Width, int32 Height)
{
	int32 Count = 1;
	while (Width > 1 || Height > 1)
	{
		Width = FMath::Max(1, Width / 2);
		Height = FMath::Max(1, Height / 2);
		++Count;
	}
	return Count;
}

bool FMantleRuntimeTextureBuilder::Decode(const TConstArrayView<uint8> Bytes, const FMantleTextureBuildParams& Params, FMantleDecodedTexture& Out,
	FString* OutError)
{
	using namespace MantleRuntimeTextureBuilderPrivate;

	Out = FMantleDecodedTexture();
	if (Bytes.Num() < 4)
	{
		return RefuseTexture(OutError, TEXT("That texture is empty."));
	}
	if (FMemory::Memcmp(Bytes.GetData(), "MTX1", 4) == 0)
	{
		// Fab F9 (#343): the worker's BC container. The blocks and mips go to the GPU as they are.
		if (!DecodeMtx1(Bytes, Params, Out, OutError))
		{
			Out = FMantleDecodedTexture();
			return false;
		}
		return true;
	}

	IImageWrapperModule* Module = GetImageWrapperModule();
	if (!Module)
	{
		return RefuseTexture(OutError, TEXT("The image decoder is not loaded (decode once on the game thread first)."));
	}
	const EImageFormat Format = Module->DetectImageFormat(Bytes.GetData(), Bytes.Num());
	if (Format != EImageFormat::PNG && Format != EImageFormat::JPEG)
	{
		return RefuseTexture(OutError, TEXT("That texture is not a PNG or JPEG image."));
	}

	// Everything the header promises is checked before the pixels are decoded. A PNG's size is read
	// from its IHDR here, before libpng sees the file at all.
	const int32 MaxDimension = FMath::Clamp(Params.MaxDimension, 1, HardMaxDimension);
	auto RefuseSize = [OutError, MaxDimension](const int64 SizeX, const int64 SizeY)
	{
		return RefuseTexture(OutError, FString::Printf(TEXT("That texture is %lld x %lld; the limit is %d on a side."), SizeX, SizeY, MaxDimension));
	};
	int64 HeaderWidth = 0;
	int64 HeaderHeight = 0;
	if (Format == EImageFormat::PNG)
	{
		if (!CheckPngStructure(Bytes, HeaderWidth, HeaderHeight, OutError))
		{
			return false;
		}
		if (HeaderWidth < 1 || HeaderHeight < 1 || HeaderWidth > MaxDimension || HeaderHeight > MaxDimension)
		{
			return RefuseSize(HeaderWidth, HeaderHeight);
		}
	}

	const TSharedPtr<IImageWrapper> Wrapper = Module->CreateImageWrapper(Format);
	if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()))
	{
		return RefuseTexture(OutError, TEXT("That image's header could not be read."));
	}
	const int64 Width = Wrapper->GetWidth();
	const int64 Height = Wrapper->GetHeight();
	if (Format == EImageFormat::PNG && (Width != HeaderWidth || Height != HeaderHeight))
	{
		return RefuseTexture(OutError, TEXT("That PNG's header is inconsistent."));
	}
	if (Width < 1 || Height < 1 || Width > MaxDimension || Height > MaxDimension)
	{
		return RefuseSize(Width, Height);
	}
	const int32 W = static_cast<int32>(Width);
	const int32 H = static_cast<int32>(Height);
	const int32 NumMips = Params.Mips == EMantleTextureMips::Generate ? ComputeFullMipCount(W, H) : 1;
	const int64 Footprint = ComputeGpuBytes(PF_B8G8R8A8, W, H, NumMips);
	if (Footprint > Params.MaxGpuBytes)
	{
		return RefuseTexture(OutError, FString::Printf(TEXT("That %d x %d texture needs %lld bytes of texture memory; the budget is %lld."),
			W, H, Footprint, Params.MaxGpuBytes));
	}

	FImage Decoded;
	// IImageWrapper::GetRawImage isn't exported from the ImageWrapper module; the module's
	// DecompressImage is the engine's supported path and fills the same FImage.
	if (!Module->DecompressImage(Bytes.GetData(), Bytes.Num(), Decoded) || Decoded.SizeX != W || Decoded.SizeY != H
		|| Decoded.NumSlices != 1)
	{
		return RefuseTexture(OutError, TEXT("That image could not be decoded."));
	}

	// To BGRA8 without changing the encoding: 8-bit images keep their bytes, 16-bit ones are quantised
	// as stored. The role decides later whether the bytes are sampled as sRGB.
	FImage Converted;
	Decoded.CopyTo(Converted, ERawImageFormat::BGRA8, Decoded.GammaSpace);
	if (Converted.RawData.Num() != static_cast<int64>(W) * H * 4)
	{
		return RefuseTexture(OutError, TEXT("That image could not be converted to 8-bit colour."));
	}

	Out.Format = PF_B8G8R8A8;
	Out.Role = Params.Role;
	Out.bSRGB = Params.Role == EMantleTextureRole::Color;
	Out.SourceWidth = W;
	Out.SourceHeight = H;
	FMantleTextureMip& Top = Out.Mips.AddDefaulted_GetRef();
	Top.Width = W;
	Top.Height = H;
	Top.Bytes = MoveTemp(Converted.RawData);
	while (Out.Mips.Num() < NumMips)
	{
		FMantleTextureMip Next;
		DownsampleBGRA8(Out.Mips.Last(), Params.Role, Next);
		Out.Mips.Add(MoveTemp(Next));
	}
	return true;
}

UTexture2D* FMantleRuntimeTextureBuilder::Build(FMantleDecodedTexture&& Decoded, const FMantleTextureBuildParams& Params, FString* OutError)
{
	using namespace MantleRuntimeTextureBuilderPrivate;

	auto Fail = [OutError](const FString& Error) -> UTexture2D*
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_TEXTURE FAIL %s"), *Error);
		RefuseTexture(OutError, Error);
		return nullptr;
	};

	if (IsRunningDedicatedServer())
	{
		return Fail(TEXT("a dedicated server does not build textures"));
	}
	if (!IsInGameThread())
	{
		return Fail(TEXT("textures are built on the game thread (decode anywhere, build there)"));
	}
	if (!IsSupportedFormat(Decoded.Format))
	{
		return Fail(FString::Printf(TEXT("pixel format %d is not one Mantle uploads"), static_cast<int32>(Decoded.Format)));
	}
	if (!GPixelFormats[Decoded.Format].Supported)
	{
		// The engine would log an error and create no resource; refuse cleanly instead (a GPU without BC support).
		return Fail(FString::Printf(TEXT("this GPU does not support %s textures"), GetPixelFormatString(Decoded.Format)));
	}
	const int32 NumMips = Decoded.Mips.Num();
	if (NumMips < 1 || NumMips > MaxMips)
	{
		return Fail(FString::Printf(TEXT("the texture has %d mips (1 to %d)"), NumMips, MaxMips));
	}
	const int32 Width = Decoded.GetWidth();
	const int32 Height = Decoded.GetHeight();
	const FPixelFormatInfo& FormatInfo = GPixelFormats[Decoded.Format];
	if (Width < 1 || Height < 1 || Width > HardMaxDimension || Height > HardMaxDimension
		|| Width % FormatInfo.BlockSizeX != 0 || Height % FormatInfo.BlockSizeY != 0)
	{
		return Fail(FString::Printf(TEXT("the texture is %d x %d, not a usable size for its format"), Width, Height));
	}
	if (static_cast<uint32>(FMath::Max(Width, Height)) > GetMax2DTextureDimension())
	{
		return Fail(FString::Printf(TEXT("the texture is %d x %d, larger than this GPU's %u"), Width, Height, GetMax2DTextureDimension()));
	}
	int32 ExpectedWidth = Width;
	int32 ExpectedHeight = Height;
	for (int32 MipIndex = 0; MipIndex < NumMips; ++MipIndex)
	{
		const FMantleTextureMip& Mip = Decoded.Mips[MipIndex];
		if (Mip.Width != ExpectedWidth || Mip.Height != ExpectedHeight || Mip.Bytes.Num() != ComputeMipBytes(Decoded.Format, Mip.Width, Mip.Height))
		{
			return Fail(FString::Printf(TEXT("mip %d is %d x %d with %lld bytes; the chain needs %d x %d"),
				MipIndex, Mip.Width, Mip.Height, Mip.Bytes.Num(), ExpectedWidth, ExpectedHeight));
		}
		ExpectedWidth = FMath::Max(1, ExpectedWidth / 2);
		ExpectedHeight = FMath::Max(1, ExpectedHeight / 2);
	}
	const int64 Footprint = Decoded.GetGpuBytes();
	if (Footprint > Params.MaxGpuBytes)
	{
		return Fail(FString::Printf(TEXT("the texture needs %lld bytes of texture memory; the budget is %lld"), Footprint, Params.MaxGpuBytes));
	}

	UObject* Outer = Params.Outer ? Params.Outer : GetTransientPackage();
	const FName Name = Params.Name.IsNone() ? NAME_None : MakeUniqueObjectName(Outer, UTexture2D::StaticClass(), Params.Name);
	UTexture2D* Texture = NewObject<UTexture2D>(Outer, Name, RF_Transient);
	// Standard per-mip layout, as UTexture2D::CreateTransient uploads it.
	Texture->bNotOfflineProcessed = true;

	FTexturePlatformData* PlatformData = new FTexturePlatformData();
	PlatformData->SizeX = Width;
	PlatformData->SizeY = Height;
	PlatformData->SetNumSlices(1);
	PlatformData->PixelFormat = Decoded.Format;
	for (FMantleTextureMip& Source : Decoded.Mips)
	{
		FTexture2DMipMap* Mip = new FTexture2DMipMap(Source.Width, Source.Height, 1);
		PlatformData->Mips.Add(Mip);
		Mip->BulkData.Lock(LOCK_READ_WRITE);
		void* Destination = Mip->BulkData.Realloc(Source.Bytes.Num());
		FMemory::Memcpy(Destination, Source.Bytes.GetData(), Source.Bytes.Num());
		Mip->BulkData.Unlock();
		Source.Bytes.Empty();
	}
	Texture->SetPlatformData(PlatformData);

	Texture->SRGB = Decoded.bSRGB;
	Texture->CompressionSettings = Decoded.Role == EMantleTextureRole::Normal ? TC_Normalmap
		: (Decoded.Role == EMantleTextureRole::Data ? TC_Masks : TC_Default);
	Texture->AddressX = Params.AddressX;
	Texture->AddressY = Params.AddressY;
	Texture->Filter = Params.Filter;
	Texture->LODGroup = Params.LODGroup;
	// All mips are inline and resident: there is nothing to stream them from.
	Texture->NeverStream = true;
	Texture->UpdateResource();

	UE_LOG(LogMantle, Verbose, TEXT("MANTLE_TEXTURE built %s: %d x %d, %d mips, format %s, %s, %lld bytes"),
		*Texture->GetName(), Width, Height, NumMips, GetPixelFormatString(Decoded.Format), Decoded.bSRGB ? TEXT("sRGB") : TEXT("linear"), Footprint);
	Decoded.Mips.Empty();
	return Texture;
}

UTexture2D* FMantleRuntimeTextureBuilder::BuildFromBytes(const TConstArrayView<uint8> Bytes, const FMantleTextureBuildParams& Params, FString* OutError)
{
	FMantleDecodedTexture Decoded;
	FString Error;
	if (!Decode(Bytes, Params, Decoded, &Error))
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_TEXTURE FAIL %s"), *Error);
		if (OutError)
		{
			*OutError = Error;
		}
		return nullptr;
	}
	return Build(MoveTemp(Decoded), Params, OutError);
}
