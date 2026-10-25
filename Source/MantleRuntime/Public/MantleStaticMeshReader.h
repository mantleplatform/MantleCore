// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleSkinnedMesh.h"

/**
 * One LOD of an uploaded place mesh: MRM2's vertex streams, indices and section table. LOD0 is
 * FMantleStaticMeshData itself; its lower LODs (#470) are FMantleStaticMeshData::LowerLODs.
 */
struct FMantleStaticMeshLOD
{
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;

	/** xyz the tangent, w the binormal sign (exactly +1 or -1): Unreal's binormal is cross(N, T) * w. */
	TArray<FVector4f> Tangents;

	/** The first UV set; (0, 0) is the image's top left in both glTF and Unreal. */
	TArray<FVector2f> UVs;

	/** #470: the second UV set (UV1, glTF's TEXCOORD_1), one per vertex when the file carries it, else empty. */
	TArray<FVector2f> UV1s;

	/** One per vertex when the file carries vertex colours (Unreal's FColor bytes), else empty. */
	TArray<FColor> Colors;

	TArray<uint32> Indices;

	/**
	 * The section table in file (draw) order. The sections tile Indices. Bind materials by
	 * MaterialName or MaterialIndex (the place's StaticMesh.materials[] slot), never by position:
	 * the fixture puts slot 1 first to catch that. A lower LOD's sections are some of LOD0's slots,
	 * each under LOD0's name for it.
	 */
	TArray<FMantleSkinnedMeshSection> Sections;

	int32 VertexCount() const { return Positions.Num(); }
	int32 TriangleCount() const { return Indices.Num() / 3; }

	/** Bytes the arrays hold, for budgets and logs. */
	int64 GetLODAllocatedBytes() const
	{
		return Positions.GetAllocatedSize() + Normals.GetAllocatedSize() + Tangents.GetAllocatedSize() + UVs.GetAllocatedSize()
			+ UV1s.GetAllocatedSize() + Colors.GetAllocatedSize() + Indices.GetAllocatedSize();
	}
};

/** FKBoxElem as MRM2's COLL block stores it: Size is the full length along each local axis. */
struct FMantleStaticMeshBox
{
	FVector3f Center = FVector3f::ZeroVector;
	FQuat4f Rotation = FQuat4f::Identity;
	FVector3f Size = FVector3f::ZeroVector;
};

/** FKSphereElem. */
struct FMantleStaticMeshSphere
{
	FVector3f Center = FVector3f::ZeroVector;
	float Radius = 0.0f;
};

/** FKSphylElem: Length is the cylinder's between the caps, along the rotation's local Z. */
struct FMantleStaticMeshCapsule
{
	FVector3f Center = FVector3f::ZeroVector;
	FQuat4f Rotation = FQuat4f::Identity;
	float Radius = 0.0f;
	float Length = 0.0f;
};

/** FKConvexElem's points in the mesh's space. */
struct FMantleStaticMeshHull
{
	TArray<FVector3f> Vertices;
};

/**
 * #470: an uploaded mesh's simple collision (MRM2's COLL block), in the mesh's own space, Unreal
 * centimetres. Empty when the file has none: the mesh then collides with its triangles
 * (complex-as-simple), as every mesh did before #470.
 */
struct FMantleStaticMeshCollision
{
	/** The shapes answer complex queries too (CTF_UseSimpleAsComplex); else complex queries use the triangles (CTF_UseSimpleAndComplex). */
	bool bSimpleAsComplex = false;

	TArray<FMantleStaticMeshBox> Boxes;
	TArray<FMantleStaticMeshSphere> Spheres;
	TArray<FMantleStaticMeshCapsule> Capsules;
	TArray<FMantleStaticMeshHull> Hulls;

	int32 ShapeCount() const { return Boxes.Num() + Spheres.Num() + Capsules.Num() + Hulls.Num(); }
	bool HasShapes() const { return ShapeCount() > 0; }
};

/**
 * An uploaded place mesh as the cook worker writes it (MRM2, backend/internal/cook/staticmesh.go,
 * F1 #335 / F2 #336): the sections, tangent frame and optional vertex colour a baked PBR material
 * needs. Unreal centimetres in the asset's own axes (Epic's glTF convention: the cook already
 * swapped glTF's y and z), triangles in glTF's index order, which is already Unreal's front face.
 *
 * The inherited streams are LOD0. #470 (backend/internal/cook/staticfidelity.go) adds, each from an
 * optional extension block a client from before it skips: UV1 on any LOD (TEX1 for LOD0), the
 * authored lower LODs with their screen sizes (LODS), and simple collision (COLL).
 */
