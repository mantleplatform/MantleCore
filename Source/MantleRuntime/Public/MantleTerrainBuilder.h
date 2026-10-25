// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleHeightfieldReader.h"

class AActor;
class UMaterialInterface;
class UObject;
class UStaticMesh;
class UTexture;
class UWorld;
struct FMeshDescription;

/**
 * The parameter names M_MantleTerrain exposes (#466), which tools/materials/Create-TerrainMaterial.py
 * builds headless. The C++ side is written against these; per-layer names are "Layer<i>" plus a
 * suffix, i from 0 to FMantlePlaceLimits::MaxTerrainLayers - 1 (FMantleTerrainBuilder::LayerParameter).
 * Eight layers (#628): the generator script's Weightmap/LayerMask weigh layers 0 to 3 and Weightmap2/LayerMask2 layers 4 to 7.
 * Kept as TCHAR literals, not FName globals, so nothing constructs an FName during static initialisation.
 */
namespace MantleTerrainMaterialParameters
{
	/** Texture: the paint weights, R to A for layers 0 to 3, over the whole terrain (UV0). Default white. */
	constexpr const TCHAR* Weightmap = TEXT("Weightmap");

	/** Texture: the paint weights of layers 4 to 7 (#628), R to A, over the whole terrain (UV0). Default white. */
	constexpr const TCHAR* Weightmap2 = TEXT("Weightmap2");

	/** Vector: 1 in each channel whose layer exists, 0 otherwise; multiplies the weights before they are normalised. */
	constexpr const TCHAR* LayerMask = TEXT("LayerMask");

	/** Vector: LayerMask for layers 4 to 7 (#628); default all 0. */
	constexpr const TCHAR* LayerMask2 = TEXT("LayerMask2");

	/** Per-layer suffixes. Textures: BaseColor (sRGB), Normal, ORM (masks), the layer material's own. */
	constexpr const TCHAR* BaseColor = TEXT("BaseColor");
	constexpr const TCHAR* Normal = TEXT("Normal");
	constexpr const TCHAR* ORM = TEXT("ORM");

	/** Vectors: the layer material's base colour factor; how often the layer repeats across the terrain in U and V (R, G). */
	constexpr const TCHAR* BaseColorFactor = TEXT("BaseColorFactor");
	constexpr const TCHAR* UVScale = TEXT("UVScale");

	/** Scalars: the layer material's metallic and roughness factors. */
	constexpr const TCHAR* MetallicFactor = TEXT("MetallicFactor");
	constexpr const TCHAR* RoughnessFactor = TEXT("RoughnessFactor");
}

/** How the terrain's chunks are built. */
struct FMantleTerrainBuildParams
{
	/** Quads along each side of a chunk at LOD 0 (the last chunk on an axis may have fewer). */
	int32 ChunkQuads = 128;

	/** Most LODs per chunk; each halves the one before it, while it still has a quad. */
	int32 MaxLODs = 5;

	/** False on a dedicated server: LOD 0 only, which is all collision needs and nothing is drawn. */
	bool bRenderLODs = true;

	/** Complex-as-simple collision from LOD 0, cooked in the build (the place is not playable without it). */
	bool bCollision = true;

	/**
	 * False builds CPU-only chunk meshes for collision: no render resources, everything else as usual. A build
	 * also skips them in a cooked process that can never render (!FApp::CanEverRenderOrProduceRenderData), which
	 * is the dedicated server (FMantleRuntimeStaticMeshBuilder::ShouldInitRenderResources).
	 */
	bool bInitRenderResources = true;

	/** Outer of the chunk meshes; the transient package when null. */
	UObject* Outer = nullptr;
};

/** One built chunk: the samples it covers and its mesh, whose vertices are relative to Origin. */
struct FMantleTerrainChunk
{
	FIntPoint FirstSample = FIntPoint::ZeroValue;
	FIntPoint LastSample = FIntPoint::ZeroValue;

	/** Where the mesh's origin is in the terrain's space (its first sample at height 0), cm. */
	FVector3f Origin = FVector3f::ZeroVector;

	UStaticMesh* Mesh = nullptr;
};

/** One paint layer as the builder draws it: the place material it takes its look from, and its tiling. */
struct FMantleTerrainLayerLook
{
	UMaterialInterface* Material = nullptr;

	/** cm per repeat of the layer's textures. */
	double TilingCm = 400.0;
};

/**
 * A Terrain entity at runtime (#466). A shipped client cannot build a ULandscape, so the heightfield
 * becomes a grid of chunks, each a transient UStaticMesh built through the engine's runtime path
 * (UStaticMesh::BuildFromMeshDescriptions, fast build, as FMantleRuntimeStaticMeshBuilder does):
 *
 *   - LOD 0 has a vertex per sample of the chunk; each further LOD takes every second vertex of the
 *     one before (the chunk's last sample on each axis is always kept, so neighbours meet), and the
 *     engine picks them by screen size, which for chunks of one size is by distance.
 *   - Neighbouring chunks at different LODs meet at T-junctions, so every edge shared with another
 *     chunk has a skirt: a strip hanging below it, deep enough to cover the largest gap any LOD
 *     leaves on that edge, drawn from both sides. The terrain's outer border has none.
 *   - Normals and tangents come from the heightfield itself (central differences over the full
 *     grid), so lighting is continuous across chunks and LODs.
 *   - UV0 spans the whole terrain, 0 to 1 on each axis: the weightmap's space, and each layer's
 *     tiling is a multiple of it.
 *   - Collision is LOD 0's triangles, complex as simple and double-sided, cooked in the build.
 *
 * Game thread (creates UObjects). Under -NullRHI the meshes build with their CPU data only.
 */
