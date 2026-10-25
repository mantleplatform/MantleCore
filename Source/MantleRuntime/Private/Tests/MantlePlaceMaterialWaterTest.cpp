// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceMaterial.h"

#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// Water on its own master: the document's water key, the two M_MantlePlace_Water instances it picks, what the
// parser refuses, the values ApplyWater and ApplyExtended write, and the fallback when the content is missing. No world;
// the permutation assets are loaded only by the fallback test, which accepts either outcome.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantlePlaceMaterialWaterTestPrivate
{
	bool Parse(const FString& Json, FMantlePlaceMaterialDesc& Out, FString* Error = nullptr)
	{
		const FTCHARToUTF8 Utf8(*Json);
		return FMantlePlaceMaterials::ParseMaterial(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), Out, Error);
	}

	const TCHAR* const Head = TEXT("{\"schemaVersion\":1,\"alphaMode\":\"opaque\",\"doubleSided\":false");
	const TCHAR* const Normal2Id = TEXT("ast_u22222222222222222222222222222222");

	/** The value the instance itself holds for Name (not the parent's), -1 when it holds none: the parent here is the engine's default material. */
	float ScalarOf(UMaterialInstanceDynamic* Instance, const TCHAR* Name)
	{
		for (const FScalarParameterValue& Value : Instance->ScalarParameterValues)
		{
			if (Value.ParameterInfo.Name == FName(Name))
			{
				return Value.ParameterValue;
			}
		}
		return -1.0f;
	}

	/** As above for a vector; (-1, -1, -1, -1) when the instance holds none. */
	FLinearColor VectorOf(UMaterialInstanceDynamic* Instance, const TCHAR* Name)
	{
		for (const FVectorParameterValue& Value : Instance->VectorParameterValues)
		{
			if (Value.ParameterInfo.Name == FName(Name))
			{
				return Value.ParameterValue;
			}
		}
		return FLinearColor(-1.0f, -1.0f, -1.0f, -1.0f);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceMaterialWaterParseTest,
	"Mantle.Runtime.PlaceMaterial.Water.Parse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceMaterialWaterParseTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceMaterialWaterTestPrivate;

	// An empty object is the defaults: clear water on one normal map in mesh UVs.
	FMantlePlaceMaterialDesc Desc;
	FString Error;
	if (!TestTrue(TEXT("an empty water object parses"), Parse(FString(Head) + TEXT(",\"water\":{}}"), Desc, &Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("it has water"), Desc.bHasWater);
	TestTrue(TEXT("scattering defaults"), Desc.Water.Scattering.Equals(FLinearColor(0.002f, 0.002f, 0.002f, 1.0f)));
	TestTrue(TEXT("absorption defaults"), Desc.Water.Absorption.Equals(FLinearColor(0.02f, 0.007f, 0.005f, 1.0f)));
	TestEqual(TEXT("phaseG defaults to 0"), Desc.Water.PhaseG, 0.0f);
	TestEqual(TEXT("the colour scale defaults to 1"), Desc.Water.ColorScaleBehindWater, 1.0f);
	TestTrue(TEXT("the first normal is in UV space"), Desc.Water.Normal.Space == EMantlePlaceWaterSpace::Uv);
	TestTrue(TEXT("with no tiling or pan"), Desc.Water.Normal.Tiling == FVector2f(1.0f, 1.0f) && Desc.Water.Normal.Panning == FVector2f(0.0f, 0.0f));
	TestFalse(TEXT("and no second normal"), Desc.Water.bHasNormal2);
	TestTrue(TEXT("water builds on its own master, not Extended's plain permutations"), Desc.ExtendedVariant() == EMantlePlaceExtendedVariant::Water);
	TestTrue(TEXT("(UsesExtended routes it off M_MantlePlace)"), Desc.UsesExtended());

	// The river: two UV normals, panning at different speeds.
	if (!TestTrue(TEXT("the river parses"),
		Parse(FString(Head) + TEXT(",\"baseColorFactor\":[0,0,0,1],\"metallicFactor\":0,\"roughnessFactor\":0.01,")
			TEXT("\"water\":{\"scattering\":[0.003,0.017,0.018],\"absorption\":[0.027,0.013,0.012],\"phaseG\":-0.25,\"colorScaleBehindWater\":0.5,")
			TEXT("\"normal\":{\"space\":\"uv\",\"tiling\":[0.25,0.25],\"panning\":[-0.0025,0]},")
			TEXT("\"normal2\":{\"texture\":\"ast_u22222222222222222222222222222222\",\"tiling\":[0.5,0.5],\"panning\":[-0.0125,0]}},")
			TEXT("\"textures\":{\"normal\":\"ast_u11111111111111111111111111111111\"}}"), Desc, &Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("scattering"), Desc.Water.Scattering.Equals(FLinearColor(0.003f, 0.017f, 0.018f, 1.0f)));
	TestTrue(TEXT("absorption"), Desc.Water.Absorption.Equals(FLinearColor(0.027f, 0.013f, 0.012f, 1.0f)));
	TestEqual(TEXT("phaseG"), Desc.Water.PhaseG, -0.25f);
	TestEqual(TEXT("colour scale"), Desc.Water.ColorScaleBehindWater, 0.5f);
	TestTrue(TEXT("first tiling"), Desc.Water.Normal.Tiling == FVector2f(0.25f, 0.25f));
	TestTrue(TEXT("first pan"), Desc.Water.Normal.Panning == FVector2f(-0.0025f, 0.0f));
	TestTrue(TEXT("a second normal"), Desc.Water.bHasNormal2);
	TestTrue(TEXT("second tiling"), Desc.Water.Normal2.Tiling == FVector2f(0.5f, 0.5f));
	TestTrue(TEXT("second pan"), Desc.Water.Normal2.Panning == FVector2f(-0.0125f, 0.0f));
	TestEqual(TEXT("the second normal's texture is its own slot"), Desc.TextureFor(EMantlePlaceTextureSlot::WaterNormal2), FString(Normal2Id));
	TestEqual(TEXT("and the first is textures.normal"), Desc.TextureFor(EMantlePlaceTextureSlot::Normal), FString(TEXT("ast_u11111111111111111111111111111111")));
	TestEqual(TEXT("roughness is the document's"), Desc.RoughnessFactor, 0.01f);
	TestTrue(TEXT("two normals pick the WaterNormal2 variant"), Desc.ExtendedVariant() == EMantlePlaceExtendedVariant::WaterNormal2);

	// The lake: one normal in world space.
	if (!TestTrue(TEXT("the lake parses"),
		Parse(FString(Head) + TEXT(",\"water\":{\"normal\":{\"space\":\"world\",\"worldSizeCm\":500,\"panning\":[-0.0025,0.0011]}}}"), Desc, &Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("world space"), Desc.Water.Normal.Space == EMantlePlaceWaterSpace::World);
	TestEqual(TEXT("world size"), Desc.Water.Normal.WorldSizeCm, 500.0f);
	TestTrue(TEXT("pan"), Desc.Water.Normal.Panning == FVector2f(-0.0025f, 0.0011f));
	TestTrue(TEXT("one normal picks the Water variant"), Desc.ExtendedVariant() == EMantlePlaceExtendedVariant::Water);

	// The bounds, inclusive.
	TestTrue(TEXT("the extremes parse"),
		Parse(FString(Head) + TEXT(",\"water\":{\"scattering\":[1,0,1],\"absorption\":[0,1,0],\"phaseG\":1,\"colorScaleBehindWater\":0,")
			TEXT("\"normal\":{\"tiling\":[-1024,1024],\"panning\":[-10,10]},")
			TEXT("\"normal2\":{\"texture\":\"ast_u22222222222222222222222222222222\",\"space\":\"world\",\"worldSizeCm\":100000}}}"), Desc, &Error));

	// What it refuses.
	struct FRefusal
	{
		const TCHAR* Label;
		FString Json;
	};
	const FRefusal Refusals[] = {
		{ TEXT("a water number"), FString(Head) + TEXT(",\"water\":1}") },
		{ TEXT("an unknown water key"), FString(Head) + TEXT(",\"water\":{\"foam\":1}}") },
		{ TEXT("a scattering of two numbers"), FString(Head) + TEXT(",\"water\":{\"scattering\":[1,1]}}") },
		{ TEXT("an absorption above 1"), FString(Head) + TEXT(",\"water\":{\"absorption\":[0,2,0]}}") },
		{ TEXT("a negative scattering"), FString(Head) + TEXT(",\"water\":{\"scattering\":[0,-0.1,0]}}") },
		{ TEXT("a phaseG above 1"), FString(Head) + TEXT(",\"water\":{\"phaseG\":1.5}}") },
		{ TEXT("a colour scale below 0"), FString(Head) + TEXT(",\"water\":{\"colorScaleBehindWater\":-0.1}}") },
		{ TEXT("a normal array"), FString(Head) + TEXT(",\"water\":{\"normal\":[]}}") },
		{ TEXT("a texture on the first normal"), FString(Head) + TEXT(",\"water\":{\"normal\":{\"texture\":\"ast_u22222222222222222222222222222222\"}}}") },
		{ TEXT("an unknown space"), FString(Head) + TEXT(",\"water\":{\"normal\":{\"space\":\"screen\"}}}") },
		{ TEXT("world space with no size"), FString(Head) + TEXT(",\"water\":{\"normal\":{\"space\":\"world\"}}}") },
		{ TEXT("world space with a tiling"), FString(Head) + TEXT(",\"water\":{\"normal\":{\"space\":\"world\",\"worldSizeCm\":100,\"tiling\":[1,1]}}}") },
		{ TEXT("UV space with a size"), FString(Head) + TEXT(",\"water\":{\"normal\":{\"worldSizeCm\":100}}}") },
		{ TEXT("a size below 1 cm"), FString(Head) + TEXT(",\"water\":{\"normal\":{\"space\":\"world\",\"worldSizeCm\":0.5}}}") },
		{ TEXT("a tiling past 1024"), FString(Head) + TEXT(",\"water\":{\"normal\":{\"tiling\":[1,1025]}}}") },
		{ TEXT("a pan past 10"), FString(Head) + TEXT(",\"water\":{\"normal\":{\"panning\":[0,11]}}}") },
		{ TEXT("a second normal with no texture"), FString(Head) + TEXT(",\"water\":{\"normal2\":{\"panning\":[1,0]}}}") },
		{ TEXT("a second normal with a built-in id"), FString(Head) + TEXT(",\"water\":{\"normal2\":{\"texture\":\"ast_cube\"}}}") },
		{ TEXT("a second normal with an unknown key"), FString(Head) + TEXT(",\"water\":{\"normal2\":{\"texture\":\"ast_u22222222222222222222222222222222\",\"speed\":1}}}") },
		{ TEXT("water on a masked material"), TEXT("{\"schemaVersion\":1,\"alphaMode\":\"mask\",\"doubleSided\":false,\"water\":{}}") },
		{ TEXT("water on a blended material"), TEXT("{\"schemaVersion\":1,\"alphaMode\":\"blend\",\"doubleSided\":false,\"water\":{}}") },
		{ TEXT("water with wind"), FString(Head) + TEXT(",\"water\":{},\"wind\":{\"intensity\":1,\"speed\":1,\"weight\":\"uniform\"}}") },
		{ TEXT("water with world-aligned mapping"), FString(Head) + TEXT(",\"water\":{},\"worldAligned\":{\"tileSizeCm\":100,\"sharpness\":1}}") },
		{ TEXT("water with a layer blend"), FString(Head) + TEXT(",\"water\":{},\"layerBlend\":{\"channel\":\"r\"}}") },
		{ TEXT("water with foliage"), TEXT("{\"schemaVersion\":1,\"alphaMode\":\"mask\",\"doubleSided\":true,\"water\":{},\"foliage\":{}}") },
		{ TEXT("water on car paint"), FString(Head) + TEXT(",\"shading\":\"carPaint\",\"water\":{}}") },
	};
	for (const FRefusal& Refusal : Refusals)
	{
		FMantlePlaceMaterialDesc Refused;
		TestFalse(FString::Printf(TEXT("%s is refused"), Refusal.Label), Parse(Refusal.Json, Refused));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceMaterialWaterPermutationTest,
	"Mantle.Runtime.PlaceMaterial.Water.Permutations",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceMaterialWaterPermutationTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceMaterialWaterTestPrivate;
	using FM = FMantlePlaceMaterials;

	TestEqual(TEXT("23 + 2 water + 2 additive permutations are cooked"), FM::PermutationCount, 27);
	TestEqual(TEXT("Water is 23"), FM::WaterPermutationIndex, 23);
	TestEqual(TEXT("WaterNormal2 is 24"), FM::WaterNormal2PermutationIndex, 24);

	// Opaque only, either sidedness; one instance for both.
	const EMantlePlaceAlphaMode Opaque = EMantlePlaceAlphaMode::Opaque;
	for (const bool bTwoSided : { false, true })
	{
		TestEqual(TEXT("Water index"), FM::ExtendedPermutationIndex(EMantlePlaceExtendedVariant::Water, Opaque, bTwoSided), FM::WaterPermutationIndex);
		TestEqual(TEXT("WaterNormal2 index"), FM::ExtendedPermutationIndex(EMantlePlaceExtendedVariant::WaterNormal2, Opaque, bTwoSided), FM::WaterNormal2PermutationIndex);
		TestEqual(TEXT("Water name"), FM::ExtendedPermutationName(EMantlePlaceExtendedVariant::Water, Opaque, bTwoSided), FString(TEXT("MI_MantlePlace_Water")));
		TestEqual(TEXT("WaterNormal2 name"), FM::ExtendedPermutationName(EMantlePlaceExtendedVariant::WaterNormal2, Opaque, bTwoSided),
			FString(TEXT("MI_MantlePlace_Water_Normal2")));
	}
	for (const EMantlePlaceAlphaMode Other : { EMantlePlaceAlphaMode::Mask, EMantlePlaceAlphaMode::Blend })
	{
		TestEqual(TEXT("Water is not cooked for another alpha mode"),
			FM::ExtendedPermutationIndex(EMantlePlaceExtendedVariant::Water, Other, false), static_cast<int32>(INDEX_NONE));
		TestEqual(TEXT("WaterNormal2 is not cooked for another alpha mode"),
			FM::ExtendedPermutationIndex(EMantlePlaceExtendedVariant::WaterNormal2, Other, true), static_cast<int32>(INDEX_NONE));
	}
	TestEqual(TEXT("the water path"), FM::ExtendedPermutationPath(EMantlePlaceExtendedVariant::Water, Opaque, false).ToString(),
		FString(TEXT("/Game/Mantle/Materials/MI_MantlePlace_Water.MI_MantlePlace_Water")));

	// A document picks them; none of the earlier 23 moved.
	FMantlePlaceMaterialDesc Desc;
	FString Error;
	if (!TestTrue(TEXT("water parses"), Parse(FString(Head) + TEXT(",\"water\":{}}"), Desc, &Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("a water document picks the Water instance"), FM::PermutationIndex(Desc), FM::WaterPermutationIndex);
	TestEqual(TEXT("by name"), FM::PermutationName(Desc), FString(TEXT("MI_MantlePlace_Water")));
	if (!TestTrue(TEXT("water with a second normal parses"),
		Parse(FString(Head) + TEXT(",\"water\":{\"normal2\":{\"texture\":\"ast_u22222222222222222222222222222222\"}}}"), Desc, &Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("with a second normal it picks WaterNormal2"), FM::PermutationIndex(Desc), FM::WaterNormal2PermutationIndex);
	TestEqual(TEXT("by name"), FM::PermutationName(Desc), FString(TEXT("MI_MantlePlace_Water_Normal2")));
	if (!TestTrue(TEXT("a two-sided water document parses"), Parse(TEXT("{\"schemaVersion\":1,\"alphaMode\":\"opaque\",\"doubleSided\":true,\"water\":{}}"), Desc, &Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("sidedness does not change it"), FM::PermutationIndex(Desc), FM::WaterPermutationIndex);

	// The second normal is a texture slot of its own, read as a normal map.
	TestEqual(TEXT("slot name"), FString(FM::SlotName(EMantlePlaceTextureSlot::WaterNormal2)), FString(TEXT("waterNormal2")));
	TestTrue(TEXT("slot mirrors the normal"), FM::BaseSlotOf(EMantlePlaceTextureSlot::WaterNormal2) == EMantlePlaceTextureSlot::Normal);
	TestTrue(TEXT("slot role is Normal"), FM::RoleForSlot(EMantlePlaceTextureSlot::WaterNormal2) == EMantleTextureRole::Normal);
	TestEqual(TEXT("eight textures are bound"), MantlePlaceMaterialTextureCount, 8);

	// Existing documents are untouched: a plain document is still M_MantlePlace's, foliage and wind keep theirs.
	if (!TestTrue(TEXT("a plain document parses"), Parse(FString(Head) + TEXT("}"), Desc, &Error)))
	{
		AddError(Error);
		return false;
	}
	TestFalse(TEXT("a plain document has no water"), Desc.bHasWater);
	TestEqual(TEXT("and keeps M_MantlePlace's permutation"), FM::PermutationIndex(Desc), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceMaterialWaterApplyTest,
	"Mantle.Runtime.PlaceMaterial.Water.Apply",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceMaterialWaterApplyTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceMaterialWaterTestPrivate;
	namespace P = MantlePlaceMaterialParameters;

	FMantlePlaceMaterialDesc Desc;
	FString Error;
	if (!TestTrue(TEXT("a river and lake parse"),
		Parse(FString(Head) + TEXT(",\"water\":{\"scattering\":[0.003,0.017,0.018],\"absorption\":[0.027,0.013,0.012],\"phaseG\":0.5,")
			TEXT("\"colorScaleBehindWater\":0.25,\"normal\":{\"tiling\":[0.25,0.5],\"panning\":[-0.0025,0]},")
			TEXT("\"normal2\":{\"texture\":\"ast_u22222222222222222222222222222222\",\"space\":\"world\",\"worldSizeCm\":400,\"panning\":[0.01,-0.02]}}}"),
			Desc, &Error)))
	{
		AddError(Error);
		return false;
	}

	UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(UMaterial::GetDefaultMaterial(MD_Surface), GetTransientPackage());
	if (!TestNotNull(TEXT("an instance on the default material"), Instance))
	{
		return false;
	}
	FMantlePlaceMaterials::ApplyWater(Instance, Desc.Water);
	TestTrue(TEXT("scattering"), VectorOf(Instance, P::WaterScattering).Equals(FLinearColor(0.003f, 0.017f, 0.018f, 1.0f)));
	TestTrue(TEXT("absorption"), VectorOf(Instance, P::WaterAbsorption).Equals(FLinearColor(0.027f, 0.013f, 0.012f, 1.0f)));
	TestEqual(TEXT("phaseG"), ScalarOf(Instance, P::WaterPhaseG), 0.5f);
	TestEqual(TEXT("colour scale"), ScalarOf(Instance, P::WaterColorScale), 0.25f);
	TestTrue(TEXT("first tiling"), VectorOf(Instance, P::NormalUVTiling).Equals(FLinearColor(0.25f, 0.5f, 0.0f, 0.0f)));
	TestEqual(TEXT("first layer is mesh UVs"), ScalarOf(Instance, P::NormalWorldSpace), 0.0f);
	TestTrue(TEXT("first pan"), VectorOf(Instance, P::NormalUVPanning).Equals(FLinearColor(-0.0025f, 0.0f, 0.0f, 0.0f)));
	TestEqual(TEXT("second layer is world space"), ScalarOf(Instance, P::Normal2WorldSpace), 1.0f);
	TestEqual(TEXT("its world scale is repeats a centimetre"), ScalarOf(Instance, P::Normal2WorldScale), 1.0f / 400.0f);
	TestTrue(TEXT("second pan"), VectorOf(Instance, P::Normal2UVPanning).Equals(FLinearColor(0.01f, -0.02f, 0.0f, 0.0f)));

	// ApplyExtended on a water document also writes the water values, and keeps the first normal's tiling in the
	// extended master's rows (the fallback when M_MantlePlace_Water is missing).
	UMaterialInstanceDynamic* Extended = UMaterialInstanceDynamic::Create(UMaterial::GetDefaultMaterial(MD_Surface), GetTransientPackage());
	if (!TestNotNull(TEXT("a second instance"), Extended))
	{
		return false;
	}
	FMantlePlaceMaterials::ApplyExtended(Extended, Desc);
	TestEqual(TEXT("phaseG through ApplyExtended"), ScalarOf(Extended, P::WaterPhaseG), 0.5f);
	TestTrue(TEXT("the first tiling is the normal's UV rows"), VectorOf(Extended, P::NormalUVTransformU).Equals(FLinearColor(0.25f, 0.0f, 0.0f, 0.0f)));
	TestTrue(TEXT("(V row)"), VectorOf(Extended, P::NormalUVTransformV).Equals(FLinearColor(0.0f, 0.5f, 0.0f, 0.0f)));
	TestTrue(TEXT("the pan is the water's, not uvPanning's"), VectorOf(Extended, P::NormalUVPanning).Equals(FLinearColor(-0.0025f, 0.0f, 0.0f, 0.0f)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceMaterialWaterFallbackTest,
	"Mantle.Runtime.PlaceMaterial.Water.Fallback",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceMaterialWaterFallbackTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceMaterialWaterTestPrivate;

	// With the water content present the document gets its instance; with it missing it gets Extended's, then
	// M_MantlePlace's, then the engine's default, and Build still returns an instance, never null.
	FMantlePlaceMaterials::ResetForTests();
	FMantlePlaceMaterialDesc Desc;
	FString Error;
	if (!TestTrue(TEXT("water parses"), Parse(FString(Head) + TEXT(",\"water\":{}}"), Desc, &Error)))
	{
		AddError(Error);
		return false;
	}
	bool bFellBack = false;
	UMaterialInterface* Parent = FMantlePlaceMaterials::LoadPermutation(Desc, &bFellBack);
	TestNotNull(TEXT("a water document always has a parent"), Parent);

	FString BuildError;
	UMaterialInstanceDynamic* Instance = FMantlePlaceMaterials::Build(Desc, TConstArrayView<UTexture*>(), nullptr, NAME_None, &BuildError);
	if (TestNotNull(TEXT("Build returns an instance"), Instance))
	{
		TestEqual(TEXT("with the document's roughness (the default 1)"), ScalarOf(Instance, MantlePlaceMaterialParameters::RoughnessFactor), 1.0f);
		TestEqual(TEXT("and the water values"), ScalarOf(Instance, MantlePlaceMaterialParameters::WaterColorScale), 1.0f);
	}
	else
	{
		AddError(BuildError);
	}
	FMantlePlaceMaterials::ResetForTests();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
