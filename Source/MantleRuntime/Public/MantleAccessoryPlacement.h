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
}
