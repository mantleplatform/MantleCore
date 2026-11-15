// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"

/**
 * Which entities of a place end Static, and where each stands in the world: worked out from the place data alone (no engine calls) before anything is spawned,
 * so the builder can create each of them where it stands and Static, before its component registers (its physics body is then made once, static, in place,
 * and never as a Movable body that is moved and made again).
 *
 * The rule (it is the builder's freeze rule, unchanged):
 *  - An entity ends Static when it is place geometry: a StaticMesh or InstancedMesh that blocks, a Terrain (it always blocks) or a Scatter (it blocks nothing, but
 *    is immovable scenery all the same), and it is not marked dynamic (a physics entity is dynamic too: the parser sets it).
 *  - Every ancestor of it must be a Group that is not dynamic, up to the root. Those Groups end Static as well: a Static child cannot follow a Movable parent.
 *  - Anything else (a light, a trigger, a dynamic entity, a mesh under a light or another mesh, a Group above only things that move) ends Movable and is not here.
 *
 * World transforms: a Group chain composes as the engine composes attached components (the child's own transform, then each ancestor's, innermost first).
 */
struct MANTLERUNTIME_API FMantlePlaceFreezePlan
{
	/** Every entity that ends Static, by id, with the transform it stands at in the world. */
	TMap<FString, FTransform> StaticWorld;

	/**
	 * Every entity of the place, by id, with the transform it stands at in the world (Static or not). An entity that ends Movable is spawned there too, so that no
	 * body is ever parked at the origin until the Transforms pass: a pile of thousands of Movable bodies on one spot costs the solver about 18 ms a step.
	 */
	TMap<FString, FTransform> AllWorld;

	/** The geometry that ends Static, in place order, and the Groups above it, each after all of its own ancestors. */
	TArray<FString> GeometryIds;
	TArray<FString> GroupIds;

	static FMantlePlaceFreezePlan Make(const FMantlePlace& Place);

	bool IsStatic(const FString& EntityId) const { return StaticWorld.Contains(EntityId); }
	/** The entity's world transform when it ends Static; null when it ends Movable. */
	const FTransform* WorldOf(const FString& EntityId) const { return StaticWorld.Find(EntityId); }
	/** The entity's world transform, whether it ends Static or Movable; null for an id the place does not have. */
	const FTransform* AnyWorldOf(const FString& EntityId) const { return AllWorld.Find(EntityId); }
};
