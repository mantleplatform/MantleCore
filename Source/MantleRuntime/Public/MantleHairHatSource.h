// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleCosmeticItem.h"
#include "MantleHairHatFit.h"
#include "MantleSkinnedMesh.h"

/** What the hat step did to one hair fit, for the log (#389). */
struct MANTLERUNTIME_API FMantleHairHatStats
{
	/** The hat step ran and changed or checked the hair (false: no hat, or it could not run; Error says why in the second case). */
	bool bApplied = false;
	/** Hat-slot items that took part. */
	int32 NumHats = 0;
	/** Outerwear hoods that took part (their triangles are counted in HatTriangles too). */
	int32 NumHoods = 0;
	/** Hat triangles the rays were tested against. */
	int32 HatTriangles = 0;
	/** Points pulled in and triangles cut out, over every LOD. */
	int32 PointsPulled = 0;
	int32 TrianglesDropped = 0;
	/** The whole step (reading the hats, placing them, the head centre, the fit, the edit), and the part that is FMantleHairHatFit::Fit, milliseconds. */
	double Ms = 0.0;
	double FitMs = 0.0;
	/** Why the step could not run; the hair is then left as the head fit made it. */
	FString Error;
};

/**
 * The hood of a worn outerwear item (#389, H6): the triangles of its fitted mesh whose three corners are each skin-weighted at
 * least MinHeadWeight to the head bone, which is the bone the scalp hair is rigid to. The fitted outerwear lies in the wearer's
 * reference pose, the space the fitted hair is in, so the triangles are used as they are. Plain data, shared read-only.
 */
struct MANTLERUNTIME_API FMantleHairHood
{
	/** A corner counts as head when its weights on the head bone add up to this share (of 1) or more. */
	static constexpr float MinHeadWeight = 0.5f;

	/** The vertices the hood triangles use, as in the fitted mesh's LOD0 (renumbered; no other vertex is kept). */
	TArray<FVector3f> Positions;
	/** A triangle list into Positions. */
	TArray<uint32> Indices;

	int32 NumTriangles() const { return Indices.Num() / 3; }
	int64 Bytes() const { return Positions.GetAllocatedSize() + Indices.GetAllocatedSize(); }

	/** The bone the scalp hair follows (FMantleHairHeadFit binds it rigid to "head"). */
	static FName HeadBoneName();

	/**
	 * The hood of a fitted outerwear mesh (LOD0 only). Out holds the triangles whose three corners each have at least MinHeadWeight
	 * on the head bone; false, and Out empty, when there are none (a jacket without a hood), the mesh has no head bone or its skin
	 * data does not agree with its vertices. Any thread.
	 */
	static bool Extract(const FMantleSkinnedMesh& Fitted, FMantleHairHood& Out);

	/** Extract, as the shared read-only data the cache hands a task; null when there is no hood. */
	static TSharedPtr<const FMantleHairHood, ESPMode::ThreadSafe> Make(const FMantleSkinnedMesh& Fitted);
};

/**
 * One worn hat-slot item (acc_hat, acc_face) as the hair fit needs it (#389): plain data, so the cache can hand it to a
 * background task. The hat's pose is worked out where the hair is fitted, from the same function the assembler places the
 * accessory with (MantleAccessoryPlacement::PlacedRelative), so the hair is fitted to where the hat is drawn.
 *
 * Everything that decides where the hat is and what it looks like is in KeyFor, so a fit made for one hat is not used for
 * another, or for the same hat somewhere else.
 */
struct MANTLERUNTIME_API FMantleHairHatSource
{
	/** Bumped when what the hat step does to a hair changes, so a fit made under an older rule is not reused. Part of the key. */
	static constexpr int32 RuleVersion = 1;

