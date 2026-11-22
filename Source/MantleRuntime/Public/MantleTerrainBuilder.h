// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleHeightfieldReader.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "UObject/StrongObjectPtr.h"

class AActor;
class UMaterialInterface;
class UObject;
class UStaticMesh;
class UStaticMeshComponent;
class UMantleTerrainCollisionComponent;
class USceneComponent;
class UTexture;
class UWorld;
struct FMantleTerrainHoleGrid;

/**
 * The parameter names M_MantleTerrain exposes.
 * The C++ side is written against these; per-layer names are "Layer<i>" plus a
 * suffix, i from 0 to FMantlePlaceLimits::MaxTerrainLayers - 1 (FMantleTerrainBuilder::LayerParameter).
 * Eight layers: the master's Weightmap/LayerMask weigh layers 0 to 3 and Weightmap2/LayerMask2 layers 4 to 7.
 * Kept as TCHAR literals, not FName globals, so nothing constructs an FName during static initialization.
 */
namespace MantleTerrainMaterialParameters
{
	/** Texture: the paint weights, R to A for layers 0 to 3, over the whole terrain (UV0). Default white. */
	constexpr const TCHAR* Weightmap = TEXT("Weightmap");

	/** Texture: the paint weights of layers 4 to 7, R to A, over the whole terrain (UV0). Default white. */
	constexpr const TCHAR* Weightmap2 = TEXT("Weightmap2");

	/** Vector: 1 in each channel whose layer exists, 0 otherwise; multiplies the weights before they are normalized. */
	constexpr const TCHAR* LayerMask = TEXT("LayerMask");

	/** Vector: LayerMask for layers 4 to 7; default all 0. */
	constexpr const TCHAR* LayerMask2 = TEXT("LayerMask2");

	/** Per-layer suffixes. Textures: BaseColor (sRGB), Normal, ORM (masks), the layer material's own. */
	constexpr const TCHAR* BaseColor = TEXT("BaseColor");
	constexpr const TCHAR* Normal = TEXT("Normal");
	constexpr const TCHAR* ORM = TEXT("ORM");

	/** Vectors: the layer material's base color factor; how often the layer repeats across the terrain in U and V (R, G). */
	constexpr const TCHAR* BaseColorFactor = TEXT("BaseColorFactor");
	constexpr const TCHAR* UVScale = TEXT("UVScale");

	/** Scalars: the layer material's metallic and roughness factors. */
	constexpr const TCHAR* MetallicFactor = TEXT("MetallicFactor");
	constexpr const TCHAR* RoughnessFactor = TEXT("RoughnessFactor");
}

/**
 * The terrains that touch one terrain along an edge: the other tiles of a landscape that was cut into tiles, or Terrains placed edge to edge by hand.
 * A neighbor shares its border samples with this terrain (its last column is this one's first, and so on), so a tile's heights can see one sample past each of its own borders.
 * Sides: 0 left (-X, this terrain's sample column 0), 1 right (+X), 2 bottom (-Y), 3 top (+Y). A null entry is no neighbor.
 */
struct FMantleTerrainEdgeNeighbors
{
	TSharedPtr<const FMantleHeightfield> Field[4];

	bool Has(const int32 Side) const { return Field[Side].IsValid(); }

	bool HasAny() const { return Has(0) || Has(1) || Has(2) || Has(3); }
};

/** How the terrain's chunks are built. */
struct FMantleTerrainBuildParams
{
	/** Quads along each side of a chunk at LOD 0 (the last chunk on an axis may have fewer). */
	int32 ChunkQuads = 128;

	/** Most LODs per chunk; each halves the one before it, while it still has a quad. */
	int32 MaxLODs = 5;

	/** False on a dedicated server: LOD 0 only (nothing is drawn; a server builds no chunk at all, see bChunkMeshes). */
	bool bRenderLODs = true;

	/**
	 * The ground's collision: one height field per block of ChunkQuads squares (UMantleTerrainCollisionComponent), on every machine, from the heights the machine already holds. The chunk
	 * meshes carry no collision at all, whether they are built or not.
	 */
	bool bCollision = true;

	/** False on a dedicated server: no chunk mesh is built, only the collision blocks (a server draws nothing). */
	bool bChunkMeshes = true;

