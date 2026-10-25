// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCoverage.h"
#include "MantlePlaceExporter.h"
#include "MantleWaterBody.h"
#include "MantleWaterBodyExport.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// An ordinary component is not a water body (GhzGarage/mantle#997). The Water plugin is not linked, so the body classes are built only when the plugin is loaded.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterBodyRecogniseTest,
	"Mantle.Editor.WaterBody.OnlyWaterBodiesAreWater",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterBodyRecogniseTest::RunTest(const FString& Parameters)
{
	USceneComponent* Plain = NewObject<USceneComponent>(GetTransientPackage());
	TestFalse(TEXT("a scene component is not a water body"), FMantleWaterBody::IsWaterBody(Plain));
	TestFalse(TEXT("nor is nothing"), FMantleWaterBody::IsWaterBody(nullptr));
	TestTrue(TEXT("a plain component has no kind of water"), FMantleWaterBody::Kind(nullptr).IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
