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

#endif // WITH_DEV_AUTOMATION_TESTS
