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

// Runtime meshes built for collision alone. A cooked dedicated server cannot initialize render
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

	/** Every material slot has its UV channel data set, so UStaticMesh::GetUVChannelData does not ensure. */
	void ExpectUVChannelData(FAutomationTestBase& Test, UStaticMesh* Mesh)
	{
		Test.TestTrue(TEXT("the mesh has a material slot"), Mesh->GetStaticMaterials().Num() > 0);
		for (const FStaticMaterial& Slot : Mesh->GetStaticMaterials())
		{
			Test.TestTrue(TEXT("the slot's UV channel data is initialized"), Slot.UVChannelData.bInitialized);
		}
		// Would ensure (and write a crash report) on an uninitialized slot.
		Test.TestTrue(TEXT("GetUVChannelData(0) reads the slot"), Mesh->GetUVChannelData(0) != nullptr);
	}

	/**
	 * What a server's collision needs from a mesh built without render resources: none initialized, one LOD of
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
		ExpectUVChannelData(Test, Mesh);
		Test.TestFalse(TEXT("the bounds are not empty"), RenderData->Bounds.BoxExtent.IsNearlyZero());
		Test.TestFalse(TEXT("the mesh bounds are not empty"), Mesh->GetBounds().BoxExtent.IsNearlyZero());

		Test.TestTrue(TEXT("the collision triangles are there"), Mesh->ContainsPhysicsTriMeshData(true));
		FTriMeshCollisionData Collision;
		Test.TestTrue(TEXT("the collision triangles read back"), Mesh->GetPhysicsTriMeshData(&Collision, true));
		Test.TestEqual(TEXT("collision vertices"), Collision.Vertices.Num(), ExpectedVertices);
		Test.TestEqual(TEXT("collision triangles"), Collision.Indices.Num(), ExpectedTriangles);
	}

	/** A terrain chunk built without render resources: a picture only, so no body and no CPU copy of its triangles (the ground's collision is the height field). */
	void ExpectPictureOnlyMesh(FAutomationTestBase& Test, UStaticMesh* Mesh, const int32 ExpectedVertices, const int32 ExpectedTriangles)
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
		Test.TestFalse(TEXT("no collision was cooked for a picture"), Body && Body->TriMeshGeometries.Num() > 0);
		Test.TestFalse(TEXT("the mesh keeps no CPU copy for collision"), Mesh->bAllowCPUAccess);
		ExpectUVChannelData(Test, Mesh);
		Test.TestFalse(TEXT("the mesh bounds are not empty"), Mesh->GetBounds().BoxExtent.IsNearlyZero());
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

