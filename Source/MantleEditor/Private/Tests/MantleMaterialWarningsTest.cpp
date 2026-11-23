// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssets.h"

#include "Engine/Texture2D.h"
#include "Engine/Texture2DArray.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

// Check said nothing about six things a published material gives up: a Material Parameter Collection (saved values used), a cooked
// material (no graph to read at all), a graph too big for the walk (it stopped silently), a UDIM, cube or array texture (one image
// survives) and a render target (bakes blank). Each is its own loss now, named by Check and the coverage report. Nothing about what
// publishes changes.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleMaterialWarningsTestPrivate
{
	template <typename TExpression>
	TExpression* AddNode(UMaterial* Material)
	{
		TExpression* Expression = NewObject<TExpression>(Material);
		Expression->Material = Material;
		Material->GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}

	/** A material whose base color is the one node the caller adds. */
	UMaterial* NewMaterialWith(UMaterialExpression* (*MakeNode)(UMaterial*))
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		Material->GetEditorOnlyData()->BaseColor.Connect(0, MakeNode(Material));
		return Material;
	}

	UMaterialExpression* MakeCollectionNode(UMaterial* Material)
	{
		return AddNode<UMaterialExpressionCollectionParameter>(Material);
	}

	/** A texture sample of Texture. */
	UMaterial* NewSampleMaterial(UTexture* Texture)
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		UMaterialExpressionTextureSample* Sample = AddNode<UMaterialExpressionTextureSample>(Material);
		Sample->Texture = Texture;
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Sample);
		return Material;
	}

	/** A 2x2 BGRA8 source image. */
	UTexture2D* NewPlainTexture()
	{
		UTexture2D* Texture = NewObject<UTexture2D>(GetTransientPackage());
		TArray<uint8> Pixels;
		Pixels.Init(255, 16);
		Texture->Source.Init(2, 2, 1, 1, TSF_BGRA8, Pixels.GetData());
		return Texture;
	}

	/** Two 2x2 blocks side by side: a UDIM texture. */
	UTexture2D* NewUdimTexture()
	{
		UTexture2D* Texture = NewObject<UTexture2D>(GetTransientPackage());
		TArray<uint8> Pixels;
		Pixels.Init(255, 16);
		const ETextureSourceFormat Format = TSF_BGRA8;
		FTextureSourceBlock Blocks[2];
		for (int32 Index = 0; Index < 2; ++Index)
		{
			Blocks[Index].BlockX = Index;
			Blocks[Index].BlockY = 0;
			Blocks[Index].SizeX = 2;
			Blocks[Index].SizeY = 2;
			Blocks[Index].NumSlices = 1;
			Blocks[Index].NumMips = 1;
		}
		const uint8* Data[2] = { Pixels.GetData(), Pixels.GetData() };
		Texture->Source.InitBlocked(&Format, Blocks, 1, 2, Data);
		return Texture;
	}

	bool Says(const TArray<FMantleValidationIssue>& Issues, const TCHAR* Text)
	{
		return Issues.ContainsByPredicate([Text](const FMantleValidationIssue& Issue)
		{
			return Issue.Severity == EMantleIssueSeverity::Warning && Issue.Problem.Contains(Text);
		});
	}

	bool CheckSays(const UMaterial* Material, const TCHAR* Text)
	{
		TArray<FMantleValidationIssue> Issues;
		FMantlePlaceAssets::CheckMaterial(Material, FString(), FMantlePlaceAssetLimits(), Issues);
		return Says(Issues, Text);
	}

	bool Flags(const UMaterial* Material, const EMantleMaterialLoss Loss)
	{
		return FMantlePlaceAssets::AnalyzeMaterial(Material).Has(Loss);
	}

	/** A full binary tree of Add nodes with Depth levels: 2^Depth - 1 nodes, each its own object so the walk counts every one. */
	UMaterialExpression* MakeTree(UMaterial* Material, const int32 Depth)
	{
		if (Depth <= 1)
		{
			return AddNode<UMaterialExpressionConstant>(Material);
		}
		UMaterialExpressionAdd* Node = AddNode<UMaterialExpressionAdd>(Material);
		Node->A.Connect(0, MakeTree(Material, Depth - 1));
		Node->B.Connect(0, MakeTree(Material, Depth - 1));
		return Node;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleParameterCollectionIsFlaggedTest,
	"Mantle.Editor.MaterialWarnings.ParameterCollectionIsFlagged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleParameterCollectionIsFlaggedTest::RunTest(const FString& Parameters)
{
	using namespace MantleMaterialWarningsTestPrivate;
	UMaterial* Tinted = NewMaterialWith(&MakeCollectionNode);
	TestTrue(TEXT("a collection value feeding the base color is flagged"), Flags(Tinted, EMantleMaterialLoss::ParameterCollection));
	TestTrue(TEXT("and Check says what is lost"), CheckSays(Tinted, TEXT("Material Parameter Collection")));

	UMaterial* Plain = NewMaterialWith([](UMaterial* Material) -> UMaterialExpression* { return AddNode<UMaterialExpressionConstant3Vector>(Material); });
	TestFalse(TEXT("a plain material is not"), Flags(Plain, EMantleMaterialLoss::ParameterCollection));
	TestFalse(TEXT("and Check says nothing"), CheckSays(Plain, TEXT("Material Parameter Collection")));

	// World Position Offset is not baked, so a collection value that only moves vertices changes nothing in the bake.
	UMaterial* Offset = NewObject<UMaterial>(GetTransientPackage());
	Offset->GetEditorOnlyData()->WorldPositionOffset.Connect(0, MakeCollectionNode(Offset));
	TestFalse(TEXT("a collection value feeding only World Position Offset is not"), Flags(Offset, EMantleMaterialLoss::ParameterCollection));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCookedMaterialIsFlaggedTest,
	"Mantle.Editor.MaterialWarnings.CookedMaterialIsFlagged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCookedMaterialIsFlaggedTest::RunTest(const FString& Parameters)
{
	using namespace MantleMaterialWarningsTestPrivate;
	UMaterial* Authored = NewObject<UMaterial>(GetTransientPackage());
	TestFalse(TEXT("a material with its graph is not cooked"), Flags(Authored, EMantleMaterialLoss::CookedMaterial));

	// A cooked material has no editor-only data (its graph is gone): take it away.
	UMaterial* Cooked = NewObject<UMaterial>(GetTransientPackage());
	const FObjectProperty* Property = FindFProperty<FObjectProperty>(UMaterialInterface::StaticClass(), TEXT("EditorOnlyData"));
	if (!TestNotNull(TEXT("the editor-only data property"), Property))
	{
		return false;
	}
	Property->SetObjectPropertyValue_InContainer(Cooked, nullptr);
	TestNull(TEXT("the cooked material has no graph"), Cooked->GetEditorOnlyData());
	TestTrue(TEXT("it is flagged"), Flags(Cooked, EMantleMaterialLoss::CookedMaterial));
	TestTrue(TEXT("and Check says it cannot be read"), CheckSays(Cooked, TEXT("cooked material")));
	TestFalse(TEXT("Check says nothing about a material with its graph"), CheckSays(Authored, TEXT("cooked material")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLargeGraphIsFlaggedTest,
	"Mantle.Editor.MaterialWarnings.LargeGraphIsFlagged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLargeGraphIsFlaggedTest::RunTest(const FString& Parameters)
{
	using namespace MantleMaterialWarningsTestPrivate;
	// 2^16 - 1 nodes is more than the walk's budget; 2^10 - 1 is far under it.
	UMaterial* Large = NewObject<UMaterial>(GetTransientPackage());
	Large->GetEditorOnlyData()->BaseColor.Connect(0, MakeTree(Large, 16));
	TestTrue(TEXT("a graph past the walk's budget is flagged"), Flags(Large, EMantleMaterialLoss::LargeGraph));
	TestTrue(TEXT("and Check says it stopped reading"), CheckSays(Large, TEXT("Check stopped reading it")));

	UMaterial* Small = NewObject<UMaterial>(GetTransientPackage());
	Small->GetEditorOnlyData()->BaseColor.Connect(0, MakeTree(Small, 10));
	TestFalse(TEXT("a normal graph is not"), Flags(Small, EMantleMaterialLoss::LargeGraph));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMultiTileTextureIsFlaggedTest,
	"Mantle.Editor.MaterialWarnings.UdimCubeAndArrayTexturesAreFlagged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMultiTileTextureIsFlaggedTest::RunTest(const FString& Parameters)
{
	using namespace MantleMaterialWarningsTestPrivate;
	TestFalse(TEXT("a plain 2D texture is not"), Flags(NewSampleMaterial(NewPlainTexture()), EMantleMaterialLoss::MultiTileTexture));

	UMaterial* Udim = NewSampleMaterial(NewUdimTexture());
	TestTrue(TEXT("a UDIM texture is flagged"), Flags(Udim, EMantleMaterialLoss::MultiTileTexture));
	TestTrue(TEXT("and Check says one image shows"), CheckSays(Udim, TEXT("UDIM, cube or array texture")));

	TestTrue(TEXT("an array texture is flagged"), Flags(NewSampleMaterial(NewObject<UTexture2DArray>(GetTransientPackage())), EMantleMaterialLoss::MultiTileTexture));
	TestTrue(TEXT("a cube texture is flagged"), Flags(NewSampleMaterial(NewObject<UTextureCube>(GetTransientPackage())), EMantleMaterialLoss::MultiTileTexture));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleRenderTargetIsFlaggedTest,
	"Mantle.Editor.MaterialWarnings.RenderTargetIsFlagged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleRenderTargetIsFlaggedTest::RunTest(const FString& Parameters)
{
	using namespace MantleMaterialWarningsTestPrivate;
	UMaterial* Screen = NewSampleMaterial(NewObject<UTextureRenderTarget2D>(GetTransientPackage()));
	TestTrue(TEXT("a render target sample is flagged"), Flags(Screen, EMantleMaterialLoss::RenderTarget));
	TestFalse(TEXT("it is not also called a multi-tile texture"), Flags(Screen, EMantleMaterialLoss::MultiTileTexture));
	TestTrue(TEXT("and Check says it bakes blank"), CheckSays(Screen, TEXT("render target")));

	TestFalse(TEXT("a plain texture is not"), Flags(NewSampleMaterial(NewPlainTexture()), EMantleMaterialLoss::RenderTarget));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
