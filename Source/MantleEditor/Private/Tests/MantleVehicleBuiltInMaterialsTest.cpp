// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleBuiltInMaterials.h"

#include "MantleVehicleAsset.h"
#include "MantleVehicleSource.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// The built-in vehicle materials the builder suggests by a section's name (mantle#239's ids): glass, tire rubber, chrome, carbon fiber and light lenses; nothing for a name that says
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
	Expect(TEXT("carbonfiber_005"), FMantleVehicleBuiltInMaterials::Carbon());
	Expect(TEXT("Carbon_Fiber"), FMantleVehicleBuiltInMaterials::Carbon());
	// Carbon wins over the parts it sits next to: a carbon mirror cap or windshield-base strip is carbon, though a plain mirror or windshield is not.
	Expect(TEXT("carbonfiber_mirror"), FMantleVehicleBuiltInMaterials::Carbon());
	Expect(TEXT("windshield_base_carbon"), FMantleVehicleBuiltInMaterials::Carbon());

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

	TestEqual(TEXT("six built-in materials"), FMantleVehicleBuiltInMaterials::All().Num(), 6);
	TestTrue(TEXT("a tracked vehicle's belt is one of them"), FMantleVehicleBuiltInMaterials::IsBuiltIn(TEXT("ast_mat_track")));
	TestEqual(TEXT("and says what it does"), FMantleVehicleBuiltInMaterials::Label(FMantleVehicleBuiltInMaterials::Track()).ToString(), FString(TEXT("Track belt (scrolls with the track)")));
	// A section's name alone never suggests a belt: only a tracked vehicle has one (ForSection).
	Expect(TEXT("Track_L"), None);
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
	TestEqual(TEXT("a null material named carbon_fiber is carbon"), FMantleVehicleBuiltInMaterials::Detect(nullptr, TEXT("carbon_fiber"), false), FString(FMantleVehicleBuiltInMaterials::Carbon()));
	TestEqual(TEXT("a section named carbon but drawn as a light is a lens"), FMantleVehicleBuiltInMaterials::Detect(NewMaterial(BLEND_Opaque), TEXT("carbonfiber_005"), true), FString(FMantleVehicleBuiltInMaterials::LightLens()));
	TestEqual(TEXT("a null material is read by its name"), FMantleVehicleBuiltInMaterials::Detect(nullptr, TEXT("Tire"), false), FString(FMantleVehicleBuiltInMaterials::Rubber()));
	TestEqual(TEXT("and a null material with another name keeps nothing"), FMantleVehicleBuiltInMaterials::Detect(nullptr, TEXT("seat"), false), FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBuiltInTrackTest,
	"Mantle.Vehicle.BuiltInMaterials.TrackedBeltScrolls",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBuiltInTrackTest::RunTest(const FString&)
{
	// A tracked vehicle's belt sections (#448) draw with Mantle's track material, whose pads scroll with the track; its other sections, and any car's, do not.
	const FString Track = FMantleVehicleBuiltInMaterials::Track();
	UMantleVehicleSource* Tank = NewObject<UMantleVehicleSource>(GetTransientPackage());
	Tank->MovementType = EMantleVehicleMovementKind::Tracked;
	UMantleVehicleSource* Car = NewObject<UMantleVehicleSource>(GetTransientPackage());
	TestEqual(TEXT("a tank's track section is the scrolling belt"), FMantleVehicleBuiltInMaterials::ForSection(*Tank, TEXT("Track_L"), nullptr), Track);
	TestEqual(TEXT("so is its tread"), FMantleVehicleBuiltInMaterials::ForSection(*Tank, TEXT("Treads"), nullptr), Track);
	TestEqual(TEXT("a sprocket is not the belt"), FMantleVehicleBuiltInMaterials::ForSection(*Tank, TEXT("Sprocket_L"), nullptr), FString());
	TestEqual(TEXT("nor is a seat belt"), FMantleVehicleBuiltInMaterials::ForSection(*Tank, TEXT("SeatBelt"), nullptr), FString());
	TestEqual(TEXT("a tank's window is still glass"), FMantleVehicleBuiltInMaterials::ForSection(*Tank, TEXT("Periscope_Glass"), nullptr), FString(FMantleVehicleBuiltInMaterials::Glass()));
	TestEqual(TEXT("a car has no belt to scroll"), FMantleVehicleBuiltInMaterials::ForSection(*Car, TEXT("Track_L"), nullptr), FString());
	// A painted section is the vehicle's color, never a built-in.
	Tank->Paint.Sections.Add(TEXT("Track_L"));
	TestEqual(TEXT("a painted track keeps its paint"), FMantleVehicleBuiltInMaterials::ForSection(*Tank, TEXT("Track_L"), nullptr), FString());
	return true;
}

#endif
