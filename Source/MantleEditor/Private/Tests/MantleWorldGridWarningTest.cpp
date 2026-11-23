// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssets.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialFunction.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// The engine's WorldGridMaterial (the grey grid on a mesh with no material, and on BSP) read as "world-aligned, baked as one 4 m tile": it
// is mapped by the mesh's UVs, and its only World Position sits inside the CameraDepthFade function, which picks the grid's scale by
// camera distance. That is no world mapping, so it no longer raises the warning. A World Position anywhere else still does, also in a
// material that calls CameraDepthFade.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleWorldGridWarningTestPrivate
{
	template <typename TExpression>
	TExpression* AddNode(UMaterial* Material)
	{
		TExpression* Expression = NewObject<TExpression>(Material);
		Expression->Material = Material;
		Material->GetExpressionCollection().AddExpression(Expression);
		return Expression;
	}

	UMaterialFunction* LoadCameraDepthFade()
	{
		return LoadObject<UMaterialFunction>(nullptr, TEXT("/Engine/Functions/Engine_MaterialFunctions01/Opacity/CameraDepthFade.CameraDepthFade"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWorldGridNoWorldAlignedWarningTest,
	"Mantle.Editor.WorldAligned.WorldGridMaterialIsNotWorldAligned",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWorldGridNoWorldAlignedWarningTest::RunTest(const FString& Parameters)
{
	using namespace MantleWorldGridWarningTestPrivate;
	UMaterialInterface* Grid = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
	UMaterialFunction* Fade = LoadCameraDepthFade();
	if (!Grid || !Fade)
	{
		AddWarning(TEXT("the engine's WorldGridMaterial or CameraDepthFade is missing; skipped"));
		return true;
	}

	// The engine's grid: no world-aligned loss.
	TestFalse(TEXT("WorldGridMaterial is not a world-aligned material"), FMantlePlaceAssets::AnalyzeMaterial(Grid).Has(EMantleMaterialLoss::WorldAligned));

	// A material that only calls CameraDepthFade: none either.
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		UMaterialExpressionMaterialFunctionCall* Call = AddNode<UMaterialExpressionMaterialFunctionCall>(Material);
		Call->SetMaterialFunction(Fade);
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Call);
		TestFalse(TEXT("a material that only fades by camera distance is not world-aligned"),
			FMantlePlaceAssets::AnalyzeMaterial(Material).Has(EMantleMaterialLoss::WorldAligned));
	}

	// A real World Position into the base color: still world-aligned.
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		Material->GetEditorOnlyData()->BaseColor.Connect(0, AddNode<UMaterialExpressionWorldPosition>(Material));
		TestTrue(TEXT("World Position into the base color is still world-aligned"),
			FMantlePlaceAssets::AnalyzeMaterial(Material).Has(EMantleMaterialLoss::WorldAligned));
	}

	// A real World Position beside a CameraDepthFade call: still world-aligned (the exception is for the function's own node only).
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transient);
		UMaterialExpressionMaterialFunctionCall* Call = AddNode<UMaterialExpressionMaterialFunctionCall>(Material);
		Call->SetMaterialFunction(Fade);
		Material->GetEditorOnlyData()->Roughness.Connect(0, Call);
		Material->GetEditorOnlyData()->BaseColor.Connect(0, AddNode<UMaterialExpressionWorldPosition>(Material));
		TestTrue(TEXT("a World Position outside the fade is still world-aligned"),
			FMantlePlaceAssets::AnalyzeMaterial(Material).Has(EMantleMaterialLoss::WorldAligned));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