	/**
	 * The terrain's heights, shared: the collision blocks keep them (the navigation system reads them from other threads) and read each block's samples from them. When null the
	 * spawn makes a copy of the heightfield it is given.
	 */
	TSharedPtr<const FMantleHeightfield> FieldOwner;

	/**
	 * False builds CPU-only chunk meshes: no render resources, everything else as usual (a test run that draws nothing). A build
	 * also skips them in a cooked process that can never render (!FApp::CanEverRenderOrProduceRenderData), which
	 * is the dedicated server (FMantleRuntimeStaticMeshBuilder::ShouldInitRenderResources), though a server builds no chunk at all (bChunkMeshes).
	 */
	bool bInitRenderResources = true;

	/**
	 * FMantleTerrainSpawn builds only the far ground: each chunk is its lowest detail level alone (the one a chunk is drawn at furthest away, the same vertices and the same skirts),
	 * with no shadow, as a component of its own. A player's machine that streams the place (FMantleTerrainStream) draws this everywhere and swaps a chunk for its
	 * full version (every LOD, shadows) while the player is near. The ground's collision is the height-field blocks, whatever a chunk draws. Ignored by BuildChunks and BuildOneChunk.
	 */
	bool bFarOnly = false;

	/**
	 * The quads that are holes (the terrain's hole mask): no triangles in any LOD, so no collision and no skirt
	 * along them; a chunk that is all hole gets no mesh. A coarser LOD's quad is a hole when any quad it covers is.
	 */
	TSharedPtr<const FMantleTerrainHoleGrid> Holes;

	/**
	 * The terrains that touch this one (FMantleTerrainAdjacency::Find), or null for none: a border shared with a neighbor gets a skirt like a border between two chunks of one terrain
	 * (so a coarse and a fine chunk across the seam leave no crack), and its normals read the neighbor's heights (so there is no shading line along the seam).
	 */
	TSharedPtr<const FMantleTerrainEdgeNeighbors> Neighbors;

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

/** How much of a chunk is built: all of it (every LOD, collision as asked), or only the lowest level of detail (the far ground). */
enum class EMantleTerrainChunkDetail : uint8
{
	Full,
	Far,
};

/**
 * One chunk's mesh descriptions, made by FMantleTerrainBuilder::PrepareChunk (any thread, nothing of the engine's object world touched) and turned into a mesh by
 * FMantleTerrainBuilder::FinishChunk (game thread). Descriptions holds the LODs the chunk has, finest first (one for the far ground).
 */
struct FMantleTerrainPreparedChunk
{
	EMantleTerrainChunkDetail Detail = EMantleTerrainChunkDetail::Full;
	int32 ChunkX = 0;
	int32 ChunkY = 0;
	FIntPoint FirstSample = FIntPoint::ZeroValue;
	FIntPoint LastSample = FIntPoint::ZeroValue;
	FVector3f Origin = FVector3f::ZeroVector;
	TArray<FMeshDescription> Descriptions;

	/** The vertices of the finest description (LOD 0 for a full chunk), for the logs. */
	int64 Vertices = 0;

	/** No triangles at all in the level asked for (a chunk lost to holes): nothing to build. */
	bool bEmpty = false;

	/** #189: the chunk slot's UV density on channels 0 to 3 (FMantleRuntimeStaticMeshBuilder::ComputeSlotUVDensities over Descriptions). */
	FVector4f UVDensity = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
};

/** One paint layer as the builder draws it: the place material it takes its look from, and its tiling. */
struct FMantleTerrainLayerLook
{
	UMaterialInterface* Material = nullptr;

