// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssets.h"
#include "MantlePlaceAssetsCoverage.h"
#include "MantlePlaceStorage.h"
#include "MantlePlaceTypes.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Stored-item authorization: what a script's Vehicles:Spawn and Backpack:Grant may use. The only
// authority is the place's own storage as the loader expanded it, so these build a place and an expansion in code
// and ask FMantleStorage the questions the script API asks. No network, no world.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleStoredItemAuthTestPrivate
{
	const TCHAR* const ItemTruck = TEXT("itm_u0123456789abcdef0123456789abcdef");
	const TCHAR* const ItemGun = TEXT("itm_u11111111111111111111111111111111");
	const TCHAR* const ItemLost = TEXT("itm_u22222222222222222222222222222222");
	const TCHAR* const ItemRevoked = TEXT("itm_u44444444444444444444444444444444");
	const TCHAR* const ItemGadget = TEXT("itm_u66666666666666666666666666666666");
	/** Well formed, resolved by nobody, declared by nobody. */
	const TCHAR* const ItemElsewhere = TEXT("itm_u55555555555555555555555555555555");
	const TCHAR* const AssetTruck = TEXT("ast_uabcdefabcdefabcdefabcdefabcdefab");
	const TCHAR* const AssetGun = TEXT("ast_u33333333333333333333333333333333");
	const TCHAR* const AssetOther = TEXT("ast_u77777777777777777777777777777777");
	const TCHAR* const AssetWeaponMesh = TEXT("ast_u88888888888888888888888888888888");
	const TCHAR* const AssetWeaponAttachment = TEXT("ast_u99999999999999999999999999999999");
	const TCHAR* const AssetWeaponMaterial = TEXT("ast_uaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
	const TCHAR* const AssetWeaponTexture = TEXT("ast_ubbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	const TCHAR* const AssetWeaponSound = TEXT("ast_ucccccccccccccccccccccccccccccccc");

	FMantleStorageEntry Declared(const TCHAR* Name, const TCHAR* Item, const int32 Version)
	{
		FMantleStorageEntry Entry;
		Entry.Name = Name;
		Entry.Item = Item;
		Entry.Version = Version;
		return Entry;
	}

	FMantleResolvedStorageEntry Resolved(
		const TCHAR* Item, const int32 Version, const TCHAR* Kind, const TCHAR* Status, const TCHAR* Primary)
	{
		FMantleResolvedStorageEntry Entry;
		Entry.Item = Item;
		Entry.Version = Version;
		Entry.Kind = Kind;
		Entry.Status = Status;
		Entry.PrimaryAssetId = Primary;
		return Entry;
	}

	/**
	 * A place whose storage declares Truck (a vehicle, version 2), Gun (a
	 * delisted weapon), Lost (the backend lists it missing), Revoked (a status nothing loads under) and Gadget (a
	 * kind that is neither), then expands it as the backend would have answered.
	 */
	FMantlePlace ExpandedPlace()
	{
		FMantlePlace Place;
		Place.PlaceId = TEXT("plc_stored_item_auth_test");
		Place.Storage.Add(Declared(TEXT("Truck"), ItemTruck, 2));
		Place.Storage.Add(Declared(TEXT("Gun"), ItemGun, 1));
		Place.Storage.Add(Declared(TEXT("Lost"), ItemLost, 1));
		Place.Storage.Add(Declared(TEXT("Revoked"), ItemRevoked, 1));
		Place.Storage.Add(Declared(TEXT("Gadget"), ItemGadget, 1));

		FMantleStorageExpansion Expansion;
		Expansion.Entries.Add(Resolved(ItemTruck, 2, TEXT("vehicle"), TEXT("active"), AssetTruck));
		Expansion.Entries.Add(Resolved(ItemGun, 1, TEXT("weapon"), TEXT("delisted"), AssetGun));
		Expansion.Entries.Add(Resolved(ItemRevoked, 1, TEXT("vehicle"), TEXT("revoked"), AssetOther));
		Expansion.Entries.Add(Resolved(ItemGadget, 1, TEXT("gadget"), TEXT("active"), AssetOther));
		FMantleStorageRef Missing;
		Missing.Item = ItemLost;
		Missing.Version = 1;
		Expansion.Missing.Add(Missing);

		FMantleStorage::Apply(Place, Expansion);
		return Place;
	}

	/** One value a script might pass as an item id, with what it is, for the failure message. */
	struct FRefusalCase
	{
		const TCHAR* Label;
		FString Value;
	};

	bool Has(const FMantlePlace& Place, const TCHAR* Item)
	{
		return Place.StorageResolved.ContainsByPredicate([Item](const FMantleResolvedStorageEntry& Entry)
		{
			return Entry.Item.Equals(Item, ESearchCase::CaseSensitive);
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStoredItemRetainTest,
	"Mantle.PlaceStorage.Authorize.RetainsOnlyUsableResolvedItems",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStoredItemRetainTest::RunTest(const FString& Parameters)
{
	using namespace MantleStoredItemAuthTestPrivate;
	const FMantlePlace Place = ExpandedPlace();

	// The vehicle, and the delisted weapon, which still loads.
	TestEqual(TEXT("two items are kept"), Place.StorageResolved.Num(), 2);
	TestTrue(TEXT("the vehicle is kept"), Has(Place, ItemTruck));
	TestTrue(TEXT("the delisted weapon is kept"), Has(Place, ItemGun));
	TestFalse(TEXT("an item the backend lists missing is absent"), Has(Place, ItemLost));
	TestFalse(TEXT("an item of a status nothing loads under is absent"), Has(Place, ItemRevoked));
	TestFalse(TEXT("an item of an unknown kind is absent"), Has(Place, ItemGadget));

	// A place with no expansion at all keeps nothing, and applying again replaces rather than adds.
	FMantlePlace Again = Place;
	FMantleStorage::Apply(Again, FMantleStorageExpansion());
	TestEqual(TEXT("an empty answer keeps nothing"), Again.StorageResolved.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStoredItemFindByNameTest,
	"Mantle.PlaceStorage.Authorize.FindsEntriesByExactName",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStoredItemFindByNameTest::RunTest(const FString& Parameters)
{
	using namespace MantleStoredItemAuthTestPrivate;
	const FMantlePlace Place = ExpandedPlace();

	const FMantleStorageEntry* Truck = FMantleStorage::FindByName(Place, TEXT("Truck"));
	if (TestNotNull(TEXT("a declared name is found"), Truck))
	{
		TestTrue(TEXT("with its item"), Truck->Item.Equals(ItemTruck, ESearchCase::CaseSensitive));
		TestEqual(TEXT("and its pinned version"), Truck->Version, 2);
	}
	TestNull(TEXT("a name differing only in case is another name"), FMantleStorage::FindByName(Place, TEXT("truck")));
	TestNull(TEXT("an undeclared name"), FMantleStorage::FindByName(Place, TEXT("Boat")));
	TestNull(TEXT("an empty name"), FMantleStorage::FindByName(Place, FString()));
	TestNull(TEXT("an item id is not a name"), FMantleStorage::FindByName(Place, ItemTruck));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStoredItemAcceptsTest,
	"Mantle.PlaceStorage.Authorize.AcceptsAStoredItemOfTheRightKind",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStoredItemAcceptsTest::RunTest(const FString& Parameters)
{
	using namespace MantleStoredItemAuthTestPrivate;
	const FMantlePlace Place = ExpandedPlace();

	const FMantleResolvedStorageEntry* Truck = FMantleStorage::FindAuthorized(Place, ItemTruck, FMantleStorage::KindVehicle);
	if (TestNotNull(TEXT("the stored vehicle is authorized as a vehicle"), Truck))
	{
		TestTrue(TEXT("and resolves to its backing asset"), Truck->PrimaryAssetId.Equals(AssetTruck, ESearchCase::CaseSensitive));
	}
	const FMantleResolvedStorageEntry* Gun = FMantleStorage::FindAuthorized(Place, ItemGun, FMantleStorage::KindWeapon);
	if (TestNotNull(TEXT("the stored (delisted) weapon is authorized as a weapon"), Gun))
	{
		TestTrue(TEXT("and resolves to its backing asset"), Gun->PrimaryAssetId.Equals(AssetGun, ESearchCase::CaseSensitive));
	}
	TestTrue(TEXT("nothing is wrong with the vehicle"),
		FMantleStorage::WhyNotAuthorized(Place, ItemTruck, FMantleStorage::KindVehicle).IsEmpty());
	TestTrue(TEXT("nothing is wrong with the weapon"),
		FMantleStorage::WhyNotAuthorized(Place, ItemGun, FMantleStorage::KindWeapon).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStoredItemRefusesTest,
	"Mantle.PlaceStorage.Authorize.RefusesEverythingNotStored",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStoredItemRefusesTest::RunTest(const FString& Parameters)
{
	using namespace MantleStoredItemAuthTestPrivate;
	const FMantlePlace Place = ExpandedPlace();
	const FString Vehicle(FMantleStorage::KindVehicle);
	const FString Weapon(FMantleStorage::KindWeapon);

	// Each of these must be refused as a vehicle AND as a weapon, and must explain itself.
	const TArray<FRefusalCase> Refused = {
		{TEXT("a raw asset id (the vehicle's own backing asset)"), AssetTruck},
		{TEXT("another raw asset id"), AssetOther},
		{TEXT("the built-in name offroad"), TEXT("offroad")},
		{TEXT("the built-in name sports"), TEXT("sports")},
		{TEXT("the built-in name sedan"), TEXT("sedan")},
		{TEXT("a built-in name in capitals"), TEXT("OFFROAD")},
		{TEXT("a built-in tool name"), TEXT("rifle")},
		{TEXT("an object path"), TEXT("/Game/Props/Vehicle/DA_Vehicle.DA_Vehicle")},
		{TEXT("a document id"), TEXT("doc:rifle")},
		{TEXT("a storage name, which Storage:Get must turn into an id first"), TEXT("Truck")},
		{TEXT("an empty string"), FString()},
		{TEXT("an item id with capitals everywhere"), FString(ItemTruck).ToUpper()},
		{TEXT("an item id with a capital prefix"), TEXT("ITM_u0123456789abcdef0123456789abcdef")},
		{TEXT("an item id with one capital hex digit"), TEXT("itm_u0123456789Abcdef0123456789abcdef")},
		{TEXT("an item id one digit short"), FString(ItemTruck).LeftChop(1)},
		{TEXT("an item id with trailing text"), FString(ItemTruck) + TEXT(" ")},
		{TEXT("a well-formed item the place does not store"), ItemElsewhere},
		{TEXT("an item whose entry the backend listed missing"), ItemLost},
		{TEXT("an item whose status nothing loads under"), ItemRevoked},
		{TEXT("an item of an unknown kind"), ItemGadget},
	};
	for (const FRefusalCase& Case : Refused)
	{
		TestNull(FString::Printf(TEXT("%s is no vehicle"), Case.Label), FMantleStorage::FindAuthorized(Place, Case.Value, Vehicle));
		TestNull(FString::Printf(TEXT("%s is no weapon"), Case.Label), FMantleStorage::FindAuthorized(Place, Case.Value, Weapon));
		TestFalse(FString::Printf(TEXT("%s is explained"), Case.Label), FMantleStorage::WhyNotAuthorized(Place, Case.Value, Vehicle).IsEmpty());
	}

	// The wrong kind of a stored, resolved item.
	TestNull(TEXT("a stored weapon is not a vehicle"), FMantleStorage::FindAuthorized(Place, ItemGun, Vehicle));
	TestNull(TEXT("a stored vehicle is not a weapon"), FMantleStorage::FindAuthorized(Place, ItemTruck, Weapon));
	TestNull(TEXT("a kind no item has"), FMantleStorage::FindAuthorized(Place, ItemTruck, TEXT("Vehicle")));

	// An entry in the resolved set that the place's storage does not declare is not an authority.
	FMantlePlace Forged = Place;
	Forged.StorageResolved.Add(Resolved(ItemElsewhere, 1, TEXT("vehicle"), TEXT("active"), AssetOther));
	TestNull(TEXT("a resolved but undeclared item"), FMantleStorage::FindAuthorized(Forged, ItemElsewhere, Vehicle));

	// Only the pinned version authorizes: the item resolved at another version than the one the place declares.
	FMantlePlace OtherVersion = Place;
	OtherVersion.StorageResolved.Reset();
	OtherVersion.StorageResolved.Add(Resolved(ItemTruck, 1, TEXT("vehicle"), TEXT("active"), AssetTruck));
	TestNull(TEXT("an item resolved at a version the place does not pin"), FMantleStorage::FindAuthorized(OtherVersion, ItemTruck, Vehicle));

	// A place that was never expanded (a file load, the plugin preview) authorizes nothing, though it declares items.
	FMantlePlace Unexpanded = Place;
	Unexpanded.StorageResolved.Reset();
	TestNull(TEXT("an unexpanded place"), FMantleStorage::FindAuthorized(Unexpanded, ItemTruck, Vehicle));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStoredItemWhyTest,
	"Mantle.PlaceStorage.Authorize.ExplainsEachRefusal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStoredItemWhyTest::RunTest(const FString& Parameters)
{
	using namespace MantleStoredItemAuthTestPrivate;
	const FMantlePlace Place = ExpandedPlace();
	const FString Vehicle(FMantleStorage::KindVehicle);
	const FString Weapon(FMantleStorage::KindWeapon);

	TestTrue(TEXT("a raw asset id is told to use Storage:Get"),
		FMantleStorage::WhyNotAuthorized(Place, AssetTruck, Vehicle).Contains(TEXT("Storage:Get")));
	TestTrue(TEXT("a built-in name is told it is not an item id"),
		FMantleStorage::WhyNotAuthorized(Place, TEXT("sedan"), Vehicle).Contains(TEXT("not a library item id")));
	TestTrue(TEXT("an unstored item is told so"),
		FMantleStorage::WhyNotAuthorized(Place, ItemElsewhere, Vehicle).Contains(TEXT("not in this place's storage")));
	TestTrue(TEXT("an unresolved item is told it did not load"),
		FMantleStorage::WhyNotAuthorized(Place, ItemLost, Vehicle).Contains(TEXT("did not load")));
	TestTrue(TEXT("a weapon used as a vehicle is told what it is"),
		FMantleStorage::WhyNotAuthorized(Place, ItemGun, Vehicle).Contains(TEXT("is a weapon, not a vehicle")));
	TestTrue(TEXT("a vehicle used as a weapon is told what it is"),
		FMantleStorage::WhyNotAuthorized(Place, ItemTruck, Weapon).Contains(TEXT("is a vehicle, not a weapon")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStoredItemPreparesVehiclesTest,
	"Mantle.PlaceStorage.Authorize.PreparesStoredVehiclesNoSpawnUses",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStoredItemPreparesVehiclesTest::RunTest(const FString& Parameters)
{
	using namespace MantleStoredItemAuthTestPrivate;
	const FMantlePlace Place = ExpandedPlace();

	// The place has no entities, so no VehicleSpawn uses the stored vehicle: it is prepared all the same.
	const TArray<FString> Ids = FMantleStorage::StoredVehicleAssetIds(Place);
	TestEqual(TEXT("one stored vehicle asset (the weapon's is not a vehicle)"), Ids.Num(), 1);
	if (Ids.Num() == 1)
	{
		TestTrue(TEXT("and it is the vehicle's"), Ids[0].Equals(AssetTruck, ESearchCase::CaseSensitive));
	}
	TestTrue(TEXT("a place storing a vehicle has uploaded assets to prepare"), FMantlePlaceAssets::HasUploadedAssets(Place));
	const TArray<FString> WeaponIds = FMantleStorage::StoredWeaponAssetIds(Place);
	TestEqual(TEXT("one stored weapon asset (the vehicle's is not a weapon)"), WeaponIds.Num(), 1);
	if (WeaponIds.Num() == 1)
	{
		TestTrue(TEXT("and it is the weapon's"), WeaponIds[0].Equals(AssetGun, ESearchCase::CaseSensitive));
	}

	// Two versions of one vehicle that share a backing asset are one asset.
	FMantlePlace Twice = Place;
	Twice.StorageResolved.Add(Resolved(ItemElsewhere, 1, TEXT("vehicle"), TEXT("active"), AssetTruck));
	TestEqual(TEXT("a shared backing asset is listed once"), FMantleStorage::StoredVehicleAssetIds(Twice).Num(), 1);

	// A place storing only a weapon has no vehicle to prepare; its stored weapon still needs preparation.
	FMantlePlace WeaponOnly;
	WeaponOnly.PlaceId = Place.PlaceId;
	WeaponOnly.StorageResolved.Add(Resolved(ItemGun, 1, TEXT("weapon"), TEXT("active"), AssetGun));
	TestEqual(TEXT("a weapon is not a stored vehicle"), FMantleStorage::StoredVehicleAssetIds(WeaponOnly).Num(), 0);
	TestEqual(TEXT("a weapon-only place prepares its backing asset"), FMantleStorage::StoredWeaponAssetIds(WeaponOnly).Num(), 1);
	TestTrue(TEXT("a stored weapon with no WeaponSpawn still needs assets prepared"), FMantlePlaceAssets::HasUploadedAssets(WeaponOnly));

	// Two versions of one item may pin the same immutable weapon asset; prepare it once.
	WeaponOnly.StorageResolved.Add(Resolved(ItemElsewhere, 1, TEXT("weapon"), TEXT("delisted"), AssetGun));
	TestEqual(TEXT("a shared weapon backing asset is listed once"), FMantleStorage::StoredWeaponAssetIds(WeaponOnly).Num(), 1);
	WeaponOnly.StorageResolved.Add(Resolved(ItemElsewhere, 1, TEXT("vehicle"), TEXT("active"), AssetOther));
	TestEqual(TEXT("wrong-kind storage is not treated as a stored weapon"), FMantleStorage::StoredWeaponAssetIds(WeaponOnly).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStoredWeaponCoverageTest,
	"Mantle.PlaceStorage.Authorize.StoredOnlyWeaponDependenciesAreCovered",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStoredWeaponCoverageTest::RunTest(const FString& Parameters)
{
	using namespace MantleStoredItemAuthTestPrivate;
	FMantlePlace Place;
	Place.PlaceId = TEXT("plc_stored_weapon_coverage_test");
	Place.StorageResolved.Add(Resolved(ItemGun, 1, TEXT("weapon"), TEXT("active"), AssetGun));

	// The weapon has no spawn or manifest weapon record. The manifest still names its dependencies; a material's
	// texture travels with that material and is not a separate manifest-only asset record.
	for (const TPair<const TCHAR*, EMantleAssetKind>& Dependency : {
		TPair<const TCHAR*, EMantleAssetKind>(AssetWeaponMesh, EMantleAssetKind::StaticMesh),
		TPair<const TCHAR*, EMantleAssetKind>(AssetWeaponAttachment, EMantleAssetKind::StaticMesh),
		TPair<const TCHAR*, EMantleAssetKind>(AssetWeaponMaterial, EMantleAssetKind::Material),
		TPair<const TCHAR*, EMantleAssetKind>(AssetWeaponTexture, EMantleAssetKind::Texture),
		TPair<const TCHAR*, EMantleAssetKind>(AssetWeaponSound, EMantleAssetKind::Sound)})
	{
		FMantleAssetRef Ref;
		Ref.Id = Dependency.Key;
		Ref.Kind = Dependency.Value;
		Place.Assets.Add(MoveTemp(Ref));
	}

	const TArray<FString> NamedWeaponDependencies = {
		AssetWeaponMesh, AssetWeaponAttachment, AssetWeaponMaterial, AssetWeaponSound};
	auto ReadWeaponDependencies = [&NamedWeaponDependencies](const FString& WeaponId, TSet<FString>& Used)
	{
		if (WeaponId == AssetGun)
		{
			for (const FString& Dependency : NamedWeaponDependencies)
			{
				Used.Add(Dependency);
			}
		}
	};

	TSet<FString> Used = MantlePlaceAssetsPrivate::UploadedIdsPlaceSetTakes(Place, ReadWeaponDependencies);
	TestTrue(TEXT("the stored weapon document itself is covered"), Used.Contains(AssetGun));
	for (const FString& Dependency : NamedWeaponDependencies)
	{
		TestTrue(FString::Printf(TEXT("stored-only weapon dependency %s is covered"), *Dependency), Used.Contains(Dependency));
	}
	TestFalse(TEXT("the material's texture is not a separate manifest-only dependency"), Used.Contains(AssetWeaponTexture));

	Place.StorageResolved.Reset();
	Used = MantlePlaceAssetsPrivate::UploadedIdsPlaceSetTakes(Place, ReadWeaponDependencies);
	TestFalse(TEXT("the weapon document is no longer covered after its storage entry is removed"), Used.Contains(AssetGun));
	for (const FString& Dependency : NamedWeaponDependencies)
	{
		TestFalse(FString::Printf(TEXT("dependency %s is no longer covered when the stored weapon is removed"), *Dependency),
			Used.Contains(Dependency));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStoredItemTakenDownTest,
	"Mantle.PlaceStorage.Authorize.ATakenDownItemIsExplainedAndNeverAuthorized",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStoredItemTakenDownTest::RunTest(const FString& Parameters)
{
	using namespace MantleStoredItemAuthTestPrivate;
	const FString Vehicle(FMantleStorage::KindVehicle);

	// The answer names the item as taken_down; a takedown overrides the place's pin.
	FMantlePlace Place;
	Place.PlaceId = TEXT("plc_taken_down_test");
	Place.Storage.Add(Declared(TEXT("Truck"), ItemTruck, 2));
	Place.Storage.Add(Declared(TEXT("Gun"), ItemGun, 1));
	FMantleStorageExpansion Expansion;
	Expansion.Entries.Add(Resolved(ItemTruck, 2, TEXT("vehicle"), TEXT("taken_down"), AssetTruck));
	Expansion.Entries.Add(Resolved(ItemGun, 1, TEXT("weapon"), TEXT("active"), AssetGun));
	FMantleStorage::Apply(Place, Expansion);

	TestFalse(TEXT("the taken-down item is not kept as resolved"), Has(Place, ItemTruck));
	TestTrue(TEXT("the other item is"), Has(Place, ItemGun));
	TestNull(TEXT("the taken-down item authorizes nothing"), FMantleStorage::FindAuthorized(Place, ItemTruck, Vehicle));
	TestEqual(TEXT("it is remembered as taken down"), Place.StorageTakenDown.Num(), 1);

	const FString Why = FMantleStorage::WhyNotAuthorized(Place, ItemTruck, Vehicle);
	TestTrue(TEXT("the script error says the item was taken down"), Why.Contains(TEXT("was taken down")));
	TestFalse(TEXT("and not the generic did-not-load text"), Why.Contains(TEXT("did not load")));
	TestTrue(TEXT("it names the item"), Why.Contains(ItemTruck));
	TestTrue(TEXT("an untouched item still authorizes"), FMantleStorage::WhyNotAuthorized(Place, ItemGun, FString(FMantleStorage::KindWeapon)).IsEmpty());

	// An answer that omits the kind and assets of a taken-down item still reads; the same omission on an active one does not.
	FMantleStorageExpansion Parsed;
	TestTrue(TEXT("a taken-down entry without kind or assets is read"), FMantleStorage::ParseResponse(
		FString::Printf(TEXT("{\"entries\":[{\"item\":\"%s\",\"version\":2,\"status\":\"taken_down\"}]}"), ItemTruck), Parsed));
	TestEqual(TEXT("as one entry"), Parsed.Entries.Num(), 1);
	TestFalse(TEXT("an active entry without them is refused"), FMantleStorage::ParseResponse(
		FString::Printf(TEXT("{\"entries\":[{\"item\":\"%s\",\"version\":2,\"status\":\"active\"}]}"), ItemTruck), Parsed));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
