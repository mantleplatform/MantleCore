// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGlbWriter.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// A1 (publish test level): under Substrate's adaptive GBuffer, the engine's default (r.Substrate.ProjectGBufferFormat 1), the engine's bake
// exports a translucent material's Opacity flat. A plain translucent card with a left-to-right opacity gradient baked as one solid color,
// where under the blendable GBuffer (City Sample's format 0) it kept the gradient, and a Masked material kept its Opacity Mask under both.
// So a translucent material whose opacity varies is baked through a Masked copy (its Opacity Mask fed by its Opacity) and still published
// see-through. The renderer state is passed in here; the real bake is proven with the test level's pictures.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleOpacityAsMaskTestPrivate
{
	template <typename TExpression>
	TExpression* AddNode(UMaterial* Material)
	{
		TExpression* Expression = NewObject<TExpression>(Material);
		Expression->Material = Material;
		Material->GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}

	/** A translucent card whose Opacity is a left-to-right gradient (Texture Coordinate's U), lit or unlit. */
	UMaterial* NewGradientCard(bool bUnlit = false)
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		Material->BlendMode = BLEND_Translucent;
		if (bUnlit)
		{
			Material->SetShadingModel(MSM_Unlit);
		}
		UMaterialExpressionTextureCoordinate* Uv = AddNode<UMaterialExpressionTextureCoordinate>(Material);
		UMaterialExpressionComponentMask* U = AddNode<UMaterialExpressionComponentMask>(Material);
		U->R = true;
		U->G = false;
		U->B = false;
		U->A = false;
		U->Input.Connect(0, Uv);
		Material->GetEditorOnlyData()->Opacity.Connect(0, U);
		Material->UpdateCachedExpressionData();
		return Material;
	}

	/** A translucent sheet whose Opacity is one number: a constant, or a scalar parameter. */
	UMaterial* NewFlatSheet(bool bParameter)
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		Material->BlendMode = BLEND_Translucent;
		if (bParameter)
		{
			UMaterialExpressionScalarParameter* Fade = AddNode<UMaterialExpressionScalarParameter>(Material);
			Fade->ParameterName = TEXT("Fade");
			Fade->DefaultValue = 0.4f;
			Material->GetEditorOnlyData()->Opacity.Connect(0, Fade);
		}
		else
		{
			UMaterialExpressionConstant* Half = AddNode<UMaterialExpressionConstant>(Material);
			Half->R = 0.5f;
			Material->GetEditorOnlyData()->Opacity.Connect(0, Half);
		}
		Material->UpdateCachedExpressionData();
		return Material;
	}

	bool Bakes(UMaterial* Material, bool bAdaptiveGBuffer)
	{
		return FMantlePlaceAssets::BakesOpacityAsMask(Material, FMantlePlaceAssets::AnalyzeMaterial(Material), bAdaptiveGBuffer);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleOpacityAsMaskTriggerTest,
	"Mantle.Editor.OpacityAsMask.OnlyAVaryingTranslucentOpacityUnderTheAdaptiveGBuffer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleOpacityAsMaskTriggerTest::RunTest(const FString& Parameters)
{
	using namespace MantleOpacityAsMaskTestPrivate;
	TestTrue(TEXT("a gradient opacity, adaptive GBuffer: through a Masked copy"), Bakes(NewGradientCard(), true));
	TestTrue(TEXT("an unlit one too"), Bakes(NewGradientCard(/*bUnlit*/ true), true));
	TestFalse(TEXT("under the blendable GBuffer (City Sample's format 0) the bake is as it was"), Bakes(NewGradientCard(), false));
	TestFalse(TEXT("a constant opacity is as it was"), Bakes(NewFlatSheet(false), true));
	TestFalse(TEXT("a scalar-parameter opacity is as it was"), Bakes(NewFlatSheet(true), true));

	UMaterial* Masked = NewGradientCard();
	Masked->BlendMode = BLEND_Masked;
	TestFalse(TEXT("a Masked material keeps its own bake"), Bakes(Masked, true));
	UMaterial* Opaque = NewGradientCard();
	Opaque->BlendMode = BLEND_Opaque;
	TestFalse(TEXT("an opaque one too"), Bakes(Opaque, true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleOpacityAsMaskCopyTest,
	"Mantle.Editor.OpacityAsMask.CopyIsMaskedWithItsOpacityAsTheMask",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleOpacityAsMaskCopyTest::RunTest(const FString& Parameters)
{
	using namespace MantleOpacityAsMaskTestPrivate;
	UMaterial* Card = NewGradientCard();
	const TStrongObjectPtr<UMaterialInterface> Copy = FMantlePlaceAssets::OpacityAsMaskCopy(Card);
	if (!TestTrue(TEXT("it gets a copy"), Copy.IsValid()) || !TestNotNull(TEXT("with a base"), Copy->GetMaterial()))
	{
		return false;
	}
	const UMaterialEditorOnlyData* Data = Copy->GetMaterial()->GetEditorOnlyData();
	TestEqual(TEXT("the copy is Masked"), static_cast<int32>(Copy->GetBlendMode()), static_cast<int32>(BLEND_Masked));
	TestNotNull(TEXT("its Opacity Mask is fed"), Data->OpacityMask.Expression);
	TestTrue(TEXT("by what feeds its Opacity (the gradient)"), Data->OpacityMask.Expression == Data->Opacity.Expression
		&& Cast<UMaterialExpressionComponentMask>(Data->OpacityMask.Expression) != nullptr);
	TestEqual(TEXT("the creator's material is still Translucent"), static_cast<int32>(Card->BlendMode), static_cast<int32>(BLEND_Translucent));
	TestNull(TEXT("and its Opacity Mask untouched"), Card->GetEditorOnlyData()->OpacityMask.Expression);

	// Build 4: City Sample's decal master carries "masked can be assumed opaque", so its Masked copy reported Opaque and baked no alpha.
	UMaterial* Flagged = NewGradientCard();
	Flagged->bCanMaskedBeAssumedOpaque = true;
	const TStrongObjectPtr<UMaterialInterface> FlaggedCopy = FMantlePlaceAssets::OpacityAsMaskCopy(Flagged);
	if (TestTrue(TEXT("a material carrying the flag gets a copy too"), FlaggedCopy.IsValid()))
	{
		TestEqual(TEXT("which reports Masked, not Opaque"), static_cast<int32>(FlaggedCopy->GetBlendMode()), static_cast<int32>(BLEND_Masked));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleOpacityAsMaskBlendedTest,
	"Mantle.Editor.OpacityAsMask.TheMaskedBakePublishesSeeThrough",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleOpacityAsMaskBlendedTest::RunTest(const FString& Parameters)
{
	// What the Masked copy's bake says: MASK with a cut-off. It publishes as the material is: BLEND, no cut-off, its base color alpha the opacity.
	FMantleGlbDocument Doc;
	Doc.Json = MakeShared<FJsonObject>();
	const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("alphaMode"), TEXT("MASK"));
	Entry->SetNumberField(TEXT("alphaCutoff"), 0.3333);
	TArray<TSharedPtr<FJsonValue>> Materials;
	Materials.Add(MakeShared<FJsonValueObject>(Entry));
	Doc.Json->SetArrayField(TEXT("materials"), Materials);

	FMantlePlaceAssetConverter::SetBakeBlended(Doc);
	FString Mode;
	TestTrue(TEXT("alphaMode is set"), Entry->TryGetStringField(TEXT("alphaMode"), Mode));
	TestEqual(TEXT("to BLEND"), Mode, FString(TEXT("BLEND")));
	TestFalse(TEXT("with no cut-off"), Entry->HasField(TEXT("alphaCutoff")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDecalMaskCutoffTest,
	"Mantle.Editor.OpacityAsMask.MeshDecalCutOffKeepsItsCoverage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDecalMaskCutoffTest::RunTest(const FString& Parameters)
{
	// A1 (Joshua: cut-outs): a mesh decal publishes as a cut-out, cut where it covers as much as the see-through decal did.
	auto Alphas = [](std::initializer_list<uint8> Values)
	{
		TArray<FColor> Pixels;
		for (const uint8 Value : Values)
		{
			Pixels.Add(FColor(200, 190, 170, Value));
		}
		return Pixels;
	};
	auto Kept = [](const TArray<FColor>& Pixels, double Cutoff)
	{
		int32 Count = 0;
		for (const FColor& Pixel : Pixels)
		{
			Count += Pixel.A / 255.0 >= Cutoff ? 1 : 0;
		}
		return Count;
	};
	using FConverter = FMantlePlaceAssetConverter;
	// One alpha everywhere (a faint stain over its whole card): cut at half that alpha, so all of it shows, at full strength.
	const TArray<FColor> Even = Alphas({ 77, 77, 77, 77 });
	TestEqual(TEXT("one alpha everywhere: cut at half of it"), FConverter::CoverageCutoff(Even), 38.5 / 255.0, 1.0e-9);
	TestEqual(TEXT("so none of it is cut away"), Kept(Even, FConverter::CoverageCutoff(Even)), 4);
	TestEqual(TEXT("every alpha 0: the middle (nothing shows either way)"), FConverter::CoverageCutoff(Alphas({ 0, 0, 0, 0 })), 0.5, 1.0e-9);
	TestEqual(TEXT("a hard-edged decal (0 and 255) is cut midway, not under 255"), FConverter::CoverageCutoff(Alphas({ 0, 0, 255, 255 })), 127.5 / 255.0, 1.0e-9);

	// A soft-edged line: a quarter solid, a quarter edge at 0.4, half bare. Mean alpha 0.35: keeping the solid quarter (0.25) is closest,
	// so the edge is cut and the solid part kept.
	const TArray<FColor> Soft = Alphas({ 255, 102, 0, 0 });
	const double SoftCut = FConverter::CoverageCutoff(Soft);
	TestEqual(TEXT("a soft edge keeps the solid part"), Kept(Soft, SoftCut), 1);

	// A faint decal (grime at 0.3 over half its card): cutting it to its 0.15 coverage would drop all of it, so it is kept whole.
	const TArray<FColor> Faint = Alphas({ 77, 77, 0, 0 });
	const double FaintCut = FConverter::CoverageCutoff(Faint);
	TestEqual(TEXT("a faint decal is never cut away"), Kept(Faint, FaintCut), 2);
	TestTrue(TEXT("and the bare part stays bare"), FaintCut > 0.0);

	// The bake's alphaMode: MASK with that cut-off.
	FMantleGlbDocument Doc;
	Doc.Json = MakeShared<FJsonObject>();
	const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("alphaMode"), TEXT("BLEND"));
	TArray<TSharedPtr<FJsonValue>> Materials;
	Materials.Add(MakeShared<FJsonValueObject>(Entry));
	Doc.Json->SetArrayField(TEXT("materials"), Materials);
	FConverter::SetBakeMasked(Doc, 0.742);
	FString Mode;
	TestTrue(TEXT("alphaMode is set"), Entry->TryGetStringField(TEXT("alphaMode"), Mode));
	TestEqual(TEXT("to MASK"), Mode, FString(TEXT("MASK")));
	double Cutoff = 0.0;
	TestTrue(TEXT("with a cut-off"), Entry->TryGetNumberField(TEXT("alphaCutoff"), Cutoff));
	TestEqual(TEXT("the one given"), Cutoff, 0.742, 1.0e-9);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
