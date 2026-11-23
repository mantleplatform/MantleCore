// Copyright Joshua Eger. All Rights Reserved.
#include "MantleClientFeatures.h"
#include "MantleCoverage.h"
#include "MantleCoverageSurvey.h"
#include "MantleGlbWriter.h"
#include "MantleGlowMaterial.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceMaterial.h"
#include "MantleTargetFeatures.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionFresnel.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSceneColor.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// Additive blending (alphaMode additive) and scene-color "fake refraction" glass: what the analysis recognizes in
// a material's graph, what the converter makes of GLTFExporter's bake of it, what an older target client gets, and the
// coverage lines. The materials are small graphs built in code, so no level or asset is needed.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleGlowMaterialTestPrivate
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

	UMaterialExpressionConstant3Vector* AddColor(UMaterial* Material, const FLinearColor& Color)
	{
		UMaterialExpressionConstant3Vector* Constant = AddNode<UMaterialExpressionConstant3Vector>(Material);
		Constant->Constant = Color;
		return Constant;
	}

	UMaterialExpressionVectorParameter* AddColorParameter(UMaterial* Material, const TCHAR* Name, const FLinearColor& Color)
	{
		UMaterialExpressionVectorParameter* Parameter = AddNode<UMaterialExpressionVectorParameter>(Material);
		Parameter->ParameterName = Name;
		Parameter->DefaultValue = Color;
		return Parameter;
	}

	UMaterialExpressionScalarParameter* AddNumberParameter(UMaterial* Material, const TCHAR* Name, const float Value)
	{
		UMaterialExpressionScalarParameter* Parameter = AddNode<UMaterialExpressionScalarParameter>(Material);
		Parameter->ParameterName = Name;
		Parameter->DefaultValue = Value;
		return Parameter;
	}

	UMaterialExpressionMultiply* AddMultiply(UMaterial* Material, UMaterialExpression* A, UMaterialExpression* B)
	{
		UMaterialExpressionMultiply* Multiply = AddNode<UMaterialExpressionMultiply>(Material);
		Multiply->A.Connect(0, A);
		Multiply->B.Connect(0, B);
		return Multiply;
	}

	/** An unlit, two-sided, translucent material, with Opacity a scalar parameter of 1: the shape of a scene-color glass. */
	UMaterial* NewUnlitTranslucent()
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		Material->BlendMode = BLEND_Translucent;
		Material->SetShadingModel(MSM_Unlit);
		Material->TwoSided = true;
		Material->GetEditorOnlyData()->Opacity.Connect(0, AddNumberParameter(Material, TEXT("Opacity"), 1.0f));
		return Material;
	}

	/**
	 * M_Base_WaterGlass's emissive, cut down: Lerp(Multiply(Multiply(SceneColor, WaterColor), WaterColorIntensity), SceneColor, 0.3),
	 * with WaterColor a vector parameter of (0, 0.706457, 1) and WaterColorIntensity a scalar parameter of 2.
	 */
	UMaterial* NewSceneColorGlass()
	{
		UMaterial* Material = NewUnlitTranslucent();
		UMaterialExpressionSceneColor* Scene = AddNode<UMaterialExpressionSceneColor>(Material);
		UMaterialExpressionMultiply* Tinted = AddMultiply(Material, Scene, AddColorParameter(Material, TEXT("WaterColor"), FLinearColor(0.0f, 0.706457f, 1.0f, 0.0f)));
		UMaterialExpressionMultiply* Boosted = AddMultiply(Material, Tinted, AddNumberParameter(Material, TEXT("WaterColorIntensity"), 2.0f));
		UMaterialExpressionLinearInterpolate* Mixed = AddNode<UMaterialExpressionLinearInterpolate>(Material);
		Mixed->A.Connect(0, Boosted);
		Mixed->B.Connect(0, Scene);
		Mixed->Alpha.Connect(0, AddNumber(Material, 0.3f));
		Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Mixed);
		return Material;
	}

	/** An additive material whose emissive is the color given (a constant), lit or unlit; Rim multiplies it by a Fresnel. */
	UMaterial* NewAdditive(const bool bUnlit, const FLinearColor& Glow, const bool bRim = false)
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		Material->BlendMode = BLEND_Additive;
		Material->TwoSided = true;
		if (bUnlit)
		{
			Material->SetShadingModel(MSM_Unlit);
		}
		UMaterialExpression* Emissive = AddColor(Material, Glow);
		if (bRim)
		{
			Emissive = AddMultiply(Material, Emissive, AddNode<UMaterialExpressionFresnel>(Material));
		}
		Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Emissive);
		return Material;
	}

	/** A Fresnel node with its exponent and base reflect fraction set (the engine's defaults are 5 and 0.04). */
	UMaterialExpressionFresnel* AddFresnel(UMaterial* Material, const float Exponent = 5.0f, const float BaseReflect = 0.04f)
	{
		UMaterialExpressionFresnel* Fresnel = AddNode<UMaterialExpressionFresnel>(Material);
		Fresnel->Exponent = Exponent;
		Fresnel->BaseReflectFraction = BaseReflect;
		return Fresnel;
	}

	/** An unlit additive material whose rim the key cannot carry: a Fresnel times the vertex color (which changes across the surface). */
	UMaterial* NewDroppedRim()
	{
		UMaterial* Material = NewAdditive(true, FLinearColor::White);
		Material->GetEditorOnlyData()->EmissiveColor.Connect(0, AddMultiply(Material, AddFresnel(Material), AddNode<UMaterialExpressionVertexColor>(Material)));
		return Material;
	}

	/** FMantleGlowMaterial::ReadRim on Material's emissive and opacity, as AnalyzeMaterial reads them. */
	FMantleRimGlow RimOf(UMaterial* Material)
	{
		FMantleRimGlow Rim;
		FMantleGlowMaterial::ReadRim(Material, &Material->GetEditorOnlyData()->EmissiveColor, &Material->GetEditorOnlyData()->Opacity, Rim);
		return Rim;
	}

	TArray<TSharedPtr<FJsonValue>> NumberValues(std::initializer_list<double> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const double Value : Values)
		{
			Out.Add(MakeShared<FJsonValueNumber>(Value));
		}
		return Out;
	}

	/**
	 * What GLTFExporter writes for a flattened material: Alpha ("BLEND"), the base color factor, an emissive factor with its strength
	 * (empty for none), and KHR_materials_unlit when Unlit (its base color is then the bake of Emissive Color).
	 */
	FMantleGlbDocument Baked(const TCHAR* Alpha, std::initializer_list<double> BaseColor, const bool bUnlit, std::initializer_list<double> Emissive, const double EmissiveStrength, const bool bTwoSided)
	{
		const TSharedRef<FJsonObject> Pbr = MakeShared<FJsonObject>();
		Pbr->SetArrayField(TEXT("baseColorFactor"), NumberValues(BaseColor));
		Pbr->SetNumberField(TEXT("metallicFactor"), 0.0);
		Pbr->SetNumberField(TEXT("roughnessFactor"), 1.0);
		const TSharedRef<FJsonObject> Material = MakeShared<FJsonObject>();
		Material->SetStringField(TEXT("alphaMode"), Alpha);
		Material->SetBoolField(TEXT("doubleSided"), bTwoSided);
		Material->SetObjectField(TEXT("pbrMetallicRoughness"), Pbr);
		if (Emissive.size() == 3)
		{
			Material->SetArrayField(TEXT("emissiveFactor"), NumberValues(Emissive));
		}
		const TSharedRef<FJsonObject> Extensions = MakeShared<FJsonObject>();
		if (bUnlit)
		{
			Extensions->SetObjectField(TEXT("KHR_materials_unlit"), MakeShared<FJsonObject>());
		}
		if (EmissiveStrength != 1.0)
		{
			const TSharedRef<FJsonObject> Strength = MakeShared<FJsonObject>();
			Strength->SetNumberField(TEXT("emissiveStrength"), EmissiveStrength);
			Extensions->SetObjectField(TEXT("KHR_materials_emissive_strength"), Strength);
		}
		if (Extensions->Values.Num() > 0)
		{
			Material->SetObjectField(TEXT("extensions"), Extensions);
		}
		TArray<TSharedPtr<FJsonValue>> Materials;
		Materials.Add(MakeShared<FJsonValueObject>(Material));
		FMantleGlbDocument Document;
		Document.Json = MakeShared<FJsonObject>();
		Document.Json->SetArrayField(TEXT("materials"), Materials);
		return Document;
	}

	/** The analysis of Material, converted over Baked. */
	FMantlePlaceMaterialDocument Convert(UMaterial* Material, const FMantleGlbDocument& Document, FMantlePlaceAssetSet& Set, FMantleMaterialAnalysis* OutAnalysis = nullptr)
	{
		const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Material);
		FMantleMaterialExtras Extras;
		Extras.Fidelity = Analysis.Fidelity2;
		if (OutAnalysis)
		{
			*OutAnalysis = Analysis;
		}
		return FMantlePlaceAssetConverter::MaterialFromGltf(Document, 0, TEXT("MI_Test"), FMantlePlaceAssetLimits(), false, Set, Extras);
	}

	/** What the survey says of one project material, as the Materials category's single example (Outcome); null when it said otherwise. */
	const FMantleCoverageExample* SurveyExample(UMaterial* Material, FMantleCoverage& Coverage, const EMantleCoverageOutcome Outcome)
	{
		FMantlePlaceDependencies Dependencies;
		FMantleGatheredMaterial& Gathered = Dependencies.Materials.AddDefaulted_GetRef();
		Gathered.Material = Material;
		Gathered.Key = TEXT("material:MI_Test");
		Gathered.Name = TEXT("MI_Test");
		FMantleCoverageSurvey::AddMaterials(Dependencies, Coverage);
		const FMantleCoverageCategory* Category = Coverage.Find(MantleCoverageCategory::Materials);
		if (!Category || Category->Get(Outcome).Examples.Num() != 1)
		{
			return nullptr;
		}
		return &Category->Get(Outcome).Examples[0];
	}

	FString JsonOf(const FMantlePlaceMaterialDocument& Document, const TMap<FString, FString>& Ids = TMap<FString, FString>())
	{
		FString Json;
		FString Error;
		if (!Document.ToJson(Ids, Json, Error))
		{
			return FString::Printf(TEXT("(did not write: %s)"), *Error);
		}
		return Json;
	}

	bool Readable(const FString& Json, FMantlePlaceMaterialDesc& Out, FString& OutError)
	{
		const FTCHARToUTF8 Utf8(*Json);
		return FMantlePlaceMaterials::ParseMaterial(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), Out, &OutError);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGlowGlassRecognizedTest,
	"Mantle.Editor.Glow.SceneColorGlassIsRecognized",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGlowGlassRecognizedTest::RunTest(const FString& Parameters)
{
	using namespace MantleGlowMaterialTestPrivate;

	UMaterial* Material = NewSceneColorGlass();
	const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Material);
	const FMantleSeeThroughGlass& Glass = Analysis.Fidelity2.Glass;
	if (!TestTrue(TEXT("an unlit translucent material whose emissive reaches a Scene Color is glass"), Glass.bFound))
	{
		return false;
	}
	TestTrue(TEXT("its tint is found"), Glass.bTint);
	TestEqual(TEXT("the tint's red"), Glass.Tint[0], 0.0, 1.0e-4);
	TestEqual(TEXT("the tint's green"), Glass.Tint[1], 0.706457, 1.0e-4);
	TestEqual(TEXT("the tint's blue"), Glass.Tint[2], 1.0, 1.0e-4);
	TestEqual(TEXT("its intensity is the scalar it is multiplied by"), Glass.Intensity, 2.0, 1.0e-4);
	double Glow[3];
	Glass.Glow(Glow);
	TestEqual(TEXT("the glow is tint x intensity x the glow scale (green)"), Glow[1], 0.706457 * 2.0 * FMantleSeeThroughGlass::GlowScale, 1.0e-4);
	TestEqual(TEXT("(blue)"), Glow[2], FMath::Min(2.0 * FMantleSeeThroughGlass::GlowScale, 1.0), 1.0e-4);

	// The emissive graph is carried, so nothing in it (Scene Color, distortion) is a loss, and unlit is exact here.
	TestEqual(TEXT("it has no losses"), static_cast<uint32>(Analysis.Losses), static_cast<uint32>(EMantleMaterialLoss::None));

	// A tint that is a constant color is found when there is no parameter, with the multiplier beside it.
	UMaterial* Constant = NewUnlitTranslucent();
	UMaterialExpressionSceneColor* Scene = AddNode<UMaterialExpressionSceneColor>(Constant);
	Constant->GetEditorOnlyData()->EmissiveColor.Connect(0, AddMultiply(Constant, AddMultiply(Constant, Scene, AddColor(Constant, FLinearColor(0.2f, 0.4f, 0.6f))), AddNumber(Constant, 3.0f)));
	const FMantleMaterialAnalysis ConstantAnalysis = FMantlePlaceAssets::AnalyzeMaterial(Constant);
	if (TestTrue(TEXT("a constant tint is glass too"), ConstantAnalysis.Fidelity2.Glass.bFound))
	{
		TestTrue(TEXT("with its tint"), ConstantAnalysis.Fidelity2.Glass.bTint);
		TestEqual(TEXT("its blue"), ConstantAnalysis.Fidelity2.Glass.Tint[2], 0.6, 1.0e-4);
		TestEqual(TEXT("and its multiplier"), ConstantAnalysis.Fidelity2.Glass.Intensity, 3.0, 1.0e-4);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGlowGlassNeutralTintTest,
	"Mantle.Editor.Glow.SceneColorGlassWithoutATintIsNeutral",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGlowGlassNeutralTintTest::RunTest(const FString& Parameters)
{
	using namespace MantleGlowMaterialTestPrivate;

	// The scene behind it, as it is: no color anywhere in the graph.
	UMaterial* Material = NewUnlitTranslucent();
	Material->GetEditorOnlyData()->EmissiveColor.Connect(0, AddNode<UMaterialExpressionSceneColor>(Material));
	const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Material);
	const FMantleSeeThroughGlass& Glass = Analysis.Fidelity2.Glass;
	if (!TestTrue(TEXT("a bare Scene Color is glass"), Glass.bFound))
	{
		return false;
	}
	TestFalse(TEXT("no tint was found"), Glass.bTint);
	TestEqual(TEXT("so the tint is neutral"), Glass.Tint[1], 1.0, 1.0e-6);
	TestEqual(TEXT("with no multiplier"), Glass.Intensity, 1.0, 1.0e-6);
	double Glow[3];
	Glass.Glow(Glow);
	TestEqual(TEXT("and a gray glow"), Glow[0], FMantleSeeThroughGlass::GlowScale, 1.0e-6);
	TestEqual(TEXT("equal in every channel"), Glow[2], Glow[0], 1.0e-9);

	FMantlePlaceAssetSet Set;
	const FMantlePlaceMaterialDocument Doc = Convert(Material, Baked(TEXT("BLEND"), { 0.0, 0.0, 0.0, 1.0 }, true, {}, 1.0, true), Set);
	TestEqual(TEXT("it publishes as blend"), Doc.AlphaMode, FString(TEXT("blend")));
	TestEqual(TEXT("with the neutral glow"), Doc.EmissiveFactor[1], FMantleSeeThroughGlass::GlowScale, 1.0e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGlowGlassOnlyUnlitTranslucentTest,
	"Mantle.Editor.Glow.OnlyUnlitTranslucentSceneColorIsGlass",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGlowGlassOnlyUnlitTranslucentTest::RunTest(const FString& Parameters)
{
	using namespace MantleGlowMaterialTestPrivate;

	// A lit translucent material with a Scene Color is not this pattern.
	UMaterial* Lit = NewSceneColorGlass();
	Lit->SetShadingModel(MSM_DefaultLit);
	TestFalse(TEXT("a lit material is not glass"), FMantlePlaceAssets::AnalyzeMaterial(Lit).Fidelity2.Glass.bFound);

	// Nor is an opaque or a masked one.
	UMaterial* Opaque = NewSceneColorGlass();
	Opaque->BlendMode = BLEND_Opaque;
	TestFalse(TEXT("an opaque material is not glass"), FMantlePlaceAssets::AnalyzeMaterial(Opaque).Fidelity2.Glass.bFound);

	// An unlit translucent material that does not read the scene is a glow, not glass.
	UMaterial* Glow = NewUnlitTranslucent();
	Glow->GetEditorOnlyData()->EmissiveColor.Connect(0, AddMultiply(Glow, AddColor(Glow, FLinearColor(0.0f, 1.0f, 1.0f)), AddNumber(Glow, 4.0f)));
	TestFalse(TEXT("a glow with no Scene Color is not glass"), FMantlePlaceAssets::AnalyzeMaterial(Glow).Fidelity2.Glass.bFound);

	// A Scene Color that is not on the emissive path (here, only in Opacity) does not count.
	UMaterial* OpacityOnly = NewUnlitTranslucent();
	OpacityOnly->GetEditorOnlyData()->EmissiveColor.Connect(0, AddColor(OpacityOnly, FLinearColor(1.0f, 0.0f, 0.0f)));
	OpacityOnly->GetEditorOnlyData()->Opacity.Connect(0, AddNode<UMaterialExpressionSceneColor>(OpacityOnly));
	TestFalse(TEXT("a Scene Color only in Opacity is not glass"), FMantlePlaceAssets::AnalyzeMaterial(OpacityOnly).Fidelity2.Glass.bFound);

	// And an additive material with a Scene Color is additive, not glass.
	UMaterial* Additive = NewAdditive(true, FLinearColor::White);
	Additive->GetEditorOnlyData()->EmissiveColor.Connect(0, AddNode<UMaterialExpressionSceneColor>(Additive));
	const FMantleMaterialAnalysis AdditiveAnalysis = FMantlePlaceAssets::AnalyzeMaterial(Additive);
	TestFalse(TEXT("additive is not glass"), AdditiveAnalysis.Fidelity2.Glass.bFound);
	TestTrue(TEXT("it is additive"), AdditiveAnalysis.Fidelity2.bAdditive);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGlowGlassDocumentTest,
	"Mantle.Editor.Glow.GlassPublishesAsTintedSeeThroughBlend",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGlowGlassDocumentTest::RunTest(const FString& Parameters)
{
	using namespace MantleGlowMaterialTestPrivate;

	// GLTFExporter's bake of M_Base_WaterGlass: Scene Color is black there, so an opaque black unlit solid.
	UMaterial* Material = NewSceneColorGlass();
	const FMantleGlbDocument Bake = Baked(TEXT("BLEND"), { 0.0, 0.0, 0.0, 1.0 }, true, {}, 1.0, true);
	FMantlePlaceAssetSet Set;
	FMantleMaterialAnalysis Analysis;
	const FMantlePlaceMaterialDocument Doc = Convert(Material, Bake, Set, &Analysis);
	TestEqual(TEXT("it publishes translucent"), Doc.AlphaMode, FString(TEXT("blend")));
	TestTrue(TEXT("two-sided"), Doc.bDoubleSided);
	TestEqual(TEXT("with the glass opacity, not the bake's 1"), Doc.BaseColorFactor[3], FMantleSeeThroughGlass::Opacity, 1.0e-9);
	TestEqual(TEXT("over a black base color"), Doc.BaseColorFactor[1], 0.0, 1.0e-9);
	TestEqual(TEXT("the tint's green is the glow's"), Doc.EmissiveFactor[1], 0.706457 * 2.0 * FMantleSeeThroughGlass::GlowScale, 1.0e-4);
	TestEqual(TEXT("and its blue"), Doc.EmissiveFactor[2], FMath::Min(2.0 * FMantleSeeThroughGlass::GlowScale, 1.0), 1.0e-4);
	TestEqual(TEXT("and not its red"), Doc.EmissiveFactor[0], 0.0, 1.0e-9);
	TestEqual(TEXT("not metallic"), Doc.MetallicFactor, 0.0, 1.0e-9);
	TestEqual(TEXT("glossy"), Doc.RoughnessFactor, 0.1, 1.0e-9);
	TestEqual(TEXT("no textures"), Doc.Textures.Num(), 0);

	const FString Json = JsonOf(Doc);
	TestTrue(TEXT("it writes blend"), Json.Contains(TEXT("\"alphaMode\":\"blend\"")));
	TestTrue(TEXT("with the low opacity"), Json.Contains(TEXT("\"baseColorFactor\":[0,0,0,0.15]")));
	TestTrue(TEXT("and the scaled tint as emissive"), Json.Contains(TEXT("\"emissiveFactor\":[0,0.282583,0.4]")));
	FMantlePlaceMaterialDesc Desc;
	FString Error;
	const bool bRead = Readable(Json, Desc, Error);
	TestTrue(FString::Printf(TEXT("the client reads it: %s"), *Error), bRead);
	TestTrue(TEXT("as blend"), Desc.AlphaMode == EMantlePlaceAlphaMode::Blend);

	// Without the recognition (a material that is not glass), the bake stands: the fix is the pattern, not every unlit material.
	UMaterial* Plain = NewUnlitTranslucent();
	Plain->GetEditorOnlyData()->EmissiveColor.Connect(0, AddColor(Plain, FLinearColor::Black));
	FMantlePlaceAssetSet PlainSet;
	const FMantlePlaceMaterialDocument PlainDoc = Convert(Plain, Bake, PlainSet);
	TestEqual(TEXT("a material that is not glass keeps the bake's opacity"), PlainDoc.BaseColorFactor[3], 1.0, 1.0e-9);

	// Coverage: approximated, in the report's words.
	FMantleCoverage Coverage;
	const FMantleCoverageExample* Example = SurveyExample(Material, Coverage, EMantleCoverageOutcome::Approximated);
	if (TestNotNull(TEXT("glass is one approximated material"), Example))
	{
		TestEqual(TEXT("it names the material"), Example->Name, FString(TEXT("MI_Test")));
		TestTrue(TEXT("and says what became of it"), Example->Detail.Contains(TEXT("scene-color refraction drawn as tinted see-through glass")));
	}

	// Check this place warns once, in plain words.
	TArray<FMantleValidationIssue> Issues;
	FMantlePlaceAssets::CheckMaterial(Material, FString(), FMantlePlaceAssetLimits(), Issues);
	const bool bWarned = Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue) { return Issue.Problem.Contains(TEXT("tinted see-through glass")); });
	TestTrue(TEXT("Check this place says it publishes as tinted see-through glass"), bWarned);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGlowAdditiveAnalysisTest,
	"Mantle.Editor.Glow.AdditiveIsRecognized",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGlowAdditiveAnalysisTest::RunTest(const FString& Parameters)
{
	using namespace MantleGlowMaterialTestPrivate;

	// Unlit additive is exact: neither the blend mode nor unlit is a loss.
	const FMantleMaterialAnalysis Unlit = FMantlePlaceAssets::AnalyzeMaterial(NewAdditive(true, FLinearColor(0.037f, 0.91f, 1.0f)));
	TestTrue(TEXT("an additive material is recognized"), Unlit.Fidelity2.bAdditive);
	TestFalse(TEXT("an unlit one is not lit"), Unlit.Fidelity2.bAdditiveLit);
	TestFalse(TEXT("and has no rim"), Unlit.Fidelity2.bAdditiveRim);
	TestFalse(TEXT("the blend mode is not a loss"), Unlit.Has(EMantleMaterialLoss::BlendMode));
	TestFalse(TEXT("nor is unlit"), Unlit.Has(EMantleMaterialLoss::Unlit));

	// A lit one loses its lighting (said, not a feature loss).
	const FMantleMaterialAnalysis Lit = FMantlePlaceAssets::AnalyzeMaterial(NewAdditive(false, FLinearColor(0.037f, 0.91f, 1.0f)));
	TestTrue(TEXT("a lit additive material is recognized"), Lit.Fidelity2.bAdditive);
	TestTrue(TEXT("and is lit"), Lit.Fidelity2.bAdditiveLit);
	TestFalse(TEXT("its blend mode is still not a loss"), Lit.Has(EMantleMaterialLoss::BlendMode));

	// A Fresnel in the emissive is a rim glow (F4): a color times a Fresnel is carried by the rim key; one it cannot carry is dropped.
	const FMantleMaterialAnalysis Rim = FMantlePlaceAssets::AnalyzeMaterial(NewAdditive(false, FLinearColor::White, true));
	TestTrue(TEXT("a Fresnel times a color is a rim the key carries"), Rim.Fidelity2.Rim.bFound);
	TestFalse(TEXT("so it is not dropped"), Rim.Fidelity2.bAdditiveRim);
	const FMantleMaterialAnalysis Dropped = FMantlePlaceAssets::AnalyzeMaterial(NewDroppedRim());
	TestFalse(TEXT("a Fresnel times the vertex color is not carried"), Dropped.Fidelity2.Rim.bFound);
	TestTrue(TEXT("so it is dropped"), Dropped.Fidelity2.bAdditiveRim);
	UMaterial* Plain = NewAdditive(false, FLinearColor::White);
	TestFalse(TEXT("no Fresnel, no rim"), FMantleGlowMaterial::HasFresnel(&Plain->GetEditorOnlyData()->EmissiveColor));
	TestFalse(TEXT("nothing connected, no rim"), FMantleGlowMaterial::HasFresnel(nullptr));

	// Translucent is not additive, and its blend mode was never a loss.
	UMaterial* Translucent = NewUnlitTranslucent();
	Translucent->GetEditorOnlyData()->EmissiveColor.Connect(0, AddColor(Translucent, FLinearColor::White));
	const FMantleMaterialAnalysis Blend = FMantlePlaceAssets::AnalyzeMaterial(Translucent);
	TestFalse(TEXT("a translucent material is not additive"), Blend.Fidelity2.bAdditive);
	TestTrue(TEXT("and unlit still is a loss on it"), Blend.Has(EMantleMaterialLoss::Unlit));

	// Modulate and the rest stay a blend-mode loss.
	UMaterial* Modulate = NewAdditive(true, FLinearColor::White);
	Modulate->BlendMode = BLEND_Modulate;
	TestTrue(TEXT("modulate blending is still a blend-mode loss"), FMantlePlaceAssets::AnalyzeMaterial(Modulate).Has(EMantleMaterialLoss::BlendMode));

	// Check this place says what a lit or rimmed additive surface gives up, and nothing about an unlit exact one.
	auto Says = [](UMaterial* Material, const TCHAR* Phrase)
	{
		TArray<FMantleValidationIssue> Issues;
		FMantlePlaceAssets::CheckMaterial(Material, FString(), FMantlePlaceAssetLimits(), Issues);
		return Issues.ContainsByPredicate([Phrase](const FMantleValidationIssue& Issue) { return Issue.Problem.Contains(Phrase); });
	};
	TestTrue(TEXT("a lit additive material is warned about"), Says(NewAdditive(false, FLinearColor::White), TEXT("lit additive")));
	TestFalse(TEXT("a carried rim is not warned about"), Says(NewAdditive(true, FLinearColor::White, true), TEXT("Fresnel")));
	TestTrue(TEXT("a dropped rim is warned about, with its reason"),
		Says(NewDroppedRim(), TEXT("its Fresnel rim glow is dropped because the rim is multiplied by a texture or another value that changes across the surface")));
	TestFalse(TEXT("an unlit additive material is not"), Says(NewAdditive(true, FLinearColor::White), TEXT("additive")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGlowAdditiveDocumentTest,
	"Mantle.Editor.Glow.AdditivePublishesAsAdditive",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGlowAdditiveDocumentTest::RunTest(const FString& Parameters)
{
	using namespace MantleGlowMaterialTestPrivate;

	// M_Base_Hologram: lit, additive. GLTFExporter writes blend over a black base color, and the emissive as a factor with a strength.
	{
		UMaterial* Material = NewAdditive(false, FLinearColor(0.2f, 0.9f, 1.0f));
		FMantlePlaceAssetSet Set;
		const FMantlePlaceMaterialDocument Doc = Convert(Material, Baked(TEXT("BLEND"), { 0.0, 0.0, 0.0, 1.0 }, false, { 0.2, 0.9, 1.0 }, 3.0, false), Set);
		TestEqual(TEXT("a lit additive material publishes as additive"), Doc.AlphaMode, FString(TEXT("additive")));
		TestTrue(TEXT("and is written so"), Doc.WritesAdditive());
		TestEqual(TEXT("with its emissive"), Doc.EmissiveFactor[1], 0.9, 1.0e-9);
		TestEqual(TEXT("and its strength"), Doc.EmissiveStrength, 3.0, 1.0e-9);
		const FString Json = JsonOf(Doc);
		TestTrue(TEXT("it writes additive"), Json.Contains(TEXT("\"alphaMode\":\"additive\"")));
		TestTrue(TEXT("the glow"), Json.Contains(TEXT("\"emissiveFactor\":[0.2,0.9,1],\"emissiveStrength\":3")));
		TestFalse(TEXT("no base color: it does nothing on additive"), Json.Contains(TEXT("baseColorFactor")));
		TestFalse(TEXT("no metallic"), Json.Contains(TEXT("metallicFactor")));
		TestFalse(TEXT("no roughness"), Json.Contains(TEXT("roughnessFactor")));
		FMantlePlaceMaterialDesc Desc;
		FString Error;
		const bool bRead = Readable(Json, Desc, Error);
		TestTrue(FString::Printf(TEXT("the client reads it: %s"), *Error), bRead);
		TestTrue(TEXT("as additive"), Desc.AlphaMode == EMantlePlaceAlphaMode::Additive);
		TestEqual(TEXT("with its strength"), Desc.EmissiveStrength, 3.0f);
		TestEqual(TEXT("it picks the one-sided Additive permutation"), FMantlePlaceMaterials::PermutationIndex(Desc), FMantlePlaceMaterials::AdditivePermutationBase);
	}

	// M_Base_FakeVolume: unlit, additive, two-sided. The unlit bake keeps the glow as its base color, which moves to the emissive;
	// a constant opacity (the base color's alpha) scales it, and the strength stays 1.
	{
		UMaterial* Material = NewAdditive(true, FLinearColor(0.037f, 0.91f, 1.0f));
		FMantlePlaceAssetSet Set;
		const FMantlePlaceMaterialDocument Doc = Convert(Material, Baked(TEXT("BLEND"), { 0.037, 0.91, 1.0, 0.5 }, true, {}, 1.0, true), Set);
		TestEqual(TEXT("an unlit additive material publishes as additive"), Doc.AlphaMode, FString(TEXT("additive")));
		TestTrue(TEXT("two-sided"), Doc.bDoubleSided);
		TestEqual(TEXT("its glow is the base color times the opacity (green)"), Doc.EmissiveFactor[1], 0.455, 1.0e-9);
		TestEqual(TEXT("(blue)"), Doc.EmissiveFactor[2], 0.5, 1.0e-9);
		TestEqual(TEXT("with strength 1"), Doc.EmissiveStrength, 1.0, 1.0e-9);
		const FString Json = JsonOf(Doc);
		TestTrue(TEXT("two-sided is written"), Json.Contains(TEXT("\"doubleSided\":true")));
		TestFalse(TEXT("no base color"), Json.Contains(TEXT("baseColorFactor")));
		FMantlePlaceMaterialDesc Desc;
		FString Error;
		const bool bRead = Readable(Json, Desc, Error);
		TestTrue(FString::Printf(TEXT("the client reads it: %s"), *Error), bRead);
		TestEqual(TEXT("it picks the two-sided Additive permutation"), FMantlePlaceMaterials::PermutationIndex(Desc), FMantlePlaceMaterials::AdditivePermutationBase + 1);
	}

	// A translucent material is untouched: it keeps blend and its base color.
	{
		UMaterial* Material = NewUnlitTranslucent();
		Material->GetEditorOnlyData()->EmissiveColor.Connect(0, AddColor(Material, FLinearColor(0.1f, 0.2f, 0.3f)));
		FMantlePlaceAssetSet Set;
		const FMantlePlaceMaterialDocument Doc = Convert(Material, Baked(TEXT("BLEND"), { 0.1, 0.2, 0.3, 0.4 }, true, {}, 1.0, true), Set);
		TestEqual(TEXT("a translucent unlit material stays blend"), Doc.AlphaMode, FString(TEXT("blend")));
		TestEqual(TEXT("and keeps its opacity"), Doc.BaseColorFactor[3], 0.4, 1.0e-9);
	}

	// The conversion cache stores a template and reads it back: an additive document survives that.
	{
		FMantlePlaceMaterialDocument Doc;
		Doc.AlphaMode = TEXT("additive");
		Doc.bDoubleSided = true;
		Doc.EmissiveFactor[0] = 0.25;
		Doc.EmissiveStrength = 2.0;
		Doc.Textures.Add(TEXT("emissive"), TEXT("texture:color:abc"));
		Doc.UvPanning.Add(TEXT("emissive"), FVector2D(0.05, -0.05));
		FMantlePlaceMaterialDocument Back;
		TestTrue(TEXT("the template reads back"), FMantlePlaceMaterialDocument::FromTemplateJson(Doc.ToTemplateJson(), Back));
		TestEqual(TEXT("still additive"), Back.AlphaMode, FString(TEXT("additive")));
		TestEqual(TEXT("with its glow"), Back.EmissiveFactor[0], 0.25, 1.0e-9);
		TestEqual(TEXT("and strength"), Back.EmissiveStrength, 2.0, 1.0e-9);
		TestEqual(TEXT("and its texture"), Back.Textures.FindRef(TEXT("emissive")), FString(TEXT("texture:color:abc")));
		TestTrue(TEXT("and its pan"), Back.UvPanning.Contains(TEXT("emissive")));
		TestEqual(TEXT("the template is the same text"), Back.ToTemplateJson(), Doc.ToTemplateJson());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGlowAdditiveDowngradeTest,
	"Mantle.Editor.Glow.AdditiveDowngradesWithoutMaterialAdditive",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGlowAdditiveDowngradeTest::RunTest(const FString& Parameters)
{
	using namespace MantleGlowMaterialTestPrivate;

	auto Additive = [](const double Strength)
	{
		FMantlePlaceMaterialDocument Document;
		Document.AlphaMode = TEXT("additive");
		Document.bDoubleSided = true;
		Document.EmissiveFactor[0] = 0.037;
		Document.EmissiveFactor[1] = 0.91;
		Document.EmissiveFactor[2] = 1.0;
		Document.EmissiveStrength = Strength;
		return Document;
	};

	// A client that reads alphaMode additive changes nothing.
	FMantlePlaceMaterialDocument Kept = Additive(2.0);
	TestFalse(TEXT("a client that reads materialAdditive downgrades nothing"), FMantleTargetFeatures::All().DowngradeMaterial(Kept));
	TestEqual(TEXT("and the document is still additive"), Kept.AlphaMode, FString(TEXT("additive")));

	// One that does not gets translucent blending: a black base color and an opacity from the glow, clamp(luminance x strength, 0, 1).
	const double Luminance = 0.2126 * 0.037 + 0.7152 * 0.91 + 0.0722 * 1.0;
	FMantlePlaceMaterialDocument Faint = Additive(0.5);
	TestTrue(TEXT("a client without it downgrades"), FMantleTargetFeatures::None().DowngradeMaterial(Faint));
	TestEqual(TEXT("to blend"), Faint.AlphaMode, FString(TEXT("blend")));
	TestEqual(TEXT("with a black base color"), Faint.BaseColorFactor[1], 0.0, 1.0e-9);
	TestEqual(TEXT("and an opacity from the glow's luminance times its strength"), Faint.BaseColorFactor[3], Luminance * 0.5, 1.0e-9);
	TestEqual(TEXT("the glow is the emissive, kept (green)"), Faint.EmissiveFactor[1], 0.91, 1.0e-9);
	TestEqual(TEXT("and its strength"), Faint.EmissiveStrength, 0.5, 1.0e-9);
	TestFalse(TEXT("the downgrade is only once"), FMantleTargetFeatures::None().DowngradeMaterial(Faint));

	FMantlePlaceMaterialDocument Bright = Additive(2.0);
	FMantleTargetFeatures::None().DowngradeMaterial(Bright);
	TestEqual(TEXT("a bright glow is clamped to opaque"), Bright.BaseColorFactor[3], 1.0, 1.0e-9);

	// A client that reads materialFidelity but not materialAdditive still gets the downgrade.
	FMantlePlaceMaterialDocument Between = Additive(1.0);
	FMantleTargetFeatures FidelityOnly = FMantleTargetFeatures::None();
	FidelityOnly.Names.Add(MantleClientFeatures::MaterialFidelity);
	TestTrue(TEXT("materialFidelity alone does not read additive"), FidelityOnly.DowngradeMaterial(Between));
	TestEqual(TEXT("so it is blend"), Between.AlphaMode, FString(TEXT("blend")));

	// What it writes: blend with the black base color and the opacity, no additive.
	FString Json;
	FString Error;
	TestTrue(TEXT("the downgraded document writes"), Faint.ToJson(TMap<FString, FString>(), Json, Error));
	TestTrue(TEXT("as blend"), Json.Contains(TEXT("\"alphaMode\":\"blend\"")));
	TestFalse(TEXT("not additive"), Json.Contains(TEXT("additive")));
	TestTrue(TEXT("with a black base color"), Json.Contains(TEXT("\"baseColorFactor\":[0,0,0,")));
	TestTrue(TEXT("and the glow as emissive"), Json.Contains(TEXT("\"emissiveFactor\":[0.037,0.91,1]")));

	// The asset set writes it through the export's target, and says what it left out.
	FMantlePlaceAssetSet Set;
	FMantleConvertedMaterial& Converted = Set.Materials.AddDefaulted_GetRef();
	Converted.Key = TEXT("material:MI_Hologram");
	Converted.Name = TEXT("MI_Hologram");
	Converted.Document = Additive(2.0);
	auto Written = [&Set, &Converted, &Error]()
	{
		TArray<uint8> Bytes;
		Set.MaterialBytes(Converted, Bytes, Error);
		const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Text.Length(), Text.Get());
	};
	TestTrue(TEXT("no export in progress: the set writes additive"), Written().Contains(TEXT("\"alphaMode\":\"additive\"")));
	{
		const FMantleTargetFeaturesScope Scope(FMantleTargetFeatures::None());
		TestTrue(TEXT("for the older client it writes blend"), Written().Contains(TEXT("\"alphaMode\":\"blend\"")));
		TestEqual(TEXT("the source document still has its additive"), Converted.Document.AlphaMode, FString(TEXT("additive")));

		FMantleCoverage Coverage;
		FMantleTargetFeatures::Current().ReportMaterialDowngrades(Set, Coverage);
		TestEqual(TEXT("one approximated material"), Coverage.Count(MantleCoverageCategory::Materials, EMantleCoverageOutcome::Approximated), static_cast<int64>(1));
		const FMantleCoverageCategory* Category = Coverage.Find(MantleCoverageCategory::Materials);
		if (TestNotNull(TEXT("the materials category"), Category) && Category->Get(EMantleCoverageOutcome::Approximated).Examples.Num() == 1)
		{
			const FMantleCoverageExample& Example = Category->Get(EMantleCoverageOutcome::Approximated).Examples[0];
			TestEqual(TEXT("it names the material"), Example.Name, FString(TEXT("MI_Hologram")));
			TestTrue(TEXT("and says why"), Example.Detail.Contains(TEXT("needs a newer Mantle client")));
		}
	}
	FMantleCoverage Quiet;
	FMantleTargetFeatures::All().ReportMaterialDowngrades(Set, Quiet);
	TestTrue(TEXT("a client that reads it reports nothing"), Quiet.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGlowAdditiveCoverageTest,
	"Mantle.Editor.Glow.AdditiveCoverageLines",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGlowAdditiveCoverageTest::RunTest(const FString& Parameters)
{
	using namespace MantleGlowMaterialTestPrivate;

	// An unlit additive material with a constant glow is kept, recognized as additive.
	{
		FMantleCoverage Coverage;
		const FMantleCoverageExample* Example = SurveyExample(NewAdditive(true, FLinearColor(0.037f, 0.91f, 1.0f)), Coverage, EMantleCoverageOutcome::Kept);
		if (TestNotNull(TEXT("an unlit additive material is kept"), Example))
		{
			TestTrue(TEXT("recognized as additive blending"), Example->Detail.Contains(TEXT("additive blending")));
		}
	}
	// A lit one is approximated, and says its lighting is lost.
	{
		FMantleCoverage Coverage;
		const FMantleCoverageExample* Example = SurveyExample(NewAdditive(false, FLinearColor(0.037f, 0.91f, 1.0f)), Coverage, EMantleCoverageOutcome::Approximated);
		if (TestNotNull(TEXT("a lit additive material is approximated"), Example))
		{
			TestTrue(TEXT("it says it is drawn unlit"), Example->Detail.Contains(TEXT("lit additive drawn unlit")));
			TestTrue(TEXT("and that additive blending is recognized"), Example->Detail.Contains(TEXT("additive blending")));
		}
	}
	// A rim glow the rim key carries is kept, and said (F4).
	{
		FMantleCoverage Coverage;
		const FMantleCoverageExample* Example = SurveyExample(NewAdditive(true, FLinearColor::White, true), Coverage, EMantleCoverageOutcome::Kept);
		if (TestNotNull(TEXT("an additive material with a carried rim is kept"), Example))
		{
			TestTrue(TEXT("it lists the rim glow"), Example->Detail.Contains(TEXT("Fresnel rim glow (brightest where the surface is seen edge-on)")));
		}
	}
	// One it cannot carry is dropped, with its reason.
	{
		FMantleCoverage Coverage;
		const FMantleCoverageExample* Example = SurveyExample(NewDroppedRim(), Coverage, EMantleCoverageOutcome::Approximated);
		if (TestNotNull(TEXT("an additive material with a dropped rim is approximated"), Example))
		{
			TestTrue(TEXT("it says the rim is dropped, and why"), Example->Detail.Contains(TEXT("Fresnel rim glow dropped (the rim is multiplied by a texture")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGlowRimReadTest,
	"Mantle.Editor.Glow.RimIsRead",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGlowRimReadTest::RunTest(const FString& Parameters)
{
	using namespace MantleGlowMaterialTestPrivate;
	auto Emissive = [](UMaterial* Material, UMaterialExpression* Node) { Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Node); };

	// Bay A12's material: a lit additive Fresnel (exponent 3, base reflect 0.04) times (0.1, 2, 3). The bake holds 0.04 of the color;
	// the rim is the other 0.96, at power 3.
	{
		UMaterial* Material = NewAdditive(false, FLinearColor::Black);
		Emissive(Material, AddMultiply(Material, AddFresnel(Material, 3.0f), AddColor(Material, FLinearColor(0.1f, 2.0f, 3.0f))));
		const FMantleRimGlow Rim = RimOf(Material);
		TestTrue(TEXT("a color times a Fresnel is a rim"), Rim.bFound);
		TestEqual(TEXT("red is 0.96 of the color"), Rim.Color[0], 0.096, 1.0e-6);
		TestEqual(TEXT("green passes 1"), Rim.Color[1], 1.92, 1.0e-6);
		TestEqual(TEXT("blue"), Rim.Color[2], 2.88, 1.0e-6);
		TestEqual(TEXT("its power is the exponent"), Rim.Power, 3.0, 1.0e-6);
		TestFalse(TEXT("no Normal pin"), Rim.bNormalPin);
	}
	// A base reflect fraction of 0.2: the rim is 0.8 of the color.
	{
		UMaterial* Material = NewAdditive(true, FLinearColor::Black);
		Emissive(Material, AddMultiply(Material, AddColor(Material, FLinearColor(1.0f, 0.5f, 0.0f)), AddFresnel(Material, 5.0f, 0.2f)));
		const FMantleRimGlow Rim = RimOf(Material);
		TestTrue(TEXT("found"), Rim.bFound);
		TestEqual(TEXT("0.8 of red"), Rim.Color[0], 0.8, 1.0e-6);
		TestEqual(TEXT("0.8 of green"), Rim.Color[1], 0.4, 1.0e-6);
	}
	// The force-field bubble (bay A12b): Lerp(dim, bright, Fresnel 5, base reflect 0.1) is dim + (bright - dim) x Fresnel.
	{
		UMaterial* Material = NewAdditive(true, FLinearColor::Black);
		UMaterialExpressionLinearInterpolate* Mix = AddNode<UMaterialExpressionLinearInterpolate>(Material);
		Mix->A.Connect(0, AddColor(Material, FLinearColor(0.02f, 0.08f, 0.12f)));
		Mix->B.Connect(0, AddColorParameter(Material, TEXT("EdgeColor"), FLinearColor(0.2f, 0.9f, 1.0f)));
		Mix->Alpha.Connect(0, AddFresnel(Material, 5.0f, 0.1f));
		Emissive(Material, Mix);
		const FMantleRimGlow Rim = RimOf(Material);
		TestTrue(TEXT("a Lerp by a Fresnel is a rim"), Rim.bFound);
		TestEqual(TEXT("(bright - dim) x 0.9, green"), Rim.Color[1], (0.9 - 0.08) * 0.9, 1.0e-6);
		TestEqual(TEXT("blue"), Rim.Color[2], (1.0 - 0.12) * 0.9, 1.0e-6);
		TestEqual(TEXT("power 5"), Rim.Power, 5.0, 1.0e-6);
	}
	// A Fresnel in the opacity (bay A12c): additive adds emissive x opacity, so the rim is the color times the Fresnel's part.
	{
		UMaterial* Material = NewAdditive(true, FLinearColor(0.0f, 0.5f, 1.0f));
		Material->GetEditorOnlyData()->Opacity.Connect(0, AddFresnel(Material, 4.0f, 0.0f));
		const FMantleRimGlow Rim = RimOf(Material);
		TestTrue(TEXT("a Fresnel opacity is a rim"), Rim.bFound);
		TestEqual(TEXT("green"), Rim.Color[1], 0.5, 1.0e-6);
		TestEqual(TEXT("blue"), Rim.Color[2], 1.0, 1.0e-6);
		TestEqual(TEXT("power 4"), Rim.Power, 4.0, 1.0e-6);
	}
	// A texture beside the rim (a pattern plus an edge glow) is the bake's; the rim is still read. Its exponent from a Scalar Parameter.
	{
		UMaterial* Material = NewAdditive(true, FLinearColor::Black);
		UMaterialExpressionFresnel* Fresnel = AddFresnel(Material);
		Fresnel->ExponentIn.Connect(0, AddNumberParameter(Material, TEXT("RimPower"), 8.0f));
		UMaterialExpressionAdd* Sum = AddNode<UMaterialExpressionAdd>(Material);
		Sum->A.Connect(0, AddNode<UMaterialExpressionTextureSample>(Material));
		Sum->B.Connect(0, AddMultiply(Material, Fresnel, AddNumber(Material, 2.0f)));
		Emissive(Material, Sum);
		const FMantleRimGlow Rim = RimOf(Material);
		TestTrue(TEXT("a texture added beside the rim leaves it readable"), Rim.bFound);
		TestEqual(TEXT("its rim is 2 x 0.96"), Rim.Color[0], 1.92, 1.0e-6);
		TestEqual(TEXT("its power from the parameter"), Rim.Power, 8.0, 1.0e-6);
	}
	// The Normal pin wired: still read, and said.
	{
		UMaterial* Material = NewAdditive(true, FLinearColor::Black);
		UMaterialExpressionFresnel* Fresnel = AddFresnel(Material);
		Fresnel->Normal.Connect(0, AddColor(Material, FLinearColor(0.0f, 0.0f, 1.0f)));
		Emissive(Material, AddMultiply(Material, Fresnel, AddColor(Material, FLinearColor::White)));
		const FMantleRimGlow Rim = RimOf(Material);
		TestTrue(TEXT("a Fresnel with a Normal input is read"), Rim.bFound);
		TestTrue(TEXT("and its Normal pin is noted"), Rim.bNormalPin);
	}

	// What the key cannot carry, each with its own reason.
	auto Dropped = [this](const TCHAR* What, UMaterial* Material, const TCHAR* Reason)
	{
		const FMantleRimGlow Rim = RimOf(Material);
		TestFalse(FString::Printf(TEXT("%s is not carried"), What), Rim.bFound);
		TestTrue(FString::Printf(TEXT("%s says why (%s): %s"), What, Reason, *Rim.Why), Rim.Why.Contains(Reason));
	};
	{
		UMaterial* Material = NewAdditive(true, FLinearColor::Black);
		Emissive(Material, AddMultiply(Material, AddFresnel(Material), AddNode<UMaterialExpressionTextureSample>(Material)));
		Dropped(TEXT("a Fresnel times a texture (a scanline hologram)"), Material, TEXT("the rim is multiplied by a texture"));
	}
	{
		UMaterial* Material = NewAdditive(true, FLinearColor::Black);
		UMaterialExpressionOneMinus* Inverse = AddNode<UMaterialExpressionOneMinus>(Material);
		Inverse->Input.Connect(0, AddFresnel(Material));
		Emissive(Material, AddMultiply(Material, Inverse, AddColor(Material, FLinearColor::White)));
		Dropped(TEXT("1 - Fresnel"), Material, TEXT("its edges are darker than its face"));
	}
	{
		UMaterial* Material = NewAdditive(true, FLinearColor::Black);
		UMaterialExpressionMaterialFunctionCall* Call = AddNode<UMaterialExpressionMaterialFunctionCall>(Material);
		// SetMaterialFunction makes the call's outputs: without them Connect below connects nothing.
		UMaterialFunctionInterface* Function = LoadObject<UMaterialFunctionInterface>(nullptr, TEXT("/Engine/Functions/Engine_MaterialFunctions02/Fresnel_Function.Fresnel_Function"));
		TestNotNull(TEXT("the engine's Fresnel_Function loads"), Function);
		TestTrue(TEXT("and the call takes it"), Function && Call->SetMaterialFunction(Function));
		Emissive(Material, AddMultiply(Material, Call, AddColor(Material, FLinearColor::White)));
		Dropped(TEXT("the Fresnel_Function"), Material, TEXT("its Fresnel is inside a material function (Fresnel_Function)"));
	}
	{
		UMaterial* Material = NewAdditive(true, FLinearColor::Black);
		UMaterialExpressionAdd* Sum = AddNode<UMaterialExpressionAdd>(Material);
		Sum->A.Connect(0, AddMultiply(Material, AddFresnel(Material, 3.0f), AddColor(Material, FLinearColor::White)));
		Sum->B.Connect(0, AddMultiply(Material, AddFresnel(Material, 8.0f), AddColor(Material, FLinearColor::Red)));
		Emissive(Material, Sum);
		Dropped(TEXT("two Fresnels of different exponents"), Material, TEXT("two Fresnels with different settings"));
	}
	{
		UMaterial* Material = NewAdditive(true, FLinearColor::Black);
		UMaterialExpressionFresnel* Fresnel = AddFresnel(Material);
		Fresnel->ExponentIn.Connect(0, AddNode<UMaterialExpressionVertexColor>(Material));
		Emissive(Material, AddMultiply(Material, Fresnel, AddColor(Material, FLinearColor::White)));
		Dropped(TEXT("an exponent that changes"), Material, TEXT("exponent is not a constant"));
	}
	{
		UMaterial* Material = NewAdditive(true, FLinearColor::White, true);
		Material->GetEditorOnlyData()->Opacity.Connect(0, AddNode<UMaterialExpressionTextureSample>(Material));
		Dropped(TEXT("a textured opacity"), Material, TEXT("its opacity changes across the surface"));
	}

	// No Fresnel, nothing to read or say.
	const FMantleRimGlow None = RimOf(NewAdditive(true, FLinearColor::White));
	TestFalse(TEXT("no Fresnel, no rim"), None.bFound);
	TestTrue(TEXT("and nothing to say"), None.Why.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGlowRimDocumentTest,
	"Mantle.Editor.Glow.RimPublishes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGlowRimDocumentTest::RunTest(const FString& Parameters)
{
	using namespace MantleGlowMaterialTestPrivate;

	// White x Fresnel (5, 0.04), unlit: the bake sees 0.04 of white; the rim key carries the other 0.96.
	{
		FMantlePlaceAssetSet Set;
		const FMantlePlaceMaterialDocument Doc = Convert(NewAdditive(true, FLinearColor::White, true), Baked(TEXT("BLEND"), { 0.04, 0.04, 0.04, 1.0 }, true, {}, 1.0, true), Set);
		TestTrue(TEXT("the document has a rim"), Doc.bRim);
		TestEqual(TEXT("its color"), Doc.RimColor[0], 0.96, 1.0e-6);
		TestEqual(TEXT("strength 1"), Doc.RimStrength, 1.0, 1.0e-6);
		TestEqual(TEXT("power 5"), Doc.RimPower, 5.0, 1.0e-6);
		TestEqual(TEXT("the face-on glow is the bake's"), Doc.EmissiveFactor[0], 0.04, 1.0e-6);
		const FString Json = JsonOf(Doc);
		TestTrue(FString::Printf(TEXT("it writes the rim: %s"), *Json), Json.Contains(TEXT("\"rim\":{\"color\":[0.96,0.96,0.96],\"strength\":1,\"power\":5}")));
		FMantlePlaceMaterialDesc Desc;
		FString Error;
		const bool bRead = Readable(Json, Desc, Error);
		TestTrue(FString::Printf(TEXT("the client reads it: %s"), *Error), bRead);
		TestEqual(TEXT("with its rim strength"), Desc.Rim.Strength, 1.0f);
		TestEqual(TEXT("and power"), Desc.Rim.Power, 5.0f);
		TestEqual(TEXT("and color"), Desc.Rim.Color.R, 0.96f, 1.0e-6f);

		// The conversion cache's template keeps it.
		FMantlePlaceMaterialDocument Back;
		TestTrue(TEXT("the template reads back"), FMantlePlaceMaterialDocument::FromTemplateJson(Doc.ToTemplateJson(), Back));
		TestTrue(TEXT("with its rim"), Back.bRim);
		TestEqual(TEXT("the template is the same text"), Back.ToTemplateJson(), Doc.ToTemplateJson());
	}

	// Bay A12: lit, (0.1, 2, 3) x Fresnel 3. Its brightest channel is the strength, as the glow's is.
	{
		UMaterial* Material = NewAdditive(false, FLinearColor::Black);
		Material->GetEditorOnlyData()->EmissiveColor.Connect(0, AddMultiply(Material, AddFresnel(Material, 3.0f), AddColor(Material, FLinearColor(0.1f, 2.0f, 3.0f))));
		FMantlePlaceAssetSet Set;
		const FMantlePlaceMaterialDocument Doc = Convert(Material, Baked(TEXT("BLEND"), { 0.0, 0.0, 0.0, 1.0 }, false, { 0.004, 0.08, 0.12 }, 1.0, false), Set);
		TestTrue(TEXT("a rim"), Doc.bRim);
		TestEqual(TEXT("strength is the brightest channel"), Doc.RimStrength, 2.88, 1.0e-6);
		TestEqual(TEXT("blue is 1"), Doc.RimColor[2], 1.0, 1.0e-6);
		TestEqual(TEXT("green is 1.92 / 2.88"), Doc.RimColor[1], 1.92 / 2.88, 1.0e-6);
		TestEqual(TEXT("power 3"), Doc.RimPower, 3.0, 1.0e-6);
	}

	// A dropped rim writes no rim key.
	{
		FMantlePlaceAssetSet Set;
		const FMantlePlaceMaterialDocument Doc = Convert(NewDroppedRim(), Baked(TEXT("BLEND"), { 0.04, 0.04, 0.04, 1.0 }, true, {}, 1.0, true), Set);
		TestFalse(TEXT("no rim"), Doc.bRim);
		TestFalse(TEXT("none written"), JsonOf(Doc).Contains(TEXT("\"rim\"")));
	}

	// An older client (no materialAdditive) gets blend, which has no rim.
	{
		FMantlePlaceAssetSet Set;
		FMantlePlaceMaterialDocument Doc = Convert(NewAdditive(true, FLinearColor::White, true), Baked(TEXT("BLEND"), { 0.04, 0.04, 0.04, 1.0 }, true, {}, 1.0, true), Set);
		TestTrue(TEXT("downgraded"), FMantleTargetFeatures::None().DowngradeMaterial(Doc));
		TestFalse(TEXT("with no rim"), Doc.bRim);
		TestFalse(TEXT("and none written"), JsonOf(Doc).Contains(TEXT("\"rim\"")));
	}

	// Only an additive material with a Fresnel changes its cache key.
	{
		const FMantleFlattenSettings Settings;
		const FMantlePlaceAssetLimits Limits;
		const FMantleBakeSurface Plain;
		auto Salt = [&](UMaterial* Material) { return FMantlePlaceAssetConverter::MaterialCacheSalt(TEXT("flattener"), Settings, Limits, Material, false, Plain); };
		TestTrue(TEXT("an additive Fresnel material's key has the rim tag"), Salt(NewAdditive(true, FLinearColor::White, true)).Contains(TEXT(" additiverim=1")));
		UMaterial* InOpacity = NewAdditive(true, FLinearColor::White);
		InOpacity->GetEditorOnlyData()->Opacity.Connect(0, AddFresnel(InOpacity));
		TestTrue(TEXT("so does one with a Fresnel opacity"), Salt(InOpacity).Contains(TEXT(" additiverim=1")));
		TestFalse(TEXT("an additive material without a Fresnel keeps its key"), Salt(NewAdditive(true, FLinearColor::White)).Contains(TEXT("additiverim")));
		UMaterial* Translucent = NewUnlitTranslucent();
		Translucent->GetEditorOnlyData()->EmissiveColor.Connect(0, AddMultiply(Translucent, AddFresnel(Translucent), AddColor(Translucent, FLinearColor::White)));
		TestFalse(TEXT("a translucent Fresnel material keeps its key"), Salt(Translucent).Contains(TEXT("additiverim")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
