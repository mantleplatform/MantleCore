// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"
#include "MantleSurface.h"

class AActor;
class USplineMeshComponent;
class UStaticMesh;
class UWorld;
struct FMantlePlaceAssetLimits;

/**
 * One static mesh a group of spline mesh components was baked into (#628): world-space geometry relative to
 * Pivot, deformed along each component's spline exactly as the component renders it.
 *
 * The mesh is transient (never written into the creator's Content) and is kept alive by the bake's own
 * cache, so Gather, Check and Export all see the same object, and so the same mesh key, for as long as the
 * source components are unchanged.
 */
struct MANTLEEDITOR_API FMantleBakedSplineMesh
{
	UStaticMesh* Mesh = nullptr;

	/** The entity's name, and the text its id is hashed from (stable while the owner and material set are). */
	FString Name;
	FString IdSeed;

	/** The owning actor, as the outliner and the export's paths name it. */
	FString OwnerLabel;
	FString OwnerName;

	/** The entity's transform: a translation to the mesh's pivot. */
	FTransform Transform;

	EMantleCollision Collision = EMantleCollision::Blocking;
	EMantleSurface Surface = EMantleSurface::Default;
	bool bCastShadow = true;

	int32 SourceComponents = 0;
	int32 Triangles = 0;
	int32 Vertices = 0;
};

/** Something that stops spline meshes baking, phrased to follow the owning actor's label, with the fix. */
struct MANTLEEDITOR_API FMantleSplineBakeProblem
{
	FString OwnerLabel;
	FString OwnerName;
	FString What;
	FString Remedy;
};

/** One owner's line in the bake's report: how many spline meshes became how many static meshes. */
struct MANTLEEDITOR_API FMantleSplineBakeReport
{
	FString OwnerLabel;

	/** Spline mesh components baked, and components a player could not see or touch, left out. */
	int32 Components = 0;
	int32 Skipped = 0;
	int32 Meshes = 0;

	/** "233 spline meshes on Landscape1 baked into 6 meshes". */
	FString ToString() const;
};

struct MANTLEEDITOR_API FMantleSplineBakeResult
{
	TArray<FMantleBakedSplineMesh> Meshes;
	TArray<FMantleSplineBakeProblem> Problems;
	TArray<FMantleSplineBakeReport> Reports;
};

/**
 * Spline meshes become ordinary static meshes at export (#628). USplineMeshComponent output (a landscape's
 * road and river-bank splines, PCG's Spawn Spline Mesh, Blueprint spline meshes) is deformed on the GPU, so
 * a place cannot carry it as it is: it is baked here, on the CPU, with the engine's own deformation
 * (USplineMeshComponent::PropagateSplineDeformationToMesh, what Merge Actors uses), merged by owning actor
 * and material set, and cut into meshes within the place mesh limits.
 *
 * What a baked mesh keeps: the deformed visible triangles, tangents, UVs, vertex colours and the effective
 * material of each slot. Collision is the visible triangles (complex as simple), where the source
 * components collide at all.
 */
class MANTLEEDITOR_API FMantleSplineMeshBake
{
public:
	/**
	 * Whether Component is placed content Export bakes: a spline mesh with a mesh that a player can see
	 * (visible in game) or touch (collides), on a placed actor. Editor-only, transient and mesh-less
	 * components, and ones nothing can see or touch, publish nothing.
	 */
	static bool IsBakeSource(const USplineMeshComponent* Component);

	/**
	 * Every component IsBakeSource accepts in World, in actor order then by component name. OutNotPublished,
	 * when given, counts per owner label the spline meshes left out because nothing could see or touch them.
	 */
	static TArray<USplineMeshComponent*> CollectSources(UWorld* World, TMap<FString, int32>* OutNotPublished = nullptr);

	/**
	 * Bakes World's spline meshes. The result is cached while the sources are unchanged, so calling it
	 * again (Gather, Check, Export) returns the same meshes; what a changed or removed source baked
	 * before is dropped.
	 */
	static FMantleSplineBakeResult Bake(UWorld* World, const FMantlePlaceAssetLimits& Limits);

	/** Bakes just these components (each must satisfy IsBakeSource). PruneWorld is the world whose cache entries this call replaces, or null. */
	static FMantleSplineBakeResult BakeComponents(TConstArrayView<USplineMeshComponent*> Components, const FMantlePlaceAssetLimits& Limits, UWorld* PruneWorld = nullptr);

	/** Forgets every cached bake (tests; the editor's shutdown). */
	static void ClearCache();
};
