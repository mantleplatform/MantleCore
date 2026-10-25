// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceExporter.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceStorage.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// The exporter's half of place storage: storage[] and a VehicleSpawn's storageItem go out only when
// used, and what goes out parses back equal.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePlaceStorageExporterTestPrivate
{
	const TCHAR* const ItemTruck = TEXT("itm_u0123456789abcdef0123456789abcdef");
	const TCHAR* const ItemBus = TEXT("itm_u11111111111111111111111111111111");
	const TCHAR* const AssetOwn = TEXT("ast_u33333333333333333333333333333333");

	FMantleStorageEntry MakeEntry(const TCHAR* Name, const TCHAR* Item, const int32 Version)
	{
		FMantleStorageEntry Entry;
		Entry.Name = Name;
		Entry.Item = Item;
		Entry.Version = Version;
		return Entry;
	}

	FMantleEntity MakeSpawn(const TCHAR* Id, const FString& Vehicle, const FString& StorageItem)
	{
		FMantleVehicleSpawnProps Props;
		Props.Vehicle = Vehicle;
		Props.StorageItem = StorageItem;
		FMantleEntity Entity;
		Entity.Id = Id;
		Entity.Name = Id;
		Entity.Type = EMantleEntityType::VehicleSpawn;
		Entity.Props.Set<FMantleVehicleSpawnProps>(MoveTemp(Props));
		return Entity;
	}

	/** A place with a stored vehicle spawn and a direct one; Storage is left for the caller. */
	FMantlePlace MakePlace()
	{
		FMantlePlace Place;
		Place.SchemaVersion = MANTLE_PLACE_SCHEMA_VERSION;
		Place.PlaceId = TEXT("plc_storage_roundtrip");
		Place.Revision.Number = 1;
		Place.Revision.State = EMantleRevisionState::Draft;
		Place.Metadata.Name = TEXT("Storage round trip");
		Place.Metadata.AuthorId = TEXT("usr_test");
		Place.Entities.Add(MakeSpawn(TEXT("stored"), FString(), TEXT("Truck")));
		Place.Entities.Add(MakeSpawn(TEXT("direct"), AssetOwn, FString()));
		return Place;
	}

	TSharedPtr<FJsonObject> ReadObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Root;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root);
		return Root;
	}

	/** The properties object of the entity at Index. */
	TSharedPtr<FJsonObject> PropertiesOf(const TSharedPtr<FJsonObject>& Root, const int32 Index)
	{
		const TArray<TSharedPtr<FJsonValue>>* Entities = nullptr;
		if (!Root->TryGetArrayField(TEXT("entities"), Entities) || !Entities->IsValidIndex(Index))
		{
			return nullptr;
		}
		return (*Entities)[Index]->AsObject()->GetObjectField(TEXT("properties"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceStorageExporterRoundTripTest,
	"Mantle.PlaceStorage.Exporter.RoundTripsStorage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceStorageExporterRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceStorageExporterTestPrivate;
	FMantlePlace Place = MakePlace();
	Place.Storage.Add(MakeEntry(TEXT("Truck"), ItemTruck, 3));
	Place.Storage.Add(MakeEntry(TEXT("Bus"), ItemBus, 1));

	const FString Json = FMantlePlaceExporter::ToJson(Place);
	const TSharedPtr<FJsonObject> Root = ReadObject(Json);
	if (!TestTrue(TEXT("the place writes valid JSON"), Root.IsValid()))
	{
		return false;
	}

	// The wire shape: storage[] of {name, item, version}, and storageItem in place of vehicle.
	const TArray<TSharedPtr<FJsonValue>>* Storage = nullptr;
	if (TestTrue(TEXT("a place that stores items writes storage"), Root->TryGetArrayField(TEXT("storage"), Storage))
		&& TestEqual(TEXT("one object per entry"), Storage->Num(), 2))
	{
		const TSharedPtr<FJsonObject> First = (*Storage)[0]->AsObject();
		TestEqual(TEXT("name"), First->GetStringField(TEXT("name")), FString(TEXT("Truck")));
		TestEqual(TEXT("item"), First->GetStringField(TEXT("item")), FString(ItemTruck));
		TestEqual(TEXT("version"), static_cast<int32>(First->GetNumberField(TEXT("version"))), 3);
		TestEqual(TEXT("and no other key"), First->Values.Num(), 3);
	}
	const TSharedPtr<FJsonObject> StoredProps = PropertiesOf(Root, 0);
	const TSharedPtr<FJsonObject> DirectProps = PropertiesOf(Root, 1);
	if (TestTrue(TEXT("both spawns write properties"), StoredProps.IsValid() && DirectProps.IsValid()))
	{
		TestTrue(TEXT("a stored spawn writes storageItem"), StoredProps->HasField(TEXT("storageItem")));
		TestFalse(TEXT("and no vehicle"), StoredProps->HasField(TEXT("vehicle")));
		TestTrue(TEXT("a direct spawn writes vehicle"), DirectProps->HasField(TEXT("vehicle")));
		TestFalse(TEXT("and no storageItem"), DirectProps->HasField(TEXT("storageItem")));
	}

	// A raw uploaded vehicle id is the exporter's draft form, which publish turns into a stored item:
	// the parser, which reads what players are sent, refuses it in plain words.
	const FMantleParseResult Draft = FMantlePlaceParser::ParseFromString(Json);
	TestFalse(TEXT("a place still holding a raw vehicle id does not parse"), Draft.bSuccess);
	TestTrue(TEXT("and the refusal names the spawn's vehicle"), Draft.Errors.ContainsByPredicate([](const FMantleParseError& Error)
	{
		return Error.Path == TEXT("/entities/1/properties/vehicle") && Error.Message.Contains(TEXT("storage"));
	}));

	// Once only stored spawns remain, it reads back equal.
	Place.Entities.RemoveAt(1);
	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(FMantlePlaceExporter::ToJson(Place));
	if (!TestTrue(TEXT("what the exporter wrote parses"), Parsed.bSuccess))
	{
		AddError(Parsed.ErrorSummary());
		return false;
	}
	TestTrue(TEXT("storage is equal after the round trip"), Parsed.Place.Storage == Place.Storage);
	const FMantleVehicleSpawnProps* Stored = Parsed.Place.Entities[0].Props.TryGet<FMantleVehicleSpawnProps>();
	if (TestNotNull(TEXT("stored spawn"), Stored))
	{
		TestEqual(TEXT("the stored spawn keeps its storage name"), Stored->StorageItem, FString(TEXT("Truck")));
		TestTrue(TEXT("and has no vehicle"), Stored->Vehicle.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceStorageExporterOmitsTest,
	"Mantle.PlaceStorage.Exporter.OmitsStorageWhenUnused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceStorageExporterOmitsTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceStorageExporterTestPrivate;
	// A place that uses no storage stays byte-identical to what older exporters wrote: no storage key, and a
	// spawn's vehicle key and nothing else new.
	FMantlePlace Place = MakePlace();
	Place.Entities.RemoveAt(0);
	const FString Json = FMantlePlaceExporter::ToJson(Place);
	const TSharedPtr<FJsonObject> Root = ReadObject(Json);
	if (!TestTrue(TEXT("the place writes valid JSON"), Root.IsValid()))
	{
		return false;
	}
	TestFalse(TEXT("no storage key"), Root->HasField(TEXT("storage")));
	TestFalse(TEXT("no storageItem anywhere in the text"), Json.Contains(TEXT("storageItem")));
	if (const TSharedPtr<FJsonObject> Props = PropertiesOf(Root, 0))
	{
		TestTrue(TEXT("the spawn still writes vehicle"), Props->HasField(TEXT("vehicle")));
	}

	// ... which is the draft form: the parser refuses a raw vehicle id, publish stores the vehicle first.
	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
	TestFalse(TEXT("and the parser refuses it"), Parsed.bSuccess);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
