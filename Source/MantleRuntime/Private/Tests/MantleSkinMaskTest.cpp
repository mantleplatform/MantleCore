// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSkinMask.h"

#include "MantleBodyCoverage.h"
#include "MantleSkinnedMesh.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// #877, the skin under clothing (D:\Mantle\Untracked\skin-hide-1022\DESIGN.md section 8), on synthetic bodies: an
// arm (a closed tube 8 cm in radius, 60 cm long) in a sleeve 40 cm long, with or without a 4 cm window cut in it, and
// a flat patch of skin under a strap. No assets.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleSkinMaskTestPrivate
{
	constexpr float ArmRadius = 8.0f;
	constexpr float ArmHalfLength = 30.0f;
	constexpr float SleeveHalfLength = 20.0f;
	constexpr int32 Around = 48;

	/** The window's half-width (2 cm of arc at the sleeve) and half-height, centred at angle 0, z 0. */
	constexpr float WindowHalfCm = 2.0f;

	/**
	 * A tube around Z of Radius from Z0 to Z1 in Rings rings of Around vertices, normals and winding outward, turned by
	 * Phase steps around. A quad whose centre Skip(theta, z) takes is left out.
	 */
	void AddTube(TArray<FVector3f>& Positions, TArray<FVector3f>& Normals, TArray<uint32>& Indices, const float Radius, const float Z0, const float Z1,
		const int32 Rings, const int32 InAround, TFunctionRef<bool(float, float)> Skip, const float Phase = 0.0f)
	{
		const uint32 Base = Positions.Num();
		for (int32 Ring = 0; Ring < Rings; ++Ring)
		{
			const float Z = Z0 + (Z1 - Z0) * Ring / (Rings - 1);
			for (int32 Step = 0; Step < InAround; ++Step)
			{
				const float Theta = 2.0f * PI * (Step + Phase) / InAround;
				Positions.Add(FVector3f(Radius * FMath::Cos(Theta), Radius * FMath::Sin(Theta), Z));
				Normals.Add(FVector3f(FMath::Cos(Theta), FMath::Sin(Theta), 0.0f));
			}
		}
		for (int32 Ring = 0; Ring + 1 < Rings; ++Ring)
		{
			for (int32 Step = 0; Step < InAround; ++Step)
			{
				const float Theta = FMath::UnwindRadians(2.0f * PI * (Step + Phase + 0.5f) / InAround);
				const float Z = Z0 + (Z1 - Z0) * (Ring + 0.5f) / (Rings - 1);
				if (Skip(Theta, Z))
				{
					continue;
				}
				const uint32 A = Base + Ring * InAround + Step;
				const uint32 B = Base + Ring * InAround + (Step + 1) % InAround;
				const uint32 C = A + InAround;
				const uint32 D = B + InAround;
				Indices.Append({ A, B, D, A, D, C });
			}
		}
	}

	/** Closes the tube's ring of InAround vertices at First (at height Z) with a fan, facing +z (bUp) or -z. */
	void AddCap(TArray<FVector3f>& Positions, TArray<FVector3f>& Normals, TArray<uint32>& Indices, const uint32 First, const int32 InAround, const float Z, const bool bUp)
	{
		const uint32 Centre = Positions.Num();
		Positions.Add(FVector3f(0.0f, 0.0f, Z));
		Normals.Add(FVector3f(0.0f, 0.0f, bUp ? 1.0f : -1.0f));
		for (int32 Step = 0; Step < InAround; ++Step)
		{
			const uint32 A = First + Step;
			const uint32 B = First + (Step + 1) % InAround;
			if (bUp)
			{
				Indices.Append({ Centre, A, B });
			}
			else
			{
				Indices.Append({ Centre, B, A });
			}
		}
	}

	/** The arm: a closed tube, InAround around and Rings rings, as a one-section body LOD. */
	FMantleSkinnedMeshLOD Arm(const int32 Rings = 61, const int32 InAround = Around)
	{
		FMantleSkinnedMeshLOD Body;
		AddTube(Body.Positions, Body.Normals, Body.Indices, ArmRadius, -ArmHalfLength, ArmHalfLength, Rings, InAround, [](float, float) { return false; });
		AddCap(Body.Positions, Body.Normals, Body.Indices, 0, InAround, -ArmHalfLength, false);
		AddCap(Body.Positions, Body.Normals, Body.Indices, (Rings - 1) * InAround, InAround, ArmHalfLength, true);
		FMantleSkinnedMeshSection& Section = Body.Sections.AddDefaulted_GetRef();
		Section.IndexCount = Body.Indices.Num();
		return Body;
	}

	/**
	 * A sleeve GapCm off the arm over |z| <= SleeveHalfLength, open at both ends; bWindow cuts the window. Phase 0 puts a
	 * sleeve vertex straight out from every arm vertex (a garment made by pushing the body out along its normals: each
	 * normal ray goes exactly through a corner six sleeve triangles share); 0.5 turns it half a step.
	 */
	struct FSleeve
	{
		TArray<FVector3f> Positions;
		TArray<FVector3f> Normals;
		TArray<uint32> Indices;

		FSleeve(const float GapCm, const bool bWindow, const float Phase = 0.0f)
		{
			const float Radius = ArmRadius + GapCm;
			const float HalfAngle = WindowHalfCm / Radius;
			AddTube(Positions, Normals, Indices, Radius, -SleeveHalfLength, SleeveHalfLength, 41, Around, [bWindow, HalfAngle](const float Theta, const float Z)
			{
				return bWindow && FMath::Abs(Theta) < HalfAngle && FMath::Abs(Z) < WindowHalfCm;
			}, Phase);
		}

		FMantleGarmentSurface Surface() const { return FMantleGarmentSurface(Positions, Normals, Indices); }
	};

	/** A body point lies under the window (inside its outline, projected straight out). */
	bool UnderWindow(const FVector3f& P)
	{
		return FMath::Abs(FMath::Atan2(P.Y, P.X)) <= WindowHalfCm / ArmRadius && FMath::Abs(P.Z) <= WindowHalfCm;
	}

	/** The mask of Garment on Body as its own source body. */
	bool DeriveOn(const FMantleSkinnedMeshLOD& Body, const FMantleGarmentSurface& Garment, FMantleSkinMask& Out)
	{
		return FMantleSkinMask::Derive(Garment, Body.Positions, Body.Normals, Body.Indices, FMantleSkinMaskParams(), Out);
	}

	/** The smallest straight distance from a Hidden vertex to a vertex Visible picks; 1e9 when either is empty. */
	float NearestHiddenToVisible(TConstArrayView<FVector3f> Positions, const TBitArray<>& Hidden, TFunctionRef<bool(int32)> Visible)
	{
		float Nearest = 1.0e9f;
		for (TConstSetBitIterator<> It(Hidden); It; ++It)
		{
			for (int32 Other = 0; Other < Positions.Num(); ++Other)
			{
				if (Visible(Other))
				{
					Nearest = FMath::Min(Nearest, FVector3f::Dist(Positions[It.GetIndex()], Positions[Other]));
				}
			}
		}
		return Nearest;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSkinMaskEnclosedTest,
	"Mantle.Runtime.SkinMask.Enclosed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSkinMaskEnclosedTest::RunTest(const FString& Parameters)
{
	using namespace MantleSkinMaskTestPrivate;
	// A sleeve 0.5 cm off the arm: the skin deep inside it hides, the skin near its open ends and past them stays. Lined
	// up with the arm's vertices, then turned half a step.
	const FMantleSkinnedMeshLOD Body = Arm();
	for (const float Phase : { 0.0f, 0.5f })
	{
		const FSleeve Sleeve(0.5f, false, Phase);
		AddInfo(FString::Printf(TEXT("sleeve turned %.1f steps"), Phase));
		FMantleSkinMask Mask;
		if (!TestTrue(TEXT("the mask derives"), DeriveOn(Body, Sleeve.Surface(), Mask)))
		{
			return false;
		}
		AddInfo(Mask.ToString());
		int32 DeepKept = 0;
		int32 OutsideHidden = 0;
		for (int32 Vertex = 0; Vertex < Body.NumVertices(); ++Vertex)
		{
			const float Z = FMath::Abs(Body.Positions[Vertex].Z);
			DeepKept += Z <= 12.0f && !Mask.Hidden[Vertex] ? 1 : 0;
			OutsideHidden += Z >= SleeveHalfLength + FMantleSkinMask::DefaultBorderCm && Mask.Hidden[Vertex] ? 1 : 0;
		}
		TestEqual(TEXT("every vertex 8 cm or more inside the sleeve hides"), DeepKept, 0);
		TestEqual(TEXT("nothing hides within the border of the sleeve's ends or past them"), OutsideHidden, 0);
		TestTrue(TEXT("the mask hides something"), Mask.NumHidden > 0);
		const float Nearest = NearestHiddenToVisible(Body.Positions, Mask.Hidden, [&Mask](const int32 Other) { return !Mask.Enclosed[Other]; });
		// The nearest-visible search over the surface may land on a vertex a little farther than the nearest: 2 mm of slack.
		TestTrue(FString::Printf(TEXT("every hidden vertex is about 2.5 cm or more from skin that can be seen (nearest %.2f cm)"), Nearest),
			Nearest >= FMantleSkinMask::DefaultBorderCm - 0.2f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSkinMaskCutoutTest,
	"Mantle.Runtime.SkinMask.Cutout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSkinMaskCutoutTest::RunTest(const FString& Parameters)
{
	using namespace MantleSkinMaskTestPrivate;
	// A 4 cm window in the sleeve: the skin under it stays, with 2.5 cm more all round; the far side still hides. Lined up
	// with the arm's vertices, then turned half a step.
	const FMantleSkinnedMeshLOD Body = Arm();
	for (const float Phase : { 0.0f, 0.5f })
	{
		const FSleeve Sleeve(0.5f, true, Phase);
		AddInfo(FString::Printf(TEXT("sleeve turned %.1f steps"), Phase));
		FMantleSkinMask Mask;
		if (!TestTrue(TEXT("the mask derives"), DeriveOn(Body, Sleeve.Surface(), Mask)))
		{
			return false;
		}
		AddInfo(Mask.ToString());
		int32 WindowHidden = 0;
		int32 FarKept = 0;
		for (int32 Vertex = 0; Vertex < Body.NumVertices(); ++Vertex)
		{
			const FVector3f& P = Body.Positions[Vertex];
			WindowHidden += UnderWindow(P) && Mask.Hidden[Vertex] ? 1 : 0;
			FarKept += FMath::Abs(FMath::Atan2(P.Y, P.X)) > 2.5f && FMath::Abs(P.Z) <= 12.0f && !Mask.Hidden[Vertex] ? 1 : 0;
		}
		TestEqual(TEXT("no skin under the window hides"), WindowHidden, 0);
		TestEqual(TEXT("the far side of the sleeve still hides"), FarKept, 0);
		// The nearest-visible search over the surface may land on a vertex a little farther than the nearest: 2 mm of slack.
		const float Nearest = NearestHiddenToVisible(Body.Positions, Mask.Hidden, [&Body](const int32 Other) { return UnderWindow(Body.Positions[Other]); });
		TestTrue(FString::Printf(TEXT("every hidden vertex is about 2.5 cm or more from the window (nearest %.2f cm)"), Nearest),
			Nearest >= FMantleSkinMask::DefaultBorderCm - 0.2f);

		// The rule before #877 removes skin right at the window's edge.
		FMantleGarmentCoverage Old;
		TestTrue(TEXT("the old coverage runs"), FMantleGarmentCoverage::Compute(Sleeve.Surface(), Body.Positions, Body.Normals, Old));
		const float OldNearest = NearestHiddenToVisible(Body.Positions, Old.Covered, [&Body](const int32 Other) { return UnderWindow(Body.Positions[Other]); });
		TestTrue(FString::Printf(TEXT("the old rule hid skin within 1.5 cm of the window (nearest %.2f cm)"), OldNearest), OldNearest < 1.5f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSkinMaskStrapTest,
	"Mantle.Runtime.SkinMask.Strap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSkinMaskStrapTest::RunTest(const FString& Parameters)
{
	// A strap 2.5 cm wide, 0.3 cm over a patch of skin 20 cm square: grazing rays leave under it, so nothing hides.
	FMantleSkinnedMeshLOD Patch;
	constexpr int32 Points = 41;
	for (int32 Y = 0; Y < Points; ++Y)
	{
		for (int32 X = 0; X < Points; ++X)
		{
			Patch.Positions.Add(FVector3f(-10.0f + 0.5f * X, -10.0f + 0.5f * Y, 0.0f));
			Patch.Normals.Add(FVector3f(0.0f, 0.0f, 1.0f));
		}
	}
	for (int32 Y = 0; Y + 1 < Points; ++Y)
	{
		for (int32 X = 0; X + 1 < Points; ++X)
		{
			const uint32 A = Y * Points + X;
			Patch.Indices.Append({ A, A + 1, A + Points + 1, A, A + Points + 1, A + Points });
		}
	}
	TArray<FVector3f> StrapPositions;
	TArray<FVector3f> StrapNormals;
	TArray<uint32> StrapIndices;
	for (int32 Y = 0; Y < 61; ++Y)
	{
		for (int32 X = 0; X < 6; ++X)
		{
			StrapPositions.Add(FVector3f(-1.25f + 0.5f * X, -15.0f + 0.5f * Y, 0.3f));
			StrapNormals.Add(FVector3f(0.0f, 0.0f, 1.0f));
		}
	}
	for (int32 Y = 0; Y + 1 < 61; ++Y)
	{
		for (int32 X = 0; X + 1 < 6; ++X)
		{
			const uint32 A = Y * 6 + X;
			StrapIndices.Append({ A, A + 1, A + 7, A, A + 7, A + 6 });
		}
	}
	const FMantleGarmentSurface Strap(StrapPositions, StrapNormals, StrapIndices);
	FMantleSkinMask Mask;
	if (!TestTrue(TEXT("the mask derives"), FMantleSkinMask::Derive(Strap, Patch.Positions, Patch.Normals, Patch.Indices, FMantleSkinMaskParams(), Mask)))
	{
		return false;
	}
	AddInfo(Mask.ToString());
	TestTrue(TEXT("the strap covers skin by the old one-ray rule"), Mask.NormalCovered > 0);
	TestEqual(TEXT("nothing hides"), Mask.NumHidden, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSkinMaskDeterministicTest,
	"Mantle.Runtime.SkinMask.Deterministic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSkinMaskDeterministicTest::RunTest(const FString& Parameters)
{
	using namespace MantleSkinMaskTestPrivate;
	// The same inputs give the same bits every run (the plugin's preview equals the client).
	const FMantleSkinnedMeshLOD Body = Arm();
	const FSleeve Sleeve(0.5f, true);
	FMantleSkinMask First;
	FMantleSkinMask Second;
	TestTrue(TEXT("the first derive runs"), DeriveOn(Body, Sleeve.Surface(), First));
	TestTrue(TEXT("the second derive runs"), DeriveOn(Body, Sleeve.Surface(), Second));
	TestTrue(TEXT("the same enclosed bits"), First.Enclosed == Second.Enclosed);
	TestTrue(TEXT("the same depths"), First.DepthMm == Second.DepthMm);
	TestTrue(TEXT("the same hidden bits"), First.Hidden == Second.Hidden);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDressedBodySkinMaskTest,
	"Mantle.Runtime.DressedBody.SkinMaskAndWearer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDressedBodySkinMaskTest::RunTest(const FString& Parameters)
{
	using namespace MantleSkinMaskTestPrivate;
	const FMantleSkinnedMeshLOD Body = Arm();
	const FSleeve Sleeve(0.5f, false);
	const FMantleGarmentSurface Garments[] = { Sleeve.Surface() };

	// A mask that hides everything still hides only what this wearer's coverage covers (the safety net).
	FMantleSkinMask All;
	All.Enclosed.Init(true, Body.NumVertices());
	All.Hidden.Init(true, Body.NumVertices());
	All.DepthMm.Init(FMantleSkinMask::FarDepthMm, Body.NumVertices());
	FMantleDressedMask Masks[] = { { &All, FMantleSkinMask::DefaultBorderCm } };
	FMantleDressedBody Dressed;
	if (!TestTrue(TEXT("the body dresses with the mask"), FMantleDressedBody::Compute(Body, Garments, FMantleDressedBodyParams::SkinMaskDefaults(), Dressed, nullptr, Masks)))
	{
		return false;
	}
	AddInfo(Dressed.ToString());
	TestTrue(TEXT("the hidden bits are exactly this wearer's coverage"), Dressed.Hidden == Dressed.Coverage[0].Covered);
	TestEqual(TEXT("the rest the mask would hide is kept by the wearer"), Dressed.MaskKeptByWearer, Body.NumVertices() - Dressed.Coverage[0].NumCovered);
	TestEqual(TEXT("no garment on the fallback"), Dressed.FallbackGarments, 0);
	TestEqual(TEXT("no seam shrink"), Dressed.Seam.Points, 0);

	// Without a mask: the wearer's coverage, kept 2.5 cm inside its edge on this body.
	FMantleDressedBody Fallback;
	TestTrue(TEXT("the body dresses without a mask"), FMantleDressedBody::Compute(Body, Garments, FMantleDressedBodyParams::SkinMaskDefaults(), Fallback));
	AddInfo(Fallback.ToString());
	TestEqual(TEXT("one garment on the fallback"), Fallback.FallbackGarments, 1);
	const TBitArray<>& Covered = Fallback.Coverage[0].Covered;
	const float Nearest = NearestHiddenToVisible(Body.Positions, Fallback.Hidden, [&Covered](const int32 Other) { return !Covered[Other]; });
	TestTrue(FString::Printf(TEXT("every hidden vertex is about 2.5 cm or more from uncovered skin (nearest %.2f cm)"), Nearest),
		Nearest >= FMantleSkinMask::DefaultBorderCm - 0.2f);
	TestTrue(TEXT("the fallback still hides the middle of the sleeve"), Fallback.CoveredVertices > 0);

	// The skin-mask switch off: the masks are ignored, the old rule as before.
	FMantleDressedBody Off;
	TestTrue(TEXT("the body dresses with the rule off"), FMantleDressedBody::Compute(Body, Garments, FMantleDressedBodyParams(), Off, nullptr, Masks));
	TestTrue(TEXT("the old rule hides the coverage"), Off.Hidden == Off.Coverage[0].Covered);
	TestEqual(TEXT("and keeps no skin-mask numbers"), Off.SlackMm.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDressedBodyNoVisibleMoveTest,
	"Mantle.Runtime.DressedBody.NoVisibleMove",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDressedBodyNoVisibleMoveTest::RunTest(const FString& Parameters)
{
	using namespace MantleSkinMaskTestPrivate;
	// A sleeve 0.3 cm off the arm with its window: the sink pulls the skin under the cloth to 0.5 cm under it, never
	// more than 1 cm, and never moves skin the cloth does not cover (under the window, past the ends).
	const FMantleSkinnedMeshLOD Body = Arm();
	const FSleeve Sleeve(0.3f, true);
	FMantleSkinMask Mask;
	TestTrue(TEXT("the mask derives"), DeriveOn(Body, Sleeve.Surface(), Mask));
	const FMantleGarmentSurface Garments[] = { Sleeve.Surface() };
	const FMantleDressedMask Masks[] = { { &Mask, FMantleSkinMask::DefaultBorderCm } };
	FMantleDressedBody Dressed;
	if (!TestTrue(TEXT("the body dresses"), FMantleDressedBody::Compute(Body, Garments, FMantleDressedBodyParams::SkinMaskDefaults(), Dressed, nullptr, Masks)))
	{
		return false;
	}
	AddInfo(Dressed.ToString());
	int32 VisibleMoved = 0;
	float MaxMove = 0.0f;
	for (int32 Vertex = 0; Vertex < Body.NumVertices(); ++Vertex)
	{
		const float Move = FVector3f::Dist(Body.Positions[Vertex], Dressed.Positions[Vertex]);
		MaxMove = FMath::Max(MaxMove, Move);
		VisibleMoved += !Dressed.WearerCovered[Vertex] && Move > 0.0f ? 1 : 0;
	}
	TestEqual(TEXT("no vertex the cloth does not cover moves"), VisibleMoved, 0);
	TestTrue(FString::Printf(TEXT("nothing moves more than 1 cm (largest %.3f cm)"), MaxMove), MaxMove <= 1.0f + 1.0e-3f);
	TestTrue(TEXT("the skin under the cloth sinks"), Dressed.Sink.Sunk > 0);
	TestEqual(TEXT("no seam shrink"), Dressed.Seam.Points, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDressedBodyLodBorderTest,
	"Mantle.Runtime.DressedBody.LodBorder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDressedBodyLodBorderTest::RunTest(const FString& Parameters)
{
	using namespace MantleSkinMaskTestPrivate;
	// A coarser LOD1 (2 cm rings, half as many around) keeps a wider border: half its mean edge more.
	FMantleSkinnedMesh Mesh;
	Mesh.LODs.Add(Arm());
	Mesh.LODs.Add(Arm(31, Around / 2));
	const FMantleSkinnedMeshLOD& LOD0 = Mesh.LODs[0];
	const FMantleSkinnedMeshLOD& LOD1 = Mesh.LODs[1];
	const FSleeve Sleeve(0.5f, true);
	FMantleSkinMask Mask;
	TestTrue(TEXT("the mask derives"), DeriveOn(LOD0, Sleeve.Surface(), Mask));
	const FMantleGarmentSurface Garments[] = { Sleeve.Surface() };
	const FMantleDressedMask Masks[] = { { &Mask, FMantleSkinMask::DefaultBorderCm } };
	FMantleDressedBody Dressed;
	TestTrue(TEXT("the body dresses"), FMantleDressedBody::Compute(LOD0, Garments, FMantleDressedBodyParams::SkinMaskDefaults(), Dressed, nullptr, Masks));
	TArray<TBitArray<>> Lower;
	FString Error;
	if (!TestTrue(TEXT("the lower LODs' bits are made"), FMantleDressedBody::LowerLODHidden(Mesh, Dressed, Lower, &Error)) || !TestEqual(TEXT("one lower LOD"), Lower.Num(), 1)
		|| !TestEqual(TEXT("one bit per LOD1 vertex"), Lower[0].Num(), LOD1.NumVertices()))
	{
		return false;
	}
	double EdgeSum = 0.0;
	for (int32 Index = 0; Index < LOD1.Indices.Num(); Index += 3)
	{
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			EdgeSum += FVector3f::Dist(LOD1.Positions[LOD1.Indices[Index + Corner]], LOD1.Positions[LOD1.Indices[Index + (Corner + 1) % 3]]);
		}
	}
	const float NeedCm = FMantleSkinMask::DefaultBorderCm + 0.5f * static_cast<float>(EdgeSum / LOD1.Indices.Num());
	// A LOD1 vertex takes its nearest LOD0 vertex's bit, at most about one LOD0 spacing (1 cm) away.
	float LodNearest = 1.0e9f;
	for (TConstSetBitIterator<> It(Lower[0]); It; ++It)
	{
		for (int32 Vertex = 0; Vertex < LOD0.NumVertices(); ++Vertex)
		{
			if (!Mask.Enclosed[Vertex])
			{
				LodNearest = FMath::Min(LodNearest, FVector3f::Dist(LOD1.Positions[It.GetIndex()], LOD0.Positions[Vertex]));
			}
		}
	}
	TestTrue(TEXT("LOD1 hides something"), Lower[0].CountSetBits() > 0);
	TestTrue(FString::Printf(TEXT("every hidden LOD1 vertex keeps the wider border (nearest %.2f cm, needs %.2f less 1 cm)"), LodNearest, NeedCm),
		LodNearest >= NeedCm - 1.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSeeThroughAuditCutoutTest,
	"Mantle.Runtime.SkinMask.SeeThroughAudit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSeeThroughAuditCutoutTest::RunTest(const FString& Parameters)
{
	using namespace MantleSkinMaskTestPrivate;
	const FMantleSkinnedMeshLOD Body = Arm();
	const FSleeve Sleeve(0.5f, true);
	const FMantleGarmentSurface Garments[] = { Sleeve.Surface() };

	// The bare arm: nothing removed, nothing to see through.
	FMantleSeeThroughAudit Bare;
	TestTrue(TEXT("the bare arm audits"), FMantleSeeThroughAudit::Run(Body, Body.Indices, Garments, Bare));
	TestEqual(TEXT("the bare arm drops nothing"), Bare.DroppedTriangles, 0);

	// The rule before #877: skin removed at the window's edge is seen through the window at an angle.
	FMantleDressedBody Old;
	TestTrue(TEXT("the old rule dresses"), FMantleDressedBody::Compute(Body, Garments, FMantleDressedBodyParams(), Old));
	FMantleSeeThroughAudit OldAudit;
	TestTrue(TEXT("the old body audits"), FMantleSeeThroughAudit::Run(Body, Old.KeptIndices, Garments, OldAudit));
	AddInfo(FString::Printf(TEXT("old: %s"), *OldAudit.ToString()));
	TestTrue(TEXT("the old rule leaves holes to see through"), OldAudit.HoleRays > 0);

	// The skin mask: none.
	FMantleSkinMask Mask;
	TestTrue(TEXT("the mask derives"), DeriveOn(Body, Sleeve.Surface(), Mask));
	const FMantleDressedMask Masks[] = { { &Mask, FMantleSkinMask::DefaultBorderCm } };
	FMantleDressedBody New;
	TestTrue(TEXT("the skin-mask rule dresses"), FMantleDressedBody::Compute(Body, Garments, FMantleDressedBodyParams::SkinMaskDefaults(), New, nullptr, Masks));
	FMantleSeeThroughAudit NewAudit;
	TestTrue(TEXT("the new body audits"), FMantleSeeThroughAudit::Run(Body, New.KeptIndices, Garments, NewAudit));
	AddInfo(FString::Printf(TEXT("new: %s"), *NewAudit.ToString()));
	TestTrue(TEXT("the skin mask still removes skin"), NewAudit.DroppedTriangles > 0);
	TestEqual(TEXT("the skin mask leaves nothing to see through"), NewAudit.HoleRays, 0);
	return true;
}

#endif
