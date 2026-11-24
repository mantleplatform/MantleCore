// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleCosmeticItem.h"

/** The published accessory transform, shared by the creator preview and the shipped client. */
namespace MantleAccessoryPlacement
{
	MANTLERUNTIME_API FTransform ClampedAttach(const FMantleCosmeticTransform& Attach);
	MANTLERUNTIME_API FTransform OverriddenSocket(const FTransform& SocketLocal, const FMantleCosmeticTransform* Override);
	/** Attach * overridden socket * original socket inverse, relative to the socket component attaches to. */
	MANTLERUNTIME_API FTransform ComposeRelative(const FTransform& Attach, const FTransform& SocketLocal, const FMantleCosmeticTransform* Override);
	/** Orientation-free slot budget at the item's scale; shared defence in depth and preview behavior. */
	MANTLERUNTIME_API float BoundsClampFactor(const FBox3f& Bounds, const FVector& Scale, float MaxDimensionCm, float MaxReachCm);

	/** limits.json rigid_accessory.boundsPerSlot for a slot id ("acc_hat"); false for a slot with none. The two must be kept in step. */
	MANTLERUNTIME_API bool FindSlotBounds(const FString& SlotId, float& OutMaxDimensionCm, float& OutMaxReachCm);

	/**
	 * Where a rigid accessory sits relative to the socket it attaches to: the attach offset, clamped, its scale brought
	 * within the slot's bounds (OutFactor, at most 1; 1 for a slot without bounds or an invalid box) and placed on the
	 * overridden socket (ComposeRelative). The one answer for the assembler (FMantleAvatarAccessories::Sync) and for the
	 * hair fit, which needs the hat's real pose off the game thread (#389).
	 */
	MANTLERUNTIME_API FTransform PlacedRelative(const FMantleCosmeticTransform& Attach, const FBox3f& Bounds, const FString& SlotId,
		const FTransform& SocketLocal, const FMantleCosmeticTransform* Override, float& OutFactor);
}