	/** cm per repeat of the layer's textures. */
	double TilingCm = 400.0;
};

/**
 * A Terrain entity at runtime. A shipped client cannot build a ULandscape, so the heightfield
 * becomes a grid of chunks, each a transient UStaticMesh built through the engine's runtime path
 * (UStaticMesh::BuildFromMeshDescriptions, fast build, as FMantleRuntimeStaticMeshBuilder does):
 *
 *   - LOD 0 has a vertex per sample of the chunk; each further LOD takes every second vertex of the
 *     one before (the chunk's last sample on each axis is always kept, so neighbors meet), and the
 *     engine picks them by screen size, which for chunks of one size is by distance.
 *   - Neighboring chunks at different LODs meet at T-junctions, so every edge shared with another
 *     chunk has a skirt: a strip hanging below it, deep enough to cover the largest gap any LOD
 *     leaves on that edge, drawn from both sides. The terrain's outer border has none.
 *   - Normals and tangents come from the heightfield itself (central differences over the full
 *     grid), so lighting is continuous across chunks and LODs.
 *   - UV0 spans the whole terrain, 0 to 1 on each axis: the weightmap's space, and each layer's
 *     tiling is a multiple of it.
 *   - A chunk is a picture only. The collision is a Chaos height field per block of 128 squares (FMantleTerrainCollision), made from the same samples, split the same way.
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

	/**
	 * The surface normal at sample (X, Y), from central differences (one-sided at a border with no neighbor). Edges, when given, supplies the sample just past a border that has a
	 * neighbor: the neighbor's sample one in from its own matching border, so the two tiles of a seam compute the same normal for the shared sample.
	 */
	static FVector3f SampleNormal(const FMantleHeightfield& Field, int32 X, int32 Y, const FMantleTerrainEdgeNeighbors* Edges = nullptr);

	/**
	 * How far a skirt must hang below the edge from First to Last (a row when bAlongX, a column
	 * otherwise, at Fixed on the other axis): the largest distance between a sample's height and the
	 * straight line any LOD up to LODs draws through it, plus 10 cm. 0 for an edge with nothing between LOD vertices.
	 */
	static float EdgeSkirtDepth(const FMantleHeightfield& Field, bool bAlongX, int32 Fixed, int32 First, int32 Last, int32 LODs);

	/**
	 * One chunk at one LOD as a mesh description, vertices relative to the chunk's origin. Skirts
	 * (SkirtDepth above 0) on each edge whose bSkirts entry is set: left (X = First.X), right, bottom
	 * (Y = First.Y), top. Front faces point up: a quad is split from (x, y) to (x + 1, y + 1), as the height field is, into the triangles (x, y), (x + 1, y + 1), (x + 1, y)
	 * and (x, y), (x, y + 1), (x + 1, y + 1), whose cross(c - a, b - a) is +Z.
	 */
	static void BuildChunkDescription(const FMantleHeightfield& Field, const FIntPoint& First, const FIntPoint& Last,
		int32 Step, float SkirtDepth, const bool (&bSkirts)[4], FMeshDescription& Out, const FMantleTerrainHoleGrid* Holes = nullptr, const FMantleTerrainEdgeNeighbors* Edges = nullptr);

	/** Every chunk's mesh. False, with the reason (and nothing in Out), when the heightfield is unusable or a mesh fails. */
	static bool BuildChunks(const FMantleHeightfield& Field, const FMantleTerrainBuildParams& Params,
		TArray<FMantleTerrainChunk>& Out, FString* OutError = nullptr);

	/** Whether the heightfield can be built into a terrain at all: its size, its sample count, its spacing and its scale. */
	static bool IsUsableHeightfield(const FMantleHeightfield& Field);

	/**
	 * One chunk's mesh, collision cooked: the unit BuildChunks and a staged Spawn (FMantleTerrainSpawn) are made of.
	 * Chunk (ChunkX, ChunkY) of the heightfield's grid goes to OutChunk, and its LOD 0 vertices are added to InOutVertices.
	 * A chunk that is all hole has no mesh: true with bOutEmpty set. False, with the reason, when the mesh fails. Game thread.
	 */
	static bool BuildOneChunk(const FMantleHeightfield& Field, const FMantleTerrainBuildParams& Params, int32 ChunkX, int32 ChunkY,
		int64& InOutVertices, FMantleTerrainChunk& OutChunk, bool& bOutEmpty, FString* OutError = nullptr);

	/**
	 * The first half of BuildOneChunk, on any thread: the chunk's skirt depth and the descriptions of its LODs (Detail Full), or of its lowest level of detail alone (Far:
	 * the coarsest LOD that still has a quad, with the skirt depth a full chunk's edges have, so a far chunk meets a full one, or a far one, with no gap). Touches no
	 * engine object. Field and the hole grid must outlive the call; Out is replaced.
	 */
	static void PrepareChunk(const FMantleHeightfield& Field, const FMantleTerrainBuildParams& Params, int32 ChunkX, int32 ChunkY, EMantleTerrainChunkDetail Detail,
		FMantleTerrainPreparedChunk& Out);