struct FMantleStaticMeshData : public FMantleStaticMeshLOD
{
	/** Exactly LOD0's positions' extent (the reader checks it). */
	FBox3f Bounds = FBox3f(ForceInit);

	/** LOD1 and LOD2 in order; empty for a one-LOD mesh. */
	TArray<FMantleStaticMeshLOD> LowerLODs;

	/** One per LOD, LOD0 first, each below the one before (FStaticMeshRenderData::ScreenSize); empty for one LOD. */
	TArray<float> ScreenSizes;

	FMantleStaticMeshCollision Collision;

	/** Tags of extension blocks this build does not know and skipped. */
	TArray<FString> SkippedExtensions;

	int32 LODCount() const { return 1 + LowerLODs.Num(); }

	/** LOD Index: this for 0, else LowerLODs[Index - 1]. */
	const FMantleStaticMeshLOD& GetLOD(int32 Index) const { return Index == 0 ? *this : LowerLODs[Index - 1]; }

	/** Material slots: one per LOD0 section, numbered 0..SlotCount-1 by MaterialIndex. */
	int32 SlotCount() const { return Sections.Num(); }

	/** Bytes every LOD's arrays and the collision hold, for budgets and logs. */
	int64 GetAllocatedBytes() const
	{
		int64 Bytes = GetLODAllocatedBytes();
		for (const FMantleStaticMeshLOD& LOD : LowerLODs)
		{
			Bytes += LOD.GetLODAllocatedBytes();
		}
		for (const FMantleStaticMeshHull& Hull : Collision.Hulls)
		{
			Bytes += Hull.Vertices.GetAllocatedSize();
		}
		return Bytes;
	}
};

/**
 * Reads MRM2 strictly. The bytes were uploaded by a stranger and arrive over the network, so every
 * count is checked against the format's caps and the bytes left before anything is allocated, every
 * float must be finite, a binormal sign must be exactly +1 or -1, the bounds must be exactly the
 * positions' extent, the sections must tile the index buffer in table order with non-empty unique
 * names and unique slots below the section count, unknown flags and trailing bytes are refused, and
 * unknown extension blocks are skipped. Mirrors StaticMesh.Validate in the Go writer, which pins the
 * shared fixtures schema/fixtures/runtime-mesh-v2.mrm and, for #470's blocks (TEX1, LODS, COLL, each
 * read with the same strictness and refused when it appears twice), runtime-mesh-v2-fidelity.mrm.
 *
 * No UObjects, no asserts, no logging: any thread, the dedicated server included.
 */
class MANTLERUNTIME_API FMantleStaticMeshReader
{
public:
	/** The version this build reads. Must match StaticMeshVersion in the worker. */
	static constexpr uint32 SupportedVersion = 1;

	/** The worker's caps (cook.MaxVertices, 3 x cook.MaxTriangles). */
	static constexpr uint32 MaxVertices = 1'500'000;
	static constexpr uint32 MaxIndices = 1'500'000;

	/** #470's structural caps (cook.MaxStaticLODs, MaxScreenSize, MaxCollisionPrimitives, MaxCollisionHulls, MaxHullVertices, MaxCollisionHullVertices, MaxCollisionExtent). */
	static constexpr int32 MaxLODs = 3;
	static constexpr float MaxScreenSize = 16.0f;
	static constexpr int32 MaxCollisionPrimitives = 256;
	static constexpr int32 MaxCollisionHulls = 256;
	static constexpr int32 MaxHullVertices = 1024;
	static constexpr int32 MaxCollisionHullVertices = 65536;
	static constexpr float MaxCollisionExtent = 1.0e7f;

	/** True when the bytes start with MRM2's magic (the loader's MRM1/MRM2 dispatch). */
	static bool HasMagic(TConstArrayView<uint8> Data);

	/** False, with the reason in OutError and Out left empty, on anything it refuses. */
	static bool Read(TConstArrayView<uint8> Data, FMantleStaticMeshData& Out, FString* OutError = nullptr);

	/**
	 * Whether a convex hull's points span a volume (cook.hullSpansVolume): the farthest point from the
	 * first, the farthest from that line and the farthest from that plane each lie more than 1e-3 cm
	 * away. The reader refuses a flat hull (the physics cook would fail on it); the exporter leaves one out.
	 */
	static bool HullSpansVolume(TConstArrayView<FVector3f> Points);
};
