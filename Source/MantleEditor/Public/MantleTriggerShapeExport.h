// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"

class AActor;
class UActorComponent;
class UCapsuleComponent;

/**
 * Trigger shapes Export carries beyond a Trigger Box and a Trigger Sphere (F18). A place's triggers are boxes and spheres, so:
 *  - a Trigger Volume, the engine's brush trigger, is a Box trigger when its brush is a box (a Trigger Volume of any other shape is
 *    still left out with a warning);
 *  - a capsule (a Trigger Capsule, or a capsule component of a prefab) is a Box trigger around it, or a Sphere trigger when the
 *    capsule is no taller than it is wide. The corners of the box are the only difference, so the report says it was approximated.
 */
class MANTLEEDITOR_API FMantleTriggerShapeExport
{
public:
	/** Whether Actor is a Trigger Volume (the engine's brush trigger, or one derived from it). */
	static bool IsTriggerBrush(const AActor* Actor);

	/**
	 * Reads a Trigger Volume whose brush is a box: OutCenter is the box's center and OutExtent its half-size, both in the actor's
	 * own space (as a Trigger Box's unscaled extent is; the actor's scale stretches them). A brush that is not a box is read as the box
	 * around its bounds, and bOutApproximated says so. False for a brush with no volume.
	 */
	static bool ReadBoxBrush(const AActor* Actor, FVector& OutCenter, FVector& OutExtent, bool* bOutApproximated = nullptr);

	/** Whether Actor is a Trigger Volume Export publishes (ReadBoxBrush). */
	static bool IsBoxTriggerBrush(const AActor* Actor);

	/** Whether Component is a capsule collision shape. */
	static bool IsCapsule(const UActorComponent* Component);

	/**
	 * Whether Component is a capsule that acts as a trigger: it generates overlap events, and it only overlaps (collision query only, or
	 * no channel it blocks). A capsule that blocks is a character's or a prop's collision, not a trigger, and stays what it is today.
	 */
	static bool IsTriggerCapsule(const UActorComponent* Component);

	/** A capsule's trigger: Box around it, or Sphere when it is no taller than it is wide, with the unscaled extent either uses. */
	static void ReadCapsule(const UCapsuleComponent* Capsule, EMantleTriggerShape& OutShape, FVector& OutExtent);
};
