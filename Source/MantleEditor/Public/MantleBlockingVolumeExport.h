// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"

class AActor;

/**
 * One Blocking Volume Export publishes as an invisible wall: a hidden, blocking library cube stretched over the volume's box.
 * Mantle already builds hidden colliding meshes (FMantleStaticMeshProps::bVisible), so this adds no place field.
 */
struct MANTLEEDITOR_API FMantleBlockingWall
{
	const AActor* Actor = nullptr;

	/** The entity id pass 1 gave the actor (set by the exporter, not by ReadWall). */
	FString Id;

	/** World transform of the library cube (100 cm, centered) that fills the volume: its scale is the box's size over 100. */
	FTransform Transform;

	EMantleCollision Collision = EMantleCollision::Blocking;

	/** False for the engine's invisible-wall profile, which lets sight and pointer traces through. */
	bool bBlocksVisibility = true;
};

/**
 * Blocking Volumes become invisible walls at export. The volume is a brush; Export reads its geometry and, when it is a box
 * (what dragging out a Blocking Volume makes), publishes a hidden cube of that size and place. A volume of any other shape
 * (a cylinder, stairs) is still left out with a warning: a box is all a library cube can stand for.
 */
class MANTLEEDITOR_API FMantleBlockingVolumeExport
{
public:
	/** Whether Actor is a Blocking Volume (the engine's class, or one derived from it). */
	static bool IsBlockingVolume(const AActor* Actor);

	/**
	 * Reads Actor as an invisible wall: true when it is a Blocking Volume whose brush is a box and whose collision is on.
	 * False for any other shape (left out with a warning, DescribeUnpublishedContent) and for a volume that collides with nothing.
	 */
	static bool ReadWall(const AActor* Actor, FMantleBlockingWall& OutWall);

	/**
	 * Whether Actor is a Blocking Volume that collides with nothing: no wall to publish, and nothing a player would miss, so it
	 * is left out without a warning.
	 */
	static bool HasNoCollision(const AActor* Actor);

	/** The box a brush's polygons make, in the brush component's space: false unless they make an axis-aligned box. */
	static bool ReadLocalBox(const AActor* Actor, FVector& OutCenter, FVector& OutSize);

	/**
	 * A Blocking Volume's collision as the place says it: OutCollision from its collision setting, and whether it stops sight traces. False
	 * for a volume that collides with nothing.
	 */
	static bool ReadCollision(const AActor* Actor, EMantleCollision& OutCollision, bool& bOutBlocksVisibility);

	/** The bounds of a brush's polygons, in the brush component's space, whatever its shape: false for a brush with no volume. */
	static bool ReadLocalBounds(const AActor* Actor, FVector& OutCenter, FVector& OutSize);
};
