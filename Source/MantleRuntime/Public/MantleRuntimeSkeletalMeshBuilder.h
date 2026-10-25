// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/SkinnedAssetCommon.h"
#include "MantleSkinnedMesh.h"

class UObject;
class USkeletalMesh;
class USkeleton;
struct FReferenceSkeleton;

/** What the triangle filter kept of a LOD. */
struct FMantleTriangleFilterResult
{
	/** The kept triangles, section after section in the LOD's section order. */
	TArray<uint32> Indices;

	/** Per section of the LOD: where its kept indices start in Indices, and how many there are. */
	TArray<int32> SectionFirstIndex;
	TArray<int32> SectionIndexCount;

	int32 KeptTriangles = 0;
	int32 DroppedTriangles = 0;
};

struct FMantleSkinnedMeshBuildParams
{
	/** The supported skeleton the mesh is built on. Every bone of the built mesh must be on it, by name. */
	USkeleton* Skeleton = nullptr;

	/**
	 * #469, a place skeletal mesh on its own skeleton: build a transient USkeleton from the mesh's own bone
	 * table and reference pose (USkeleton::MergeAllBonesToBoneTree over the built mesh) instead of binding to
	 * a supported one. Skeleton and ReferencePose must be null, and the mesh must carry its reference pose.
	 * The new skeleton is the built mesh's (GetSkeleton()), in the same outer.
	 */
	bool bOwnSkeleton = false;

	/**
	 * Null: the mesh's own bone table and reference pose (avatars, faces). Otherwise the built mesh
	 * takes this reference skeleton's raw bones and pose, and the mesh's influences are remapped
	 * onto it by bone name (garments take their wearer's: pass the body mesh's GetRefSkeleton()).
	 */
	const FReferenceSkeleton* ReferencePose = nullptr;

	/** Garments only: collapse missing helper-bone influences onto a retained ancestor (#641). Requires ReferencePose. */
	bool bAllowMissingHelperBones = false;

	/**
	 * Optional, one bit per LOD0 vertex: triangles whose three vertices are all set are left out
	 * (the body under worn garments, AVATAR_CATALOG_PLAN.md section 0.1 item 4). Vertices are kept.
	 * Lower LODs take each vertex's nearest LOD0 vertex's bit (TransferHiddenVertices).
	 */
	const TBitArray<>* HiddenVertices = nullptr;

	/**
	 * Optional, per lower LOD (entry 0 is LOD1): its own hidden bits, one per vertex of that LOD, used instead of
	 * the nearest-LOD0-vertex transfer. An empty entry (or none) takes the transfer. For a mesh whose LOD0 vertices
	 * are not all one surface (a merged look: body and garments), where the nearest LOD0 vertex may be a garment's.
	 */
	TArray<TBitArray<>> LowerLODHiddenVertices;

	/**
	 * A3.8: FSkeletalMeshLODInfo::ScreenSize per built LOD, LOD0 first; a missing entry takes
	 * FMantleRuntimeSkeletalMeshBuilder::DefaultLODScreenSize (limits.json lods.screenSizes).
	 */
	TArray<float> LODScreenSizes;

	/**
	 * The most LODs to build (1 builds LOD0 alone); 0 takes the console variable
	 * Mantle.Avatar.MaxBuiltLODs (default 3, every LOD the mesh has). A5.1's harness runs LODs on and off with it.
	 */
	int32 MaxLODs = 0;

	/**
	 * The built mesh's material slots; padded with empty slots up to the highest section material index.
	 * A padded slot is named after a section's MaterialName when it has one (else "Slot<n>").
	 */
	TArray<FSkeletalMaterial> Materials;

	/** Outer of the new mesh; the transient package when null. */
	UObject* Outer = nullptr;

	/** Name of the new mesh; a unique generated one when None. */
	FName Name;

	/**
	 * Keep the CPU copies of the vertex and skin weight buffers after the RHI upload (the default: garments and bodies
	 * are read back, fitted from and merged). Off for a mesh nothing reads again (a merged look, A3.7): a cooked client
	 * then drops them once the GPU has its copy. The index buffer always keeps its copy (the engine's container does).
	 */
	bool bKeepCPUData = true;
};

