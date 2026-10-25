// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleBodyOffsetsBuilder.h"
#include "MantleStaticMeshReader.h"

class FJsonObject;

/** One joint of a skinned GLB: a bone, its parent (an earlier joint, or INDEX_NONE) and its local rest transform in Unreal space. */
struct MANTLEEDITOR_API FMantleGlbJoint
{
	FName Name;
	int32 Parent = INDEX_NONE;
	FTransform Local = FTransform::Identity;
};

/** One primitive: a contiguous run of vertices drawn with one material. Indices are relative to FirstVertex. */
struct MANTLEEDITOR_API FMantleGlbPrimitive
{
	int32 MaterialIndex = 0;
	int32 FirstVertex = 0;
	int32 NumVertices = 0;
	TArray<uint32> Indices;
};

/**
 * A mesh as the plugin uploads it, in Unreal space (centimetres, Z up, the asset's own axes),
 * written to GLB by FMantleGlbWriter in Epic's glTF convention.
 */
struct MANTLEEDITOR_API FMantleGlbMesh
{
	FString Name;

	/** Optional immutable authoring dependency, sha256:<64 lowercase hex>. Written once
	 * in asset.extras.mantleAuthoringSource so identical geometry authored on another
	 * source has distinct upload bytes. The backend's source option remains authoritative. */
	FString AuthoringSourceHash;

	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;

	/** Optional: empty, or one per vertex (xyz tangent, w the binormal sign). */
	TArray<FVector4f> Tangents;

	TArray<FVector2f> UVs;

	/** #470, optional: empty, or one per vertex: the second UV set (a static mesh's UV channel 1), written as TEXCOORD_1. */
	TArray<FVector2f> UV1s;

	/** #470, optional: empty, or one per vertex: the vertex colour, written as normalized unsigned-byte COLOR_0 (R, G, B, A). */
	TArray<FColor> Colors;

	/** One glTF material per entry, named: sections are identified by material name, which must be unique. */
	TArray<FString> MaterialNames;

	/** Vertex ranges in order; together they are every vertex, so the file's vertex order is Positions' order. */
	TArray<FMantleGlbPrimitive> Primitives;

	/** Skin, parent-first. Empty for a rigid mesh. */
	TArray<FMantleGlbJoint> Joints;

	/** Influence slots per vertex (1 to 8); InfluenceJoints / InfluenceWeights hold NumInfluences per vertex. */
	int32 NumInfluences = 0;
	TArray<uint16> InfluenceJoints;
	TArray<float> InfluenceWeights;

	/** Hair cards (optional, both or neither): the card's root UV and the position along it, root 0 to tip 1. */
	TArray<FVector2f> HairRootUV;
	TArray<float> HairCoordU;

	/** A body shape's RBF warp weights (optional): the root extension MANTLE_body_shape. */
	TArray<uint32> WarpSampleIndices;
	TArray<float> WarpInterpolationWeights;

	/**
	 * #849, optional: the body-shape offsets (weight+, ...), written as the root extension MANTLE_body_offsets from LOD0's
	 * mesh. Each shape needs a delta array per LOD written (matching that LOD's vertex count) and one joint delta per
	 * skin joint, in Unreal axes and centimetres like Positions; the writer converts them (without a translation).
	 */
	TArray<FMantleBodyOffsetShape> BodyOffsets;

	/**
	 * #430, optional, beside the warp weights: the neck measured on the face the body was authored with
	 * (FMantleBodyShapeNeck), as MANTLE_body_shape's "neck". Method "correspondence" or "rays"; one id per sample;
	 * positions in Unreal space like Positions (the writer converts them); the RBF weights over Positions followed
	 * by NeckPositions.
	 */
	FString NeckMethod;
	TArray<uint32> NeckIds;
	TArray<FVector3f> NeckPositions;
	TArray<uint32> NeckSampleIndices;
	TArray<float> NeckInterpolationWeights;

	/**
	 * #435, optional: the glTF root's extras (a vehicle's document at extras.mantleVehicle). LOD0's is written;
	 * lower LODs' are ignored.
	 */
	TSharedPtr<FJsonObject> Extras;

	bool IsSkinned() const { return Joints.Num() > 0; }

	bool HasNeck() const { return !NeckMethod.IsEmpty() || NeckPositions.Num() > 0 || NeckSampleIndices.Num() > 0; }
};

/**
 * #470: what a static mesh upload carries beside its geometry, written as the root extension
 * MANTLE_static_mesh (backend cook.StaticMeshExtension).
 */
struct MANTLEEDITOR_API FMantleGlbStaticExtras
{
	/** One per LOD, LOD0 first, each below the one before, when the upload has several LODs; else empty. */
	TArray<float> ScreenSizes;

	/**
	 * The mesh's simple collision in Unreal's frame and centimetres, in the mesh's own space: written as it
	 * is, not converted to glTF's axes (it is the plugin's reading of FKAggregateGeom, which the cook stores
	 * in the same frame). None when it has no shapes.
	 */
	FMantleStaticMeshCollision Collision;

	bool IsEmpty() const { return ScreenSizes.Num() == 0 && !Collision.HasShapes(); }
};