class MANTLERUNTIME_API FMantleTerrainBuilder
{
public:
	/** Where the terrain master material's one instance lives (cooked into every client with /Game/Mantle/Materials). */
	static constexpr const TCHAR* MaterialPath = TEXT("/Game/Mantle/Materials/MI_MantleTerrain.MI_MantleTerrain");

	/** The chunk meshes' one material slot. */
	static constexpr const TCHAR* MaterialSlotName = TEXT("Terrain");

	/** Tag on the actor Spawn makes: the exporter skips it and Check names it (it is not a landscape). */
	static constexpr const TCHAR* TerrainActorTag = TEXT("MantleBuiltTerrain");

	/** Chunks along an axis of Samples samples: ceil((Samples - 1) / ChunkQuads), at least 1. */
	static int32 ChunkCount(int32 Samples, int32 ChunkQuads);

	/** The first and last sample of chunk Chunk along an axis. */
	static void ChunkSpan(int32 Samples, int32 ChunkQuads, int32 Chunk, int32& OutFirst, int32& OutLast);

	/** The samples a LOD with Step keeps from First to Last: First, First + Step, ..., and always Last. */
	static void AxisSamples(int32 First, int32 Last, int32 Step, TArray<int32>& Out);

	/** LODs a chunk of ChunkQuads quads gets: while a step still leaves a quad, at most MaxLODs. */
	static int32 LODCount(int32 ChunkQuads, int32 MaxLODs);

	/** The screen size below which LOD Lod gives way to the next: 1 for LOD 0, then 0.5 / 2^Lod. */
	static float LODScreenSize(int32 Lod);

	/** The surface normal at sample (X, Y), from central differences (one-sided at the border). */
	static FVector3f SampleNormal(const FMantleHeightfield& Field, int32 X, int32 Y);

	/**
	 * How far a skirt must hang below the edge from First to Last (a row when bAlongX, a column
	 * otherwise, at Fixed on the other axis): the largest distance between a sample's height and the
	 * straight line any LOD up to LODs draws through it, plus 10 cm. 0 for an edge with nothing between LOD vertices.
	 */
	static float EdgeSkirtDepth(const FMantleHeightfield& Field, bool bAlongX, int32 Fixed, int32 First, int32 Last, int32 LODs);

	/**
	 * One chunk at one LOD as a mesh description, vertices relative to the chunk's origin. Skirts
	 * (SkirtDepth above 0) on each edge whose bSkirts entry is set: left (X = First.X), right, bottom
	 * (Y = First.Y), top. Front faces point up: a quad's triangles are (x, y), (x, y + 1), (x + 1, y)
	 * and (x + 1, y), (x, y + 1), (x + 1, y + 1), whose cross(c - a, b - a) is +Z.
	 */
	static void BuildChunkDescription(const FMantleHeightfield& Field, const FIntPoint& First, const FIntPoint& Last,
		int32 Step, float SkirtDepth, const bool (&bSkirts)[4], FMeshDescription& Out);

	/** Every chunk's mesh. False, with the reason (and nothing in Out), when the heightfield is unusable or a mesh fails. */
	static bool BuildChunks(const FMantleHeightfield& Field, const FMantleTerrainBuildParams& Params,
		TArray<FMantleTerrainChunk>& Out, FString* OutError = nullptr);

	/** "Layer<Layer><Suffix>", one of M_MantleTerrain's per-layer parameters. */
	static FName LayerParameter(int32 Layer, const TCHAR* Suffix);

	/**
	 * The terrain's material: a dynamic instance of MI_MantleTerrain with each layer's textures and
	 * factors copied from its place material (an uploaded material is an M_MantlePlace instance, whose
	 * parameters it reads), its tiling across the terrain's extent, and the weightmaps (Weightmap2 holds
	 * layers 4 to 7 and may be null for a terrain with up to four, #628). A single layer
	 * whose material is not an M_MantlePlace instance (a built-in library material) is used as it is,
	 * and so is layer 0's material when MI_MantleTerrain is missing (the content handoff not done yet),
	 * with OutWarning set. Null for no layers. Game thread.
	 */
	static UMaterialInterface* BuildMaterial(const FMantleHeightfield& Field, TConstArrayView<FMantleTerrainLayerLook> Layers,
		UTexture* Weightmap, UTexture* Weightmap2, UObject* Outer, FString* OutWarning = nullptr);

	/**
	 * An actor named Name holding the terrain: a root scene component and one static mesh component per
	 * chunk, placed at the chunk's origin, blocking everything, casting shadows when bCastShadow. Material
	 * may be null (a dedicated server draws nothing). Null, with the reason, when the chunks fail. Game thread.
	 */
	static AActor* Spawn(UWorld* World, const FString& Name, const FMantleHeightfield& Field, UMaterialInterface* Material,
		bool bCastShadow, const FMantleTerrainBuildParams& Params, FString* OutError = nullptr);
};
