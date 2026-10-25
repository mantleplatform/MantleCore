// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSoundReader.h"

#include "MantleCookedCursor.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleSoundReaderPrivate
{
	static_assert(sizeof(int16) == 2, "MAU1 samples are read straight into int16");

	bool FailSound(FMantleSoundData& Out, FString* OutError, const FString& Error)
	{
		Out = FMantleSoundData();
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("MAU1: %s"), *Error);
		}
		return false;
	}
}

bool FMantleSoundReader::Read(const TConstArrayView<uint8> Data, FMantleSoundData& Out, FString* OutError)
{
	using namespace MantleSoundReaderPrivate;
	using MantleCookedFormat::FCursor;

	Out = FMantleSoundData();
	FCursor Cursor(Data);
	if (!Cursor.Magic("MAU1"))
	{
		return FailSound(Out, OutError, TEXT("that asset is not a Mantle sound"));
	}
	const uint32 Version = Cursor.U32();
	const uint32 Flags = Cursor.U32();
	const uint32 SampleRate = Cursor.U32();
	const uint32 Channels = Cursor.U32();
	const uint32 Frames = Cursor.U32();
	if (Cursor.Failed())
	{
		return FailSound(Out, OutError, TEXT("the header is truncated"));
	}
	if (Version != SupportedVersion)
	{
		// Specific on purpose: an old client meeting a newer asset should say "update Mantle".
		return FailSound(Out, OutError, FString::Printf(
			TEXT("the sound was cooked for a newer version of Mantle (format %u, this build reads %u)"), Version, SupportedVersion));
	}
	if (Flags != 0)
	{
		return FailSound(Out, OutError, FString::Printf(TEXT("unknown flags 0x%x"), Flags));
	}
	if (Channels != 1 && Channels != 2)
	{
		return FailSound(Out, OutError, FString::Printf(TEXT("the header claims %u channels (1 or 2)"), Channels));
	}
	if (SampleRate < MinSampleRate || SampleRate > MaxSampleRate)
	{
		return FailSound(Out, OutError, FString::Printf(
			TEXT("the header claims a sample rate of %u Hz (%u to %u)"), SampleRate, MinSampleRate, MaxSampleRate));
	}
	const uint64 MaxFrames = static_cast<uint64>(MaxDurationSeconds) * SampleRate;
	if (Frames < 1 || Frames > MaxFrames)
	{
		return FailSound(Out, OutError, FString::Printf(
			TEXT("the header claims %u frames at %u Hz (1 to %llu, at most %u seconds)"), Frames, SampleRate, MaxFrames, MaxDurationSeconds));
	}

	// The whole file is the header and exactly the samples it describes: nothing is half-read.
	const int64 SampleCount = static_cast<int64>(Frames) * Channels;
	if (Cursor.Remaining() != SampleCount * 2)
	{
		return FailSound(Out, OutError, FString::Printf(
			TEXT("%lld bytes of samples, the header describes %lld"), Cursor.Remaining(), SampleCount * 2));
	}
	if (!Cursor.Array(Out.Samples, SampleCount))
	{
		return FailSound(Out, OutError, TEXT("the samples are truncated"));
	}
	Out.SampleRate = static_cast<int32>(SampleRate);
	Out.Channels = static_cast<int32>(Channels);
	return true;
}
