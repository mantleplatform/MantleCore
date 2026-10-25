// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHairPieceFit.h"
#include "MantleHairScalpFit.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The whole-piece fit (unit scale, scale/offset search) and the head samples it scores against.
// Synthetic head (a sphere), synthetic hair cards (short, and long hair far wider at the bottom than the head), no assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleHairPieceFitTestPrivate
{
	/** A UV sphere (radius 9 cm at (0, 0, 160), so its top vertex is at 169), outward windings, or inward when bInward. */
	void PieceFitSphere(bool bInward, TArray<FVector3f>& Positions, TArray<uint32>& Indices)
	{
		const int32 Rings = 24, Segments = 36;
		const double Radius = 9.0;
		const FVector Centre(0.0, 0.0, 160.0);
		Positions.Add(FVector3f(0.f, 0.f, static_cast<float>(Centre.Z + Radius)));
		for (int32 R = 1; R < Rings; ++R)
		{
			const double Theta = UE_DOUBLE_PI * R / Rings;
			for (int32 S = 0; S < Segments; ++S)
			{
				const double Phi = 2.0 * UE_DOUBLE_PI * S / Segments;
				const FVector P = Centre + Radius * FVector(FMath::Sin(Theta) * FMath::Cos(Phi), FMath::Sin(Theta) * FMath::Sin(Phi), FMath::Cos(Theta));
				Positions.Add(FVector3f(static_cast<float>(P.X), static_cast<float>(P.Y), static_cast<float>(P.Z)));
			}
		}
		const uint32 Bottom = Positions.Num();
		Positions.Add(FVector3f(0.f, 0.f, static_cast<float>(Centre.Z - Radius)));
		const auto Ring = [Segments](int32 R, int32 S) { return static_cast<uint32>(1 + (R - 1) * Segments + (S % Segments)); };
		const auto AddTriangle = [&](uint32 A, uint32 B, uint32 C)
		{
			const FVector3f& A3 = Positions[static_cast<int32>(A)];
			const FVector3f& B3 = Positions[static_cast<int32>(B)];
			const FVector3f& C3 = Positions[static_cast<int32>(C)];
			const FVector PA(A3.X, A3.Y, A3.Z), PB(B3.X, B3.Y, B3.Z), PC(C3.X, C3.Y, C3.Z);
			const bool bOutward = (((PB - PA) ^ (PC - PA)) | ((PA + PB + PC) / 3.0 - Centre)) > 0.0;
			if (bOutward != bInward) Indices.Append({A, B, C}); else Indices.Append({A, C, B});
		};
		for (int32 S = 0; S < Segments; ++S) AddTriangle(0, Ring(1, S), Ring(1, S + 1));
		for (int32 R = 1; R < Rings - 1; ++R) for (int32 S = 0; S < Segments; ++S)
		{
			AddTriangle(Ring(R, S), Ring(R + 1, S), Ring(R, S + 1));
			AddTriangle(Ring(R, S + 1), Ring(R + 1, S), Ring(R + 1, S + 1));
		}
		for (int32 S = 0; S < Segments; ++S) AddTriangle(Ring(Rings - 1, S), Bottom, Ring(Rings - 1, S + 1));
	}

	/**
	 * Hair cards in the head's frame (the sphere above: centre (0, 0, 160), radius 9). Each card is a strip 0.8 cm wide
	 * rooted 1 mm off the skin at a polar angle of 10 to 55 degrees, combed down over the sphere to the equator. Short hair
	 * stops there, its tip lifted 1.65 cm off the skin; long hair stays 1.5 mm off it to the equator. Long hair then falls 30 cm and flares out to 16 cm from the axis, so it is 32 cm wide at the
	 * bottom, far wider than at the scalp (as bought long hair is). Each card's first two vertices are its root end.
	 */
	void PieceFitCards(bool bLong, TArray<FVector3f>& P, TArray<uint32>& I)
	{
		const FVector Centre(0.0, 0.0, 160.0);
		const auto Add = [&P](const FVector& V) { P.Add(FVector3f(static_cast<float>(V.X), static_cast<float>(V.Y), static_cast<float>(V.Z))); };
		for (const double ThetaDeg : {10.0, 25.0, 40.0, 55.0})
		{
			for (int32 J = 0; J < 12; ++J)
			{
				const double Phi = 2.0 * UE_DOUBLE_PI * (J + 0.25 * (ThetaDeg / 15.0)) / 12.0;
				const FVector Out(FMath::Cos(Phi), FMath::Sin(Phi), 0.0);
				const FVector Side = FVector(-FMath::Sin(Phi), FMath::Cos(Phi), 0.0) * 0.4;
				TArray<FVector> Spine;
				const int32 Over = 6;
				for (int32 K = 0; K <= Over; ++K)
				{
					const double Theta = FMath::DegreesToRadians(ThetaDeg + (90.0 - ThetaDeg) * K / Over);
					// The root sits 1 mm off the skin; the card lifts off it towards its tip, as a real card does.
					const double R = K == 0 ? 9.1 : 9.15 + (bLong ? 0.0 : 1.5 * K / Over);
					Spine.Add(Centre + R * (Out * FMath::Sin(Theta) + FVector(0.0, 0.0, FMath::Cos(Theta))));
				}
				if (bLong)
				{
					for (int32 K = 1; K <= 6; ++K) Spine.Add(Centre + Out * (9.15 + (16.0 - 9.15) * K / 6.0) - FVector(0.0, 0.0, 30.0 * K / 6.0));
				}
				const uint32 Base = P.Num();
				for (const FVector& S : Spine) { Add(S - Side); Add(S + Side); }
				for (int32 K = 0; K + 1 < Spine.Num(); ++K)
				{
					const uint32 A = Base + 2 * K;
					I.Append({A, A + 1, A + 3, A, A + 3, A + 2});
				}
			}
		}
	}

	/** The vendor's copy of head-frame hair: Vendor = (Fitted - Offset) * Scale, so the fit should find 1 / Scale and Offset. */
	TArray<FVector3f> PieceFitVendorCopy(const TArray<FVector3f>& Fitted, float Scale, const FVector3f& Offset)
	{
		TArray<FVector3f> Out;
		for (const FVector3f& P : Fitted) Out.Add((P - Offset) * Scale);
		return Out;
	}

	/** How far, at most and at the median, the fit puts each card's root (its first vertex) from where it belongs, cm. */
	void PieceFitRootError(const FMantleHairPieceFit::FResult& Fit, const TArray<FVector3f>& Vendor, const TArray<FVector3f>& Fitted,
		bool bLong, double& OutMedian, double& OutWorst)
	{
		const int32 PerCard = 2 * (7 + (bLong ? 6 : 0));
		TArray<double> Errors;
		for (int32 V = 0; V < Fitted.Num(); V += PerCard) Errors.Add((Fit.Apply(Vendor[V]) - Fitted[V]).Size());
		Errors.Sort();
		OutMedian = Errors[Errors.Num() / 2];
		OutWorst = Errors.Last();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairPieceFitSearchGridTest,
	"Mantle.Editor.HairPieceFit.DefaultSearchReachesThreeTimesTheStartingSize",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairPieceFitSearchGridTest::RunTest(const FString& Parameters)
{
	const FMantleHairPieceFit::FSearch Search = FMantleHairPieceFit::FSearch::Default();
	TestEqual(TEXT("scales"), Search.Scales.Num(), 23);
	TestNearlyEqual(TEXT("first scale"), Search.Scales[0], 0.8, 1.e-9);
	TestNearlyEqual(TEXT("last scale"), Search.Scales.Last(), 3.0, 1.e-9);
	TestEqual(TEXT("y offsets"), Search.OffsetsY.Num(), 11);
	TestNearlyEqual(TEXT("first y offset"), Search.OffsetsY[0], -10.0, 1.e-9);
	TestNearlyEqual(TEXT("last y offset"), Search.OffsetsY.Last(), 10.0, 1.e-9);
	TestEqual(TEXT("top offsets"), Search.TopOffsets.Num(), 15);
	TestNearlyEqual(TEXT("first top offset"), Search.TopOffsets[0], -16.0, 1.e-9);
	TestNearlyEqual(TEXT("last top offset"), Search.TopOffsets.Last(), 12.0, 1.e-9);
	TestTrue(TEXT("a finer pass follows"), Search.FineScaleStep > 0.0 && Search.FineOffsetStep > 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairPieceFitHeadSamplesFaceOutTest,
	"Mantle.Editor.HairPieceFit.HeadSamplesFaceOutWhateverTheWinding",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairPieceFitHeadSamplesFaceOutTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairPieceFitTestPrivate;
	for (const bool bInward : {false, true})
	{
		TArray<FVector3f> Positions; TArray<uint32> Indices;
		PieceFitSphere(bInward, Positions, Indices);
		FMantleHairHeadSamples Head; FString Error;
		if (!TestTrue(TEXT("head builds"), Head.Build(Positions, Indices, Error))) { AddError(Error); return false; }
		TestNearlyEqual(TEXT("top of the head"), Head.TopCm(), 169.0, 1.e-4);
		int32 Wrong = 0;
		for (int32 I = 0; I < Head.Num(); ++I)
		{
			if (((Head.Point(I) - FVector(0.0, 0.0, 160.0)) | Head.Normal(I)) <= 0.0) ++Wrong;
		}
		TestEqual(bInward ? TEXT("inward-wound sphere: samples facing in") : TEXT("outward-wound sphere: samples facing in"), Wrong, 0);

		// The nearest sample to a point is within a few millimetres of it, on the near side.
		int32 Id = INDEX_NONE; double D2 = 0.0;
		Head.Nearest(FVector(0.0, 30.0, 160.0), Id, D2);
		TestTrue(TEXT("nearest sample is on the +Y side"), Id != INDEX_NONE && Head.Point(Id).Y > 8.0);
		TestTrue(TEXT("nearest sample is about 21 cm away"), FMath::Abs(FMath::Sqrt(D2) - 21.0) < 0.1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairPieceFitScalpTest,
	"Mantle.Editor.HairPieceFit.TheScalpIsTheTopAndBackOfTheHeadNotTheFace",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairPieceFitScalpTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairPieceFitTestPrivate;
	TArray<FVector3f> Positions; TArray<uint32> Indices;
	PieceFitSphere(false, Positions, Indices);
	FMantleHairHeadSamples Head; FString Error;
	if (!TestTrue(TEXT("head builds"), Head.Build(Positions, Indices, Error))) { AddError(Error); return false; }
	TestTrue(TEXT("the scalp has points"), Head.NumScalpPoints() > 100 && Head.NumScalpPoints() <= FMantleHairHeadSamples::MaxScalpPoints);
	int32 Crown = 0, CrownScalp = 0, Face = 0, FaceScalp = 0, Nape = 0, NapeScalp = 0, Chin = 0, ChinScalp = 0;
	for (int32 I = 0; I < Head.Num(); ++I)
	{
		const FVector& P = Head.Point(I);
		const bool bScalp = Head.NearestScalpPoint(I) != INDEX_NONE;
		if (P.Z > 166.0) { ++Crown; CrownScalp += bScalp; }
		if (P.Y > 6.0 && P.Z > 156.0 && P.Z < 162.0) { ++Face; FaceScalp += bScalp; }
		if (P.Y < -6.0 && P.Z > 156.0 && P.Z < 162.0) { ++Nape; NapeScalp += bScalp; }
		if (P.Z < 153.0) { ++Chin; ChinScalp += bScalp; }
	}
	TestTrue(TEXT("the crown is scalp"), Crown > 0 && CrownScalp == Crown);
	TestTrue(TEXT("the back of the head is scalp"), Nape > 0 && NapeScalp == Nape);
	TestTrue(TEXT("the face is not"), Face > 0 && FaceScalp == 0);
	TestTrue(TEXT("nor is the bottom of the head"), Chin > 0 && ChinScalp == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairPieceFitLongHairTest,
	"Mantle.Editor.HairPieceFit.LongHairWiderThanTheHeadSitsWithItsRootsOnTheScalp",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairPieceFitLongHairTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairPieceFitTestPrivate;
	TArray<FVector3f> HeadPositions; TArray<uint32> HeadIndices;
	PieceFitSphere(false, HeadPositions, HeadIndices);
	FMantleHairHeadSamples Head; FString Error;
	if (!TestTrue(TEXT("head builds"), Head.Build(HeadPositions, HeadIndices, Error))) { AddError(Error); return false; }

	// Long hair 32 cm wide at the bottom: sizing it to 18 cm wide, as the fit starts, makes it far too small (the old fit
	// then lifted it above the crown). The vendor made it at 2.5 times the size, somewhere else.
	TArray<FVector3f> Fitted; TArray<uint32> Indices;
	PieceFitCards(true, Fitted, Indices);
	const TArray<FVector3f> Vendor = PieceFitVendorCopy(Fitted, 2.5f, FVector3f(0.f, 3.f, -16.f));
	const TArray<FVector3f> Ends = FMantleScalpFit::FindCardEnds(Vendor, Indices);
	TestEqual(TEXT("two ends per card"), Ends.Num(), 2 * 48);

	FMantleHairPieceFit::FResult Fit;
	if (!TestTrue(TEXT("fit runs"), FMantleHairPieceFit::Fit(Head, Vendor, Ends, Fit, Error))) { AddError(Error); return false; }
	TestTrue(TEXT("keeps out of the head"), Fit.bKeepsOutOfHead);
	TestTrue(TEXT("the searched scale is well past the old 1.425 limit"), Fit.Scale > 1.6);
	// A root within SeatCm (1 cm) of the scalp counts as seated, so the piece fit is good to about a centimetre and a tenth of
	// the size; the scalp fit (stage 2) then pulls the roots in.
	TestTrue(FString::Printf(TEXT("total scale within 10%% of the vendor's (%.4f for 0.4, offsets %.2f, %.2f, seated %.2f, coverage %.2f)"),
		Fit.TotalScale(), Fit.OffsetY, Fit.OffsetZ, Fit.SeatedFraction, Fit.ScalpCoverage), FMath::Abs(Fit.TotalScale() * 2.5 - 1.0) < 0.10);
	TestTrue(TEXT("most roots are seated on the scalp"), Fit.SeatedFraction > 0.8);
	double Median = 0.0, Worst = 0.0;
	PieceFitRootError(Fit, Vendor, Fitted, true, Median, Worst);
	TestTrue(FString::Printf(TEXT("roots land where they belong (median %.2f cm, worst %.2f cm)"), Median, Worst), Median < 1.5 && Worst < 2.5);
	TestTrue(TEXT("Describe gives words"), Fit.Describe().StartsWith(TEXT("Hair fitted to the head")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairPieceFitShortHairTest,
	"Mantle.Editor.HairPieceFit.ShortHairAlreadyOnTheHeadStaysPut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairPieceFitShortHairTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairPieceFitTestPrivate;
	TArray<FVector3f> HeadPositions; TArray<uint32> HeadIndices;
	PieceFitSphere(false, HeadPositions, HeadIndices);
	FMantleHairHeadSamples Head; FString Error;
	if (!TestTrue(TEXT("head builds"), Head.Build(HeadPositions, HeadIndices, Error))) { AddError(Error); return false; }

	TArray<FVector3f> Fitted; TArray<uint32> Indices;
	PieceFitCards(false, Fitted, Indices);
	const TArray<FVector3f> Ends = FMantleScalpFit::FindCardEnds(Fitted, Indices);
	FMantleHairPieceFit::FResult Fit;
	if (!TestTrue(TEXT("fit runs"), FMantleHairPieceFit::Fit(Head, Fitted, Ends, Fit, Error))) { AddError(Error); return false; }
	TestTrue(TEXT("keeps out of the head"), Fit.bKeepsOutOfHead);
	TestTrue(FString::Printf(TEXT("total scale stays within 10%% of 1 (%.4f, offsets %.2f, %.2f, seated %.2f, coverage %.2f)"),
		Fit.TotalScale(), Fit.OffsetY, Fit.OffsetZ, Fit.SeatedFraction, Fit.ScalpCoverage), FMath::Abs(Fit.TotalScale() - 1.0) < 0.10);
	double Median = 0.0, Worst = 0.0;
	PieceFitRootError(Fit, Fitted, Fitted, false, Median, Worst);
	TestTrue(FString::Printf(TEXT("roots stay within the seating distance (median %.2f cm, worst %.2f cm)"), Median, Worst), Median < 1.5 && Worst < 2.5);

	// Shrunk onto a patch of the head is not a fit: a candidate must cover the scalp as well as seat its roots.
	TestTrue(FString::Printf(TEXT("the roots spread over the scalp (%.2f)"), Fit.ScalpCoverage), Fit.ScalpCoverage > 0.3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairPieceFitFailsTest,
	"Mantle.Editor.HairPieceFit.NothingToFitOrNothingToFitToFails",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairPieceFitFailsTest::RunTest(const FString& Parameters)
{
	using namespace MantleHairPieceFitTestPrivate;
	TArray<FVector3f> HeadPositions; TArray<uint32> HeadIndices;
	PieceFitSphere(false, HeadPositions, HeadIndices);
	FMantleHairHeadSamples Head; FString Error;
	if (!TestTrue(TEXT("head builds"), Head.Build(HeadPositions, HeadIndices, Error))) { AddError(Error); return false; }
	TArray<FVector3f> Hair; TArray<uint32> Indices;
	PieceFitCards(false, Hair, Indices);
	const TArray<FVector3f> Ends = FMantleScalpFit::FindCardEnds(Hair, Indices);
	FMantleHairPieceFit::FResult Fit;
	TestFalse(TEXT("empty hair fails"), FMantleHairPieceFit::Fit(Head, TConstArrayView<FVector3f>(), Ends, Fit, Error));
	TestFalse(TEXT("no cards fails"), FMantleHairPieceFit::Fit(Head, Hair, TConstArrayView<FVector3f>(), Fit, Error));
	TestFalse(TEXT("no head fails"), FMantleHairPieceFit::Fit(FMantleHairHeadSamples(), Hair, Ends, Fit, Error));
	TestTrue(TEXT("a bad index gives no ends"), FMantleScalpFit::FindCardEnds(Hair, TArray<uint32>{0u, 1u, 999999u}).IsEmpty());
	return true;
}

#endif
