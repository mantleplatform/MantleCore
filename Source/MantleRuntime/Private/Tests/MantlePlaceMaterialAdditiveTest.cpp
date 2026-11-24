// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceMaterial.h"

#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// Additive blending on the place master: the document's alphaMode additive, the two permutations it picks
// (one- and two-sided), what the parser refuses next to it, and that the alpha modes before it did not move; and its
// Fresnel rim glow (the rim key, F4). No world; only the Rim.Content test loads the permutation assets.

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

	/** The value the instance itself holds for Name (not the parent's), -1 when it holds none. */
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceMaterialAdditiveRimParseTest,
	"Mantle.Runtime.PlaceMaterial.Additive.Rim.Parse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceMaterialAdditiveRimParseTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceMaterialAdditiveTestPrivate;

	FMantlePlaceMaterialDesc Desc;
	FString Error;
	if (!TestTrue(TEXT("an additive document with a rim parses"),
		Parse(Head(true) + TEXT(",\"emissiveFactor\":[0.02,0.08,0.12],\"rim\":{\"color\":[0.05,0.6,1],\"strength\":3,\"power\":5}}"), Desc, &Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("rim color"), Desc.Rim.Color.Equals(FLinearColor(0.05f, 0.6f, 1.0f, 1.0f)));
	TestEqual(TEXT("rim strength"), Desc.Rim.Strength, 3.0f);
	TestEqual(TEXT("rim power"), Desc.Rim.Power, 5.0f);
	TestTrue(TEXT("still the Additive variant"), Desc.ExtendedVariant() == EMantlePlaceExtendedVariant::Additive);

	// No rim key: black, so the Additive instances' rim adds nothing.
	if (!TestTrue(TEXT("an additive document without a rim parses"), Parse(Head(false) + TEXT("}"), Desc, &Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("no rim is black"), Desc.Rim.Color.Equals(FLinearColor::Black));
	TestEqual(TEXT("with no strength"), Desc.Rim.Strength, 0.0f);

	struct FRefusal
	{
		const TCHAR* What;
		FString Json;
		const TCHAR* Says;
	};
	const FString Rim = TEXT(",\"rim\":{\"color\":[1,1,1],\"strength\":1,\"power\":5}}");
	const FRefusal Refusals[] = {
		{ TEXT("a rim on a blend material"), Head(false, TEXT("blend")) + Rim, TEXT("/rim") },
		{ TEXT("a rim on an opaque material"), Head(false, TEXT("opaque")) + Rim, TEXT("/rim") },
		{ TEXT("a rim with no power"), Head(false) + TEXT(",\"rim\":{\"color\":[1,1,1],\"strength\":1}}"), TEXT("/rim") },
		{ TEXT("a rim power above 64"), Head(false) + TEXT(",\"rim\":{\"color\":[1,1,1],\"strength\":1,\"power\":65}}"), TEXT("/rim/power") },
		{ TEXT("a rim strength above 100"), Head(false) + TEXT(",\"rim\":{\"color\":[1,1,1],\"strength\":101,\"power\":5}}"), TEXT("/rim/strength") },
		{ TEXT("a rim color above 1"), Head(false) + TEXT(",\"rim\":{\"color\":[1,2,1],\"strength\":1,\"power\":5}}"), TEXT("/rim/color") },
		{ TEXT("an unknown rim key"), Head(false) + TEXT(",\"rim\":{\"color\":[1,1,1],\"strength\":1,\"power\":5,\"width\":2}}"), TEXT("/rim/width") },
		{ TEXT("a rim that is not an object"), Head(false) + TEXT(",\"rim\":5}"), TEXT("/rim") },
	};
	for (const FRefusal& Refusal : Refusals)
	{
		FMantlePlaceMaterialDesc Refused;
		FString Why;
		TestFalse(FString::Printf(TEXT("%s is refused"), Refusal.What), Parse(Refusal.Json, Refused, &Why));
		TestTrue(FString::Printf(TEXT("and the refusal for %s names %s: %s"), Refusal.What, Refusal.Says, *Why), Why.Contains(Refusal.Says));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceMaterialAdditiveRimApplyTest,
	"Mantle.Runtime.PlaceMaterial.Additive.Rim.Apply",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceMaterialAdditiveRimApplyTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceMaterialAdditiveTestPrivate;
	namespace P = MantlePlaceMaterialParameters;

	FMantlePlaceMaterialDesc Desc;
	FString Error;
	if (!TestTrue(TEXT("an additive document with a rim parses"),
		Parse(Head(false) + TEXT(",\"rim\":{\"color\":[0.05,0.6,1],\"strength\":3,\"power\":2.5}}"), Desc, &Error)))
	{
		AddError(Error);
		return false;
	}
	UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(UMaterial::GetDefaultMaterial(MD_Surface), GetTransientPackage());
	if (!TestNotNull(TEXT("an instance on the default material"), Instance))
	{
		return false;
	}
	FMantlePlaceMaterials::ApplyExtended(Instance, Desc);
	TestTrue(TEXT("RimColor"), VectorOf(Instance, P::RimColor).Equals(FLinearColor(0.05f, 0.6f, 1.0f, 1.0f)));
	TestEqual(TEXT("RimStrength"), ScalarOf(Instance, P::RimStrength), 3.0f);
	TestEqual(TEXT("RimPower"), ScalarOf(Instance, P::RimPower), 2.5f);

	// An additive document without a rim still writes the three, black: an instance never keeps another's rim.
	FMantlePlaceMaterialDesc Plain;
	TestTrue(TEXT("a plain additive document parses"), Parse(Head(false) + TEXT("}"), Plain));
	UMaterialInstanceDynamic* Unrimmed = UMaterialInstanceDynamic::Create(UMaterial::GetDefaultMaterial(MD_Surface), GetTransientPackage());
	if (!TestNotNull(TEXT("a second instance"), Unrimmed))
	{
		return false;
	}
	FMantlePlaceMaterials::ApplyExtended(Unrimmed, Plain);
	TestTrue(TEXT("no rim writes black"), VectorOf(Unrimmed, P::RimColor).Equals(FLinearColor(0.0f, 0.0f, 0.0f, 1.0f)));
	TestEqual(TEXT("and no strength"), ScalarOf(Unrimmed, P::RimStrength), 0.0f);

	// Another variant writes none of them.
	FMantlePlaceMaterialDesc Blend;
	TestTrue(TEXT("a panned blend document parses"), Parse(Head(false, TEXT("blend")) + TEXT(",\"uvPanning\":{\"emissive\":[0.1,0]}}"), Blend));
	UMaterialInstanceDynamic* Other = UMaterialInstanceDynamic::Create(UMaterial::GetDefaultMaterial(MD_Surface), GetTransientPackage());
	if (!TestNotNull(TEXT("a third instance"), Other))
	{
		return false;
	}
	FMantlePlaceMaterials::ApplyExtended(Other, Blend);
	TestEqual(TEXT("a blend material has no rim strength"), ScalarOf(Other, P::RimStrength), -1.0f);
	return true;
}

#if WITH_EDITORONLY_DATA
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceMaterialAdditiveRimContentTest,
	"Mantle.Runtime.PlaceMaterial.Additive.Rim.Content",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceMaterialAdditiveRimContentTest::RunTest(const FString& Parameters)
{
	using FM = FMantlePlaceMaterials;
	namespace P = MantlePlaceMaterialParameters;

	// The MantleCore plugin keeps a copy of this test, in projects that have no client content: there is nothing to check.
	if (!FPackageName::DoesPackageExist(FSoftObjectPath(FM::ExtendedMasterMaterialPath).GetLongPackageName()))
	{
		AddInfo(TEXT("M_MantlePlace_Extended is not in this project (the plugin's copy of the test): nothing to check."));
		return true;
	}

	// The AdditiveRim switch is on in both Additive instances (a place cannot turn it off: a dynamic instance cannot
	// change a static switch) and off in the plain extended ones, which compile as before.
	auto SwitchOf = [this](const FSoftObjectPath& Path, bool& bOut) -> bool
	{
		const UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(Path.TryLoad());
		if (!TestNotNull(FString::Printf(TEXT("%s loads"), *Path.ToString()), Instance))
		{
			return false;
		}
		FGuid Guid;
		bOut = false;
		return TestTrue(FString::Printf(TEXT("%s has the AdditiveRim switch"), *Path.ToString()),
			Instance->GetStaticSwitchParameterValue(FHashedMaterialParameterInfo(TEXT("AdditiveRim")), bOut, Guid));
	};
	for (const bool bTwoSided : { false, true })
	{
		bool bOn = false;
		if (SwitchOf(FM::ExtendedPermutationPath(EMantlePlaceExtendedVariant::Additive, EMantlePlaceAlphaMode::Additive, bTwoSided), bOn))
		{
			TestTrue(FString::Printf(TEXT("AdditiveRim is on in the %s Additive instance"), bTwoSided ? TEXT("two-sided") : TEXT("one-sided")), bOn);
		}
		bool bPlain = true;
		if (SwitchOf(FM::ExtendedPermutationPath(EMantlePlaceAlphaMode::Blend, bTwoSided), bPlain))
		{
			TestFalse(TEXT("and off in the plain extended Translucent instance"), bPlain);
		}
	}

	// The master has the three rim parameters, and its default rim is black.
	const UMaterialInterface* Additive = Cast<UMaterialInterface>(
		FM::ExtendedPermutationPath(EMantlePlaceExtendedVariant::Additive, EMantlePlaceAlphaMode::Additive, false).TryLoad());
	if (TestNotNull(TEXT("the Additive instance loads"), Additive))
	{
		FLinearColor Color(-1.0f, -1.0f, -1.0f, -1.0f);
		float Value = 0.0f;
		TestTrue(TEXT("it has RimColor"), Additive->GetVectorParameterValue(FHashedMaterialParameterInfo(P::RimColor), Color));
		TestTrue(TEXT("its default rim is black, so a material without a rim adds nothing"), Color.R == 0.0f && Color.G == 0.0f && Color.B == 0.0f);
		TestTrue(TEXT("it has RimStrength"), Additive->GetScalarParameterValue(FHashedMaterialParameterInfo(P::RimStrength), Value));
		TestTrue(TEXT("it has RimPower"), Additive->GetScalarParameterValue(FHashedMaterialParameterInfo(P::RimPower), Value));
	}
	return true;
}
#endif // WITH_EDITORONLY_DATA

#endif // WITH_DEV_AUTOMATION_TESTS
