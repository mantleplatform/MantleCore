// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGlbWriter.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceAssets.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionPerInstanceRandom.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// A material whose base colour is multiplied by Per Instance Random (a Fab foliage master's colour variation) read as 0 in
// GLTFExporter's bake, which has no instance, and so published black. The converter now bakes a transient copy with each
// per-instance node at a representative value and says the variation is averaged. The flattener here is scripted to read the base
// colour the way the bake does (Per Instance Random as 0), so no renderer is needed.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePerInstanceMaterialTestPrivate
{
	template <typename TExpression>
	TExpression* AddNode(UMaterial* Material)
	{
		TExpression* Expression = NewObject<TExpression>(Material);
		Expression->Material = Material;
		Material->GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}

	/** Base colour = Multiply(Constant3Vector (0.8, 0.6, 0.4), Variation), where Variation is the node the caller adds. */
	UMaterial* NewVaryingMaterial(UMaterialExpression* (*MakeVariation)(UMaterial*))
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		UMaterialExpressionConstant3Vector* Colour = AddNode<UMaterialExpressionConstant3Vector>(Material);
		Colour->Constant = FLinearColor(0.8f, 0.6f, 0.4f, 1.0f);
		UMaterialExpressionMultiply* Multiply = AddNode<UMaterialExpressionMultiply>(Material);
		Multiply->A.Connect(0, Colour);
		Multiply->B.Connect(0, MakeVariation(Material));
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Multiply);
		return Material;
	}

	UMaterialExpression* MakeRandom(UMaterial* Material)
	{
		return AddNode<UMaterialExpressionPerInstanceRandom>(Material);
	}

	UMaterialExpression* MakeCustomData(UMaterial* Material)
	{
		UMaterialExpressionPerInstanceCustomData* Data = AddNode<UMaterialExpressionPerInstanceCustomData>(Material);
		Data->ConstDefaultValue = 0.25f;
		return Data;
	}

	/** What the real bake sees in the base colour of the material it is handed: a per-instance node reads 0, a constant its value. */
	class FScriptedFlattener : public IMantleMaterialFlattener
	{
	public:
		int32 Calls = 0;
		bool bSawPerInstanceNode = false;
		bool bSawMaterialFromThisTest = false;

		virtual bool Flatten(UMaterialInterface* Material, const FMantleFlattenSettings& Settings, FMantleGlbDocument& OutDocument,
			TArray<FString>& OutMessages, FString& OutError) override
		{
			++Calls;
			const UMaterial* Base = Cast<UMaterial>(Material);
			const UMaterialExpressionMultiply* Multiply = Base && Base->GetEditorOnlyData() ? Cast<UMaterialExpressionMultiply>(Base->GetEditorOnlyData()->BaseColor.Expression) : nullptr;
			const UMaterialExpressionConstant3Vector* Colour = Multiply ? Cast<UMaterialExpressionConstant3Vector>(Multiply->A.Expression) : nullptr;
			if (!Colour)
			{
				OutError = TEXT("the scripted flattener expected Multiply(Constant3Vector, x)");
				return false;
			}
			bSawMaterialFromThisTest = true;
			double Variation = 0.0;
			if (const UMaterialExpressionConstant* Constant = Cast<UMaterialExpressionConstant>(Multiply->B.Expression))
			{
				Variation = Constant->R;
			}
			else
			{
				bSawPerInstanceNode = true;
			}
			const FLinearColor Value = Colour->Constant;
			TArray<TSharedPtr<FJsonValue>> Factor;
			for (const double Channel : { Value.R * Variation, Value.G * Variation, Value.B * Variation, 1.0 })
			{
				Factor.Add(MakeShared<FJsonValueNumber>(Channel));
			}
			const TSharedRef<FJsonObject> Pbr = MakeShared<FJsonObject>();
			Pbr->SetArrayField(TEXT("baseColorFactor"), Factor);
			Pbr->SetNumberField(TEXT("metallicFactor"), 0.0);
			Pbr->SetNumberField(TEXT("roughnessFactor"), 1.0);
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("alphaMode"), TEXT("OPAQUE"));
			Entry->SetObjectField(TEXT("pbrMetallicRoughness"), Pbr);
			TArray<TSharedPtr<FJsonValue>> Materials;
			Materials.Add(MakeShared<FJsonValueObject>(Entry));
			OutDocument = FMantleGlbDocument();
			OutDocument.Json = MakeShared<FJsonObject>();
			OutDocument.Json->SetArrayField(TEXT("materials"), Materials);
			return true;
		}

		virtual FString Describe() const override { return TEXT("scripted per-instance"); }
		virtual bool CanBakeTextures() const override { return true; }
	};

	bool HasNote(const TArray<FMantleValidationIssue>& Issues)
	{
		for (const FMantleValidationIssue& Issue : Issues)
		{
			if (Issue.Problem.Contains(TEXT("per instance"), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	bool GraphReaches(const UMaterialExpression* Expression, const UClass* Class, int32 Depth = 0)
	{
		if (!Expression || Depth > 16)
		{
			return false;
		}
		if (Expression->IsA(Class))
		{
			return true;
		}
		if (const UMaterialExpressionMultiply* Multiply = Cast<UMaterialExpressionMultiply>(Expression))
		{
			return GraphReaches(Multiply->A.Expression, Class, Depth + 1) || GraphReaches(Multiply->B.Expression, Class, Depth + 1);
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePerInstanceRandomBakesNonBlackTest,
	"Mantle.Editor.PerInstance.RandomMultipliedBaseColourExportsNonBlack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePerInstanceRandomBakesNonBlackTest::RunTest(const FString& Parameters)
{
	using namespace MantlePerInstanceMaterialTestPrivate;

	UMaterial* Material = NewVaryingMaterial(&MakeRandom);
	const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Material);
	TestTrue(TEXT("the analysis finds the per-instance variation"), Analysis.bPerInstanceVariation);
	TestEqual(TEXT("it is not a loss"), static_cast<uint32>(Analysis.Losses), static_cast<uint32>(EMantleMaterialLoss::None));

	TSharedRef<FScriptedFlattener> Flattener = MakeShared<FScriptedFlattener>();
	FMantlePlaceAssetConverter Converter(Flattener, FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantlePerInstance")));
	FMantlePlaceAssetSet Set;
	if (!TestTrue(TEXT("it converts"), Converter.ConvertMaterial(Material, Set)))
	{
		return false;
	}
	TestEqual(TEXT("the flattener ran once"), Flattener->Calls, 1);
	TestTrue(TEXT("it was handed a material to read"), Flattener->bSawMaterialFromThisTest);
	TestFalse(TEXT("the material it was handed has no per-instance node in the base colour"), Flattener->bSawPerInstanceNode);

	const FMantleConvertedMaterial* Converted = Set.FindMaterial(FMantlePlaceAssets::MaterialKey(Material));
	if (!TestNotNull(TEXT("the material is in the set"), Converted))
	{
		return false;
	}
	const FMantlePlaceMaterialDocument& Doc = Converted->Document;
	TestEqual(TEXT("red is the colour at Per Instance Random 0.5"), Doc.BaseColorFactor[0], 0.4, 1.0e-4);
	TestEqual(TEXT("green"), Doc.BaseColorFactor[1], 0.3, 1.0e-4);
	TestEqual(TEXT("blue"), Doc.BaseColorFactor[2], 0.2, 1.0e-4);
	TestTrue(TEXT("a note says the variation is averaged"), HasNote(Set.Issues));

	// The creator's material is untouched.
	const UMaterial* Source = Material;
	TestTrue(TEXT("the source still has its Per Instance Random"), GraphReaches(Source->GetEditorOnlyData()->BaseColor.Expression, UMaterialExpressionPerInstanceRandom::StaticClass()));

	// Check this place carries the same note.
	TArray<FMantleValidationIssue> Issues;
	FMantlePlaceAssets::CheckMaterial(Material, FString(), FMantlePlaceAssetLimits(), Issues);
	TestTrue(TEXT("Check this place has the note too"), HasNote(Issues));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePerInstanceCustomDataBakesDefaultTest,
	"Mantle.Editor.PerInstance.CustomDataBakesAtItsDefault",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePerInstanceCustomDataBakesDefaultTest::RunTest(const FString& Parameters)
{
	using namespace MantlePerInstanceMaterialTestPrivate;

	UMaterial* Material = NewVaryingMaterial(&MakeCustomData);
	TSharedRef<FScriptedFlattener> Flattener = MakeShared<FScriptedFlattener>();
	FMantlePlaceAssetConverter Converter(Flattener, FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantlePerInstance")));
	FMantlePlaceAssetSet Set;
	if (!TestTrue(TEXT("it converts"), Converter.ConvertMaterial(Material, Set)))
	{
		return false;
	}
	const FMantleConvertedMaterial* Converted = Set.FindMaterial(FMantlePlaceAssets::MaterialKey(Material));
	if (!TestNotNull(TEXT("the material is in the set"), Converted))
	{
		return false;
	}
	TestEqual(TEXT("red is the colour at the custom data's default (0.25)"), Converted->Document.BaseColorFactor[0], 0.2, 1.0e-4);
	TestTrue(TEXT("with the note"), HasNote(Set.Issues));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePerInstanceAbsentIsUntouchedTest,
	"Mantle.Editor.PerInstance.AMaterialWithoutOneIsBakedAsItIs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePerInstanceAbsentIsUntouchedTest::RunTest(const FString& Parameters)
{
	using namespace MantlePerInstanceMaterialTestPrivate;

	UMaterial* Material = NewVaryingMaterial([](UMaterial* Owner) -> UMaterialExpression*
	{
		UMaterialExpressionConstant* Constant = AddNode<UMaterialExpressionConstant>(Owner);
		Constant->R = 1.0f;
		return Constant;
	});
	TestFalse(TEXT("no per-instance variation is found"), FMantlePlaceAssets::AnalyzeMaterial(Material).bPerInstanceVariation);
	TestFalse(TEXT("so there is no copy to bake"), FMantlePlaceAssets::NeutralisePerInstanceValues(Material).IsValid());

	TSharedRef<FScriptedFlattener> Flattener = MakeShared<FScriptedFlattener>();
	FMantlePlaceAssetConverter Converter(Flattener, FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("MantlePerInstance")));
	FMantlePlaceAssetSet Set;
	TestTrue(TEXT("it converts"), Converter.ConvertMaterial(Material, Set));
	TestFalse(TEXT("and carries no per-instance note"), HasNote(Set.Issues));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
