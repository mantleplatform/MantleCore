// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleSkinnedMesh.h"

/** What an MSK1 skinned mesh is for (the file's role field). */
enum class EMantleSkinnedMeshRole : uint8
{
	AvatarBody = 0,
	AvatarFace = 1,
	Garment = 2,
	/**
	 * A place's skeletal mesh on its own skeleton (#469): it carries its bone table and reference pose,
	 * and its skeleton is the table's hash (FMantleSkeletonHash), which its clips carry too. Nothing that
	 * loads avatar content accepts it.
	 */
	PlaceMesh = 3,
	/**
	 * A creator vehicle's body on its own bone table, skeleton "vehicle" (#435); only inside MVH1.
	 */
	VehicleBody = 4,
};

/** The MSK1 header fields FMantleSkinnedMesh has no place for. */
struct FMantleSkinnedMeshHeader
{
	EMantleSkinnedMeshRole Role = EMantleSkinnedMeshRole::AvatarBody;

	/**
	 * The supported skeleton the mesh is skinned to: "metahuman", "ue5mannequin", "ue4mannequin", "uefnmannequin" or "metahuman_face"; for a
	 * place skeletal mesh (#469), its own bone table's hash ("skel_" + 16 hex digits), checked on read; "vehicle"
	 * for a vehicle body (#435).
	 */
	FString Skeleton;

	/** Avatar bodies: LOD0 has the reference topology, so garments can be fitted to it. */
	bool bGarmentCompatible = false;

	/** The LODs carry hair card data (FMantleSkinnedMeshLOD::HairCards). */
	bool bHairCards = false;

	/** LOD0's quantised UV hash (UMantleBodyTopologyData::ComputeUVHash). */
	uint32 UVHash = 0;

	/** Garments: body-shape SHA256. New head-hair: source face cooked BLAKE3, matched against its approved catalog dependency. */
	FString SourceShape;

	/** Garments: body-shape positions hash. Head-hair uses immutable face provenance and stores 0. */
	uint32 SourceShapePositionsHash = 0;

	/** LOD0's bounds as the cook worker wrote them, reference pose, cm. */
	FBox3f Bounds = FBox3f(ForceInit);

	/** Tags of the extension blocks this build does not know and skipped (version 1 defines none). */
	TArray<FString> SkippedExtensions;
};

struct FMantleSkinnedMeshReadResult
{
	bool bSuccess = false;

	/** Avatars and faces carry their reference pose; garments carry none (they are built against their wearer's). */
	FMantleSkinnedMesh Mesh;

	FMantleSkinnedMeshHeader Header;

	/** What went wrong, phrased for whoever has to act on it. */
	FString Error;
};

/**
 * Reads MSK1, the cooked skinned mesh (avatar bodies, avatar faces, skinned garments and hair), into
 * FMantleSkinnedMesh. Written by backend/internal/cook/skinnedmesh.go; layout in
 * AVATAR_CATALOG_PLAN.md section 3.4 as A2.2 settled it. Positions are Unreal centimetres in the
 * asset's own axes (Epic's glTF convention, not MRM1's rotated one); sections carry their material
 * name, the only thing to bind materials by.
 *
 * Up to three LODs (A3.8). MSK1 has carried its LOD count since version 1, so a file cooked with one LOD
 * before LODs were generated reads exactly as before; a lower LOD must draw only LOD0's materials.
 *
 * Strict, like FMantleRuntimeMeshReader: these bytes were uploaded by a stranger and arrive over the
 * network. Every count is checked against the bytes left and against a sanity cap before anything is
 * allocated, every float is checked finite, strings may not hold a NUL, unknown flags and trailing
 * bytes are refused, unknown extension blocks are skipped, and the result must pass
 * FMantleSkinnedMesh::Validate. A bad file returns bSuccess false with the reason; nothing asserts.
 * Touches no UObjects: safe on any thread, and on the dedicated server.
 */
class MANTLERUNTIME_API FMantleSkinnedMeshReader
{
public:
	/** The version this build understands. Must match SkinnedMeshVersion in the worker. */
	static constexpr uint32 SupportedVersion = 1;

	/** Sanity caps, far above limits.json's (the worker enforces those): they bound what a header can make this allocate. */
	static constexpr int32 MaxBones = 2048;
	static constexpr int32 MaxLODs = 3;
	static constexpr int32 MaxInfluences = 8;
	static constexpr int32 MaxVertices = 1'000'000;
	static constexpr int32 MaxIndices = 3'000'000;

	/** The skeleton field of every vehicle body (#435): its bone table is its own. */
	static constexpr const TCHAR* VehicleSkeleton = TEXT("vehicle");

	static FMantleSkinnedMeshReadResult Read(TConstArrayView<uint8> Data);
};