	/** The slot id ("acc_hat"): names the socket and the slot's bounds. */
	FString SlotId;
	/** The hat's cooked mesh hash text: names its geometry. */
	FString MeshHash;
	/** The hat's cooked mesh (MRM4, or MRM1 cooked before F2): the cache entry's bytes. Shared and read-only, so holding it keeps it alive for a task. */
	TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> MeshBytes;
	/** The item's hairRule. LeaveAlone items are left out by the caller; HideAll for scalp hair is the caller's too (the hair is not fitted at all). */
	EMantleHairHatRule Rule = EMantleHairHatRule::UnderIt;
	/** The item's attach offset. */
	FMantleCosmeticTransform Attach;
	/** The shown avatar's override of the socket (its "sockets" metadata); unset: none. */
	TOptional<FMantleCosmeticTransform> SocketOverride;
	/**
	 * Set for a hood (#389, H6) instead of a hat: SlotId is "outerwear", MeshHash names the outerwear's fit (its fitted mesh, not a cooked
	 * file), and Hood holds the triangles, already in the hair's space. MeshBytes, Attach, SocketLocal and BoneName are not used.
	 */
	TSharedPtr<const FMantleHairHood, ESPMode::ThreadSafe> Hood;
	/** The socket relative to its bone, and the bone's name: the avatar skeleton's. */
	FTransform SocketLocal = FTransform::Identity;
	FName BoneName;

	/** "underIt", "hideAll", "leaveAlone" (what FMantleCosmeticItem::HairRule holds); anything else is underIt. */
	static EMantleHairHatRule RuleFromText(const FString& Text);

	/** A hash of everything that places the hat apart from its mesh: slot, attach, override, socket, bone. */
	uint32 PlacementHash() const;

	/** "slot=meshHash/rule/placementHash"; a hood has no placement: "slot=fitIdentity/rule/hood". */
	FString KeyPiece() const;

	/**
	 * The hair fit key's hat part: ":hathair-v<RuleVersion>[piece;piece]", empty for no sources. Aim UnderHatCards (the dev
	 * comparison, rule b) makes it ":hathair-v<RuleVersion>-cards[...]"; the default aim's key is unchanged.
	 */
	static FString KeyFor(TConstArrayView<FMantleHairHatSource> Sources, EMantleHairHatAim Aim = EMantleHairHatAim::PokingThrough);

	/** Bone's transform in the component space of Body's reference pose; false when Body has no such bone or no pose. */
	static bool BoneComponentTransform(const FMantleSkinnedMesh& Body, FName Bone, FTransform& Out);

	/** The transform from the hat mesh's own space to Body's reference component space (where fitted hair lies). MeshBounds are the mesh's, for the slot's size clamp. */
	bool HatToHair(const FMantleSkinnedMesh& Body, const FBox3f& MeshBounds, FTransform& Out, FString& Error) const;

	/** Decodes MeshBytes (LOD0 only: its positions, triangle list and bounds). */
	bool ReadMesh(TArray<FVector3f>& OutPositions, TArray<uint32>& OutIndices, FBox3f& OutBounds, FString& Error) const;

	/**
	 * The hat step on a hair the head fit made (Hair, in Body's reference space): the sources' triangles placed in that space,
	 * the head centre from Face's skin, then FMantleHairHatFit::Fit on every LOD, moving the points that poke through and
	 * cutting out triangles that poke entirely (bFacialHair: nothing is moved, only cut). Aim is FMantleHairHatFitParams::Aim
	 * (scalp hair only; the dev comparison passes UnderHatCards). Hair is left as it was when this returns false (Error says
	 * why) or when nothing changed. Any thread.
	 */
	static bool Apply(TConstArrayView<FMantleHairHatSource> Sources, const FMantleSkinnedMesh& Body, const FMantleSkinnedMesh& Face,
		bool bFacialHair, FMantleSkinnedMesh& Hair, FMantleHairHatStats& Stats, FString& Error,
		EMantleHairHatAim Aim = EMantleHairHatAim::PokingThrough);

	/** Apply with the hat and the head skin already triangle lists in Hair's space (Apply's second half; the tests' door). */
	static bool ApplyTriangles(TConstArrayView<FVector3f> HatPositions, TConstArrayView<uint32> HatIndices,
		TConstArrayView<FVector3f> ScalpPositions, TConstArrayView<uint32> ScalpIndices,
		bool bFacialHair, FMantleSkinnedMesh& Hair, FMantleHairHatStats& Stats, FString& Error,
		EMantleHairHatAim Aim = EMantleHairHatAim::PokingThrough);
};
