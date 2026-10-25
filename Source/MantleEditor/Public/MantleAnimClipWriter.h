// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleCosmeticValidation.h"

class FMantleAvatarContract;
class UAnimSequence;
class USkeletalMesh;

/**
 * One bone's keys in an emote: local transforms in Unreal's frame and units. Named apart from MantleRuntime's reader types
 * (MantleAnimClip.h's FMantleAnimClipTrack and FMantleAnimClipEvent), so the writer and the reader can meet in one
 * translation unit (MantleEditor builds as a unity build; the emote check decodes what it encodes).
 */
struct MANTLEEDITOR_API FMantleAnimClipTrackData
{
	FName Bone;

	/** 1 key (constant) or FrameCount keys. */
	TArray<FQuat4f> Rotations;

	/** 0 keys (keeps the reference translation), 1 (constant) or FrameCount. */
	TArray<FVector3f> Translations;
};

struct MANTLEEDITOR_API FMantleAnimClipEventData
{
	float TimeSeconds = 0.0f;
	FString Name;
};

/** An emote as MAN1 holds it (backend cook/animclip.go). */
struct MANTLEEDITOR_API FMantleAnimClipData
{
	float FrameRate = 30.0f;
	int32 FrameCount = 0;
	TArray<FMantleAnimClipTrackData> Tracks;
	TArray<FMantleAnimClipEventData> Events;

	/**
	 * A place clip's skeleton (#469, MAN1 flag bit 0): the bone table of the mesh it was sampled over, parent-first,
	 * and its hash (FMantleSkeletonHash). Empty for an emote.
	 */
	FString SkeletonHash;
	TArray<FName> SkeletonBones;
	TArray<int32> SkeletonParents;

	/**
	 * What FMantleAnimClipWriter::Sample did that the bytes do not show, for the export's notes and warnings (not encoded).
	 * RootMotion: the root's motion it moved onto the root's children (FMantleAnimClipWriter::MoveRootOntoChildren).
	 * DroppedNotifies: the names of the sequence's notifies outside the event allowlist, which no event carries.
	 * DroppedCurves: how many float curves the sequence has (MAN1 carries bones only). LoopSeamDegrees and LoopSeamCm:
	 * how far apart the last frame and the loop_start frame were before CloseLoopSeam joined them (0 without loop_start).
	 */
	struct FRootMotion
	{
		double TravelCm = 0.0;
		double RiseCm = 0.0;
		double TurnDegrees = 0.0;
		/** How far the root's first key is from its reference pose (cm, degrees): an offset the client would also drop. */
		double OffsetCm = 0.0;
		double OffsetDegrees = 0.0;
		bool bMoved = false;
		bool bInPlace = false;
	} RootMotion;
	TArray<FString> DroppedNotifies;
	int32 DroppedCurves = 0;
	double LoopSeamDegrees = 0.0;
	double LoopSeamCm = 0.0;

	/** (FrameCount - 1) / FrameRate. */
	double DurationSeconds() const;
};

/**
 * The plugin's MAN1 writer (A2.4 #283): samples an emote on the UEFN driver skeleton at 30 fps and
 * encodes it the way the cook worker re-encodes it (cook/animclip.go CookAnimClip: tracks in
 * skeleton order, constant tracks collapsed to one key, rotations normalised, events by time), so
 * the bytes a creator uploads are already the canonical ones.
 *
 * A clip on another rig has to be retargeted onto the UEFN mannequin first. #535: the export does it
 * for a MetaHuman, UE5 Mannequin or UE4 Mannequin clip (FMantleEmoteRetarget: IK Rig's batch retarget through the
 * plugin's reverse retargeters) and samples the retargeted copy here; for any other rig Validate
 * names the foreign bone and says to retarget it.
 */
struct MANTLEEDITOR_API FMantleAnimClipWriter
{
	/**
	 * MAN1 bytes. Tracks are ordered by Skeleton (bones it does not have go last, in their own order). A place clip (#469,
	 * SkeletonHash set) writes its skeleton block and orders its tracks by its own bone table instead.
	 */
	static TArray<uint8> Encode(const FMantleAnimClipData& Clip, const struct FMantleContractSkeleton* Skeleton = nullptr);