	/**
	 * The second half: the mesh of a prepared chunk, on the game thread. The mesh is a picture only (no body, no CPU copy of its triangles). A chunk that is all hole has no mesh: true
	 * with bOutEmpty set. False, with the reason, when the mesh fails.
	 */
	static bool FinishChunk(const FMantleTerrainBuildParams& Params, FMantleTerrainPreparedChunk&& Prepared, int64& InOutVertices,
		FMantleTerrainChunk& OutChunk, bool& bOutEmpty, FString* OutError = nullptr);

	/**
	 * One chunk's mesh as a component of Owner under Root: Static or Movable as the terrain is, with no collision (the ground's collision is AddCollisionBlock's), casting a shadow when
	 * bCastShadow (a far chunk never does: it has no overlaps and no part in distance-field lighting either). Registered unless bRegister is false. Game thread.
	 */
	static UStaticMeshComponent* AddChunkComponent(AActor* Owner, USceneComponent* Root, const FMantleTerrainChunk& Chunk, UMaterialInterface* Material, bool bStatic,
		bool bCastShadow, bool bFar, bool bRegister = true);

	/**
	 * The collision of chunk (ChunkX, ChunkY)'s squares: a height-field component under Root, Static or Movable as the terrain is, made from Field's samples and the hole grid. Null for a
	 * chunk with nothing to stand on (all hole) or a heightfield the shape cannot be made from. Game thread.
	 */
	static UMantleTerrainCollisionComponent* AddCollisionBlock(AActor* Owner, USceneComponent* Root, const TSharedPtr<const FMantleHeightfield>& Field,
		const TSharedPtr<const FMantleTerrainHoleGrid>& Holes, int32 ChunkQuads, int32 ChunkX, int32 ChunkY, bool bStatic);

	/** "Layer<Layer><Suffix>", one of M_MantleTerrain's per-layer parameters. */
	static FName LayerParameter(int32 Layer, const TCHAR* Suffix);

	/**
	 * The terrain's material: a dynamic instance of MI_MantleTerrain with each layer's textures and
	 * factors copied from its place material (an uploaded material is an M_MantlePlace instance, whose
	 * parameters it reads), its tiling across the terrain's extent, and the weightmaps (Weightmap2 holds
	 * layers 4 to 7 and may be null for a terrain with up to four). A single layer
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

/** One Terrain as the adjacency search sees it: where it stands in the world and its heights. */
struct FMantleTerrainPlacement
{
	FTransform World = FTransform::Identity;
	TSharedPtr<const FMantleHeightfield> Field;
};

/**
 * Which Terrains touch which (stage 4, terrain tiles). Two Terrains touch when one's right (+X) border row of samples lies on the other's left border in world space, sample for
 * sample, to within ToleranceCm in all three axes (or the same with the top and bottom borders): the same number of samples along the shared border, the same turn and scale, no step in
 * height between the two origins, the same spacing across the border (so a height of one is a height of the other and the two slopes mean the same). That is exactly how an exporter cuts a landscape into tiles that share their border samples, and it needs no key in the place file; Terrains placed edge to
 * edge by hand are found the same way. A diagonal neighbor does not count (a normal reads only along an axis). Pure data, any thread.
 */
class MANTLERUNTIME_API FMantleTerrainAdjacency
{
public:
	/** How far, in cm, two border samples may lie apart and still be the same point (the place file's own precision is 0.01 cm). */
	static constexpr double ToleranceCm = 1.0;

	/**
	 * Out gets one entry per Terrain, in order: its neighbors (null Field for none). A Terrain with no Field, or one the builder cannot use
	 * (FMantleTerrainBuilder::IsUsableHeightfield), touches nothing.
	 */
	static void Find(TConstArrayView<FMantleTerrainPlacement> Terrains, TArray<FMantleTerrainEdgeNeighbors>& Out);

