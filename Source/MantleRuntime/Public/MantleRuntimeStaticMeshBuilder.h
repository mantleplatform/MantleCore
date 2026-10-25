// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
class UObject;
class UStaticMesh;
struct FMantleRuntimeMesh;
struct FMantleStaticMeshData;

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
	 * With bComplexCollision, for an MRM2 that carries simple collision (#470): build its boxes, spheres,
	 * capsules and convex hulls into the body setup instead, with the trace flag the upload asked for
	 * (CTF_UseSimpleAsComplex, or CTF_UseSimpleAndComplex, which keeps the triangles for complex queries). Off, or
	 * for a mesh without simple collision, the collision is complex-as-simple as above.
	 */
	bool bUseSimpleCollision = true;

	/**
	 * #470: build an MRM2's authored lower LODs, switched at its screen sizes. Off, only LOD0 is built:
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
	 * refuses to initialise them (UStaticMesh::InitResources asserts). An uncooked process (the editor, a
	 * commandlet) initialises them as before. See FMantleRuntimeStaticMeshBuilder::ShouldInitRenderResources.
	 */
	bool bInitRenderResources = true;

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
 * MRM2 (FMantleStaticMeshReader, Fab F8 #342): uploaded place meshes and rigid accessories. Every
 * attribute is taken as cooked: positions, normals, tangents with their binormal sign, UVs and vertex
 * colours; nothing is recomputed. Indices keep glTF's order, which in Epic's axis convention is
 * already Unreal's front face (never reversed here). One polygon group per section in table (draw)
 * order; material slot i is the section whose MaterialIndex is i and is named after that section's
 * material, so a component's SetMaterial(i) and a place's materials[i] address the same slot.
 * #470: a second UV channel when the LOD carries UV1; the authored lower LODs (each a mesh
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
	 * a process the engine lets initialise them (FApp::CanEverRenderOrProduceRenderData). A dedicated server builds its meshes for collision only, and
	 * UStaticMesh::InitResources asserts there. The static mesh and terrain builders share this one answer.
	 */
	static bool ShouldInitRenderResources(bool bRequested);

	/**
	 * Null, with the reason logged as "MANTLE_STATICMESH FAIL ..." and returned in OutError, when the
	 * mesh is empty, its arrays disagree, an index is out of range, a position is not finite, no
	 * triangle survives, or it is called off the game thread.
	 */
	static UStaticMesh* Build(const FMantleRuntimeMesh& Mesh, const FMantleStaticMeshBuildParams& Params, FString* OutError = nullptr);

	/** MRM2, as above. Null, with the same logging, on anything inconsistent (FMantleStaticMeshReader's output never is). */
	static UStaticMesh* Build(const FMantleStaticMeshData& Mesh, const FMantleStaticMeshBuildParams& Params, FString* OutError = nullptr);
};
