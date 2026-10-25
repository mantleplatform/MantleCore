// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class IMeshReduction;
class UStaticMesh;
struct FMantlePlaceAssetLimits;
struct FMeshDescription;
struct FStaticMeshLODResources;

/**
 * An over-limit place mesh is reduced automatically at publish: a transient copy of
 * the mesh with fewer triangles stands in for it, with a notice. The creator's asset is never touched, in memory or
 * on disk.
 *
 * The copy is made as the mirror bake makes its meshes (FMantleMirrorBake): the render LOD 0 (a Nanite mesh's
 * fallback, which is what it exports as anyway) is read into a mesh description, reduced with the editor's static
 * mesh reduction (IMeshReduction::ReduceMeshDescription), and built into an RF_Transient static mesh with the
 * source's material slots (same indices and slot names), its lightmap setup and its simple collision. The copy is
 * kept alive by this class's own cache, keyed by (source, limits), so that Check and Convert see one object for as
 * long as the source is unchanged. A source the reducer could not bring within the limits is remembered too, so the
 * (slow) attempt is not repeated for every Check.
 *
 * The aim is TargetShare of each limit, not the limit itself: the reducer stops near its target, not on it.
 */
class MANTLEEDITOR_API FMantleMeshReduce
{
public:
	/** The share of the triangle and vertex limits a reduction aims for. */
	static constexpr float TargetShare = 0.9f;

	/** A reduction that comes out over a limit is tried again this many times in all, each aiming lower. */
	static constexpr int32 MaxAttempts = 3;

	/** Whether the editor has a static mesh reducer to use (the engine's, in an editor build). */
	static bool IsAvailable();

	/**
	 * What a conversion that reduces folds into its cache key: the targets and the reducer's own version, so a
	 * newer reducer does not reuse an older one's result. Empty when no reducer is available.
	 */
	static FString CacheTag(const FMantlePlaceAssetLimits& Limits);

	/** The LOD has vertices an export can read (an editor build always keeps the CPU copy; a cooked asset may not). */
	static bool HasCpuCopy(const FStaticMeshLODResources& LOD);

	/** The name given to the polygon group and the material slot a source slot becomes in a copy: Prefix + the slot's index. */
	static FName SlotName(const TCHAR* Prefix, int32 Slot);

	enum class EReadResult : uint8
	{
		Read,

		/** No such LOD, or no CPU copy of its vertices. */
		Unreadable,

		/** The LOD is readable and has no triangles. */
		Empty,
	};

	/**
	 * Render LOD LODIndex of Source as a mesh description, as the mirror bake and the reduction read it. The engine's
	 * export makes one polygon group per section with triangles, in section order, named by the slot's imported name
	 * (which a mesh may repeat or leave empty); they are renamed SlotName(SlotPrefix, the section's material index), so
	 * each section keeps its slot.
	 */
	static EReadResult ReadRenderLOD(const UStaticMesh& Source, int32 LODIndex, const TCHAR* SlotPrefix, FMeshDescription& Out);

	/**
	 * In reduced to about TargetTriangles triangles and TargetVertices vertices (whichever takes more work to reach
	 * both), polygon groups and their names kept. False with OutWhy when there is no reducer.
	 */
	static bool ReduceDescription(const FMeshDescription& In, int32 TargetTriangles, int32 TargetVertices, FMeshDescription& Out, FString& OutWhy);

	/**
	 * Reduces In towards TargetShare of Limits' triangles and vertices, then hands each try to Measure, which builds
	 * what it likes from the description and answers with the triangles and vertices that came out (false stops, with
	 * OutWhy set by Measure). A try over either limit is followed by another aiming lower, MaxAttempts in all.
	 * True when the last try Measure saw is within both limits.
	 */
	static bool ReduceUntilFits(const FMeshDescription& In, const FMantlePlaceAssetLimits& Limits,
		TFunctionRef<bool(const FMeshDescription& Reduced, int32& OutTriangles, int32& OutVertices)> Measure, FString& OutWhy);

	/**
	 * A transient copy of Source with its render LOD 0 reduced within Limits' triangles and vertices, or null with
	 * OutWhy set (no reducer, no readable geometry, the reducer could not get under a limit, or reducing removed one of
	 * the mesh's materials). The copy has one LOD and the source's material slots, in the same indices. Cached by
	 * (Source, Limits.MaxTriangles, Limits.MaxVertices); a source that changed since (its geometry or its material
	 * slots) is reduced again. Source is never modified.
	 */
	static UStaticMesh* ReducedCopy(const UStaticMesh* Source, const FMantlePlaceAssetLimits& Limits, FString& OutWhy);

	/** Forgets every cached copy (tests; the end of a publish; the editor's shutdown). */
	static void ClearCache();

	/** Tests: reduce with Reducer in place of the editor's (null: the editor's again). Not owned. */
	static void SetReducerForTests(IMeshReduction* Reducer);

	/** Tests: behave as though the editor has no static mesh reducer. */
	static void SetUnavailableForTests(bool bUnavailable);
};
