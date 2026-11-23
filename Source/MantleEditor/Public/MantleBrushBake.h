// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleSplineMeshBake.h"

class ABrush;
class UWorld;
struct FMantlePlaceAssetLimits;

/**
 * BSP brushes become ordinary static meshes at export (F23). A level's BSP (the Cube, Cylinder and Stairs brushes of Geometry mode,
 * and whatever a Fab map was blocked out with) is not a mesh a place can carry, so it is baked here: the level's static Add and
 * Subtract brushes are cut against each other into a temporary model of their own, exactly as the editor's Build does, and the
 * result's visible, solid surfaces become static meshes with each surface's material and texture coordinates.
 *
 * Nothing of the creator's level is touched: the temporary model is a transient object, and the brushes are only read. The baked
 * meshes are transient too, and are cached while the brushes are unchanged (the cable bake's way), so Gather, Check and Export
 * see the same meshes and the BSP is cut once, not on every Check.
 *
 * What a baked mesh keeps: the geometry, one mesh per material (cut into meshes within the place mesh limits), the material of
 * each surface and its texture mapping, with the triangles as its collision. What it gives up: BSP lightmaps and per-surface
 * flags beyond "invisible" and "portal", which are left out.
 */
class MANTLEEDITOR_API FMantleBrushBake
{
public:
	/**
	 * Whether Brush is BSP geometry Export bakes: a static brush a creator placed (not the builder brush, a volume, a brush shape
	 * or a movable brush) with a shape, in a level of the world.
	 */
	static bool IsBakeSource(const ABrush* Brush);

	/** Every brush IsBakeSource accepts in World, in a fixed order. */
	static TArray<ABrush*> CollectSources(UWorld* World);

	/** Bakes World's BSP. Cached while the brushes are unchanged: calling it again returns the same meshes. */
	static FMantleSplineBakeResult Bake(UWorld* World, const FMantlePlaceAssetLimits& Limits);

	/** Bakes just these brushes (each must satisfy IsBakeSource), cut level by level. PruneWorld is the world whose cache entries this call replaces, or null. */
	static FMantleSplineBakeResult BakeBrushes(TConstArrayView<ABrush*> Brushes, const FMantlePlaceAssetLimits& Limits, UWorld* PruneWorld = nullptr);

	/** Forgets every cached bake (tests; the editor's shutdown). */
	static void ClearCache();
};
