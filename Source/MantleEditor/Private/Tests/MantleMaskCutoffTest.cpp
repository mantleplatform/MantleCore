// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGlbWriter.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantlePng.h"
#include "MantlePlaceValidator.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// A masked material never publishes an alphaCutoff of 0 (it keeps every pixel, so a leaf card shows as a solid rectangle: Medieval's
// pines), and a masked bake keeps its opacity in the base color image's alpha.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleMaskCutoffTestPrivate
{
	/** What GLTFExporter writes for a material: its alphaMode, its alphaCutoff when Cutoff is not negative, and a base color texture when a PNG is given. */
	FMantleGlbDocument Bake(const TCHAR* Alpha, const double Cutoff, const TArray<uint8>& BaseColorPng = TArray<uint8>())
	{
		const TSharedRef<FJsonObject> Pbr = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Color;
		for (const double Value : { 1.0, 1.0, 1.0, 1.0 })
		{
			Color.Add(MakeShared<FJsonValueNumber>(Value));
		}
		Pbr->SetArrayField(TEXT("baseColorFactor"), Color);
		Pbr->SetNumberField(TEXT("metallicFactor"), 0.0);
		Pbr->SetNumberField(TEXT("roughnessFactor"), 1.0);
		const TSharedRef<FJsonObject> Material = MakeShared<FJsonObject>();
		Material->SetStringField(TEXT("alphaMode"), Alpha);
		if (Cutoff >= 0.0)
		{
			Material->SetNumberField(TEXT("alphaCutoff"), Cutoff);
		}
		Material->SetBoolField(TEXT("doubleSided"), true);
		FMantleGlbDocument Document;
		Document.Json = MakeShared<FJsonObject>();
		if (BaseColorPng.Num() > 0)
		{
			const TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
			Info->SetNumberField(TEXT("index"), 0);
			Pbr->SetObjectField(TEXT("baseColorTexture"), Info);
			const TSharedRef<FJsonObject> Texture = MakeShared<FJsonObject>();
			Texture->SetNumberField(TEXT("source"), 0);
			const TSharedRef<FJsonObject> Image = MakeShared<FJsonObject>();
			Image->SetNumberField(TEXT("bufferView"), 0);
			Image->SetStringField(TEXT("mimeType"), TEXT("image/png"));
			const TSharedRef<FJsonObject> View = MakeShared<FJsonObject>();
			View->SetNumberField(TEXT("buffer"), 0);
			View->SetNumberField(TEXT("byteOffset"), 0);
			View->SetNumberField(TEXT("byteLength"), BaseColorPng.Num());
			const TSharedRef<FJsonObject> Buffer = MakeShared<FJsonObject>();
			Buffer->SetNumberField(TEXT("byteLength"), BaseColorPng.Num());
			Document.Json->SetArrayField(TEXT("textures"), { MakeShared<FJsonValueObject>(Texture) });
			Document.Json->SetArrayField(TEXT("images"), { MakeShared<FJsonValueObject>(Image) });
			Document.Json->SetArrayField(TEXT("bufferViews"), { MakeShared<FJsonValueObject>(View) });
			Document.Json->SetArrayField(TEXT("buffers"), { MakeShared<FJsonValueObject>(Buffer) });
			Document.Bin = BaseColorPng;
		}
		Material->SetObjectField(TEXT("pbrMetallicRoughness"), Pbr);
		Document.Json->SetArrayField(TEXT("materials"), { MakeShared<FJsonValueObject>(Material) });
		return Document;
	}

	FMantlePlaceMaterialDocument Convert(const FMantleGlbDocument& Document, FMantlePlaceAssetSet& Set, const double SourceThreshold = 0.0)
	{
		FMantleMaterialExtras Extras;
		Extras.SourceAlphaThreshold = SourceThreshold;
		return FMantlePlaceAssetConverter::MaterialFromGltf(Document, 0, TEXT("MI_Leaf"), FMantlePlaceAssetLimits(), false, Set, Extras);
	}

	bool SaysCutoffGuessed(const FMantlePlaceAssetSet& Set)
	{
		for (const FMantleValidationIssue& Issue : Set.Issues)
		{
			if (Issue.Problem.Contains(TEXT("cut-out threshold")))
			{
				return true;
			}
		}
		return false;
	}

	/** A 4x4 leaf card picture: the left half see-through (alpha 0), the right half leaf (alpha 255). */
	TArray<uint8> LeafPng()
	{
		TArray<FColor> Pixels;
		Pixels.Init(FColor(40, 90, 30, 255), 16);
		for (int32 Y = 0; Y < 4; ++Y)
		{
			for (int32 X = 0; X < 2; ++X)
			{
				Pixels[Y * 4 + X].A = 0;
			}
		}
		return FMantlePng::EncodeRGBA8(4, 4, Pixels);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMaskCutoffResolveTest,
	"Mantle.Editor.MaskCutoff.ResolveMaskCutoff",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMaskCutoffResolveTest::RunTest(const FString& Parameters)
{
	bool bGuessed = true;
	TestEqual(TEXT("a real cut-off is kept"), FMantlePlaceAssetConverter::ResolveMaskCutoff(0.25, 0.0, bGuessed), 0.25, 1.0e-9);
	TestFalse(TEXT("and nothing is guessed"), bGuessed);

	TestEqual(TEXT("a real cut-off wins over the graph's threshold"), FMantlePlaceAssetConverter::ResolveMaskCutoff(0.6, 0.4, bGuessed), 0.6, 1.0e-9);
	TestFalse(TEXT("also unguessed"), bGuessed);

	TestEqual(TEXT("a cut-off of 0 takes the graph's threshold"), FMantlePlaceAssetConverter::ResolveMaskCutoff(0.0, 0.4, bGuessed), 0.4, 1.0e-9);
	TestFalse(TEXT("which is read, not guessed"), bGuessed);

	TestEqual(TEXT("a cut-off of 0 with nothing else is the foliage default"), FMantlePlaceAssetConverter::ResolveMaskCutoff(0.0, 0.0, bGuessed), 0.3333, 1.0e-9);
	TestTrue(TEXT("and that is a guess"), bGuessed);

	TestEqual(TEXT("a threshold of 1 or more is not a cut-off"), FMantlePlaceAssetConverter::ResolveMaskCutoff(0.0, 1.0, bGuessed), 0.3333, 1.0e-9);
	TestTrue(TEXT("a guess too"), bGuessed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMaskCutoffDocumentTest,
	"Mantle.Editor.MaskCutoff.MaskedMaterialsNeverPublishZero",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMaskCutoffDocumentTest::RunTest(const FString& Parameters)
{
	using namespace MantleMaskCutoffTestPrivate;

	// What Medieval's far pines gave: masked, two-sided, alphaCutoff 0.
	FMantlePlaceAssetSet ZeroSet;
	const FMantlePlaceMaterialDocument Zero = Convert(Bake(TEXT("MASK"), 0.0), ZeroSet);
	TestEqual(TEXT("a masked material stays masked"), Zero.AlphaMode, FString(TEXT("mask")));
	TestEqual(TEXT("a cut-off of 0 publishes 0.3333"), Zero.AlphaCutoff, 0.3333, 1.0e-9);
	TestTrue(TEXT("and Check says the threshold was guessed"), SaysCutoffGuessed(ZeroSet));

	// The graph's own threshold, when the clip value is 0.
	FMantlePlaceAssetSet ThresholdSet;
	TestEqual(TEXT("the graph's Alpha Threshold is used"), Convert(Bake(TEXT("MASK"), 0.0), ThresholdSet, 0.45).AlphaCutoff, 0.45, 1.0e-9);
	TestFalse(TEXT("and nothing is said"), SaysCutoffGuessed(ThresholdSet));

	// A real cut-off is kept, whatever the graph's threshold says.
	FMantlePlaceAssetSet RealSet;
	TestEqual(TEXT("a real cut-off is kept"), Convert(Bake(TEXT("MASK"), 0.3333), RealSet, 0.45).AlphaCutoff, 0.3333, 1.0e-9);
	TestFalse(TEXT("with nothing said"), SaysCutoffGuessed(RealSet));

	// glTF leaves out an alphaCutoff of 0.5: the default stays 0.5.
	FMantlePlaceAssetSet DefaultSet;
	TestEqual(TEXT("an absent cut-off is glTF's 0.5"), Convert(Bake(TEXT("MASK"), -1.0), DefaultSet).AlphaCutoff, 0.5, 1.0e-9);
	TestFalse(TEXT("and nothing is said"), SaysCutoffGuessed(DefaultSet));

	// An opaque material has no cut-off to fix and nothing to say.
	FMantlePlaceAssetSet OpaqueSet;
	const FMantlePlaceMaterialDocument Opaque = Convert(Bake(TEXT("OPAQUE"), 0.0), OpaqueSet);
	TestEqual(TEXT("an opaque material stays opaque"), Opaque.AlphaMode, FString(TEXT("opaque")));
	TestFalse(TEXT("and says nothing of a threshold"), SaysCutoffGuessed(OpaqueSet));

	// What is written for a masked material carries the fixed cut-off.
	FString Json;
	FString Error;
	if (TestTrue(TEXT("a masked document writes"), Zero.ToJson(TMap<FString, FString>(), Json, Error)))
	{
		TestTrue(TEXT("with the 0.3333 cut-off"), Json.Contains(TEXT("\"alphaCutoff\":0.3333")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMaskBakeAlphaTest,
	"Mantle.Editor.MaskCutoff.MaskedBakeKeepsItsAlpha",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMaskBakeAlphaTest::RunTest(const FString& Parameters)
{
	using namespace MantleMaskCutoffTestPrivate;

	// A masked bake whose base color image carries the cut-out in its alpha: published as a PNG (alpha kept), masked.
	FMantlePlaceAssetSet MaskedSet;
	const FMantlePlaceMaterialDocument Masked = Convert(Bake(TEXT("MASK"), 0.0, LeafPng()), MaskedSet);
	TestEqual(TEXT("masked"), Masked.AlphaMode, FString(TEXT("mask")));
	TestEqual(TEXT("with a real cut-off"), Masked.AlphaCutoff, 0.3333, 1.0e-9);
	const FString* BaseColorId = Masked.Textures.Find(TEXT("baseColor"));
	if (TestNotNull(TEXT("the base color is published"), BaseColorId))
	{
		const FMantleConvertedTexture* Texture = MaskedSet.Textures.FindByPredicate([BaseColorId](const FMantleConvertedTexture& Candidate) { return Candidate.Key == *BaseColorId; });
		if (TestNotNull(TEXT("and is in the set"), Texture))
		{
			TestEqual(TEXT("as a PNG, so its alpha is kept"), Texture->Format, FString(TEXT("png")));
		}
	}

	// The same picture on an opaque material is published without alpha (JPEG): unchanged.
	FMantlePlaceAssetSet OpaqueSet;
	const FMantlePlaceMaterialDocument Opaque = Convert(Bake(TEXT("OPAQUE"), -1.0, LeafPng()), OpaqueSet);
	TestEqual(TEXT("opaque"), Opaque.AlphaMode, FString(TEXT("opaque")));
	const FString* OpaqueId = Opaque.Textures.Find(TEXT("baseColor"));
	if (TestNotNull(TEXT("its base color is published"), OpaqueId))
	{
		const FMantleConvertedTexture* Texture = OpaqueSet.Textures.FindByPredicate([OpaqueId](const FMantleConvertedTexture& Candidate) { return Candidate.Key == *OpaqueId; });
		if (TestNotNull(TEXT("and is in the set"), Texture))
		{
			TestEqual(TEXT("as a JPEG"), Texture->Format, FString(TEXT("jpeg")));
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
