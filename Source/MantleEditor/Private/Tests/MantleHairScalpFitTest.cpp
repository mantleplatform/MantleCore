// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHairScalpFit.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The scalp offset field, the cap's lower-edge wrap and the head query. Synthetic heads, no assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleHairScalpFitTestPrivate
{
	/** A flat "head" over [-Half, Half]^2 cm at Z = 0, facing +Z, one vertex per centimetre. */
	void ScalpFitFlatHead(int32 Half, TArray<FVector3f>& Positions, TArray<uint32>& Indices)
	{
		const int32 Side = 2 * Half + 1;
		for (int32 Y = -Half; Y <= Half; ++Y) for (int32 X = -Half; X <= Half; ++X) Positions.Add(FVector3f(X, Y, 0.f));
		const auto Idx = [Side, Half](int32 X, int32 Y) { return static_cast<uint32>((Y + Half) * Side + (X + Half)); };
		for (int32 Y = -Half; Y < Half; ++Y) for (int32 X = -Half; X < Half; ++X)
			Indices.Append({Idx(X, Y), Idx(X + 1, Y), Idx(X + 1, Y + 1), Idx(X, Y), Idx(X + 1, Y + 1), Idx(X, Y + 1)});
	}

	/** An octahedron of radius 10 cm, outward windings. */
	void ScalpFitOctahedron(TArray<FVector3f>& Positions, TArray<uint32>& Indices)
	{
		Positions = {FVector3f(10, 0, 0), FVector3f(-10, 0, 0), FVector3f(0, 10, 0), FVector3f(0, -10, 0), FVector3f(0, 0, 10), FVector3f(0, 0, -10)};
		for (int32 SX = 0; SX < 2; ++SX) for (int32 SY = 0; SY < 2; ++SY) for (int32 SZ = 0; SZ < 2; ++SZ)
		{
			const uint32 A = SX ? 1 : 0, B = SY ? 3 : 2, C = SZ ? 5 : 4;
			const int32 Negatives = SX + SY + SZ;
			if (Negatives % 2 == 0) Indices.Append({A, B, C}); else Indices.Append({A, C, B});
		}
	}

	/**
	 * One tiny card (0.1 cm x 0.02 cm, four vertices) hanging at Z above each lattice point of the flat head: every card
	 * vertex has the same nearest head vertex, so the field sees a uniform surface of hair.
	 */
	void ScalpFitCardLattice(int32 Half, float Z, TArray<FVector3f>& Positions, TArray<uint32>& Indices)
	{
		for (int32 Y = -Half; Y <= Half; ++Y) for (int32 X = -Half; X <= Half; ++X)
		{
			const uint32 Base = Positions.Num();
			Positions.Add(FVector3f(X - .05f, Y - .01f, Z));
			Positions.Add(FVector3f(X + .05f, Y - .01f, Z));
			Positions.Add(FVector3f(X + .05f, Y + .01f, Z));
			Positions.Add(FVector3f(X - .05f, Y + .01f, Z));
			Indices.Append({Base, Base + 1, Base + 2, Base, Base + 2, Base + 3});
		}
	}

	FMantleScalpFit::FMesh ScalpFitMesh(const TArray<FVector3f>& Positions, const TArray<uint32>& Indices)
	{
		FMantleScalpFit::FMesh Mesh;
		Mesh.Positions = Positions;
		Mesh.Indices = Indices;
		return Mesh;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairScalpFitSignedDistanceTest,
	"Mantle.Editor.HairScalpFit.SignedDistanceInsideOutsideOn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairScalpFitSignedDistanceTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairScalpFitTestPrivate;
	TArray<FVector3f> Positions; TArray<uint32> Indices;
	ScalpFitOctahedron(Positions, Indices);
	FMantleScalpHeadSurface Head; FString Error;
	if (!TestTrue(TEXT("head builds"), Head.Build(Positions, Indices, Error))) { AddError(Error); return false; }

	// The centre is 10 / sqrt(3) from every face, inside.
	const FMantleScalpHeadSurface::FHit Inside = Head.Query(FVector::ZeroVector);
	TestNearlyEqual(TEXT("inside distance"), Inside.SignedDistance, -10.0 / FMath::Sqrt(3.0), 1.e-6);
	// The centre is equidistant from all eight faces, so which face wins is a tie; any of them is right. Whichever it is,
	// the closest point is that face's centre (every component +-10/3) ...
	const double FaceCentre = 10.0 / 3.0;
	TestTrue(TEXT("closest point is a face centre"), FMath::IsNearlyEqual(FMath::Abs(Inside.Point.X), FaceCentre, 1.e-6)
		&& FMath::IsNearlyEqual(FMath::Abs(Inside.Point.Y), FaceCentre, 1.e-6) && FMath::IsNearlyEqual(FMath::Abs(Inside.Point.Z), FaceCentre, 1.e-6));
	// ... and the normal there points out of that face: from the centre of the octahedron, along the closest point.
	TestTrue(TEXT("normal points out of the face"), Inside.Normal.Equals(Inside.Point.GetSafeNormal(), 1.e-6));

	// Above the top corner: nearest is the corner, the normal there is straight up.
	const FMantleScalpHeadSurface::FHit Outside = Head.Query(FVector(0, 0, 20));
	TestNearlyEqual(TEXT("outside distance"), Outside.SignedDistance, 10.0, 1.e-6);
	TestTrue(TEXT("corner normal"), Outside.Normal.Equals(FVector(0, 0, 1), 1.e-6));

	// A point on a face is at distance zero.
	const FMantleScalpHeadSurface::FHit On = Head.Query(FVector(10.0 / 3.0));
	TestTrue(TEXT("on the surface"), FMath::Abs(On.SignedDistance) < 1.e-6);

	// Just under and just over a face keep their signs.
	TestTrue(TEXT("1 mm under is negative"), Head.Query(FVector(10.0 / 3.0) - FVector(1, 1, 1).GetSafeNormal() * 0.1).SignedDistance < -0.0999);
	TestTrue(TEXT("1 mm over is positive"), Head.Query(FVector(10.0 / 3.0) + FVector(1, 1, 1).GetSafeNormal() * 0.1).SignedDistance > 0.0999);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairScalpFitGaussianNormalisesTest,
	"Mantle.Editor.HairScalpFit.GaussianKeepsAConstantFieldConstant",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairScalpFitGaussianNormalisesTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairScalpFitTestPrivate;
	TArray<FVector3f> Positions; TArray<uint32> Indices;
	ScalpFitFlatHead(10, Positions, Indices);
	FMantleScalpHeadSurface Head; FString Error;
	if (!TestTrue(TEXT("head builds"), Head.Build(Positions, Indices, Error))) { AddError(Error); return false; }

	// Half the vertices carry data (2.5); the rest hold garbage the smoothing must ignore.
	TArray<double> Value; TArray<uint8> Mask;
	for (int32 Y = -10; Y <= 10; ++Y) for (int32 X = -10; X <= 10; ++X)
	{
		const bool bData = ((X + Y) & 1) == 0;
		Mask.Add(bData ? 1 : 0);
		Value.Add(bData ? 2.5 : 99.0);
	}
	TArray<double> Out;
	FMantleScalpFit::SmoothField(Head, Value, Mask, FMantleScalpFit::SigmaCm, TArray<uint8>(), Out);
	double Worst = 0.0;
	for (const double V : Out) Worst = FMath::Max(Worst, FMath::Abs(V - 2.5));
	TestTrue(TEXT("a constant field stays constant (nothing is scaled by the weights)"), Worst < 1.e-9);

	// Only the wanted vertices are computed; the rest read 0.
	TArray<uint8> Wanted; Wanted.Init(0, Mask.Num()); Wanted[0] = 1;
	FMantleScalpFit::SmoothField(Head, Value, Mask, FMantleScalpFit::SigmaCm, Wanted, Out);
	TestNearlyEqual(TEXT("wanted vertex"), Out[0], 2.5, 1.e-9);
	TestEqual(TEXT("unwanted vertex"), Out[1], 0.0);

	// No data anywhere: nothing to spread.
	TArray<uint8> None; None.Init(0, Mask.Num());
	FMantleScalpFit::SmoothField(Head, Value, None, FMantleScalpFit::SigmaCm, TArray<uint8>(), Out);
	double Sum = 0.0;
	for (const double V : Out) Sum += FMath::Abs(V);
	TestEqual(TEXT("empty mask gives a zero field"), Sum, 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairScalpFitSunkVertexTest,
	"Mantle.Editor.HairScalpFit.SunkVertexEndsAtLeastOneMillimetreOut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairScalpFitSunkVertexTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairScalpFitTestPrivate;
	TArray<FVector3f> HeadP; TArray<uint32> HeadI;
	ScalpFitFlatHead(10, HeadP, HeadI);
	FMantleScalpHeadSurface Head; FString Error;
	if (!TestTrue(TEXT("head builds"), Head.Build(HeadP, HeadI, Error))) { AddError(Error); return false; }

	// The last-resort lift on its own: only the vertex more than 1 mm inside moves, to exactly the clearance.
	{
		TArray<FVector> V = {FVector(0, 0, -0.5), FVector(1, 0, -0.05), FVector(2, 0, 1.0)};
		TArray<uint8> Used = {1, 1, 1};
		double MaxLift = 0.0;
		const int32 Lifted = FMantleScalpFit::LiftSunk(Head, V, Used, FMantleScalpFit::ClearanceCm, MaxLift);
		TestEqual(TEXT("one vertex lifted"), Lifted, 1);
		TestNearlyEqual(TEXT("sunk vertex ends 1 mm out"), V[0].Z, 0.1, 1.e-9);
		TestNearlyEqual(TEXT("largest lift"), MaxLift, 0.6, 1.e-9);
		TestNearlyEqual(TEXT("a vertex under 1 mm inside is left alone"), V[1].Z, -0.05, 1.e-9);
		TestNearlyEqual(TEXT("a floating vertex is left alone"), V[2].Z, 1.0, 1.e-9);
		TArray<uint8> Skip = {0, 0, 0};
		V[0].Z = -0.5;
		TestEqual(TEXT("unused vertices are not lifted"), FMantleScalpFit::LiftSunk(Head, V, Skip, FMantleScalpFit::ClearanceCm, MaxLift), 0);
	}

	// Through the whole fit: hair sunk half a centimetre comes out 1 mm clear of the skin.
	TArray<FVector3f> CardP; TArray<uint32> CardI;
	ScalpFitCardLattice(4, -0.5f, CardP, CardI);
	FMantleScalpFit::FResult Result;
	if (!TestTrue(TEXT("fit runs"), FMantleScalpFit::Run(Head, ScalpFitMesh(CardP, CardI), nullptr, Result, Error))) { AddError(Error); return false; }
	TestEqual(TEXT("vertex count kept"), Result.CardPositions.Num(), CardP.Num());
	float Lowest = TNumericLimits<float>::Max();
	for (const FVector3f& P : Result.CardPositions) Lowest = FMath::Min(Lowest, P.Z);
	TestTrue(TEXT("every sunk vertex ends at least 1 mm outside"), Lowest >= 0.1f - 1.e-5f);
	TestEqual(TEXT("none still inside"), Result.Metrics.CardInsideAfter, 0);
	TestTrue(TEXT("it was sunk before"), Result.Metrics.CardInsideBefore > 0);
	TestFalse(TEXT("a clean fit does not ask for a check"), Result.bCheckFit);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairScalpFitFloatingRootTest,
	"Mantle.Editor.HairScalpFit.FloatingRootsWithoutACapPullToThreeMillimetres",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairScalpFitFloatingRootTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairScalpFitTestPrivate;
	TArray<FVector3f> HeadP; TArray<uint32> HeadI;
	ScalpFitFlatHead(10, HeadP, HeadI);
	FMantleScalpHeadSurface Head; FString Error;
	if (!TestTrue(TEXT("head builds"), Head.Build(HeadP, HeadI, Error))) { AddError(Error); return false; }

	// Cards 1 cm off the skin: the roots float by more than 5 mm, so the field pulls them to 3 mm.
	TArray<FVector3f> CardP; TArray<uint32> CardI;
	ScalpFitCardLattice(4, 1.0f, CardP, CardI);
	FMantleScalpFit::FResult Result;
	if (!TestTrue(TEXT("fit runs"), FMantleScalpFit::Run(Head, ScalpFitMesh(CardP, CardI), nullptr, Result, Error))) { AddError(Error); return false; }
	TestEqual(TEXT("one root per card"), Result.Metrics.CardCount, 81);
	TestEqual(TEXT("roots start off the 5 mm band"), Result.Metrics.RootsWithin5mmBefore, 0.0);
	TestEqual(TEXT("roots end inside the 5 mm band"), Result.Metrics.RootsWithin5mmAfter, 100.0);
	for (const FVector3f& P : Result.CardPositions) TestTrue(TEXT("pulled to 3 mm"), FMath::Abs(P.Z - 0.3f) < 1.e-4f);
	TestNearlyEqual(TEXT("largest pull"), Result.Metrics.MaxPullCm, -0.7, 1.e-6);
	TestEqual(TEXT("the last-resort lift was not needed"), Result.Metrics.CardLifted, 0);

	// A root that is only 3 mm off is left where the pack put it.
	TArray<FVector3f> NearP; TArray<uint32> NearI;
	ScalpFitCardLattice(4, 0.3f, NearP, NearI);
	if (!TestTrue(TEXT("second fit runs"), FMantleScalpFit::Run(Head, ScalpFitMesh(NearP, NearI), nullptr, Result, Error))) { AddError(Error); return false; }
	for (const FVector3f& P : Result.CardPositions) TestTrue(TEXT("kept at 3 mm"), FMath::Abs(P.Z - 0.3f) < 1.e-4f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairScalpFitCapEdgeTest,
	"Mantle.Editor.HairScalpFit.CapEdgeSnapIsCappedAtThreeCentimetres",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairScalpFitCapEdgeTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairScalpFitTestPrivate;
	TArray<FVector3f> HeadP; TArray<uint32> HeadI;
	ScalpFitFlatHead(10, HeadP, HeadI);
	FMantleScalpHeadSurface Head; FString Error;
	if (!TestTrue(TEXT("head builds"), Head.Build(HeadP, HeadI, Error))) { AddError(Error); return false; }

	// An open 8 cm sheet 10 cm above the head. Its edge wants to move 9.9 cm; the snap is capped at 3.
	TArray<FVector> CapP; TArray<uint32> CapI;
	const int32 Half = 4, Side = 2 * Half + 1;
	for (int32 Y = -Half; Y <= Half; ++Y) for (int32 X = -Half; X <= Half; ++X) CapP.Add(FVector(X, Y, 10.0));
	const auto Idx = [Side, Half](int32 X, int32 Y) { return static_cast<uint32>((Y + Half) * Side + (X + Half)); };
	for (int32 Y = -Half; Y < Half; ++Y) for (int32 X = -Half; X < Half; ++X)
		CapI.Append({Idx(X, Y), Idx(X + 1, Y), Idx(X + 1, Y + 1), Idx(X, Y), Idx(X + 1, Y + 1), Idx(X, Y + 1)});
	TArray<FVector> Wrapped; TArray<int32> Edge;
	FMantleScalpFit::FMetrics Metrics;
	if (!TestTrue(TEXT("wrap runs"), FMantleScalpFit::WrapCapEdge(Head, CapP, CapI, Wrapped, Metrics, &Edge))) return false;

	TestEqual(TEXT("open-edge vertices"), Metrics.CapEdgeVertices, 32);
	TestEqual(TEXT("every edge vertex hit the 3 cm cap"), Metrics.CapEdgeClamped, 32);
	TestEqual(TEXT("edge list"), Edge.Num(), 32);
	TestNearlyEqual(TEXT("edge vertex moved exactly 3 cm"), Wrapped[Idx(Half, 0)].Z, 7.0, 1.e-9);
	TestNearlyEqual(TEXT("edge corner moved exactly 3 cm"), Wrapped[Idx(Half, Half)].Z, 7.0, 1.e-9);
	// 1 cm in from the edge: cosine falloff over 3 cm, (1 + cos(pi / 3)) / 2 = 0.75 of the move.
	TestNearlyEqual(TEXT("1 cm in from the edge"), Wrapped[Idx(Half - 1, 0)].Z, 10.0 - 3.0 * 0.75, 1.e-9);
	// The middle is 4 cm from the edge, past the falloff: untouched.
	TestNearlyEqual(TEXT("the middle stays put"), Wrapped[Idx(0, 0)].Z, 10.0, 1.e-9);
	TestEqual(TEXT("nothing needed lifting off the skin"), Metrics.CapLifted, 0);

	// A sheet 2 cm above the head reaches the skin + 1 mm, since that is within the cap.
	for (FVector& P : CapP) P.Z = 2.0;
	FMantleScalpFit::WrapCapEdge(Head, CapP, CapI, Wrapped, Metrics);
	TestNearlyEqual(TEXT("edge snaps to skin + 1 mm"), Wrapped[Idx(Half, 0)].Z, 0.1, 1.e-9);
	return true;
}

#endif
