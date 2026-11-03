// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssets.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// A material set to Translucent whose opacity is always 1 shows nothing through it, so it publishes as a solid surface (FMantleMaterialAnalysis::bOpaqueInDisguise,
// ConvertMaterial). Real glass (an opacity that is not 1), a light, a glow, an additive card and an opaque material are left as they are.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleOpaqueTranslucentTestPrivate
{
	template <typename TExpression>
	TExpression* AddNode(UMaterial* Material)
	{
		TExpression* Expression = NewObject<TExpression>(Material);
		Expression->Material = Material;
		Material->GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}

	UMaterialExpressionConstant* AddNumber(UMaterial* Material, const float Value)
	{
		UMaterialExpressionConstant* Constant = AddNode<UMaterialExpressionConstant>(Material);
		Constant->R = Value;
		return Constant;
	}

	UMaterialExpressionScalarParameter* AddNumberParameter(UMaterial* Material, const TCHAR* Name, const float Value)
	{
		UMaterialExpressionScalarParameter* Parameter = AddNode<UMaterialExpressionScalarParameter>(Material);
		Parameter->ParameterName = Name;
		Parameter->DefaultValue = Value;
		return Parameter;
	}

	/** A lit, two-sided material of the blend mode given, its opacity not connected. */
	UMaterial* NewMaterial(const EBlendMode Blend)
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		Material->BlendMode = Blend;
		Material->TwoSided = true;
		return Material;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleOpaqueTranslucentTest,
	"Mantle.Material.OpaqueInDisguise.OnlyASolidTranslucentIsFlagged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleOpaqueTranslucentTest::RunTest(const FString&)
{
	using namespace MantleOpaqueTranslucentTestPrivate;

	// Translucent with nothing in Opacity: solid.
	UMaterial* Unconnected = NewMaterial(BLEND_Translucent);
	TestTrue(TEXT("a translucent material with no opacity connected is solid"), FMantlePlaceAssets::AnalyzeMaterial(Unconnected).bOpaqueInDisguise);

	// An opacity of 1, as a constant and as a parameter.
	UMaterial* ConstantOne = NewMaterial(BLEND_Translucent);
	ConstantOne->GetEditorOnlyData()->Opacity.Connect(0, AddNumber(ConstantOne, 1.0f));
	TestTrue(TEXT("an opacity constant of 1 is solid"), FMantlePlaceAssets::AnalyzeMaterial(ConstantOne).bOpaqueInDisguise);
	UMaterial* ParameterOne = NewMaterial(BLEND_Translucent);
	ParameterOne->GetEditorOnlyData()->Opacity.Connect(0, AddNumberParameter(ParameterOne, TEXT("Opacity"), 1.0f));
	TestTrue(TEXT("an opacity parameter of 1 is solid"), FMantlePlaceAssets::AnalyzeMaterial(ParameterOne).bOpaqueInDisguise);

	// A product of ones is one; a product with a half is glass.
	UMaterial* Product = NewMaterial(BLEND_Translucent);
	UMaterialExpressionMultiply* Ones = AddNode<UMaterialExpressionMultiply>(Product);
	Ones->A.Connect(0, AddNumberParameter(Product, TEXT("Opacity"), 1.0f));
	Ones->B.Connect(0, AddNumber(Product, 1.0f));
	Product->GetEditorOnlyData()->Opacity.Connect(0, Ones);
	TestTrue(TEXT("one times one is solid"), FMantlePlaceAssets::AnalyzeMaterial(Product).bOpaqueInDisguise);
	UMaterial* Half = NewMaterial(BLEND_Translucent);
	UMaterialExpressionMultiply* Mixed = AddNode<UMaterialExpressionMultiply>(Half);
	Mixed->A.Connect(0, AddNumberParameter(Half, TEXT("Opacity"), 1.0f));
	Mixed->B.Connect(0, AddNumber(Half, 0.5f));
	Half->GetEditorOnlyData()->Opacity.Connect(0, Mixed);
	TestFalse(TEXT("one times a half is glass"), FMantlePlaceAssets::AnalyzeMaterial(Half).bOpaqueInDisguise);

	// Glass: an opacity under 1.
	UMaterial* Glass = NewMaterial(BLEND_Translucent);
	Glass->GetEditorOnlyData()->Opacity.Connect(0, AddNumber(Glass, 0.4f));
	TestFalse(TEXT("an opacity of 0.4 is glass"), FMantlePlaceAssets::AnalyzeMaterial(Glass).bOpaqueInDisguise);
	UMaterial* GlassParameter = NewMaterial(BLEND_Translucent);
	GlassParameter->GetEditorOnlyData()->Opacity.Connect(0, AddNumberParameter(GlassParameter, TEXT("Opacity"), 0.25f));
	TestFalse(TEXT("an opacity parameter of 0.25 is glass"), FMantlePlaceAssets::AnalyzeMaterial(GlassParameter).bOpaqueInDisguise);

	// An instance's own value counts: a parameter the instance sets low is glass, one it leaves alone is read from the master.
	UMaterial* Base = NewMaterial(BLEND_Translucent);
	Base->GetEditorOnlyData()->Opacity.Connect(0, AddNumberParameter(Base, TEXT("Opacity"), 1.0f));
	UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(GetTransientPackage());
	Instance->SetParentEditorOnly(Base);
	Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Opacity")), 0.3f);
	Instance->PostEditChange();
	TestFalse(TEXT("an instance that sets the opacity to 0.3 is glass"), FMantlePlaceAssets::AnalyzeMaterial(Instance).bOpaqueInDisguise);
	UMaterialInstanceConstant* Plain = NewObject<UMaterialInstanceConstant>(GetTransientPackage());
	Plain->SetParentEditorOnly(Base);
	Plain->PostEditChange();
	TestTrue(TEXT("an instance that leaves the opacity at 1 is solid"), FMantlePlaceAssets::AnalyzeMaterial(Plain).bOpaqueInDisguise);

	// Not translucent, or not lit: left alone.
	TestFalse(TEXT("an opaque material is not flagged"), FMantlePlaceAssets::AnalyzeMaterial(NewMaterial(BLEND_Opaque)).bOpaqueInDisguise);
	TestFalse(TEXT("an additive material is not flagged"), FMantlePlaceAssets::AnalyzeMaterial(NewMaterial(BLEND_Additive)).bOpaqueInDisguise);
	UMaterial* Unlit = NewMaterial(BLEND_Translucent);
	Unlit->SetShadingModel(MSM_Unlit);
	TestFalse(TEXT("an unlit translucent material (a glow or a card) is not flagged"), FMantlePlaceAssets::AnalyzeMaterial(Unlit).bOpaqueInDisguise);
	return true;
}

#endif