/**
 * Builds a transient, renderable USkeletalMesh from an FMantleSkinnedMesh at runtime: the production
 * path for avatars and garments. No
 * MeshDescription, import data or editor code: the same sequence as the engine's own FSkeletalMeshMerge
 * and the runtime mesh-instance update of MUT section 3.
 *
 * Every LOD of the mesh is built (up to MaxLODs), each with AddLODInfo and its screen size from
 * limits.json lods.screenSizes, one render data LOD per mesh LOD, all inline (nothing to stream from).
 * Material slots are shared: a section's MaterialIndex means the same slot on every LOD (the cook
 * aligns them, MSK1). A3.7's merge (FMantleLookMerger) merges per LOD, so garments and bodies built
 * here carry their LODs into it.
 *
 * Within a LOD every section shares one bone map (the bones the LOD's vertices are weighted to, in
 * reference skeleton order), so sections may share vertices. The CPU copies of every buffer, on every
 * LOD, are kept, the skin weights included, so the mesh can be read back
 * (FMantleSkinnedMesh::ExtractFromSkeletalMesh), unless the caller says nothing will read it again
 * (FMantleSkinnedMeshBuildParams::bKeepCPUData, which then applies to every LOD).
 *
 * Triangle filter: a section left with no triangles is kept, with zero triangles and disabled, so
 * section indices and material slots mean the same with or without the filter. When every triangle is
 * dropped the index buffer holds one degenerate triangle that no section draws (the engine needs a
 * non-empty index buffer).
 *
 * Game thread (creates a UObject and its render resources).
 */
class MANTLERUNTIME_API FMantleRuntimeSkeletalMeshBuilder
{
public:
	/**
	 * Builds every LOD of Mesh (up to Params.MaxLODs). Null, with the reason logged as
	 * "MANTLE_SKINNEDMESH FAIL ..." and returned in OutError, when the mesh does not Validate, a bone
	 * is not on the skeleton (or not in the supplied reference pose), the bone table's hierarchy
	 * contradicts the skeleton's, the mesh has no reference pose and none is supplied, or the hidden
	 * bitset is not one bit per LOD0 vertex.
	 */
	static USkeletalMesh* Build(const FMantleSkinnedMesh& Mesh, const FMantleSkinnedMeshBuildParams& Params,
		FString* OutError = nullptr);

	/** The most LODs the cooked formats carry (MSK1, limits.json lods.maxLods). */
	static constexpr int32 MaxLODCount = 3;

	/**
	 * limits.json lods.screenSizes (1.0, 0.35, 0.15): where the client switches to LOD LODIndex.
	 * Past the last entry, half the previous LOD's.
	 */
	static float DefaultLODScreenSize(int32 LODIndex);

	/**
	 * Hidden bits for a lower LOD (A3.8): each vertex of To takes the bit of its nearest vertex of From
	 * (by position, a uniform grid searched ring by ring), since a reduced LOD has its own vertices and
	 * the coverage is measured on LOD0. Out gets one bit per To vertex. False, with the reason, when
	 * Hidden is not one bit per From vertex.
	 */
	static bool TransferHiddenVertices(const FMantleSkinnedMeshLOD& From, const TBitArray<>& Hidden, const FMantleSkinnedMeshLOD& To,
		TBitArray<>& Out, FString* OutError = nullptr);

	/**
	 * The triangle filter Build applies. HiddenVertices null keeps everything; otherwise it must hold
	 * one bit per vertex of LOD (false, with the reason in OutError, when it does not). LOD's indices
	 * and sections must be in range (FMantleSkinnedMesh::Validate).
	 */
	static bool FilterTriangles(const FMantleSkinnedMeshLOD& LOD, const TBitArray<>* HiddenVertices,
		FMantleTriangleFilterResult& Out, FString* OutError = nullptr);

	/**
	 * The tangents Build derives for a LOD cooked without any: from its UV layout over every triangle (hidden or
	 * not), orthogonal to the normals, W the binormal sign. One per vertex. Any thread.
	 */
	static void DeriveTangents(const FMantleSkinnedMeshLOD& LOD, TArray<FVector4f>& OutTangents);
};
