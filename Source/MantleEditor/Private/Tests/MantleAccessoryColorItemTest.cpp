// Copyright Joshua Eger. All Rights Reserved.
#include "MantleClothingColor.h"
#include "MantleCosmeticPackage.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// A rigid accessory (a hat) declares colour areas in its item body as clothing does; a slot outside clothing
// and accessories (hair) still refuses them. Pure: no network, no assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleAccessoryColorItemTestPrivate
{
	FMantleCosmeticPackage CapPackage()
	{
		FMantleCosmeticPackage Package;
		Package.Class = EMantleCosmeticClass::Accessory;
		Package.Slot = TEXT("acc_hat");
		Package.Name = TEXT("Cap");
		Package.Description = TEXT("A cap.");
		Package.ThumbnailKey = TEXT("thumbnail");
		Package.MeshKey = TEXT("mesh");
		Package.Visibility = TEXT("private");
		FMantleCosmeticMaterialRef& Ref = Package.Materials.AddDefaulted_GetRef();
		Ref.AlbedoKey = TEXT("albedo");
		Ref.ColorMaskKey = TEXT("mask");
		FMantleClothingColorRegion& Region = Ref.ColorRegions.AddDefaulted_GetRef();
		Region.Id = TEXT("main");
		Region.Label = TEXT("Main");
		Region.Channel = 0;
		Region.DefaultColor = FColor(0x30, 0x30, 0x30, 255);
		Region.ReferenceLuminance = 0.05f;
		return Package;
	}

	TMap<FString, FString> CapIds()
	{
		TMap<FString, FString> Ids;
		Ids.Add(TEXT("thumbnail"), TEXT("ast_u11111111111111111111111111111111"));
		Ids.Add(TEXT("mesh"), TEXT("ast_u22222222222222222222222222222222"));
		Ids.Add(TEXT("albedo"), TEXT("ast_u33333333333333333333333333333333"));
		Ids.Add(TEXT("mask"), TEXT("ast_u44444444444444444444444444444444"));
		return Ids;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleAccessoryColorItemTest,
	"Mantle.ClothingColor.AccessoryItemDeclaresColors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleAccessoryColorItemTest::RunTest(const FString& Parameters)
{
	using namespace MantleAccessoryColorItemTestPrivate;
	FString Json, Error;
	const FMantleCosmeticPackage Cap = CapPackage();
	if (!TestTrue(TEXT("a cap with colour areas makes its item"), Cap.MakeItemJson(CapIds(), Json, Error)))
	{
		AddError(Error);
		return false;
	}
	TSharedPtr<FJsonObject> Item;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Item);
	const TArray<TSharedPtr<FJsonValue>>* Materials = nullptr;
	const TSharedPtr<FJsonObject>* Colors = nullptr;
	const bool bHasColors = Item.IsValid() && Item->TryGetArrayField(TEXT("materials"), Materials) && Materials->Num() == 1
		&& (*Materials)[0]->AsObject()->TryGetObjectField(TEXT("colorCustomization"), Colors);
	if (TestTrue(TEXT("its material carries colorCustomization"), bHasColors))
	{
		TestEqual(TEXT("with its mask"), (*Colors)->GetStringField(TEXT("maskAssetId")), FString(TEXT("ast_u44444444444444444444444444444444")));
		TestEqual(TEXT("and its one region"), (*Colors)->GetArrayField(TEXT("regions")).Num(), 1);
		TestFalse(TEXT("an accessory still sends no normal map"), (*Materials)[0]->AsObject()->HasField(TEXT("normalAssetId")));
	}

	FMantleCosmeticPackage Hair = CapPackage();
	Hair.Class = EMantleCosmeticClass::Hair;
	Hair.Slot = TEXT("hair");
	TestFalse(TEXT("hair refuses colour areas"), Hair.MakeItemJson(CapIds(), Json, Error));
	return true;
}

#endif
