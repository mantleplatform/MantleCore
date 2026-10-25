// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMirrorBake.h"
#include "MantlePlaceAssets.h"

#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Math/RotationMatrix.h"
#include "Math/ScaleMatrix.h"
#include "Misc/AutomationTest.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

// A mesh mirrored by a negative scale becomes a mirrored copy of the mesh at export: the same mesh with its
// vertices mirrored on each negative axis, its triangles turned over when an odd number of axes is mirrored, its
// slots, UVs and collision kept.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleMirrorBakeTestPrivate
{
	const TCHAR* const CubePath = TEXT("/Engine/BasicShapes/Cube.Cube");

	/** The engine cube's centre moved here: a mesh that is not symmetric about its origin, so a mirror shows. */
	const FVector CubeOffset(100.0, 20.0, 30.0);

	/** A box that is off-centre and turned, to see collision mirror. */
	const FVector BoxCenter(100.0, 20.0, 30.0);
	const FRotator BoxRotation(0.0, 30.0, 0.0);

	/** Cleans the cache up either side of a test, and holds the source meshes alive. */
	struct FMirrorFixture
	{
		TStrongObjectPtr<UStaticMesh> Cube;
		TStrongObjectPtr<UStaticMesh> Offset;

		FMirrorFixture()
		{
			FMantleMirrorBake::ClearCache();
			Cube.Reset(LoadObject<UStaticMesh>(nullptr, CubePath));
			if (Cube.IsValid() && Cube->GetRenderData() && Cube->GetRenderData()->LODResources.Num() > 0)
			{
				Offset.Reset(MakeOffsetCube(*Cube));
			}
		}

		~FMirrorFixture()
		{
			FMantleMirrorBake::ClearCache();
		}

		bool IsReady() const { return Cube.IsValid() && Offset.IsValid() && Offset->GetRenderData() && Offset->GetRenderData()->LODResources.Num() > 0; }

		/** The engine cube moved by CubeOffset, with the cube's slot and a turned box as simple collision: transient, built as the bakes build theirs. */
		static UStaticMesh* MakeOffsetCube(const UStaticMesh& From)
		{
			FMeshDescription Part;
			From.ExportStaticMeshLOD(From.GetRenderData()->LODResources[0], Part);
			FStaticMeshOperations::ApplyTransform(Part, FTransform(CubeOffset));

			UObject* Outer = GetTransientPackage();
			UStaticMesh* Mesh = NewObject<UStaticMesh>(Outer, MakeUniqueObjectName(Outer, UStaticMesh::StaticClass(), FName(TEXT("MirrorTestCube"))), RF_Transient);
			Mesh->GetStaticMaterials() = From.GetStaticMaterials();
			Mesh->bAllowCPUAccess = true;
			Mesh->SetNumSourceModels(1);
			FMeshBuildSettings& Settings = Mesh->GetSourceModel(0).BuildSettings;
			Settings.bRecomputeNormals = false;
			Settings.bRecomputeTangents = false;
			Settings.bGenerateLightmapUVs = false;
			Settings.bBuildReversedIndexBuffer = false;
			Settings.DistanceFieldResolutionScale = 0.0f;
			Settings.MaxLumenMeshCards = 0;

			UStaticMesh::FBuildMeshDescriptionsParams Params;
			Params.bMarkPackageDirty = false;
			Params.bAllowCpuAccess = true;
			TArray<const FMeshDescription*> Descriptions;
			Descriptions.Add(&Part);
			Mesh->BuildFromMeshDescriptions(Descriptions, Params);

			Mesh->CreateBodySetup();
			if (UBodySetup* Body = Mesh->GetBodySetup())
			{
				Body->CollisionTraceFlag = CTF_UseSimpleAndComplex;
				Body->AggGeom.EmptyElements();
				FKBoxElem Box;
				Box.Center = BoxCenter;
				Box.Rotation = BoxRotation;
				Box.X = 100.0f;
				Box.Y = 100.0f;
				Box.Z = 100.0f;
				Body->AggGeom.BoxElems.Add(Box);
				Body->InvalidatePhysicsData();
			}
			return Mesh;
		}
	};

	/** LOD 0 as the converter reads it. */
	struct FReadMesh
	{
		TArray<FVector> Positions;
		TArray<FVector> Normals;
		TArray<FVector> TangentsX;
		TArray<float> BinormalSigns;
		TArray<FVector2D> UV0s;
		TArray<FVector2D> UV1s;
		TArray<uint32> Indices;
		int32 NumTexCoords = 0;
		FBox Bounds = FBox(ForceInit);

		bool Read(const UStaticMesh& Mesh)
		{
			const FStaticMeshRenderData* RenderData = Mesh.GetRenderData();
			if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
			{
				return false;
			}
			const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
			const FPositionVertexBuffer& PositionBuffer = LOD.VertexBuffers.PositionVertexBuffer;
			const FStaticMeshVertexBuffer& VertexBuffer = LOD.VertexBuffers.StaticMeshVertexBuffer;
			if (PositionBuffer.GetNumVertices() == 0 || !PositionBuffer.GetVertexData() || !VertexBuffer.GetTangentData() || !VertexBuffer.GetTexCoordData())
			{
				return false;
			}
			NumTexCoords = static_cast<int32>(VertexBuffer.GetNumTexCoords());
			for (uint32 Vertex = 0; Vertex < PositionBuffer.GetNumVertices(); ++Vertex)
			{
				const FVector Position(PositionBuffer.VertexPosition(Vertex));
				Positions.Add(Position);
				Bounds += Position;
				const FVector4f TangentX = VertexBuffer.VertexTangentX(Vertex);
				const FVector4f TangentZ = VertexBuffer.VertexTangentZ(Vertex);
				TangentsX.Add(FVector(TangentX.X, TangentX.Y, TangentX.Z));
				Normals.Add(FVector(TangentZ.X, TangentZ.Y, TangentZ.Z));
				BinormalSigns.Add(TangentZ.W < 0.0f ? -1.0f : 1.0f);
				UV0s.Add(FVector2D(VertexBuffer.GetVertexUV(Vertex, 0)));
				UV1s.Add(NumTexCoords > 1 ? FVector2D(VertexBuffer.GetVertexUV(Vertex, 1)) : FVector2D::ZeroVector);
			}
			LOD.IndexBuffer.GetCopy(Indices);
			return Indices.Num() >= 3;
		}

		/**
		 * Which way triangle Triangle winds round the mesh's middle: positive when its face looks out of it, negative when
		 * it looks in (for a convex mesh like a cube, whichever way the engine's winding goes: compare it with the source's).
		 */
		double Orientation(int32 Triangle, const FVector& Middle) const
		{
			const FVector& A = Positions[Indices[Triangle * 3]];
			const FVector& B = Positions[Indices[Triangle * 3 + 1]];
			const FVector& C = Positions[Indices[Triangle * 3 + 2]];
			return FVector::DotProduct(FVector::CrossProduct(B - A, C - A), (A + B + C) / 3.0 - Middle);
		}

		int32 NumTriangles() const { return Indices.Num() / 3; }
	};

	FVector SignsOf(const FVector& Scale)
	{
		return FVector(Scale.X < 0.0 ? -1.0 : 1.0, Scale.Y < 0.0 ? -1.0 : 1.0, Scale.Z < 0.0 ? -1.0 : 1.0);
	}

	/** Box with every axis Signs mirrors flipped about the origin. */
	FBox MirroredBox(const FBox& Box, const FVector& Signs)
	{
		const FVector A = Box.Min * Signs;
		const FVector B = Box.Max * Signs;
		return FBox(A.ComponentMin(B), A.ComponentMax(B));
	}

	/** The negative-scale patterns a level can have, as the scales that make them. */
	TArray<FVector> Patterns()
	{
		return {
			FVector(-1.0, 1.0, 1.0), FVector(1.0, -1.0, 1.0), FVector(1.0, 1.0, -1.0),
			FVector(-1.0, -1.0, 1.0), FVector(-1.0, 1.0, -1.0), FVector(1.0, -1.0, -1.0), FVector(-1.0, -1.0, -1.0),
		};
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMirrorBakeBoundsTest,
	"Mantle.Editor.MirrorBake.BoundsAreMirrored",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMirrorBakeBoundsTest::RunTest(const FString& Parameters)
{
	using namespace MantleMirrorBakeTestPrivate;
	FMirrorFixture Fixture;
	if (!TestTrue(TEXT("the fixture builds (the engine cube and an off-centre copy of it)"), Fixture.IsReady()))
	{
		return false;
	}
	FReadMesh Source;
	if (!TestTrue(TEXT("the source mesh reads"), Source.Read(*Fixture.Offset)))
	{
		return false;
	}
	// The fixture is off-centre, or no mirror would show.
	TestTrue(TEXT("the source cube is off-centre"), Source.Bounds.GetCenter().Equals(CubeOffset, 0.1));

	// A cube mirrored on X: its bounds are the source's about x = 0.
	{
		FVector Positive;
		FString Why;
		UStaticMesh* Mirrored = FMantleMirrorBake::MirroredCopy(Fixture.Offset.Get(), FVector(-1.0, 1.0, 1.0), Positive, Why);
		if (!TestNotNull(*FString::Printf(TEXT("a copy mirrored on X (%s)"), *Why), Mirrored))
		{
			return false;
		}
		FReadMesh Copy;
		if (!TestTrue(TEXT("the copy reads"), Copy.Read(*Mirrored)))
		{
			return false;
		}
		const FBox Expected = MirroredBox(Source.Bounds, FVector(-1.0, 1.0, 1.0));
		TestTrue(FString::Printf(TEXT("the copy's bounds are the source's mirrored on X (copy %s, expected %s)"), *Copy.Bounds.ToString(), *Expected.ToString()),
			Copy.Bounds.Min.Equals(Expected.Min, 0.1) && Copy.Bounds.Max.Equals(Expected.Max, 0.1));
		TestTrue(TEXT("and the mesh's own bounds agree"), Mirrored->GetBounds().GetBox().Min.Equals(Expected.Min, 0.5) && Mirrored->GetBounds().GetBox().Max.Equals(Expected.Max, 0.5));
		TestTrue(TEXT("the copy is placed at the positive scale"), Positive.Equals(FVector(1.0, 1.0, 1.0)));
		TestTrue(TEXT("it is transient, not in the creator's Content"), Mirrored->GetOutermost() == GetTransientPackage());
		TestEqual(TEXT("it has the source's vertices"), Copy.Positions.Num(), Source.Positions.Num());
		TestEqual(TEXT("and triangles"), Copy.NumTriangles(), Source.NumTriangles());
	}

	// Every pattern, with the scale's magnitude along: the positive scale is the magnitude.
	for (const FVector& Pattern : Patterns())
	{
		const FVector Scale = Pattern * FVector(2.0, 3.0, 0.5);
		FVector Positive;
		FString Why;
		UStaticMesh* Mirrored = FMantleMirrorBake::MirroredCopy(Fixture.Offset.Get(), Scale, Positive, Why);
		FReadMesh Copy;
		if (!TestNotNull(*FString::Printf(TEXT("a copy for %s (%s)"), *Scale.ToString(), *Why), Mirrored) || !TestTrue(TEXT("it reads"), Copy.Read(*Mirrored)))
		{
			continue;
		}
		const FBox Expected = MirroredBox(Source.Bounds, SignsOf(Scale));
		TestTrue(FString::Printf(TEXT("%s: bounds mirrored (copy %s, expected %s)"), *Scale.ToString(), *Copy.Bounds.ToString(), *Expected.ToString()),
			Copy.Bounds.Min.Equals(Expected.Min, 0.1) && Copy.Bounds.Max.Equals(Expected.Max, 0.1));
		TestTrue(FString::Printf(TEXT("%s: the positive scale is abs(scale) (%s)"), *Scale.ToString(), *Positive.ToString()), Positive.Equals(Scale.GetAbs()));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMirrorBakeWindingTest,
	"Mantle.Editor.MirrorBake.WindingFlipsForAnOddNumberOfAxes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMirrorBakeWindingTest::RunTest(const FString& Parameters)
{
	using namespace MantleMirrorBakeTestPrivate;

	// The rule: one or three negative axes turn a mesh inside out; two are a rotation.
	TestFalse(TEXT("no negative axis: no flip"), FMantleMirrorBake::FlipsWinding(FVector(1.0, 2.0, 3.0)));
	TestTrue(TEXT("one negative axis flips"), FMantleMirrorBake::FlipsWinding(FVector(-1.0, 1.0, 1.0)));
	TestTrue(TEXT("one negative axis flips (Z)"), FMantleMirrorBake::FlipsWinding(FVector(1.0, 1.0, -3.0)));
	TestFalse(TEXT("two negative axes do not"), FMantleMirrorBake::FlipsWinding(FVector(-1.0, -1.0, 1.0)));
	TestFalse(TEXT("two negative axes do not (YZ)"), FMantleMirrorBake::FlipsWinding(FVector(1.0, -2.0, -2.0)));
	TestTrue(TEXT("three negative axes flip"), FMantleMirrorBake::FlipsWinding(FVector(-1.0, -1.0, -1.0)));

	FMirrorFixture Fixture;
	if (!TestTrue(TEXT("the fixture builds"), Fixture.IsReady()))
	{
		return false;
	}
	FReadMesh Source;
	if (!TestTrue(TEXT("the source mesh reads"), Source.Read(*Fixture.Offset)))
	{
		return false;
	}
	const FVector SourceMiddle = Source.Bounds.GetCenter();
	const double SourceSide = Source.Orientation(0, SourceMiddle);
	if (!TestTrue(TEXT("the source's triangles have a side"), FMath::Abs(SourceSide) > 1.0))
	{
		return false;
	}
	for (int32 Triangle = 0; Triangle < Source.NumTriangles(); ++Triangle)
	{
		if (Source.Orientation(Triangle, SourceMiddle) * SourceSide <= 0.0)
		{
			AddError(TEXT("the source cube's triangles do not all wind the same way round its middle"));
			return false;
		}
	}

	// The copy of every pattern winds the same way round its own middle as the source does: a mirror that left the
	// winding as it was would show every triangle facing in for an odd number of axes (and only those).
	for (const FVector& Pattern : Patterns())
	{
		FVector Positive;
		FString Why;
		UStaticMesh* Mirrored = FMantleMirrorBake::MirroredCopy(Fixture.Offset.Get(), Pattern, Positive, Why);
		FReadMesh Copy;
		if (!TestNotNull(*FString::Printf(TEXT("a copy for %s (%s)"), *Pattern.ToString(), *Why), Mirrored) || !TestTrue(TEXT("it reads"), Copy.Read(*Mirrored)))
		{
			continue;
		}
		const FVector Middle = Copy.Bounds.GetCenter();
		int32 Inside = 0;
		for (int32 Triangle = 0; Triangle < Copy.NumTriangles(); ++Triangle)
		{
			if (Copy.Orientation(Triangle, Middle) * SourceSide <= 0.0)
			{
				++Inside;
			}
		}
		TestEqual(FString::Printf(TEXT("%s (%s): triangles that wind against the source's"), *Pattern.ToString(),
			FMantleMirrorBake::FlipsWinding(Pattern) ? TEXT("winding flipped") : TEXT("winding kept")), Inside, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMirrorBakeNormalsTest,
	"Mantle.Editor.MirrorBake.NormalsPointOutwardAndBasisIsMirrored",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMirrorBakeNormalsTest::RunTest(const FString& Parameters)
{
	using namespace MantleMirrorBakeTestPrivate;
	FMirrorFixture Fixture;
	if (!TestTrue(TEXT("the fixture builds"), Fixture.IsReady()))
	{
		return false;
	}
	FReadMesh Source;
	if (!TestTrue(TEXT("the source mesh reads"), Source.Read(*Fixture.Offset)))
	{
		return false;
	}

	for (const FVector& Pattern : Patterns())
	{
		FVector Positive;
		FString Why;
		UStaticMesh* Mirrored = FMantleMirrorBake::MirroredCopy(Fixture.Offset.Get(), Pattern, Positive, Why);
		FReadMesh Copy;
		if (!TestNotNull(*FString::Printf(TEXT("a copy for %s (%s)"), *Pattern.ToString(), *Why), Mirrored) || !TestTrue(TEXT("it reads"), Copy.Read(*Mirrored)))
		{
			continue;
		}
		const FVector Middle = Copy.Bounds.GetCenter();
		const FVector Signs = SignsOf(Pattern);
		const double Determinant = Signs.X * Signs.Y * Signs.Z;

		// Every vertex's normal points away from the middle of the cube (a face's normal does, on a cube).
		int32 Inward = 0;
		for (int32 Vertex = 0; Vertex < Copy.Positions.Num(); ++Vertex)
		{
			if (FVector::DotProduct(Copy.Normals[Vertex], Copy.Positions[Vertex] - Middle) <= 0.0)
			{
				++Inward;
			}
		}
		TestEqual(FString::Printf(TEXT("%s: vertices whose normal points in"), *Pattern.ToString()), Inward, 0);

		// Each source vertex has its mirror image in the copy (same place, normal, tangent and UVs), with the binormal sign
		// turned over when an odd number of axes is mirrored.
		int32 Unmatched = 0;
		int32 WrongUVs = 0;
		int32 WrongBasis = 0;
		for (int32 Vertex = 0; Vertex < Source.Positions.Num(); ++Vertex)
		{
			const FVector Position = Source.Positions[Vertex] * Signs;
			const FVector Normal = Source.Normals[Vertex] * Signs;
			int32 Match = INDEX_NONE;
			for (int32 Candidate = 0; Candidate < Copy.Positions.Num() && Match == INDEX_NONE; ++Candidate)
			{
				if (Copy.Positions[Candidate].Equals(Position, 0.1) && Copy.Normals[Candidate].Equals(Normal, 0.02)
					&& Copy.UV0s[Candidate].Equals(Source.UV0s[Vertex], 0.01))
				{
					Match = Candidate;
				}
			}
			if (Match == INDEX_NONE)
			{
				++Unmatched;
				continue;
			}
			if (Copy.NumTexCoords != Source.NumTexCoords || !Copy.UV1s[Match].Equals(Source.UV1s[Vertex], 0.01))
			{
				++WrongUVs;
			}
			if (!Copy.TangentsX[Match].Equals(Source.TangentsX[Vertex] * Signs, 0.02) || Copy.BinormalSigns[Match] != Source.BinormalSigns[Vertex] * static_cast<float>(Determinant))
			{
				++WrongBasis;
			}
		}
		TestEqual(FString::Printf(TEXT("%s: source vertices with no mirror image (position, normal, UV0)"), *Pattern.ToString()), Unmatched, 0);
		TestEqual(FString::Printf(TEXT("%s: vertices with other UVs"), *Pattern.ToString()), WrongUVs, 0);
		TestEqual(FString::Printf(TEXT("%s: vertices whose tangent or binormal sign is not the source's, mirrored"), *Pattern.ToString()), WrongBasis, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMirrorBakeCacheTest,
	"Mantle.Editor.MirrorBake.TheSameSourceAndSignsShareACopy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMirrorBakeCacheTest::RunTest(const FString& Parameters)
{
	using namespace MantleMirrorBakeTestPrivate;
	FMirrorFixture Fixture;
	if (!TestTrue(TEXT("the fixture builds"), Fixture.IsReady()))
	{
		return false;
	}

	FVector Positive;
	FString Why;
	UStaticMesh* First = FMantleMirrorBake::MirroredCopy(Fixture.Offset.Get(), FVector(-1.0, 1.0, 1.0), Positive, Why);
	UStaticMesh* Second = FMantleMirrorBake::MirroredCopy(Fixture.Offset.Get(), FVector(-1.0, 1.0, 1.0), Positive, Why);
	if (!TestNotNull(TEXT("a copy"), First))
	{
		return false;
	}
	TestTrue(TEXT("the same source and signs return the same copy"), First == Second);
	TestEqual(TEXT("and so the same mesh key"), FMantlePlaceAssets::MeshKey(First), FMantlePlaceAssets::MeshKey(Second));

	// Ten instances, whatever their other scales, share it: only which axes are negative picks the copy.
	for (int32 Instance = 0; Instance < 10; ++Instance)
	{
		FVector Scale(-(1.0 + Instance * 0.25), 1.0 + Instance, 0.5 + Instance * 0.1);
		FVector Instanced;
		UStaticMesh* Copy = FMantleMirrorBake::MirroredCopy(Fixture.Offset.Get(), Scale, Instanced, Why);
		TestTrue(*FString::Printf(TEXT("instance %d at %s shares the copy"), Instance, *Scale.ToString()), Copy == First);
		TestTrue(*FString::Printf(TEXT("instance %d is placed at abs(scale)"), Instance), Instanced.Equals(Scale.GetAbs()));
	}

	// Another sign pattern, or another source, is another copy.
	UStaticMesh* OnY = FMantleMirrorBake::MirroredCopy(Fixture.Offset.Get(), FVector(1.0, -1.0, 1.0), Positive, Why);
	UStaticMesh* OtherSource = FMantleMirrorBake::MirroredCopy(Fixture.Cube.Get(), FVector(-1.0, 1.0, 1.0), Positive, Why);
	TestTrue(TEXT("another pattern is another copy"), OnY && OnY != First);
	TestTrue(TEXT("another source is another copy"), OtherSource && OtherSource != First);
	if (OnY && OtherSource)
	{
		TestTrue(TEXT("with its own mesh key"), FMantlePlaceAssets::MeshKey(OnY) != FMantlePlaceAssets::MeshKey(First)
			&& FMantlePlaceAssets::MeshKey(OtherSource) != FMantlePlaceAssets::MeshKey(First));
	}

	// Forgetting the cache makes a new one (a fixed or changed source is mirrored again).
	FMantleMirrorBake::ClearCache();
	UStaticMesh* Again = FMantleMirrorBake::MirroredCopy(Fixture.Offset.Get(), FVector(-1.0, 1.0, 1.0), Positive, Why);
	TestTrue(TEXT("a cleared cache mirrors again"), Again && Again != First);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMirrorBakeNoMirrorTest,
	"Mantle.Editor.MirrorBake.AnAllPositiveScaleReturnsNull",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMirrorBakeNoMirrorTest::RunTest(const FString& Parameters)
{
	using namespace MantleMirrorBakeTestPrivate;
	FMirrorFixture Fixture;
	if (!TestTrue(TEXT("the fixture builds"), Fixture.IsReady()))
	{
		return false;
	}

	const FVector Scale(1.0, 2.0, 3.0);
	FVector Positive = FVector::ZeroVector;
	FString Why;
	TestNull(TEXT("an all-positive scale returns no copy"), FMantleMirrorBake::MirroredCopy(Fixture.Offset.Get(), Scale, Positive, Why));
	TestFalse(TEXT("and says why"), Why.IsEmpty());
	TestTrue(TEXT("the positive scale is still given"), Positive.Equals(Scale));

	// A zero axis is flat, not mirrored.
	Why.Reset();
	TestNull(TEXT("a zero axis is not a mirror"), FMantleMirrorBake::MirroredCopy(Fixture.Offset.Get(), FVector(0.0, 1.0, 1.0), Positive, Why));
	TestFalse(TEXT("it says why"), Why.IsEmpty());

	// No mesh: nothing to mirror, said.
	Why.Reset();
	TestNull(TEXT("no mesh returns no copy"), FMantleMirrorBake::MirroredCopy(nullptr, FVector(-1.0, 1.0, 1.0), Positive, Why));
	TestFalse(TEXT("and says why"), Why.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMirrorBakeKeepsTest,
	"Mantle.Editor.MirrorBake.KeepsSlotsAndCollision",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMirrorBakeKeepsTest::RunTest(const FString& Parameters)
{
	using namespace MantleMirrorBakeTestPrivate;
	FMirrorFixture Fixture;
	if (!TestTrue(TEXT("the fixture builds"), Fixture.IsReady()))
	{
		return false;
	}

	FVector Positive;
	FString Why;
	UStaticMesh* Mirrored = FMantleMirrorBake::MirroredCopy(Fixture.Offset.Get(), FVector(-1.0, 1.0, 1.0), Positive, Why);
	if (!TestNotNull(*FString::Printf(TEXT("a copy mirrored on X (%s)"), *Why), Mirrored))
	{
		return false;
	}

	// The slots: the same count, in order, with the same materials, and the sections draw with the same slots.
	const TArray<FStaticMaterial>& SourceSlots = Fixture.Offset->GetStaticMaterials();
	const TArray<FStaticMaterial>& CopySlots = Mirrored->GetStaticMaterials();
	if (TestEqual(TEXT("the copy has the source's material slots"), CopySlots.Num(), SourceSlots.Num()))
	{
		for (int32 Slot = 0; Slot < SourceSlots.Num(); ++Slot)
		{
			TestEqual(*FString::Printf(TEXT("slot %d keeps its name"), Slot), CopySlots[Slot].MaterialSlotName.ToString(), SourceSlots[Slot].MaterialSlotName.ToString());
			TestTrue(*FString::Printf(TEXT("slot %d keeps its material"), Slot), CopySlots[Slot].MaterialInterface == SourceSlots[Slot].MaterialInterface);
		}
	}
	TestTrue(TEXT("the sections draw with the same slots"), FMantlePlaceAssets::UploadSlots(Mirrored) == FMantlePlaceAssets::UploadSlots(Fixture.Offset.Get()));
	TestEqual(TEXT("and the lightmap UVs are kept as a UV set"), static_cast<int32>(Mirrored->GetRenderData()->LODResources[0].VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords()),
		static_cast<int32>(Fixture.Offset->GetRenderData()->LODResources[0].VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords()));

	// The collision: the box's centre mirrored on X, its turn mirrored (yaw 30 becomes yaw -30), the same complexity.
	const UBodySetup* Body = Mirrored->GetBodySetup();
	if (!TestNotNull(TEXT("the copy has a body setup"), Body))
	{
		return false;
	}
	TestTrue(TEXT("with the source's collision complexity"), Body->GetCollisionTraceFlag() == CTF_UseSimpleAndComplex);
	if (TestEqual(TEXT("and the source's one box"), Body->AggGeom.BoxElems.Num(), 1))
	{
		const FKBoxElem& Box = Body->AggGeom.BoxElems[0];
		TestTrue(FString::Printf(TEXT("the box's centre is mirrored on X (%s)"), *Box.Center.ToString()), Box.Center.Equals(FVector(-BoxCenter.X, BoxCenter.Y, BoxCenter.Z), 0.01));
		TestTrue(FString::Printf(TEXT("the box's rotation is mirrored (%s)"), *Box.Rotation.ToString()),
			FMath::Abs(Box.Rotation.Quaternion() | FRotator(0.0, -30.0, 0.0).Quaternion()) > 0.9999);
		TestTrue(TEXT("and its size is kept"), FMath::IsNearlyEqual(Box.X, 100.0f) && FMath::IsNearlyEqual(Box.Y, 100.0f) && FMath::IsNearlyEqual(Box.Z, 100.0f));
	}

	// Mirroring collision on its own: every shape and every axis.
	UBodySetup* Source = NewObject<UBodySetup>(GetTransientPackage());
	Source->CollisionTraceFlag = CTF_UseSimpleAsComplex;
	FKSphereElem Sphere;
	Sphere.Center = FVector(10.0, 20.0, 30.0);
	Sphere.Radius = 5.0f;
	Source->AggGeom.SphereElems.Add(Sphere);
	FKSphylElem Capsule;
	Capsule.Center = FVector(10.0, 20.0, 30.0);
	Capsule.Rotation = FRotator(20.0, 30.0, 40.0);
	Capsule.Radius = 5.0f;
	Capsule.Length = 20.0f;
	Source->AggGeom.SphylElems.Add(Capsule);
	FKConvexElem Hull;
	Hull.VertexData = { FVector(0.0, 0.0, 0.0), FVector(10.0, 0.0, 0.0), FVector(0.0, 10.0, 0.0), FVector(0.0, 0.0, 10.0) };
	Hull.SetTransform(FTransform(FVector(100.0, 0.0, 0.0)));
	Source->AggGeom.ConvexElems.Add(Hull);

	UBodySetup* Target = NewObject<UBodySetup>(GetTransientPackage());
	FMantleMirrorBake::MirrorCollision(*Source, *Target, FVector(-1.0, -1.0, 1.0));
	TestTrue(TEXT("a body's complexity is kept"), Target->CollisionTraceFlag == CTF_UseSimpleAsComplex);
	if (TestEqual(TEXT("its sphere"), Target->AggGeom.SphereElems.Num(), 1))
	{
		TestTrue(TEXT("is mirrored on X and Y"), Target->AggGeom.SphereElems[0].Center.Equals(FVector(-10.0, -20.0, 30.0), 0.01));
	}
	if (TestEqual(TEXT("its capsule"), Target->AggGeom.SphylElems.Num(), 1))
	{
		TestTrue(TEXT("is mirrored on X and Y"), Target->AggGeom.SphylElems[0].Center.Equals(FVector(-10.0, -20.0, 30.0), 0.01));
		// Mirroring a rotation R by M is M R M (a reflection conjugates it), worked out here with matrices.
		const FVector Signs(-1.0, -1.0, 1.0);
		const FMatrix Conjugated = FScaleMatrix(Signs) * FRotationMatrix(Capsule.Rotation) * FScaleMatrix(Signs);
		const FQuat Expected(Conjugated);
		TestTrue(FString::Printf(TEXT("and its rotation is the reflected one (%s)"), *Target->AggGeom.SphylElems[0].Rotation.ToString()),
			FMath::Abs(Target->AggGeom.SphylElems[0].Rotation.Quaternion() | Expected) > 0.9999);
	}
	if (TestEqual(TEXT("its hull"), Target->AggGeom.ConvexElems.Num(), 1))
	{
		const FKConvexElem& MirroredHull = Target->AggGeom.ConvexElems[0];
		TestTrue(TEXT("has its own transform applied, then mirrored"), MirroredHull.GetTransform().Equals(FTransform::Identity)
			&& MirroredHull.VertexData.Num() == 4 && MirroredHull.VertexData[1].Equals(FVector(-110.0, 0.0, 0.0), 0.01)
			&& MirroredHull.VertexData[2].Equals(FVector(-100.0, -10.0, 0.0), 0.01));
	}
	return true;
}

#endif
