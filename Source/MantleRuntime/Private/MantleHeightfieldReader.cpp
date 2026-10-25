// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHeightfieldReader.h"

#include "MantleCookedCursor.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleHeightfieldReaderPrivate
{
	static_assert(sizeof(uint16) == 2, "MHF1 samples are read straight into uint16");

	bool FailHeightfield(FMantleHeightfield& Out, FString* OutError, const FString& Error)
	{
		Out = FMantleHeightfield();
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("MHF1: %s"), *Error);
		}
		return false;
	}

	void AppendU32(TArray<uint8>& Out, const uint32 Value)
	{
		for (int32 Shift = 0; Shift < 32; Shift += 8)
		{
			Out.Add(static_cast<uint8>((Value >> Shift) & 0xFF));
		}
	}

	void AppendF32(TArray<uint8>& Out, const float Value)
	{
		uint32 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		AppendU32(Out, Bits);
	}
}

bool FMantleHeightfieldReader::Read(const TConstArrayView<uint8> Data, FMantleHeightfield& Out, FString* OutError)
{
	using namespace MantleHeightfieldReaderPrivate;
	using MantleCookedFormat::FCursor;

	Out = FMantleHeightfield();
	FCursor Cursor(Data);
	if (!Cursor.Magic("MHF1"))
	{
		return FailHeightfield(Out, OutError, TEXT("that asset is not a Mantle heightfield"));
	}
	const uint32 Version = Cursor.U32();
	const uint32 Flags = Cursor.U32();
	const uint32 Width = Cursor.U32();
	const uint32 Height = Cursor.U32();
	const float SpacingX = Cursor.F32();
	const float SpacingY = Cursor.F32();
	const float HeightScale = Cursor.F32();
	const float HeightOffset = Cursor.F32();
	if (Cursor.Failed())
	{
		return FailHeightfield(Out, OutError, TEXT("the header is truncated"));
	}
	if (Version != SupportedVersion)
	{
		// Specific on purpose: an old client meeting a newer asset should say "update Mantle".
		return FailHeightfield(Out, OutError, FString::Printf(
			TEXT("the heightfield was cooked for a newer version of Mantle (format %u, this build reads %u)"), Version, SupportedVersion));
	}
	if (Flags != 0)
	{
		return FailHeightfield(Out, OutError, FString::Printf(TEXT("unknown flags 0x%x"), Flags));
	}
	if (Width < static_cast<uint32>(MinSize) || Width > static_cast<uint32>(MaxSize)
		|| Height < static_cast<uint32>(MinSize) || Height > static_cast<uint32>(MaxSize))
	{
		return FailHeightfield(Out, OutError, FString::Printf(
			TEXT("the header claims a %u x %u grid (each side %d to %d)"), Width, Height, MinSize, MaxSize));
	}
	if (!FMath::IsFinite(SpacingX) || !FMath::IsFinite(SpacingY) || SpacingX <= 0.0f || SpacingY <= 0.0f)
	{
		return FailHeightfield(Out, OutError, FString::Printf(TEXT("the spacing (%g, %g) cm must be finite and above zero"), SpacingX, SpacingY));
	}
	if (!FMath::IsFinite(HeightScale) || HeightScale <= 0.0f || !FMath::IsFinite(HeightOffset))
	{
		return FailHeightfield(Out, OutError, FString::Printf(
			TEXT("the height scale %g and offset %g must be finite, the scale above zero"), HeightScale, HeightOffset));
	}

	// The whole file is the header and exactly the samples it describes: nothing is half-read.
	const int64 SampleCount = static_cast<int64>(Width) * Height;
	if (Cursor.Remaining() != SampleCount * 2)
	{
		return FailHeightfield(Out, OutError, FString::Printf(
			TEXT("%lld bytes of samples, the header describes %lld"), Cursor.Remaining(), SampleCount * 2));
	}
	if (!Cursor.Array(Out.Heights, SampleCount))
	{
		return FailHeightfield(Out, OutError, TEXT("the samples are truncated"));
	}
	Out.Width = static_cast<int32>(Width);
	Out.Height = static_cast<int32>(Height);
	Out.SpacingX = SpacingX;
	Out.SpacingY = SpacingY;
	Out.HeightScale = HeightScale;
	Out.HeightOffset = HeightOffset;
	return true;
}

TArray<uint8> FMantleHeightfieldReader::Write(const FMantleHeightfield& Field)
{
	using namespace MantleHeightfieldReaderPrivate;

	TArray<uint8> Out;
	if (Field.Width < 0 || Field.Height < 0 || static_cast<int64>(Field.Width) * Field.Height != Field.Heights.Num())
	{
		return Out;
	}
	Out.Reserve(HeaderBytes + Field.Heights.Num() * 2);
	Out.Append(reinterpret_cast<const uint8*>("MHF1"), 4);
	AppendU32(Out, SupportedVersion);
	AppendU32(Out, 0);
	AppendU32(Out, static_cast<uint32>(Field.Width));
	AppendU32(Out, static_cast<uint32>(Field.Height));
	AppendF32(Out, Field.SpacingX);
	AppendF32(Out, Field.SpacingY);
	AppendF32(Out, Field.HeightScale);
	AppendF32(Out, Field.HeightOffset);
	for (const uint16 Value : Field.Heights)
	{
		Out.Add(static_cast<uint8>(Value & 0xFF));
		Out.Add(static_cast<uint8>(Value >> 8));
	}
	return Out;
}
