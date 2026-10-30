// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleSplineMeshBake.h"

class UActorComponent;
class UCableComponent;
class UWorld;
struct FMantlePlaceAssetLimits;

/**
 * Cables become ordinary static meshes at export (the Cable Component plugin's Cable Actor: a hanging wire). A cable is
 * drawn on the GPU from its simulated points, so a place cannot carry it as it is: it is baked here, on the CPU, into
 * the same tube the component draws (its width, sides, UVs and material) along the points it has in the editor, where
 * it simulates and hangs. The tube is a fixed shape: in a place the cable does not sway, and it collides with nothing,
 * as the component itself collides with nothing in the game.
 *
 * Cables that share a material are merged, and cut into meshes within the place mesh limits. The result has the spline
 * bake's types, and is cached the same way, so Gather, Check and Export see the same meshes.
 */
class MANTLEEDITOR_API FMantleCableBake
{
public:
	/** Whether Component is a Cable Component (matched by class, so callers need not include the plugin's header). */
	static bool IsCableComponent(const UActorComponent* Component);

	/**
	 * Whether Component is placed content Export bakes: a cable a player can see (visible in game), on a placed
	 * actor. Editor-only and transient components, and hidden ones, publish nothing.
	 */
	static bool IsBakeSource(const UCableComponent* Component);

	/** Every component IsBakeSource accepts in World, in actor order then by component name. */
	static TArray<UCableComponent*> CollectSources(UWorld* World);

	/**
	 * Bakes World's cables. The result is cached while the sources are unchanged, so calling it again (Gather,
	 * Check, Export) returns the same meshes; what a changed or removed cable baked before is dropped.
	 */
	static FMantleSplineBakeResult Bake(UWorld* World, const FMantlePlaceAssetLimits& Limits);

	/** Bakes just these components (each must satisfy IsBakeSource). PruneWorld is the world whose cache entries this call replaces, or null. */
	static FMantleSplineBakeResult BakeComponents(TConstArrayView<UCableComponent*> Components, const FMantlePlaceAssetLimits& Limits, UWorld* PruneWorld = nullptr);

	/** Forgets every cached bake (tests; the editor's shutdown). */
	static void ClearCache();
};
