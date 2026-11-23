// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGlbWriter.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceValidator.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// The publish test's A11 and A13. An unlit glow brighter than 1 (an emissive of 3.0, 0.4, 0.1) was clipped to 1 on its way through the
// bake, so a sign published yellower and dimmer; it now keeps the part over 1 as the emissive strength, as a lit emissive does. A Modulate
// card (it multiplies what is behind it) baked with opacity 0 and published invisible; it now publishes as a darkening sheet.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleUnlitModulateTestPrivate
{
	template <typename TExpression>
	TExpression* AddNode(UMaterial* Material)
	{
		TExpression* Expression = NewObject<TExpression>(Material);
		Expression->Material = Material;
		Material->GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}

	/** An unlit material whose emissive is the color given. */
	UMaterial* NewUnlit(const FLinearColor& Emissive)
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		Material->SetShadingModel(MSM_Unlit);
		UMaterialExpressionConstant3Vector* Color = AddNode<UMaterialExpressionConstant3Vector>(Material);
		Color->Constant = Emissive;
		Material->GetEditorOnlyData()->EmissiveColor.Connect(0, Color);
		return Material;
	}

	/** What GLTFExporter writes for an unlit material: KHR_materials_unlit and its (clipped) color as the base color factor. */
	FMantleGlbDocument UnlitBake(const TCHAR* Alpha, const FLinearColor& Color)
	{
		const TSharedRef<FJsonObject> Pbr = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Factor;
		for (const double Value : { static_cast<double>(Color.R), static_cast<double>(Color.G), static_cast<double>(Color.B), static_cast<double>(Color.A) })
		{
			Factor.Add(MakeShared<FJsonValueNumber>(Value));
		}
		Pbr->SetArrayField(TEXT("baseColorFactor"), Factor);
		const TSharedRef<FJsonObject> Extensions = MakeShared<FJsonObject>();
		Extensions->SetObjectField(TEXT("KHR_materials_unlit"), MakeShared<FJsonObject>());
		const TSharedRef<FJsonObject> Material = MakeShared<FJsonObject>();
		Material->SetStringField(TEXT("alphaMode"), Alpha);
		Material->SetObjectField(TEXT("pbrMetallicRoughness"), Pbr);
		Material->SetObjectField(TEXT("extensions"), Extensions);
		TArray<TSharedPtr<FJsonValue>> Materials;
		Materials.Add(MakeShared<FJsonValueObject>(Material));
		FMantleGlbDocument Document;
		Document.Json = MakeShared<FJsonObject>();
		Document.Json->SetArrayField(TEXT("materials"), Materials);
		return Document;
	}

	FMantlePlaceMaterialDocument Convert(const FMantleGlbDocument& Bake, const FMantleMaterialFidelity2& Fidelity)
	{
		FMantlePlaceAssetSet Set;
		FMantleMaterialExtras Extras;
		Extras.Fidelity = Fidelity;
		return FMantlePlaceAssetConverter::MaterialFromGltf(Bake, 0, TEXT("MI_Test"), FMantlePlaceAssetLimits(), false, Set, Extras);
	}

	bool CheckSays(const UMaterial* Material, const TCHAR* Text)
	{
		TArray<FMantleValidationIssue> Issues;
		FMantlePlaceAssets::CheckMaterial(Material, FString(), FMantlePlaceAssetLimits(), Issues);
		return Issues.ContainsByPredicate([Text](const FMantleValidationIssue& Issue) { return Issue.Problem.Contains(Text); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUnlitGlowOverOneTest,
	"Mantle.Editor.UnlitModulate.UnlitGlowOverOneKeepsItsStrength",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUnlitGlowOverOneTest::RunTest(const FString& Parameters)
{
	using namespace MantleUnlitModulateTestPrivate;

	// The A11 sign: an emissive of (3.0, 0.4, 0.1). The bake gives the base color factor clipped to (1.0, 0.4, 0.1).
	UMaterial* Sign = NewUnlit(FLinearColor(3.0f, 0.4f, 0.1f));
	const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Sign);
	if (!TestTrue(TEXT("the glow over 1 is read from the graph"), Analysis.Fidelity2.bGlowOverOne))
	{
		return false;
	}
	TestEqual(TEXT("with its whole red"), Analysis.Fidelity2.GlowColor[0], 3.0, 1.0e-6);
	const FMantlePlaceMaterialDocument Doc = Convert(UnlitBake(TEXT("OPAQUE"), FLinearColor(1.0f, 0.4f, 0.1f)), Analysis.Fidelity2);
	TestEqual(TEXT("the strength is the brightest channel"), Doc.EmissiveStrength, 3.0, 1.0e-6);
	TestEqual(TEXT("red is full"), Doc.EmissiveFactor[0], 1.0, 1.0e-6);
	TestEqual(TEXT("green keeps its share, 0.4 of 3"), Doc.EmissiveFactor[1], 0.4 / 3.0, 1.0e-6);
	TestEqual(TEXT("and blue its share"), Doc.EmissiveFactor[2], 0.1 / 3.0, 1.0e-6);
	TestEqual(TEXT("the base color stays black"), Doc.BaseColorFactor[0], 0.0, 1.0e-9);

	// A color times an intensity (a Multiply) reads as one color.
	UMaterial* Scaled = NewObject<UMaterial>(GetTransientPackage());
	Scaled->SetShadingModel(MSM_Unlit);
	UMaterialExpressionConstant3Vector* Orange = AddNode<UMaterialExpressionConstant3Vector>(Scaled);
	Orange->Constant = FLinearColor(1.0f, 0.5f, 0.2f);
	UMaterialExpressionConstant* Intensity = AddNode<UMaterialExpressionConstant>(Scaled);
	Intensity->R = 4.0f;
	UMaterialExpressionMultiply* Times = AddNode<UMaterialExpressionMultiply>(Scaled);
	Times->A.Connect(0, Orange);
	Times->B.Connect(0, Intensity);
	Scaled->GetEditorOnlyData()->EmissiveColor.Connect(0, Times);
	const FMantleMaterialAnalysis ScaledAnalysis = FMantlePlaceAssets::AnalyzeMaterial(Scaled);
	TestTrue(TEXT("a color times an intensity over 1 is read too"), ScaledAnalysis.Fidelity2.bGlowOverOne);
	TestEqual(TEXT("its red is 4"), ScaledAnalysis.Fidelity2.GlowColor[0], 4.0, 1.0e-6);

	// A glow at or under 1 needs nothing: what published before is unchanged.
	const FMantleMaterialAnalysis Dim = FMantlePlaceAssets::AnalyzeMaterial(NewUnlit(FLinearColor(1.0f, 0.4f, 0.1f)));
	TestFalse(TEXT("a glow of at most 1 is not flagged"), Dim.Fidelity2.bGlowOverOne);
	const FMantlePlaceMaterialDocument Plain = Convert(UnlitBake(TEXT("OPAQUE"), FLinearColor(1.0f, 0.4f, 0.1f)), Dim.Fidelity2);
	TestEqual(TEXT("its strength stays 1"), Plain.EmissiveStrength, 1.0, 1.0e-9);
	TestEqual(TEXT("and its factor is the bake's"), Plain.EmissiveFactor[1], 0.4, 1.0e-6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleModulateDarkensTest,
	"Mantle.Editor.UnlitModulate.ModulatePublishesAsADarkeningSheet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleModulateDarkensTest::RunTest(const FString& Parameters)
{
	using namespace MantleUnlitModulateTestPrivate;

	// The A13 card: a grey modulate of 0.51. The bake wrote it unlit and translucent with opacity 0.
	UMaterial* Card = NewUnlit(FLinearColor(0.51f, 0.51f, 0.51f));
	Card->BlendMode = BLEND_Modulate;
	const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Card);
	TestTrue(TEXT("it is a modulate material"), Analysis.Fidelity2.bModulate);
	TestFalse(TEXT("it is not called a glow"), Analysis.Has(EMantleMaterialLoss::Unlit));
	TestTrue(TEXT("its blend-mode loss says what it becomes"), CheckSays(Card, TEXT("darkening sheet")));
	TestFalse(TEXT("Check does not call it ordinary translucency"), CheckSays(Card, TEXT("ordinary translucency")));
	TestFalse(TEXT("nor a glowing material"), CheckSays(Card, TEXT("glowing")));

	const FMantlePlaceMaterialDocument Doc = Convert(UnlitBake(TEXT("BLEND"), FLinearColor(0.51f, 0.51f, 0.51f, 0.0f)), Analysis.Fidelity2);
	TestEqual(TEXT("it publishes see-through"), Doc.AlphaMode, FString(TEXT("blend")));
	TestEqual(TEXT("black"), Doc.BaseColorFactor[0], 0.0, 1.0e-9);
	TestEqual(TEXT("with the opacity that darkens by the card's brightness"), Doc.BaseColorFactor[3], 0.49, 1.0e-6);
	TestEqual(TEXT("and no glow"), Doc.EmissiveFactor[0], 0.0, 1.0e-9);

	// A white modulate darkens nothing; a black one blocks everything.
	TestEqual(TEXT("white modulate is invisible"), FMantlePlaceAssetConverter::ModulateOpacity({ 1.0, 1.0, 1.0 }), 0.0, 1.0e-9);
	TestEqual(TEXT("black modulate is solid"), FMantlePlaceAssetConverter::ModulateOpacity({ 0.0, 0.0, 0.0 }), 1.0, 1.0e-9);

	// A textured modulate keeps its darkness per pixel, in the alpha of a black image.
	TArray<FColor> Texture;
	Texture.Add(FColor(255, 255, 255, 255));
	Texture.Add(FColor(0, 0, 0, 255));
	Texture.Add(FColor(255, 255, 255, 255));
	TArray<FColor> Sheet;
	FMantlePlaceAssetConverter::ModulateSheet(Texture, { 0.5, 0.5, 0.5 }, Sheet);
	if (TestEqual(TEXT("one sheet pixel per texture pixel"), Sheet.Num(), 3))
	{
		TestTrue(TEXT("a white pixel at factor 0.5 darkens by half"), FMath::Abs(static_cast<int32>(Sheet[0].A) - 128) <= 1);
		TestEqual(TEXT("a black pixel blocks fully"), static_cast<int32>(Sheet[1].A), 255);
		TestEqual(TEXT("the sheet itself is black"), static_cast<int32>(Sheet[0].R) + Sheet[0].G + Sheet[0].B, 0);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
