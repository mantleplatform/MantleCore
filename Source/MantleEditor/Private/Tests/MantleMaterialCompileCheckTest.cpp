// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssets.h"
#include "MantlePlaceValidator.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionStaticSwitch.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// Check this place and a material that does not compile: Unreal then draws its grey checkerboard instead, and publish would bake
// that. A headless (-nullrhi) editor never compiles shaders by itself, so the check asks for the compile; the materials here are
// small graphs built in code, so no level or asset is needed.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleMaterialCompileCheckTestPrivate
{
	UMaterial* NewOpaque(const FLinearColor& Color, const bool bBrokenStaticSwitch)
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		UMaterialExpressionConstant3Vector* Constant = NewObject<UMaterialExpressionConstant3Vector>(Material);
		Constant->Material = Material;
		Constant->Constant = Color;
		Material->GetExpressionCollection().AddExpression(Constant);
		if (bBrokenStaticSwitch)
		{
			// A Static Switch whose B input is left empty: the case the Showcase's High_Tech_Base master has.
			UMaterialExpressionStaticSwitch* Switch = NewObject<UMaterialExpressionStaticSwitch>(Material);
			Switch->Material = Material;
			Switch->DefaultValue = true;
			Switch->A.Connect(0, Constant);
			Material->GetExpressionCollection().AddExpression(Switch);
			Material->GetEditorOnlyData()->BaseColor.Connect(0, Switch);
		}
		else
		{
			Material->GetEditorOnlyData()->BaseColor.Connect(0, Constant);
		}
		Material->PostEditChange();
		return Material;
	}

	UMaterialInstanceConstant* NewInstance(UMaterial* Parent)
	{
		UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(GetTransientPackage());
		Instance->SetParentEditorOnly(Parent);
		Instance->PostEditChange();
		return Instance;
	}

	FMantleGatheredMaterial Gathered(UMaterialInterface* Material, const TCHAR* UsedBy)
	{
		FMantleGatheredMaterial Entry;
		Entry.Material = Material;
		Entry.Key = Material->GetName();
		Entry.Name = Material->GetName();
		Entry.UsedBy.Add(UsedBy);
		return Entry;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMaterialCompileCheckTest,
	"Mantle.Editor.MaterialCompileCheck.ReportsABrokenMaterial",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMaterialCompileCheckTest::RunTest(const FString& Parameters)
{
	using namespace MantleMaterialCompileCheckTestPrivate;

	UMaterial* Broken = NewOpaque(FLinearColor::Red, true);
	UMaterialInstanceConstant* BrokenInstance = NewInstance(Broken);
	UMaterial* Good = NewOpaque(FLinearColor::Green, false);
	UMaterialInstanceConstant* GoodInstance = NewInstance(Good);

	TArray<FMantleGatheredMaterial> Materials;
	Materials.Add(Gathered(BrokenInstance, TEXT("Wall")));
	Materials.Add(Gathered(Broken, TEXT("Floor")));
	Materials.Add(Gathered(GoodInstance, TEXT("Roof")));
	Materials.Add(Gathered(Good, TEXT("Door")));

	TArray<FMantleValidationIssue> Issues;
	FMantlePlaceAssets::CheckMaterialsCompile(Materials, Issues);

	// One error, for the broken master; the working one is not named.
	TestEqual(TEXT("one issue, for the one broken material, however many instances and actors reach it"), Issues.Num(), 1);
	if (Issues.Num() == 1)
	{
		const FMantleValidationIssue& Issue = Issues[0];
		TestTrue(TEXT("it is an error, as publish would bake the checkerboard"), Issue.Severity == EMantleIssueSeverity::Error);
		TestTrue(TEXT("it names the material"), Issue.Problem.Contains(Broken->GetName()));
		TestTrue(TEXT("it names the instance that reaches it"), Issue.Problem.Contains(BrokenInstance->GetName()));
		TestTrue(TEXT("it names the actors"), Issue.Problem.Contains(TEXT("used by 'Wall' and 1 more")));
		TestTrue(TEXT("it says the checkerboard is what publishes"), Issue.Problem.Contains(TEXT("grey checkerboard")));
		TestTrue(TEXT("it carries Unreal's own words"), Issue.Problem.Contains(TEXT("Missing B input")));
		TestFalse(TEXT("it does not name the working material"), Issue.Problem.Contains(Good->GetName()));
		TestTrue(TEXT("it says how to fix it"), Issue.Remedy.Contains(TEXT("Material Editor")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
