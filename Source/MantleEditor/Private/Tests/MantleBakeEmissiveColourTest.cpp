// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarAtlasFixer.h"

#include "Engine/EngineTypes.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// A toon material (VRoid's MToon: unlit, all its colour in emissive) bakes its emissive as the base colour; a lit material with
// a real base colour keeps it. Synthetic texels, no assets.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBakeEmissiveColourTest,
	"Mantle.Editor.AvatarAtlas.EmissiveAsColour",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBakeEmissiveColourTest::RunTest(const FString& Parameters)
{
	const TArray<FColor> Skin = { FColor(240, 200, 180), FColor(230, 190, 170), FColor(120, 60, 40) };
	const TArray<FColor> Black = { FColor(0, 0, 0), FColor(1, 2, 0), FColor(0, 0, 3) };
	const TArray<FColor> Grey = { FColor(128, 128, 128), FColor(128, 128, 128), FColor(128, 128, 128) };
	const TArray<FColor> DarkFabric = { FColor(10, 10, 12), FColor(20, 18, 22), FColor(6, 6, 6) };
	const TArray<FColor> Glow = { FColor(0, 0, 0), FColor(0, 200, 255), FColor(0, 0, 0) };

	TestTrue(TEXT("an unlit toon material's emissive is its colour"), FMantleAvatarAtlasFixer::BakesEmissiveAsColour(true, Grey, Skin));
	TestTrue(TEXT("a lit material with a black base colour shows its emissive"), FMantleAvatarAtlasFixer::BakesEmissiveAsColour(false, Black, Skin));
	TestFalse(TEXT("a lit material with a real base colour keeps it"), FMantleAvatarAtlasFixer::BakesEmissiveAsColour(false, Skin, Glow));
	TestFalse(TEXT("dark fabric with glowing trim keeps its fabric"), FMantleAvatarAtlasFixer::BakesEmissiveAsColour(false, DarkFabric, Glow));
	TestFalse(TEXT("an unlit material with no emissive keeps what it has"), FMantleAvatarAtlasFixer::BakesEmissiveAsColour(true, Grey, Black));
	TestFalse(TEXT("no emissive baked, nothing changes"), FMantleAvatarAtlasFixer::BakesEmissiveAsColour(true, Grey, TArray<FColor>()));
	return true;
}


// #167: the Hair shading model's Metallic pin is Scatter and its Normal pin the strand Tangent; the Eye model's Metallic is Curvature.
// Neither is baked as metal (or the hair turns near-black); a default lit material bakes both.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBakeShadingModelPinsTest,
	"Mantle.Editor.AvatarAtlas.HairShadingPins",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBakeShadingModelPinsTest::RunTest(const FString& Parameters)
{
	const FMaterialShadingModelField Lit(MSM_DefaultLit);
	const FMaterialShadingModelField Hair(MSM_Hair);
	const FMaterialShadingModelField Eye(MSM_Eye);
	FMaterialShadingModelField Mixed(MSM_DefaultLit);
	Mixed.AddShadingModel(MSM_Hair);

	TestTrue(TEXT("a lit material bakes its metal"), FMantleAvatarAtlasFixer::BakesMetallic(Lit));
	TestTrue(TEXT("a lit material bakes its normal"), FMantleAvatarAtlasFixer::BakesNormal(Lit));
	TestFalse(TEXT("hair bakes metal 0 (its Metallic is Scatter)"), FMantleAvatarAtlasFixer::BakesMetallic(Hair));
	TestFalse(TEXT("hair bakes a flat normal (its Normal is the strand Tangent)"), FMantleAvatarAtlasFixer::BakesNormal(Hair));
	TestFalse(TEXT("an eye bakes metal 0 (its Metallic is Curvature)"), FMantleAvatarAtlasFixer::BakesMetallic(Eye));
	TestTrue(TEXT("an eye keeps its normal"), FMantleAvatarAtlasFixer::BakesNormal(Eye));
	TestFalse(TEXT("a material that may shade as hair bakes metal 0"), FMantleAvatarAtlasFixer::BakesMetallic(Mixed));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
