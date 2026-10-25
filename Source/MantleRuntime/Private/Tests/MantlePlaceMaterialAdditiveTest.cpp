// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceMaterial.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Additive blending on the place master: the document's alphaMode additive, the two permutations it picks
// (one- and two-sided), what the parser refuses next to it, and that the alpha modes before it did not move. No
// world and no content: the permutation assets are not loaded.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantlePlaceMaterialAdditiveTestPrivate
{
	bool Parse(const FString& Json, FMantlePlaceMaterialDesc& Out, FString* Error = nullptr)
	{
		const FTCHARToUTF8 Utf8(*Json);
		return FMantlePlaceMaterials::ParseMaterial(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), Out, Error);
	}

	FString Head(const bool bTwoSided, const TCHAR* Mode = TEXT("additive"))
	{
		return FString::Printf(TEXT("{\"schemaVersion\":1,\"alphaMode\":\"%s\",\"doubleSided\":%s"), Mode, bTwoSided ? TEXT("true") : TEXT("false"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceMaterialAdditiveParseTest,
	"Mantle.Runtime.PlaceMaterial.Additive.Parse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceMaterialAdditiveParseTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceMaterialAdditiveTestPrivate;

	FMantlePlaceMaterialDesc Desc;
	FString Error;
	if (!TestTrue(TEXT("a minimal additive document parses"), Parse(Head(false) + TEXT("}"), Desc, &Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("its alpha mode is additive"), Desc.AlphaMode == EMantlePlaceAlphaMode::Additive);
	TestEqual(TEXT("and it reads back as additive"), FString(FMantlePlaceMaterials::AlphaModeName(Desc.AlphaMode)), FString(TEXT("additive")));
	TestTrue(TEXT("it is the Additive variant"), Desc.ExtendedVariant() == EMantlePlaceExtendedVariant::Additive);
	TestTrue(TEXT("and builds on M_MantlePlace_Extended"), Desc.UsesExtended());

	// Emissive, a pan and a texture ride along; the glow is the factor times the strength.
	if (!TestTrue(TEXT("an additive document with emissive values parses"),
		Parse(Head(true) + TEXT(",\"emissiveFactor\":[0.037,0.91,1],\"emissiveStrength\":2.5,\"uvPanning\":{\"emissive\":[0.05,0.05]},")
			TEXT("\"textures\":{\"emissive\":\"ast_u0123456789abcdef0123456789abcdef\"}}"), Desc, &Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("two-sided"), Desc.bDoubleSided);
	TestEqual(TEXT("emissive strength"), Desc.EmissiveStrength, 2.5f);
	TestEqual(TEXT("emissive green"), Desc.EmissiveFactor.G, 0.91f);
	TestEqual(TEXT("the emissive pan"), Desc.UVPanning[static_cast<int32>(EMantlePlaceTextureSlot::Emissive)].X, 0.05f);
	TestFalse(TEXT("the emissive texture is named"), Desc.TextureFor(EMantlePlaceTextureSlot::Emissive).IsEmpty());

	// What it refuses: the keys that need another master or alpha mode, an upper-case value, a cutoff.
	struct FRefusal
	{
		const TCHAR* What;
		FString Json;
		const TCHAR* Says;
	};
	const FRefusal Refusals[] = {
		{ TEXT("wind"), Head(false) + TEXT(",\"wind\":{\"intensity\":1,\"speed\":1,\"weight\":\"uniform\"}}"), TEXT("/alphaMode") },
		{ TEXT("foliage"), Head(true) + TEXT(",\"foliage\":{}}"), TEXT("/alphaMode") },
		{ TEXT("water"), Head(false) + TEXT(",\"water\":{}}"), TEXT("/alphaMode") },
		{ TEXT("a layer blend"), Head(false) + TEXT(",\"layerBlend\":{\"channel\":\"r\"}}"), TEXT("/alphaMode") },
		{ TEXT("world-aligned mapping"), Head(false) + TEXT(",\"worldAligned\":{\"tileSizeCm\":100,\"sharpness\":1}}"), TEXT("/alphaMode") },
		{ TEXT("car paint"), Head(false) + TEXT(",\"shading\":\"carPaint\"}"), TEXT("/alphaMode") },
		{ TEXT("a cutoff"), Head(false) + TEXT(",\"alphaCutoff\":0.5}"), TEXT("/alphaCutoff") },
		{ TEXT("an upper-case mode"), Head(false, TEXT("Additive")) + TEXT("}"), TEXT("/alphaMode") },
	};
	for (const FRefusal& Refusal : Refusals)
	{
		FMantlePlaceMaterialDesc Refused;
		FString Why;
		TestFalse(FString::Printf(TEXT("additive with %s is refused"), Refusal.What), Parse(Refusal.Json, Refused, &Why));
		TestTrue(FString::Printf(TEXT("and the refusal for %s names %s: %s"), Refusal.What, Refusal.Says, *Why), Why.Contains(Refusal.Says));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceMaterialAdditivePermutationTest,
	"Mantle.Runtime.PlaceMaterial.Additive.Permutations",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceMaterialAdditivePermutationTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceMaterialAdditiveTestPrivate;
	using FM = FMantlePlaceMaterials;

	const EMantlePlaceExtendedVariant Additive = EMantlePlaceExtendedVariant::Additive;
	const EMantlePlaceAlphaMode Mode = EMantlePlaceAlphaMode::Additive;
	TestEqual(TEXT("the two additive permutations follow the water ones"), FM::AdditivePermutationBase, FM::WaterNormal2PermutationIndex + 1);
	TestEqual(TEXT("and are the last two cooked"), FM::AdditivePermutationBase + 2, FM::PermutationCount);

	TestEqual(TEXT("Additive one-sided index"), FM::ExtendedPermutationIndex(Additive, Mode, false), FM::AdditivePermutationBase);
	TestEqual(TEXT("Additive two-sided index"), FM::ExtendedPermutationIndex(Additive, Mode, true), FM::AdditivePermutationBase + 1);
	TestEqual(TEXT("Additive is cooked for the additive mode only"),
		FM::ExtendedPermutationIndex(Additive, EMantlePlaceAlphaMode::Blend, false), static_cast<int32>(INDEX_NONE));
	for (const EMantlePlaceExtendedVariant Other : { EMantlePlaceExtendedVariant::Wind, EMantlePlaceExtendedVariant::WorldAligned, EMantlePlaceExtendedVariant::LayerBlend,
			 EMantlePlaceExtendedVariant::Foliage, EMantlePlaceExtendedVariant::FoliageWind, EMantlePlaceExtendedVariant::Water, EMantlePlaceExtendedVariant::WaterNormal2 })
	{
		TestEqual(TEXT("no other variant is cooked for the additive mode"), FM::ExtendedPermutationIndex(Other, Mode, false), static_cast<int32>(INDEX_NONE));
		TestEqual(TEXT("not two-sided either"), FM::ExtendedPermutationIndex(Other, Mode, true), static_cast<int32>(INDEX_NONE));
	}

	TestEqual(TEXT("Additive name"), FM::ExtendedPermutationName(Additive, Mode, false), FString(TEXT("MI_MantlePlace_Extended_Additive")));
	TestEqual(TEXT("Additive two-sided name"), FM::ExtendedPermutationName(Additive, Mode, true), FString(TEXT("MI_MantlePlace_Extended_Additive_TwoSided")));
	TestEqual(TEXT("Additive path"), FM::ExtendedPermutationPath(Additive, Mode, true).ToString(),
		FString(TEXT("/Game/Mantle/Materials/MI_MantlePlace_Extended_Additive_TwoSided.MI_MantlePlace_Extended_Additive_TwoSided")));

	// A document picks them by sidedness.
	FMantlePlaceMaterialDesc Desc;
	FString Error;
	if (!TestTrue(TEXT("a one-sided additive document parses"), Parse(Head(false) + TEXT("}"), Desc, &Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("a one-sided additive document picks the one-sided permutation"), FM::PermutationIndex(Desc), FM::AdditivePermutationBase);
	TestEqual(TEXT("by name"), FM::PermutationName(Desc), FString(TEXT("MI_MantlePlace_Extended_Additive")));
	if (!TestTrue(TEXT("a two-sided additive document parses"), Parse(Head(true) + TEXT("}"), Desc, &Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("a two-sided one picks the two-sided permutation"), FM::PermutationIndex(Desc), FM::AdditivePermutationBase + 1);
	TestEqual(TEXT("its path"), FM::PermutationPath(Desc).ToString(),
		FString(TEXT("/Game/Mantle/Materials/MI_MantlePlace_Extended_Additive_TwoSided.MI_MantlePlace_Extended_Additive_TwoSided")));

	// With the Additive permutations missing, a document falls back on the translucent family, never the opaque one.
	TestEqual(TEXT("additive stands in as Translucent among the six"), FM::PermutationIndex(Mode, false), FM::PermutationIndex(EMantlePlaceAlphaMode::Blend, false));
	TestEqual(TEXT("and as Translucent two-sided"), FM::PermutationIndex(Mode, true), FM::PermutationIndex(EMantlePlaceAlphaMode::Blend, true));

	// Nothing before it moved: the six, car paint, the extended six, wind, world-aligned, layer blend, foliage and water.
	TestEqual(TEXT("opaque stays 0"), FM::PermutationIndex(EMantlePlaceAlphaMode::Opaque, false), 0);
	TestEqual(TEXT("translucent two-sided stays 5"), FM::PermutationIndex(EMantlePlaceAlphaMode::Blend, true), 5);
	TestEqual(TEXT("wind on mask two-sided stays 16"), FM::ExtendedPermutationIndex(EMantlePlaceExtendedVariant::Wind, EMantlePlaceAlphaMode::Mask, true), 16);
	TestEqual(TEXT("wind on translucent is still not cooked"),
		FM::ExtendedPermutationIndex(EMantlePlaceExtendedVariant::Wind, EMantlePlaceAlphaMode::Blend, false), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("plain extended translucent two-sided stays 12"), FM::ExtendedPermutationIndex(EMantlePlaceExtendedVariant::Plain, EMantlePlaceAlphaMode::Blend, true), 12);
	TestEqual(TEXT("water stays 23"), FM::WaterPermutationIndex, 23);

	// A blend document is unchanged: translucent, not the additive variant.
	if (!TestTrue(TEXT("a blend document parses"), Parse(Head(false, TEXT("blend")) + TEXT("}"), Desc, &Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("a blend document keeps M_MantlePlace's Translucent"), FM::PermutationIndex(Desc), 4);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
