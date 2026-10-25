// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGlbWriter.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceValidator.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// #628: a translucent glass material's index of refraction (glTF KHR_materials_ior) is drawn as the glass look a place
// material has (opacity), instead of being dropped with a warning.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleGlassIorTestPrivate
{
	/** What GLTFExporter writes for a translucent material: Alpha, base colour (alpha Opacity), roughness and, when Ior is above 0, its KHR_materials_ior. */
	FMantleGlbDocument GlassBake(const TCHAR* Alpha, const double Opacity, const double Roughness, const double Ior)
	{
		const TSharedRef<FJsonObject> Pbr = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Colour;
		for (const double Value : { 0.2, 0.4, 0.6, Opacity })
		{
			Colour.Add(MakeShared<FJsonValueNumber>(Value));
		}
		Pbr->SetArrayField(TEXT("baseColorFactor"), Colour);
		Pbr->SetNumberField(TEXT("metallicFactor"), 0.0);
		Pbr->SetNumberField(TEXT("roughnessFactor"), Roughness);
		const TSharedRef<FJsonObject> Material = MakeShared<FJsonObject>();
		Material->SetStringField(TEXT("alphaMode"), Alpha);
		Material->SetObjectField(TEXT("pbrMetallicRoughness"), Pbr);
		if (Ior > 0.0)
		{
			const TSharedRef<FJsonObject> IorObject = MakeShared<FJsonObject>();
			IorObject->SetNumberField(TEXT("ior"), Ior);
			const TSharedRef<FJsonObject> Extensions = MakeShared<FJsonObject>();
			Extensions->SetObjectField(TEXT("KHR_materials_ior"), IorObject);
			Material->SetObjectField(TEXT("extensions"), Extensions);
		}
		TArray<TSharedPtr<FJsonValue>> Materials;
		Materials.Add(MakeShared<FJsonValueObject>(Material));
		FMantleGlbDocument Document;
		Document.Json = MakeShared<FJsonObject>();
		Document.Json->SetArrayField(TEXT("materials"), Materials);
		return Document;
	}

	FMantlePlaceMaterialDocument Convert(const FMantleGlbDocument& Bake, FMantlePlaceAssetSet& Set)
	{
		return FMantlePlaceAssetConverter::MaterialFromGltf(Bake, 0, TEXT("MI_Glass"), FMantlePlaceAssetLimits(), false, Set, FMantleMaterialExtras());
	}

	bool HasIorWarning(const FMantlePlaceAssetSet& Set)
	{
		for (const FMantleValidationIssue& Issue : Set.Issues)
		{
			if (Issue.Problem.Contains(TEXT("ior")))
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGlassIorOpacityTest,
	"Mantle.Editor.Glass.IorShapesTheOpacity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGlassIorOpacityTest::RunTest(const FString& Parameters)
{
	// An index of 1.5 reflects as the default surface does: only the floor applies.
	TestEqual(TEXT("1.5 leaves a solid enough glass alone"), FMantlePlaceAssetConverter::GlassOpacityFromIor(1.5, 0.3), 0.3, 1.0e-9);
	TestEqual(TEXT("1.5 raises a too-thin glass to the floor"), FMantlePlaceAssetConverter::GlassOpacityFromIor(1.5, 0.02), FMantlePlaceAssetConverter::MinGlassOpacity, 1.0e-9);

	// F0 of 2.0 is (1/3)^2; the part above the default 0.04 reflects, so it is added to the opacity.
	TestEqual(TEXT("a denser glass reads more opaque by its extra reflectance"), FMantlePlaceAssetConverter::GlassOpacityFromIor(2.0, 0.3), 0.3 + (1.0 / 9.0 - 0.04), 1.0e-9);
	TestTrue(TEXT("and a less dense one less"), FMantlePlaceAssetConverter::GlassOpacityFromIor(1.0, 0.3) < 0.3);
	TestEqual(TEXT("opacity stays within 0..1"), FMantlePlaceAssetConverter::GlassOpacityFromIor(3.0, 0.99), 1.0, 1.0e-9);
	TestEqual(TEXT("an index below 1 is read as 1"), FMantlePlaceAssetConverter::GlassOpacityFromIor(0.2, 0.3), FMantlePlaceAssetConverter::GlassOpacityFromIor(1.0, 0.3), 1.0e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGlassIorDocumentTest,
	"Mantle.Editor.Glass.IorPublishesAsGlassBlend",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGlassIorDocumentTest::RunTest(const FString& Parameters)
{
	using namespace MantleGlassIorTestPrivate;

	FMantlePlaceAssetSet Set;
	const FMantlePlaceMaterialDocument Doc = Convert(GlassBake(TEXT("BLEND"), 0.3, 0.15, 2.0), Set);
	TestEqual(TEXT("it publishes translucent"), Doc.AlphaMode, FString(TEXT("blend")));
	TestEqual(TEXT("with the opacity the index implies"), Doc.BaseColorFactor[3], FMantlePlaceAssetConverter::GlassOpacityFromIor(2.0, 0.3), 1.0e-9);
	TestEqual(TEXT("the tint is kept"), Doc.BaseColorFactor[2], 0.6, 1.0e-9);
	TestEqual(TEXT("and the roughness"), Doc.RoughnessFactor, 0.15, 1.0e-9);
	TestFalse(TEXT("the index is not reported as dropped"), HasIorWarning(Set));

	// What is written is the same, read back as JSON.
	FString Json;
	FString Error;
	if (TestTrue(TEXT("it writes"), Doc.ToJson(TMap<FString, FString>(), Json, Error)))
	{
		TSharedPtr<FJsonObject> Parsed;
		if (TestTrue(TEXT("as JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Parsed) && Parsed.IsValid()))
		{
			const TArray<TSharedPtr<FJsonValue>>* Colour = nullptr;
			if (TestTrue(TEXT("with a base colour factor"), Parsed->TryGetArrayField(TEXT("baseColorFactor"), Colour) && Colour->Num() == 4))
			{
				TestEqual(TEXT("whose alpha is the glass opacity"), (*Colour)[3]->AsNumber(), FMantlePlaceAssetConverter::GlassOpacityFromIor(2.0, 0.3), 1.0e-3);
			}
		}
	}

	// An opaque material with an index (a gem-like solid) keeps its opacity and the warning: the glass look is for translucent only.
	FMantlePlaceAssetSet OpaqueSet;
	const FMantlePlaceMaterialDocument Opaque = Convert(GlassBake(TEXT("OPAQUE"), 1.0, 0.5, 2.0), OpaqueSet);
	TestEqual(TEXT("an opaque material keeps its opacity"), Opaque.BaseColorFactor[3], 1.0, 1.0e-9);
	TestTrue(TEXT("and still says its index is dropped"), HasIorWarning(OpaqueSet));

	// A translucent material without an index is left as it was.
	FMantlePlaceAssetSet PlainSet;
	TestEqual(TEXT("a translucent material without an index is unchanged"), Convert(GlassBake(TEXT("BLEND"), 0.3, 0.5, 0.0), PlainSet).BaseColorFactor[3], 0.3, 1.0e-9);
	return true;
}

#endif
