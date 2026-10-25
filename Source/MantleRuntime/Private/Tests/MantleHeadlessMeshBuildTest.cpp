// Copyright Joshua Eger. All Rights Reserved.
#include "Engine/StaticMesh.h"
#include "Interfaces/Interface_CollisionDataProvider.h"
#include "MantleHeightfieldReader.h"
#include "MantleRuntimeMesh.h"
#include "MantleRuntimeStaticMeshBuilder.h"
#include "MantleTerrainBuilder.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshResources.h"
#include "UObject/StrongObjectPtr.h"

#include "Misc/App.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Runtime meshes built for collision alone. A cooked dedicated server cannot initialise render
// resources (UStaticMesh::InitResources asserts there), so both the static mesh builder and the terrain
// builder build the mesh without them: the CPU buffers, the bounds and the body setup stay, which is all
// the server's collision reads. No world, no network.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleHeadlessMeshBuildTestPrivate
{
	/** A unit box (100 cm a side) as an MRM1 mesh: 8 shared vertices, 12 triangles, planar UVs, no normals. */
	FMantleRuntimeMesh MakeBox()
	{
		FMantleRuntimeMesh Mesh;
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const float X = (Corner & 1) ? 50.0f : -50.0f;
			const float Y = (Corner & 2) ? 50.0f : -50.0f;
			const float Z = (Corner & 4) ? 50.0f : -50.0f;
			Mesh.Positions.Add(FVector3f(X, Y, Z));
			Mesh.UVs.Add(FVector2f((X + 50.0f) / 100.0f, (Y + 50.0f) / 100.0f));
		}
		const uint32 Triangles[12][3] = {
			{ 0, 2, 1 }, { 1, 2, 3 }, { 4, 5, 6 }, { 5, 7, 6 },
			{ 0, 1, 4 }, { 1, 5, 4 }, { 2, 6, 3 }, { 3, 6, 7 },
			{ 0, 4, 2 }, { 2, 4, 6 }, { 1, 3, 5 }, { 3, 7, 5 } };
		for (const uint32 (&Triangle)[3] : Triangles)
		{
			Mesh.Indices.Add(Triangle[0]);
			Mesh.Indices.Add(Triangle[1]);
			Mesh.Indices.Add(Triangle[2]);
		}
		return Mesh;
	}

	/** A Size x Size heightfield, SpacingCm apart, rising SlopePerSample cm for each sample along X. */
	FMantleHeightfield MakeHeadlessField(const int32 Size, const float SpacingCm, const uint16 SlopePerSample)
	{
		FMantleHeightfield Field;
		Field.Width = Size;
		Field.Height = Size;
		Field.SpacingX = SpacingCm;
		Field.SpacingY = SpacingCm;
		Field.HeightScale = 1.0f;
		Field.HeightOffset = 0.0f;
		Field.Heights.SetNumUninitialized(Size * Size);
		for (int32 Y = 0; Y < Size; ++Y)
		{
			for (int32 X = 0; X < Size; ++X)
			{
				Field.Heights[Y * Size + X] = static_cast<uint16>(X * SlopePerSample);
			}
		}
		return Field;
	}

	/**
	 * What a server's collision needs from a mesh built without render resources: none initialised, one LOD of
	 * render data holding the CPU vertices, a body setup, bounds, and the collision triangles read back.
	 */
	void ExpectCollisionOnlyMesh(FAutomationTestBase& Test, UStaticMesh* Mesh, const int32 ExpectedVertices, const int32 ExpectedTriangles)
	{
		if (!Test.TestNotNull(TEXT("the mesh"), Mesh))
		{
			return;
		}
		Test.TestFalse(TEXT("no render resources"), Mesh->AreRenderingResourcesInitialized());
		const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		if (!Test.TestNotNull(TEXT("render data"), RenderData) || !Test.TestEqual(TEXT("one LOD"), RenderData->LODResources.Num(), 1))
		{
			return;
		}
		const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
		Test.TestEqual(TEXT("LOD0 position vertices"), static_cast<int32>(LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices()), ExpectedVertices);
		Test.TestEqual(TEXT("LOD0 triangles"), LOD.GetNumTriangles(), ExpectedTriangles);
		const UBodySetup* Body = Mesh->GetBodySetup();
		if (Test.TestNotNull(TEXT("the body setup"), Body))
		{
			// The collision cook ran off the CPU buffers: a cooked triangle mesh, no failure.
			Test.TestFalse(TEXT("the collision cook did not fail"), static_cast<bool>(Body->bFailedToCreatePhysicsMeshes));
			Test.TestTrue(TEXT("the collision cooked a triangle mesh"), Body->TriMeshGeometries.Num() > 0);
		}
		Test.TestFalse(TEXT("the bounds are not empty"), RenderData->Bounds.BoxExtent.IsNearlyZero());
		Test.TestFalse(TEXT("the mesh bounds are not empty"), Mesh->GetBounds().BoxExtent.IsNearlyZero());

		Test.TestTrue(TEXT("the collision triangles are there"), Mesh->ContainsPhysicsTriMeshData(true));
		FTriMeshCollisionData Collision;
		Test.TestTrue(TEXT("the collision triangles read back"), Mesh->GetPhysicsTriMeshData(&Collision, true));
		Test.TestEqual(TEXT("collision vertices"), Collision.Vertices.Num(), ExpectedVertices);
		Test.TestEqual(TEXT("collision triangles"), Collision.Indices.Num(), ExpectedTriangles);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStaticMeshHeadlessBuildTest,
	"Mantle.Runtime.StaticMesh.HeadlessBuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStaticMeshHeadlessBuildTest::RunTest(const FString& Parameters)
{
	using namespace MantleHeadlessMeshBuildTestPrivate;

	const FMantleRuntimeMesh Box = MakeBox();

	FMantleStaticMeshBuildParams Params;
	Params.bComplexCollision = true;
	Params.bCookCollisionNow = true;
	Params.bInitRenderResources = false;
	FString Error;
	TStrongObjectPtr<UStaticMesh> CpuOnly(FMantleRuntimeStaticMeshBuilder::Build(Box, Params, &Error));
	if (!TestNotNull(TEXT("the CPU-only mesh builds"), CpuOnly.Get()))
	{
		AddError(Error);
		return false;
	}
	ExpectCollisionOnlyMesh(*this, CpuOnly.Get(), 8, 12);

	// Asked for render resources: they exist exactly when the engine allows them in this process (the same
	// condition as the assert in FStaticMeshRenderData::InitResources).
	Params.bInitRenderResources = true;
	TStrongObjectPtr<UStaticMesh> Default(FMantleRuntimeStaticMeshBuilder::Build(Box, Params, &Error));
	if (!TestNotNull(TEXT("the default mesh builds"), Default.Get()))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("render resources follow FApp::CanEverRenderOrProduceRenderData"), Default->AreRenderingResourcesInitialized() == FApp::CanEverRenderOrProduceRenderData());
	FTriMeshCollisionData Collision;
	TestTrue(TEXT("the default mesh's collision triangles read back"), Default->GetPhysicsTriMeshData(&Collision, true));
	TestEqual(TEXT("the default mesh's collision triangles"), Collision.Indices.Num(), 12);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainHeadlessBuildTest,
	"Mantle.Runtime.Terrain.HeadlessBuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainHeadlessBuildTest::RunTest(const FString& Parameters)
{
	using namespace MantleHeadlessMeshBuildTestPrivate;

	// 3 x 3 samples, one chunk: 9 vertices and 8 triangles, no skirts.
	const FMantleHeightfield Field = MakeHeadlessField(3, 100.0f, 50);

	// What a dedicated server asks for: LOD 0 only, collision, no render resources.
	FMantleTerrainBuildParams Params;
	Params.bRenderLODs = false;
	Params.bCollision = true;
	Params.bInitRenderResources = false;
	FString Error;
	TArray<FMantleTerrainChunk> Chunks;
	if (!TestTrue(TEXT("the CPU-only terrain builds"), FMantleTerrainBuilder::BuildChunks(Field, Params, Chunks, &Error)))
	{
		AddError(Error);
		return false;
	}
	TArray<TStrongObjectPtr<UStaticMesh>> Keep;
	for (const FMantleTerrainChunk& Chunk : Chunks)
	{
		Keep.Emplace(Chunk.Mesh);
	}
	if (!TestEqual(TEXT("one chunk"), Chunks.Num(), 1))
	{
		return false;
	}
	ExpectCollisionOnlyMesh(*this, Chunks[0].Mesh, 9, 8);
	if (Chunks[0].Mesh && Chunks[0].Mesh->GetBodySetup())
	{
		TestTrue(TEXT("the terrain collision is complex-as-simple"), Chunks[0].Mesh->GetBodySetup()->CollisionTraceFlag == CTF_UseComplexAsSimple);
	}

	// Asked for render resources: the engine's build, so they exist exactly when this process can render.
	Params.bInitRenderResources = true;
	TArray<FMantleTerrainChunk> Default;
	if (!TestTrue(TEXT("the default terrain builds"), FMantleTerrainBuilder::BuildChunks(Field, Params, Default, &Error)))
	{
		AddError(Error);
		return false;
	}
	for (const FMantleTerrainChunk& Chunk : Default)
	{
		Keep.Emplace(Chunk.Mesh);
	}
	if (TestEqual(TEXT("one default chunk"), Default.Num(), 1) && TestNotNull(TEXT("the default chunk's mesh"), Default[0].Mesh))
	{
		TestTrue(TEXT("render resources follow FApp::CanEverRenderOrProduceRenderData"), Default[0].Mesh->AreRenderingResourcesInitialized() == FApp::CanEverRenderOrProduceRenderData());
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
