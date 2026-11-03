// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleBuiltInMaterials.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The built-in vehicle materials the builder suggests by a section's name (mantle#239's ids): glass, tire rubber, chrome and light lenses; nothing for a name that says
// something else.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuiltInSuggestTest,
	"Mantle.Vehicle.BuiltInMaterials.NamesSuggestTheirMaterial",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuiltInSuggestTest::RunTest(const FString&)
{
	const auto Expect = [this](const TCHAR* Name, const FString& Id)
	{
		TestEqual(*FString::Printf(TEXT("'%s' suggests '%s'"), Name, *Id), FMantleVehicleBuiltInMaterials::Suggest(Name), Id);
	};
	const FString None;
	Expect(TEXT("glass_svet"), FMantleVehicleBuiltInMaterials::Glass());
	Expect(TEXT("vehicle_generic_glasswindows2"), FMantleVehicleBuiltInMaterials::Glass());
	Expect(TEXT("Windscreen"), FMantleVehicleBuiltInMaterials::Glass());
	Expect(TEXT("FrontWindshield"), FMantleVehicleBuiltInMaterials::Glass());
	Expect(TEXT("x7glass-min [SECONDARY]"), FMantleVehicleBuiltInMaterials::Glass());
	Expect(TEXT("Tire"), FMantleVehicleBuiltInMaterials::Rubber());
	Expect(TEXT("tyre_front"), FMantleVehicleBuiltInMaterials::Rubber());
	Expect(TEXT("Chrome_Bumper"), FMantleVehicleBuiltInMaterials::Chrome());
	Expect(TEXT("chrome3"), FMantleVehicleBuiltInMaterials::Chrome());
	Expect(TEXT("headlight_lens"), FMantleVehicleBuiltInMaterials::LightLens());
	Expect(TEXT("Lens_Glass"), FMantleVehicleBuiltInMaterials::LightLens());

	// Not what they sit next to.
	Expect(TEXT("tire_rim"), None);
	Expect(TEXT("TyreHub"), None);
	Expect(TEXT("window_frame"), None);
	Expect(TEXT("windshield_wiper"), None);
	Expect(TEXT("mirror_glass"), None);
	Expect(TEXT("door_seal"), None);
	Expect(TEXT("body_paint"), None);
	Expect(TEXT("carpet1"), None);
	Expect(TEXT(""), None);

	TestEqual(TEXT("four built-in materials"), FMantleVehicleBuiltInMaterials::All().Num(), 4);
	TestTrue(TEXT("they are recognized by id"), FMantleVehicleBuiltInMaterials::IsBuiltIn(TEXT("ast_mat_glass")));
	TestFalse(TEXT("an uploaded material is not one"), FMantleVehicleBuiltInMaterials::IsBuiltIn(TEXT("ast_u0123456789abcdef0123456789abcdef")));
	return true;
}

#endif
