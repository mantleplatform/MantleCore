// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGlbWriter.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantlePng.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionPanner.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// A scrolling texture times a constant tint or strength (a cloud card times its brightness, a glow times a color): the scroll keeps
// moving (uvPanning on the source texture) and the tint rides beside it as the base color's factor, or the glow's color and strength.
// Before, only a bare sample scrolled; a sample times anything froze at time 0. Graphs are built in code.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePannedTintTestPrivate
{
	template <typename TExpression>
	TExpression* AddNode(UMaterial* Material)
	{
		TExpression* Expression = NewObject<TExpression>(Material);
		Expression->Material = Material;
		Material->GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}

	/** A sample of Texture whose UVs are the first UV set panned at (SpeedX, SpeedY). */
	UMaterialExpressionTextureSample* AddPannedSample(UMaterial* Material, UTexture* Texture, const float SpeedX, const float SpeedY)
	{
		UMaterialExpressionPanner* Panner = AddNode<UMaterialExpressionPanner>(Material);
		Panner->SpeedX = SpeedX;
		Panner->SpeedY = SpeedY;
		UMaterialExpressionTextureSample* Sample = AddNode<UMaterialExpressionTextureSample>(Material);
		Sample->Texture = Texture;
		Sample->Coordinates.Connect(0, Panner);
		return Sample;
	}

	UMaterialExpressionMultiply* AddProduct(UMaterial* Material, UMaterialExpression* A, UMaterialExpression* B)
	{
		UMaterialExpressionMultiply* Product = AddNode<UMaterialExpressionMultiply>(Material);
		Product->A.Connect(0, A);
		Product->B.Connect(0, B);
		return Product;
	}

	UMaterialExpressionConstant3Vector* AddColor(UMaterial* Material, const float R, const float G, const float B)
	{
		UMaterialExpressionConstant3Vector* Color = AddNode<UMaterialExpressionConstant3Vector>(Material);
		Color->Constant = FLinearColor(R, G, B);
		return Color;
	}

	UMaterialExpressionScalarParameter* AddScalar(UMaterial* Material, const TCHAR* Name, const float Value)
	{
		UMaterialExpressionScalarParameter* Parameter = AddNode<UMaterialExpressionScalarParameter>(Material);
		Parameter->ParameterName = Name;
		Parameter->DefaultValue = Value;
		return Parameter;
	}

	/** What GLTFExporter writes for a material with no textures: factors of 1. */
	FMantleGlbDocument PlainBake()
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
		Material->SetObjectField(TEXT("pbrMetallicRoughness"), Pbr);
		FMantleGlbDocument Document;
		Document.Json = MakeShared<FJsonObject>();
		Document.Json->SetArrayField(TEXT("materials"), { MakeShared<FJsonValueObject>(Material) });
		return Document;
	}

	TArray<uint8> SmallPng()
	{
		TArray<FColor> Pixels;
		Pixels.Init(FColor(200, 200, 200, 255), 16);
		return FMantlePng::EncodeRGBA8(4, 4, Pixels);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePannedTintRecognizeTest,
	"Mantle.Editor.PannedTint.ASampleTimesATintStillScrolls",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePannedTintRecognizeTest::RunTest(const FString& Parameters)
{
	using namespace MantlePannedTintTestPrivate;
	UTexture2D* Texture = UTexture2D::CreateTransient(4, 4);

	// Base color: a panned sample times a color.
	UMaterial* Cloud = NewObject<UMaterial>(GetTransientPackage());
	Cloud->GetEditorOnlyData()->BaseColor.Connect(0, AddProduct(Cloud, AddPannedSample(Cloud, Texture, 0.1f, -0.2f), AddColor(Cloud, 0.5f, 0.25f, 1.0f)));
	const FMantleMaterialAnalysis CloudSeen = FMantlePlaceAssets::AnalyzeMaterial(Cloud);
	if (TestTrue(TEXT("a panned sample times a color is a panned source texture"), CloudSeen.Fidelity2.HasSource(0) && CloudSeen.Fidelity2.bPanning[0]))
	{
		TestTrue(TEXT("of its texture"), CloudSeen.Fidelity2.SourceTextures[0].Get() == Texture);
		TestEqual(TEXT("it pans along u"), CloudSeen.Fidelity2.Panning[0][0], 0.1, 1.0e-6);
		TestEqual(TEXT("and along v"), CloudSeen.Fidelity2.Panning[0][1], -0.2, 1.0e-6);
		TestTrue(TEXT("with a tint"), CloudSeen.Fidelity2.bSourceTint[0]);
		TestEqual(TEXT("red 0.5"), CloudSeen.Fidelity2.SourceTint[0][0], 0.5, 1.0e-6);
		TestEqual(TEXT("green 0.25"), CloudSeen.Fidelity2.SourceTint[0][1], 0.25, 1.0e-6);
		TestEqual(TEXT("blue 1"), CloudSeen.Fidelity2.SourceTint[0][2], 1.0, 1.0e-6);
	}
	TestFalse(TEXT("it is not a frozen animation"), CloudSeen.Has(EMantleMaterialLoss::Animation));

	// Emissive: (panned sample x a scalar parameter) x a color, so the two multiply: 5 x (1, 0.5, 0).
	UMaterial* Glow = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpressionMultiply* Bright = AddProduct(Glow, AddPannedSample(Glow, Texture, 0.0f, 0.5f), AddScalar(Glow, TEXT("Intensity"), 5.0f));
	Glow->GetEditorOnlyData()->EmissiveColor.Connect(0, AddProduct(Glow, Bright, AddColor(Glow, 1.0f, 0.5f, 0.0f)));
	const FMantleMaterialAnalysis GlowSeen = FMantlePlaceAssets::AnalyzeMaterial(Glow);
	if (TestTrue(TEXT("a nested product is a panned emissive source"), GlowSeen.Fidelity2.HasSource(3) && GlowSeen.Fidelity2.bPanning[3]))
	{
		TestEqual(TEXT("red 5"), GlowSeen.Fidelity2.SourceTint[3][0], 5.0, 1.0e-6);
		TestEqual(TEXT("green 2.5"), GlowSeen.Fidelity2.SourceTint[3][1], 2.5, 1.0e-6);
		TestEqual(TEXT("blue 0"), GlowSeen.Fidelity2.SourceTint[3][2], 0.0, 1.0e-6);
	}

	// An instance sets the parameter: Intensity 10 doubles it.
	UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(GetTransientPackage());
	Instance->SetParentEditorOnly(Glow, false);
	Instance->UpdateOverridableBaseProperties();
	Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Intensity")), 10.0f);
	const FMantleMaterialAnalysis Set = FMantlePlaceAssets::AnalyzeMaterial(Instance);
	if (TestTrue(TEXT("the instance's emissive is a panned source too"), Set.Fidelity2.HasSource(3)))
	{
		TestEqual(TEXT("with the instance's strength"), Set.Fidelity2.SourceTint[3][0], 10.0, 1.0e-6);
	}

	// A bare panned sample has no tint.
	UMaterial* Bare = NewObject<UMaterial>(GetTransientPackage());
	Bare->GetEditorOnlyData()->BaseColor.Connect(0, AddPannedSample(Bare, Texture, 0.1f, 0.0f));
	const FMantleMaterialAnalysis BareSeen = FMantlePlaceAssets::AnalyzeMaterial(Bare);
	TestTrue(TEXT("a bare panned sample is a source"), BareSeen.Fidelity2.HasSource(0));
	TestFalse(TEXT("with no tint"), BareSeen.Fidelity2.bSourceTint[0]);

	// A multiplier that changes with time is not a constant tint: nothing is carried and the animation is frozen, as it was.
	UMaterial* Pulse = NewObject<UMaterial>(GetTransientPackage());
	Pulse->GetEditorOnlyData()->EmissiveColor.Connect(0, AddProduct(Pulse, AddPannedSample(Pulse, Texture, 0.1f, 0.0f), AddNode<UMaterialExpressionTime>(Pulse)));
	const FMantleMaterialAnalysis PulseSeen = FMantlePlaceAssets::AnalyzeMaterial(Pulse);
	TestFalse(TEXT("a sample times Time is not carried"), PulseSeen.Fidelity2.HasSource(3));
	TestTrue(TEXT("it says the animation is frozen"), PulseSeen.Has(EMantleMaterialLoss::Animation));

	// A multiplier that is another texture is not a constant tint either.
	UMaterial* Masked = NewObject<UMaterial>(GetTransientPackage());
	UMaterialExpressionTextureSample* Mask = AddNode<UMaterialExpressionTextureSample>(Masked);
	Mask->Texture = Texture;
	Masked->GetEditorOnlyData()->BaseColor.Connect(0, AddProduct(Masked, AddPannedSample(Masked, Texture, 0.1f, 0.0f), Mask));
	TestFalse(TEXT("a sample times a texture is not carried"), FMantlePlaceAssets::AnalyzeMaterial(Masked).Fidelity2.HasSource(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePannedTintDocumentTest,
	"Mantle.Editor.PannedTint.TheTintRidesBesideTheSourceTexture",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePannedTintDocumentTest::RunTest(const FString& Parameters)
{
	using namespace MantlePannedTintTestPrivate;
	const TArray<uint8> Png = SmallPng();

	// A base color source with a tint of (0.5, 0.25, 1): the document's factor, over the source texture, with its pan.
	FMantleMaterialExtras Extras;
	Extras.SourceImages[0] = Png;
	Extras.Fidelity.bPanning[0] = true;
	Extras.Fidelity.Panning[0][0] = 0.1;
	Extras.Fidelity.bSourceTint[0] = true;
	Extras.Fidelity.SourceTint[0][0] = 0.5;
	Extras.Fidelity.SourceTint[0][1] = 0.25;
	Extras.Fidelity.SourceTint[0][2] = 1.0;
	FMantlePlaceAssetSet Set;
	const FMantlePlaceMaterialDocument Base = FMantlePlaceAssetConverter::MaterialFromGltf(PlainBake(), 0, TEXT("MI_Cloud"), FMantlePlaceAssetLimits(), false, Set, Extras);
	TestTrue(TEXT("the base color is the source texture"), Base.Textures.Contains(TEXT("baseColor")));
	TestEqual(TEXT("red factor"), Base.BaseColorFactor[0], 0.5, 1.0e-6);
	TestEqual(TEXT("green factor"), Base.BaseColorFactor[1], 0.25, 1.0e-6);
	TestEqual(TEXT("blue factor"), Base.BaseColorFactor[2], 1.0, 1.0e-6);
	TestEqual(TEXT("the opacity factor is untouched"), Base.BaseColorFactor[3], 1.0, 1.0e-6);

	// An emissive source with a tint of (5, 2.5, 0): strength 5, color (1, 0.5, 0), so the glow is 5 x the picture x (1, 0.5, 0).
	FMantleMaterialExtras GlowExtras;
	GlowExtras.SourceImages[3] = Png;
	GlowExtras.Fidelity.bPanning[3] = true;
	GlowExtras.Fidelity.Panning[3][1] = 0.5;
	GlowExtras.Fidelity.bSourceTint[3] = true;
	GlowExtras.Fidelity.SourceTint[3][0] = 5.0;
	GlowExtras.Fidelity.SourceTint[3][1] = 2.5;
	GlowExtras.Fidelity.SourceTint[3][2] = 0.0;
	FMantlePlaceAssetSet GlowSet;
	const FMantlePlaceMaterialDocument Glow = FMantlePlaceAssetConverter::MaterialFromGltf(PlainBake(), 0, TEXT("MI_Neon"), FMantlePlaceAssetLimits(), false, GlowSet, GlowExtras);
	TestTrue(TEXT("the glow is the source texture"), Glow.Textures.Contains(TEXT("emissive")));
	TestEqual(TEXT("strength 5"), Glow.EmissiveStrength, 5.0, 1.0e-6);
	TestEqual(TEXT("red 1"), Glow.EmissiveFactor[0], 1.0, 1.0e-6);
	TestEqual(TEXT("green 0.5"), Glow.EmissiveFactor[1], 0.5, 1.0e-6);
	TestEqual(TEXT("blue 0"), Glow.EmissiveFactor[2], 0.0, 1.0e-6);
	TestTrue(TEXT("and it still pans"), Glow.UvPanning.Contains(TEXT("emissive")));

	// No tint: the factors stay what the bake gave.
	FMantleMaterialExtras Plain;
	Plain.SourceImages[0] = Png;
	FMantlePlaceAssetSet PlainSet;
	const FMantlePlaceMaterialDocument Untinted = FMantlePlaceAssetConverter::MaterialFromGltf(PlainBake(), 0, TEXT("MI_Plain"), FMantlePlaceAssetLimits(), false, PlainSet, Plain);
	TestEqual(TEXT("an untinted source keeps factor 1"), Untinted.BaseColorFactor[0], 1.0, 1.0e-6);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
