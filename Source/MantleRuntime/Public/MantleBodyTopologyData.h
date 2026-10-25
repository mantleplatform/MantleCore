// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "UObject/SoftObjectPath.h"
#include "MantleBodyTopologyData.generated.h"

class USkeletalMesh;

/** What one LOD of a body mesh measures as: the numbers the reference topology is compared on. */
struct MANTLERUNTIME_API FMantleBodyLODTopology
{
	int32 LODIndex = INDEX_NONE;
	int32 VertexCount = 0;
	int32 TriangleCount = 0;
	int32 SectionCount = 0;

	/** UMantleBodyTopologyData::ComputeUVHash of the LOD's UV0. */
	uint32 UVHash = 0;

	/** UMantleBodyTopologyData::ComputePositionsHash of the LOD's positions. */
	uint32 PositionsHash = 0;

	/** Reference pose, component space, cm. */
	FBox3f Bounds = FBox3f(ForceInit);

	/** "verts=8568 tris=15196 sections=1 uvhash=4514a950 poshash=... size=(114.5 x 42.9 x 158.9) cm". */
	FString Describe() const;
};

/**
 * Mantle's reference body topology (AVATAR_CATALOG_PLAN.md section 3.3, amended by section 0.1
 * item 4): the vertex count and quantised UV hash of the MetaHuman body at UE Optimized Medium LOD0
 * (= High LOD1), taken from the pinned converted Aera body by the MantleBuildBodyTopology commandlet and saved as
 * /Game/Avatars/Topology/DA_MantleBodyTopology. A body LOD with the same count and hash has the
 * same vertex order and UV layout, so garments authored on any such body fit any other (the
 * garment fitter's contract). No positions and no region ids: coverage replaced the region paint.
 *
 * The same numbers are written to schema/avatar/body-topology-v1.json for the Go cook worker and
 * the plugin, which check uploads without an engine.
 */
UCLASS()
class MANTLERUNTIME_API UMantleBodyTopologyData : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Vertices of the reference LOD (render vertices, as the fitter and the builder see them). */
	UPROPERTY(VisibleAnywhere, Category = "Topology")
	int32 VertexCount = 0;

	/** Triangles of the reference LOD; informational (the count and UV hash decide a match). */
	UPROPERTY(VisibleAnywhere, Category = "Topology")
	int32 TriangleCount = 0;

	/** ComputeUVHash of the reference LOD. */
	UPROPERTY(VisibleAnywhere, Category = "Topology")
	uint32 UVHash = 0;

	/** The body the topology was taken from, and which of its LODs. */
	UPROPERTY(VisibleAnywhere, Category = "Topology")
	FSoftObjectPath ReferenceAsset;

	UPROPERTY(VisibleAnywhere, Category = "Topology")
	int32 ReferenceLOD = INDEX_NONE;

	/** ComputePositionsHash of the reference LOD: tells whether the reference body itself changed. */
	UPROPERTY(VisibleAnywhere, Category = "Topology")
	uint32 PositionsHash = 0;

	/** Same vertex count and UV hash. */
	bool Matches(const FMantleBodyLODTopology& LOD) const;

	/** The first LOD of Mesh that Matches, INDEX_NONE when none does (or its render data has no CPU copies). */
	int32 FindReferenceLOD(const USkeletalMesh* Mesh) const;

	/**
	 * The quantised UV hash (the backend's Go UV hash must match it): per vertex in
	 * order, U and V each scaled by 2048 and rounded to an int32, both CRC32-chained
	 * (FCrc::MemCrc32, seed 0, 4 little-endian bytes each). Proves vertex order as well as layout.
	 */
	static uint32 ComputeUVHash(TConstArrayView<FVector2f> UVs);

	/** CRC32 (FCrc::MemCrc32, seed 0) of the positions as little-endian float32 x, y, z per vertex, in order. */
	static uint32 ComputePositionsHash(TConstArrayView<FVector3f> Positions);

	/** "4514a950": lower-case, eight digits, the form the JSON and the logs use. */
	static FString FormatHash(uint32 Hash);

	/** LODs Mesh has render data for (0 when none). */
	static int32 GetNumLODs(const USkeletalMesh* Mesh);

	/**
	 * Reads LOD LODIndex of Mesh's render data (CPU copies: editor builds always keep them, cooked
	 * builds only with bAllowCPUAccess, so a dev and editor tool like ExtractFromSkeletalMesh).
	 * OutPositions / OutUVs may be null. False, with the reason in OutError, when the LOD or its CPU
	 * data is missing.
	 */
	static bool ReadLOD(const USkeletalMesh* Mesh, int32 LODIndex, FMantleBodyLODTopology& OutTopology,
		TArray<FVector3f>* OutPositions = nullptr, TArray<FVector2f>* OutUVs = nullptr, FString* OutError = nullptr);
};
