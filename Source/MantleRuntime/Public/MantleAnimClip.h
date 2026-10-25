// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

struct FReferenceSkeleton;

/**
 * One bone's keys, local (parent-relative) in Unreal's frame and units. Rotations has 1 key (constant)
 * or FrameCount; Translations has 0 (the bone keeps its reference translation), 1 or FrameCount.
 * Scale is never animated.
 */
struct FMantleAnimClipTrack
{
	FName Bone;
	TArray<FQuat4f> Rotations;
	TArray<FVector3f> Translations;

	bool HasTranslation() const { return Translations.Num() > 0; }
};

/** A named moment in the clip (the emote event allowlist: footstep_l, footstep_r, impact, loop_start). */
struct FMantleAnimClipEvent
{
	/** Seconds from the start, 0 to the clip's duration. */
	float Time = 0.0f;
	FName Name;
};

/**
 * Which bone of one skeleton each track drives: build it once per (clip, skeleton) with
 * FMantleAnimClip::Bind and keep it (the cached bone index map of AVATAR_CATALOG_PLAN.md A2.3), so
 * sampling does no name lookups. Plain data, so it can be sampled on any thread.
 */
struct FMantleAnimClipBinding
{
	/** Per track of the clip: the bone index in the bound skeleton, INDEX_NONE when it has no such bone. */
	TArray<int32> TrackToBone;

	/** Bones in the bound skeleton. */
	int32 NumBones = 0;

	/** Tracks that found their bone, and those that did not (skipped when sampling). */
	int32 NumBound = 0;
	int32 NumUnbound = 0;

	/** Track index of the root bone (skeleton bone 0) when a track drives it; its translation is root motion. */
	int32 RootTrack = INDEX_NONE;
};

/** How SampleTrack reads the clip. */
struct FMantleAnimClipSampleOptions
{
	/** Wrap the time around the clip's duration instead of clamping it to [0, duration]. */
	bool bLoop = false;
};

/**
 * An emote: the in-memory MAN1 clip (AVATAR_CATALOG_PLAN.md section 3.4; the worker's canonical
 * encoding, backend/internal/cook/animclip.go), on the UEFN driver skeleton. Sampling interpolates
 * between the two frames around a time: rotations by shortest-path slerp, translations linearly.
 *
 * Holds no UObjects: read, bind and sample on any thread.
 */
struct MANTLERUNTIME_API FMantleAnimClip
{
	/** Frames per second (the emote contract's 30). */
	float FrameRate = 30.0f;

	/** At least 1; frame i is at i / FrameRate seconds, so the clip lasts (FrameCount - 1) / FrameRate. */
	int32 FrameCount = 0;

	/** One per bone at most, in skeleton order as the worker writes them. */
	TArray<FMantleAnimClipTrack> Tracks;

	/** Sorted by time. */
	TArray<FMantleAnimClipEvent> Events;

	/**
	 * A place clip's skeleton (#469, MAN1 flag bit 0): the hash of the bone table it was authored on
	 * (FMantleSkeletonHash), and that table, parent-first. Empty for an emote. A place clip plays only on
	 * a place skeletal mesh whose skeleton is SkeletonHash, and every track names a bone of the table.
	 */
	FString SkeletonHash;
	TArray<FName> SkeletonBoneNames;
	TArray<int32> SkeletonBoneParents;

	bool HasSkeleton() const { return !SkeletonHash.IsEmpty(); }

	/** Seconds: (FrameCount - 1) / FrameRate, 0 for a one-frame clip. */
	float GetDuration() const;

	/** Binds the tracks to a skeleton given as its bone names in bone-index order (index 0 the root). */
	FMantleAnimClipBinding Bind(TConstArrayView<FName> BoneNames) const;

	/** Binds the tracks to a reference skeleton's raw bones (the driver mesh's GetRefSkeleton()). */
	FMantleAnimClipBinding Bind(const FReferenceSkeleton& Skeleton) const;

	/**
	 * One track at Time (seconds, clamped or wrapped as Options says): its interpolated rotation, and its
	 * translation when it has one (OutHasTranslation false, OutTranslation untouched, otherwise). False
	 * when TrackIndex is out of range.
	 */
	bool SampleTrack(int32 TrackIndex, float Time, FQuat4f& OutRotation, FVector3f& OutTranslation, bool& bOutHasTranslation,
		const FMantleAnimClipSampleOptions& Options = FMantleAnimClipSampleOptions()) const;

	/**
	 * Every structural rule sampling relies on: a finite positive frame rate, at least one frame and one
	 * track, tracks with a unique non-None bone, 1 or FrameCount rotation keys (unit, finite), 0, 1 or
	 * FrameCount translation keys (finite), events with a name inside the clip. A place clip's skeleton
	 * (#469): a parent-first table with one root and unique names, its hash the one it names, every track
	 * on one of its bones, and no events. False with the first problem in OutError.
	 */
	bool Validate(FString& OutError) const;
};

struct FMantleAnimClipReadResult
{
	bool bSuccess = false;

	FMantleAnimClip Clip;

	/** Tags of the extension blocks this build does not know and skipped (version 1 defines none). */
	TArray<FString> SkippedExtensions;

	/** What went wrong, phrased for whoever has to act on it. */
	FString Error;
};

/**
 * Reads MAN1, the cooked emote clip or place clip (#469, flag bit 0: the skeleton block of the bone table
 * it was authored on), into FMantleAnimClip. Quaternion components are int16 / 32767 and
 * are renormalised after the length is checked; translations are uint16 steps across each track's own
 * [min, max] range. Strict like the other cooked readers: counts against the bytes left and sanity caps
 * before allocating, flags and floats checked, duplicate tracks, unknown flags and trailing bytes
 * refused, unknown extensions skipped, then FMantleAnimClip::Validate. Touches no UObjects.
 */
class MANTLERUNTIME_API FMantleAnimClipReader
{
public:
	/** The version this build understands. Must match AnimClipVersion in the worker. */
	static constexpr uint32 SupportedVersion = 1;

	/** Sanity caps above the emote contract's (30 fps, 10 s, 64 events). */
	static constexpr int32 MaxFrames = 36'000;
	static constexpr int32 MaxTracks = 2048;
	static constexpr int32 MaxEvents = 256;
	static constexpr float MaxFrameRate = 240.0f;
	static constexpr float MaxTranslationCm = 10'000.0f;

	/** Bones in a place clip's skeleton block (limits.json placeAssets.animClip.maxBones is 256; MSK1 holds 2048). */
	static constexpr int32 MaxSkeletonBones = 2048;

	static FMantleAnimClipReadResult Read(TConstArrayView<uint8> Data);

	/** Convenience for tests and tools. */
	static FMantleAnimClipReadResult ReadFromFile(const FString& FilePath);
};
