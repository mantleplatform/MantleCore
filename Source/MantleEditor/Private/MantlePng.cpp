// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePng.h"

#include "Misc/Compression.h"
#include "Misc/Crc.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePngPrivate
{
	void AppendBigEndian32(TArray<uint8>& Out, uint32 Value)
	{
		Out.Add(static_cast<uint8>(Value >> 24));
		Out.Add(static_cast<uint8>(Value >> 16));
		Out.Add(static_cast<uint8>(Value >> 8));
		Out.Add(static_cast<uint8>(Value));
	}

	uint32 ReadBigEndian32(const uint8* Data)
	{
		return (uint32(Data[0]) << 24) | (uint32(Data[1]) << 16) | (uint32(Data[2]) << 8) | uint32(Data[3]);
	}

	/** Length, type, data and the CRC-32 of type and data (PNG uses the IEEE polynomial, FCrc::MemCrc32's). */
	void AppendChunk(TArray<uint8>& Out, const char Type[4], TConstArrayView<uint8> Data)
	{
		AppendBigEndian32(Out, static_cast<uint32>(Data.Num()));
		const int32 TypeAt = Out.Num();
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Out.Add(static_cast<uint8>(Type[Index]));
		}
		Out.Append(Data.GetData(), Data.Num());
		const uint32 Crc = FCrc::MemCrc32(Out.GetData() + TypeAt, 4 + Data.Num());
		AppendBigEndian32(Out, Crc);
	}

	constexpr uint8 Signature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
}

TArray<uint8> FMantlePng::EncodeRGBA8(int32 Width, int32 Height, TConstArrayView<FColor> Pixels)
{
	using namespace MantlePngPrivate;

	TArray<uint8> Out;
	if (Width <= 0 || Height <= 0 || Pixels.Num() != Width * Height)
	{
		return Out;
	}

	// Scanlines: a filter byte (0, none) then R, G, B, A per pixel.
	TArray<uint8> Raw;
	Raw.Reserve(Height * (1 + Width * 4));
	for (int32 Row = 0; Row < Height; ++Row)
	{
		Raw.Add(0);
		for (int32 Column = 0; Column < Width; ++Column)
		{
			const FColor& Pixel = Pixels[Row * Width + Column];
			Raw.Add(Pixel.R);
			Raw.Add(Pixel.G);
			Raw.Add(Pixel.B);
			Raw.Add(Pixel.A);
		}
	}

	int64 Bound = 0;
	if (!FCompression::CompressMemoryBound(NAME_Zlib, Bound, Raw.Num()) || Bound <= 0)
	{
		return Out;
	}
	TArray<uint8> Compressed;
	Compressed.SetNumUninitialized(static_cast<int32>(Bound));
	int64 CompressedSize = Bound;
	if (!FCompression::CompressMemory(NAME_Zlib, Compressed.GetData(), CompressedSize, Raw.GetData(), Raw.Num()))
	{
		return Out;
	}
	Compressed.SetNum(static_cast<int32>(CompressedSize));

	Out.Append(Signature, 8);
	TArray<uint8> Header;
	AppendBigEndian32(Header, static_cast<uint32>(Width));
	AppendBigEndian32(Header, static_cast<uint32>(Height));
	Header.Add(8);  // bit depth
	Header.Add(6);  // colour type: RGBA
	Header.Add(0);  // compression: deflate
	Header.Add(0);  // filter method
	Header.Add(0);  // no interlace
	AppendChunk(Out, "IHDR", Header);
	AppendChunk(Out, "IDAT", Compressed);
	AppendChunk(Out, "IEND", TConstArrayView<uint8>());
	return Out;
}

TArray<uint8> FMantlePng::EncodeSolid(int32 Width, int32 Height, FColor Color)
{
	TArray<FColor> Pixels;
	Pixels.Init(Color, FMath::Max(0, Width) * FMath::Max(0, Height));
	return EncodeRGBA8(Width, Height, Pixels);
}

bool FMantlePng::ReadHeader(TConstArrayView<uint8> Bytes, int32& OutWidth, int32& OutHeight, bool& OutHasAlpha)
{
	using namespace MantlePngPrivate;

	OutWidth = OutHeight = 0;
	OutHasAlpha = false;
	if (Bytes.Num() < 33 || FMemory::Memcmp(Bytes.GetData(), Signature, 8) != 0
		|| FMemory::Memcmp(Bytes.GetData() + 12, "IHDR", 4) != 0)
	{
		return false;
	}
	OutWidth = static_cast<int32>(ReadBigEndian32(Bytes.GetData() + 16));
	OutHeight = static_cast<int32>(ReadBigEndian32(Bytes.GetData() + 20));
	const uint8 ColourType = Bytes[25];
	OutHasAlpha = ColourType == 4 || ColourType == 6;

	// A palette or grey image carries alpha in a tRNS chunk.
	int64 Offset = 8;
	while (!OutHasAlpha && Offset + 8 <= Bytes.Num())
	{
		const uint32 Length = ReadBigEndian32(Bytes.GetData() + Offset);
		if (FMemory::Memcmp(Bytes.GetData() + Offset + 4, "tRNS", 4) == 0)
		{
			OutHasAlpha = true;
		}
		if (FMemory::Memcmp(Bytes.GetData() + Offset + 4, "IDAT", 4) == 0)
		{
			break;
		}
		Offset += 12 + static_cast<int64>(Length);
	}
	return OutWidth > 0 && OutHeight > 0;
}
