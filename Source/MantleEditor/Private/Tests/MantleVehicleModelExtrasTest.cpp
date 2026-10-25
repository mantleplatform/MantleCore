// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSkinnedMesh.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehicleChassisMesh.h"
#include "MantleVehicleChassisSkin.h"

#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Misc/AutomationTest.h"
#include "Rendering/ColorVertexBuffer.h"
#include "Rendering/PositionVertexBuffer.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/StaticMeshVertexBuffer.h"
#include "StaticMeshAttributes.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

// What a creator's model keeps when it becomes a vehicle's body: its own tangents, its UV channels above the first and its painted vertex colours, up to the
// skeletal mesh's limit; and a Nanite mesh is read from its full-detail source and not its reduced fallback. The model is a 6 x 6 grid made in the test, with two UV
// channels, colours that differ at every corner, and a tangent that is not the one a build would work out from the UVs, so a kept tangent can tell from a recomputed one.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleModelExtrasTestPrivate
{
	constexpr int32 Cells = 6;
	constexpr float Spacing = 100.0f;

	/** The colour the grid's corner (X, Y) has, as a vertex colour the static build keeps. */
	FColor ColourOf(const int32 X, const int32 Y)
	{
		return FLinearColor(static_cast<float>(X) / Cells, static_cast<float>(Y) / Cells, 0.25f, 1.0f).ToFColor(true);
	}

	/** A 6 x 6 grid of quads in the XY plane, a static mesh with its geometry kept in memory, two UV channels, colours and tangents along +Y. 72 triangles. */
	UStaticMesh* MakeGridMesh()
	{
		UObject* Outer = GetTransientPackage();
		UStaticMesh* Mesh = NewObject<UStaticMesh>(Outer, MakeUniqueObjectName(Outer, UStaticMesh::StaticClass(), FName(TEXT("MantleExtrasTestGrid"))), RF_Transient);
		Mesh->GetStaticMaterials().Add(FStaticMaterial(nullptr, FName(TEXT("Hull")), FName(TEXT("Hull"))));
		Mesh->bAllowCPUAccess = true;
		Mesh->SetNumSourceModels(1);
		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> BinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		TPolygonGroupAttributesRef<FName> GroupNames = Attributes.GetPolygonGroupMaterialSlotNames();
		UVs.SetNumChannels(2);
		const FPolygonGroupID Group = Description.CreatePolygonGroup();
		GroupNames[Group] = FName(TEXT("Hull"));
		TArray<FVertexInstanceID> Instances;
		for (int32 Y = 0; Y <= Cells; ++Y)
		{
			for (int32 X = 0; X <= Cells; ++X)
			{
				const FVertexID Vertex = Description.CreateVertex();
				Positions[Vertex] = FVector3f(Spacing * X, Spacing * Y, 0.0f);
				const FVertexInstanceID Instance = Description.CreateVertexInstance(Vertex);
				Normals[Instance] = FVector3f(0.0f, 0.0f, 1.0f);
				Tangents[Instance] = FVector3f(0.0f, 1.0f, 0.0f);
				BinormalSigns[Instance] = 1.0f;
				UVs.Set(Instance, 0, FVector2f(static_cast<float>(X) / Cells, static_cast<float>(Y) / Cells));
				UVs.Set(Instance, 1, FVector2f(static_cast<float>(Y) / Cells, static_cast<float>(X) / Cells));
				const FLinearColor Linear = FLinearColor(static_cast<float>(X) / Cells, static_cast<float>(Y) / Cells, 0.25f, 1.0f);
				Colors[Instance] = FVector4f(Linear.R, Linear.G, Linear.B, Linear.A);
				Instances.Add(Instance);
			}
		}
		for (int32 Y = 0; Y < Cells; ++Y)
		{
			for (int32 X = 0; X < Cells; ++X)
			{
				const int32 A = Y * (Cells + 1) + X;
				const int32 B = A + 1;
				const int32 C = A + Cells + 2;
				const int32 D = A + Cells + 1;
				const FVertexInstanceID First[3] = { Instances[A], Instances[B], Instances[C] };
				const FVertexInstanceID Second[3] = { Instances[A], Instances[C], Instances[D] };
				Description.CreateTriangle(Group, TArrayView<const FVertexInstanceID>(First, 3));
				Description.CreateTriangle(Group, TArrayView<const FVertexInstanceID>(Second, 3));
			}
		}
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
		TArray<const FMeshDescription*> Pointers = { &Description };
		Mesh->BuildFromMeshDescriptions(Pointers, Params);
		return Mesh;
	}

	/** The corner (X, Y) of the grid a vertex at Position (before any placement) is, or false when it is not one. */
	bool CornerOf(const FVector3f& Position, int32& OutX, int32& OutY)
	{
		OutX = FMath::RoundToInt(Position.X / Spacing);
		OutY = FMath::RoundToInt(Position.Y / Spacing);
		return OutX >= 0 && OutX <= Cells && OutY >= 0 && OutY <= Cells && FMath::Abs(Position.X - Spacing * OutX) < 0.05f && FMath::Abs(Position.Y - Spacing * OutY) < 0.05f && FMath::Abs(Position.Z) < 0.05f;
	}

	bool NearUV(const FVector2f& A, const FVector2f& B)
	{
		return FMath::Abs(A.X - B.X) < 0.01f && FMath::Abs(A.Y - B.Y) < 0.01f;
	}

	void DiscardMesh(USkeletalMesh* Mesh)
	{
		if (!Mesh)
		{
			return;
		}
		if (USkeleton* Skeleton = Mesh->GetSkeleton())
		{
			Skeleton->ClearFlags(RF_Public | RF_Standalone);
			Skeleton->MarkAsGarbage();
		}
		Mesh->ClearFlags(RF_Public | RF_Standalone);
		Mesh->MarkAsGarbage();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleModelExtrasReadTest,
	"Mantle.Vehicle.ChassisSkin.ModelKeepsUVsColoursAndTangents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleModelExtrasReadTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleModelExtrasTestPrivate;
	const TStrongObjectPtr<UStaticMesh> Grid(MakeGridMesh());
	if (!TestTrue(TEXT("the grid keeps its geometry"), Grid.IsValid() && Grid->GetRenderData() && Grid->GetRenderData()->LODResources.Num() > 0))
	{
		return false;
	}
	FMantleVehicleModel Model;
	FString Error;
	if (!TestTrue(*FString::Printf(TEXT("the grid reads as a model (%s)"), *Error), FMantleVehicleModel::Read(Grid.Get(), Model, Error)))
	{
		return false;
	}
	const int32 NumVertices = Model.LOD.Positions.Num();
	TestEqual(TEXT("72 triangles"), Model.NumTriangles(), 72);

	// Read from the render data: the second UV channel, every colour and the creator's own tangent, each on the vertex they belong to.
	if (TestEqual(TEXT("one UV channel above the first"), Model.ExtraUVs.Num(), 1) && TestEqual(TEXT("with an entry for every vertex"), Model.ExtraUVs[0].Num(), NumVertices)
		&& TestEqual(TEXT("and a colour for every vertex"), Model.Colors.Num(), NumVertices) && TestEqual(TEXT("and a tangent for every vertex"), Model.LOD.Tangents.Num(), NumVertices))
	{
		int32 Wrong = 0;
		for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
		{
			int32 X = 0;
			int32 Y = 0;
			if (!CornerOf(Model.LOD.Positions[Vertex], X, Y))
			{
				++Wrong;
				continue;
			}
			const FVector4f& Tangent = Model.LOD.Tangents[Vertex];
			const bool bUV = NearUV(Model.ExtraUVs[0][Vertex], FVector2f(static_cast<float>(Y) / Cells, static_cast<float>(X) / Cells));
			const bool bColour = Model.Colors[Vertex] == ColourOf(X, Y);
			const bool bTangent = Tangent.Y > 0.99f && FMath::Abs(Tangent.X) < 0.05f;
			Wrong += bUV && bColour && bTangent ? 0 : 1;
		}
		TestEqual(TEXT("each vertex has its own second UV, colour and the creator's tangent"), Wrong, 0);
	}
	TestTrue(TEXT("a mesh that is not Nanite says nothing about it"), Model.Notes.IsEmpty());

	// Worn on a chassis and made into the vehicle's body asset, they reach it: both UV channels, the colours, the tangents, and the build keeps the tangents.
	FMantleChassisSpec Spec;
	FMantleVehicleAsset Grey;
	if (!TestTrue(*FString::Printf(TEXT("the sedan chassis loads (%s)"), *Error), FMantleVehicleChassisBuilder::LoadSpec(TEXT("sedan"), Spec, Error) && FMantleVehicleChassisBuilder::MakeAsset(Spec, Grey, Error)))
	{
		return false;
	}
	const TArray<FMantleVehiclePartSlot> Slots = FMantleVehicleChassisSkin::PartSlots(Spec, Grey.Body);
	FMantleVehicleModelPlacement Placement;
	Placement.Scale = 0.5;
	Placement.Offset = FVector(-150.0, -150.0, 20.0);
	FMantleVehicleSkinned Skinned;
	if (!TestTrue(*FString::Printf(TEXT("the grid is put on the chassis (%s)"), *Error), FMantleVehicleChassisSkin::Skin(Grey.Body, Slots, Model, Placement, FMantleVehicleModelMap(), Skinned, Error)))
	{
		return false;
	}
	TestEqual(TEXT("the skin carries the UV channel along"), Skinned.Extras.UVs.Num(), 1);
	TestEqual(TEXT("and the colours, one for each vertex of the body"), Skinned.Extras.Colors.Num(), Skinned.Body.LODs[0].Positions.Num());

	const FString Folder = TEXT("/Temp/MantleVehicleExtrasTest");
	USkeletalMesh* Kept = FMantleVehicleChassisMesh::Create(Skinned.Body, Folder, TEXT("GridKept"), Error, &Skinned.Materials, &Skinned.Extras);
	if (TestNotNull(*FString::Printf(TEXT("the body is made (%s)"), *Error), Kept))
	{
		const FSkeletalMeshRenderData* RenderData = Kept->GetResourceForRendering();
		if (TestTrue(TEXT("with render data"), RenderData && RenderData->LODRenderData.Num() > 0))
		{
			const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[0];
			const FStaticMeshVertexBuffer& Vertices = LOD.StaticVertexBuffers.StaticMeshVertexBuffer;
			const FPositionVertexBuffer& Positions = LOD.StaticVertexBuffers.PositionVertexBuffer;
			const FColorVertexBuffer& Colors = LOD.StaticVertexBuffers.ColorVertexBuffer;
			TestEqual(TEXT("the body has both UV channels"), static_cast<int32>(Vertices.GetNumTexCoords()), 2);
			TestEqual(TEXT("and a colour for every vertex"), static_cast<int32>(Colors.GetNumVertices()), static_cast<int32>(Positions.GetNumVertices()));
			int32 Wrong = 0;
			int32 Matched = 0;
			for (uint32 Vertex = 0; Vertex < Positions.GetNumVertices(); ++Vertex)
			{
				// Back to the grid's corner: the body's vertex is the corner scaled by the placement.
				const FVector3f Position = Positions.VertexPosition(Vertex);
				const FVector3f Original = (Position - FVector3f(Placement.Offset)) / static_cast<float>(Placement.Scale);
				int32 X = 0;
				int32 Y = 0;
				if (!CornerOf(Original, X, Y))
				{
					++Wrong;
					continue;
				}
				++Matched;
				const FVector4f Tangent = Vertices.VertexTangentX(Vertex);
				const bool bUV1 = NearUV(Vertices.GetVertexUV(Vertex, 1), FVector2f(static_cast<float>(Y) / Cells, static_cast<float>(X) / Cells));
				const bool bColour = Colors.VertexColor(Vertex) == ColourOf(X, Y);
				const bool bTangent = Tangent.Y > 0.9f && FMath::Abs(Tangent.X) < 0.2f;
				Wrong += bUV1 && bColour && bTangent ? 0 : 1;
			}
			TestTrue(*FString::Printf(TEXT("every vertex of the body is a corner of the grid (%d of %d)"), Matched, static_cast<int32>(Positions.GetNumVertices())), Matched == static_cast<int32>(Positions.GetNumVertices()));
			TestEqual(TEXT("each keeps its second UV, its colour and the creator's tangent"), Wrong, 0);
		}
		const FSkeletalMeshLODInfo* Info = Kept->GetLODInfo(0);
		TestTrue(TEXT("the build does not recompute the tangents it was given"), Info && !Info->BuildSettings.bRecomputeTangents);
	}

	// Positive control: with the tangents taken away the build works them out from the UVs, along the first UV's U (+X on this grid), not along +Y.
	FMantleVehicleSkinned Bare = Skinned;
	Bare.Body.LODs[0].Tangents.Reset();
	USkeletalMesh* Worked = FMantleVehicleChassisMesh::Create(Bare.Body, Folder, TEXT("GridWorkedOut"), Error, &Bare.Materials, &Bare.Extras);
	if (TestNotNull(*FString::Printf(TEXT("the same body without tangents is made (%s)"), *Error), Worked))
	{
		const FSkeletalMeshRenderData* RenderData = Worked->GetResourceForRendering();
		if (RenderData && RenderData->LODRenderData.Num() > 0)
		{
			const FStaticMeshVertexBuffer& Vertices = RenderData->LODRenderData[0].StaticVertexBuffers.StaticMeshVertexBuffer;
			int32 AlongY = 0;
			for (uint32 Vertex = 0; Vertex < Vertices.GetNumVertices(); ++Vertex)
			{
				AlongY += FMath::Abs(Vertices.VertexTangentX(Vertex).Y) > 0.9f ? 1 : 0;
			}
			TestEqual(TEXT("none of its tangents is the one the creator had (+Y): they were worked out"), AlongY, 0);
		}
		const FSkeletalMeshLODInfo* Info = Worked->GetLODInfo(0);
		TestTrue(TEXT("and the build was told to recompute them"), Info && Info->BuildSettings.bRecomputeTangents);
	}

	// Negative control: with no extras the body has one UV channel, as it always had.
	USkeletalMesh* Plain = FMantleVehicleChassisMesh::Create(Skinned.Body, Folder, TEXT("GridPlain"), Error, &Skinned.Materials, nullptr);
	if (TestNotNull(*FString::Printf(TEXT("the body without extras is made (%s)"), *Error), Plain))
	{
		const FSkeletalMeshRenderData* RenderData = Plain->GetResourceForRendering();
		if (RenderData && RenderData->LODRenderData.Num() > 0)
		{
			TestEqual(TEXT("it has one UV channel"), static_cast<int32>(RenderData->LODRenderData[0].StaticVertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords()), 1);
		}
	}
	DiscardMesh(Kept);
	DiscardMesh(Worked);
	DiscardMesh(Plain);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleModelNaniteTest,
	"Mantle.Vehicle.ChassisSkin.NaniteMeshReadsItsSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleModelNaniteTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleModelExtrasTestPrivate;
	// The choice, as arithmetic: a Nanite mesh's full-detail source is read when it is kept and fits a vehicle, and the build says which it read and why.
	{
		FString Note;
		TestFalse(TEXT("a mesh without Nanite is read from its render data"), FMantleVehicleModel::UseNaniteSource(false, true, 100, 100, &Note));
		TestTrue(TEXT("and says nothing"), Note.IsEmpty());
		TestTrue(TEXT("a Nanite mesh with its source is read from the source"), FMantleVehicleModel::UseNaniteSource(true, true, 5000, 800, &Note));
		TestTrue(TEXT("and says so, with both counts"), Note.Contains(TEXT("full-detail source")) && Note.Contains(TEXT("5000")) && Note.Contains(TEXT("800")));
		TestFalse(TEXT("a Nanite mesh whose source is not kept falls back"), FMantleVehicleModel::UseNaniteSource(true, false, 0, 800, &Note));
		TestTrue(TEXT("and says why"), Note.Contains(TEXT("fallback")) && Note.Contains(TEXT("source isn't kept")));
		TestFalse(TEXT("a Nanite mesh with more triangles than a vehicle may carry falls back"), FMantleVehicleModel::UseNaniteSource(true, true, FMantleVehicleLimits::MaxTriangles + 1, 800, &Note));
		TestTrue(TEXT("and says why"), Note.Contains(TEXT("fallback")) && Note.Contains(TEXT("more than a vehicle can carry")));
		TestTrue(TEXT("a source of exactly the cap is read"), FMantleVehicleModel::UseNaniteSource(true, true, FMantleVehicleLimits::MaxTriangles, 800, nullptr));
	}

	// The branch, on a real mesh. The grid is built (its render data is the 72 triangles at x 0 to 600), then its kept source description is moved 1000 cm along +X, so
	// which of the two a read took shows in where the model is. The Nanite switch is flipped after the build: the choice is the setting's, and no Nanite data is needed.
	const TStrongObjectPtr<UStaticMesh> Grid(MakeGridMesh());
	if (!TestTrue(TEXT("the grid keeps its geometry"), Grid.IsValid() && Grid->GetRenderData() && Grid->GetRenderData()->LODResources.Num() > 0))
	{
		return false;
	}
	FMeshDescription Moved;
	if (!TestTrue(TEXT("the grid keeps its source description"), Grid->IsMeshDescriptionValid(0) && Grid->CloneMeshDescription(0, Moved)))
	{
		return false;
	}
	{
		FStaticMeshAttributes Attributes(Moved);
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		for (const FVertexID Vertex : Moved.Vertices().GetElementIDs())
		{
			Positions[Vertex] += FVector3f(1000.0f, 0.0f, 0.0f);
		}
	}
	// Committed to the source only: the render data is not rebuilt until the mesh is built again.
	Grid->CreateMeshDescription(0, MoveTemp(Moved));
	Grid->CommitMeshDescription(0);

	FMantleVehicleModel Render;
	FString Error;
	if (TestTrue(*FString::Printf(TEXT("without Nanite the grid reads (%s)"), *Error), FMantleVehicleModel::Read(Grid.Get(), Render, Error)))
	{
		TestTrue(*FString::Printf(TEXT("from its render data, at x %.0f"), Render.Bounds().GetCenter().X), Render.Bounds().GetCenter().X < 400.0);
		TestTrue(TEXT("with no note"), Render.Notes.IsEmpty());
	}

	Grid->GetNaniteSettings().bEnabled = true;
	FMantleVehicleModel FromSource;
	if (TestTrue(*FString::Printf(TEXT("with Nanite the grid reads (%s)"), *Error), FMantleVehicleModel::Read(Grid.Get(), FromSource, Error)))
	{
		TestTrue(*FString::Printf(TEXT("from its full-detail source, at x %.0f"), FromSource.Bounds().GetCenter().X), FromSource.Bounds().GetCenter().X > 1200.0);
		TestEqual(TEXT("all 72 triangles"), FromSource.NumTriangles(), 72);
		TestEqual(TEXT("the second UV channel, the colours and the tangents come with it"), FromSource.ExtraUVs.Num() + (FromSource.Colors.Num() == FromSource.LOD.Positions.Num() ? 1 : 0)
			+ (FromSource.LOD.Tangents.Num() == FromSource.LOD.Positions.Num() ? 1 : 0), 3);
		if (FromSource.ExtraUVs.Num() == 1 && FromSource.Colors.Num() == FromSource.LOD.Positions.Num())
		{
			int32 Wrong = 0;
			for (int32 Vertex = 0; Vertex < FromSource.LOD.Positions.Num(); ++Vertex)
			{
				int32 X = 0;
				int32 Y = 0;
				const FVector3f Back = FromSource.LOD.Positions[Vertex] - FVector3f(1000.0f, 0.0f, 0.0f);
				const bool bCorner = CornerOf(Back, X, Y);
				Wrong += bCorner && NearUV(FromSource.ExtraUVs[0][Vertex], FVector2f(static_cast<float>(Y) / Cells, static_cast<float>(X) / Cells)) && FromSource.Colors[Vertex] == ColourOf(X, Y) ? 0 : 1;
			}
			TestEqual(TEXT("each vertex has its own second UV and colour"), Wrong, 0);
		}
		TestTrue(TEXT("the build is told it was read from the source"), FromSource.Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("full-detail source")); }));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
