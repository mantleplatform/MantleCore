// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeBool.h"
#include "MantleNaniteSideFile.h"

struct FMeshDescription;
class FMantleAssetStore;
struct FMantleContentHash;
class UMaterialInterface;
class UObject;
class UStaticMesh;
struct FMantleMeshLumenData;
struct FMantleRuntimeMesh;
struct FMantleStaticMeshData;

/** One slot per entry of SlotNames, in slot order. */
struct FMantleStaticMeshSlots
{
	TArray<FName> SlotNames;

	/**
	 * Per LOD, the material slot of each render section, keyed by the section's first index in the built index buffer: one per polygon group that kept a triangle (the fast
	 * build writes each group's triangles contiguously, in creation order, and makes no section for an empty one).
	 */
	TArray<TMap<uint32, int32>> SlotByFirstIndex;
};

/**
 * An MRM2's LODs as mesh descriptions, with their UV channels filled for the fast build: everything FMantleRuntimeStaticMeshBuilder::Build does that touches no engine object, so it can be
 * made on a worker (FMantleRuntimeStaticMeshBuilder::Prepare) while the game thread builds something else. A big mesh (60,000 vertices) is 50 to 70 ms of it.
 */
struct FMantlePreparedStaticMesh
{
	// Out of line (MantleRuntimeStaticMeshBuilder.cpp): FMeshDescription is only forward-declared here, so a file that includes this header needs no MeshDescription dependency.
	FMantlePreparedStaticMesh();
	~FMantlePreparedStaticMesh();
	FMantlePreparedStaticMesh(FMantlePreparedStaticMesh&&);
	FMantlePreparedStaticMesh& operator=(FMantlePreparedStaticMesh&&);
	FMantlePreparedStaticMesh(const FMantlePreparedStaticMesh&) = delete;
	FMantlePreparedStaticMesh& operator=(const FMantlePreparedStaticMesh&) = delete;

	TArray<FMeshDescription> Descriptions;
	FMantleStaticMeshSlots Slots;

	/** Per LOD, the texture coordinate channels its file authored (the description is filled to MAX_STATIC_TEXCOORDS for the engine's fast build, and cut back to these). */
	TArray<int32> AuthoredChannels;

	int32 Dropped = 0;
	int32 Triangles = 0;

	/** What the descriptions hold, about: the decodes in flight are counted by it. */
	int64 GetAllocatedBytes() const;
};

namespace Nanite
{
	struct FResources;
}

/**
 * A mesh's Nanite side file already read into the engine's Nanite data (FMantleRuntimeStaticMeshBuilder::ReadNanite), on any thread: reading a big file is a copy of all of
 * it (110 MB, 20 ms for the biggest mesh of the Medieval place), which the game thread's frame cannot afford. The build then takes the data instead of reading the file.
 */
struct FMantlePreparedNanite
{
	FMantlePreparedNanite();
	~FMantlePreparedNanite();

	/** Set by the reader, as its last write, when it is done (read or refused); read by the game thread before it touches the rest. */
	FThreadSafeBool bDone;

	/** True when the file was this mesh's and this engine's and was read; else Reason says why not. */
	bool bRead = false;
	FString Reason;
	TUniquePtr<Nanite::FResources> Resources;
	TArray<int32> SlotToPublished;

	/** What the file's streamed pages add up to (0: it has none). */
	FMantleNaniteStreamedBlock Streamed;
};

struct FMantleStaticMeshBuildParams
{
	/** The material every slot starts with; the engine's default material when null. Place meshes get theirs per slot from the place file. */
	UMaterialInterface* Material = nullptr;

	/** MRM1's one material slot's name. MRM2 names each slot after its section's material. */
	FName MaterialSlotName = TEXT("Material");

	/** Keep CPU copies of the index buffer (collision or authority on a server reads them). */
	bool bAllowCPUAccess = false;

	/**
	 * Complex-as-simple collision: the render triangles are the collision (double-sided), which is
	 * what place geometry needs for character movement on clients and authority on the dedicated
	 * server. Implies CPU access, since a non-editor build reads the triangles back from the render
	 * data to cook them.
	 */
	bool bComplexCollision = false;

	/**
	 * With bComplexCollision, for an MRM2 that carries simple collision: build its boxes, spheres,
	 * capsules and convex hulls into the body setup instead, with the trace flag the upload asked for
	 * (CTF_UseSimpleAsComplex, or CTF_UseSimpleAndComplex, which keeps the triangles for complex queries). Off, or
	 * for a mesh without simple collision, the collision is complex-as-simple as above.
	 */
	bool bUseSimpleCollision = true;

