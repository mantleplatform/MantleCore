// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHairCapSplit.h"
#include "MantleHairFitReport.h"
#include "MantleHairCardsFit.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Cap detection (#51) and the preview's message conditions. Synthetic meshes, no assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleHairCapSplitTestPrivate
{
	/** A flat grid of Side x Side quads, Size cm across, at height Z, as an open sheet: Side*Side*2 triangles. */
	void CapTestSheet(int32 Side, float Size, float Z, TArray<FVector3f>& P, TArray<uint32>& I)
	{
		const uint32 Base = P.Num();
		for (int32 Y = 0; Y <= Side; ++Y) for (int32 X = 0; X <= Side; ++X) P.Add(FVector3f(Size * X / Side, Size * Y / Side, Z));
		const auto Idx = [Base, Side](int32 X, int32 Y) { return Base + static_cast<uint32>(Y * (Side + 1) + X); };
		for (int32 Y = 0; Y < Side; ++Y) for (int32 X = 0; X < Side; ++X)
			I.Append({Idx(X, Y), Idx(X + 1, Y), Idx(X + 1, Y + 1), Idx(X, Y), Idx(X + 1, Y + 1), Idx(X, Y + 1)});
	}

	/** Count small cards, 1 cm x 5 cm, two triangles each, spaced apart (separate pieces). */
	void CapTestCards(int32 Count, TArray<FVector3f>& P, TArray<uint32>& I)
	{
		for (int32 C = 0; C < Count; ++C)
		{
			const uint32 B = P.Num();
			const float X = 3.f * C;
			P.Add(FVector3f(X, 100.f, 0.f)); P.Add(FVector3f(X + 1.f, 100.f, 0.f)); P.Add(FVector3f(X + 1.f, 105.f, 0.f)); P.Add(FVector3f(X, 105.f, 0.f));
			I.Append({B, B + 1, B + 2, B, B + 2, B + 3});
		}
	}

	/** A big closed shell: the same sheet twice at one place, so every edge, the rim included, is used by two triangles. */
	void CapTestClosedShell(TArray<FVector3f>& P, TArray<uint32>& I)
	{
		CapTestSheet(10, 20.f, 0.f, P, I);
		CapTestSheet(10, 20.f, 0.f, P, I);
	}

	/** The upper half of a sphere of radius R cm at the origin, open at its rim: a cap over a head of a little less than R. */
	void CapTestDome(float R, TArray<FVector3f>& P, TArray<uint32>& I)
	{
		const int32 Rings = 8, Segments = 24;
		const uint32 Base = P.Num();
		P.Add(FVector3f(0.f, 0.f, R));
		for (int32 Ring = 1; Ring <= Rings; ++Ring)
		{
			const float Phi = 0.5f * UE_PI * Ring / Rings;
			for (int32 Seg = 0; Seg < Segments; ++Seg)
			{
				const float Theta = 2.f * UE_PI * Seg / Segments;
				P.Add(FVector3f(R * FMath::Sin(Phi) * FMath::Cos(Theta), R * FMath::Sin(Phi) * FMath::Sin(Theta), R * FMath::Cos(Phi)));
			}
		}
		const auto At = [Base, Segments](int32 Ring, int32 Seg) { return Ring == 0 ? Base : Base + 1 + (Ring - 1) * Segments + Seg % Segments; };
		for (int32 Seg = 0; Seg < Segments; ++Seg) I.Append({At(0, 0), At(1, Seg), At(1, Seg + 1)});
		for (int32 Ring = 1; Ring < Rings; ++Ring) for (int32 Seg = 0; Seg < Segments; ++Seg)
			I.Append({At(Ring, Seg), At(Ring + 1, Seg), At(Ring + 1, Seg + 1), At(Ring, Seg), At(Ring + 1, Seg + 1), At(Ring, Seg + 1)});
	}

	/**
	 * Cards 0.8 cm wide rooted on a dome of radius R (polar angles 15 to 60 degrees, 12 round each), each combed down over
	 * the dome to its rim, its tip lifting 1.5 cm off: the cards of a capped pack.
	 */
	void CapTestCombedCards(float R, TArray<FVector3f>& P, TArray<uint32>& I)
	{
		for (const float ThetaDeg : {15.f, 30.f, 45.f, 60.f})
		{
			for (int32 J = 0; J < 12; ++J)
			{
				const float Phi = 2.f * UE_PI * (J + ThetaDeg / 60.f) / 12.f;
				const FVector3f Out(FMath::Cos(Phi), FMath::Sin(Phi), 0.f), Side(-0.4f * FMath::Sin(Phi), 0.4f * FMath::Cos(Phi), 0.f);
				const uint32 B = P.Num();
				const int32 Steps = 4;
				for (int32 K = 0; K <= Steps; ++K)
				{
					const float Theta = FMath::DegreesToRadians(ThetaDeg + (90.f - ThetaDeg) * K / Steps);
					const FVector3f S = (R + 0.1f + 1.5f * K / Steps) * (Out * FMath::Sin(Theta) + FVector3f(0.f, 0.f, FMath::Cos(Theta)));
					P.Add(S - Side); P.Add(S + Side);
				}
				for (int32 K = 0; K < Steps; ++K) I.Append({B + 2 * K, B + 2 * K + 1, B + 2 * K + 3, B + 2 * K, B + 2 * K + 3, B + 2 * K + 2});
			}
		}
	}

	/** A UV sphere, outward windings, radius R cm, at the origin. */
	void CapTestSphere(float R, int32 Rings, int32 Segments, TArray<FVector3f>& P, TArray<uint32>& I)
	{
		const uint32 Base = P.Num();
		for (int32 Ring = 0; Ring <= Rings; ++Ring)
		{
			const float Phi = UE_PI * Ring / Rings;
			for (int32 Seg = 0; Seg < Segments; ++Seg)
			{
				const float Theta = 2.f * UE_PI * Seg / Segments;
				P.Add(FVector3f(R * FMath::Sin(Phi) * FMath::Cos(Theta), R * FMath::Sin(Phi) * FMath::Sin(Theta), R * FMath::Cos(Phi)));
			}
		}
		for (int32 Ring = 0; Ring < Rings; ++Ring) for (int32 Seg = 0; Seg < Segments; ++Seg)
		{
			const uint32 A = Base + Ring * Segments + Seg, B = Base + Ring * Segments + (Seg + 1) % Segments;
			const uint32 C = A + Segments, D = B + Segments;
			I.Append({A, B, D, A, D, C});
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairCapSplitFindsCapTest,
	"Mantle.Editor.HairCapSplit.FindsTheCapAndSplitsItFromTheCards",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairCapSplitFindsCapTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairCapSplitTestPrivate;
	TArray<FVector3f> P; TArray<uint32> I;
	CapTestCards(30, P, I);
	const int32 CardTriangles = I.Num() / 3;
	CapTestSheet(10, 20.f, 0.f, P, I);
	const int32 CapTriangles = I.Num() / 3 - CardTriangles;

	// The cards come first, the cap after, in sections "Cards" and "Hair_Mat".
	TArray<int32> Sections;
	for (int32 T = 0; T < CardTriangles; ++T) Sections.Add(0);
	for (int32 T = 0; T < CapTriangles; ++T) Sections.Add(1);
	const TArray<FString> Names = {TEXT("Cards"), TEXT("Hair_Mat")};

	const FMantleHairCapSplit::FResult Result = FMantleHairCapSplit::Split(P, I, Sections, Names);
	TestTrue(TEXT("a cap is found by its size"), Result.bFound);
	TestEqual(TEXT("cap triangles"), Result.CapIndices.Num() / 3, CapTriangles);
	TestEqual(TEXT("card triangles"), Result.CardIndices.Num() / 3, CardTriangles);
	TestEqual(TEXT("one cap piece"), Result.CapPieceCount, 1);
	TestEqual(TEXT("the cap's section is named"), Result.CapSection, FString(TEXT("Hair_Mat")));
	TestEqual(TEXT("pieces counted"), Result.PieceCount, 31);
	bool bCapUsesCardVertex = false;
	for (const uint32 V : Result.CapIndices) if (V < 30u * 4u) bCapUsesCardVertex = true;
	TestFalse(TEXT("the cap's triangles use only the cap's vertices"), bCapUsesCardVertex);

	// A cap too small for the size test is still found when its section is named like one.
	TArray<FVector3f> P2; TArray<uint32> I2;
	CapTestCards(30, P2, I2);
	CapTestSheet(8, 6.f, 0.f, P2, I2); // 128 triangles over 36 cm2 against cards of 5 cm2: ratio 7, under 15, over 4
	TArray<int32> Sections2;
	for (int32 T = 0; T < I2.Num() / 3; ++T) Sections2.Add(T < 60 ? 0 : 1);
	TestFalse(TEXT("unnamed, a small cap is not taken for one"), FMantleHairCapSplit::Split(P2, I2, Sections2, {TEXT("Cards"), TEXT("Mat")}).bFound);
	TestTrue(TEXT("named like a cap, it is"), FMantleHairCapSplit::Split(P2, I2, Sections2, {TEXT("Cards"), TEXT("HairCap")}).bFound);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairCapSplitNoCapTest,
	"Mantle.Editor.HairCapSplit.LeavesCardsOnlyMeshesAlone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairCapSplitNoCapTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairCapSplitTestPrivate;
	{
		TArray<FVector3f> P; TArray<uint32> I;
		CapTestCards(40, P, I);
		const FMantleHairCapSplit::FResult Result = FMantleHairCapSplit::Split(P, I, {}, {});
		TestFalse(TEXT("cards only: no cap"), Result.bFound);
		TestEqual(TEXT("every triangle stays a card"), Result.CardIndices.Num(), I.Num());
		TestTrue(TEXT("the note says there is none"), Result.Describe().Contains(TEXT("none found")));
	}
	{
		// One big open sheet and one card: too few pieces to be split from anything.
		TArray<FVector3f> P; TArray<uint32> I;
		CapTestCards(1, P, I);
		CapTestSheet(10, 20.f, 0.f, P, I);
		TestFalse(TEXT("two pieces are not enough"), FMantleHairCapSplit::Split(P, I, {}, {}).bFound);
	}
	{
		// A big closed shell is not a cap (a cap has an open edge).
		TArray<FVector3f> P; TArray<uint32> I;
		CapTestCards(30, P, I);
		CapTestClosedShell(P, I);
		TestFalse(TEXT("closed: not a cap"), FMantleHairCapSplit::Split(P, I, {}, {}).bFound);
	}
	{
		// Four big open sheets are a pack of sheets, not a cap.
		TArray<FVector3f> P; TArray<uint32> I;
		CapTestCards(30, P, I);
		for (int32 S = 0; S < 4; ++S) CapTestSheet(10, 20.f, 50.f * (S + 1), P, I);
		TestFalse(TEXT("too many big pieces: nothing split"), FMantleHairCapSplit::Split(P, I, {}, {}).bFound);
	}
	{
		// A triangle that names a missing vertex leaves everything as cards.
		TArray<FVector3f> P = {FVector3f::ZeroVector, FVector3f(1, 0, 0), FVector3f(0, 1, 0)};
		const TArray<uint32> I = {0, 1, 7};
		const FMantleHairCapSplit::FResult Result = FMantleHairCapSplit::Split(P, I, {}, {});
		TestFalse(TEXT("bad index: not found"), Result.bFound);
		TestEqual(TEXT("bad index: cards untouched"), Result.CardIndices.Num(), 3);
	}
	TestTrue(TEXT("cap names"), FMantleHairCapSplit::IsCapName(TEXT("Hair_Cap")) && FMantleHairCapSplit::IsCapName(TEXT("scalp")) && !FMantleHairCapSplit::IsCapName(TEXT("Cards")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairFitReportTest,
	"Mantle.Editor.HairFitReport.MessageConditions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairFitReportTest::RunTest(const FString& Parameters)
{
	// A clean fit: the headline only.
	FMantleHairFitReport::FInput Good;
	Good.RootsSeatedPercent = 80.0;
	Good.CardVertices = 1000;
	FMantleHairFitReport::FResult Result = FMantleHairFitReport::Describe(Good);
	TestEqual(TEXT("headline"), Result.Headline, FString(TEXT("We sized your hair to fit the standard head")));
	TestFalse(TEXT("clean fit: confident"), Result.bLowConfidence);
	TestFalse(TEXT("clean fit: no baked head"), Result.bBakedHead);
	TestEqual(TEXT("clean fit: no messages"), Result.Messages.Num(), 0);
	TestEqual(TEXT("confirm label"), FString(FMantleHairFitReport::ConfirmLabel()), FString(TEXT("Looks good")));
	TestEqual(TEXT("adjust label"), FString(FMantleHairFitReport::AdjustLabel()), FString(TEXT("Adjust")));

	// Each low-confidence condition on its own.
	{ FMantleHairFitReport::FInput In = Good; In.bKeepsOutOfHead = false; TestTrue(TEXT("no outside fit"), FMantleHairFitReport::Describe(In).bLowConfidence); }
	{ FMantleHairFitReport::FInput In = Good; In.bScalpCheckFit = true; TestTrue(TEXT("long lifts"), FMantleHairFitReport::Describe(In).bLowConfidence); }
	{ FMantleHairFitReport::FInput In = Good; In.CardInsideAfter = 50; TestTrue(TEXT("5% still inside"), FMantleHairFitReport::Describe(In).bLowConfidence); }
	{ FMantleHairFitReport::FInput In = Good; In.CardInsideAfter = 10; TestFalse(TEXT("1% inside is fine"), FMantleHairFitReport::Describe(In).bLowConfidence); }
	{ FMantleHairFitReport::FInput In = Good; In.RootsSeatedPercent = 20.0; TestTrue(TEXT("few roots on the scalp"), FMantleHairFitReport::Describe(In).bLowConfidence); }
	{ FMantleHairFitReport::FInput In = Good; In.RootsSeatedPercent = 40.0; TestFalse(TEXT("40% of roots on the scalp is fine (long hair)"), FMantleHairFitReport::Describe(In).bLowConfidence); }
	{ FMantleHairFitReport::FInput In = Good; In.RootsSeatedPercent = 20.0; In.bHadCap = true; TestFalse(TEXT("roots do not count with a cap"), FMantleHairFitReport::Describe(In).bLowConfidence); }
	{ FMantleHairFitReport::FInput In = Good; In.RootsSeatedPercent = -1.0; TestFalse(TEXT("roots unmeasured"), FMantleHairFitReport::Describe(In).bLowConfidence); }
	{
		FMantleHairFitReport::FInput In = Good; In.bKeepsOutOfHead = false;
		const FMantleHairFitReport::FResult Low = FMantleHairFitReport::Describe(In);
		TestEqual(TEXT("low confidence says so once"), Low.Messages.Num(), 1);
		TestTrue(TEXT("and points to Adjust"), Low.Messages[0].Contains(TEXT("Adjust")));
	}

	// A baked-in head.
	{
		FMantleHairFitReport::FInput In = Good; In.bBakedHead = true; In.BakedHeadSection = TEXT("Head_Mat");
		const FMantleHairFitReport::FResult Baked = FMantleHairFitReport::Describe(In);
		TestTrue(TEXT("baked head flagged"), Baked.bBakedHead);
		TestEqual(TEXT("baked head message"), Baked.Messages.Num(), 1);
		TestTrue(TEXT("it names the section"), Baked.Messages[0].Contains(TEXT("Head_Mat")));
		TestFalse(TEXT("a baked head alone is not low confidence"), Baked.bLowConfidence);
	}
	TestTrue(TEXT("head names"), FMantleHairFitReport::IsBakedHeadName(TEXT("M_Head")) && FMantleHairFitReport::IsBakedHeadName(TEXT("Face_skin")) && FMantleHairFitReport::IsBakedHeadName(TEXT("Body")));
	TestFalse(TEXT("cap and scalp are not heads"), FMantleHairFitReport::IsBakedHeadName(TEXT("Head_Cap")) || FMantleHairFitReport::IsBakedHeadName(TEXT("Scalp_skin")));
	TestFalse(TEXT("cards are not heads"), FMantleHairFitReport::IsBakedHeadName(TEXT("Hair_Cards")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairCardsFitRunTest,
	"Mantle.Editor.HairCardsFit.CapReachesTheFitAndABakedHeadIsLeftOut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairCardsFitRunTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairCapSplitTestPrivate;
	TArray<FVector3f> HeadP; TArray<uint32> HeadI;
	CapTestSphere(9.f, 16, 24, HeadP, HeadI);

	// Hair: 48 cards combed over a cap (section 0) and the cap, a dome over the head (section 1); the same again with a
	// huge built-in head (section 2).
	TArray<FVector3f> P; TArray<uint32> I; TArray<int32> Sections;
	CapTestCombedCards(9.3f, P, I);
	for (int32 T = 0; T < I.Num() / 3; ++T) Sections.Add(0);
	CapTestDome(9.3f, P, I);
	while (Sections.Num() < I.Num() / 3) Sections.Add(1);
	const TArray<FString> Names = {TEXT("Cards"), TEXT("Hair_Cap"), TEXT("Head_Mat")};

	FMantleHairCardsFit::FInput Input;
	Input.HeadPositions = HeadP; Input.HeadIndices = HeadI;
	Input.HairPositions = P; Input.HairIndices = I; Input.TriangleSections = Sections; Input.SectionNames = Names;
	const FMantleHairCardsFit::FResult Plain = FMantleHairCardsFit::Run(Input);
	TestTrue(TEXT("fit ran"), Plain.bFitted);
	TestEqual(TEXT("positions keep their count"), Plain.Positions.Num(), P.Num());
	TestTrue(FString::Printf(TEXT("the cap reached the scalp fit (%s)"), *FString::Join(Plain.Notes, TEXT(" | "))), Plain.bCapFound);
	TestTrue(TEXT("and the notes say so"), Plain.Notes.ContainsByPredicate([](const FString& N) { return N.Contains(TEXT("found 1 cap")); }));
	TestFalse(TEXT("no built-in head"), Plain.Report.bBakedHead);
	TestEqual(TEXT("upright hair is not turned"), Plain.QuarterTurns, 0);

	// The same hair lying on its side (a Y-up file, either way round) is found, turned upright and lands where the upright one did.
	for (const int32 Lying : {-1, 1})
	{
		TArray<FVector3f> Sideways = P;
		for (FVector3f& V : Sideways) V = FMantleHairCardsFit::RotateUpright(V, Lying);
		FMantleHairCardsFit::FInput Turned = Input;
		Turned.HairPositions = Sideways;
		const FMantleHairCardsFit::FResult Fixed = FMantleHairCardsFit::Run(Turned);
		TestEqual(TEXT("sideways hair is turned back"), Fixed.QuarterTurns, -Lying);
		bool bBack = Fixed.Positions.Num() == P.Num();
		for (int32 V = 0; bBack && V < P.Num(); ++V) bBack &= Fixed.Positions[V].Equals(Plain.Positions[V], 1.e-2f);
		TestTrue(TEXT("and fits as the upright hair did"), bBack);
	}

	TArray<FVector3f> P2 = P; TArray<uint32> I2 = I; TArray<int32> Sections2 = Sections;
	CapTestSphere(80.f, 12, 16, P2, I2);
	while (Sections2.Num() < I2.Num() / 3) Sections2.Add(2);
	FMantleHairCardsFit::FInput Baked = Input;
	Baked.HairPositions = P2; Baked.HairIndices = I2; Baked.TriangleSections = Sections2;
	const FMantleHairCardsFit::FResult WithHead = FMantleHairCardsFit::Run(Baked);
	TestTrue(TEXT("fit ran with a built-in head"), WithHead.bFitted);
	TestTrue(TEXT("the built-in head is reported"), WithHead.Report.bBakedHead);
	TestEqual(TEXT("sizing is the same as without it (the head did not size the piece)"), WithHead.Notes[0], Plain.Notes[0]);
	bool bSame = true;
	for (int32 V = 0; V < P.Num(); ++V) bSame &= WithHead.Positions[V].Equals(Plain.Positions[V], 1.e-3f);
	TestTrue(TEXT("hair lands in the same place with or without it"), bSame);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairCardsFitClearFaceTest,
	"Mantle.Editor.HairCardsFit.OnlyFaceCoverageTheFitAddedIsUndone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairCardsFitClearFaceTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairCapSplitTestPrivate;
	TArray<FVector3f> HeadP; TArray<uint32> HeadI;
	CapTestSphere(9.f, 16, 24, HeadP, HeadI);
	FMantleHairHeadSamples Head; FString Error;
	if (!TestTrue(TEXT("head samples build"), Head.Build(HeadP, HeadI, Error))) { AddError(Error); return false; }

	// The sphere is centred on the origin (radius 9, top 9, +Y forward): at z = -5 the skin is in the face zone.
	const FVector3f Fringe(0.f, 8.f, -5.f);   // in front of the face
	const FVector3f Clear(0.f, 12.f, -5.f);   // well in front of it
	const FVector3f Crown(0.f, 0.f, 9.1f);
	TestTrue(TEXT("the fringe spot covers the face"), FMantleHairCardsFit::CoversFace(Head, FVector(Fringe)));
	TestFalse(TEXT("the clear spot does not"), FMantleHairCardsFit::CoversFace(Head, FVector(Clear)));
	TestFalse(TEXT("the crown does not"), FMantleHairCardsFit::CoversFace(Head, FVector(Crown)));

	// 0: a deliberate fringe, over the face as made and after the fit. 1: a lock the fit pushed onto the face. 2: the crown. 3: moved but still clear.
	const TArray<FVector3f> Baseline = {Fringe, Clear, Crown, Clear};
	TArray<FVector3f> Fitted = {Fringe + FVector3f(0.f, 0.2f, 0.f), Fringe, Crown, Clear + FVector3f(0.f, 1.f, 0.f)};
	const TArray<FVector3f> FittedBefore = Fitted;
	const int32 Restored = FMantleHairCardsFit::ClearFaceAddedByTheFit(Head, TArray<int32>({0, 1, 2, 3}), Baseline, Fitted);
	TestEqual(TEXT("one vertex went back"), Restored, 1);
	TestTrue(TEXT("the deliberate fringe stays"), Fitted[0].Equals(FittedBefore[0], 1.e-5f));
	TestTrue(TEXT("the lock the fit pushed onto the face goes back"), Fitted[1].Equals(Baseline[1], 1.e-5f));
	TestTrue(TEXT("the crown stays"), Fitted[2].Equals(FittedBefore[2], 1.e-5f));
	TestTrue(TEXT("hair that is still clear stays"), Fitted[3].Equals(FittedBefore[3], 1.e-5f));
	// Nothing added by the fit, nothing changes.
	TArray<FVector3f> Same = Baseline;
	TestEqual(TEXT("an unchanged fit restores nothing"), FMantleHairCardsFit::ClearFaceAddedByTheFit(Head, TArray<int32>({0, 1, 2, 3}), Baseline, Same), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairFitMeshesHeadInHairFrameTest,
	"Mantle.Editor.HairFitReport.TheBeforePictureHeadIsScaledToTheAuthoredHair",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairFitMeshesHeadInHairFrameTest::RunTest(const FString& Parameters)
{
	// Hair authored at 1/10 size and 100 cm elsewhere, fitted by scale 10 and a shift: the head must come back to the hair's frame.
	FMantleHairFitMeshes Meshes;
	const TArray<FVector3f> Authored = {FVector3f(0.f, 0.f, 0.f), FVector3f(1.f, 0.f, 0.f), FVector3f(0.f, 2.f, 0.f), FVector3f(0.f, 0.f, 3.f)};
	for (const FVector3f& P : Authored)
	{
		Meshes.BeforePositions.Add(P + FVector3f(100.f, 50.f, 20.f));
		Meshes.AfterPositions.Add(P * 10.f + FVector3f(0.f, 5.f, 160.f));
	}
	Meshes.HeadPositions = {FVector3f(0.f, 5.f, 160.f), FVector3f(10.f, 5.f, 160.f)};
	Meshes.HairTriangles = {0, 1, 2};
	Meshes.HeadTriangles = {0, 1, 0};
	const TArray<FVector3f> Head = Meshes.HeadInHairFrame();
	TestTrue(TEXT("the head's first point lands on the authored hair's origin"), Head[0].Equals(FVector3f(100.f, 50.f, 20.f), 1.e-3f));
	TestTrue(TEXT("and the head is a tenth of its size there"), Head[1].Equals(FVector3f(101.f, 50.f, 20.f), 1.e-3f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairCardsFitCapGuardTest,
	"Mantle.Editor.HairCardsFit.ABigSheetFarFromTheSkinIsHairNotACap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairCardsFitCapGuardTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairCapSplitTestPrivate;
	TArray<FVector3f> HeadP; TArray<uint32> HeadI;
	CapTestSphere(9.f, 16, 24, HeadP, HeadI);
	FMantleScalpHeadSurface Head; FString Error;
	if (!TestTrue(TEXT("head builds"), Head.Build(HeadP, HeadI, Error))) { AddError(Error); return false; }

	TArray<FVector3f> Near, Far; TArray<uint32> NearI, FarI;
	CapTestSheet(10, 4.f, 9.5f, Near, NearI); // a small sheet half a centimetre off the crown
	CapTestSheet(10, 4.f, 30.f, Far, FarI);  // the same sheet 21 cm above it
	const double NearMedian = FMantleHairCardsFit::MedianDistanceToSkin(Head, Near, NearI);
	const double FarMedian = FMantleHairCardsFit::MedianDistanceToSkin(Head, Far, FarI);
	TestTrue(TEXT("a close sheet hugs the scalp: a cap"), NearMedian <= FMantleHairCardsFit::CapMaxMedianDistanceCm);
	TestTrue(TEXT("a far sheet is hair, not a cap"), FarMedian > FMantleHairCardsFit::CapMaxMedianDistanceCm);
	TestEqual(TEXT("no triangles, no distance"), FMantleHairCardsFit::MedianDistanceToSkin(Head, Near, TConstArrayView<uint32>()), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairCardsFitCapShapeTest,
	"Mantle.Editor.HairCardsFit.PigtailsCloseToTheHeadAreHairNotACap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairCardsFitCapShapeTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairCapSplitTestPrivate;
	TArray<FVector3f> HeadP; TArray<uint32> HeadI;
	CapTestSphere(9.f, 16, 24, HeadP, HeadI);

	// A dome over the head is a cap.
	TArray<FVector3f> Dome; TArray<uint32> DomeI;
	CapTestDome(9.3f, Dome, DomeI);
	TestTrue(TEXT("a dome over the head is a cap"), FMantleHairCardsFit::CoversScalp(HeadP, Dome, DomeI));

	// Two pigtails (anime6): big pieces, each 4 cm across and 16 cm long, hanging at either side of the head. Together they
	// are as wide as the head but only 4 cm deep, so they are hair, not a cap, however close to the skin they hang.
	TArray<FVector3f> Tails; TArray<uint32> TailsI;
	for (const float X : {-9.f, 5.f})
	{
		TArray<FVector3f> Sheet; TArray<uint32> SheetI;
		CapTestSheet(8, 4.f, 0.f, Sheet, SheetI);
		const uint32 Base = Tails.Num();
		for (const FVector3f& V : Sheet) Tails.Add(FVector3f(X + V.X, V.Y - 2.f, 4.f * V.Y - 10.f)); // tilt it up: 4 cm wide, 4 cm deep, 16 cm tall
		for (const uint32 Index : SheetI) TailsI.Add(Base + Index);
	}
	TestFalse(TEXT("two pigtails are not a cap"), FMantleHairCardsFit::CoversScalp(HeadP, Tails, TailsI));
	TestFalse(TEXT("no triangles are not a cap"), FMantleHairCardsFit::CoversScalp(HeadP, Dome, TConstArrayView<uint32>()));
	return true;
}

#endif
