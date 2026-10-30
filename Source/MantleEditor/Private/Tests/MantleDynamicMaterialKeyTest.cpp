// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssets.h"

#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// A Blueprint makes one Material Instance Dynamic per actor (a book's cover, pages and spine textures picked at random), so a level held
// hundreds of them, each a separate upload and a separate 2 s bake. A transient instance is now keyed by its content (parent and the
// values it overrides), so identical ones are one material.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDynamicMaterialKeyTestPrivate
{
	UMaterialInstanceDynamic* NewInstance(UMaterial* Parent, const TCHAR* TexturePath)
	{
		UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Parent, GetTransientPackage());
		Instance->SetTextureParameterValue(TEXT("Cover"), LoadObject<UTexture2D>(nullptr, TexturePath));
		return Instance;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDynamicMaterialKeySameContentTest,
	"Mantle.Editor.MaterialKey.DynamicInstancesWithTheSameTextureShareAKey",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDynamicMaterialKeySameContentTest::RunTest(const FString& Parameters)
{
	using namespace MantleDynamicMaterialKeyTestPrivate;

	UMaterial* Parent = NewObject<UMaterial>(GetTransientPackage());
	UMaterialInstanceDynamic* First = NewInstance(Parent, TEXT("/Engine/EngineResources/DefaultTexture"));
	UMaterialInstanceDynamic* Second = NewInstance(Parent, TEXT("/Engine/EngineResources/DefaultTexture"));
	TestTrue(TEXT("they are two objects"), First != Second);
	TestTrue(TEXT("a dynamic instance is transient"), FMantlePlaceAssets::IsTransientMaterialInstance(First));
	TestEqual(TEXT("they have the same key"), FMantlePlaceAssets::MaterialKey(First), FMantlePlaceAssets::MaterialKey(Second));
	TestFalse(TEXT("which is not the object path"), FMantlePlaceAssets::MaterialKey(First) == TEXT("material:") + First->GetPathName());

	Second->SetScalarParameterValue(TEXT("Wear"), 0.5f);
	TestTrue(TEXT("a different scalar changes the key"), FMantlePlaceAssets::MaterialKey(First) != FMantlePlaceAssets::MaterialKey(Second));
	First->SetScalarParameterValue(TEXT("Wear"), 0.5f);
	TestEqual(TEXT("the same scalar again shares it"), FMantlePlaceAssets::MaterialKey(First), FMantlePlaceAssets::MaterialKey(Second));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDynamicMaterialKeyDifferentTextureTest,
	"Mantle.Editor.MaterialKey.DynamicInstancesWithADifferentTextureOrParentDiffer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDynamicMaterialKeyDifferentTextureTest::RunTest(const FString& Parameters)
{
	using namespace MantleDynamicMaterialKeyTestPrivate;

	UMaterial* Parent = NewObject<UMaterial>(GetTransientPackage());
	UMaterialInstanceDynamic* First = NewInstance(Parent, TEXT("/Engine/EngineResources/DefaultTexture"));
	UMaterialInstanceDynamic* OtherTexture = NewInstance(Parent, TEXT("/Engine/EngineResources/DefaultTexture_Low"));
	TestTrue(TEXT("a different texture gives a different key"), FMantlePlaceAssets::MaterialKey(First) != FMantlePlaceAssets::MaterialKey(OtherTexture));

	UMaterial* OtherParent = NewObject<UMaterial>(GetTransientPackage());
	UMaterialInstanceDynamic* OtherParentInstance = NewInstance(OtherParent, TEXT("/Engine/EngineResources/DefaultTexture"));
	TestTrue(TEXT("a different parent gives a different key"), FMantlePlaceAssets::MaterialKey(First) != FMantlePlaceAssets::MaterialKey(OtherParentInstance));
	return true;
}

// MagiciansLibrary's books still made 559 distinct looks (cover, spine and pages each picked at random), 52 minutes of bakes. Past
// MaxVariantsPerMaterial looks of one material, the rest are drawn as one of those, picked by content so the same level picks the same.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDynamicMaterialVariantCapTest,
	"Mantle.Editor.MaterialKey.LooksOfOneMaterialPastTheCapStandInAsAnEarlierOne",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDynamicMaterialVariantCapTest::RunTest(const FString& Parameters)
{
	using namespace MantleDynamicMaterialKeyTestPrivate;

	const int32 Cap = FMantlePlaceAssets::MaxVariantsPerMaterial;
	UMaterial* Parent = NewObject<UMaterial>(GetTransientPackage());
	UMaterial* FewParent = NewObject<UMaterial>(GetTransientPackage());
	TArray<UMaterialInstanceDynamic*> Instances;
	FMantlePlaceDependencies Dependencies;
	FMantlePlaceAssets::ClearVariantStandIns();
	for (int32 Index = 0; Index < Cap + 5; ++Index)
	{
		UMaterialInstanceDynamic* Instance = NewInstance(Parent, TEXT("/Engine/EngineResources/DefaultTexture"));
		Instance->SetScalarParameterValue(TEXT("Wear"), static_cast<float>(Index));
		Instances.Add(Instance);
		Dependencies.AddMaterial(Instance, FString::Printf(TEXT("Book%d"), Index));
	}
	for (int32 Index = 0; Index < 3; ++Index)
	{
		UMaterialInstanceDynamic* Instance = NewInstance(FewParent, TEXT("/Engine/EngineResources/DefaultTexture"));
		Instance->SetScalarParameterValue(TEXT("Wear"), static_cast<float>(Index));
		Dependencies.AddMaterial(Instance, FString::Printf(TEXT("Candle%d"), Index));
	}

	TestEqual(TEXT("the capped material and the other one's three looks are gathered"), Dependencies.Materials.Num(), Cap + 3);
	const TArray<const UMaterialInterface*>* Capped = Dependencies.CappedInstances.Find(Parent->GetName());
	TestTrue(TEXT("the books past the cap are named"), Capped && Capped->Num() == 5);
	TestFalse(TEXT("a material with few looks is not capped"), Dependencies.CappedInstances.Contains(FewParent->GetName()));
	for (int32 Index = Cap; Index < Instances.Num(); ++Index)
	{
		const FString Key = FMantlePlaceAssets::MaterialKey(Instances[Index]);
		const bool bGathered = Dependencies.Materials.ContainsByPredicate([&Key](const FMantleGatheredMaterial& Entry) { return Entry.Key == Key; });
		TestTrue(FString::Printf(TEXT("book %d's key is one of the gathered looks"), Index), bGathered);
	}
	TestEqual(TEXT("an earlier book keeps its own key"), FMantlePlaceAssets::MaterialKey(Instances[0]), Dependencies.Materials[0].Key);
	FMantlePlaceAssets::ClearVariantStandIns();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePersistentMaterialKeyTest,
	"Mantle.Editor.MaterialKey.APersistentMaterialKeepsItsPathKey",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePersistentMaterialKeyTest::RunTest(const FString& Parameters)
{
	UMaterialInterface* Material = UMaterial::GetDefaultMaterial(MD_Surface);
	TestFalse(TEXT("an on-disk material is not a transient instance"), FMantlePlaceAssets::IsTransientMaterialInstance(Material));
	TestEqual(TEXT("its key is still its path"), FMantlePlaceAssets::MaterialKey(Material), TEXT("material:") + Material->GetPathName());
	TestEqual(TEXT("and none has none"), FMantlePlaceAssets::MaterialKey(nullptr), FString());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
