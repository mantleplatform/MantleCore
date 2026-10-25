// Copyright Joshua Eger. All Rights Reserved.
#include "MantleLibraryPublish.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceTypes.h"
#include "MantleWeaponSpawnPoint.h"

#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// The editor's half of the Weapon Spawn: a spawn that names a built weapon exports as a WeaponSpawn that
// names the weapon's upload (and lists it in assets[]), one with no weapon is refused in plain words, the entity writes
// and parses back, and a published weapon's library item turns its spawns into storage references. Pure or on a
// transient editor world; the weapon builder's files and the upload itself are the end-to-end check's.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleWeaponSpawnPublishTestPrivate
{
	const TCHAR* const AssetRifle = TEXT("ast_u11111111111111111111111111111111");
	const TCHAR* const AssetTruck = TEXT("ast_u22222222222222222222222222222222");
	const TCHAR* const AssetMesh = TEXT("ast_u33333333333333333333333333333333");

	/** An empty editor world for one test, destroyed with it. */
	struct FWeaponSpawnWorld
	{
		UWorld* World = nullptr;

		FWeaponSpawnWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleWeaponSpawnPublishTestWorld"));
		}

		~FWeaponSpawnWorld()
		{
			if (World)
			{
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
				if (World->IsRooted())
				{
					World->RemoveFromRoot();
				}
			}
		}
	};

	AMantleWeaponSpawnPoint* SpawnWeaponSpawn(UWorld* World, const TCHAR* Label, const FString& Weapon)
	{
		AMantleWeaponSpawnPoint* Spawn = World->SpawnActor<AMantleWeaponSpawnPoint>(FVector(10.0, 20.0, 30.0), FRotator::ZeroRotator);
		if (Spawn)
		{
			Spawn->SetActorLabel(Label);
			Spawn->Weapon = Weapon;
		}
		return Spawn;
	}

	FMantleEntity MakeWeaponSpawn(const TCHAR* Id, const FString& Weapon, const FString& StorageItem)
	{
		FMantleWeaponSpawnProps Props;
		Props.Weapon = Weapon;
		Props.StorageItem = StorageItem;
		Props.RespawnSeconds = 25.0;
		FMantleEntity Entity;
		Entity.Id = Id;
		Entity.Name = Id;
		Entity.Type = EMantleEntityType::WeaponSpawn;
		Entity.Props.Set<FMantleWeaponSpawnProps>(MoveTemp(Props));
		return Entity;
	}

	TSharedPtr<FJsonObject> ReadObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Root;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root);
		return Root;
	}

	TSharedPtr<FJsonObject> PropertiesOf(const TSharedPtr<FJsonObject>& Root, const int32 Index)
	{
		const TArray<TSharedPtr<FJsonValue>>* Entities = nullptr;
		if (!Root.IsValid() || !Root->TryGetArrayField(TEXT("entities"), Entities) || !Entities->IsValidIndex(Index))
		{
			return nullptr;
		}
		return (*Entities)[Index]->AsObject()->GetObjectField(TEXT("properties"));
	}

	/** A place file text with a weapon spawn, a vehicle spawn and both uploads in assets[]. */
	FString MakePlaceText()
	{
		return FString::Printf(
			TEXT("{\"schemaVersion\":1,\"assets\":[{\"id\":\"%s\",\"kind\":\"Weapon\",\"hash\":\"sha256:aa\",\"bytes\":10},")
			TEXT("{\"id\":\"%s\",\"kind\":\"Vehicle\",\"hash\":\"sha256:bb\",\"bytes\":12},")
			TEXT("{\"id\":\"%s\",\"kind\":\"SkeletalMesh\",\"hash\":\"sha256:cc\",\"bytes\":3}],\"entities\":[")
			TEXT("{\"id\":\"w\",\"type\":\"WeaponSpawn\",\"properties\":{\"weapon\":\"%s\",\"respawnSeconds\":10,\"enabled\":true}},")
			TEXT("{\"id\":\"v\",\"type\":\"VehicleSpawn\",\"properties\":{\"vehicle\":\"%s\",\"respawnSeconds\":10,\"enabled\":true}}]}"),
			AssetRifle, AssetTruck, AssetMesh, AssetRifle, AssetTruck);
	}

	FMantleLibraryVehicleRef MakeRef(const TCHAR* AssetId, const TCHAR* Name, const int32 Number)
	{
		FMantleLibraryVehicleRef Ref;
		Ref.AssetId = AssetId;
		Ref.Name = Name;
		Ref.ItemId = FString::Printf(TEXT("itm_u%032x"), Number);
		Ref.Version = 1;
		return Ref;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponSpawnExportsChosenWeaponTest,
	"Mantle.WeaponSpawn.Editor.Export.AChosenWeaponNamesItsUpload",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponSpawnExportsChosenWeaponTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponSpawnPublishTestPrivate;
	FWeaponSpawnWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	AMantleWeaponSpawnPoint* Spawn = SpawnWeaponSpawn(Fixture.World, TEXT("Rifle rack"), TEXT("My_Rifle"));
	if (!TestNotNull(TEXT("the spawn"), Spawn))
	{
		return false;
	}
	Spawn->RespawnSeconds = 42.0;
	Spawn->bEnabled = false;
	TestTrue(TEXT("the exporter classifies it as a WeaponSpawn"), [Spawn]()
	{
		EMantleEntityType Type = EMantleEntityType::Group;
		return FMantlePlaceExporter::ClassifyActor(Spawn, Type) && Type == EMantleEntityType::WeaponSpawn;
	}());

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestFalse(TEXT("the export does not refuse the spawn"), Result.Errors.ContainsByPredicate([](const FMantleParseError& Error) { return Error.Path.Contains(TEXT("weapon")); }));
	const FMantleEntity* Entity = Result.Place.Entities.FindByPredicate([](const FMantleEntity& Candidate) { return Candidate.Type == EMantleEntityType::WeaponSpawn; });
	if (!TestNotNull(TEXT("a WeaponSpawn entity"), Entity))
	{
		return false;
	}
	const FMantleWeaponSpawnProps* Props = Entity->Props.TryGet<FMantleWeaponSpawnProps>();
	if (!TestNotNull(TEXT("with weapon properties"), Props))
	{
		return false;
	}
	const FString Expected = FMantlePlaceAssets::ProvisionalId(FMantlePlaceAssets::WeaponKey(TEXT("My_Rifle")));
	TestEqual(TEXT("it names the weapon's upload"), Props->Weapon, Expected);
	TestTrue(TEXT("and no storage item"), Props->StorageItem.IsEmpty());
	TestEqual(TEXT("with the respawn time"), Props->RespawnSeconds, 42.0);
	TestFalse(TEXT("and the Enabled box"), Props->bEnabled);
	TestTrue(TEXT("the weapon is listed in assets[] as a Weapon"), Result.Place.Assets.ContainsByPredicate(
		[&Expected](const FMantleAssetRef& Ref) { return Ref.Id == Expected && Ref.Kind == EMantleAssetKind::Weapon; }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponSpawnExportsNoWeaponTest,
	"Mantle.WeaponSpawn.Editor.Export.ASpawnWithNoWeaponIsRefusedInPlainWords",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponSpawnExportsNoWeaponTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponSpawnPublishTestPrivate;
	FWeaponSpawnWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World) || !TestNotNull(TEXT("the spawn"), SpawnWeaponSpawn(Fixture.World, TEXT("Empty rack"), FString())))
	{
		return false;
	}
	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	const FMantleParseError* Refusal = Result.Errors.FindByPredicate([](const FMantleParseError& Error) { return Error.Path.EndsWith(TEXT("/properties/weapon")); });
	if (TestNotNull(TEXT("the export refuses it"), Refusal))
	{
		TestTrue(TEXT("naming the spawn"), Refusal->Message.Contains(TEXT("Empty rack")));
		TestTrue(TEXT("and saying what to do, with no ids or schema words"), Refusal->Message.Contains(TEXT("pick one in its Weapon list")) && !Refusal->Message.Contains(TEXT("ast_u")));
	}
	TestFalse(TEXT("so the export does not succeed"), Result.bSuccess);

	// A name that could leave the weapon folder is refused too, not looked up.
	FWeaponSpawnWorld Second;
	if (Second.World && SpawnWeaponSpawn(Second.World, TEXT("Odd rack"), TEXT("..\\Elsewhere")))
	{
		const FMantleExportResult Odd = FMantlePlaceExporter::Export(Second.World, FMantleExportOptions());
		TestTrue(TEXT("a weapon name outside the builder's folder is refused"),
			Odd.Errors.ContainsByPredicate([](const FMantleParseError& Error) { return Error.Path.EndsWith(TEXT("/properties/weapon")); }));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponSpawnWritesAndParsesTest,
	"Mantle.WeaponSpawn.Editor.Export.WritesAndReadsBack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponSpawnWritesAndParsesTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponSpawnPublishTestPrivate;
	FMantlePlace Place;
	Place.SchemaVersion = MANTLE_PLACE_SCHEMA_VERSION;
	Place.PlaceId = TEXT("plc_weapon_roundtrip");
	Place.Revision.Number = 1;
	Place.Revision.State = EMantleRevisionState::Draft;
	Place.Metadata.Name = TEXT("Weapon round trip");
	Place.Metadata.AuthorId = TEXT("usr_test");
	Place.Entities.Add(MakeWeaponSpawn(TEXT("direct"), AssetRifle, FString()));
	Place.Entities.Add(MakeWeaponSpawn(TEXT("stored"), FString(), TEXT("Rifle")));
	FMantleStorageEntry Entry;
	Entry.Name = TEXT("Rifle");
	Entry.Item = TEXT("itm_u0123456789abcdef0123456789abcdef");
	Entry.Version = 2;
	Place.Storage.Add(Entry);

	const FString Json = FMantlePlaceExporter::ToJson(Place);
	const TSharedPtr<FJsonObject> Root = ReadObject(Json);
	const TSharedPtr<FJsonObject> Direct = PropertiesOf(Root, 0);
	const TSharedPtr<FJsonObject> Stored = PropertiesOf(Root, 1);
	if (!TestTrue(TEXT("both spawns write properties"), Direct.IsValid() && Stored.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("a direct spawn writes weapon"), Direct->GetStringField(TEXT("weapon")), FString(AssetRifle));
	TestFalse(TEXT("and no storageItem"), Direct->HasField(TEXT("storageItem")));
	TestEqual(TEXT("a stored spawn writes storageItem"), Stored->GetStringField(TEXT("storageItem")), FString(TEXT("Rifle")));
	TestFalse(TEXT("and no weapon"), Stored->HasField(TEXT("weapon")));
	TestEqual(TEXT("respawnSeconds"), Direct->GetNumberField(TEXT("respawnSeconds")), 25.0);
	TestTrue(TEXT("enabled"), Direct->GetBoolField(TEXT("enabled")));

	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
	if (!TestTrue(TEXT("what the exporter wrote parses"), Parsed.bSuccess))
	{
		AddError(Parsed.ErrorSummary());
		return false;
	}
	const FMantleWeaponSpawnProps* ReadDirect = Parsed.Place.Entities[0].Props.TryGet<FMantleWeaponSpawnProps>();
	const FMantleWeaponSpawnProps* ReadStored = Parsed.Place.Entities[1].Props.TryGet<FMantleWeaponSpawnProps>();
	if (TestNotNull(TEXT("direct"), ReadDirect) && TestNotNull(TEXT("stored"), ReadStored))
	{
		TestTrue(TEXT("the direct spawn keeps its weapon"), ReadDirect->Weapon.Equals(AssetRifle, ESearchCase::CaseSensitive));
		TestEqual(TEXT("the stored spawn keeps its storage name"), ReadStored->StorageItem, FString(TEXT("Rifle")));
		TestEqual(TEXT("and the respawn time"), ReadDirect->RespawnSeconds, 25.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponSpawnLibraryRewriteTest,
	"Mantle.WeaponSpawn.Editor.LibraryPublish.SpawnsNameStorageInsteadOfTheUpload",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponSpawnLibraryRewriteTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponSpawnPublishTestPrivate;
	const FString Json = MakePlaceText();
	const TArray<FString> WeaponIds = FMantleLibraryPublish::WeaponAssetIdsIn(Json);
	const TArray<FString> VehicleIds = FMantleLibraryPublish::VehicleAssetIdsIn(Json);
	TestTrue(TEXT("the weapon ids the place uses are the rifle's"), WeaponIds.Num() == 1 && WeaponIds[0].Equals(AssetRifle, ESearchCase::CaseSensitive));
	TestTrue(TEXT("the vehicle ids are only the vehicle's"), VehicleIds.Num() == 1 && VehicleIds[0].Equals(AssetTruck, ESearchCase::CaseSensitive));

	const TArray<FMantleLibraryVehicleRef> Refs = { MakeRef(AssetRifle, TEXT("My_Rifle"), 1), MakeRef(AssetTruck, TEXT("SKM_Truck_Vehicle"), 2) };
	FString Out;
	FString Error;
	TArray<FMantleStoredVehicle> Stored;
	if (!TestTrue(TEXT("the weapon rewrite succeeds"), FMantleLibraryPublish::RewriteWeaponSpawns(Json, Refs, Out, Error, &Stored)))
	{
		AddError(Error);
		return false;
	}
	const TSharedPtr<FJsonObject> Root = ReadObject(Out);
	const TSharedPtr<FJsonObject> Weapon = PropertiesOf(Root, 0);
	const TSharedPtr<FJsonObject> Vehicle = PropertiesOf(Root, 1);
	if (!TestTrue(TEXT("both spawns are still there"), Weapon.IsValid() && Vehicle.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("the weapon spawn names its storage entry"), Weapon->GetStringField(TEXT("storageItem")), FString(TEXT("My_Rifle")));
	TestFalse(TEXT("and no longer the upload"), Weapon->HasField(TEXT("weapon")));
	TestEqual(TEXT("the vehicle spawn keeps its upload: it is not this rewrite's"), Vehicle->GetStringField(TEXT("vehicle")), FString(AssetTruck));
	if (TestEqual(TEXT("one weapon was stored"), Stored.Num(), 1))
	{
		TestEqual(TEXT("it is the rifle"), Stored[0].AssetId, FString(AssetRifle));
		TestEqual(TEXT("under the name the spawn uses"), Stored[0].StorageName, FString(TEXT("My_Rifle")));
	}
	const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
	if (TestTrue(TEXT("assets stay"), Root->TryGetArrayField(TEXT("assets"), Assets)))
	{
		TestEqual(TEXT("the weapon upload leaves assets[], its mesh and the vehicle stay"), Assets->Num(), 2);
	}
	const TArray<TSharedPtr<FJsonValue>>* Storage = nullptr;
	if (TestTrue(TEXT("storage is written"), Root->TryGetArrayField(TEXT("storage"), Storage)) && TestEqual(TEXT("one entry"), Storage->Num(), 1))
	{
		TestEqual(TEXT("the entry is the rifle's item"), (*Storage)[0]->AsObject()->GetStringField(TEXT("item")), Refs[0].ItemId);
	}

	// The vehicle rewrite on the same refs leaves the weapon spawn alone.
	FString VehicleOut;
	TArray<FMantleStoredVehicle> StoredVehicles;
	if (TestTrue(TEXT("the vehicle rewrite succeeds"), FMantleLibraryPublish::RewritePlace(Json, Refs, VehicleOut, Error, &StoredVehicles)))
	{
		const TSharedPtr<FJsonObject> VehicleRoot = ReadObject(VehicleOut);
		TestEqual(TEXT("the weapon spawn keeps its upload there"), PropertiesOf(VehicleRoot, 0)->GetStringField(TEXT("weapon")), FString(AssetRifle));
		TestEqual(TEXT("only the vehicle is stored"), StoredVehicles.Num(), 1);
	}

	// The item is made as a weapon.
	const TSharedPtr<FJsonObject> Body = ReadObject(FMantleLibraryPublish::MakeItemBody(TEXT("My_Rifle"), false, AssetRifle, FMantleProvenance(), TEXT("weapon")));
	if (TestTrue(TEXT("the item body is JSON"), Body.IsValid()))
	{
		TestEqual(TEXT("its kind is weapon"), Body->GetStringField(TEXT("kind")), FString(TEXT("weapon")));
		TestEqual(TEXT("and its primary asset is the upload"), Body->GetStringField(TEXT("primaryAssetId")), FString(AssetRifle));
	}
	TestEqual(TEXT("a vehicle item is still made as a vehicle"),
		ReadObject(FMantleLibraryPublish::MakeItemBody(TEXT("Truck"), false, AssetTruck))->GetStringField(TEXT("kind")), FString(TEXT("vehicle")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
