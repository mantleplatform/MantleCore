// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

struct FMantleCosmeticDraft;

/**
 * #65, #136, #389: the creator-set item settings (limits.json itemSettings: tuckable, tallBoots, hairRule, pairId) and the
 * earring Pair. A Pair is two items, one in acc_ear_l and one in acc_ear_r (a mirrored copy of the left mesh), both
 * carrying the same pairId. The pure parts live here so they are testable without the editor's UI.
 */
class MANTLEEDITOR_API FMantleItemSettings
{
public:
	/** The item field names, as limits.json spells them. */
	static const TCHAR* Tuckable() { return TEXT("tuckable"); }
	static const TCHAR* TallBoots() { return TEXT("tallBoots"); }
	static const TCHAR* HairRule() { return TEXT("hairRule"); }
	static const TCHAR* PairIdField() { return TEXT("pairId"); }

	/** pairId's pattern ^[A-Za-z0-9_-]{1,64}$, checked by hand. */
	static bool IsValidPairId(const FString& Id);

	/**
	 * A pair id from the item's name and a guid: the name lowercased, every character the pattern does not allow turned into
	 * '-', cut to 48 characters, then '-' and the guid's first 8 hex digits. The same name and guid give the same id.
	 */
	static FString MakePairId(const FString& ItemName, const FGuid& Guid);

	/** Name with " (left)" or " (right)" at its end, once: a suffix it already ends with (either side) is replaced. */
	static FString WithSideSuffix(const FString& Name, bool bRight);

	/**
	 * The signs of the mirror between the left and the right earring. The accessory draft's mesh is aligned to the avatar's axes
	 * (FMantleAccessorySettings::AlignToAvatarAxes turns it into the bone's frame with the attach rotation), and a MetaHuman's
	 * component axes have X to the body's left and right, Y forward, Z up. So the mesh's own X is the left-right axis, and the
	 * right socket is the left one mirrored: its frame is the left frame conjugated by this mirror.
	 */
	static FVector MirrorSigns() { return FVector(-1.0, 1.0, 1.0); }

	/**
	 * The attach offset (location cm, rotation as pitch, yaw, roll degrees, scale) of the left earring as the right one's: the
	 * same placement conjugated by MirrorSigns (FMantleMirrorBake::ReflectTransform). The right socket frame is the left one
	 * mirrored, and the right mesh is the left mesh mirrored (FMantleMirrorBake::MirroredCopy), so the worn earring is the mirror image.
	 */
	static void MirrorAttach(const FVector3f& Location, const FVector3f& Rotation, const FVector3f& Scale,
		FVector3f& OutLocation, FVector3f& OutRotation, FVector3f& OutScale);

	/**
	 * The draft of the right earring from the left one (Left.Slot acc_ear_l): its source is a mirrored transient copy of the
	 * left static mesh, its slot acc_ear_r, its name the left's with " (right)", its attach offset mirrored, its pair id the left's.
	 * False with OutWhy when the source is not a static mesh or cannot be mirrored.
	 */
	static bool MakeRightDraft(const FMantleCosmeticDraft& Left, FMantleCosmeticDraft& OutRight, FString& OutWhy);
};
