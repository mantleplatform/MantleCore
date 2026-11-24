// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHairHatFit.h"

#include "Math/RandomStream.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Hair under hats (#389): a sphere head of radius 9 cm around the origin (which is also the head centre), hats of radius 10 cm
// (a dome, a dome with a brim, an open crown) and hair as loose triangles. The numbers the fit should land on are measured
// from the same meshes (FirstHitBruteForce), so the facets of the sphere do not matter.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleHairHatFitTestPrivate
{
	struct FSoup
	{
		TArray<FVector3f> P;
		TArray<uint32> I;
	};

	constexpr float HeadRadiusCm = 9.0f;
	constexpr float HatRadiusCm = 10.0f;

	/** A point by polar angle from +Z, azimuth and radius. */
	FVector3f Sph(const float ThetaDeg, const float PhiDeg, const float R)
	{
		const float T = FMath::DegreesToRadians(ThetaDeg);
		const float F = FMath::DegreesToRadians(PhiDeg);
		return FVector3f(R * FMath::Sin(T) * FMath::Cos(F), R * FMath::Sin(T) * FMath::Sin(F), R * FMath::Cos(T));
	}

	/** A surface of revolution about Z from a profile of (radius, z) points. */
	FSoup Revolve(const TArray<FVector2f>& Profile, const int32 Segments, const FVector3f& Offset = FVector3f::ZeroVector)
	{
		FSoup Out;
		for (const FVector2f& Ring : Profile)
		{
			for (int32 S = 0; S < Segments; ++S)
			{
				const float A = 2.0f * UE_PI * static_cast<float>(S) / static_cast<float>(Segments);
				Out.P.Add(Offset + FVector3f(Ring.X * FMath::Cos(A), Ring.X * FMath::Sin(A), Ring.Y));
			}
		}
		for (int32 R = 0; R + 1 < Profile.Num(); ++R)
		{
			for (int32 S = 0; S < Segments; ++S)
			{
				const uint32 A = R * Segments + S;
				const uint32 B = R * Segments + (S + 1) % Segments;
				const uint32 C = (R + 1) * Segments + S;
				const uint32 D = (R + 1) * Segments + (S + 1) % Segments;
				Out.I.Append({ A, C, B, B, C, D });
			}
		}
		return Out;
	}

	/** A sphere cap: radius R, from the pole down to ThetaMaxDeg (180 is a whole sphere). Open at the bottom. */
	FSoup Dome(const float R, const float ThetaMaxDeg, const int32 Segments = 64, const FVector3f& Offset = FVector3f::ZeroVector)
	{
		const int32 Steps = FMath::Max(4, FMath::CeilToInt(ThetaMaxDeg / 3.0f));
		TArray<FVector2f> Profile;
		for (int32 I = 0; I <= Steps; ++I)
		{
			const float T = FMath::DegreesToRadians(ThetaMaxDeg * static_cast<float>(I) / static_cast<float>(Steps));
			Profile.Add(FVector2f(R * FMath::Sin(T), R * FMath::Cos(T)));
		}
		return Revolve(Profile, Segments, Offset);
	}

	/** A dome whose rim, 2 cm below the head centre, runs out into a flat brim 18 cm from the axis (a cowboy hat). */
	FSoup BrimHat(const int32 Segments = 64)
	{
		const float RimThetaDeg = FMath::RadiansToDegrees(FMath::Acos(-0.2f));
		const int32 Steps = 40;
		TArray<FVector2f> Profile;
		for (int32 I = 0; I <= Steps; ++I)
		{
			const float T = FMath::DegreesToRadians(RimThetaDeg * static_cast<float>(I) / static_cast<float>(Steps));
			Profile.Add(FVector2f(HatRadiusCm * FMath::Sin(T), HatRadiusCm * FMath::Cos(T)));
		}
		Profile.Add(FVector2f(14.0f, -2.0f));
		Profile.Add(FVector2f(18.0f, -2.0f));
		return Revolve(Profile, Segments);
	}

	/** A band of radius 10 from 1 cm below the head centre to 3 cm above it: open at the top and the bottom (a crown, a visor ring). */
	FSoup OpenTopHat(const int32 Segments = 64)
	{
		TArray<FVector2f> Profile;
		for (int32 I = 0; I <= 4; ++I)
		{
			Profile.Add(FVector2f(HatRadiusCm, -1.0f + static_cast<float>(I)));
		}
		return Revolve(Profile, Segments);
	}

	FSoup Head()
	{
		return Dome(HeadRadiusCm, 180.0f);
	}

	void AddTri(FMantleSkinnedMeshLOD& Hair, const FVector3f& A, const FVector3f& B, const FVector3f& C)
	{
		const uint32 Base = Hair.Positions.Num();
		Hair.Positions.Add(A);
		Hair.Positions.Add(B);
		Hair.Positions.Add(C);
		Hair.Indices.Append({ Base, Base + 1, Base + 2 });
	}

	FVector3f Corner(const FMantleSkinnedMeshLOD& Hair, const int32 Triangle, const int32 Index)
	{
		return Hair.Positions[Hair.Indices[Triangle * 3 + Index]];
	}

	FMantleHairHatFitResult Run(const FMantleSkinnedMeshLOD& Hair, const FSoup& Hat, const FSoup& Scalp,
		const EMantleHairHatRule Rule = EMantleHairHatRule::UnderIt, const bool bFacial = false, const bool bSpecks = false, const bool bBrute = false)
	{
		FMantleHairHatFitParams Params;
		Params.Rule = Rule;
		Params.bFacialHair = bFacial;
		Params.bDropSpecks = bSpecks;
		Params.bBruteForce = bBrute;
		Params.HeadCentre = FVector3f::ZeroVector;
		FMantleHairHatFitResult Out;
		FMantleHairHatFit::Fit(Hair, Hat.P, Hat.I, Scalp.P, Scalp.I, Params, Out);
		return Out;
	}

	/** Rule b (EMantleHairHatAim::UnderHatCards): whole cards mostly under the hat are hidden, the rest pulled in. */
	FMantleHairHatFitResult RunCards(const FMantleSkinnedMeshLOD& Hair, const FSoup& Hat, const FSoup& Scalp, const bool bFacial = false, const bool bBrute = false)
	{
		FMantleHairHatFitParams Params;
		Params.Aim = EMantleHairHatAim::UnderHatCards;
		Params.bFacialHair = bFacial;
		Params.bBruteForce = bBrute;
		Params.HeadCentre = FVector3f::ZeroVector;
		FMantleHairHatFitResult Out;
		FMantleHairHatFit::Fit(Hair, Hat.P, Hat.I, Scalp.P, Scalp.I, Params, Out);
		return Out;
	}

	/**
	 * A card: a strip along polar angle, one row of two points (azimuth Phi -/+ 1 degree) per (theta, radius) in Rows, the
	 * rows joined by two triangles each, sharing their indices. Returns the index of its first point; its triangles follow
	 * the hair's earlier ones.
	 */
	int32 AddCardStrip(FMantleSkinnedMeshLOD& Hair, const float PhiDeg, const TArray<FVector2f>& Rows)
	{
		const int32 First = Hair.Positions.Num();
		for (const FVector2f& Row : Rows)
		{
			Hair.Positions.Add(Sph(Row.X, PhiDeg - 1.0f, Row.Y));
			Hair.Positions.Add(Sph(Row.X, PhiDeg + 1.0f, Row.Y));
		}
		for (int32 Row = 0; Row + 1 < Rows.Num(); ++Row)
		{
			const uint32 A = First + Row * 2, B = A + 1, C = A + 2, D = A + 3;
			Hair.Indices.Append({ A, B, C, B, D, C });
		}
		return First;
	}

	/** The triangles from First up to (not including) End, as a list. */
	TArray<int32> TriangleRange(const int32 First, const int32 End)
	{
		TArray<int32> Out;
		for (int32 T = First; T < End; ++T)
		{
			Out.Add(T);
		}
		return Out;
	}

	/** The distance from the head centre to the first hit of a soup along the ray through Point; negative for none. */
	float HitAlong(const FSoup& Soup, const FVector3f& Point)
	{
		return FMantleHairHatGrid::FirstHitBruteForce(FVector3f::ZeroVector, Point.GetSafeNormal(), Soup.P, Soup.I);
	}

	bool SamePositions(const TArray<FVector3f>& A, const TArray<FVector3f>& B)
	{
		return A.Num() == B.Num() && FMemory::Memcmp(A.GetData(), B.GetData(), A.Num() * sizeof(FVector3f)) == 0;
	}

	/** Every point but the listed ones is exactly where it was. */
	bool OnlyMoved(const FMantleSkinnedMeshLOD& Hair, const FMantleHairHatFitResult& Result, const TArray<int32>& Moved)
	{
		if (Result.Positions.Num() != Hair.Positions.Num())
		{
			return false;
		}
		for (int32 V = 0; V < Hair.Positions.Num(); ++V)
		{
			if (!Moved.Contains(V) && Result.Positions[V] != Hair.Positions[V])
			{
				return false;
			}
		}
		return true;
	}

	/** The hair of the dome scene: inside, one corner poking, all corners poking, below the dome. */
	FMantleSkinnedMeshLOD DomeHair()
	{
		FMantleSkinnedMeshLOD Hair;
		AddTri(Hair, Sph(30.0f, 0.0f, 9.3f), Sph(30.0f, 20.0f, 9.3f), Sph(50.0f, 0.0f, 9.3f));
		AddTri(Hair, Sph(30.0f, 100.0f, 10.4f), Sph(30.0f, 120.0f, 9.3f), Sph(50.0f, 100.0f, 9.3f));
		AddTri(Hair, Sph(40.0f, 200.0f, 11.0f), Sph(40.0f, 220.0f, 11.0f), Sph(60.0f, 200.0f, 11.0f));
		AddTri(Hair, Sph(130.0f, 0.0f, 11.0f), Sph(130.0f, 20.0f, 11.0f), Sph(150.0f, 0.0f, 11.0f));
		return Hair;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairHatFitJudgeTest,
	"Mantle.Runtime.HairHatFit.JudgeAndCornerRules",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairHatFitJudgeTest::RunTest(const FString& Parameters)
{
	using F = FMantleHairHatFit;

	// No hit: free. More than 0.5 cm inside: tucked.
	TestFalse(TEXT("no hat on the ray"), F::Judge(9.0f, -1.0f, -1.0f).bPokes);
	TestFalse(TEXT("0.7 cm inside the hat is tucked"), F::Judge(9.3f, 10.0f, -1.0f).bPokes);

	// Within 0.5 cm inside: pulled to 0.5 cm under the surface.
	F::FVertexVerdict V = F::Judge(9.6f, 10.0f, -1.0f);
	TestTrue(TEXT("0.4 cm inside the hat pokes"), V.bPokes);
	TestNearlyEqual(TEXT("0.4 cm inside: beyond is -0.4"), V.BeyondCm, -0.4f, 1.e-4f);
	TestNearlyEqual(TEXT("0.4 cm inside: pulled to 0.5 under the surface"), V.TargetCm, 9.5f, 1.e-4f);

	// Beyond the surface.
	V = F::Judge(10.4f, 10.0f, -1.0f);
	TestTrue(TEXT("beyond the hat pokes"), V.bPokes);
	TestNearlyEqual(TEXT("beyond the hat: 0.4 cm"), V.BeyondCm, 0.4f, 1.e-4f);
	TestNearlyEqual(TEXT("beyond the hat: pulled to 0.5 under the surface"), V.TargetCm, 9.5f, 1.e-4f);

	// With a scalp: room, a tight hat that stops at the clearance, and a hat with no room (the hat wins, the point goes under the skin).
	V = F::Judge(10.3f, 10.0f, 9.0f);
	TestTrue(TEXT("loose hat: room"), V.bRoom);
	TestNearlyEqual(TEXT("loose hat: 0.5 under the surface"), V.TargetCm, 9.5f, 1.e-4f);
	V = F::Judge(10.3f, 9.5f, 9.0f);
	TestTrue(TEXT("tight hat: still room"), V.bRoom);
	TestNearlyEqual(TEXT("tight hat: stops 0.1 above the scalp"), V.TargetCm, 9.1f, 1.e-4f);
	V = F::Judge(10.3f, 9.15f, 9.0f);
	TestFalse(TEXT("hat on the scalp: no room"), V.bRoom);
	TestNearlyEqual(TEXT("hat on the scalp: the hat wins"), V.TargetCm, 8.65f, 1.e-4f);

	// Never outward.
	V = F::Judge(9.52f, 10.0f, 9.45f);
	TestTrue(TEXT("poking, but already under the clearance floor"), V.bPokes);
	TestEqual(TEXT("a pull-in never pushes a point outward"), V.TargetCm, 9.52f);

	// Which corners count toward dropping a triangle.
	TestFalse(TEXT("tucked is not lost"), F::CornerIsLost(F::Judge(9.3f, 10.0f, 9.0f), false));
	TestFalse(TEXT("tucked is not lost for facial hair either"), F::CornerIsLost(F::Judge(9.3f, 10.0f, 9.0f), true));
	TestFalse(TEXT("0.3 beyond with room can be pulled in"), F::CornerIsLost(F::Judge(10.3f, 10.0f, 9.0f), false));
	TestTrue(TEXT("0.3 beyond is lost for facial hair (no pull-in)"), F::CornerIsLost(F::Judge(10.3f, 10.0f, 9.0f), true));
	TestTrue(TEXT("1 cm beyond is lost"), F::CornerIsLost(F::Judge(11.0f, 10.0f, 9.0f), false));
	TestTrue(TEXT("no room is lost"), F::CornerIsLost(F::Judge(10.3f, 9.15f, 9.0f), false));
	TestFalse(TEXT("no hat is not lost"), F::CornerIsLost(F::Judge(11.0f, -1.0f, 9.0f), true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairHatFitDomeTest,
	"Mantle.Runtime.HairHatFit.DomeCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairHatFitDomeTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairHatFitTestPrivate;

	const FSoup Scalp = Head();
	const FSoup Hat = Dome(HatRadiusCm, 100.0f);
	const FMantleSkinnedMeshLOD Hair = DomeHair();
	const FMantleHairHatFitResult Result = Run(Hair, Hat, Scalp);

	// Only the one poking corner (triangle 1, point 3) moves; the other points stay exactly where they were.
	TestTrue(TEXT("every point but the poking corner is untouched"), OnlyMoved(Hair, Result, { 3 }));
	TestEqual(TEXT("one point pulled"), Result.PointsPulled, 1);
	TestEqual(TEXT("one triangle dropped"), Result.TrianglesDropped, 1);
	TestEqual(TEXT("it is the one with all three corners through"), Result.DroppedTriangles, TArray<int32>({ 2 }));
	TestTrue(TEXT("the time is measured"), Result.Ms >= 0.0);

	const FVector3f Before = Hair.Positions[3];
	const float HatCm = HitAlong(Hat, Before);
	TestTrue(TEXT("the poking corner is beyond the hat"), HatCm > 0.0f && Before.Size() > HatCm);
	TestNearlyEqual(TEXT("pulled to 0.5 cm under the hat's surface"), Result.Positions[3].Size(), HatCm - 0.5f, 2.e-3f);
	TestTrue(TEXT("pulled along its ray"), FVector3f::DotProduct(Result.Positions[3].GetSafeNormal(), Before.GetSafeNormal()) > 0.99999f);

	// Below the dome's rim: no hit, left alone, even though it is far outside the head.
	for (int32 K = 0; K < 3; ++K)
	{
		TestTrue(TEXT("hair below the dome meets no hat"), HitAlong(Hat, Corner(Hair, 3, K)) < 0.0f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairHatFitClearanceTest,
	"Mantle.Runtime.HairHatFit.ScalpClearance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairHatFitClearanceTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairHatFitTestPrivate;

	const FSoup Scalp = Head();
	// One corner through the hat, the others well inside it.
	FMantleSkinnedMeshLOD Hair;
	AddTri(Hair, Sph(40.0f, 0.0f, 9.8f), Sph(40.0f, 20.0f, 8.5f), Sph(60.0f, 0.0f, 8.5f));
	const FVector3f Poking = Hair.Positions[0];

	// A hat 0.45 cm off the skin: the pull-in would go under the scalp, so it stops 0.1 cm above it.
	{
		const FSoup Hat = Dome(9.45f, 100.0f);
		const FMantleHairHatFitResult Result = Run(Hair, Hat, Scalp);
		TestTrue(TEXT("tight hat: only the poking corner moved"), OnlyMoved(Hair, Result, { 0 }));
		TestEqual(TEXT("tight hat: nothing dropped"), Result.TrianglesDropped, 0);
		TestNearlyEqual(TEXT("tight hat: 0.1 cm above the scalp"), Result.Positions[0].Size(), HitAlong(Scalp, Poking) + 0.1f, 2.e-3f);
	}
	// A hat on the scalp: the hat wins and the corner goes 0.5 cm under its surface (under the skin, hidden by the head).
	{
		const FSoup Hat = Dome(9.1f, 100.0f);
		const FMantleHairHatFitResult Result = Run(Hair, Hat, Scalp);
		TestTrue(TEXT("hat on the scalp: only the poking corner moved"), OnlyMoved(Hair, Result, { 0 }));
		TestNearlyEqual(TEXT("hat on the scalp: 0.5 under the hat"), Result.Positions[0].Size(), HitAlong(Hat, Poking) - 0.5f, 2.e-3f);
		TestTrue(TEXT("hat on the scalp: below the scalp"), Result.Positions[0].Size() < HitAlong(Scalp, Poking));
	}
	// No scalp given: no clearance.
	{
		const FSoup Hat = Dome(9.45f, 100.0f);
		const FSoup NoScalp{};
		const FMantleHairHatFitResult Result = Run(Hair, Hat, NoScalp);
		TestNearlyEqual(TEXT("no scalp: 0.5 under the hat"), Result.Positions[0].Size(), HitAlong(Hat, Poking) - 0.5f, 2.e-3f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairHatFitBrimTest,
	"Mantle.Runtime.HairHatFit.BrimHat",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairHatFitBrimTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairHatFitTestPrivate;

	const FSoup Scalp = Head();
	const FSoup Hat = BrimHat();
	FMantleSkinnedMeshLOD Hair;
	// 0: below the brim, hanging at the sides: its rays leave through the open bottom of the hat.
	AddTri(Hair, FVector3f(12.0f, 0.0f, -6.0f), FVector3f(12.0f * FMath::Cos(0.35f), 12.0f * FMath::Sin(0.35f), -6.0f), FVector3f(9.5f, 0.0f, -5.0f));
	// 1: all three corners through the crown.
	AddTri(Hair, Sph(50.0f, 0.0f, 11.0f), Sph(50.0f, 20.0f, 11.0f), Sph(70.0f, 0.0f, 11.0f));
	// 2: one corner through the crown, two tucked.
	AddTri(Hair, Sph(50.0f, 100.0f, 10.4f), Sph(50.0f, 120.0f, 9.3f), Sph(70.0f, 100.0f, 9.3f));
	// 3: tucked under the crown.
	AddTri(Hair, Sph(50.0f, 200.0f, 9.3f), Sph(50.0f, 220.0f, 9.3f), Sph(70.0f, 200.0f, 9.3f));

	for (int32 K = 0; K < 3; ++K)
	{
		TestTrue(TEXT("hair below the brim meets no hat"), HitAlong(Hat, Corner(Hair, 0, K)) < 0.0f);
	}
	const FMantleHairHatFitResult Result = Run(Hair, Hat, Scalp);
	TestTrue(TEXT("only the one poking corner moved"), OnlyMoved(Hair, Result, { 6 }));
	TestEqual(TEXT("only the fully poking triangle is dropped"), Result.DroppedTriangles, TArray<int32>({ 1 }));
	TestEqual(TEXT("one point pulled"), Result.PointsPulled, 1);
	TestNearlyEqual(TEXT("pulled to 0.5 cm under the crown"), Result.Positions[6].Size(), HitAlong(Hat, Hair.Positions[6]) - 0.5f, 2.e-3f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairHatFitOpenTopTest,
	"Mantle.Runtime.HairHatFit.OpenTopHat",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairHatFitOpenTopTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairHatFitTestPrivate;

	const FSoup Scalp = Head();
	const FSoup Hat = OpenTopHat();
	FMantleSkinnedMeshLOD Hair;
	// 0: up through the open top.
	AddTri(Hair, FVector3f(3.0f, 0.0f, 6.0f), FVector3f(0.0f, 3.0f, 6.0f), FVector3f(-3.0f, 0.0f, 7.0f));
	// 1: one corner through the band, two tucked.
	AddTri(Hair, FVector3f(10.4f, 0.0f, 1.0f), FVector3f(9.0f, 2.0f, 1.2f), FVector3f(9.0f, -2.0f, 0.8f));
	// 2: all three corners through the band.
	AddTri(Hair, FVector3f(11.0f, 0.0f, 0.0f), FVector3f(11.0f, 2.0f, 1.0f), FVector3f(11.0f, -2.0f, 2.0f));
	// 3: below the band.
	AddTri(Hair, FVector3f(11.0f, 0.0f, -5.0f), FVector3f(11.0f, 2.0f, -5.0f), FVector3f(9.0f, 0.0f, -6.0f));

	for (const int32 T : { 0, 3 })
	{
		for (int32 K = 0; K < 3; ++K)
		{
			TestTrue(TEXT("hair through the open top, or below the band, meets no hat"), HitAlong(Hat, Corner(Hair, T, K)) < 0.0f);
		}
	}
	const FMantleHairHatFitResult Result = Run(Hair, Hat, Scalp);
	TestTrue(TEXT("only the one poking corner moved"), OnlyMoved(Hair, Result, { 3 }));
	TestEqual(TEXT("only the fully poking triangle is dropped"), Result.DroppedTriangles, TArray<int32>({ 2 }));
	TestEqual(TEXT("one point pulled"), Result.PointsPulled, 1);
	TestNearlyEqual(TEXT("pulled to 0.5 cm under the band"), Result.Positions[3].Size(), HitAlong(Hat, Hair.Positions[3]) - 0.5f, 2.e-3f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairHatFitRulesTest,
	"Mantle.Runtime.HairHatFit.HideAllLeaveAloneAndFacialHair",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairHatFitRulesTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairHatFitTestPrivate;

	const FSoup Scalp = Head();
	const FSoup Hat = Dome(HatRadiusCm, 100.0f);
	const FMantleSkinnedMeshLOD Hair = DomeHair();

	// leaveAlone: nothing changes, poking or not.
	{
		const FMantleHairHatFitResult Result = Run(Hair, Hat, Scalp, EMantleHairHatRule::LeaveAlone);
		TestTrue(TEXT("leaveAlone keeps every point"), SamePositions(Result.Positions, Hair.Positions));
		TestEqual(TEXT("leaveAlone drops nothing"), Result.TrianglesDropped, 0);
		TestEqual(TEXT("leaveAlone pulls nothing"), Result.PointsPulled, 0);
	}
	// hideAll: every triangle of the slot goes; the points stay as they were.
	{
		const FMantleHairHatFitResult Result = Run(Hair, Hat, Scalp, EMantleHairHatRule::HideAll);
		TestEqual(TEXT("hideAll drops every triangle"), Result.DroppedTriangles, TArray<int32>({ 0, 1, 2, 3 }));
		TestEqual(TEXT("hideAll counts them"), Result.TrianglesDropped, 4);
		TestTrue(TEXT("hideAll moves nothing"), SamePositions(Result.Positions, Hair.Positions));
		TestEqual(TEXT("hideAll pulls nothing"), Result.PointsPulled, 0);
	}
	// facial hair: the poke test, drop only. A corner through the hat stays where it is; a triangle with all three through goes.
	{
		const FMantleHairHatFitResult Result = Run(Hair, Hat, Scalp, EMantleHairHatRule::UnderIt, true);
		TestTrue(TEXT("facial hair is never pulled in"), SamePositions(Result.Positions, Hair.Positions));
		TestEqual(TEXT("facial hair pulls nothing"), Result.PointsPulled, 0);
		TestEqual(TEXT("facial hair: the fully poking triangle goes"), Result.DroppedTriangles, TArray<int32>({ 2 }));
	}
	// hideAll never hides facial hair whole: it gets the same drop-only test.
	{
		const FMantleHairHatFitResult Result = Run(Hair, Hat, Scalp, EMantleHairHatRule::HideAll, true);
		TestEqual(TEXT("hideAll on facial hair is the drop-only test"), Result.DroppedTriangles, TArray<int32>({ 2 }));
		TestTrue(TEXT("hideAll on facial hair moves nothing"), SamePositions(Result.Positions, Hair.Positions));
	}
	// A triangle just through the hat (0.3 cm inside the surface): hair is pulled in and kept, facial hair has no pull-in so it goes.
	{
		FMantleSkinnedMeshLOD Shallow;
		AddTri(Shallow, Sph(40.0f, 0.0f, 9.7f), Sph(40.0f, 20.0f, 9.7f), Sph(60.0f, 0.0f, 9.7f));
		const FMantleHairHatFitResult AsHair = Run(Shallow, Hat, Scalp);
		TestEqual(TEXT("shallow poke: hair is kept"), AsHair.TrianglesDropped, 0);
		TestEqual(TEXT("shallow poke: all three corners pulled"), AsHair.PointsPulled, 3);
		for (int32 K = 0; K < 3; ++K)
		{
			TestNearlyEqual(TEXT("shallow poke: pulled to 0.5 under the hat"), AsHair.Positions[K].Size(), HitAlong(Hat, Shallow.Positions[K]) - 0.5f, 2.e-3f);
		}
		const FMantleHairHatFitResult AsFacial = Run(Shallow, Hat, Scalp, EMantleHairHatRule::UnderIt, true);
		TestEqual(TEXT("shallow poke: facial hair goes"), AsFacial.DroppedTriangles, TArray<int32>({ 0 }));
		TestTrue(TEXT("shallow poke: facial hair is not moved"), SamePositions(AsFacial.Positions, Shallow.Positions));
	}
	// No hat triangles at all: nothing happens.
	{
		const FSoup NoHat{};
		const FMantleHairHatFitResult Result = Run(Hair, NoHat, Scalp);
		TestTrue(TEXT("no hat: nothing moves"), SamePositions(Result.Positions, Hair.Positions));
		TestEqual(TEXT("no hat: nothing dropped"), Result.TrianglesDropped, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairHatFitGridTest,
	"Mantle.Runtime.HairHatFit.GridMatchesBruteForceAndIsDeterministic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairHatFitGridTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairHatFitTestPrivate;

	const FSoup Scalp = Head();
	const FSoup Hat = BrimHat(32);

	// The grid answers exactly what the brute-force loop does, for random directions and for the awkward ones (axes, cube-face edges and corners).
	FMantleHairHatGrid Grid;
	Grid.Build(FVector3f::ZeroVector, Hat.P, Hat.I);
	TestTrue(TEXT("the grid holds the hat"), Grid.NumTriangles() == Hat.I.Num() / 3);
	TArray<FVector3f> Directions = {
		FVector3f(1, 0, 0), FVector3f(-1, 0, 0), FVector3f(0, 1, 0), FVector3f(0, -1, 0), FVector3f(0, 0, 1), FVector3f(0, 0, -1),
		FVector3f(1, 1, 0).GetSafeNormal(), FVector3f(1, 0, 1).GetSafeNormal(), FVector3f(0, -1, -1).GetSafeNormal(), FVector3f(1, 1, 1).GetSafeNormal(),
		FVector3f(-1, 1, -1).GetSafeNormal(), FVector3f(1, -1, 0.2f).GetSafeNormal() };
	FRandomStream Random(20241124);
	for (int32 I = 0; I < 1500; ++I)
	{
		Directions.Add(FVector3f(Random.VRand()));
	}
	int32 Hits = 0, Misses = 0, Mismatches = 0;
	for (const FVector3f& Direction : Directions)
	{
		const float FromGrid = Grid.FirstHit(Direction);
		const float FromLoop = FMantleHairHatGrid::FirstHitBruteForce(FVector3f::ZeroVector, Direction, Hat.P, Hat.I);
		Mismatches += FromGrid == FromLoop ? 0 : 1;
		(FromLoop > 0.0f ? Hits : Misses) += 1;
	}
	TestEqual(TEXT("the grid and the brute-force loop never disagree"), Mismatches, 0);
	TestTrue(TEXT("the directions include hits"), Hits > 100);
	TestTrue(TEXT("the directions include misses"), Misses > 100);

	// A whole fit of scattered hair: grid, grid again, and brute force give the very same answer.
	FMantleSkinnedMeshLOD Hair;
	for (int32 I = 0; I < 400; ++I)
	{
		const float Theta = Random.FRandRange(0.0f, 170.0f);
		const float Phi = Random.FRandRange(0.0f, 360.0f);
		const float R = Random.FRandRange(8.0f, 14.0f);
		AddTri(Hair,
			Sph(Theta + Random.FRandRange(-8.0f, 8.0f), Phi + Random.FRandRange(-8.0f, 8.0f), R + Random.FRandRange(-1.0f, 1.0f)),
			Sph(Theta + Random.FRandRange(-8.0f, 8.0f), Phi + Random.FRandRange(-8.0f, 8.0f), R + Random.FRandRange(-1.0f, 1.0f)),
			Sph(Theta + Random.FRandRange(-8.0f, 8.0f), Phi + Random.FRandRange(-8.0f, 8.0f), R + Random.FRandRange(-1.0f, 1.0f)));
	}
	const FMantleHairHatFitResult A = Run(Hair, Hat, Scalp);
	const FMantleHairHatFitResult B = Run(Hair, Hat, Scalp);
	const FMantleHairHatFitResult C = Run(Hair, Hat, Scalp, EMantleHairHatRule::UnderIt, false, false, true);
	TestTrue(TEXT("the scattered hair has points to pull"), A.PointsPulled > 0);
	TestTrue(TEXT("the scattered hair has triangles to drop"), A.TrianglesDropped > 0 && A.TrianglesDropped < 400);
	TestTrue(TEXT("same input, same positions"), SamePositions(A.Positions, B.Positions));
	TestEqual(TEXT("same input, same drops"), A.DroppedTriangles, B.DroppedTriangles);
	TestTrue(TEXT("grid and brute force: same positions"), SamePositions(A.Positions, C.Positions));
	TestEqual(TEXT("grid and brute force: same drops"), A.DroppedTriangles, C.DroppedTriangles);
	TestEqual(TEXT("grid and brute force: same pulls"), A.PointsPulled, C.PointsPulled);

	const FMantleHairHatFitResult FacialA = Run(Hair, Hat, Scalp, EMantleHairHatRule::UnderIt, true);
	const FMantleHairHatFitResult FacialC = Run(Hair, Hat, Scalp, EMantleHairHatRule::UnderIt, true, false, true);
	TestEqual(TEXT("facial hair: grid and brute force drop the same"), FacialA.DroppedTriangles, FacialC.DroppedTriangles);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairHatFitSpeckTest,
	"Mantle.Runtime.HairHatFit.SpeckCleanUp",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairHatFitSpeckTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairHatFitTestPrivate;

	const FSoup Scalp = Head();
	const FSoup Hat = Dome(HatRadiusCm, 100.0f);

	// One card: a strip of 40 quads all through the hat (80 triangles), plus a tiny triangle hanging off its last point whose other two corners are tucked.
	const auto MakeHair = [](const int32 NumFiller)
	{
		FMantleSkinnedMeshLOD Hair;
		for (int32 Row = 0; Row <= 40; ++Row)
		{
			const float Theta = 40.0f + 1.4f * static_cast<float>(Row);
			Hair.Positions.Add(Sph(Theta, -0.5f, 11.0f));
			Hair.Positions.Add(Sph(Theta, 0.5f, 11.0f));
		}
		for (int32 Row = 0; Row < 40; ++Row)
		{
			const uint32 A = Row * 2, B = Row * 2 + 1, C = Row * 2 + 2, D = Row * 2 + 3;
			Hair.Indices.Append({ A, B, C, B, D, C });
		}
		const uint32 Last = 80;
		const uint32 Base = Hair.Positions.Num();
		Hair.Positions.Add(Sph(80.0f, -0.5f, 9.3f));
		Hair.Positions.Add(Sph(80.0f, -0.4f, 9.3f));
		Hair.Indices.Append({ Last, Base, Base + 1 });
		// Filler: loose triangles well inside the hat, each its own card, so the strip holds under 10% of the hair.
		for (int32 I = 0; I < NumFiller; ++I)
		{
			const float Phi = 0.45f * static_cast<float>(I);
			AddTri(Hair, Sph(25.0f, Phi, 9.3f), Sph(25.0f, Phi + 0.2f, 9.3f), Sph(27.0f, Phi, 9.3f));
		}
		return Hair;
	};

	{
		const FMantleSkinnedMeshLOD Hair = MakeHair(800);
		const FMantleHairHatFitResult Plain = Run(Hair, Hat, Scalp);
		TestEqual(TEXT("without clean-up the strip's 80 triangles go and the speck stays"), Plain.TrianglesDropped, 80);
		TestEqual(TEXT("no specks counted"), Plain.TrianglesDroppedAsSpecks, 0);

		const FMantleHairHatFitResult Cleaned = Run(Hair, Hat, Scalp, EMantleHairHatRule::UnderIt, false, true);
		TestEqual(TEXT("with clean-up the speck goes too"), Cleaned.TrianglesDropped, 81);
		TestEqual(TEXT("one speck counted"), Cleaned.TrianglesDroppedAsSpecks, 1);
		TestTrue(TEXT("the speck is the last triangle of the strip's card"), Cleaned.DroppedTriangles.Contains(80));
		TestFalse(TEXT("the filler stays"), Cleaned.DroppedTriangles.Contains(81));
	}
	{
		// With no filler the strip is the whole hair: a solid shell is one giant card and is never cleaned up.
		const FMantleSkinnedMeshLOD Hair = MakeHair(0);
		const FMantleHairHatFitResult Cleaned = Run(Hair, Hat, Scalp, EMantleHairHatRule::UnderIt, false, true);
		TestEqual(TEXT("a card holding most of the hair keeps its leftovers"), Cleaned.TrianglesDropped, 80);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairHatFitApplyDropsTest,
	"Mantle.Runtime.HairHatFit.ApplyDropsFixesSections",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairHatFitApplyDropsTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairHatFitTestPrivate;

	// Four triangles in two sections: triangles 0 and 1 in the first, 2 and 3 in the second.
	const auto Make = []()
	{
		FMantleSkinnedMeshLOD LOD;
		for (int32 T = 0; T < 4; ++T)
		{
			AddTri(LOD, FVector3f(T, 0, 0), FVector3f(T, 1, 0), FVector3f(T, 0, 1));
		}
		LOD.Sections.SetNum(2);
		LOD.Sections[0].MaterialIndex = 0;
		LOD.Sections[0].FirstIndex = 0;
		LOD.Sections[0].IndexCount = 6;
		LOD.Sections[1].MaterialIndex = 1;
		LOD.Sections[1].FirstIndex = 6;
		LOD.Sections[1].IndexCount = 6;
		return LOD;
	};

	{
		FMantleSkinnedMeshLOD LOD = Make();
		FMantleHairHatFit::ApplyDrops(LOD, { 1 });
		TestEqual(TEXT("one triangle out of the index list"), LOD.Indices.Num(), 9);
		TestEqual(TEXT("first section: 1 triangle"), LOD.Sections[0].IndexCount, 3);
		TestEqual(TEXT("first section starts at 0"), LOD.Sections[0].FirstIndex, 0);
		TestEqual(TEXT("second section moves up"), LOD.Sections[1].FirstIndex, 3);
		TestEqual(TEXT("second section keeps its two triangles"), LOD.Sections[1].IndexCount, 6);
		TestEqual(TEXT("the kept triangles are in order"), LOD.Indices, TArray<uint32>({ 0, 1, 2, 6, 7, 8, 9, 10, 11 }));
		TestEqual(TEXT("positions are untouched"), LOD.Positions.Num(), 12);
	}
	{
		// A section left with nothing: zero triangles, its start where the next section would begin, and no range overlaps.
		FMantleSkinnedMeshLOD LOD = Make();
		FMantleHairHatFit::ApplyDrops(LOD, { 2, 3 });
		TestEqual(TEXT("two triangles left"), LOD.Indices.Num(), 6);
		TestEqual(TEXT("first section is whole"), LOD.Sections[0].IndexCount, 6);
		TestEqual(TEXT("emptied section has no indices"), LOD.Sections[1].IndexCount, 0);
		TestEqual(TEXT("emptied section starts at the end"), LOD.Sections[1].FirstIndex, 6);
		TestEqual(TEXT("emptied section keeps its material"), LOD.Sections[1].MaterialIndex, 1);
	}
	{
		FMantleSkinnedMeshLOD LOD = Make();
		FMantleHairHatFit::ApplyDrops(LOD, { 0, 1, 2, 3 });
		TestEqual(TEXT("everything dropped: no indices"), LOD.Indices.Num(), 0);
		TestEqual(TEXT("everything dropped: first section empty"), LOD.Sections[0].IndexCount, 0);
		TestEqual(TEXT("everything dropped: second section empty"), LOD.Sections[1].IndexCount, 0);
	}
	{
		FMantleSkinnedMeshLOD LOD = Make();
		FMantleHairHatFit::ApplyDrops(LOD, {});
		TestEqual(TEXT("nothing dropped: nothing changes"), LOD.Indices.Num(), 12);
		TestEqual(TEXT("nothing dropped: second section stays"), LOD.Sections[1].FirstIndex, 6);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairHatFitHeadCentreTest,
	"Mantle.Runtime.HairHatFit.HeadCentreFromSkin",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairHatFitHeadCentreTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairHatFitTestPrivate;

	// A head of radius 9 whose centre is at (1, -2, 10): the top is 19, so the head centre is 12 high, 7 below the top.
	const FSoup Skin = Dome(HeadRadiusCm, 180.0f, 48, FVector3f(1.0f, -2.0f, 10.0f));
	const FVector3f Centre = FMantleHairHatFit::HeadCentreFromSkin(Skin.P, Skin.I);
	TestNearlyEqual(TEXT("centre X"), Centre.X, 1.0f, 1.e-3f);
	TestNearlyEqual(TEXT("centre Y"), Centre.Y, -2.0f, 1.e-3f);
	TestNearlyEqual(TEXT("centre Z is 7 cm below the top"), Centre.Z, 12.0f, 1.e-3f);

	// Points no triangle uses (the rest of a face mesh) do not count.
	FSoup WithStray = Skin;
	WithStray.P.Add(FVector3f(0.0f, 0.0f, 100.0f));
	const FVector3f Same = FMantleHairHatFit::HeadCentreFromSkin(WithStray.P, WithStray.I);
	TestNearlyEqual(TEXT("an unused point does not raise the top"), Same.Z, 12.0f, 1.e-3f);

	TestEqual(TEXT("no skin, no centre"), FMantleHairHatFit::HeadCentreFromSkin(TArray<FVector3f>(), TArray<uint32>()), FVector3f::ZeroVector);
	return true;
}

// Rule b (EMantleHairHatAim::UnderHatCards, the #389 plan as first written), for the side-by-side comparison with rule a.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairHatFitCardsDomeTest,
	"Mantle.Runtime.HairHatFit.RuleB.DomeCards",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairHatFitCardsDomeTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairHatFitTestPrivate;

	const FSoup Scalp = Head();
	const FSoup Hat = Dome(HatRadiusCm, 100.0f);
	FMantleSkinnedMeshLOD Hair;
	// A (points 0-5, triangles 0-3): wholly under the dome, tucked 0.7 cm inside it.
	AddCardStrip(Hair, 0.0f, { FVector2f(30.0f, 9.3f), FVector2f(35.0f, 9.3f), FVector2f(40.0f, 9.3f) });
	// B (points 6-11, triangles 4-5): two loose triangles welded by one shared spot. The first is below the dome's rim; the
	// second has that spot and two points under the dome. Alone the second would be mostly under the hat; welded, the card is
	// 2 of 6 points under it and stays.
	AddTri(Hair, Sph(115.0f, 100.0f, 11.0f), Sph(115.0f, 110.0f, 11.0f), Sph(125.0f, 100.0f, 11.0f));
	AddTri(Hair, Sph(115.0f, 100.0f, 11.0f), Sph(90.0f, 100.0f, 9.3f), Sph(90.0f, 110.0f, 9.3f));
	// C (points 12-17, triangles 6-9): crosses the rim, one row of two (of three) under the dome and poking 0.3 cm out of it.
	AddCardStrip(Hair, 200.0f, { FVector2f(95.0f, 10.3f), FVector2f(110.0f, 11.0f), FVector2f(120.0f, 11.0f) });
	// D (points 18-23, triangles 10-13): crosses the rim with two rows of three poking through the dome: mostly under the hat.
	AddCardStrip(Hair, 300.0f, { FVector2f(80.0f, 10.3f), FVector2f(90.0f, 10.3f), FVector2f(110.0f, 11.0f) });

	for (const int32 V : { 6, 7, 8, 14, 15, 16, 17, 22, 23 })
	{
		TestTrue(TEXT("the points below the rim meet no hat"), HitAlong(Hat, Hair.Positions[V]) < 0.0f);
	}
	for (const int32 V : { 0, 1, 2, 3, 4, 5, 10, 11, 12, 13, 18, 19, 20, 21 })
	{
		TestTrue(TEXT("the points under the dome meet it"), HitAlong(Hat, Hair.Positions[V]) > 0.0f);
	}

	const FMantleHairHatFitResult Result = RunCards(Hair, Hat, Scalp);
	TestEqual(TEXT("rule b: the card under the dome and the card mostly under it go whole"), Result.DroppedTriangles,
		TArray<int32>({ 0, 1, 2, 3, 10, 11, 12, 13 }));
	TestEqual(TEXT("rule b: two cards hidden"), Result.CardsHidden, 2);
	TestEqual(TEXT("rule b: the dropped triangles are counted"), Result.TrianglesDropped, 8);
	TestTrue(TEXT("rule b: only the crossing card's poking row moved (the hidden card's points are put back)"), OnlyMoved(Hair, Result, { 12, 13 }));
	TestEqual(TEXT("rule b: two points pulled"), Result.PointsPulled, 2);
	for (const int32 V : { 12, 13 })
	{
		TestNearlyEqual(TEXT("rule b: the crossing card's points under the hat end 0.5 cm under it"), Result.Positions[V].Size(),
			HitAlong(Hat, Hair.Positions[V]) - 0.5f, 2.e-3f);
		TestTrue(TEXT("rule b: pulled along its ray"), FVector3f::DotProduct(Result.Positions[V].GetSafeNormal(), Hair.Positions[V].GetSafeNormal()) > 0.99999f);
	}

	// Rule a on the same hair: nothing is tucked deep enough to drop or far enough out to cut; both poking rows are pulled in.
	const FMantleHairHatFitResult RuleA = Run(Hair, Hat, Scalp);
	TestEqual(TEXT("rule a: nothing dropped"), RuleA.TrianglesDropped, 0);
	TestEqual(TEXT("rule a: no cards"), RuleA.CardsHidden, 0);
	TestTrue(TEXT("rule a: the poking rows of C and D moved"), OnlyMoved(Hair, RuleA, { 12, 13, 18, 19, 20, 21 }));
	TestEqual(TEXT("rule a: six points pulled"), RuleA.PointsPulled, 6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairHatFitCardsBrimTest,
	"Mantle.Runtime.HairHatFit.RuleB.BrimCards",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairHatFitCardsBrimTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairHatFitTestPrivate;

	const FSoup Scalp = Head();
	const FSoup Hat = BrimHat();
	FMantleSkinnedMeshLOD Hair;
	// E (points 0-5, triangles 0-3): hanging below the brim: its rays leave through the hat's open bottom.
	AddCardStrip(Hair, 0.0f, { FVector2f(115.0f, 12.0f), FVector2f(125.0f, 12.0f), FVector2f(135.0f, 12.0f) });
	// F (points 6-11, triangles 4-7): crosses the brim; one row of three pokes 0.3 cm out of the crown, two hang below the brim.
	AddCardStrip(Hair, 120.0f, { FVector2f(95.0f, 10.3f), FVector2f(110.0f, 12.0f), FVector2f(120.0f, 12.0f) });
	// G (points 12-17, triangles 8-11): crosses the brim with two rows of three through the crown: mostly under the hat.
	AddCardStrip(Hair, 240.0f, { FVector2f(70.0f, 10.3f), FVector2f(85.0f, 10.3f), FVector2f(115.0f, 12.0f) });

	for (const int32 V : { 0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 16, 17 })
	{
		TestTrue(TEXT("hair below the brim meets no hat"), HitAlong(Hat, Hair.Positions[V]) < 0.0f);
	}
	for (const int32 V : { 6, 7, 12, 13, 14, 15 })
	{
		TestTrue(TEXT("hair through the crown meets the hat"), HitAlong(Hat, Hair.Positions[V]) > 0.0f);
	}

	const FMantleHairHatFitResult Result = RunCards(Hair, Hat, Scalp);
	TestEqual(TEXT("rule b: only the card mostly under the hat goes, its part below the brim too"), Result.DroppedTriangles, TriangleRange(8, 12));
	TestEqual(TEXT("rule b: one card hidden"), Result.CardsHidden, 1);
	TestTrue(TEXT("rule b: hair below the brim and the crossing card's outside part are untouched"), OnlyMoved(Hair, Result, { 6, 7 }));
	TestEqual(TEXT("rule b: two points pulled"), Result.PointsPulled, 2);
	for (const int32 V : { 6, 7 })
	{
		TestNearlyEqual(TEXT("rule b: the crossing card's inside points end 0.5 cm under the crown"), Result.Positions[V].Size(),
			HitAlong(Hat, Hair.Positions[V]) - 0.5f, 2.e-3f);
	}

	// Rule a keeps every card and pulls in both poking rows.
	const FMantleHairHatFitResult RuleA = Run(Hair, Hat, Scalp);
	TestEqual(TEXT("rule a: nothing dropped"), RuleA.TrianglesDropped, 0);
	TestTrue(TEXT("rule a: the poking rows of F and G moved"), OnlyMoved(Hair, RuleA, { 6, 7, 12, 13, 14, 15 }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairHatFitCardsRulesTest,
	"Mantle.Runtime.HairHatFit.RuleB.FacialHairGridAndDefault",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairHatFitCardsRulesTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairHatFitTestPrivate;

	TestTrue(TEXT("the default aim is rule a"), FMantleHairHatFitParams().Aim == EMantleHairHatAim::PokingThrough);

	const FSoup Scalp = Head();
	{
		// Facial hair ignores the aim: the poke test, drop only.
		const FSoup Hat = Dome(HatRadiusCm, 100.0f);
		const FMantleSkinnedMeshLOD Hair = DomeHair();
		const FMantleHairHatFitResult AsCards = RunCards(Hair, Hat, Scalp, true);
		const FMantleHairHatFitResult AsPoke = Run(Hair, Hat, Scalp, EMantleHairHatRule::UnderIt, true);
		TestEqual(TEXT("facial hair: the same drops under either aim"), AsCards.DroppedTriangles, AsPoke.DroppedTriangles);
		TestTrue(TEXT("facial hair: nothing moves under either aim"), SamePositions(AsCards.Positions, Hair.Positions));
		TestEqual(TEXT("facial hair: no cards hidden"), AsCards.CardsHidden, 0);
	}
	{
		// Scattered strips under a brim hat: grid and brute force give the very same rule b answer, and some of each outcome shows.
		const FSoup Hat = BrimHat(32);
		FRandomStream Random(20261124);
		FMantleSkinnedMeshLOD Hair;
		for (int32 Card = 0; Card < 120; ++Card)
		{
			const float Theta = Random.FRandRange(0.0f, 150.0f);
			const float Phi = Random.FRandRange(0.0f, 360.0f);
			const float Length = Random.FRandRange(10.0f, 50.0f);
			TArray<FVector2f> Rows;
			for (int32 Row = 0; Row < 5; ++Row)
			{
				Rows.Add(FVector2f(Theta + Length * static_cast<float>(Row) / 4.0f, Random.FRandRange(9.2f, 11.5f)));
			}
			AddCardStrip(Hair, Phi, Rows);
		}
		const FMantleHairHatFitResult A = RunCards(Hair, Hat, Scalp);
		const FMantleHairHatFitResult B = RunCards(Hair, Hat, Scalp, false, true);
		TestTrue(TEXT("scattered strips: some cards hidden"), A.CardsHidden > 0 && A.CardsHidden < 120);
		TestTrue(TEXT("scattered strips: some points pulled"), A.PointsPulled > 0);
		TestEqual(TEXT("scattered strips: a hidden card is 8 triangles"), A.TrianglesDropped, A.CardsHidden * 8);
		TestTrue(TEXT("grid and brute force: same positions"), SamePositions(A.Positions, B.Positions));
		TestEqual(TEXT("grid and brute force: same drops"), A.DroppedTriangles, B.DroppedTriangles);
		TestEqual(TEXT("grid and brute force: same cards"), A.CardsHidden, B.CardsHidden);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
