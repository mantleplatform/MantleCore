// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssets.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionPanner.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// A mask read from a second UV set (UV1 or higher: City Sample's road lane masks, a test plane's stripes) was baked silently
// wrong: the bake card has UV0 only and the engine's bake reads a missing set as the last one it has, so the mask repeated with
// UV0's tiling and Check said nothing. The analysis now flags it, and Check says so.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleSecondUvMaterialTestPrivate
{
	template <typename TExpression>
	TExpression* AddNode(UMaterial* Material)
	{
		TExpression* Expression = NewObject<TExpression>(Material);
		Expression->Material = Material;
		Material->GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}

	/** Base color = Lerp(grey, yellow, R of the UV node the caller adds): stripes placed by that UV set. */
	UMaterial* NewUvMaskMaterial(UMaterialExpression* (*MakeUv)(UMaterial*))
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		UMaterialExpressionConstant3Vector* Grey = AddNode<UMaterialExpressionConstant3Vector>(Material);
		Grey->Constant = FLinearColor(0.5f, 0.5f, 0.5f, 1.0f);
		UMaterialExpressionConstant3Vector* Yellow = AddNode<UMaterialExpressionConstant3Vector>(Material);
		Yellow->Constant = FLinearColor(0.9f, 0.75f, 0.05f, 1.0f);
		UMaterialExpressionComponentMask* Mask = AddNode<UMaterialExpressionComponentMask>(Material);
		Mask->R = true;
		Mask->G = false;
		Mask->B = false;
		Mask->A = false;
		Mask->Input.Connect(0, MakeUv(Material));
		UMaterialExpressionLinearInterpolate* Lerp = AddNode<UMaterialExpressionLinearInterpolate>(Material);
		Lerp->A.Connect(0, Grey);
		Lerp->B.Connect(0, Yellow);
		Lerp->Alpha.Connect(0, Mask);
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Lerp);
		return Material;
	}

	UMaterialExpression* MakeCoordinate(UMaterial* Material, int32 Index)
	{
		UMaterialExpressionTextureCoordinate* Coordinate = AddNode<UMaterialExpressionTextureCoordinate>(Material);
		Coordinate->CoordinateIndex = Index;
		return Coordinate;
	}

	UMaterialExpression* MakeUv0(UMaterial* Material) { return MakeCoordinate(Material, 0); }
	UMaterialExpression* MakeUv1(UMaterial* Material) { return MakeCoordinate(Material, 1); }
	UMaterialExpression* MakeUv2(UMaterial* Material) { return MakeCoordinate(Material, 2); }

	/** A texture sample with its UV input unwired and set to UV1 (its Const Coordinate). */
	UMaterialExpression* MakeSampleOnUv1(UMaterial* Material)
	{
		UMaterialExpressionTextureSample* Sample = AddNode<UMaterialExpressionTextureSample>(Material);
		Sample->ConstCoordinate = 1;
		return Sample;
	}

	/** The same sample, but its UV input wired to UV0: the wire wins over Const Coordinate. */
	UMaterialExpression* MakeSampleWiredToUv0(UMaterial* Material)
	{
		UMaterialExpressionTextureSample* Sample = AddNode<UMaterialExpressionTextureSample>(Material);
		Sample->ConstCoordinate = 1;
		Sample->Coordinates.Connect(0, MakeCoordinate(Material, 0));
		return Sample;
	}

	/** A panner with its UV input unwired and set to UV1. */
	UMaterialExpression* MakePannerOnUv1(UMaterial* Material)
	{
		UMaterialExpressionPanner* Panner = AddNode<UMaterialExpressionPanner>(Material);
		Panner->ConstCoordinate = 1;
		return Panner;
	}

	bool ReadsSecondUv(UMaterialExpression* (*MakeUv)(UMaterial*))
	{
		return FMantlePlaceAssets::AnalyzeMaterial(NewUvMaskMaterial(MakeUv)).Has(EMantleMaterialLoss::SecondUv);
	}

	bool SaysSecondUv(const TArray<FMantleValidationIssue>& Issues)
	{
		return Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
		{
			return Issue.Severity == EMantleIssueSeverity::Warning && Issue.Problem.Contains(TEXT("second UV set"));
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSecondUvIsFlaggedTest,
	"Mantle.Editor.SecondUv.MaskFromASecondUvSetIsFlagged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSecondUvIsFlaggedTest::RunTest(const FString& Parameters)
{
	using namespace MantleSecondUvMaterialTestPrivate;
	TestTrue(TEXT("a Texture Coordinate on UV1 is flagged"), ReadsSecondUv(&MakeUv1));
	TestTrue(TEXT("a Texture Coordinate on UV2 is flagged"), ReadsSecondUv(&MakeUv2));
	TestTrue(TEXT("a texture sample whose Const Coordinate is UV1 is flagged"), ReadsSecondUv(&MakeSampleOnUv1));
	TestTrue(TEXT("a panner whose Const Coordinate is UV1 is flagged"), ReadsSecondUv(&MakePannerOnUv1));

	TestFalse(TEXT("a Texture Coordinate on UV0 is not"), ReadsSecondUv(&MakeUv0));
	TestFalse(TEXT("a sample wired to UV0 is not, whatever its Const Coordinate"), ReadsSecondUv(&MakeSampleWiredToUv0));

	TArray<FMantleValidationIssue> Issues;
	FMantlePlaceAssets::CheckMaterial(NewUvMaskMaterial(&MakeUv1), FString(), FMantlePlaceAssetLimits(), Issues);
	TestTrue(TEXT("Check warns about the second UV set"), SaysSecondUv(Issues));

	TArray<FMantleValidationIssue> Plain;
	FMantlePlaceAssets::CheckMaterial(NewUvMaskMaterial(&MakeUv0), FString(), FMantlePlaceAssetLimits(), Plain);
	TestFalse(TEXT("and says nothing about it for UV0"), SaysSecondUv(Plain));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSecondUvInWorldPositionOffsetTest,
	"Mantle.Editor.SecondUv.OnlyWorldPositionOffsetIsNotFlagged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSecondUvInWorldPositionOffsetTest::RunTest(const FString& Parameters)
{
	using namespace MantleSecondUvMaterialTestPrivate;
	// World Position Offset is not baked (it is its own loss), so UV1 that only moves vertices changes nothing in the bake.
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
	Material->GetEditorOnlyData()->WorldPositionOffset.Connect(0, MakeUv1(Material));
	const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Material);
	TestFalse(TEXT("UV1 feeding only World Position Offset is not a second-UV loss"), Analysis.Has(EMantleMaterialLoss::SecondUv));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
