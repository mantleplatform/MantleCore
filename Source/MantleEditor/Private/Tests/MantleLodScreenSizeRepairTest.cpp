// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssetConverter.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// #628: the export repairs a mesh's LOD screen sizes (on its own copy) so each is strictly below the one before.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLodScreenSizeRepairTest,
	"Mantle.Editor.Mesh.LodScreenSizesAreRepaired",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLodScreenSizeRepairTest::RunTest(const FString& Parameters)
{
	// Valid sizes are left exactly as the creator set them.
	TArray<float> Valid = { 1.0f, 0.4f, 0.1f };
	TestFalse(TEXT("valid sizes need no repair"), FMantlePlaceAssetConverter::RepairLODScreenSizes(Valid));
	TestEqual(TEXT("and are unchanged"), Valid, TArray<float>({ 1.0f, 0.4f, 0.1f }));

	// SM_BalanceCover: LOD0 above 1 is capped, the repeat takes half of the one before.
	TArray<float> Balance = { 2.0f, 0.084029f, 0.084029f };
	TestTrue(TEXT("the Fab mesh's sizes are repaired"), FMantlePlaceAssetConverter::RepairLODScreenSizes(Balance));
	if (TestEqual(TEXT("with the same number of LODs"), Balance.Num(), 3))
	{
		TestEqual(TEXT("LOD0 is capped at 1"), Balance[0], 1.0f);
		TestEqual(TEXT("LOD1 keeps the creator's value"), Balance[1], 0.084029f);
		TestEqual(TEXT("LOD2 is half of LOD1"), Balance[2], 0.084029f * 0.5f);
	}

	// A rise, a zero and a NaN are each replaced, and later LODs are judged against the repaired one.
	TArray<float> Messy = { 0.5f, 0.6f, 0.0f, 0.1f, NAN };
	TestTrue(TEXT("a rise, a zero and a NaN are repaired"), FMantlePlaceAssetConverter::RepairLODScreenSizes(Messy));
	if (TestEqual(TEXT("keeping the LOD count"), Messy.Num(), 5))
	{
		TestEqual(TEXT("LOD0 is kept"), Messy[0], 0.5f);
		TestEqual(TEXT("a rise takes half the one before"), Messy[1], 0.25f);
		TestEqual(TEXT("a zero takes half the one before"), Messy[2], 0.125f);
		TestEqual(TEXT("a value already below the one before is kept"), Messy[3], 0.1f);
		TestEqual(TEXT("a NaN takes half the one before"), Messy[4], 0.05f);
		for (int32 Index = 1; Index < Messy.Num(); ++Index)
		{
			TestTrue(TEXT("every LOD is below the one before"), Messy[Index] > 0.0f && Messy[Index] < Messy[Index - 1]);
		}
	}
	return true;
}

#endif
