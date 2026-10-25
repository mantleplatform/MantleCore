// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleGlbWriter.h"

class USkeletalMesh;

/** One render section of the reference face: a run of vertices and the triangles that use them, with the material it draws. */
struct MANTLEEDITOR_API FMantleFaceLayoutSection
{
	FString Material;
	int32 FirstVertex = 0;
	int32 NumVertices = 0;
	int32 FirstIndex = 0;
	int32 NumTriangles = 0;
};

/**
 * The layout of a built MetaHuman face (Epic's UE Optimized / Medium LOD0, 6872 triangles): each vertex's quantised UV, the triangles and the render
 * sections, in the built order, and no geometry. The client fits a collar by pairing the face a garment was authored with to the wearer's face by
 * TRIANGLE INDEX, so a face has to be numbered like the built ones. A face made in memory by MetaHuman Creator is its preview LOD3, the same surface
 * numbered differently; Reorder puts it into this layout (a UV match inside each section, tolerant of flipped quad diagonals as the body's is).
 * Recorded once from a converted character by Mantle.Dev.RecordFaceLayout and shipped as a plugin-only resource (Resources/FaceLayout).
 */
struct MANTLEEDITOR_API FMantleFaceLayout
{
	static constexpr int32 ReferenceTriangles = 6872;
	static constexpr int32 Quantization = 2048;

	FString Source;
	FString Generated;
	TArray<FIntPoint> QuantizedUVs;
	TArray<uint32> Indices;
	TArray<FMantleFaceLayoutSection> Sections;

	/** Resources/FaceLayout/face-layout-v1.json in the plugin. */
	static FString DefaultPath();
	/** The shipped layout, read once; null when the file is missing or invalid. */
	static const FMantleFaceLayout* Get();

	/** The first LOD of Face with the reference face triangle count, INDEX_NONE when none has. */
	static int32 FindFaceLOD(const USkeletalMesh* Face);

	/**
	 * A face LOD as a GLB mesh in its render order: positions, UVs and one primitive per render section (the sections must tile the vertices in
	 * order). Enough for the layout to be recorded from, or matched; the upload reads the full mesh itself.
	 */
	static bool ReadRender(const USkeletalMesh* Face, int32 LOD, FMantleGlbMesh& Out, FString& OutError);

	/** The layout of a built face's LOD (FindFaceLOD). */
	static bool Record(const USkeletalMesh* Face, FMantleFaceLayout& Out, FString& OutError);

	/**
	 * Puts Mesh (one primitive per section, as ReadRender or the upload's reader makes) into this layout's order, in place: every per-vertex
	 * array carried along, the primitives rebuilt in the layout's sections with the materials of the sections they were matched to. Sections are
	 * paired by vertex and triangle count (the material name breaks a tie); inside a section vertices are matched by UV and neighbours, and
	 * triangles must be the layout's but for flipped quad diagonals. On failure Mesh is unchanged and OutError says exactly what differs.
	 * OutNote says how many sections matched exactly and how many diagonals were flipped.
	 */
	bool Reorder(FMantleGlbMesh& Mesh, FString& OutNote, FString& OutError) const;

	/**
	 * True when Mesh (one primitive per section, in this layout's order) already numbers its triangles like this layout: the same triangle count and
	 * every triangle's UV centroid where the layout's triangle of that index has it. A face MetaHuman Creator made from the DNA archetype (as the
	 * build does) is; nothing then needs reordering. OutWhy says what differs when it is not. With NeckBaseZ (the body's neck base) only the triangles of
	 * the collar region the client samples (6 cm under the base to 12 cm over it, within 16 cm of the axis, by the mesh's own triangle centroids) have to
	 * match; the others are counted into OutNote. The UV centroid is the same however the corners are rotated.
	 */
	bool IsNumberedLikeThis(const FMantleGlbMesh& Mesh, FString& OutWhy, const float* NeckBaseZ = nullptr, FString* OutNote = nullptr) const;

	static bool Parse(const FString& Json, FMantleFaceLayout& Out, FString& OutError);
	FString ToJson() const;
};
