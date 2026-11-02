// Copyright Joshua Eger. All Rights Reserved.
#include "MantleMeshReduce.h"
#include "MantleSkinnedMesh.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleBodyReduce.h"
#include "MantleVehicleBodySplit.h"
#include "Engine/StaticMesh.h"
#include "Misc/AutomationTest.h"
#include "StaticMeshResources.h"

#if WITH_DEV_AUTOMATION_TESTS

// A vehicle's attached parts (the pieces its body is split into) carry far-away versions of themselves (LOD1 and LOD2 at half and a quarter of their triangles), and the
// whole car (body and parts together) is held to the vehicle's triangle limit near.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehiclePartLODsTestPrivate
{
	/** Adds a flat N x N grid (2 N^2 triangles) with a gentle bump, drawn as Material, as one more section of Piece. */
	void AddPieceGrid(FMantleVehicleBodyPiece& Piece, const int32 N, const int32 Material, const FVector3f& Offset)
	{
		FMantleVehicleBodyPiece::FSection& Section = Piece.Sections.AddDefaulted_GetRef();
		Section.FirstIndex = Piece.Indices.Num();
		Section.Material = Material;
		const uint32 First = static_cast<uint32>(Piece.Positions.Num());
		for (int32 Y = 0; Y <= N; ++Y)
		{
			for (int32 X = 0; X <= N; ++X)
			{
				Piece.Positions.Add(Offset + FVector3f(X * 5.0f, Y * 5.0f, 4.0f * FMath::Sin(X * 0.4f) * FMath::Cos(Y * 0.3f)));
				Piece.Normals.Add(FVector3f::UpVector);
				Piece.UVs.Add(FVector2f(static_cast<float>(X) / N, static_cast<float>(Y) / N));
			}
		}
		for (int32 Y = 0; Y < N; ++Y)
		{
			for (int32 X = 0; X < N; ++X)
			{
				const uint32 A = First + Y * (N + 1) + X, B = A + 1, C = A + N + 1, D = C + 1;
				Piece.Indices.Append({ A, C, B, B, C, D });
			}
		}
		Section.IndexCount = Piece.Indices.Num() - Section.FirstIndex;
	}

	FMantleVehicleBodyPiece MakePiece(const int32 N)
	{
		FMantleVehicleBodyPiece Piece;
		Piece.Bone = FName(TEXT("root"));
		Piece.Materials = { nullptr, nullptr };
		Piece.SlotNames = { FName(TEXT("seat")), FName(TEXT("door")) };
		AddPieceGrid(Piece, N, 0, FVector3f::ZeroVector);
		AddPieceGrid(Piece, N, 1, FVector3f(0.0f, 0.0f, 100.0f));
		return Piece;
	}

	int32 SectionTriangles(const FStaticMeshLODResources& LOD)
	{
		int32 Triangles = 0;
		for (const FStaticMeshSection& Section : LOD.Sections)
		{
			Triangles += static_cast<int32>(Section.NumTriangles);
		}
		return Triangles;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehiclePartLODsTest,
	"Mantle.Vehicle.LODs.PartsGetLowerLODs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehiclePartLODsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehiclePartLODsTestPrivate;
	if (!FMantleMeshReduce::IsAvailable())
	{
		AddInfo(TEXT("This editor has no static mesh reducer, so the part LODs are not made."));
		return true;
	}
	FString Error;
	// 2 x 2 x 40 x 40: 6,400 triangles over two materials.
	const FMantleVehicleBodyPiece Piece = MakePiece(40);
	UStaticMesh* Mesh = FMantleVehicleBodySplit::MakePieceMesh(Piece, TEXT("/Temp/MantleVehiclePartLODsTest"), TEXT("SM_TestPart_Big"), Error);
	if (!TestNotNull(*FString::Printf(TEXT("the part builds (%s)"), *Error), Mesh) || !Mesh->GetRenderData())
	{
		return false;
	}
	const int32 Triangles = Piece.NumTriangles();
	TestEqual(TEXT("the part has LOD0, LOD1 and LOD2"), Mesh->GetNumLODs(), 3);
	if (Mesh->GetNumLODs() != 3)
	{
		return false;
	}
	const FStaticMeshRenderData& Render = *Mesh->GetRenderData();
	TestEqual(TEXT("LOD0 is the part as it was"), SectionTriangles(Render.LODResources[0]), Triangles);
	int32 Previous = Triangles;
	const float Fractions[2] = { 0.5f, 0.25f };
	for (int32 LOD = 1; LOD < 3; ++LOD)
	{
		const int32 Count = SectionTriangles(Render.LODResources[LOD]);
		TestTrue(*FString::Printf(TEXT("LOD%d is smaller than LOD%d (%d of %d)"), LOD, LOD - 1, Count, Previous), Count > 0 && Count < Previous);
		TestTrue(*FString::Printf(TEXT("LOD%d is about %.0f%% of LOD0 (%d of %d)"), LOD, Fractions[LOD - 1] * 100.0f, Count, Triangles),
			Count <= FMath::CeilToInt(Triangles * Fractions[LOD - 1] * 1.25f));
		Previous = Count;
		TestEqual(*FString::Printf(TEXT("LOD%d keeps both material slots drawn"), LOD), Render.LODResources[LOD].Sections.Num(), 2);
	}
	TestEqual(TEXT("it has its two material slots"), Mesh->GetStaticMaterials().Num(), 2);
	TestEqual(TEXT("LOD1 switches at the skeletal LODs' screen size"), Render.ScreenSize[1].Default, 0.35f);
	TestEqual(TEXT("LOD2 switches at the skeletal LODs' screen size"), Render.ScreenSize[2].Default, 0.15f);

	// A part too small to gain from far-away versions has one LOD, as before.
	const FMantleVehicleBodyPiece Small = MakePiece(3);
	UStaticMesh* SmallMesh = FMantleVehicleBodySplit::MakePieceMesh(Small, TEXT("/Temp/MantleVehiclePartLODsTest"), TEXT("SM_TestPart_Small"), Error);
	if (TestNotNull(TEXT("the small part builds"), SmallMesh))
	{
		TestEqual(TEXT("a part of 36 triangles has one LOD"), SmallMesh->GetNumLODs(), 1);
	}

	// Building the same part again updates it in place, and a part that got smaller does not keep its old far-away versions.
	UStaticMesh* Again = FMantleVehicleBodySplit::MakePieceMesh(Small, TEXT("/Temp/MantleVehiclePartLODsTest"), TEXT("SM_TestPart_Big"), Error);
	if (TestNotNull(TEXT("the part builds again"), Again))
	{
		TestTrue(TEXT("it is the same asset"), Again == Mesh);
		TestEqual(TEXT("now with one LOD"), Again->GetNumLODs(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleCarBudgetTest,
	"Mantle.Vehicle.LODs.WholeCarIsWithinTheLimit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleCarBudgetTest::RunTest(const FString& Parameters)
{
	if (!FMantleMeshReduce::IsAvailable())
	{
		AddInfo(TEXT("This editor has no static mesh reducer, so the reduction is not run."));
		return true;
	}
	// A body of 11 materials (3 more than a body may draw), each a grid on one of two bones: 11 x 800 = 8,800 triangles, so with a limit of 5,000 the whole car is over it.
	FMantleSkinnedMesh Body;
	Body.Bones = { { FName(TEXT("root")), INDEX_NONE }, { FName(TEXT("hub")), 0 } };
	Body.ReferencePose = { FTransform::Identity, FTransform::Identity };
	FMantleSkinnedMeshLOD& LOD = Body.LODs.AddDefaulted_GetRef();
	LOD.NumInfluences = 1;
	TArray<FSkeletalMaterial> Materials;
	constexpr int32 MaterialCount = 11;
	constexpr int32 N = 20;
	for (int32 Material = 0; Material < MaterialCount; ++Material)
	{
		Materials.Add(FSkeletalMaterial(nullptr, FName(*FString::Printf(TEXT("mat%d"), Material))));
		FMantleSkinnedMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
		Section.MaterialIndex = Material;
		Section.MaterialName = FName(*FString::Printf(TEXT("mat%d"), Material));
		Section.FirstIndex = LOD.Indices.Num();
		const uint32 First = static_cast<uint32>(LOD.Positions.Num());
		for (int32 Y = 0; Y <= N; ++Y)
		{
			for (int32 X = 0; X <= N; ++X)
			{
				LOD.Positions.Add(FVector3f(X * 5.0f + Material * 200.0f, Y * 5.0f, 4.0f * FMath::Sin(X * 0.4f) * FMath::Cos(Y * 0.3f)));
				LOD.Normals.Add(FVector3f::UpVector);
				LOD.UVs.Add(FVector2f(static_cast<float>(X) / N, static_cast<float>(Y) / N));
				LOD.InfluenceBones.Add(static_cast<uint16>(Material % 2));
				LOD.InfluenceWeights.Add(255);
			}
		}
		for (int32 Y = 0; Y < N; ++Y)
		{
			for (int32 X = 0; X < N; ++X)
			{
				const uint32 A = First + Y * (N + 1) + X, B = A + 1, C = A + N + 1, D = C + 1;
				LOD.Indices.Append({ A, C, B, B, C, D });
			}
		}
		Section.IndexCount = LOD.Indices.Num() - Section.FirstIndex;
	}
	const int32 Before = FMantleVehicleBodyReduce::CountTriangles(Body);
	constexpr int32 Limit = 5000;
	TestEqual(TEXT("the car starts with 8,800 triangles"), Before, MaterialCount * 2 * N * N);

	// The builder's order: the whole car is reduced to the limit, then the materials the body may not draw are split off as parts, triangles unchanged.
	FMantleVehicleBodyExtras Extras;
	FMantleVehicleBodyReduceResult Reduced;
	TestTrue(*FString::Printf(TEXT("the car is reduced, not refused (%s)"), *Reduced.Problem), FMantleVehicleBodyReduce::ReduceToFit(Body, Extras, Limit, Reduced));
	TestTrue(TEXT("and says it was reduced"), Reduced.bReduced);
	FMantleVehicleBodySplitResult Split;
	TestTrue(*FString::Printf(TEXT("the body splits into parts (%s)"), *Split.Problem),
		FMantleVehicleBodySplit::Split(Body, Materials, Extras, FMantleVehicleLimits::MaxSections, FMantleVehicleLimits::MaxPartMaterials, FMantleVehicleLimits::MaxParts, Split));
	int32 PartTriangles = 0;
	for (const FMantleVehicleBodyPiece& Piece : Split.Pieces)
	{
		PartTriangles += Piece.NumTriangles();
	}
	const int32 BodyTriangles = FMantleVehicleBodyReduce::CountTriangles(Body);
	const int32 Total = BodyTriangles + PartTriangles;
	TestTrue(*FString::Printf(TEXT("the body and its parts are within the limit together (%d + %d = %d of %d)"), BodyTriangles, PartTriangles, Total, Limit),
		Total > 0 && Total <= Limit);
	TestTrue(TEXT("the parts carry triangles, so the budget covered them"), PartTriangles > 0);
	TestEqual(TEXT("every material is still drawn (body and parts)"), FMantleVehicleBodySplit::CountMaterials(Body) + Split.MaterialsBefore - Split.MaterialsAfter, MaterialCount);

	// A car within the limit is left as it is: no reduction.
	FMantleSkinnedMesh Small = Body;
	FMantleVehicleBodyExtras SmallExtras;
	FMantleVehicleBodyReduceResult SmallResult;
	const int32 SmallBefore = FMantleVehicleBodyReduce::CountTriangles(Small);
	TestTrue(TEXT("a car within the limit passes"), FMantleVehicleBodyReduce::ReduceToFit(Small, SmallExtras, Limit, SmallResult));
	TestFalse(TEXT("and is not reduced"), SmallResult.bReduced);
	TestEqual(TEXT("with its triangles as they were"), FMantleVehicleBodyReduce::CountTriangles(Small), SmallBefore);
	return true;
}

#endif