	/**
	 * Build an MRM2's authored lower LODs, switched at its screen sizes. Off, only LOD0 is built:
	 * what a dedicated server wants, since it never draws and its collision is LOD0's.
	 */
	bool bBuildLowerLODs = true;

	/**
	 * With bComplexCollision: cook the collision now, on the game thread. Off, the caller cooks it
	 * (UBodySetup::CreatePhysicsMeshesAsync), as the place asset loader does to keep large meshes off
	 * the frame.
	 */
	bool bCookCollisionNow = true;

	/**
	 * False builds a CPU-only mesh for collision: no render resources (GPU buffers, ray tracing representation),
	 * everything else as usual. A build also skips them in a cooked process that can never render
	 * (!FApp::CanEverRenderOrProduceRenderData: a dedicated server, a cooked -NullRHI run), where the engine
	 * refuses to initialize them (UStaticMesh::InitResources asserts). An uncooked process (the editor, a
	 * commandlet) initializes them as before. See FMantleRuntimeStaticMeshBuilder::ShouldInitRenderResources.
	 */
	bool bInitRenderResources = true;

	/**
	 * #149: the mesh's baked Lumen data (FMantleMeshLumenFile), or null. The build takes both halves (they move to
	 * LOD0's DistanceFieldData and CardRepresentationData, which the mesh then owns) before the render resources
	 * are initialized, so the scene proxy finds them: Lumen's software tracing and distance field lighting see the
	 * mesh. Left alone on a build without render resources.
	 */
	FMantleMeshLumenData* Lumen = nullptr;

	/**
	 * The inflated head (piece 0) of the mesh's Nanite file (FMantleNaniteSideFile::ReadHead), or null. The build reads
	 * it into the mesh's Nanite data before the render resources are initialized, next to the Lumen data, so the scene proxy draws the
	 * full-detail model where the built LOD0 is only Nanite's reduced stand-in. The built LODs, collision, Lumen data and ray tracing stay
	 * as they are. A file for another engine, another mesh (NaniteMeshTag), another slot order, or a damaged one is logged
	 * ("MANTLE_NANITE") and the mesh builds as today. Needs NaniteTable.
	 */
	const TArray<uint8>* NaniteFile = nullptr;

	/**
	 * The container's table of that file (shared with the mesh's page store), and where the file is on disk: the streamed pages are read from it, one page at a time, for as long as
	 * the mesh lives (FMantleNanitePageSource). Nothing of them is held in memory.
	 */
	TSharedPtr<const FMantleNaniteTable, ESPMode::ThreadSafe> NaniteTable;
	FString NanitePath;

	/** The file's hash in NaniteAssetStore, which the page store pins for as long as the mesh lives (the asset set's own pin goes with its builds). Both null: the file is not pinned (tests). Valid during the build call. */
	TSharedPtr<FMantleAssetStore> NaniteAssetStore;
	const FMantleContentHash* NaniteHash = nullptr;

	/** The Nanite root pages the caller still has room for (its budget less what it has attached): a file with more is not attached and the mesh draws plain. MAX: no limit. */
	int64 NaniteRootPagesLeft = MAX_int64;

	/** NaniteFile, already read on a worker (ReadNanite): the build takes this instead of reading the bytes. Needs NaniteFile set too (it says the mesh has a file). */
	FMantlePreparedNanite* NaniteRead = nullptr;

	/** The mesh upload's hash ("sha256:<hex>") the NaniteFile must belong to. */
	FString NaniteMeshTag;

	/** Outer of the new mesh; the transient package when null. */
	UObject* Outer = nullptr;

	/** Name of the new mesh; a unique generated one when None. */
	FName Name;
};

