// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHairHeadFit.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Cards pivot at their true root and scalp hair is translated, never rotated. Synthetic heads, no assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleHairHeadFitTestPrivate
{
	constexpr int32 GridN = 8;

	/** A skin grid over [0,10]^2 whose UVs are the unit square; Flip changes every quad's diagonal. */
	FMantleSkinnedMesh HeadFitTestFace(bool bFlip, float ScaleX, float Bowl, float LiftZ)
	{
		FMantleSkinnedMesh Mesh;
		Mesh.Bones = { {FName(TEXT("root")), INDEX_NONE}, {FName(TEXT("FACIAL_C_Test")), 0} };
		FMantleSkinnedMeshLOD& L = Mesh.LODs.AddDefaulted_GetRef();
		for (int32 Y = 0; Y <= GridN; ++Y) for (int32 X = 0; X <= GridN; ++X)
		{
			const float U = float(X) / GridN, V = float(Y) / GridN;
			L.Positions.Add(FVector3f(U * 10.f * ScaleX, V * 10.f, LiftZ + Bowl * (FMath::Square(U - .5f) + FMath::Square(V - .5f))));
			L.Normals.Add(FVector3f(0, 0, 1));
			L.UVs.Add(FVector2f(U, V));
		}
		auto Idx = [](int32 X, int32 Y) { return uint32(Y * (GridN + 1) + X); };
		for (int32 Y = 0; Y < GridN; ++Y) for (int32 X = 0; X < GridN; ++X)
		{
			const uint32 A = Idx(X, Y), B = Idx(X + 1, Y), C = Idx(X + 1, Y + 1), D = Idx(X, Y + 1);
			if (!bFlip) L.Indices.Append({A, B, C, A, C, D}); else L.Indices.Append({A, B, D, B, C, D});
		}
		FMantleSkinnedMeshSection& S = L.Sections.AddDefaulted_GetRef();
		S.FirstIndex = 0; S.IndexCount = L.Indices.Num(); S.MaterialName = FName(TEXT("head_skin"));
		L.NumInfluences = 1; L.InfluenceBones.Init(1, L.NumVertices()); L.InfluenceWeights.Init(255, L.NumVertices());
		return Mesh;
	}

	FMantleSkinnedMesh HeadFitTestBody()
	{
		FMantleSkinnedMesh Mesh = HeadFitTestFace(false, 1.f, 0.f, 0.f);
		Mesh.Bones = { {FName(TEXT("root")), INDEX_NONE}, {FName(TEXT("head")), 0} };
		return Mesh;
	}

	/**
	 * One hanging card, two verts wide. Its root edge is high above the skin (Z 0) and its tip edge nearly touches it,
	 * so the vertex nearest the skin is the tip, mid-card or beyond, never the root.
	 */
	FMantleSkinnedMesh HeadFitTestCard()
	{
		FMantleSkinnedMesh Mesh = HeadFitTestFace(false, 1.f, 0.f, 0.f);
		FMantleSkinnedMeshLOD& L = Mesh.LODs[0];
		L.Positions.Reset(); L.Normals.Reset(); L.UVs.Reset(); L.Indices.Reset(); L.Sections.Reset();
		L.InfluenceBones.Reset(); L.InfluenceWeights.Reset();
		const float Heights[] = {10.f, 6.f, 2.f};
		for (int32 Row = 0; Row < 3; ++Row) for (int32 Side = 0; Side < 2; ++Side)
		{
			const float T = Row / 2.f;
			L.Positions.Add(FVector3f(3.f + 4.f * T + Side * .5f, 2.f + 4.f * T, Heights[Row]));
			L.Normals.Add(FVector3f(0, -1, 0).GetSafeNormal());
			L.UVs.Add(FVector2f(T, Side));
			L.HairCards.Add(FVector3f(.3f, .2f, T));
		}
		for (uint32 Row = 0; Row < 2; ++Row) L.Indices.Append({Row * 2, Row * 2 + 1, Row * 2 + 2, Row * 2 + 1, Row * 2 + 3, Row * 2 + 2});
		FMantleSkinnedMeshSection& S = L.Sections.AddDefaulted_GetRef();
		S.FirstIndex = 0; S.IndexCount = L.Indices.Num(); S.MaterialName = FName(TEXT("card"));
		L.NumInfluences = 1; L.InfluenceBones.Init(1, L.NumVertices()); L.InfluenceWeights.Init(255, L.NumVertices());
		return Mesh;
	}

	/** A face whose skin stops at UV V = MaxV, as a reduced head LOD trims its border. */
	FMantleSkinnedMesh HeadFitTestTrimmedFace(float MaxV, float LiftZ)
	{
		FMantleSkinnedMesh Mesh = HeadFitTestFace(false, 1.f, 0.f, LiftZ);
		FMantleSkinnedMeshLOD& L = Mesh.LODs[0];
		TArray<uint32> Kept;
		for (int32 I = 0; I < L.Indices.Num(); I += 3)
			if (L.UVs[L.Indices[I]].Y <= MaxV && L.UVs[L.Indices[I + 1]].Y <= MaxV && L.UVs[L.Indices[I + 2]].Y <= MaxV)
				Kept.Append({L.Indices[I], L.Indices[I + 1], L.Indices[I + 2]});
		L.Indices = Kept; L.Sections[0].IndexCount = Kept.Num();
		return Mesh;
	}

	/** Small upright cards rooted .5 above the skin at each (X, Y). */
	FMantleSkinnedMesh HeadFitTestCards(TConstArrayView<FVector2f> Spots)
	{
		FMantleSkinnedMesh Mesh = HeadFitTestCard();
		FMantleSkinnedMeshLOD& L = Mesh.LODs[0];
		L.Positions.Reset(); L.Normals.Reset(); L.UVs.Reset(); L.Indices.Reset(); L.HairCards.Reset();
		for (const FVector2f& Spot : Spots)
		{
			const uint32 First = L.Positions.Num();
			for (int32 Row = 0; Row < 2; ++Row) for (int32 Side = 0; Side < 2; ++Side)
			{
				L.Positions.Add(FVector3f(Spot.X + Side * .2f, Spot.Y, .5f + Row * 2.f));
				L.Normals.Add(FVector3f(0, -1, 0));
				L.UVs.Add(FVector2f(Row, Side));
				L.HairCards.Add(FVector3f(0.f, 0.f, Row));
			}
			L.Indices.Append({First, First + 1, First + 2, First + 1, First + 3, First + 2});
		}
		L.Sections[0].IndexCount = L.Indices.Num();
		L.InfluenceBones.Init(1, L.NumVertices()); L.InfluenceWeights.Init(255, L.NumVertices());
		return Mesh;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairHeadFitBorrowedRootTest,
	"Mantle.Editor.HairHeadFit.UnmappedRootBorrowsNearest",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairHeadFitBorrowedRootTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairHeadFitTestPrivate;
	// 25 cards on skin both heads share, one on a border strip the wearer's head lacks (UV V above .875).
	TArray<FVector2f> Spots;
	for (int32 Y = 0; Y < 5; ++Y) for (int32 X = 0; X < 5; ++X) Spots.Add(FVector2f(1.f + 2.f * X, 1.f + 1.75f * Y));
	Spots.Add(FVector2f(5.f, 9.5f));
	const FMantleSkinnedMesh Cards = HeadFitTestCards(Spots);
	const FMantleSkinnedMesh Source = HeadFitTestFace(false, 1.f, 0.f, 0.f), Target = HeadFitTestTrimmedFace(.875f, 5.f), Body = HeadFitTestBody();
	FMantleSkinnedMesh Fitted; FString Error; int32 Borrowed = -1;
	TestTrue(TEXT("fit succeeds"), FMantleHairHeadFit::Fit(Cards, Source, Target, Body, false, Fitted, Error, &Borrowed));
	if (Fitted.LODs.IsEmpty()) { AddError(Error); return false; }
	TestEqual(TEXT("one root borrowed"), Borrowed, 1);
	const FMantleSkinnedMeshLOD& In = Cards.LODs[0];
	const FMantleSkinnedMeshLOD& Out = Fitted.LODs[0];
	for (int32 V = 0; V < In.NumVertices(); ++V)
		TestTrue(*FString::Printf(TEXT("vertex %d moved by the head offset"), V), (Out.Positions[V] - In.Positions[V] - FVector3f(0.f, 0.f, 5.f)).Size() < 1.e-3f);

	// Mostly unmapped: the heads do not correspond, so it still fails with the correspondence error.
	const FMantleSkinnedMesh Few = HeadFitTestCards({FVector2f(5.f, 5.f), FVector2f(5.f, 9.5f)});
	TestFalse(TEXT("half unmapped fails"), FMantleHairHeadFit::Fit(Few, Source, Target, Body, false, Fitted, Error, &Borrowed));
	TestTrue(TEXT("names the correspondence failure"), Error.Contains(TEXT("head correspondence failed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairHeadFitRootPivotTest,
	"Mantle.Editor.HairHeadFit.PivotsAtRootAndStaysRigid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairHeadFitRootPivotTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairHeadFitTestPrivate;
	const FMantleSkinnedMesh Cards = HeadFitTestCard();
	// The target is wider in X (not a pure translation) and lifted by 5, so a pivot at the tip would offset the card
	// by 7 in X where the root offset is 3.
	const FMantleSkinnedMesh Source = HeadFitTestFace(false, 1.f, 0.f, 0.f), Target = HeadFitTestFace(true, 2.f, 0.f, 5.f), Body = HeadFitTestBody();
	FMantleSkinnedMesh Fitted; FString Error;
	TestTrue(TEXT("fit succeeds"), FMantleHairHeadFit::Fit(Cards, Source, Target, Body, false, Fitted, Error));
	TestEqual(TEXT("fit version"), FMantleHairHeadFit::Version, 3);
	if (Fitted.LODs.IsEmpty()) { AddError(Error); return false; }
	const FMantleSkinnedMeshLOD& In = Cards.LODs[0];
	const FMantleSkinnedMeshLOD& Out = Fitted.LODs[0];
	const FVector3f RootOffset(3.f, 0.f, 5.f);
	for (int32 V = 0; V < In.NumVertices(); ++V)
	{
		TestTrue(*FString::Printf(TEXT("vertex %d moved by the root's offset"), V), (Out.Positions[V] - In.Positions[V] - RootOffset).Size() < 1.e-3f);
		TestTrue(*FString::Printf(TEXT("normal %d unchanged"), V), Out.Normals[V].Equals(In.Normals[V], 1.e-6f));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairHeadFitMismatchedTriangulationTest,
	"Mantle.Editor.HairHeadFit.MismatchedTriangulationDoesNotRotate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairHeadFitMismatchedTriangulationTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairHeadFitTestPrivate;
	const FMantleSkinnedMesh Cards = HeadFitTestCard();
	// The same curved head, triangulated the other way: the per-triangle frames differ, the surface does not.
	const FMantleSkinnedMesh Source = HeadFitTestFace(false, 1.f, .08f, 0.f), Target = HeadFitTestFace(true, 1.f, .08f, 5.f), Body = HeadFitTestBody();
	FMantleSkinnedMesh Fitted; FString Error;
	TestTrue(TEXT("fit succeeds"), FMantleHairHeadFit::Fit(Cards, Source, Target, Body, false, Fitted, Error));
	if (Fitted.LODs.IsEmpty()) { AddError(Error); return false; }
	const FMantleSkinnedMeshLOD& In = Cards.LODs[0];
	const FMantleSkinnedMeshLOD& Out = Fitted.LODs[0];
	for (int32 V = 1; V < In.NumVertices(); ++V)
		TestTrue(*FString::Printf(TEXT("edge 0-%d keeps its length and direction"), V), ((Out.Positions[V] - Out.Positions[0]) - (In.Positions[V] - In.Positions[0])).Size() < 1.e-3f);
	for (int32 V = 0; V < In.NumVertices(); ++V)
		TestTrue(*FString::Printf(TEXT("normal %d unchanged"), V), Out.Normals[V].Equals(In.Normals[V], 1.e-6f));
	return true;
}

#endif
