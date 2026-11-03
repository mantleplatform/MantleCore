// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleBuiltInMaterials.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuiltInDetectTest,
	"Mantle.Vehicle.BuiltInMaterials.DetectionUsesNamesLightsAndClearGlass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuiltInDetectTest::RunTest(const FString&)
{
	// Lit, two-sided materials of the blend mode given, Opacity a constant when given.
	const auto NewMaterial = [](const EBlendMode Blend, const float Opacity = -1.0f)
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		Material->BlendMode = Blend;
		Material->TwoSided = true;
		if (Opacity >= 0.0f)
		{
			UMaterialExpressionConstant* Constant = NewObject<UMaterialExpressionConstant>(Material);
			Constant->Material = Material;
			Constant->R = Opacity;
			Material->GetExpressionCollection().AddExpression(Constant);
			Material->GetEditorOnlyData()->Opacity.Connect(0, Constant);
		}
		return Material;
	};
	// A name wins, a light section is a lens, a clear translucent material is glass; a body panel, a cutout and an opaque material are left to their own.
	TestEqual(TEXT("a section named windshield is glass"), FMantleVehicleBuiltInMaterials::Detect(NewMaterial(BLEND_Opaque), TEXT("windshield"), false), FString(FMantleVehicleBuiltInMaterials::Glass()));
	TestEqual(TEXT("a light section is a lens whatever its name"), FMantleVehicleBuiltInMaterials::Detect(NewMaterial(BLEND_Opaque), TEXT("x7_brakelight"), true), FString(FMantleVehicleBuiltInMaterials::LightLens()));
	TestEqual(TEXT("a translucent material of opacity 0.3 is glass whatever its name"), FMantleVehicleBuiltInMaterials::Detect(NewMaterial(BLEND_Translucent, 0.3f), TEXT("material_7"), false),
		FString(FMantleVehicleBuiltInMaterials::Glass()));
	TestEqual(TEXT("a translucent material of opacity 1 is solid, not glass"), FMantleVehicleBuiltInMaterials::Detect(NewMaterial(BLEND_Translucent, 1.0f), TEXT("material_7"), false), FString());
	TestEqual(TEXT("a translucent material with no opacity connected is solid, not glass"), FMantleVehicleBuiltInMaterials::Detect(NewMaterial(BLEND_Translucent), TEXT("material_7"), false), FString());
	TestEqual(TEXT("an opaque body panel keeps its material"), FMantleVehicleBuiltInMaterials::Detect(NewMaterial(BLEND_Opaque), TEXT("body_panel"), false), FString());
	TestEqual(TEXT("a null material is read by its name"), FMantleVehicleBuiltInMaterials::Detect(nullptr, TEXT("Tire"), false), FString(FMantleVehicleBuiltInMaterials::Rubber()));
	TestEqual(TEXT("and a null material with another name keeps nothing"), FMantleVehicleBuiltInMaterials::Detect(nullptr, TEXT("seat"), false), FString());
	return true;
}

#endif
