// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarHeight.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Converted avatars and presets both land within +-0.5 cm of the driver height (the backend's publish limit). Pure; no assets.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleConvertedHeightToleranceTest,
	"Mantle.Editor.AvatarHeight.ConvertedTolerance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleConvertedHeightToleranceTest::RunTest(const FString& Parameters)
{
	const double Target = 165.6;
	const double Converted = FMantleAvatarHeight::ConvertedToleranceCm;
	TestEqual(TEXT("converted tolerance is 0.5 cm, the backend's limit"), Converted, 0.5);
	TestTrue(TEXT("on target passes"), FMantleAvatarHeight::IsWithin(165.6, Target, Converted));
	TestTrue(TEXT("0.4 cm under passes"), FMantleAvatarHeight::IsWithin(165.2, Target, Converted));
	TestTrue(TEXT("0.49 cm over passes"), FMantleAvatarHeight::IsWithin(166.09, Target, Converted));
	TestFalse(TEXT("0.88 cm under fails (the backend refuses it)"), FMantleAvatarHeight::IsWithin(164.72, Target, Converted));
	TestFalse(TEXT("1.1 cm over fails"), FMantleAvatarHeight::IsWithin(166.7, Target, Converted));
	TestFalse(TEXT("unmeasured fails"), FMantleAvatarHeight::IsWithin(-1.0, Target, Converted));
	TestTrue(TEXT("the line for 0.88 cm under warns and names +- 0.5"),
		FMantleAvatarHeight::BuiltHeightLine(164.72, Target, Converted).EndsWith(TEXT("+- 0.5")));
	return true;
}

#endif
