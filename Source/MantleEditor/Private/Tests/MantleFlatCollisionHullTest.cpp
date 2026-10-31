// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssetConverter.h"
#include "MantleStaticMeshReader.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// SciFiAlienFacility's corridor floor is a flat convex hull: Unreal collides with it, but the cook takes only hulls with volume, so it was
// left out and players fell through. A flat hull is now thickened across its plane, and a flat box on its flat side.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleFlatCollisionHullTest,
	"Mantle.Editor.Collision.FlatHullsAreThickened",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFlatCollisionHullTest::RunTest(const FString& Parameters)
{
	// A 600 cm square floor at z 273, as one flat hull.
	TArray<FVector3f> Floor = { FVector3f(0, 0, 273), FVector3f(600, 0, 273), FVector3f(600, 600, 273), FVector3f(0, 600, 273) };
	TestFalse(TEXT("a flat hull has no volume"), FMantleStaticMeshReader::HullSpansVolume(Floor));
	TestTrue(TEXT("it is thickened"), FMantlePlaceAssetConverter::ThickenFlatHull(Floor));
	TestEqual(TEXT("each point twice"), Floor.Num(), 8);
	TestTrue(TEXT("and then has volume"), FMantleStaticMeshReader::HullSpansVolume(Floor));
	float Low = TNumericLimits<float>::Max();
	float High = TNumericLimits<float>::Lowest();
	for (const FVector3f& Point : Floor)
	{
		Low = FMath::Min(Low, Point.Z);
		High = FMath::Max(High, Point.Z);
	}
	const float Half = static_cast<float>(FMantlePlaceAssetConverter::FlatHullHalfThicknessCm);
	TestEqual(TEXT("its underside is just below the floor"), Low, 273.0f - Half, 1.0e-3f);
	TestEqual(TEXT("and its top just above"), High, 273.0f + Half, 1.0e-3f);

	// A tilted ramp is thickened along its own normal, not up.
	TArray<FVector3f> Ramp = { FVector3f(0, 0, 0), FVector3f(100, 0, 100), FVector3f(100, 100, 100), FVector3f(0, 100, 0) };
	TestTrue(TEXT("a tilted flat hull is thickened"), FMantlePlaceAssetConverter::ThickenFlatHull(Ramp) && FMantleStaticMeshReader::HullSpansVolume(Ramp));
	TestEqual(TEXT("each point moves by the half thickness"), (Ramp[0] - FVector3f(0, 0, 0)).Length(), Half, 1.0e-3f);

	// Points on a line, or too few, span no plane: unchanged.
	TArray<FVector3f> Line = { FVector3f(0, 0, 0), FVector3f(50, 0, 0), FVector3f(100, 0, 0), FVector3f(150, 0, 0) };
	TestFalse(TEXT("a line is not thickened"), FMantlePlaceAssetConverter::ThickenFlatHull(Line));
	TestEqual(TEXT("and is left as it was"), Line.Num(), 4);
	TArray<FVector3f> Two = { FVector3f(0, 0, 0), FVector3f(1, 0, 0) };
	TestFalse(TEXT("two points are not thickened"), FMantlePlaceAssetConverter::ThickenFlatHull(Two));

	// A box flat on one side (SciFiAlienFacility's SM_SUW_wall_13) gets that side's thickness; flat on two, or whole, is left as it is.
	const double Full = 2.0 * FMantlePlaceAssetConverter::FlatHullHalfThicknessCm;
	TestEqual(TEXT("a wall sheet box is thickened"), FMantlePlaceAssetConverter::ThickenFlatBox(FVector(0.0, 400.0, 300.0)), FVector(Full, 400.0, 300.0));
	TestEqual(TEXT("a whole box is unchanged"), FMantlePlaceAssetConverter::ThickenFlatBox(FVector(10.0, 20.0, 30.0)), FVector(10.0, 20.0, 30.0));
	TestEqual(TEXT("a box flat on two sides is unchanged"), FMantlePlaceAssetConverter::ThickenFlatBox(FVector(0.0, 0.0, 30.0)), FVector(0.0, 0.0, 30.0));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
