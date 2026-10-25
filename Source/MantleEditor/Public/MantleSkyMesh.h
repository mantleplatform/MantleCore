// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class AActor;
class UStaticMeshComponent;

/**
 * A level's own sky dome (#63): a huge inverted sphere or dome mesh with a painted or panning sky material, usually
 * inside a sky Blueprint (the engine's BP_Sky_Sphere, a Fab map's BP_Embersky). Mantle supplies its own sky, so the
 * exporter leaves these meshes out the way it leaves out SkyLight and SkyAtmosphere, and says so in the coverage
 * (the "Sky" category); the rest of the actor (a directional light, say) publishes as it always did.
 *
 * A static mesh component is a sky mesh when any of these holds, in this order:
 *  1. Its actor is the engine's sky sphere (a class under /Engine/EngineSky/, BP_Sky_Sphere or a child of it).
 *  2. One of its materials is flagged Is Sky (UMaterial::bIsSky): the engine's own marker for a sky dome material.
 *  3. It is huge (a half-extent of MinHalfExtent or more), its bounds enclose the centre of every other mesh in
 *     the level that is smaller than it, and its look says sky: its actor, Blueprint or component is named like a
 *     sky ("Sky", "Cloud") and its material is unlit or two-sided, or its material is unlit and two-sided.
 * Real large geometry is kept: terrain, a mountain backdrop or a big building has to be huge, enclose the level
 * and look like a sky at once, and a lit mesh that is not named like a sky never does.
 */
class MANTLEEDITOR_API FMantleSkyMesh
{
public:
	/** The smallest half-extent of a sky dome, in centimetres (100 m): a gazebo's dome is not a sky. */
	static constexpr double MinHalfExtent = 10000.0;

	/** Whether Component is a sky mesh by the rule above. OutWhy, when given, says which part of it held. */
	static bool IsSkyMesh(const UStaticMeshComponent* Component, FString* OutWhy = nullptr);

	/** Whether Actor is a Static Mesh Actor with nothing else a player could see, whose mesh is a sky mesh: the whole actor is left out. */
	static bool IsSkyMeshActor(const AActor* Actor);

	/** The coverage detail for a sky mesh left out as an actor of its own, phrased to follow its label. */
	static FString StandaloneDetail();

	/** The coverage detail for a sky mesh component of a Blueprint, phrased to follow the actor's label. */
	static FString ComponentDetail(const FString& ComponentName);
};