namespace MantleHeadlessMeshBuildTestPrivate
{
	/** Whether every LOD's position, colour, tangent and UV buffer keeps its CPU copy (all true, all false, or a mix: -1). */
	int32 VertexCopyState(const UStaticMesh& Mesh)
	{
		const FStaticMeshRenderData* RenderData = Mesh.GetRenderData();
		if (!RenderData || RenderData->LODResources.IsEmpty())
		{
			return -1;
		}
		int32 Kept = 0;
		int32 Total = 0;
		for (const FStaticMeshLODResources& LOD : RenderData->LODResources)
		{
			Kept += LOD.VertexBuffers.PositionVertexBuffer.GetAllowCPUAccess() ? 1 : 0;
			Kept += LOD.VertexBuffers.ColorVertexBuffer.GetAllowCPUAccess() ? 1 : 0;
			Kept += LOD.VertexBuffers.StaticMeshVertexBuffer.GetAllowCPUAccess() ? 1 : 0;
			Total += 3;
		}
		return Kept == Total ? 1 : (Kept == 0 ? 0 : -1);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStaticMeshVertexCPUCopiesTest,
	"Mantle.Runtime.StaticMesh.VertexCPUCopies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStaticMeshVertexCPUCopiesTest::RunTest(const FString& Parameters)
{
	using namespace MantleHeadlessMeshBuildTestPrivate;

	// The rule: a copy where collision reads the triangles back, and where nothing uploads the buffers; none for a mesh that is drawn only.
	TestTrue(TEXT("collision mesh, render resources: kept"), FMantleRuntimeStaticMeshBuilder::KeepsVertexCPUCopies(true, true));
	TestTrue(TEXT("collision mesh, none: kept"), FMantleRuntimeStaticMeshBuilder::KeepsVertexCPUCopies(true, false));
	TestTrue(TEXT("picture mesh without render resources (a server's): kept, there is no upload"), FMantleRuntimeStaticMeshBuilder::KeepsVertexCPUCopies(false, false));
	TestTrue(TEXT("picture mesh with render resources: given back wherever the process can render"),
		FMantleRuntimeStaticMeshBuilder::KeepsVertexCPUCopies(false, true) == !FApp::CanEverRenderOrProduceRenderData());

	// The buffers: every value survives both ways, the flag is what was asked for, and the UV channels are cut.
	{
		constexpr uint32 Count = 5;
		constexpr uint32 Channels = 3;
		FStaticMeshVertexBuffers Buffers;
		TArray<FVector3f> Positions;
		TArray<FColor> Colors;
		for (uint32 Vertex = 0; Vertex < Count; ++Vertex)
		{
			Positions.Add(FVector3f(1.0f + Vertex, 2.0f * Vertex, -3.0f * Vertex));
			Colors.Add(FColor(static_cast<uint8>(10 * Vertex), static_cast<uint8>(20 + Vertex), 30, 255));
		}
		Buffers.PositionVertexBuffer.Init(Positions, /*bNeedsCPUAccess*/ true);
		Buffers.ColorVertexBuffer.InitFromColorArray(Colors.GetData(), Count, sizeof(FColor), /*bNeedsCPUAccess*/ true);
		Buffers.StaticMeshVertexBuffer.Init(Count, Channels, /*bNeedsCPUAccess*/ true);
		for (uint32 Vertex = 0; Vertex < Count; ++Vertex)
		{
			Buffers.StaticMeshVertexBuffer.SetVertexTangents(Vertex, FVector3f(1, 0, 0), FVector3f(0, 1, 0), FVector3f(0, 0, 1));
			for (uint32 Channel = 0; Channel < Channels; ++Channel)
			{
				Buffers.StaticMeshVertexBuffer.SetVertexUV(Vertex, Channel, FVector2f(0.25f * Vertex, 0.5f * Channel));
			}
		}
		const FVector4f TangentZBefore = Buffers.StaticMeshVertexBuffer.VertexTangentZ(2);

		FMantleRuntimeStaticMeshBuilder::SetVertexCPUAccess(Buffers, /*AuthoredChannels*/ 2, /*bNeedsCPUAccess*/ false);
		TestFalse(TEXT("positions: no CPU copy asked for"), Buffers.PositionVertexBuffer.GetAllowCPUAccess());
		TestFalse(TEXT("colours: no CPU copy asked for"), Buffers.ColorVertexBuffer.GetAllowCPUAccess());
		TestFalse(TEXT("tangents and UVs: no CPU copy asked for"), Buffers.StaticMeshVertexBuffer.GetAllowCPUAccess());
		TestEqual(TEXT("UV channels cut to the authored ones"), static_cast<int32>(Buffers.StaticMeshVertexBuffer.GetNumTexCoords()), 2);
		TestEqual(TEXT("vertex count kept"), static_cast<int32>(Buffers.PositionVertexBuffer.GetNumVertices()), static_cast<int32>(Count));
		TestEqual(TEXT("colour count kept"), static_cast<int32>(Buffers.ColorVertexBuffer.GetNumVertices()), static_cast<int32>(Count));
		for (uint32 Vertex = 0; Vertex < Count; ++Vertex)
		{
			TestTrue(TEXT("position kept"), Buffers.PositionVertexBuffer.VertexPosition(Vertex) == Positions[Vertex]);
			TestTrue(TEXT("colour kept"), Buffers.ColorVertexBuffer.VertexColor(Vertex) == Colors[Vertex]);
			for (uint32 Channel = 0; Channel < 2; ++Channel)
			{
				TestTrue(TEXT("UV kept"), Buffers.StaticMeshVertexBuffer.GetVertexUV(Vertex, Channel).Equals(FVector2f(0.25f * Vertex, 0.5f * Channel), 0.001f));
			}
		}
		TestTrue(TEXT("tangent kept"), Buffers.StaticMeshVertexBuffer.VertexTangentZ(2).Equals(TangentZBefore, 0.01f));

		// Asked again without a change: nothing is made again.
		FMantleRuntimeStaticMeshBuilder::SetVertexCPUAccess(Buffers, 2, false);
		TestFalse(TEXT("still no copy"), Buffers.PositionVertexBuffer.GetAllowCPUAccess());
		// And back: a mesh upgraded in place would get its copies again with every value.
		FMantleRuntimeStaticMeshBuilder::SetVertexCPUAccess(Buffers, 2, true);
		TestTrue(TEXT("positions: copy asked for again"), Buffers.PositionVertexBuffer.GetAllowCPUAccess());
		TestTrue(TEXT("colours: copy asked for again"), Buffers.ColorVertexBuffer.GetAllowCPUAccess());
		TestTrue(TEXT("tangents and UVs: copy asked for again"), Buffers.StaticMeshVertexBuffer.GetAllowCPUAccess());
		for (uint32 Vertex = 0; Vertex < Count; ++Vertex)
		{
			TestTrue(TEXT("position kept both ways"), Buffers.PositionVertexBuffer.VertexPosition(Vertex) == Positions[Vertex]);
			TestTrue(TEXT("colour kept both ways"), Buffers.ColorVertexBuffer.VertexColor(Vertex) == Colors[Vertex]);
		}
	}

	// The builds: a mesh with no collision follows the rule, a mesh with complex collision keeps its copies and still cooks and reads its triangles back.
	const FMantleRuntimeMesh Box = MakeBox();
	FString Error;
	FMantleStaticMeshBuildParams Picture;
	Picture.bComplexCollision = false;
	TStrongObjectPtr<UStaticMesh> PictureMesh(FMantleRuntimeStaticMeshBuilder::Build(Box, Picture, &Error));
	if (!TestNotNull(TEXT("the picture mesh builds"), PictureMesh.Get()))
	{
		AddError(Error);
		return false;
	}
	TestFalse(TEXT("a picture mesh allows no CPU access"), PictureMesh->bAllowCPUAccess);
	const bool bPictureKeeps = FMantleRuntimeStaticMeshBuilder::KeepsVertexCPUCopies(false, true);
	TestEqual(TEXT("a picture mesh's vertex buffers follow the rule"), VertexCopyState(*PictureMesh), bPictureKeeps ? 1 : 0);

	FMantleStaticMeshBuildParams Solid;
	Solid.bComplexCollision = true;
	Solid.bCookCollisionNow = true;
	TStrongObjectPtr<UStaticMesh> SolidMesh(FMantleRuntimeStaticMeshBuilder::Build(Box, Solid, &Error));
	if (!TestNotNull(TEXT("the solid mesh builds"), SolidMesh.Get()))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("a solid mesh allows CPU access"), SolidMesh->bAllowCPUAccess);
	TestEqual(TEXT("a solid mesh keeps every vertex copy"), VertexCopyState(*SolidMesh), 1);
	FTriMeshCollisionData Collision;
	TestTrue(TEXT("a solid mesh's triangles read back"), SolidMesh->GetPhysicsTriMeshData(&Collision, true));
	TestEqual(TEXT("with all its vertices"), Collision.Vertices.Num(), 8);
	TestEqual(TEXT("and all its triangles"), Collision.Indices.Num(), 12);
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

	// What a test process asks for: LOD 0 only, no render resources. (A dedicated server builds no chunk at all.)
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
	ExpectPictureOnlyMesh(*this, Chunks[0].Mesh, 9, 8);

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
