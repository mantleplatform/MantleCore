// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class USkeletalMesh;
struct FSkeletalMaterial;

/**
 * One render section: a run of whole triangles in its LOD's index buffer drawn with one
 * material slot. Sections are identified by their material slot, never by where they sit.
 */
struct FMantleSkinnedMeshSection
{
	int32 MaterialIndex = 0;

	/** Offset of the section's first index in FMantleSkinnedMeshLOD::Indices (a multiple of 3). */
	int32 FirstIndex = 0;

	/** Number of indices (a multiple of 3). */
	int32 IndexCount = 0;

	/**
	 * The section's material name as uploaded (MSK1 carries it; unique within a cooked LOD). Bind
	 * materials by this name, never by section or slot index (AVATAR_CATALOG_PLAN.md section 0.1
	 * item 12). None when unknown; the spike helper fills it from the source asset's slot name.
	 */
	FName MaterialName;
};

/** One bone of a skinned mesh's bone table. Bound by name against a USkeleton when built. */
struct FMantleSkinnedMeshBone
{
	FName Name;

	/** Index into the same table; INDEX_NONE for the root. Parents come before their children. */
	int32 ParentIndex = INDEX_NONE;
};

/**
 * One level of detail. Vertex attributes are indexed by vertex; skin influences take
 * NumInfluences slots per vertex, slot [Vertex * NumInfluences + Slot].
 */
struct FMantleSkinnedMeshLOD
{
	/** Unreal centimetres, component space of the reference pose. */
	TArray<FVector3f> Positions;

	/** One per vertex. */
	TArray<FVector3f> Normals;

	/**
	 * Optional: empty, or one per vertex. XYZ is the tangent, W the binormal sign (negative for
	 * a mirrored UV frame). When empty the builder derives tangents from the UVs.
	 */
	TArray<FVector4f> Tangents;

	/** The one UV set, one per vertex. */
	TArray<FVector2f> UVs;

	/** Triangle list of absolute vertex indices. */
	TArray<uint32> Indices;

	TArray<FMantleSkinnedMeshSection> Sections;

	/**
	 * Hair card data (MSK1 flag bit 16): empty, or one per vertex. X and Y are the card's root UV (the
	 * skin UV under the card's root, one value per card), Z is CoordU (0 at the root, 1 at the tip),
	 * what MetaHuman's hair material reads for root-to-tip colour. Stored as half floats in MSK1, so
	 * these are exact. The builder hands them to the material as UV1 = root UV and UV2.x = CoordU.
	 */
	TArray<FVector3f> HairCards;

	/**
	 * Influence slots per vertex. MSK1 records its own count, 1 to 8 (AVATAR_CATALOG_PLAN.md section
	 * 0.1 item 18); the spike helper keeps the source asset's own count (up to MAX_TOTAL_INFLUENCES)
	 * so a round trip stays exact.
	 */
	int32 NumInfluences = 4;

	/**
	 * Index into FMantleSkinnedMesh::Bones per slot, 16 bits as in MSK1 (the MetaHuman body's table
	 * has 342 bones); a slot with weight 0 still has to name a real bone.
	 */
	TArray<uint16> InfluenceBones;

	/** Weight per slot, 0-255; a vertex's weights sum to exactly 255 (the engine's 8-bit skin weights). */
	TArray<uint8> InfluenceWeights;

	/**
	 * Storage precision the builder asks the engine for. Cooked data is float, so UVs default to
	 * full precision; the spike helper copies the source asset's flags so a round trip is exact.
	 */
	bool bFullPrecisionUVs = true;
	bool bHighPrecisionTangents = false;

	int32 NumVertices() const { return Positions.Num(); }
	int32 NumTriangles() const { return Indices.Num() / 3; }
};

struct FMantleSkinnedMesh;

/**
 * Body-shape offsets (#849): the cooked avatar body's MSK1 extension block BOFS. Up to eight named
 * shapes, each a per-LOD, per-vertex position delta and a per-bone local-translation delta (cm) that
 * the body sliders scale. Shape ids: 0 weight+, 1 weight-, 2 muscle+, 3 muscle-, 4 chest+, 5 chest-,
 * 6 hips+, 7 hips- (slider k uses 2k for a positive value and 2k+1 for a negative one).
 * Empty on every mesh that carries none. Holds no UObjects, so Apply runs on any thread.
 */
struct MANTLERUNTIME_API FMantleBodyOffsets
{
	static constexpr int32 NumShapes = 8;

	struct FShape
	{
		bool bPresent = false;

		/** [LOD][Vertex], MSK1 vertex order. */
		TArray<TArray<FVector3f>> LODs;

		/** [Bone], MSK1 bone order: a delta of the bone's local translation in the reference pose. */
		TArray<FVector3f> Bones;
	};

