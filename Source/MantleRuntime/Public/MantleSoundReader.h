// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * A creator's uploaded sound as the cook worker writes it (MAU1, backend/internal/cook/sound.go, #324):
 * decoded PCM16 with nothing left to interpret. The plugin uploads a canonical WAV; the worker parses
 * it and stores this, so a player's client never parses a stranger's source format (the MRM1 rule).
 */
struct FMantleSoundData
{
	/** Hz, within FMantleSoundReader's range. */
	int32 SampleRate = 0;

	/** 1 (mono) or 2 (stereo). */
	int32 Channels = 0;

	/** FrameCount() x Channels samples, interleaved (left, right, left, ... for stereo). */
	TArray<int16> Samples;

	/** Samples per channel. */
	int32 FrameCount() const { return Channels > 0 ? Samples.Num() / Channels : 0; }

	double DurationSeconds() const { return SampleRate > 0 ? static_cast<double>(FrameCount()) / SampleRate : 0.0; }

	/** Bytes the samples hold, for budgets and logs (MAU1's samples block, the cook's cookInfo.decodedBytes). */
	int64 GetAllocatedBytes() const { return Samples.GetAllocatedSize(); }
};

/**
 * Reads MAU1 strictly. The bytes were uploaded by a stranger and arrive over the network, so the
 * header is checked field by field before anything is allocated: the magic, version 1, no flags,
 * 1 or 2 channels, a sample rate and duration within limits.json placeAssets.sound (the cook's own
 * limits), and a file size exactly the header's. Mirrors cook.DecodeSound in the Go writer, which pins
 * the shared fixture schema/fixtures/sound-v1.mau.
 *
 * Layout, little-endian: "MAU1", then uint32 version, flags, sampleRate, channels, frameCount (a
 * 24-byte header), then frameCount x channels int16 samples, interleaved.
 *
 * No UObjects, no asserts, no logging: any thread.
 */
class MANTLERUNTIME_API FMantleSoundReader
{
public:
	/** The version this build reads. Must match SoundVersion in the worker. */
	static constexpr uint32 SupportedVersion = 1;

	/** limits.json placeAssets.sound, which the cook enforces on every upload. */
	static constexpr uint32 MinSampleRate = 22050;
	static constexpr uint32 MaxSampleRate = 48000;
	static constexpr uint32 MaxDurationSeconds = 60;

	/** False, with the reason in OutError and Out left empty, on anything it refuses. */
	static bool Read(TConstArrayView<uint8> Data, FMantleSoundData& Out, FString* OutError = nullptr);
};
