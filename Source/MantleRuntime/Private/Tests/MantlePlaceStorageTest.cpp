// Copyright Joshua Eger. All Rights Reserved.
#include "MantleApiClient.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceStorage.h"

#include "Dom/JsonObject.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// Place storage: the parser's storage and storageItem rules, the resolve answer's reader, and the
// rewrite that gives a stored vehicle spawn its resolved asset id.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantlePlaceStorageTestPrivate
{
	const TCHAR* const ItemTruck = TEXT("itm_u0123456789abcdef0123456789abcdef");
	const TCHAR* const ItemGun = TEXT("itm_u11111111111111111111111111111111");
	const TCHAR* const ItemLost = TEXT("itm_u22222222222222222222222222222222");
	const TCHAR* const AssetTruck = TEXT("ast_uabcdefabcdefabcdefabcdefabcdefab");
	const TCHAR* const AssetOwn = TEXT("ast_u33333333333333333333333333333333");

	/** A VehicleSpawn entity whose properties are Properties (the text between the braces). */
	FString SpawnEntity(const TCHAR* Id, const FString& Properties)
	{
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"type\":\"VehicleSpawn\",\"name\":\"%s\",\"parent\":null,")
				TEXT("\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},")
				TEXT("\"properties\":{%s}}"),
			Id, Id, *Properties);
	}

	/** A complete place around Entities (comma-separated entity objects) and, when not empty, a storage array. */
	FString PlaceJson(const FString& Entities, const FString& Storage)
	{
		const FString StorageKey = Storage.IsEmpty() ? FString() : TEXT(",\"storage\":") + Storage;
		return FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_storage_test\",\"revision\":{\"number\":1,\"state\":\"draft\"},")
				TEXT("\"metadata\":{\"name\":\"Storage test\",\"authorId\":\"usr_test\"},\"settings\":{},\"assets\":[],\"entities\":[%s]%s}"),
			*Entities, *StorageKey);
	}

	FString StorageEntry(const TCHAR* Name, const TCHAR* Item, const int32 Version)
	{
		return FString::Printf(TEXT("{\"name\":\"%s\",\"item\":\"%s\",\"version\":%d}"), Name, Item, Version);
	}

	bool HasErrorAt(const FMantleParseResult& Result, const FString& Path)
	{
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error)
		{
			return Error.Path.Equals(Path, ESearchCase::CaseSensitive);
		});
	}

	const FMantleVehicleSpawnProps* SpawnAt(const FMantlePlace& Place, const int32 Index)
	{
		return Place.Entities.IsValidIndex(Index) ? Place.Entities[Index].Props.TryGet<FMantleVehicleSpawnProps>() : nullptr;
	}

	/** The resolve answer for Truck (a vehicle), Gun (a weapon) and Lost (missing), as the backend sends it. */
	FString ResolveAnswerJson()
	{
		return FString::Printf(
			TEXT("{\"entries\":[")
				TEXT("{\"name\":\"Truck\",\"item\":\"%s\",\"version\":2,\"kind\":\"vehicle\",\"status\":\"active\",")
				TEXT("\"primaryAssetId\":\"%s\",\"dependencyAssetIds\":[\"%s\",\"%s\"]},")
				TEXT("{\"name\":\"Gun\",\"item\":\"%s\",\"version\":1,\"kind\":\"weapon\",\"status\":\"delisted\",")
				TEXT("\"primaryAssetId\":\"%s\",\"dependencyAssetIds\":[]}],")
				TEXT("\"missing\":[{\"item\":\"%s\",\"version\":1}]}"),
			ItemTruck, AssetTruck, AssetTruck, AssetOwn, ItemGun, AssetOwn, ItemLost);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceStorageParserAcceptsTest,
	"Mantle.PlaceStorage.Parser.AcceptsStorageAndStorageItem",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceStorageParserAcceptsTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceStorageTestPrivate;
	const FString Storage = TEXT("[") + StorageEntry(TEXT("Truck"), ItemTruck, 2) + TEXT(",") + StorageEntry(TEXT("truck"), ItemGun, 1) + TEXT("]");
	const FString Entities = SpawnEntity(TEXT("a"), TEXT("\"storageItem\":\"Truck\",\"respawnSeconds\":3"))
		+ TEXT(",") + SpawnEntity(TEXT("b"), TEXT("\"storageItem\":\"truck\""));

	const FMantleParseResult Result = FMantlePlaceParser::ParseFromString(PlaceJson(Entities, Storage));
	if (!TestTrue(TEXT("a place with storage parses"), Result.bSuccess))
	{
		AddError(Result.ErrorSummary());
		return false;
	}

	// Names are case-sensitive: 'Truck' and 'truck' are two entries.
	if (TestEqual(TEXT("two storage entries"), Result.Place.Storage.Num(), 2))
	{
		TestEqual(TEXT("first name"), Result.Place.Storage[0].Name, FString(TEXT("Truck")));
		TestTrue(TEXT("first item"), Result.Place.Storage[0].Item.Equals(ItemTruck, ESearchCase::CaseSensitive));
		TestEqual(TEXT("first version"), Result.Place.Storage[0].Version, 2);
		TestEqual(TEXT("second name"), Result.Place.Storage[1].Name, FString(TEXT("truck")));
	}

	const FMantleVehicleSpawnProps* Stored = SpawnAt(Result.Place, 0);
	const FMantleVehicleSpawnProps* Lower = SpawnAt(Result.Place, 1);
	if (TestNotNull(TEXT("stored spawn"), Stored) && TestNotNull(TEXT("second stored spawn"), Lower))
	{
		TestEqual(TEXT("the stored spawn names its entry"), Stored->StorageItem, FString(TEXT("Truck")));
		TestTrue(TEXT("and has no vehicle until storage expands"), Stored->Vehicle.IsEmpty());
		TestEqual(TEXT("its other properties still read"), Stored->RespawnSeconds, 3.0);
		TestEqual(TEXT("the second spawn names the other entry (names are case-sensitive)"), Lower->StorageItem, FString(TEXT("truck")));
	}

	// No storage key at all stays valid, and reads as no entries.
	const FMantleParseResult Without = FMantlePlaceParser::ParseFromString(PlaceJson(FString(), FString()));
	TestTrue(TEXT("a place without storage parses"), Without.bSuccess);
	TestEqual(TEXT("and has none"), Without.Place.Storage.Num(), 0);

	// An empty array is valid too.
	TestTrue(TEXT("an empty storage array parses"),
		FMantlePlaceParser::ParseFromString(PlaceJson(FString(), TEXT("[]"))).bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceStorageParserRefusesTest,
	"Mantle.PlaceStorage.Parser.RefusesInvalidStorage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceStorageParserRefusesTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceStorageTestPrivate;
	const FString StoredSpawn = SpawnEntity(TEXT("a"), TEXT("\"storageItem\":\"Truck\""));
	const FString OneEntry = TEXT("[") + StorageEntry(TEXT("Truck"), ItemTruck, 1) + TEXT("]");

	// The base place, with a valid stored spawn and storage, must parse: otherwise a refusal below could be for
	// something other than the storage rule under test.
	const FMantleParseResult Base = FMantlePlaceParser::ParseFromString(PlaceJson(StoredSpawn, OneEntry));
	if (!TestTrue(TEXT("the base place parses before any case mutates it"), Base.bSuccess))
	{
		AddError(Base.ErrorSummary());
		return false;
	}

	const auto ExpectRefused = [this](const TCHAR* Case, const FString& Json, const FString& Path)
	{
		const FMantleParseResult Result = FMantlePlaceParser::ParseFromString(Json);
		TestFalse(FString::Printf(TEXT("%s is refused"), Case), Result.bSuccess);
		TestTrue(FString::Printf(TEXT("%s is reported at %s (got: %s)"), Case, *Path, *Result.ErrorSummary()), HasErrorAt(Result, Path));
		// Every error is a storage error: nothing else about the place is wrong, so the refusal is for the case.
		for (const FMantleParseError& Error : Result.Errors)
		{
			TestTrue(FString::Printf(TEXT("%s: error '%s' concerns storage"), Case, *Error.ToString()),
				Error.Path.StartsWith(TEXT("/storage")) || Error.Path.StartsWith(TEXT("/entities/0/properties")));
		}
	};

	// The storage array itself.
	ExpectRefused(TEXT("a duplicate name"),
		PlaceJson(StoredSpawn, TEXT("[") + StorageEntry(TEXT("Truck"), ItemTruck, 1) + TEXT(",") + StorageEntry(TEXT("Truck"), ItemGun, 1) + TEXT("]")),
		TEXT("/storage/1/name"));
	ExpectRefused(TEXT("the same item stored twice at two versions"),
		PlaceJson(StoredSpawn, TEXT("[") + StorageEntry(TEXT("Truck"), ItemTruck, 1) + TEXT(",") + StorageEntry(TEXT("TruckNew"), ItemTruck, 2) + TEXT("]")),
		TEXT("/storage/1/item"));
	ExpectRefused(TEXT("an item id that is not itm_u plus 32 hex digits"),
		PlaceJson(StoredSpawn, TEXT("[") + StorageEntry(TEXT("Truck"), TEXT("itm_uNOTHEX"), 1) + TEXT("]")),
		TEXT("/storage/0/item"));
	ExpectRefused(TEXT("an asset id where an item id belongs"),
		PlaceJson(StoredSpawn, TEXT("[") + StorageEntry(TEXT("Truck"), AssetOwn, 1) + TEXT("]")),
		TEXT("/storage/0/item"));
	ExpectRefused(TEXT("an item id with upper-case hex"),
		PlaceJson(StoredSpawn, TEXT("[") + StorageEntry(TEXT("Truck"), TEXT("itm_u0123456789ABCDEF0123456789abcdef"), 1) + TEXT("]")),
		TEXT("/storage/0/item"));
	ExpectRefused(TEXT("a version of zero"),
		PlaceJson(StoredSpawn, TEXT("[") + StorageEntry(TEXT("Truck"), ItemTruck, 0) + TEXT("]")),
		TEXT("/storage/0/version"));
	ExpectRefused(TEXT("a name that starts with a digit"),
		PlaceJson(FString(), TEXT("[") + StorageEntry(TEXT("1Truck"), ItemTruck, 1) + TEXT("]")),
		TEXT("/storage/0/name"));
	ExpectRefused(TEXT("a name with a space"),
		PlaceJson(FString(), TEXT("[") + StorageEntry(TEXT("Big Truck"), ItemTruck, 1) + TEXT("]")),
		TEXT("/storage/0/name"));
	ExpectRefused(TEXT("an unknown key in an entry"),
		PlaceJson(FString(), FString::Printf(TEXT("[{\"name\":\"Truck\",\"item\":\"%s\",\"version\":1,\"extra\":true}]"), ItemTruck)),
		TEXT("/storage/0/extra"));
	ExpectRefused(TEXT("a storage value that is not an array"),
		PlaceJson(FString(), TEXT("{}")),
		TEXT("/storage"));

	FString TooMany = TEXT("[");
	for (int32 i = 0; i <= FMantlePlaceLimits::MaxStorageEntries; ++i)
	{
		TooMany += (i > 0 ? TEXT(",") : TEXT("")) + StorageEntry(*FString::Printf(TEXT("Item%d"), i), ItemTruck, 1);
	}
	TooMany += TEXT("]");
	ExpectRefused(TEXT("more than 32 entries"), PlaceJson(FString(), TooMany), TEXT("/storage"));

	// VehicleSpawn's storageItem and vehicle.
	ExpectRefused(TEXT("a storageItem no entry names"),
		PlaceJson(SpawnEntity(TEXT("a"), TEXT("\"storageItem\":\"Boat\"")), OneEntry),
		TEXT("/entities/0/properties/storageItem"));
	ExpectRefused(TEXT("a storageItem that differs from its entry only in case"),
		PlaceJson(SpawnEntity(TEXT("a"), TEXT("\"storageItem\":\"truck\"")), OneEntry),
		TEXT("/entities/0/properties/storageItem"));
	ExpectRefused(TEXT("a storageItem in a place with no storage"),
		PlaceJson(StoredSpawn, FString()),
		TEXT("/entities/0/properties/storageItem"));
	ExpectRefused(TEXT("both vehicle and storageItem"),
		PlaceJson(SpawnEntity(TEXT("a"), FString::Printf(TEXT("\"vehicle\":\"%s\",\"storageItem\":\"Truck\""), AssetOwn)), OneEntry),
		TEXT("/entities/0/properties/vehicle"));
	ExpectRefused(TEXT("a raw uploaded vehicle id"),
		PlaceJson(SpawnEntity(TEXT("a"), FString::Printf(TEXT("\"vehicle\":\"%s\""), AssetOwn)), OneEntry),
		TEXT("/entities/0/properties/vehicle"));
	ExpectRefused(TEXT("neither vehicle nor storageItem"),
		PlaceJson(SpawnEntity(TEXT("a"), TEXT("\"respawnSeconds\":3")), OneEntry),
		TEXT("/entities/0/properties/storageItem"));
	ExpectRefused(TEXT("a storageItem that is not a name"),
		PlaceJson(SpawnEntity(TEXT("a"), TEXT("\"storageItem\":\"not a name\"")), OneEntry),
		TEXT("/entities/0/properties/storageItem"));

	// storage is a root key like any other: it must not make an unknown root key pass.
	const FMantleParseResult UnknownRoot = FMantlePlaceParser::ParseFromString(PlaceJson(FString(), FString()).LeftChop(1) + TEXT(",\"storages\":[]}"));
	TestFalse(TEXT("an unknown root key is still refused"), UnknownRoot.bSuccess);
	TestTrue(TEXT("and named: /storages"), HasErrorAt(UnknownRoot, TEXT("/storages")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceStorageResponseTest,
	"Mantle.PlaceStorage.Resolve.ReadsAnswerAndBuildsRequest",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceStorageResponseTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceStorageTestPrivate;
	FMantleStorageExpansion Expansion;
	if (!TestTrue(TEXT("the answer reads"), FMantleStorage::ParseResponse(ResolveAnswerJson(), Expansion)))
	{
		return false;
	}
	TestEqual(TEXT("two entries"), Expansion.Entries.Num(), 2);
	TestEqual(TEXT("one missing"), Expansion.Missing.Num(), 1);
	const FMantleResolvedStorageEntry* Truck = Expansion.Find(ItemTruck, 2);
	if (TestNotNull(TEXT("truck v2 is found"), Truck))
	{
		TestEqual(TEXT("kind"), Truck->Kind, FString(TEXT("vehicle")));
		TestTrue(TEXT("primary asset"), Truck->PrimaryAssetId.Equals(AssetTruck, ESearchCase::CaseSensitive));
		TestEqual(TEXT("dependencies"), Truck->DependencyAssetIds.Num(), 2);
	}
	TestNull(TEXT("truck v1 is not the entry that was answered"), Expansion.Find(ItemTruck, 1));
	TestTrue(TEXT("lost is missing"), Expansion.IsMissing(ItemLost, 1));
	TestFalse(TEXT("truck is not missing"), Expansion.IsMissing(ItemTruck, 2));

	// An answer that is the wrong shape reads as a failure, not as an empty expansion.
	FMantleStorageExpansion Broken;
	TestFalse(TEXT("not JSON"), FMantleStorage::ParseResponse(TEXT("nope"), Broken));
	TestFalse(TEXT("no entries array"), FMantleStorage::ParseResponse(TEXT("{\"missing\":[]}"), Broken));
	TestFalse(TEXT("an entry without a primary asset id"),
		FMantleStorage::ParseResponse(
			FString::Printf(TEXT("{\"entries\":[{\"item\":\"%s\",\"version\":1,\"kind\":\"vehicle\",\"status\":\"active\"}]}"), ItemTruck), Broken));
	TestFalse(TEXT("a version of zero"),
		FMantleStorage::ParseResponse(
			FString::Printf(TEXT("{\"entries\":[{\"item\":\"%s\",\"version\":0,\"kind\":\"vehicle\",\"status\":\"active\",\"primaryAssetId\":\"%s\"}]}"),
				ItemTruck, AssetTruck), Broken));
	TestTrue(TEXT("missing may be left out"),
		FMantleStorage::ParseResponse(TEXT("{\"entries\":[]}"), Broken));

	// The request: one ref per distinct item version, however many names pin it.
	TArray<FMantleStorageEntry> Entries;
	for (const TPair<const TCHAR*, int32>& Row : { TPair<const TCHAR*, int32>(TEXT("A"), 1), TPair<const TCHAR*, int32>(TEXT("B"), 1),
		TPair<const TCHAR*, int32>(TEXT("C"), 2) })
	{
		FMantleStorageEntry Entry;
		Entry.Name = Row.Key;
		Entry.Item = ItemTruck;
		Entry.Version = Row.Value;
		Entries.Add(MoveTemp(Entry));
	}
	const TArray<FMantleStorageRef> Refs = FMantleStorage::RefsOf(Entries);
	TestEqual(TEXT("two distinct item versions"), Refs.Num(), 2);

	TSharedPtr<FJsonObject> Body;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FMantleApiClient::MakeResolveStorageBody(Refs)), Body);
	if (TestTrue(TEXT("the request body is JSON"), Body.IsValid()))
	{
		const TArray<TSharedPtr<FJsonValue>>* Listed = nullptr;
		if (TestTrue(TEXT("with an entries array"), Body->TryGetArrayField(TEXT("entries"), Listed)) && TestEqual(TEXT("of two"), Listed->Num(), 2))
		{
			const TSharedPtr<FJsonObject> Second = (*Listed)[1]->AsObject();
			TestEqual(TEXT("an item"), Second->GetStringField(TEXT("item")), FString(ItemTruck));
			TestEqual(TEXT("and its version"), static_cast<int32>(Second->GetNumberField(TEXT("version"))), 2);
		}
		TestEqual(TEXT("and no other key"), Body->Values.Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceStorageApplyTest,
	"Mantle.PlaceStorage.Resolve.AppliesPrimaryAssetIdToSpawns",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceStorageApplyTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceStorageTestPrivate;
	const FString Storage = TEXT("[") + StorageEntry(TEXT("Truck"), ItemTruck, 2) + TEXT(",") + StorageEntry(TEXT("Gun"), ItemGun, 1)
		+ TEXT(",") + StorageEntry(TEXT("Lost"), ItemLost, 1) + TEXT("]");
	const FString Entities = SpawnEntity(TEXT("a"), TEXT("\"storageItem\":\"Truck\""))
		+ TEXT(",") + SpawnEntity(TEXT("b"), TEXT("\"storageItem\":\"Gun\""))
		+ TEXT(",") + SpawnEntity(TEXT("c"), TEXT("\"storageItem\":\"Lost\""))
		+ TEXT(",") + SpawnEntity(TEXT("e"), TEXT("\"storageItem\":\"Truck\""));
	FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(PlaceJson(Entities, Storage));
	if (!TestTrue(TEXT("the place parses"), Parsed.bSuccess))
	{
		AddError(Parsed.ErrorSummary());
		return false;
	}

	FMantleStorageExpansion Expansion;
	if (!TestTrue(TEXT("the fake answer reads"), FMantleStorage::ParseResponse(ResolveAnswerJson(), Expansion)))
	{
		return false;
	}

	// Each turned-off spawn logs a warning by design.
	const int32 TurnedOff = FMantleStorage::Apply(Parsed.Place, Expansion);
	TestEqual(TEXT("the weapon and the missing item turn their spawns off"), TurnedOff, 2);

	const FMantleVehicleSpawnProps* A = SpawnAt(Parsed.Place, 0);
	const FMantleVehicleSpawnProps* B = SpawnAt(Parsed.Place, 1);
	const FMantleVehicleSpawnProps* C = SpawnAt(Parsed.Place, 2);
	const FMantleVehicleSpawnProps* E = SpawnAt(Parsed.Place, 3);
	if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B) || !TestNotNull(TEXT("C"), C) || !TestNotNull(TEXT("E"), E))
	{
		return false;
	}

	TestTrue(TEXT("a stored vehicle spawns the item's primary asset"), A->Vehicle.Equals(AssetTruck, ESearchCase::CaseSensitive));
	TestTrue(TEXT("and stays on"), A->bEnabled);
	TestEqual(TEXT("and still names its storage entry"), A->StorageItem, FString(TEXT("Truck")));
	TestTrue(TEXT("two spawns of one entry get the same id (the server and every client agree)"),
		E->Vehicle.Equals(A->Vehicle, ESearchCase::CaseSensitive));

	TestTrue(TEXT("a weapon in a vehicle spawn is refused: no vehicle"), B->Vehicle.IsEmpty());
	TestFalse(TEXT("and the spawn is off"), B->bEnabled);
	TestTrue(TEXT("a missing item has no vehicle"), C->Vehicle.IsEmpty());
	TestFalse(TEXT("and its spawn is off"), C->bEnabled);

	// A place without storage is a no-op for the whole step.
	bool bCalled = false;
	bool bSucceeded = false;
	FMantleParseResult Plain = FMantlePlaceParser::ParseFromString(PlaceJson(FString(), FString()));
	FMantleStorage::ExpandAsync(TEXT("plc_unused"), /*bServer*/ false, MoveTemp(Plain.Place), [&bCalled, &bSucceeded](FMantleStorageExpandResult Result)
	{
		bCalled = true;
		bSucceeded = Result.bSuccess;
	});
	TestTrue(TEXT("no storage means no request: the callback runs before ExpandAsync returns"), bCalled);
	TestTrue(TEXT("and reports success"), bSucceeded);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceStorageClassifyTest,
	"Mantle.PlaceStorage.Resolve.ClassifiesAnswers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceStorageClassifyTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceStorageTestPrivate;
	FMantleStorageExpansion Out;
	FString Error;
	const auto Classify = [&Out, &Error](const int32 Status, const FString& Body, const FString& Transport = FString())
	{
		return FMantleStorage::Classify(Status, Body, Transport, Out, Error);
	};

	// A readable 200 (or any 2xx) is applied, with the expansion filled.
	TestTrue(TEXT("200 with a readable body applies"), Classify(200, ResolveAnswerJson()) == EMantleStorageAnswer::Apply);
	TestEqual(TEXT("and fills the expansion"), Out.Entries.Num(), 2);
	TestTrue(TEXT("201 with a readable body applies"), Classify(201, TEXT("{\"entries\":[]}")) == EMantleStorageAnswer::Apply);

	// A missing entry is part of a successful answer, not a failure: Apply turns that spawn off.
	TestTrue(TEXT("an answer that lists missing items still applies"), Classify(200, ResolveAnswerJson()) == EMantleStorageAnswer::Apply);
	TestEqual(TEXT("with the missing item listed"), Out.Missing.Num(), 1);

	// Transient: the request never arrived, or the API had a server error.
	TestTrue(TEXT("a transport failure retries"), Classify(0, FString(), TEXT("Could not reach Mantle.")) == EMantleStorageAnswer::Retry);
	TestTrue(TEXT("its message is kept"), Error.Contains(TEXT("Could not reach Mantle.")));
	TestTrue(TEXT("500 retries"), Classify(500, TEXT("oops")) == EMantleStorageAnswer::Retry);
	TestTrue(TEXT("503 retries"), Classify(503, FString()) == EMantleStorageAnswer::Retry);
	TestFalse(TEXT("and names the status"), Error.IsEmpty());

	// Permanent: trying again cannot help.
	TestTrue(TEXT("400 fails"), Classify(400, TEXT("{}")) == EMantleStorageAnswer::Fail);
	TestTrue(TEXT("401 fails"), Classify(401, FString()) == EMantleStorageAnswer::Fail);
	TestTrue(TEXT("403 fails"), Classify(403, FString()) == EMantleStorageAnswer::Fail);
	TestTrue(TEXT("404 (no such route) fails"), Classify(404, FString()) == EMantleStorageAnswer::Fail);
	TestTrue(TEXT("429 fails"), Classify(429, FString()) == EMantleStorageAnswer::Fail);
	TestTrue(TEXT("a redirect fails"), Classify(302, FString()) == EMantleStorageAnswer::Fail);
	TestTrue(TEXT("200 with an unreadable body fails"), Classify(200, TEXT("not json")) == EMantleStorageAnswer::Fail);
	TestTrue(TEXT("and says so"), Error.Contains(TEXT("could not be read")));
	TestTrue(TEXT("200 without an entries array fails"), Classify(200, TEXT("{}")) == EMantleStorageAnswer::Fail);
	TestEqual(TEXT("a failure leaves the expansion empty"), Out.Entries.Num(), 0);

	// The backoff: short, growing, and bounded to two retries.
	TestEqual(TEXT("two retries"), FMantleStorage::MaxRetries, 2);
	TestTrue(TEXT("the second wait is longer than the first"), FMantleStorage::RetryDelaySeconds(2) > FMantleStorage::RetryDelaySeconds(1));
	TestTrue(TEXT("and both are short"), FMantleStorage::RetryDelaySeconds(2) <= 5.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceStorageTakenDownTest,
	"Mantle.PlaceStorage.Resolve.ATakenDownItemTurnsItsSpawnOffAndIsNamedAsTakenDown",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceStorageTakenDownTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceStorageTestPrivate;
	const FString Storage = TEXT("[") + StorageEntry(TEXT("Truck"), ItemTruck, 2) + TEXT("]");
	const FString Entities = SpawnEntity(TEXT("a"), TEXT("\"storageItem\":\"Truck\""));
	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(PlaceJson(Entities, Storage));
	if (!TestTrue(TEXT("the place parses"), Parsed.bSuccess))
	{
		return false;
	}

	// The same item the backend resolved as active a moment ago now answers taken_down, without the assets it no longer serves.
	FMantleStorageExpansion Expansion;
	const FString Answer = FString::Printf(TEXT("{\"entries\":[{\"name\":\"Truck\",\"item\":\"%s\",\"version\":2,\"kind\":\"vehicle\",\"status\":\"taken_down\"}]}"), ItemTruck);
	if (!TestTrue(TEXT("the answer reads"), FMantleStorage::ParseResponse(Answer, Expansion)))
	{
		return false;
	}
	FMantlePlace Place = Parsed.Place;
	TestEqual(TEXT("the spawn is turned off"), FMantleStorage::Apply(Place, Expansion), 1);
	const FMantleVehicleSpawnProps* Spawn = SpawnAt(Place, 0);
	if (TestNotNull(TEXT("the spawn is still there"), Spawn))
	{
		TestTrue(TEXT("with no vehicle"), Spawn->Vehicle.IsEmpty());
		TestFalse(TEXT("and disabled"), Spawn->bEnabled);
	}
	TestEqual(TEXT("the place remembers the takedown"), Place.StorageTakenDown.Num(), 1);
	TestEqual(TEXT("and resolved nothing"), Place.StorageResolved.Num(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