/**
 * Builds a transient, renderable UStaticMesh from a cooked mesh at runtime, through the engine's own
 * runtime path: UStaticMesh::BuildFromMeshDescriptions with the fast build a non-editor client
 * requires. No editor code.
 *
 * MRM1 (FMantleRuntimeMeshReader): rigid avatar accessories cooked before F2, and place meshes cooked
 * before MRM2. One section, optional normals and optional UVs: missing normals are computed (smooth,
 * area and angle weighted), tangents are always computed from the UVs, positions and winding are used
 * as the worker wrote them (MRM1's own axis convention).
 *
 * MRM2 (FMantleStaticMeshReader): uploaded place meshes and rigid accessories. Every
 * attribute is taken as cooked: positions, normals, tangents with their binormal sign, UVs and vertex
 * colors; nothing is recomputed. Indices keep glTF's order, which in Epic's axis convention is
 * already Unreal's front face (never reversed here). One polygon group per section in table (draw)
 * order; material slot i is the section whose MaterialIndex is i and is named after that section's
 * material, so a component's SetMaterial(i) and a place's materials[i] address the same slot.
 * A second UV channel when the LOD carries UV1; the authored lower LODs (each a mesh
 * description of its own, its sections bound to LOD0's slots) with the file's screen sizes
 * (FStaticMeshRenderData::ScreenSize); and the file's simple collision in the body setup
 * (bUseSimpleCollision).
 *
 * Either way, triangles naming a vertex twice are dropped (the mesh description refuses them).
 *
 * Game thread (creates a UObject and its render resources). Under -NullRHI the mesh builds with its
 * CPU data and no GPU resources.
 */
class MANTLERUNTIME_API FMantleRuntimeStaticMeshBuilder
{
public:
	/**
	 * Whether a runtime-built mesh gets render resources: bRequested (the build params' bInitRenderResources) and
	 * a process the engine lets initialize them (FApp::CanEverRenderOrProduceRenderData). A dedicated server builds its meshes for collision only, and
	 * UStaticMesh::InitResources asserts there. The static mesh and terrain builders share this one answer.
	 */
	static bool ShouldInitRenderResources(bool bRequested);

	/**
	 * Null, with the reason logged as "MANTLE_STATICMESH FAIL ..." and returned in OutError, when the
	 * mesh is empty, its arrays disagree, an index is out of range, a position is not finite, no
	 * triangle survives, or it is called off the game thread.
	 */
	static UStaticMesh* Build(const FMantleRuntimeMesh& Mesh, const FMantleStaticMeshBuildParams& Params, FString* OutError = nullptr);

	/** MRM2, as above. Null, with the same logging, on anything inconsistent (FMantleStaticMeshReader's output never is). Prepare and BuildPrepared in one call. */
	static UStaticMesh* Build(const FMantleStaticMeshData& Mesh, const FMantleStaticMeshBuildParams& Params, FString* OutError = nullptr);

	/**
	 * The first half of Build for an MRM2, on ANY thread: the mesh description of every LOD (the lower ones only with bBuildLowerLODs) and the UV channels filled. Touches no engine
	 * object. False, with the reason logged as "MANTLE_STATICMESH FAIL ..." and returned in OutError, when the mesh is inconsistent.
	 */
	static bool Prepare(const FMantleStaticMeshData& Mesh, bool bBuildLowerLODs, FMantlePreparedStaticMesh& Out, FString* OutError = nullptr);

	/**
	 * The second half, on the game thread: the UStaticMesh, the engine's build of each LOD, Nanite and Lumen data, render resources, collision. Mesh is the same data Prepare read (the screen sizes
	 * and the simple collision come from it). The result is the one Build would make.
	 */
	static UStaticMesh* BuildPrepared(const FMantleStaticMeshData& Mesh, FMantlePreparedStaticMesh&& Prepared, const FMantleStaticMeshBuildParams& Params, FString* OutError = nullptr);

	/** The size of a Nanite file from which reading it is done on a worker (a smaller one is read in the build: under a millisecond on the game thread, and no frame of waiting). */
	static constexpr int64 NaniteReadOffThreadBytes = 4 * 1024 * 1024;

	/** Whether a Nanite file whose root data is this many bytes is read on a worker: NaniteReadOffThreadBytes or more (a version 2 file's root data is at most about 1 MiB, however big the file). */
	static bool ShouldReadNaniteOffThread(int64 FileBytes);

	/** Reads the file's head (FMantleNaniteSideFile::Read, which checks the tag, the engine, the layout and the table) into Out, any thread, and sets Out.bDone last. Out.bRead says whether it was usable. */
	static void ReadNanite(const TArray<uint8>& Head, const FString& MeshTag, const FMantleNaniteTable& Table, FMantlePreparedNanite& Out);

	/**
	 * Says in what a build of a mesh with a Nanite file attached what it did with it, for the asset set: the root pages and root bytes that went into the engine's Nanite data
	 * (0, 0 when the mesh has none).
	 */
	static void GetNaniteAttached(const UStaticMesh& Mesh, int32& OutRootPages, int64& OutRootBytes);
};
