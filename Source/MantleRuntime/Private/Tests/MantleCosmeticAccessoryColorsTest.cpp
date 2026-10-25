// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCosmeticItem.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// #966: the API sends colour regions on a rigid item in an accessory slot (a hat). The item batch reader takes
// them there, as on clothing, and still refuses them on other slots (hair).

namespace MantleCosmeticAccessoryColorsTest
{
	FString Batch(const TCHAR* Slot, const TCHAR* Kind)
	{
		const FString Hash = TEXT("blake3:") + FString::ChrN(64, TEXT('a'));
		return FString::Printf(TEXT(R"({"items":[{"id":"itm_u00000000000000000000000000000001","slot":"%s","kind":"%s","version":1,)"
			R"("mesh":{"url":"https://example.test/mesh","cookedHash":"%s"},)"
			R"("materials":[{"role":"other","albedo":{"url":"https://example.test/albedo","cookedHash":"%s"},)"
			R"("colorCustomization":{"version":1,"mode":"masked_luminance_v1","mask":{"url":"https://example.test/mask","cookedHash":"%s"},)"
			R"("regions":[{"id":"main","label":"Main","channel":"r","defaultColor":"#336699","referenceLuminance":0.5}]}}]}]})"),
			Slot, Kind, *Hash, *Hash, *Hash);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCosmeticAccessoryColorsTest,
	"Mantle.Cosmetic.AccessoryColorRegions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCosmeticAccessoryColorsTest::RunTest(const FString& Parameters)
{
	using namespace MantleCosmeticAccessoryColorsTest;
	TArray<FMantleCosmeticItem> Items;
	TArray<FString> Missing;
	FString WhyNot;
	if (!TestTrue(TEXT("a hat with colour regions reads"), MantleCosmetic::ParseItemBatch(Batch(TEXT("acc_hat"), TEXT("rigid")), Items, Missing, WhyNot)))
	{
		AddInfo(WhyNot);
		return false;
	}
	TestTrue(TEXT("the hat keeps its region"), Items.Num() == 1 && Items[0].Materials.Num() == 1 && Items[0].Materials[0].ColorRegions.Num() == 1
		&& Items[0].Materials[0].ColorRegions[0].Id == TEXT("main") && Items[0].Materials[0].ColorMask.IsLoadable());

	TestTrue(TEXT("a top with colour regions reads"), MantleCosmetic::ParseItemBatch(Batch(TEXT("top"), TEXT("skinned")), Items, Missing, WhyNot));
	TestFalse(TEXT("hair refuses colour regions"), MantleCosmetic::ParseItemBatch(Batch(TEXT("hair"), TEXT("skinned")), Items, Missing, WhyNot));
	return true;
}

#endif