/**
 * Writes the plugin's uploads as binary glTF (A2.4 #283), for the cook worker's readers
 * (backend/internal/cook/gltf.go and gltfskin.go).
 *
 * Coordinates are Epic's own glTF convention, the one GLTFExporter writes and Interchange reads
 * and the one the worker undoes for skinned content (A2.2 decision): Unreal (x, y, z) in
 * centimetres is glTF (x, z, y) in metres; directions swap y and z the same way; a rotation
 * (qx, qy, qz, qw) is (-qx, -qz, -qy, qw); a scale swaps y and z; a tangent's w is kept. Triangle
 * indices keep their order: the axis swap is itself the change of handedness.
 *
 * Why its own writer rather than UGLTFSkeletalMeshExporter for geometry: the exporter re-indexes
 * each section's vertices in the order the index buffer first uses them (FGLTFMeshSection), and
 * the reference-topology check (a quantised UV hash over the vertices in order) needs the render
 * vertices exactly in their own order; the plugin also has to add the hair card attributes, the
 * body-shape extension and meshes built from groom cards, which the exporter cannot. GLTFExporter
 * is still what the plugin bakes materials through (FMantleCosmeticExport).
 *
 * Every vertex attribute is one buffer view over the whole mesh; each primitive's accessors start
 * at its FirstVertex, so the file stores each vertex once. Pure: no UObjects.
 */
class MANTLEEDITOR_API FMantleGlbWriter
{
public:
	/** The GLB of Mesh. False with the first problem in OutError when the arrays do not agree. */
	static bool Write(const FMantleGlbMesh& Mesh, TArray<uint8>& OutBytes, FString& OutError);

	/**
	 * A3.8: one GLB carrying every LOD, LOD0 first, the cook's convention (limits.json lods.glTF): one glTF
	 * mesh per LOD, each placed by its own node named LOD0, LOD1, ... that names the one skin. LOD0 carries
	 * the joints, the materials and any body-shape extension; every lower LOD must have the same joints
	 * (names and parents), draw only materials LOD0 has (matched by name) and carry the same optional
	 * streams (tangents, hair cards). One LOD writes exactly what Write writes.
	 */
	static bool WriteLODs(TConstArrayView<FMantleGlbMesh> LODs, TArray<uint8>& OutBytes, FString& OutError);

	/**
	 * A converted avatar's body or face: geometry, rig and section-name metadata only. This is
	 * deliberately separate from Write/WriteLODs so the As-Is route retains authored vertex
	 * colours, while converted publication can never emit COLOR_* appearance attributes.
	 */
	static bool WriteGeometryOnly(const FMantleGlbMesh& Mesh, TArray<uint8>& OutBytes, FString& OutError);
	static bool WriteGeometryOnlyLODs(TConstArrayView<FMantleGlbMesh> LODs, TArray<uint8>& OutBytes, FString& OutError);

	/**
	 * #470: a static mesh upload: its LODs as WriteLODs writes them (no skin), and Extras as the root
	 * extension MANTLE_static_mesh when there is anything in it: screen sizes (one per LOD, strictly
	 * decreasing, in (0, 16]) and simple collision ({center, rotation, size} boxes, {center, radius}
	 * spheres, {center, rotation, radius, length} capsules, convex hulls as float VEC3 accessors). Empty
	 * extras write exactly what WriteLODs writes.
	 */
	static bool WriteStatic(TConstArrayView<FMantleGlbMesh> LODs, const FMantleGlbStaticExtras& Extras, TArray<uint8>& OutBytes, FString& OutError);

	/** The extension that carries a static mesh's screen sizes and simple collision (backend cook.StaticMeshExtension). */
	static const TCHAR* StaticMeshExtension;

	/** The most LODs an upload carries (MSK1's and limits.json lods.maxLods). */
	static constexpr int32 MaxLODs = 3;

	/** The LOD node names' prefix (backend cook.LODNodePrefix): LOD0, LOD1, LOD2. */
	static const TCHAR* LODNodePrefix;

	/** Unreal centimetres to glTF metres, Epic's axes. */
	static FVector3d ToGltfPosition(const FVector3d& Unreal);

	/** A direction (normal, tangent xyz): y and z swap. */
	static FVector3d ToGltfDirection(const FVector3d& Unreal);

	/** A rotation: (qx, qy, qz, qw) becomes (-qx, -qz, -qy, qw), normalised. */
	static FQuat ToGltfRotation(const FQuat& Unreal);

	/** A scale: y and z swap. */
	static FVector3d ToGltfScale(const FVector3d& Unreal);

	/** The extension that carries a body shape's warp weights (backend cook.BodyShapeExtension). */
	static const TCHAR* BodyShapeExtension;

	/** The extension that carries the body-shape offsets (backend cook.BodyOffsetsExtension). */
	static const TCHAR* BodyOffsetsExtension;

	/** The hair card attribute names (backend cook.HairRootUVAttribute / HairCoordUAttribute). */
	static const TCHAR* HairRootUVAttribute;
	static const TCHAR* HairCoordUAttribute;
};

/** A parsed GLB: its JSON and binary chunk. For tests, and for reading images out of a GLTFExporter bake. */
struct MANTLEEDITOR_API FMantleGlbDocument
{
	TSharedPtr<FJsonObject> Json;
	TArray<uint8> Bin;

	/** Reads the header, the JSON chunk and the optional BIN chunk. */
	static bool Parse(TConstArrayView<uint8> Bytes, FMantleGlbDocument& Out, FString& OutError);

	/** Every component of accessor Index as doubles (float, unsigned byte/short/int; normalised integers scaled). */
	bool ReadAccessor(int32 Index, TArray<double>& OutValues, int32* OutComponents = nullptr, int32* OutCount = nullptr) const;

	/** The bytes of buffer view Index (an embedded image, say). */
	bool ReadBufferView(int32 Index, TArray<uint8>& OutBytes) const;
};
