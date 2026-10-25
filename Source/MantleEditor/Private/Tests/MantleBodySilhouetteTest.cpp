// Copyright Joshua Eger. All Rights Reserved.
#include "MantleBodySilhouette.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// #50: the build a kept face's body starts from, on synthetic torsos. No assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleBodySilhouetteTestPrivate
{
	/** A 170 cm standing tube of elliptical rings (half widths HalfX, HalfY), with optional arms hanging apart from the torso. */
	TArray<FVector3f> SilhouetteTestTube(const double HalfX, const double HalfY, const bool bArms)
	{
		TArray<FVector3f> Out;
		for (double Z = 0.0; Z <= 170.0; Z += 1.0)
		{
			for (int32 Step = 0; Step < 256; ++Step)
			{
				const double Angle = Step * UE_TWO_PI / 256.0;
				Out.Add(FVector3f(HalfX * FMath::Cos(Angle), HalfY * FMath::Sin(Angle), Z));
				if (bArms)
				{
					// Arms 30 cm out from the middle, well apart from the torso.
					Out.Add(FVector3f(HalfX + 30.0 + 4.0 * FMath::Cos(Angle), 4.0 * FMath::Sin(Angle), Z));
					Out.Add(FVector3f(-HalfX - 30.0 + 4.0 * FMath::Cos(Angle), 4.0 * FMath::Sin(Angle), Z));
				}
			}
		}
		return Out;
	}

	FMantleSilhouette SilhouetteTestMeasure(const double HalfX, const double HalfY, const bool bArms)
	{
		FMantleSilhouette Out;
		FMantleBodySilhouette::Measure(SilhouetteTestTube(HalfX, HalfY, bArms), 0.0, 170.0, Out);
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBodySilhouetteMeasureTest,
	"Mantle.Editor.BodySilhouette.Measure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBodySilhouetteMeasureTest::RunTest(const FString& Parameters)
{
	using namespace MantleBodySilhouetteTestPrivate;
	const FMantleSilhouette Plain = SilhouetteTestMeasure(15.0, 10.0, false);
	TestTrue(TEXT("a tube has a silhouette"), Plain.bValid);
	TestTrue(TEXT("its width is the tube's"), FMath::IsNearlyEqual(Plain.Width[FMantleSilhouette::Waist], 30.0, 1.0));
	TestTrue(TEXT("its depth is the tube's"), FMath::IsNearlyEqual(Plain.Depth[FMantleSilhouette::Waist], 20.0, 1.0));

	const FMantleSilhouette Armed = SilhouetteTestMeasure(15.0, 10.0, true);
	TestTrue(TEXT("arms apart from the torso don't widen it"), FMath::IsNearlyEqual(Armed.Width[FMantleSilhouette::Hips], 30.0, 1.0));

	FMantleSilhouette None;
	TestFalse(TEXT("no vertices, no silhouette"), FMantleBodySilhouette::Measure(TArray<FVector3f>(), 0.0, 170.0, None));
	TestFalse(TEXT("no height, no silhouette"), FMantleBodySilhouette::Measure(SilhouetteTestTube(15.0, 10.0, false), 0.0, 0.0, None));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBodySilhouetteSeedTest,
	"Mantle.Editor.BodySilhouette.Seed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBodySilhouetteSeedTest::RunTest(const FString& Parameters)
{
	using namespace MantleBodySilhouetteTestPrivate;
	const FMantleSilhouette Default = FMantleBodySilhouette::Normalised(SilhouetteTestMeasure(15.0, 10.0, false), 170.0);

	const FMantleSliderSeed Same = FMantleBodySilhouette::Seed(Default, Default);
	TestTrue(TEXT("the same build moves nothing"), FMath::IsNearlyZero(Same.Weight) && FMath::IsNearlyZero(Same.Fat) && FMath::IsNearlyZero(Same.Muscularity));

	const FMantleSliderSeed Wide = FMantleBodySilhouette::Seed(FMantleBodySilhouette::Normalised(SilhouetteTestMeasure(19.5, 13.0, false), 170.0), Default);
	TestTrue(TEXT("a wider, deeper torso raises Weight"), Wide.Weight > 0.1);
	TestTrue(TEXT("and Fat"), Wide.Fat > 0.1);
	TestTrue(TEXT("and Muscularity"), Wide.Muscularity > 0.1);

	const FMantleSliderSeed Thin = FMantleBodySilhouette::Seed(FMantleBodySilhouette::Normalised(SilhouetteTestMeasure(12.0, 8.0, false), 170.0), Default);
	TestTrue(TEXT("a thinner torso goes below the default"), Thin.Weight < -0.1 && Thin.Fat < -0.1 && Thin.Muscularity < -0.1);

	const FMantleSliderSeed Huge = FMantleBodySilhouette::Seed(FMantleBodySilhouette::Normalised(SilhouetteTestMeasure(60.0, 40.0, false), 170.0), Default);
	TestTrue(TEXT("a huge one is clamped"), FMath::IsNearlyEqual(Huge.Weight, FMantleBodySilhouette::SeedLimit) && FMath::IsNearlyEqual(Huge.Fat, FMantleBodySilhouette::SeedLimit));

	TestTrue(TEXT("an unmeasured build moves nothing"), FMath::IsNearlyZero(FMantleBodySilhouette::Seed(FMantleSilhouette(), Default).Weight));

	// Only the legs bare (a robe over the torso): no band at the chest or waist, so nothing seeds from them.
	TArray<FVector3f> Legs;
	for (const FVector3f& Vertex : SilhouetteTestTube(30.0, 20.0, false))
	{
		if (Vertex.Z < 40.0)
		{
			Legs.Add(Vertex);
		}
	}
	FMantleSilhouette Covered;
	TestFalse(TEXT("no bare torso, no silhouette"), FMantleBodySilhouette::Measure(Legs, 0.0, 170.0, Covered));

	// A bulky robe over the chest and waist, a slim body measured at the hips alone: Fat and Weight follow the hips only, Muscularity stays 0.
	FMantleSilhouette HipsOnly = Default;
	HipsOnly.bBand[FMantleSilhouette::Chest] = false;
	HipsOnly.bBand[FMantleSilhouette::Waist] = false;
	HipsOnly.Width[FMantleSilhouette::Chest] = HipsOnly.Width[FMantleSilhouette::Waist] = 99.0;
	HipsOnly.Depth[FMantleSilhouette::Chest] = HipsOnly.Depth[FMantleSilhouette::Waist] = 99.0;
	const FMantleSliderSeed Partial = FMantleBodySilhouette::Seed(HipsOnly, Default);
	TestTrue(TEXT("unmeasured bands are left out of every slider"), FMath::IsNearlyZero(Partial.Weight) && FMath::IsNearlyZero(Partial.Fat));
	TestTrue(TEXT("and the chest slider stays put"), FMath::IsNearlyZero(Partial.Muscularity));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