	/**
	 * #469: samples Sequence over the bones of Mesh (a place skeletal mesh on its own skeleton) at FrameRate, as the cook
	 * re-encodes a place clip: one track per bone of Mesh's raw reference skeleton that the sequence animates, a rotation
	 * that never leaves the bone's reference rotation and a translation that never leaves its reference translation left
	 * out (the bone keeps them anyway), no events, and Mesh's bone table and hash as the skeleton block. A sequence that
	 * moves nothing keeps one constant track on the root. False with OutError when the sequence has no data model or no
	 * frames, or the mesh no bones.
	 */
	static bool SampleOnMesh(const UAnimSequence* Sequence, const USkeletalMesh* Mesh, float FrameRate, FMantleAnimClipData& OutClip, FString& OutError);

	/** Collapses constant tracks, normalises rotations, sorts events, as the worker does before encoding. */
	static void Canonicalize(FMantleAnimClipData& Clip);

	/** Every rule of cook.ValidateAnimClip, with its messages. */
	static void Validate(const FMantleAnimClipData& Clip, const FMantleAvatarContract& Contract, TArray<FMantleCosmeticIssue>& OutIssues);

	/**
	 * Samples Sequence at the contract's frame rate: every bone track, interpolated between the
	 * source's keys; a translation track is dropped when it never leaves the bone's reference
	 * translation (0.01 cm), which the node keeps anyway; notifies named in the event allowlist
	 * become events. The root's motion moves onto its children first (MoveRootOntoChildren), and a
	 * clip with a loop_start event has its end joined to it (CloseLoopSeam); OutClip.DroppedNotifies,
	 * DroppedCurves, RootMotion and LoopSeam* say what was left out or changed. False with OutError
	 * when the sequence has no data model or no frames.
	 */
	static bool Sample(const UAnimSequence* Sequence, const FMantleAvatarContract& Contract, FMantleAnimClipData& OutClip, FString& OutError);

	/**
	 * Players' clients hold an emote's root still (the driver keeps the graph's root, so root motion is ignored), so
	 * Sample moves whatever the root does onto the root's children that have tracks (the pelvis, and the IK roots):
	 * each child's keys become child * root(t) * RootReference^-1, and the root's track holds RootReference. A turn, a
	 * jump or a sway on the root then still shows. Travel across the floor further than MaxTravelCm (the contract's
	 * emoteMaxRootCm) is taken out instead (bInPlace): emotes play in place, so such a clip dances on the spot. The
	 * root's and the children's tracks hold 1 or FrameCount keys of each kind (Sample runs it before it drops translations
	 * at the reference); a child without translation keys is left alone. Clip.RootMotion says what was found. Pure.
	 */
	static void MoveRootOntoChildren(FMantleAnimClipData& Clip, FName Root, TConstArrayView<FName> Children, const FTransform& RootReference,
		double MaxTravelCm);

	/**
	 * #291: a clip with a loop_start event loops from it to its end on players' clients, so its last frame should be its
	 * loop_start frame's pose. Blends every animated track over the last BlendSeconds (at most half the loop) from its own
	 * keys into that pose, so the jump back is seamless; Clip.LoopSeamDegrees and LoopSeamCm say how far apart the two
	 * frames were. Nothing changes for a loop_start at or after the last frame. Tracks hold 1 or FrameCount keys. Pure.
	 */
	static void CloseLoopSeam(FMantleAnimClipData& Clip, float LoopStartSeconds, float BlendSeconds);

	/**
	 * The creator's Loop choice for an emote (FMantleCosmeticDraft::bEmoteLoop). Players' clients loop a clip that has a
	 * loop_start event, from that event to its end, and play any other once. bLoop: a clip without a loop_start event gets
	 * one at 0, so it loops whole, and its end is joined to its first frame (CloseLoopSeam); a clip with one keeps it.
	 * !bLoop: every loop_start event goes. Events stay sorted. True when it changed the clip. Pure.
	 */
	static bool ApplyLoopChoice(FMantleAnimClipData& Clip, bool bLoop);

	/** 16-bit snorm as the worker quantizes it (round(clamp(v) * 32767)). */
	static int16 QuantizeSnorm16(float Value);
};