	/** Whether B's left border lies on A's right border (bAlongX true) or B's bottom border on A's top border (false). */
	static bool BorderMeets(const FMantleTerrainPlacement& A, const FMantleTerrainPlacement& B, bool bAlongX);
};

/**
 * FMantleTerrainBuilder::Spawn in steps: Begin makes the actor and its root, each Step builds one chunk (the unit that
 * costs, tens of milliseconds for 128 quads with five LODs and cooked collision) and puts it on the actor, so a big
 * terrain is many short frames instead of one of seconds. Spawn is Begin and Steps until done.
 *
 * Until it is finished the actor holds only the chunks so far. Dropping the object before then destroys the actor.
 * The heightfield and the material must outlive it. Game thread.
 */
class MANTLERUNTIME_API FMantleTerrainSpawn
{
public:
	/**
	 * Null, with the reason in OutError, when the world or the heightfield cannot take a terrain. StaticWorld, when given, is where the terrain stands and ends
	 * (Static): its root is made there, Static, before it registers, and so is every chunk, so each chunk's collision is made once, static, in place. Without
	 * it the terrain is Movable: at MovableWorld when given (where it ends: the place builder gives it so no body is parked at the origin), or at the origin for a
	 * caller that places it itself.
	 */
	static TSharedPtr<FMantleTerrainSpawn> Begin(UWorld* World, const FString& Name, const FMantleHeightfield& Field, UMaterialInterface* Material,
		bool bCastShadow, const FMantleTerrainBuildParams& Params, FString* OutError = nullptr, const FTransform* StaticWorld = nullptr, const FTransform* MovableWorld = nullptr);

	~FMantleTerrainSpawn();

	/** What one Step of a far-only spawn, or of a server's (collision blocks only), may spend, seconds. */
	static constexpr double FarBudgetSeconds = 0.003;

	/**
	 * Builds the next chunk and its collision block. True once there is nothing more to do: every chunk is on the actor, or a chunk failed (HasFailed). A far-only spawn builds as many
	 * chunks as fit in FarBudgetSeconds (at least one): a far chunk costs a fraction of a full one. So does a spawn with no chunk meshes (a server's): a block costs a fraction of a mesh.
	 */
	bool Step();

	bool HasFailed() const { return !Error.IsEmpty(); }
	const FString& GetError() const { return Error; }

	/** The finished terrain actor, given up for good (it is then no longer destroyed with this object). Null when a chunk failed. */
	AActor* TakeActor();

	/** Chunks along each axis. */
	int32 GetChunksX() const { return ChunksX; }
	int32 GetChunksY() const { return ChunksY; }
	bool IsStatic() const { return bStatic; }
	bool CastsShadow() const { return bCastShadow; }
	UMaterialInterface* GetMaterial() const { return Material.Get(); }

	/**
	 * A far-only spawn (FMantleTerrainBuildParams::bFarOnly): each chunk's far component, by chunk index (Y * ChunksX + X); null for a chunk with nothing to show at that level
	 * of detail. Given up for good, like the actor.
	 */
	TArray<TWeakObjectPtr<UStaticMeshComponent>> TakeFarComponents() { return MoveTemp(FarComponents); }

private:
	FMantleTerrainSpawn() = default;

	TWeakObjectPtr<AActor> Actor;
	TWeakObjectPtr<USceneComponent> Root;
	/** Held strongly: a dynamic instance in the transient package, which nothing else roots until a chunk takes it. */
	TStrongObjectPtr<UMaterialInterface> Material;
	const FMantleHeightfield* Field = nullptr;
	/** The heights the collision blocks share (Params.FieldOwner, or a copy). */
	TSharedPtr<const FMantleHeightfield> FieldShared;
	FMantleTerrainBuildParams Params;
	bool bCastShadow = true;
	/** The terrain was made Static, in place: its chunks are too. */
	bool bStatic = false;
	int32 ChunksX = 0;
	int32 ChunksY = 0;
	/** The next chunk, in the order BuildChunks takes them: X fastest. */
	int32 Next = 0;
	/** Collision blocks made so far, and the bytes their shapes hold. */
	int32 Blocks = 0;
	int64 ShapeBytes = 0;
	/** -MantleTestTerrainRounding (not in a shipped build): the largest the blocks' shapes lie below the file, the largest bound of a block, and the blocks that broke a bound or lay above the file. */
	double RoundingLargestCm = 0.0;
	double RoundingBoundCm = 0.0;
	int32 RoundingBroken = 0;
	int64 Vertices = 0;
	double BuildSeconds = 0.0;
	FString Error;
	/** A far-only spawn: the components made, by chunk index. */
	TArray<TWeakObjectPtr<UStaticMeshComponent>> FarComponents;
};
