// Copyright Joshua Eger. All Rights Reserved.
#include "MantleUnriggedBody.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "Misc/AutomationTest.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

// The arm bend for a model with no skeleton, on synthetic figures. No assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleUnriggedBodyTestPrivate
{
	// A 165 cm figure (Z up, X shoulder to shoulder): a column from the floor to the head, a torso, and an arm either side running
	// from the shoulder (16.5 cm out, 127 cm up) to Tip. Estimated: pivot 16.5, arm height 127 when Tip is at 127.
	TArray<FVector> UnriggedTestFigure(const FVector2D& Tip, const double Scale = 1.0)
	{
		TArray<FVector> Points;
		for (int32 Step = 0; Step <= 33; ++Step)
		{
			Points.Add(FVector(0.0, 0.0, 5.0 * Step));
		}
		for (int32 Step = 0; Step <= 6; ++Step)
		{
			Points.Add(FVector(-12.0, 0.0, 100.0 + 5.0 * Step));
			Points.Add(FVector(12.0, 0.0, 100.0 + 5.0 * Step));
		}
		const FVector2D Shoulder(16.5, 127.0);
		for (int32 Step = 0; Step <= 40; ++Step)
		{
			const FVector2D At = FMath::Lerp(Shoulder, Tip, Step / 40.0);
			Points.Add(FVector(At.X, 0.0, At.Y));
			Points.Add(FVector(-At.X, 0.0, At.Y));
		}
		for (FVector& Point : Points)
		{
			Point *= Scale;
		}
		return Points;
	}

	bool UnriggedNear(const double A, const double B, const double Tolerance = 1e-6)
	{
		return FMath::Abs(A - B) <= Tolerance;
	}

	// A closed 10 cm cube, wound outward: corner I is at (I & 1, I & 2, I & 4) times 10, and each face is a quad of corners split
	// into two triangles (A, B, C) and (A, C, D).
	FVector UnriggedCubeCorner(const int32 Corner)
	{
		return FVector((Corner & 1) ? 10.0 : 0.0, (Corner & 2) ? 10.0 : 0.0, (Corner & 4) ? 10.0 : 0.0);
	}

	const int32 UnriggedCubeFaces[6][4] = { { 0, 2, 3, 1 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 }, { 2, 6, 7, 3 }, { 0, 4, 6, 2 }, { 1, 3, 7, 5 } };

	// The cube's 12 triangles in a mesh of their own.
	UDynamicMesh* UnriggedCubeMesh(const bool bSoup, const FVector& Jitter = FVector::ZeroVector)
	{
		UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
		Mesh->EditMesh([bSoup, &Jitter](UE::Geometry::FDynamicMesh3& Edit)
		{
			int32 Shared[8] = {};
			if (!bSoup)
			{
				for (int32 Corner = 0; Corner < 8; ++Corner)
				{
					Shared[Corner] = Edit.AppendVertex(UnriggedCubeCorner(Corner));
				}
			}
			bool bJittered = false;
			auto Vertex = [&](const int32 Corner)
			{
				if (!bSoup)
				{
					return Shared[Corner];
				}
				// A soup: every triangle its own three vertices. The first copy of corner 7 is moved by Jitter.
				const bool bMove = Corner == 7 && !bJittered;
				bJittered = bJittered || Corner == 7;
				return Edit.AppendVertex(UnriggedCubeCorner(Corner) + (bMove ? Jitter : FVector::ZeroVector));
			};
			for (int32 FaceIndex = 0; FaceIndex < 6; ++FaceIndex)
			{
				const int32* Face = UnriggedCubeFaces[FaceIndex];
				const int32 A = Vertex(Face[0]);
				const int32 B = Vertex(Face[1]);
				const int32 C = Vertex(Face[2]);
				Edit.AppendTriangle(A, B, C);
				const int32 A2 = Vertex(Face[0]);
				const int32 C2 = Vertex(Face[2]);
				const int32 D = Vertex(Face[3]);
				Edit.AppendTriangle(A2, C2, D);
			}
		});
		return Mesh;
	}

	// A welded flat sheet of Cells x Cells unit quads at Offset (shared vertices, open edges all round).
	void AppendUnriggedSheet(UE::Geometry::FDynamicMesh3& Edit, const int32 Cells, const FVector& Offset)
	{
		const int32 First = Edit.MaxVertexID();
		for (int32 Row = 0; Row <= Cells; ++Row)
		{
			for (int32 Column = 0; Column <= Cells; ++Column)
			{
				Edit.AppendVertex(Offset + FVector(static_cast<double>(Column), static_cast<double>(Row), 0.0));
			}
		}
		for (int32 Row = 0; Row < Cells; ++Row)
		{
			for (int32 Column = 0; Column < Cells; ++Column)
			{
				const int32 A = First + Row * (Cells + 1) + Column;
				const int32 B = A + 1;
				const int32 C = A + Cells + 2;
				const int32 D = A + Cells + 1;
				Edit.AppendTriangle(A, B, C);
				Edit.AppendTriangle(A, C, D);
			}
		}
	}

	// Count triangles, each its own three vertices, 5 cm apart along X from Offset: a soup with nothing to weld.
	void AppendUnriggedScatter(UE::Geometry::FDynamicMesh3& Edit, const int32 Count, const FVector& Offset)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FVector At = Offset + FVector(5.0 * Index, 0.0, 0.0);
			const int32 A = Edit.AppendVertex(At);
			const int32 B = Edit.AppendVertex(At + FVector(1.0, 0.0, 0.0));
			const int32 C = Edit.AppendVertex(At + FVector(0.0, 1.0, 0.0));
			Edit.AppendTriangle(A, B, C);
		}
	}

	// Every vertex position of a dynamic mesh, in vertex ID order.
	TArray<FVector> UnriggedWeldPoints(const UDynamicMesh& Mesh)
	{
		TArray<FVector> Out;
		Mesh.ProcessMesh([&Out](const UE::Geometry::FDynamicMesh3& Read)
		{
			for (const int32 Vertex : Read.VertexIndicesItr())
			{
				Out.Add(Read.GetVertex(Vertex));
			}
		});
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUnriggedBodyArmBendTest,
	"Mantle.Editor.UnriggedBody.ArmBend",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUnriggedBodyArmBendTest::RunTest(const FString& Parameters)
{
	using namespace MantleUnriggedBodyTestPrivate;

	const FMantleArmBend Bend = FMantleUnriggedBody::EstimateArmBend(UnriggedTestFigure(FVector2D(75.0, 127.0)));
	TestTrue(TEXT("the arms are found"), Bend.bValid);
	TestTrue(TEXT("a figure with its arms out sideways is a T-pose"), Bend.bTPose);
	TestTrue(TEXT("the arm height is the arms'"), UnriggedNear(Bend.ArmZ, 127.0));
	TestTrue(TEXT("the pivot is a tenth of the height from the middle"), UnriggedNear(Bend.PivotX, 16.5));
	TestTrue(TEXT("the height is the figure's"), UnriggedNear(Bend.Height, 165.0));
	TestTrue(TEXT("the widest point is the fingertip"), UnriggedNear(Bend.MaxX, 75.0));

	// A vertex on the horizontal arm ends up 45 degrees lower about the pivot, the same distance from it.
	const FVector OnArm(60.0, 5.0, 127.0);
	const FVector Bent = FMantleUnriggedBody::BendPoint(Bend, OnArm);
	const double Reach = OnArm.X - Bend.PivotX;
	TestTrue(TEXT("a vertex on the arm keeps its depth"), UnriggedNear(Bent.Y, 5.0));
	TestTrue(TEXT("a vertex on the arm moves out 43.5 cos 45"), UnriggedNear(Bent.X, Bend.PivotX + Reach * FMath::Cos(UE_PI / 4.0)));
	TestTrue(TEXT("a vertex on the arm drops 43.5 sin 45"), UnriggedNear(Bent.Z, 127.0 - Reach * FMath::Sin(UE_PI / 4.0)));
	TestTrue(TEXT("a vertex on the arm ends 45 degrees below the pivot"),
		UnriggedNear(FMath::RadiansToDegrees(FMath::Atan2(Bend.ArmZ - Bent.Z, Bent.X - Bend.PivotX)), 45.0, 1e-6));
	TestTrue(TEXT("a vertex on the arm stays the same distance from the pivot"),
		UnriggedNear(FVector2D(Bent.X - Bend.PivotX, Bent.Z - Bend.ArmZ).Size(), Reach));

	// The other arm bends the same way, mirrored.
	const FVector Mirrored = FMantleUnriggedBody::BendPoint(Bend, FVector(-OnArm.X, OnArm.Y, OnArm.Z));
	TestTrue(TEXT("the other arm mirrors it (X)"), UnriggedNear(Mirrored.X, -Bent.X));
	TestTrue(TEXT("the other arm mirrors it (Z)"), UnriggedNear(Mirrored.Z, Bent.Z));

	// Nothing below the armpit, on the torso, or in the head is touched.
	TestTrue(TEXT("a vertex below the armpit does not move"), FMantleUnriggedBody::BendPoint(Bend, FVector(60.0, 0.0, 100.0)) == FVector(60.0, 0.0, 100.0));
	TestTrue(TEXT("a hip vertex does not move"), FMantleUnriggedBody::BendPoint(Bend, FVector(15.0, 0.0, 90.0)) == FVector(15.0, 0.0, 90.0));
	TestTrue(TEXT("a vertex on the torso does not move"), FMantleUnriggedBody::BendPoint(Bend, FVector(8.0, 3.0, 128.0)) == FVector(8.0, 3.0, 128.0));
	TestTrue(TEXT("a vertex on the spine does not move"), FMantleUnriggedBody::BendPoint(Bend, FVector(0.0, 0.0, 100.0)) == FVector(0.0, 0.0, 100.0));
	TestTrue(TEXT("a vertex in the head does not move"), FMantleUnriggedBody::BendPoint(Bend, FVector(0.0, 0.0, 160.0)) == FVector(0.0, 0.0, 160.0));
	TestTrue(TEXT("a vertex beside the head does not move"), FMantleUnriggedBody::BendPoint(Bend, FVector(9.0, 0.0, 160.0)) == FVector(9.0, 0.0, 160.0));

	// A different angle bends by that much (the T-pose to a straight-down arm).
	const FVector Down = FMantleUnriggedBody::BendPoint(Bend, OnArm, 90.0);
	TestTrue(TEXT("90 degrees drops the arm straight down from the pivot (X)"), UnriggedNear(Down.X, Bend.PivotX));
	TestTrue(TEXT("90 degrees drops the arm straight down from the pivot (Z)"), UnriggedNear(Down.Z, 127.0 - Reach));

	// On a dynamic mesh: the arm vertex moves, the torso and below-the-armpit vertices don't.
	UDynamicMesh* Mesh = NewObject<UDynamicMesh>(GetTransientPackage());
	const TStrongObjectPtr<UDynamicMesh> MeshHeld(Mesh);
	Mesh->EditMesh([](UE::Geometry::FDynamicMesh3& Edit)
	{
		Edit.AppendVertex(FVector(60.0, 0.0, 127.0));
		Edit.AppendVertex(FVector(0.0, 0.0, 100.0));
		Edit.AppendVertex(FVector(60.0, 0.0, 100.0));
		Edit.AppendTriangle(0, 1, 2);
	});
	TestEqual(TEXT("one vertex of the three moves"), FMantleUnriggedBody::BendArms(*Mesh, Bend), 1);
	const TArray<FVector> After = UnriggedWeldPoints(*Mesh);
	if (TestEqual(TEXT("the mesh keeps its vertices"), After.Num(), 3))
	{
		TestTrue(TEXT("the arm vertex is where BendPoint puts it"), After[0].Equals(FMantleUnriggedBody::BendPoint(Bend, FVector(60.0, 0.0, 127.0)), 1e-9));
		TestTrue(TEXT("the torso vertex stays"), After[1] == FVector(0.0, 0.0, 100.0));
		TestTrue(TEXT("the vertex below the armpit stays"), After[2] == FVector(60.0, 0.0, 100.0));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUnriggedBodyFalloffTest,
	"Mantle.Editor.UnriggedBody.Falloff",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUnriggedBodyFalloffTest::RunTest(const FString& Parameters)
{
	using namespace MantleUnriggedBodyTestPrivate;

	const FMantleArmBend Bend = FMantleUnriggedBody::EstimateArmBend(UnriggedTestFigure(FVector2D(75.0, 127.0)));

	TestTrue(TEXT("smoothstep is 0 at 0"), UnriggedNear(FMantleUnriggedBody::SmoothStep(0.0), 0.0));
	TestTrue(TEXT("smoothstep is 1 at 1"), UnriggedNear(FMantleUnriggedBody::SmoothStep(1.0), 1.0));
	TestTrue(TEXT("smoothstep is a half at a half"), UnriggedNear(FMantleUnriggedBody::SmoothStep(0.5), 0.5));
	TestTrue(TEXT("smoothstep clamps below 0"), UnriggedNear(FMantleUnriggedBody::SmoothStep(-3.0), 0.0));
	TestTrue(TEXT("smoothstep clamps above 1"), UnriggedNear(FMantleUnriggedBody::SmoothStep(4.0), 1.0));

	// The ends of the fade: nothing inside PivotX - 7 or below the armpit, all of it from PivotX + 7 and from 9 cm under the arm height.
	TestTrue(TEXT("no bend at PivotX - 7"), UnriggedNear(FMantleUnriggedBody::BendWeight(Bend, FVector(9.5, 0.0, 127.0)), 0.0));
	TestTrue(TEXT("the whole bend at PivotX + 7"), UnriggedNear(FMantleUnriggedBody::BendWeight(Bend, FVector(23.5, 0.0, 127.0)), 1.0));
	TestTrue(TEXT("no bend 17 cm under the arm height"), UnriggedNear(FMantleUnriggedBody::BendWeight(Bend, FVector(60.0, 0.0, 110.0)), 0.0));
	TestTrue(TEXT("the whole bend 9 cm under the arm height"), UnriggedNear(FMantleUnriggedBody::BendWeight(Bend, FVector(60.0, 0.0, 118.0)), 1.0));
	TestTrue(TEXT("half the bend halfway across the shoulder"), UnriggedNear(FMantleUnriggedBody::BendWeight(Bend, FVector(16.5, 0.0, 127.0)), 0.5));

	// Continuous and monotone: a sweep across the shoulder (X) and up the armpit (Z) moves a vertex smoothly, never by a jump.
	const double Step = 0.01;
	const double MaxJump = 0.3;
	double WorstJump = 0.0;
	bool bMonotone = true;
	FVector Last = FMantleUnriggedBody::BendPoint(Bend, FVector(0.0, 0.0, 127.0));
	double LastWeight = 0.0;
	for (int32 Index = 1; Index <= 8000; ++Index)
	{
		const FVector At(Step * Index, 0.0, 127.0);
		const FVector Bent = FMantleUnriggedBody::BendPoint(Bend, At);
		const double Weight = FMantleUnriggedBody::BendWeight(Bend, At);
		WorstJump = FMath::Max(WorstJump, FVector::Dist(Last, Bent));
		bMonotone = bMonotone && Weight >= LastWeight - 1e-12;
		Last = Bent;
		LastWeight = Weight;
	}
	TestTrue(TEXT("sweeping out along the arm moves a vertex without a jump"), WorstJump < MaxJump);
	TestTrue(TEXT("the bend only grows going out along the arm"), bMonotone);

	WorstJump = 0.0;
	bMonotone = true;
	Last = FMantleUnriggedBody::BendPoint(Bend, FVector(60.0, 0.0, 90.0));
	LastWeight = 0.0;
	for (int32 Index = 1; Index <= 5000; ++Index)
	{
		const FVector At(60.0, 0.0, 90.0 + Step * Index);
		const FVector Bent = FMantleUnriggedBody::BendPoint(Bend, At);
		const double Weight = FMantleUnriggedBody::BendWeight(Bend, At);
		WorstJump = FMath::Max(WorstJump, FVector::Dist(Last, Bent));
		bMonotone = bMonotone && Weight >= LastWeight - 1e-12;
		Last = Bent;
		LastWeight = Weight;
	}
	TestTrue(TEXT("sweeping up past the armpit moves a vertex without a jump"), WorstJump < MaxJump);
	TestTrue(TEXT("the bend only grows going up past the armpit"), bMonotone);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUnriggedBodyTPoseTest,
	"Mantle.Editor.UnriggedBody.TPoseDetection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUnriggedBodyTPoseTest::RunTest(const FString& Parameters)
{
	using namespace MantleUnriggedBodyTestPrivate;

	// T-pose: arms straight out at shoulder height.
	const FMantleArmBend TPose = FMantleUnriggedBody::EstimateArmBend(UnriggedTestFigure(FVector2D(75.0, 127.0)));
	TestTrue(TEXT("arms straight out are a T-pose"), TPose.bTPose);

	// A-pose: arms 45 degrees down. Hands down: arms at the sides. Arms raised high. None is bent.
	const FMantleArmBend APose = FMantleUnriggedBody::EstimateArmBend(UnriggedTestFigure(FVector2D(55.0, 88.0)));
	TestTrue(TEXT("an A-pose is valid"), APose.bValid);
	TestFalse(TEXT("arms 45 degrees down are not a T-pose"), APose.bTPose);
	const FMantleArmBend Down = FMantleUnriggedBody::EstimateArmBend(UnriggedTestFigure(FVector2D(22.0, 70.0)));
	TestFalse(TEXT("arms at the sides are not a T-pose"), Down.bTPose);
	const FMantleArmBend Up = FMantleUnriggedBody::EstimateArmBend(UnriggedTestFigure(FVector2D(60.0, 164.0)));
	TestFalse(TEXT("arms raised high are not a T-pose"), Up.bTPose);

	// Size does not matter: the same figure in metres (or a tenth of a millimetre) is judged and bent alike.
	const double Scale = 0.01;
	const FMantleArmBend Small = FMantleUnriggedBody::EstimateArmBend(UnriggedTestFigure(FVector2D(75.0, 127.0), Scale));
	TestTrue(TEXT("a figure in metres is a T-pose too"), Small.bTPose);
	TestTrue(TEXT("a figure in metres has the pivot scaled"), UnriggedNear(Small.PivotX, TPose.PivotX * Scale, 1e-9));
	const FVector OnArm(60.0, 5.0, 127.0);
	const FVector Bent = FMantleUnriggedBody::BendPoint(TPose, OnArm);
	const FVector SmallBent = FMantleUnriggedBody::BendPoint(Small, OnArm * Scale);
	TestTrue(TEXT("a figure in metres bends the same way, scaled"), (SmallBent / Scale).Equals(Bent, 1e-6));

	// Nothing to estimate from.
	TestFalse(TEXT("no points are not a figure"), FMantleUnriggedBody::EstimateArmBend(TConstArrayView<FVector>()).bValid);
	TestFalse(TEXT("a flat sheet has no height"), FMantleUnriggedBody::EstimateArmBend({ FVector(-5.0, 0.0, 3.0), FVector(5.0, 0.0, 3.0) }).bValid);
	TestFalse(TEXT("a column has no width"), FMantleUnriggedBody::EstimateArmBend({ FVector(0.0, 0.0, 0.0), FVector(0.0, 0.0, 100.0) }).bValid);
	TestFalse(TEXT("an invalid estimate is no T-pose"), FMantleArmBend().bTPose);
	TestTrue(TEXT("an invalid estimate bends nothing"), FMantleUnriggedBody::BendPoint(FMantleArmBend(), OnArm) == OnArm);

	TestEqual(TEXT("the plain message"), FMantleUnriggedBody::PlainMessage(), FString(TEXT("This model has no skeleton, so we'll fit it to a MetaHuman body.")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUnriggedBodyWeldSoupTest,
	"Mantle.Editor.UnriggedBody.WeldSoup",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUnriggedBodyWeldSoupTest::RunTest(const FString& Parameters)
{
	using namespace MantleUnriggedBodyTestPrivate;

	// A closed cube as a triangle soup (36 vertices for 12 triangles) welds to its 8 corners, closed, nothing loose.
	UDynamicMesh* Soup = UnriggedCubeMesh(true);
	const TStrongObjectPtr<UDynamicMesh> SoupHeld(Soup);
	const FMantleUnriggedWeld Weld = FMantleUnriggedBody::WeldVertices(*Soup);
	TestEqual(TEXT("the soup has 36 vertices"), Weld.VerticesBefore, 36);
	TestEqual(TEXT("the soup welds to the cube's 8 corners"), Weld.VerticesAfter, 8);
	TestEqual(TEXT("the weld keeps all 12 triangles"), Weld.Triangles, 12);
	TestEqual(TEXT("no triangle is loose after the weld"), Weld.LooseTriangles, 0);
	TestEqual(TEXT("no vertex is loose after the weld"), Weld.LooseVertices, 0);
	TestFalse(TEXT("the welded cube is not loose pieces"), Weld.bLoosePieces);
	bool bClosed = false;
	Soup->ProcessMesh([&bClosed](const UE::Geometry::FDynamicMesh3& Read)
	{
		bClosed = Read.IsClosed();
	});
	TestTrue(TEXT("the welded cube is closed"), bClosed);
	const TArray<FVector> Points = UnriggedWeldPoints(*Soup);
	TSet<int32> Corners;
	for (const FVector& Point : Points)
	{
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			if (Point == UnriggedCubeCorner(Corner))
			{
				Corners.Add(Corner);
			}
		}
	}
	TestEqual(TEXT("the weld moves no vertex: each is a different corner, exactly"), Corners.Num(), 8);

	// Within the tolerance (0.01 mm) a copy still welds; ten times it does not.
	UDynamicMesh* Near = UnriggedCubeMesh(true, FVector(0.5 * FMantleUnriggedBody::WeldToleranceCm, 0.0, 0.0));
	const TStrongObjectPtr<UDynamicMesh> NearHeld(Near);
	TestEqual(TEXT("a copy half the tolerance away welds"), FMantleUnriggedBody::WeldVertices(*Near).VerticesAfter, 8);
	UDynamicMesh* Far = UnriggedCubeMesh(true, FVector(10.0 * FMantleUnriggedBody::WeldToleranceCm, 0.0, 0.0));
	const TStrongObjectPtr<UDynamicMesh> FarHeld(Far);
	TestTrue(TEXT("a copy ten times the tolerance away stays apart"), FMantleUnriggedBody::WeldVertices(*Far).VerticesAfter > 8);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUnriggedBodyUnusedVerticesTest,
	"Mantle.Editor.UnriggedBody.UnusedVertices",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUnriggedBodyUnusedVerticesTest::RunTest(const FString& Parameters)
{
	using namespace MantleUnriggedBodyTestPrivate;

	// #165: a closed cube with two vertices no triangle uses (one far off, as ElfArden's were) loses those two, nothing else.
	UDynamicMesh* Cube = UnriggedCubeMesh(false);
	const TStrongObjectPtr<UDynamicMesh> CubeHeld(Cube);
	Cube->EditMesh([](UE::Geometry::FDynamicMesh3& Edit)
	{
		Edit.AppendVertex(FVector(0.0, 0.0, 500.0));
		Edit.AppendVertex(UnriggedCubeCorner(0));
	});
	TestEqual(TEXT("the cube has 8 corners and 2 unused vertices"), Cube->GetMeshRef().VertexCount(), 10);
	TestEqual(TEXT("both unused vertices are removed"), FMantleUnriggedBody::RemoveUnusedVertices(*Cube), 2);
	TestEqual(TEXT("the 8 corners stay"), Cube->GetMeshRef().VertexCount(), 8);
	TestEqual(TEXT("all 12 triangles stay"), Cube->GetTriangleCount(), 12);
	TArray<FVector> Points = UnriggedWeldPoints(*Cube);
	TestTrue(TEXT("nothing is left 500 cm up"), !Points.ContainsByPredicate([](const FVector& Point) { return Point.Z > 100.0; }));
	TestEqual(TEXT("a mesh with none loses none"), FMantleUnriggedBody::RemoveUnusedVertices(*Cube), 0);

	// The weld then reads only the drawn vertices.
	const FMantleUnriggedWeld Weld = FMantleUnriggedBody::WeldVertices(*Cube);
	TestEqual(TEXT("the weld starts from the 8 drawn corners"), Weld.VerticesBefore, 8);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUnriggedBodyWeldKeepsWeldedTest,
	"Mantle.Editor.UnriggedBody.WeldKeepsWelded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUnriggedBodyWeldKeepsWeldedTest::RunTest(const FString& Parameters)
{
	using namespace MantleUnriggedBodyTestPrivate;

	// An already welded closed cube: the same 8 vertices, where they were.
	UDynamicMesh* Cube = UnriggedCubeMesh(false);
	const TStrongObjectPtr<UDynamicMesh> CubeHeld(Cube);
	const TArray<FVector> Before = UnriggedWeldPoints(*Cube);
	const FMantleUnriggedWeld Weld = FMantleUnriggedBody::WeldVertices(*Cube);
	TestEqual(TEXT("a welded cube has 8 vertices before"), Weld.VerticesBefore, 8);
	TestEqual(TEXT("a welded cube has 8 vertices after"), Weld.VerticesAfter, 8);
	TestEqual(TEXT("a welded cube keeps its 12 triangles"), Weld.Triangles, 12);
	TestFalse(TEXT("a welded cube is not loose pieces"), Weld.bLoosePieces);
	TestTrue(TEXT("a welded cube's vertices stay where they were"), UnriggedWeldPoints(*Cube) == Before);

	// An open welded sheet (its rim edges have nothing to pair with): unchanged too.
	UDynamicMesh* Sheet = NewObject<UDynamicMesh>(GetTransientPackage());
	const TStrongObjectPtr<UDynamicMesh> SheetHeld(Sheet);
	Sheet->EditMesh([](UE::Geometry::FDynamicMesh3& Edit)
	{
		AppendUnriggedSheet(Edit, 4, FVector::ZeroVector);
	});
	const TArray<FVector> SheetBefore = UnriggedWeldPoints(*Sheet);
	const FMantleUnriggedWeld SheetWeld = FMantleUnriggedBody::WeldVertices(*Sheet);
	TestEqual(TEXT("an open sheet keeps its 25 vertices"), SheetWeld.VerticesAfter, 25);
	TestEqual(TEXT("an open sheet had 25 vertices"), SheetWeld.VerticesBefore, 25);
	TestEqual(TEXT("an open sheet has no loose triangle"), SheetWeld.LooseTriangles, 0);
	TestTrue(TEXT("an open sheet's vertices stay where they were"), UnriggedWeldPoints(*Sheet) == SheetBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUnriggedBodyWeldRefusesLooseTest,
	"Mantle.Editor.UnriggedBody.WeldRefusesLoosePieces",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUnriggedBodyWeldRefusesLooseTest::RunTest(const FString& Parameters)
{
	using namespace MantleUnriggedBodyTestPrivate;

	// A scattered soup: 20 triangles with no vertex in common welds to nothing and is all loose, so it is refused.
	UDynamicMesh* Scatter = NewObject<UDynamicMesh>(GetTransientPackage());
	const TStrongObjectPtr<UDynamicMesh> ScatterHeld(Scatter);
	Scatter->EditMesh([](UE::Geometry::FDynamicMesh3& Edit)
	{
		AppendUnriggedScatter(Edit, 20, FVector::ZeroVector);
	});
	const FMantleUnriggedWeld Weld = FMantleUnriggedBody::WeldVertices(*Scatter);
	TestEqual(TEXT("a scattered soup keeps its 60 vertices"), Weld.VerticesAfter, 60);
	TestEqual(TEXT("every triangle of a scattered soup is loose"), Weld.LooseTriangles, 20);
	TestEqual(TEXT("every vertex of a scattered soup is loose"), Weld.LooseVertices, 60);
	TestTrue(TEXT("all of a scattered soup is loose"), UnriggedNear(Weld.LooseShare(), 1.0));
	TestTrue(TEXT("a scattered soup is loose pieces"), Weld.bLoosePieces);

	// One stray triangle on a big welded surface (3 of 444 vertices) is not.
	UDynamicMesh* Mostly = NewObject<UDynamicMesh>(GetTransientPackage());
	const TStrongObjectPtr<UDynamicMesh> MostlyHeld(Mostly);
	Mostly->EditMesh([](UE::Geometry::FDynamicMesh3& Edit)
	{
		AppendUnriggedSheet(Edit, 20, FVector::ZeroVector);
		AppendUnriggedScatter(Edit, 1, FVector(0.0, 0.0, 50.0));
	});
	const FMantleUnriggedWeld MostlyWeld = FMantleUnriggedBody::WeldVertices(*Mostly);
	TestEqual(TEXT("one stray triangle is loose"), MostlyWeld.LooseTriangles, 1);
	TestTrue(TEXT("its share is 3 of 444 vertices"), UnriggedNear(MostlyWeld.LooseShare(), 3.0 / 444.0));
	TestFalse(TEXT("a welded surface with one stray triangle is not loose pieces"), MostlyWeld.bLoosePieces);

	// An empty mesh has nothing loose (the step refuses it for having no geometry, not for this).
	UDynamicMesh* Empty = NewObject<UDynamicMesh>(GetTransientPackage());
	const TStrongObjectPtr<UDynamicMesh> EmptyHeld(Empty);
	TestFalse(TEXT("an empty mesh is not loose pieces"), FMantleUnriggedBody::WeldVertices(*Empty).bLoosePieces);

	TestEqual(TEXT("the loose-pieces message"), FMantleUnriggedBody::LoosePiecesMessage(),
		FString(TEXT("This model's surface is made of loose pieces, so it can't be fitted. Re-export it with its vertices merged.")));
	return true;
}

#endif
