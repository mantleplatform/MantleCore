// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarContract.h"
#include "MantleCosmeticExport.h"
#include "MantleCosmeticPackage.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// A new version published into an existing item works for every class, not just avatars: the export no longer
// refuses a target id on a garment, and the PATCH body carries the whole new version. Pure: no network, no assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleUpdateInPlaceTestPrivate
{
	const TCHAR* const ItemId = TEXT("itm_1234567890abcdef");

	/** A garment package with two body variants, the smallest MakeItemJson accepts. */
	FMantleCosmeticPackage MakeVariantGarmentPackage()
	{
		FMantleCosmeticPackage Package;
		Package.Class = EMantleCosmeticClass::Garment;
		Package.Slot = TEXT("top");
		Package.Name = TEXT("Jacket");
		Package.Description = TEXT("A jacket.");
		Package.Visibility = TEXT("private");
		Package.TargetItemId = ItemId;
		Package.ThumbnailKey = TEXT("thumbnail");
		Package.MeshKey = TEXT("mesh");
		Package.SourceShapeKey = TEXT("shape");
		FMantleCosmeticMaterialRef& Ref = Package.Materials.AddDefaulted_GetRef();
		Ref.AlbedoKey = TEXT("albedo");
		Ref.NormalKey = TEXT("normal");
		Package.DefaultGarmentVariant = TEXT("medium");
		for (const FString& Id : TArray<FString>{ TEXT("medium"), TEXT("large") })
		{
			FMantleCosmeticGarmentVariantExport& Variant = Package.GarmentVariants.AddDefaulted_GetRef();
			Variant.Id = Id;
			Variant.NativeSourceSize = Id;
			Variant.MeshKey = TEXT("mesh_") + Id;
			Variant.SourceShapeKey = TEXT("shape_") + Id;
			Variant.Materials = Package.Materials;
		}
		return Package;
	}

	TMap<FString, FString> VariantGarmentIds(const FMantleCosmeticPackage& Package)
	{
		TMap<FString, FString> Ids;
		int32 Next = 1;
		for (const FString& Key : TArray<FString>{ TEXT("thumbnail"), TEXT("mesh"), TEXT("shape"), TEXT("albedo"), TEXT("normal") })
		{
			Ids.Add(Key, FString::Printf(TEXT("ast_u%032d"), Next++));
		}
		for (const FMantleCosmeticGarmentVariantExport& Variant : Package.GarmentVariants)
		{
			Ids.Add(Variant.MeshKey, FString::Printf(TEXT("ast_u%032d"), Next++));
			Ids.Add(Variant.SourceShapeKey, FString::Printf(TEXT("ast_u%032d"), Next++));
		}
		return Ids;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUpdateInPlaceGarmentBuildTest,
	"Mantle.Cosmetic.UpdateInPlace.GarmentTargetIsNotRefused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUpdateInPlaceGarmentBuildTest::RunTest(const FString& Parameters)
{
	using namespace MantleUpdateInPlaceTestPrivate;
	FString ContractError;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!TestNotNull(*FString::Printf(TEXT("the plugin's contract loads (%s)"), *ContractError), Contract))
	{
		return false;
	}

	// No source asset is set, so the build stops at "choose the asset"; the target must have got past the old refusal.
	FMantleCosmeticDraft Draft;
	Draft.Class = EMantleCosmeticClass::Garment;
	Draft.Slot = TEXT("top");
	Draft.Name = TEXT("Jacket");
	Draft.PublishIntoItemId = FString(TEXT("  ")) + ItemId + TEXT(" ");
	const FMantleCosmeticPackage Package = FMantleCosmeticExport::BuildCore(Draft, *Contract);
	TestEqual(TEXT("the target id is kept, trimmed"), Package.TargetItemId, FString(ItemId));
	for (const FMantleCosmeticIssue& Issue : Package.Issues)
	{
		TestFalse(TEXT("no issue says a garment cannot be updated"), Issue.ToString().Contains(TEXT("cannot be updated")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUpdateInPlaceGarmentPatchTest,
	"Mantle.Cosmetic.UpdateInPlace.GarmentPatchCarriesMaterialsAndVariantsButNoSlot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUpdateInPlaceGarmentPatchTest::RunTest(const FString& Parameters)
{
	using namespace MantleUpdateInPlaceTestPrivate;
	const auto Read = [](const FString& Json)
	{
		TSharedPtr<FJsonObject> Root;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root);
		return Root;
	};

	FMantleCosmeticPackage Package = MakeVariantGarmentPackage();
	FString Json;
	FString Error;
	if (!TestTrue(TEXT("the patch is made"), Package.MakeItemPatchJson(VariantGarmentIds(Package), Json, Error)))
	{
		AddError(Error);
		return false;
	}
	TSharedPtr<FJsonObject> Patch = Read(Json);
	if (!TestTrue(TEXT("it is JSON"), Patch.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("the slot cannot change, so it is not sent"), Patch->HasField(TEXT("slot")));
	TestTrue(TEXT("the materials are sent"), Patch->HasTypedField<EJson::Array>(TEXT("materials")));
	const TSharedPtr<FJsonObject>* Manifest = nullptr;
	if (TestTrue(TEXT("the garment variants are sent"), Patch->TryGetObjectField(TEXT("garmentVariants"), Manifest) && Manifest))
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (TestTrue(TEXT("with their rows"), (*Manifest)->TryGetArrayField(TEXT("variants"), Rows) && Rows))
		{
			TestEqual(TEXT("one per body variant"), Rows->Num(), 2);
		}
	}
	TestEqual(TEXT("the unused coverage is cleared"), Patch->GetStringField(TEXT("coverageAssetId")), FString());
	TestEqual(TEXT("and the unused face mesh"), Patch->GetStringField(TEXT("faceMeshAssetId")), FString());

	// A version without variants clears the old version's manifest instead of keeping it.
	Package.GarmentVariants.Reset();
	if (TestTrue(TEXT("a patch without variants is made"), Package.MakeItemPatchJson(VariantGarmentIds(Package), Json, Error)))
	{
		Patch = Read(Json);
		const TSharedPtr<FJsonObject>* Cleared = nullptr;
		if (TestTrue(TEXT("it still names the variants"), Patch.IsValid() && Patch->TryGetObjectField(TEXT("garmentVariants"), Cleared) && Cleared))
		{
			TestEqual(TEXT("as an empty object"), (*Cleared)->Values.Num(), 0);
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