	/** Empty, or NumShapes entries indexed by shape id (absent ones have bPresent false). */
	TArray<FShape> Shapes;

	bool Has(const int32 Id) const { return Shapes.IsValidIndex(Id) && Shapes[Id].bPresent; }
	bool IsEmpty() const { return Shapes.Num() == 0; }

	/**
	 * Moves Mesh by the four sliders, each -100 to 100 (clamped): t = |v| / 100 times the slider's + shape for
	 * v > 0, its - shape for v < 0, nothing for a shape the mesh lacks. The deltas are added to every LOD's
	 * positions and to each bone's local reference-pose translation (skipped when the mesh has no pose).
	 * When anything moved, normals are recomputed per LOD (area-weighted, welded by identical position so the
	 * copies split at UV seams agree) and tangents are re-orthogonalised against them, keeping their sign.
	 * All zeros, or no offsets, leave Mesh bit-identical. Adds to Mesh's current positions and keeps
	 * Mesh.BodyOffsets, so run it on a copy of the base mesh, never twice on one mesh.
	 */
	static void Apply(FMantleSkinnedMesh& Mesh, int32 Weight, int32 Muscle, int32 Chest, int32 Hips);

	/** A cache key for the clamped slider values; empty when all are 0, so unchanged bodies keep today's keys. */
	static FString Key(int32 Weight, int32 Muscle, int32 Chest, int32 Hips);
};

/**
 * A skinned mesh as plain arrays: the in-memory form of the MSK1 cooked format (AVATAR_CATALOG_PLAN.md
 * section 3.4) that FMantleRuntimeSkeletalMeshBuilder turns into a transient USkeletalMesh.
 *
 * The bone table is by name so the mesh binds to whichever supported USkeleton it is built against.
 * Avatars and faces carry their own reference pose; garments carry none and are built against their
 * wearer's (FMantleSkinnedMeshBuildParams::ReferencePose).
 *
 * Holds no UObjects, so it can be read, filtered and fitted off the game thread.
 */
struct MANTLERUNTIME_API FMantleSkinnedMesh
{
	/**
	 * LOD0 first, up to three (A3.8: the plugin generates LOD1 and LOD2 at upload). Each LOD has its own
	 * vertices; a section's MaterialIndex names the same material slot on every LOD. The builder builds
	 * them all; the fitter fits them all. What is measured on the body (topology, coverage, the dressed
	 * positions) is LOD0's.
	 */
	TArray<FMantleSkinnedMeshLOD> LODs;

	/** Parent-first, one root. */
	TArray<FMantleSkinnedMeshBone> Bones;

	/** Optional: empty, or one local (parent-relative) transform per bone. */
	TArray<FTransform> ReferencePose;

	/** Avatar bodies cooked with body-shape offsets (MSK1 block BOFS); empty otherwise. */
	FMantleBodyOffsets BodyOffsets;

	bool HasReferencePose() const { return ReferencePose.Num() > 0; }

	/** Bounds of a LOD's positions; an invalid box when there is no such LOD or it has no vertices. */
	FBox3f ComputeBounds(int32 LODIndex = 0) const;

	/**
	 * Every structural rule the builder relies on: counts that agree (hair card data none or one per
	 * vertex, all finite), indices in range, sections of
	 * whole triangles inside the index buffer, influence slots naming real bones, every vertex's
	 * weights summing to 255, a parent-first bone table with one root and unique names, a reference
	 * pose (when present) per bone with finite, unit rotations. False with the first problem in
	 * OutError.
	 */
	bool Validate(FString& OutError) const;

	/**
	 * Fills Out from Source's
	 * LOD0 render data and reference skeleton (LOD0 only: cooked meshes bring their own LODs). Positions, normals, tangents, UV0, indices, sections
	 * (the LOD's material map applied), skin weights converted from section-local to mesh bone
	 * indices, the raw bone table and its reference pose, and the vertex buffers' precision flags.
	 * OutMaterials, when given, gets Source's material slots. Vertex colours and extra UV sets are
	 * not carried (MSK1 has neither).
	 *
	 * Reads the render data's CPU copies, which editor builds always keep and cooked builds keep only
	 * for LODs with bAllowCPUAccess, so it is a dev and editor tool rather than a shipping path.
	 * 16-bit skin weights are rounded to 8 bits (lossy, logged). False, logged, with the reason in
	 * OutError when the LOD or its CPU data is missing or the result does not Validate.
	 */
	static bool ExtractFromSkeletalMesh(const USkeletalMesh* Source, FMantleSkinnedMesh& Out,
		FString* OutError = nullptr, TArray<FSkeletalMaterial>* OutMaterials = nullptr);
};
