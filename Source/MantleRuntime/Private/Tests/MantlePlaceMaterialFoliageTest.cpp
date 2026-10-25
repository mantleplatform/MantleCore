// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceMaterial.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Foliage shading on the place master: the document's foliage key, the two permutations it picks (with and
// without wind), and what the parser refuses. No world and no content: the permutation assets are not loaded.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantlePlaceMaterialFoliageTestPrivate
{
	bool Parse(const FString& Json, FMantlePlaceMaterialDesc& Out, FString* Error = nullptr)
	{
		const FTCHARToUTF8 Utf8(*Json);
		return FMantlePlaceMaterials::ParseMaterial(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), Out, Error);
	}

	const TCHAR* const Head = TEXT("{\"schemaVersion\":1,\"alphaMode\":\"mask\",\"alphaCutoff\":0.3333,\"doubleSided\":true");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceMaterialFoliageParseTest,
	"Mantle.Runtime.PlaceMaterial.Foliage.Parse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceMaterialFoliageParseTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceMaterialFoliageTestPrivate;

	// An empty object is the defaults: the tinted base colour as it is.
	FMantlePlaceMaterialDesc Desc;
	FString Error;
	if (!TestTrue(TEXT("an empty foliage object parses"), Parse(FString(Head) + TEXT(",\"foliage\":{}}"), Desc, &Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("it has foliage"), Desc.bHasFoliage);
	TestTrue(TEXT("tint defaults to white"), Desc.Foliage.Tint.Equals(FLinearColor::White));
	TestEqual(TEXT("strength defaults to 1"), Desc.Foliage.Strength, 1.0f);
	TestEqual(TEXT("the vertex-colour weight defaults to 0"), Desc.Foliage.VertexColorWeight, 0.0f);
	TestTrue(TEXT("foliage alone builds on M_MantlePlace_Extended"), Desc.UsesExtended());
	TestTrue(TEXT("and is the Foliage variant"), Desc.ExtendedVariant() == EMantlePlaceExtendedVariant::Foliage);

	// Every value, and wind beside it.
	if (!TestTrue(TEXT("foliage with its values and wind parses"),
		Parse(FString(Head) + TEXT(",\"foliage\":{\"tint\":[1,0.5,0.25],\"strength\":2.5,\"vertexColorWeight\":0.75},")
			TEXT("\"wind\":{\"intensity\":1,\"speed\":2,\"weight\":\"vertexColorR\"}}"), Desc, &Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("tint"), Desc.Foliage.Tint.Equals(FLinearColor(1.0f, 0.5f, 0.25f, 1.0f)));
	TestEqual(TEXT("strength"), Desc.Foliage.Strength, 2.5f);
	TestEqual(TEXT("vertex-colour weight"), Desc.Foliage.VertexColorWeight, 0.75f);
	TestTrue(TEXT("foliage with wind is the FoliageWind variant"), Desc.ExtendedVariant() == EMantlePlaceExtendedVariant::FoliageWind);

	// What it refuses.
	struct FRefusal
	{
		const TCHAR* Label;
		FString Json;
	};
	const FRefusal Refusals[] = {
		{ TEXT("a foliage number"), FString(Head) + TEXT(",\"foliage\":1}") },
		{ TEXT("an unknown foliage key"), FString(Head) + TEXT(",\"foliage\":{\"glow\":1}}") },
		{ TEXT("a strength above 4"), FString(Head) + TEXT(",\"foliage\":{\"strength\":4.5}}") },
		{ TEXT("a tint of two numbers"), FString(Head) + TEXT(",\"foliage\":{\"tint\":[1,1]}}") },
		{ TEXT("a tint above 1"), FString(Head) + TEXT(",\"foliage\":{\"tint\":[1,2,1]}}") },
		{ TEXT("a weight above 1"), FString(Head) + TEXT(",\"foliage\":{\"vertexColorWeight\":2}}") },
		{ TEXT("foliage on an opaque material"), TEXT("{\"schemaVersion\":1,\"alphaMode\":\"opaque\",\"doubleSided\":true,\"foliage\":{}}") },
		{ TEXT("foliage on a one-sided material"), TEXT("{\"schemaVersion\":1,\"alphaMode\":\"mask\",\"doubleSided\":false,\"foliage\":{}}") },
		{ TEXT("foliage with a layer blend"), FString(Head) + TEXT(",\"foliage\":{},\"layerBlend\":{\"channel\":\"r\"}}") },
		{ TEXT("foliage with world-aligned mapping"), FString(Head) + TEXT(",\"foliage\":{},\"worldAligned\":{\"tileSizeCm\":100,\"sharpness\":1}}") },
		{ TEXT("foliage on car paint"), TEXT("{\"schemaVersion\":1,\"alphaMode\":\"opaque\",\"doubleSided\":false,\"shading\":\"carPaint\",\"foliage\":{}}") },
	};
	for (const FRefusal& Refusal : Refusals)
	{
		FMantlePlaceMaterialDesc Refused;
		TestFalse(FString::Printf(TEXT("%s is refused"), Refusal.Label), Parse(Refusal.Json, Refused));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceMaterialFoliagePermutationTest,
	"Mantle.Runtime.PlaceMaterial.Foliage.Permutations",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceMaterialFoliagePermutationTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceMaterialFoliageTestPrivate;
	using FM = FMantlePlaceMaterials;

	TestEqual(TEXT("21 + 2 foliage + 2 water + 2 additive permutations are cooked"), FM::PermutationCount, 27);

	// Both are masked and two-sided, and only those.
	const EMantlePlaceAlphaMode Mask = EMantlePlaceAlphaMode::Mask;
	TestEqual(TEXT("Foliage index"), FM::ExtendedPermutationIndex(EMantlePlaceExtendedVariant::Foliage, Mask, true), FM::FoliagePermutationIndex);
	TestEqual(TEXT("FoliageWind index"), FM::ExtendedPermutationIndex(EMantlePlaceExtendedVariant::FoliageWind, Mask, true), FM::FoliageWindPermutationIndex);
	TestEqual(TEXT("Foliage is not cooked one-sided"), FM::ExtendedPermutationIndex(EMantlePlaceExtendedVariant::Foliage, Mask, false), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("Foliage is not cooked opaque"),
		FM::ExtendedPermutationIndex(EMantlePlaceExtendedVariant::Foliage, EMantlePlaceAlphaMode::Opaque, true), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("FoliageWind is not cooked translucent"),
		FM::ExtendedPermutationIndex(EMantlePlaceExtendedVariant::FoliageWind, EMantlePlaceAlphaMode::Blend, true), static_cast<int32>(INDEX_NONE));

	TestEqual(TEXT("Foliage name"), FM::ExtendedPermutationName(EMantlePlaceExtendedVariant::Foliage, Mask, true),
		FString(TEXT("MI_MantlePlace_Extended_Foliage_Masked_TwoSided")));
	TestEqual(TEXT("FoliageWind name"), FM::ExtendedPermutationName(EMantlePlaceExtendedVariant::FoliageWind, Mask, true),
		FString(TEXT("MI_MantlePlace_Extended_FoliageWind_Masked_TwoSided")));

	// A document picks them; the permutations after the wind ones do not overlap anything cooked before.
	FMantlePlaceMaterialDesc Desc;
	FString Error;
	if (!TestTrue(TEXT("foliage parses"), Parse(FString(Head) + TEXT(",\"foliage\":{}}"), Desc, &Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("a foliage document picks the Foliage permutation"), FM::PermutationIndex(Desc), FM::FoliagePermutationIndex);
	TestEqual(TEXT("by name"), FM::PermutationName(Desc), FString(TEXT("MI_MantlePlace_Extended_Foliage_Masked_TwoSided")));
	if (!TestTrue(TEXT("foliage with wind parses"),
		Parse(FString(Head) + TEXT(",\"foliage\":{},\"wind\":{\"intensity\":1,\"speed\":1,\"weight\":\"uniform\"}}"), Desc, &Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("with wind it picks FoliageWind"), FM::PermutationIndex(Desc), FM::FoliageWindPermutationIndex);

	// Wind alone still picks the Wind permutation (the existing masked, two-sided one).
	if (!TestTrue(TEXT("wind alone parses"),
		Parse(FString(Head) + TEXT(",\"wind\":{\"intensity\":1,\"speed\":1,\"weight\":\"uniform\"}}"), Desc, &Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("wind alone keeps its permutation"), FM::PermutationIndex(Desc), FM::WindPermutationBase + 3);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
