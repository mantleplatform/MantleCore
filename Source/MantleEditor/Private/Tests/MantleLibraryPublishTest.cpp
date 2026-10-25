// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCosmeticPackage.h"
#include "MantleLibraryPublish.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// Publishing a vehicle as a library item: the rewrite of the exported place file so its
// spawns name storage, and the request bodies. Pure: no network, no level.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleLibraryPublishTestPrivate
{
	const TCHAR* const AssetTruck = TEXT("ast_u11111111111111111111111111111111");
	const TCHAR* const AssetBus = TEXT("ast_u22222222222222222222222222222222");
	const TCHAR* const AssetMaterial = TEXT("ast_u33333333333333333333333333333333");

	FString ItemId(const int32 Number)
	{
		return FString::Printf(TEXT("itm_u%032x"), Number);
	}

	FMantleLibraryVehicleRef MakeRef(const TCHAR* AssetId, const TCHAR* Name, const FString& Item, const int32 Version)
	{
		FMantleLibraryVehicleRef Ref;
		Ref.AssetId = AssetId;
		Ref.Name = Name;
		Ref.ItemId = Item;
		Ref.Version = Version;
		return Ref;
	}

	FString Spawn(const TCHAR* EntityId, const TCHAR* AssetId)
	{
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"type\":\"VehicleSpawn\",\"properties\":{\"vehicle\":\"%s\",\"respawnSeconds\":0.1,\"spawnWeight\":1e3,\"enabled\":true}}"),
			EntityId, AssetId);
	}

	/** A place file with the given spawns (as JSON objects) and an optional storage array text. */
	FString MakePlace(const TArray<FString>& Spawns, const FString& StorageJson = FString())
	{
		FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"assets\":[{\"id\":\"%s\",\"kind\":\"Vehicle\",\"hash\":\"sha256:aa\",\"bytes\":10},")
			TEXT("{\"id\":\"%s\",\"kind\":\"Vehicle\",\"hash\":\"sha256:bb\",\"bytes\":12},")
			TEXT("{\"id\":\"%s\",\"kind\":\"Material\",\"hash\":\"sha256:cc\",\"bytes\":3}],\"entities\":[%s]"),
			AssetTruck, AssetBus, AssetMaterial, *FString::Join(Spawns, TEXT(",")));
		if (!StorageJson.IsEmpty())
		{
			Json += TEXT(",\"storage\":") + StorageJson;
		}
		return Json + TEXT("}");
	}

	TSharedPtr<FJsonObject> ReadObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Root;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root);
		return Root;
	}

	TSharedPtr<FJsonObject> SpawnProps(const TSharedPtr<FJsonObject>& Root, const int32 Index)
	{
		const TArray<TSharedPtr<FJsonValue>>* Entities = nullptr;
		if (!Root->TryGetArrayField(TEXT("entities"), Entities) || !Entities->IsValidIndex(Index))
		{
			return nullptr;
		}
		return (*Entities)[Index]->AsObject()->GetObjectField(TEXT("properties"));
	}

	TArray<FString> AssetIds(const TSharedPtr<FJsonObject>& Root)
	{
		TArray<FString> Ids;
		const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
		if (Root->TryGetArrayField(TEXT("assets"), Assets))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Assets)
			{
				Ids.Add(Value->AsObject()->GetStringField(TEXT("id")));
			}
		}
		return Ids;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryPublishRewritesSpawnsTest,
	"Mantle.LibraryPublish.Place.SpawnsNameStorageInsteadOfTheUpload",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryPublishRewritesSpawnsTest::RunTest(const FString& Parameters)
{
	using namespace MantleLibraryPublishTestPrivate;
	const FString Json = MakePlace({ Spawn(TEXT("a"), AssetTruck), Spawn(TEXT("b"), AssetTruck), Spawn(TEXT("c"), AssetBus) });
	FString Out;
	FString Error;
	const TArray<FMantleLibraryVehicleRef> Refs = { MakeRef(AssetTruck, TEXT("SKM_Truck_Vehicle"), ItemId(1), 2) };
	if (!TestTrue(TEXT("the rewrite succeeds"), FMantleLibraryPublish::RewritePlace(Json, Refs, Out, Error)))
	{
		AddError(Error);
		return false;
	}
	const TSharedPtr<FJsonObject> Root = ReadObject(Out);
	if (!TestTrue(TEXT("the result is a place file"), Root.IsValid()))
	{
		return false;
	}
	for (const int32 Index : { 0, 1 })
	{
		const TSharedPtr<FJsonObject> Props = SpawnProps(Root, Index);
		TestEqual(TEXT("a spawn of the stored vehicle names its storage entry"), Props->GetStringField(TEXT("storageItem")), FString(TEXT("SKM_Truck_Vehicle")));
		TestFalse(TEXT("and no longer names the upload"), Props->HasField(TEXT("vehicle")));
	}
	const TSharedPtr<FJsonObject> Other = SpawnProps(Root, 2);
	TestEqual(TEXT("a spawn of another vehicle keeps its upload id"), Other->GetStringField(TEXT("vehicle")), FString(AssetBus));
	TestFalse(TEXT("and has no storage name"), Other->HasField(TEXT("storageItem")));

	const TArray<TSharedPtr<FJsonValue>>* Storage = nullptr;
	if (TestTrue(TEXT("storage is written"), Root->TryGetArrayField(TEXT("storage"), Storage)) && TestEqual(TEXT("one entry"), Storage->Num(), 1))
	{
		const TSharedPtr<FJsonObject> Entry = (*Storage)[0]->AsObject();
		TestEqual(TEXT("its name"), Entry->GetStringField(TEXT("name")), FString(TEXT("SKM_Truck_Vehicle")));
		TestEqual(TEXT("its item"), Entry->GetStringField(TEXT("item")), ItemId(1));
		TestEqual(TEXT("its version"), static_cast<int32>(Entry->GetNumberField(TEXT("version"))), 2);
	}

	const TArray<FString> Assets = AssetIds(Root);
	TestFalse(TEXT("the stored vehicle leaves the place's assets"), Assets.Contains(AssetTruck));
	TestTrue(TEXT("the other vehicle stays"), Assets.Contains(AssetBus));
	TestTrue(TEXT("and so does the material"), Assets.Contains(AssetMaterial));
	// 0.1 and 1e3 are what a re-serialise would change (to 0.10000000000000001 and 1000): they must survive byte for byte.
	TestTrue(TEXT("a number the rewrite did not touch keeps its exact text"), Out.Contains(TEXT("\"respawnSeconds\":0.1,")));
	TestTrue(TEXT("and so does one written with an exponent"), Out.Contains(TEXT("\"spawnWeight\":1e3,")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryPublishReplacesDeclaredEntryTest,
	"Mantle.LibraryPublish.Place.ANewVersionReplacesTheDeclaredOneOfTheSameItem",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryPublishReplacesDeclaredEntryTest::RunTest(const FString& Parameters)
{
	using namespace MantleLibraryPublishTestPrivate;
	const FString Storage = FString::Printf(
		TEXT("[{\"name\":\"MyTruck\",\"item\":\"%s\",\"version\":1},{\"name\":\"Bus\",\"item\":\"%s\",\"version\":4}]"), *ItemId(1), *ItemId(2));
	const FString Json = MakePlace({ Spawn(TEXT("a"), AssetTruck) }, Storage);
	FString Out;
	FString Error;
	const TArray<FMantleLibraryVehicleRef> Refs = { MakeRef(AssetTruck, TEXT("SKM_Truck_Vehicle"), ItemId(1), 3) };
	if (!TestTrue(TEXT("the rewrite succeeds"), FMantleLibraryPublish::RewritePlace(Json, Refs, Out, Error)))
	{
		AddError(Error);
		return false;
	}
	const TSharedPtr<FJsonObject> Root = ReadObject(Out);
	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	if (!TestTrue(TEXT("storage is written"), Root.IsValid() && Root->TryGetArrayField(TEXT("storage"), Entries)))
	{
		return false;
	}
	TestEqual(TEXT("still two entries: one version of one item, never two"), Entries->Num(), 2);
	const TSharedPtr<FJsonObject> Truck = (*Entries)[0]->AsObject();
	TestEqual(TEXT("the declared entry keeps its name"), Truck->GetStringField(TEXT("name")), FString(TEXT("MyTruck")));
	TestEqual(TEXT("and takes the new version"), static_cast<int32>(Truck->GetNumberField(TEXT("version"))), 3);
	TestEqual(TEXT("the other entry is untouched"), static_cast<int32>((*Entries)[1]->AsObject()->GetNumberField(TEXT("version"))), 4);
	TestEqual(TEXT("the spawn names the declared entry"), SpawnProps(Root, 0)->GetStringField(TEXT("storageItem")), FString(TEXT("MyTruck")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryPublishUniqueNamesTest,
	"Mantle.LibraryPublish.Place.CollidingStorageNamesAreMadeUnique",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryPublishUniqueNamesTest::RunTest(const FString& Parameters)
{
	using namespace MantleLibraryPublishTestPrivate;
	// Another item already holds the name "Truck", and both new vehicles are called Truck too.
	const FString Storage = FString::Printf(TEXT("[{\"name\":\"Truck\",\"item\":\"%s\",\"version\":1}]"), *ItemId(9));
	const FString Json = MakePlace({ Spawn(TEXT("a"), AssetTruck), Spawn(TEXT("b"), AssetBus) }, Storage);
	FString Out;
	FString Error;
	const TArray<FMantleLibraryVehicleRef> Refs = {
		MakeRef(AssetTruck, TEXT("Truck"), ItemId(1), 1),
		MakeRef(AssetBus, TEXT("Truck"), ItemId(2), 1),
	};
	if (!TestTrue(TEXT("the rewrite succeeds"), FMantleLibraryPublish::RewritePlace(Json, Refs, Out, Error)))
	{
		AddError(Error);
		return false;
	}
	const TSharedPtr<FJsonObject> Root = ReadObject(Out);
	TestEqual(TEXT("the first takes the next free name"), SpawnProps(Root, 0)->GetStringField(TEXT("storageItem")), FString(TEXT("Truck_2")));
	TestEqual(TEXT("the second the one after"), SpawnProps(Root, 1)->GetStringField(TEXT("storageItem")), FString(TEXT("Truck_3")));
	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	if (TestTrue(TEXT("storage is written"), Root->TryGetArrayField(TEXT("storage"), Entries)))
	{
		TestEqual(TEXT("three entries"), Entries->Num(), 3);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryPublishCapTest,
	"Mantle.LibraryPublish.Place.MoreThanTheStorageCapIsRefused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryPublishCapTest::RunTest(const FString& Parameters)
{
	using namespace MantleLibraryPublishTestPrivate;
	TArray<FString> Declared;
	for (int32 Index = 0; Index < 32; ++Index)
	{
		Declared.Add(FString::Printf(TEXT("{\"name\":\"Item%d\",\"item\":\"%s\",\"version\":1}"), Index, *ItemId(100 + Index)));
	}
	const FString Json = MakePlace({ Spawn(TEXT("a"), AssetTruck) }, TEXT("[") + FString::Join(Declared, TEXT(",")) + TEXT("]"));
	FString Out;
	FString Error;
	const TArray<FMantleLibraryVehicleRef> Refs = { MakeRef(AssetTruck, TEXT("Truck"), ItemId(1), 1) };
	TestFalse(TEXT("a 33rd item is refused"), FMantleLibraryPublish::RewritePlace(Json, Refs, Out, Error));
	TestFalse(TEXT("with a sentence saying why"), Error.IsEmpty());

	// The same 32 with a vehicle whose item is already one of them is not a 33rd.
	const TArray<FMantleLibraryVehicleRef> Known = { MakeRef(AssetTruck, TEXT("Truck"), ItemId(100), 5) };
	TestTrue(TEXT("a new version of a stored item fits a full storage"), FMantleLibraryPublish::RewritePlace(Json, Known, Out, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryPublishUnchangedTest,
	"Mantle.LibraryPublish.Place.APlaceWithoutStoredVehiclesIsUnchanged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryPublishUnchangedTest::RunTest(const FString& Parameters)
{
	using namespace MantleLibraryPublishTestPrivate;
	const FString Json = MakePlace({ Spawn(TEXT("a"), AssetBus) });
	FString Out;
	FString Error;
	TestTrue(TEXT("no vehicles: succeeds"), FMantleLibraryPublish::RewritePlace(Json, {}, Out, Error));
	TestEqual(TEXT("no vehicles: byte for byte"), Out, Json);
	const TArray<FMantleLibraryVehicleRef> Unused = { MakeRef(AssetTruck, TEXT("Truck"), ItemId(1), 1) };
	TestTrue(TEXT("a vehicle no spawn uses: succeeds"), FMantleLibraryPublish::RewritePlace(Json, Unused, Out, Error));
	TestEqual(TEXT("a vehicle no spawn uses: byte for byte"), Out, Json);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryPublishStorageNameTest,
	"Mantle.LibraryPublish.Place.StorageNamesFollowTheSchemaPattern",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryPublishStorageNameTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("a plain name is kept"), FMantleLibraryPublish::StorageNameFor(TEXT("SKM_Truck_Vehicle")), FString(TEXT("SKM_Truck_Vehicle")));
	TestEqual(TEXT("other characters become underscores"), FMantleLibraryPublish::StorageNameFor(TEXT("My Truck-2")), FString(TEXT("My_Truck_2")));
	TestEqual(TEXT("a name that starts with a digit gets a prefix"), FMantleLibraryPublish::StorageNameFor(TEXT("2Fast")), FString(TEXT("Vehicle_2Fast")));
	TestEqual(TEXT("nothing usable is Vehicle"), FMantleLibraryPublish::StorageNameFor(TEXT("---")), FString(TEXT("Vehicle")));
	TestEqual(TEXT("an empty name is Vehicle"), FMantleLibraryPublish::StorageNameFor(FString()), FString(TEXT("Vehicle")));
	TestEqual(TEXT("never longer than 64"), FMantleLibraryPublish::StorageNameFor(FString::ChrN(100, TEXT('a'))).Len(), 64);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryPublishBodiesTest,
	"Mantle.LibraryPublish.Requests.PublicNeedsTheDeclarationAndPrivateSendsNone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryPublishBodiesTest::RunTest(const FString& Parameters)
{
	using namespace MantleLibraryPublishTestPrivate;
	const TSharedPtr<FJsonObject> Public = ReadObject(FMantleLibraryPublish::MakeItemBody(TEXT("Truck"), true, AssetTruck));
	if (!TestTrue(TEXT("the public body is JSON"), Public.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("it is a vehicle"), Public->GetStringField(TEXT("kind")), FString(TEXT("vehicle")));
	TestEqual(TEXT("public"), Public->GetStringField(TEXT("visibility")), FString(TEXT("public")));
	TestEqual(TEXT("with the declaration version the panel shows"), Public->GetStringField(TEXT("rightsDeclarationVersion")), FString(MantleRights::DeclarationVersion));
	TestEqual(TEXT("its name"), Public->GetStringField(TEXT("name")), FString(TEXT("Truck")));
	TestEqual(TEXT("its primary asset"), Public->GetStringField(TEXT("primaryAssetId")), FString(AssetTruck));

	const TSharedPtr<FJsonObject> Private = ReadObject(FMantleLibraryPublish::MakeItemBody(TEXT("Truck"), false, AssetTruck));
	if (!TestTrue(TEXT("the private body is JSON"), Private.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("private"), Private->GetStringField(TEXT("visibility")), FString(TEXT("private")));
	TestFalse(TEXT("with no declaration"), Private->HasField(TEXT("rightsDeclarationVersion")));

	const TSharedPtr<FJsonObject> Long = ReadObject(FMantleLibraryPublish::MakeItemBody(FString::ChrN(100, TEXT('x')), false, AssetTruck));
	TestEqual(TEXT("a long name is cut to the API's 64"), Long->GetStringField(TEXT("name")).Len(), 64);

	const TSharedPtr<FJsonObject> Version = ReadObject(FMantleLibraryPublish::MakeVersionBody(AssetBus));
	if (TestTrue(TEXT("the version body is JSON"), Version.IsValid()))
	{
		TestEqual(TEXT("it names the asset"), Version->GetStringField(TEXT("primaryAssetId")), FString(AssetBus));
		TestEqual(TEXT("and nothing else"), Version->Values.Num(), 1);
	}

	const FString Key = FMantleLibraryPublish::MakeIdempotencyKey(TEXT("/Game/Cars/V"), AssetTruck, true);
	TestEqual(TEXT("the same upload has the same key"), FMantleLibraryPublish::MakeIdempotencyKey(TEXT("/Game/Cars/V"), AssetTruck, true), Key);
	TestNotEqual(TEXT("another upload has another"), FMantleLibraryPublish::MakeIdempotencyKey(TEXT("/Game/Cars/V"), AssetBus, true), Key);
	TestNotEqual(TEXT("another visibility has another"), FMantleLibraryPublish::MakeIdempotencyKey(TEXT("/Game/Cars/V"), AssetTruck, false), Key);
	TestTrue(TEXT("it is a printable key without spaces within 128 characters"), Key.Len() >= 1 && Key.Len() <= 128 && !Key.Contains(TEXT(" ")));

	// A vehicle whose item was forgotten (deleted or delisted) makes a new item, which must not replay the old creation.
	TestEqual(TEXT("generation 0 is the key a vehicle always had"), FMantleLibraryPublish::MakeIdempotencyKey(TEXT("/Game/Cars/V"), AssetTruck, true, 0), Key);
	const FString SecondKey = FMantleLibraryPublish::MakeIdempotencyKey(TEXT("/Game/Cars/V"), AssetTruck, true, 1);
	TestNotEqual(TEXT("a later generation has another key"), SecondKey, Key);
	TestEqual(TEXT("the same generation has the same key"), FMantleLibraryPublish::MakeIdempotencyKey(TEXT("/Game/Cars/V"), AssetTruck, true, 1), SecondKey);
	TestNotEqual(TEXT("and each generation its own"), FMantleLibraryPublish::MakeIdempotencyKey(TEXT("/Game/Cars/V"), AssetTruck, true, 2), SecondKey);
	TestTrue(TEXT("a generation's key is still a printable key within 128 characters"), SecondKey.Len() >= 1 && SecondKey.Len() <= 128 && !SecondKey.Contains(TEXT(" ")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryPublishExistingItemTest,
	"Mantle.LibraryPublish.Requests.AnExistingItemFollowsTheChoiceAndADelistedOneIsReplaced",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryPublishExistingItemTest::RunTest(const FString& Parameters)
{
	using namespace MantleLibraryPublishTestPrivate;
	FString Visibility;
	FString Status;
	TestTrue(TEXT("an item answer is read"), FMantleLibraryPublish::ReadItemState(
		FString::Printf(TEXT("{\"id\":\"%s\",\"visibility\":\"private\",\"status\":\"active\",\"versions\":[]}"), *ItemId(3)), Visibility, Status));
	TestEqual(TEXT("its visibility"), Visibility, FString(TEXT("private")));
	TestEqual(TEXT("its status"), Status, FString(TEXT("active")));
	TestFalse(TEXT("an answer that is not JSON is refused"), FMantleLibraryPublish::ReadItemState(TEXT("nope"), Visibility, Status));
	TestTrue(TEXT("an answer without them reads as unknown"), FMantleLibraryPublish::ReadItemState(TEXT("{\"id\":\"x\"}"), Visibility, Status) && Visibility.IsEmpty() && Status.IsEmpty());

	TestTrue(TEXT("a private item the creator now makes public changes"), FMantleLibraryPublish::VisibilityDiffers(TEXT("private"), true));
	TestTrue(TEXT("a public item the creator now makes private changes"), FMantleLibraryPublish::VisibilityDiffers(TEXT("public"), false));
	TestFalse(TEXT("a public item chosen public stays"), FMantleLibraryPublish::VisibilityDiffers(TEXT("public"), true));
	TestFalse(TEXT("a private item chosen private stays"), FMantleLibraryPublish::VisibilityDiffers(TEXT("private"), false));
	TestFalse(TEXT("an unknown visibility is left alone"), FMantleLibraryPublish::VisibilityDiffers(FString(), true));
	TestFalse(TEXT("ids and enum-like words are case sensitive"), FMantleLibraryPublish::VisibilityDiffers(TEXT("Public"), true));

	// An agent's publish is private because it cannot declare rights, not because the creator chose private.
	FMantleVehicleLibraryChoice AgentChoice;
	AgentChoice.bPublic = false;
	AgentChoice.bKeepExistingVisibility = true;
	FMantleVehicleLibraryChoice PanelPrivate;
	PanelPrivate.bPublic = false;
	TestTrue(TEXT("an existing public item stays public for an agent"), FMantleLibraryPublish::ResolveExistingItemPublic(TEXT("public"), AgentChoice));
	TestFalse(TEXT("so no visibility change is sent"), FMantleLibraryPublish::VisibilityDiffers(TEXT("public"), FMantleLibraryPublish::ResolveExistingItemPublic(TEXT("public"), AgentChoice)));
	TestFalse(TEXT("an existing private item stays private for an agent"), FMantleLibraryPublish::ResolveExistingItemPublic(TEXT("private"), AgentChoice));
	TestFalse(TEXT("a new item (no visibility yet) is created private for an agent"), FMantleLibraryPublish::ResolveExistingItemPublic(FString(), AgentChoice));
	TestFalse(TEXT("the panel choosing private still makes an existing public item private"), FMantleLibraryPublish::ResolveExistingItemPublic(TEXT("public"), PanelPrivate));
	TestTrue(TEXT("which is a change"), FMantleLibraryPublish::VisibilityDiffers(TEXT("public"), FMantleLibraryPublish::ResolveExistingItemPublic(TEXT("public"), PanelPrivate)));
	const TSharedPtr<FJsonObject> PictureOnly = ReadObject(FMantleLibraryPublish::MakeThumbnailPatchBody(TEXT("ast_x")));
	TestTrue(TEXT("a picture-only edit names the picture"), PictureOnly.IsValid() && PictureOnly->HasField(TEXT("thumbnailAssetId")));
	TestTrue(TEXT("and not the visibility or a declaration"), PictureOnly.IsValid() && !PictureOnly->HasField(TEXT("visibility")) && !PictureOnly->HasField(TEXT("rightsDeclarationVersion")));

	TestTrue(TEXT("delisted is delisted"), FMantleLibraryPublish::IsDelisted(TEXT("delisted")));
	TestFalse(TEXT("active is not"), FMantleLibraryPublish::IsDelisted(TEXT("active")));
	TestFalse(TEXT("nor is an empty status"), FMantleLibraryPublish::IsDelisted(FString()));

	const TSharedPtr<FJsonObject> ToPublic = ReadObject(FMantleLibraryPublish::MakeVisibilityPatchBody(true));
	if (TestTrue(TEXT("the public patch is JSON"), ToPublic.IsValid()))
	{
		TestEqual(TEXT("it makes the item public"), ToPublic->GetStringField(TEXT("visibility")), FString(TEXT("public")));
		TestEqual(TEXT("with the declaration version the panel shows"), ToPublic->GetStringField(TEXT("rightsDeclarationVersion")), FString(MantleRights::DeclarationVersion));
		TestEqual(TEXT("and nothing else"), ToPublic->Values.Num(), 2);
	}
	const TSharedPtr<FJsonObject> ToPrivate = ReadObject(FMantleLibraryPublish::MakeVisibilityPatchBody(false));
	if (TestTrue(TEXT("the private patch is JSON"), ToPrivate.IsValid()))
	{
		TestEqual(TEXT("it makes the item private"), ToPrivate->GetStringField(TEXT("visibility")), FString(TEXT("private")));
		TestEqual(TEXT("and sends only that"), ToPrivate->Values.Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryPublishRetryTest,
	"Mantle.LibraryPublish.Requests.RateLimitedStepsRepeatAndOthersOnlyWhenSafe",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryPublishRetryTest::RunTest(const FString& Parameters)
{
	FMantleApiResult TooMany;
	TooMany.StatusCode = 429;
	FMantleApiResult Down;
	Down.StatusCode = 503;
	FMantleApiResult NoAnswer;
	NoAnswer.StatusCode = 0;
	FMantleApiResult Unauthorized;
	Unauthorized.StatusCode = 401;
	FMantleApiResult Refused;
	Refused.StatusCode = 409;
	TestTrue(TEXT("a 429 repeats a step that is safe to repeat"), FMantleLibraryPublish::ShouldRetry(TooMany, true));
	TestTrue(TEXT("a 429 repeats a new version too: it was not processed"), FMantleLibraryPublish::ShouldRetry(TooMany, false));
	TestTrue(TEXT("a 5xx repeats a step that is safe to repeat"), FMantleLibraryPublish::ShouldRetry(Down, true));
	TestFalse(TEXT("a 5xx does not repeat a new version: it may have been made"), FMantleLibraryPublish::ShouldRetry(Down, false));
	TestTrue(TEXT("no answer repeats a step that is safe to repeat"), FMantleLibraryPublish::ShouldRetry(NoAnswer, true));
	TestFalse(TEXT("no answer does not repeat a new version"), FMantleLibraryPublish::ShouldRetry(NoAnswer, false));
	TestFalse(TEXT("a 401 is never repeated"), FMantleLibraryPublish::ShouldRetry(Unauthorized, true));
	TestFalse(TEXT("nor is a refusal"), FMantleLibraryPublish::ShouldRetry(Refused, true));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryPublishTooManyVersionsTest,
	"Mantle.LibraryPublish.Requests.TooManyVersionsSuggestsANewAssetNotARemoveAction",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryPublishTooManyVersionsTest::RunTest(const FString& Parameters)
{
	FMantleApiResult Refused;
	Refused.StatusCode = 409;
	Refused.Code = TEXT("too_many_versions");
	const FString Text = FMantleLibraryPublish::DescribeFailure(Refused, TEXT("SKM_Truck"));
	TestTrue(TEXT("it names the vehicle"), Text.Contains(TEXT("SKM_Truck")));
	TestTrue(TEXT("it suggests renaming or duplicating the asset"), Text.Contains(TEXT("Rename or duplicate")));
	TestFalse(TEXT("it does not point at a remove action the Library panel lacks"), Text.Contains(TEXT("Remove")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryPublishAnswersTest,
	"Mantle.LibraryPublish.Requests.AnswersAreReadStrictly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryPublishAnswersTest::RunTest(const FString& Parameters)
{
	using namespace MantleLibraryPublishTestPrivate;
	FString Item;
	int32 Version = 0;
	TestTrue(TEXT("a create answer is read"),
		FMantleLibraryPublish::ReadCreated(FString::Printf(TEXT("{\"itemId\":\"%s\",\"version\":3}"), *ItemId(7)), Item, Version));
	TestEqual(TEXT("its item"), Item, ItemId(7));
	TestEqual(TEXT("its version"), Version, 3);
	TestFalse(TEXT("an answer without a library item id is refused"),
		FMantleLibraryPublish::ReadCreated(TEXT("{\"itemId\":\"ast_u11111111111111111111111111111111\",\"version\":1}"), Item, Version));
	TestFalse(TEXT("an answer that is not JSON is refused"), FMantleLibraryPublish::ReadCreated(TEXT("nope"), Item, Version));

	const FString Detail = FString::Printf(
		TEXT("{\"id\":\"%s\",\"versions\":[{\"version\":1,\"primaryAssetId\":\"%s\"},{\"version\":2,\"primaryAssetId\":\"%s\"}]}"),
		*ItemId(7), AssetTruck, AssetBus);
	TestTrue(TEXT("a version of the same upload is found"), FMantleLibraryPublish::FindVersionWithAsset(Detail, AssetBus, Version));
	TestEqual(TEXT("with its number"), Version, 2);
	TestFalse(TEXT("an upload no version holds is not"), FMantleLibraryPublish::FindVersionWithAsset(Detail, AssetMaterial, Version));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryPublishWriteBackTest,
	"Mantle.LibraryPublish.Level.PublishedEntriesMergeIntoTheLevelsStorageWithoutRemovingAny",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryPublishWriteBackTest::RunTest(const FString& Parameters)
{
	using namespace MantleLibraryPublishTestPrivate;
	// What the rewrite reports for the published file.
	const FString Json = MakePlace({ Spawn(TEXT("a"), AssetTruck), Spawn(TEXT("b"), AssetBus) },
		FString::Printf(TEXT("[{\"name\":\"MyTruck\",\"item\":\"%s\",\"version\":1}]"), *ItemId(1)));
	FString Out;
	FString Error;
	TArray<FMantleStoredVehicle> Stored;
	const TArray<FMantleLibraryVehicleRef> Refs = {
		MakeRef(AssetTruck, TEXT("SKM_Truck_Vehicle"), ItemId(1), 2),
		MakeRef(AssetBus, TEXT("Bus"), ItemId(2), 1),
	};
	if (!TestTrue(TEXT("the rewrite succeeds"), FMantleLibraryPublish::RewritePlace(Json, Refs, Out, Error, &Stored)))
	{
		AddError(Error);
		return false;
	}
	if (!TestEqual(TEXT("both vehicles are reported"), Stored.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("the declared name is reported for the declared item"), Stored[0].StorageName, FString(TEXT("MyTruck")));
	TestEqual(TEXT("a new name for the new item"), Stored[1].StorageName, FString(TEXT("Bus")));

	// The level's own list: the declared entry (an older version), and an unrelated one that must survive.
	TArray<FMantleStorageEntry> Level;
	FMantleStorageEntry Declared;
	Declared.Name = TEXT("MyTruck");
	Declared.Item = ItemId(1);
	Declared.Version = 1;
	Level.Add(Declared);
	FMantleStorageEntry Unrelated;
	Unrelated.Name = TEXT("Tank");
	Unrelated.Item = ItemId(5);
	Unrelated.Version = 7;
	Level.Add(Unrelated);

	FMantleLibraryPublish::MergeIntoStorage(Level, Stored);
	if (!TestEqual(TEXT("one entry is added, none removed"), Level.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("the declared entry takes the published version"), Level[0].Version, 2);
	TestEqual(TEXT("and keeps its name"), Level[0].Name, FString(TEXT("MyTruck")));
	TestTrue(TEXT("the unrelated entry is untouched"), Level[1] == Unrelated);
	TestEqual(TEXT("the new item is added"), Level[2].Item, ItemId(2));
	TestEqual(TEXT("under its name"), Level[2].Name, FString(TEXT("Bus")));

	// A name another item holds is made unique, and the stored vehicle reports the name it got.
	TArray<FMantleStorageEntry> Taken;
	FMantleStorageEntry Holder;
	Holder.Name = TEXT("Bus");
	Holder.Item = ItemId(9);
	Taken.Add(Holder);
	TArray<FMantleStoredVehicle> Again = { Stored[1] };
	Again[0].StorageName = TEXT("Bus");
	FMantleLibraryPublish::MergeIntoStorage(Taken, Again);
	TestEqual(TEXT("the added entry gets a free name"), Taken.Last().Name, FString(TEXT("Bus_2")));
	TestEqual(TEXT("and the vehicle reports it"), Again[0].StorageName, FString(TEXT("Bus_2")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryPublishProvenanceTest,
	"Mantle.LibraryPublish.Requests.ProvenanceGoesWithPublicSubmissionsOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryPublishProvenanceTest::RunTest(const FString& Parameters)
{
	using namespace MantleLibraryPublishTestPrivate;
	const FMantleProvenance Provenance = FMantleProvenance::Make(TEXT("/Game/Cars/SM_Truck.SM_Truck"), TEXT("E:\\Bought\\Trucks\\SK_Truck.fbx"));
	TestEqual(TEXT("the vehicle's source path"), Provenance.SourcePath, FString(TEXT("/Game/Cars/SM_Truck")));
	TestEqual(TEXT("and its body mesh's file name, with no folders"), Provenance.ImportFilename, FString(TEXT("SK_Truck.fbx")));

	// A new public item.
	const TSharedPtr<FJsonObject> Public = ReadObject(FMantleLibraryPublish::MakeItemBody(TEXT("Truck"), true, AssetTruck, Provenance));
	const TSharedPtr<FJsonObject>* Sent = nullptr;
	if (!TestTrue(TEXT("a public item carries provenance"), Public.IsValid() && Public->TryGetObjectField(TEXT("provenance"), Sent) && Sent))
	{
		return false;
	}
	TestEqual(TEXT("sourcePath"), (*Sent)->GetStringField(TEXT("sourcePath")), FString(TEXT("/Game/Cars/SM_Truck")));
	TestEqual(TEXT("importFilename"), (*Sent)->GetStringField(TEXT("importFilename")), FString(TEXT("SK_Truck.fbx")));
	TestEqual(TEXT("and no other key"), (*Sent)->Values.Num(), 2);

	// A new private item: none.
	const TSharedPtr<FJsonObject> Private = ReadObject(FMantleLibraryPublish::MakeItemBody(TEXT("Truck"), false, AssetTruck, Provenance));
	TestTrue(TEXT("a private item is JSON"), Private.IsValid());
	TestFalse(TEXT("a private item sends no provenance"), Private.IsValid() && Private->HasField(TEXT("provenance")));

	// Nothing to send: absent.
	const TSharedPtr<FJsonObject> Bare = ReadObject(FMantleLibraryPublish::MakeItemBody(TEXT("Truck"), true, AssetTruck));
	TestFalse(TEXT("a public item with no provenance leaves the field out"), Bare.IsValid() && Bare->HasField(TEXT("provenance")));

	// A new version.
	const TSharedPtr<FJsonObject> PublicVersion = ReadObject(FMantleLibraryPublish::MakeVersionBody(AssetBus, true, Provenance));
	TestTrue(TEXT("a version of a public item carries provenance"), PublicVersion.IsValid() && PublicVersion->HasTypedField<EJson::Object>(TEXT("provenance")));
	TestEqual(TEXT("with the asset"), PublicVersion.IsValid() ? PublicVersion->GetStringField(TEXT("primaryAssetId")) : FString(), FString(AssetBus));
	const TSharedPtr<FJsonObject> PrivateVersion = ReadObject(FMantleLibraryPublish::MakeVersionBody(AssetBus, false, Provenance));
	TestTrue(TEXT("a version of a private item is JSON"), PrivateVersion.IsValid());
	TestFalse(TEXT("and sends no provenance"), PrivateVersion.IsValid() && PrivateVersion->HasField(TEXT("provenance")));
	if (PrivateVersion.IsValid())
	{
		TestEqual(TEXT("only the asset"), PrivateVersion->Values.Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLibraryPublishModerationTest,
	"Mantle.LibraryPublish.Requests.ModerationRefusalsAndTakenDownItemsAreExplained",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLibraryPublishModerationTest::RunTest(const FString& Parameters)
{
	FMantleApiResult Revoked;
	Revoked.StatusCode = 403;
	Revoked.Code = TEXT("public_publishing_revoked");
	Revoked.Message = TEXT("raw server text");
	const FString RevokedText = FMantleLibraryPublish::DescribeFailure(Revoked, TEXT("Truck"));
	TestTrue(TEXT("revoked: names the vehicle"), RevokedText.Contains(TEXT("'Truck'")));
	TestTrue(TEXT("revoked: says public publishing is off"), RevokedText.Contains(TEXT("Public publishing is turned off for this account after repeated copyright strikes.")));
	TestTrue(TEXT("revoked: says private still works"), RevokedText.Contains(TEXT("You can still publish privately.")));
	TestFalse(TEXT("revoked: not the raw text"), RevokedText.Contains(TEXT("raw server text")));

	FMantleApiResult Duplicate;
	Duplicate.StatusCode = 409;
	Duplicate.Code = TEXT("public_duplicate_claim");
	const FString DuplicateText = FMantleLibraryPublish::DescribeFailure(Duplicate, TEXT("Truck"));
	TestTrue(TEXT("duplicate: says another account was first"), DuplicateText.Contains(TEXT("Another account uploaded this content first.")));
	TestTrue(TEXT("duplicate: gives the two ways out"), DuplicateText.Contains(TEXT("Publish it privately, or contact support if it is yours.")));

	TestTrue(TEXT("taken_down is taken down"), FMantleLibraryPublish::IsTakenDown(TEXT("taken_down")));
	TestFalse(TEXT("delisted is not"), FMantleLibraryPublish::IsTakenDown(TEXT("delisted")));
	TestFalse(TEXT("nor is active"), FMantleLibraryPublish::IsTakenDown(TEXT("active")));
	TestFalse(TEXT("the status is case sensitive"), FMantleLibraryPublish::IsTakenDown(TEXT("Taken_Down")));
	TestFalse(TEXT("a taken-down item is not delisted (it is never replaced by a new one)"), FMantleLibraryPublish::IsDelisted(TEXT("taken_down")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
