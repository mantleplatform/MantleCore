// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssets.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The world-aligned warning said the engine's WorldAlignedTexture is "kept exactly", so a material using its XY Texture output (a
// top-down projection, which Mantle bakes as a 4 m tile) read as a contradiction. Only the XYZ Texture output is kept
// (WorldAlignedXYZ in MantlePlaceAssets.cpp), and the warning now names it.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWorldAlignedWarningNamesOutputTest,
	"Mantle.Editor.WorldAligned.WarningNamesTheKeptOutput",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWorldAlignedWarningNamesOutputTest::RunTest(const FString& Parameters)
{
	const FString Warning = FMantlePlaceAssets::DescribeLoss(EMantleMaterialLoss::WorldAligned, FMantleMaterialAnalysis());
	TestTrue(TEXT("it says the XYZ Texture output is the one kept exactly"), Warning.Contains(TEXT("XYZ Texture output")));
	TestTrue(TEXT("and that the XY Texture and Z Texture outputs are baked"), Warning.Contains(TEXT("XY Texture and Z Texture outputs are baked")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
