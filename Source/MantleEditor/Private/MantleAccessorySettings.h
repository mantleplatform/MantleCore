// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleAvatarContract.h"
#include "MantleCosmeticExport.h"

/** The point of an accessory's own bounds that a slot's default placement puts on the slot's socket. */
enum class EMantleAccessoryAnchor : uint8
{
	/** Not seated by default: the mesh's pivot sits on the socket (every slot before the small ones). */
	Pivot,
	/** The bounds' center (a ring on its finger, a watch on the wrist, a stud in the nose, glasses on the face). */
	Center,
	/** The top of the bounds, centered (an earring's hook on the ear). */
	TopCenter,
};

/** Creator corrections, keyed by source object path and accessory slot; never alter the source mesh. */
struct FMantleAccessorySettings
{
	static FString DefaultConfigFile();
	/** Returns whether this exact source/slot has saved settings, including an explicitly disabled correction. */
	static bool Load(FMantleCosmeticDraft& Draft, const FString& ConfigFile);
	static void Save(const FMantleCosmeticDraft& Draft, const FString& ConfigFile);
	/** Align standard Unreal component-space mesh axes to this avatar's neutral anchor, preserving location and scale. */
	static bool AlignToAvatarAxes(FMantleCosmeticDraft& Draft, const USkeletalMesh* Body, FString& OutError);

	/**
	 * The placement a new accessory starts with: AlignToAvatarAxes, and for a neck slot also RestOnShoulders, and for a small slot
	 * (eyes, ears, nose, wrists, ring) SeatOnSocket with its DefaultAnchor. Saved placements
	 * (Load) are never passed through here, so a creator's own always wins.
	 */
	static bool ApplyDefaultPlacement(FMantleCosmeticDraft& Draft, const USkeletalMesh* Body, FString& OutError, const USkeletalMesh* Face = nullptr);

	/** Which point of its bounds a slot's default placement seats on the socket. Pivot for every slot but the small ones. Pure (tested). */
	static EMantleAccessoryAnchor DefaultAnchor(const FString& Slot);

	/** The point of Bounds (the mesh's box after axis alignment, avatar axes: Z up) that Anchor names. The box's center for Pivot. Pure (tested). */
	static FVector3f AnchorPoint(EMantleAccessoryAnchor Anchor, const FBox3f& Bounds);

	/**
	 * Moves an axis-aligned accessory (the draft's rotation already set by AlignToAvatarAxes) so the slot's anchor point of its
	 * bounds, as the upload centers them on the socket (RecenterOffset), sits on the slot's socket: only the attach location
	 * changes, within the panel's range. False, with OutError, when the draft has no static mesh or the mesh has no bounds.
	 */
	static bool SeatOnSocket(FMantleCosmeticDraft& Draft, EMantleAccessoryAnchor Anchor, FString& OutError);

	/**
	 * Lowers a neck accessory (already aligned) along the avatar's up axis until its upper half rests on the skin of Body and
	 * Face (the neck belongs to both) in the reference pose, so a chain lies on the shoulders and base of the neck instead of
	 * floating at the jaw (what hangs below, a pendant, is left to rest on the chest). Only the draft's attach location
	 * changes. False when Body lacks the neck bone or vertices, or nothing lies below the chain within 30 cm.
	 */
	static bool RestOnShoulders(FMantleCosmeticDraft& Draft, const USkeletalMesh* Body, FString& OutError, const USkeletalMesh* Face = nullptr);

	/**
	 * How far Links (points, Z up) can drop before one reaches the surface below it, with ClearanceCm to spare: each link
	 * falls straight down onto the highest triangle beneath it (Triangles holds three corners per triangle), so a link
	 * cannot slip between vertices. Surfaces above a link are ignored. Negative when no link has a surface below it within
	 * MaxDropCm. Pure (tested).
	 */
	static double DropOntoSurface(TConstArrayView<FVector> Links, TConstArrayView<FVector> Triangles, double ClearanceCm, double MaxDropCm);

	/**
	 * The shift to take off every vertex so a mesh whose pivot is out of the slot's reach (a model exported with its pivot at
	 * the feet) is centered on its pivot, or zero when the pivot already reaches or the mesh is too big for the slot either way.
	 */
	static FVector3f RecenterOffset(const FBox3f& Bounds, const FMantleContractAccessoryBounds& Limits);

	/**
	 * The mesh that uploads for Source: Source itself within the class's triangle limit, otherwise the cached reduced copy
	 * the export makes. Source when it cannot be reduced. Never modifies Source.
	 */
	static const UStaticMesh* UploadMesh(const UStaticMesh* Source, const FMantleContractClass& Class);
};
