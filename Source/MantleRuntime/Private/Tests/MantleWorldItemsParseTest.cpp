// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// World items' place format. settings.worldItems (persistence) and the ItemSpawn entity.
// Mirrors validate.py (and the schema) and the backend's validate.go.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleWorldItemsParseTestPrivate
{
	/** A place with the given settings body and entities (the text between the braces and the brackets). */
	FMantleParseResult ParsePlace(const FString& Settings, const FString& Entities)
	{
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_worlditems1\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"World items\",\"authorId\":\"usr_4b1d\"},\"settings\":{%s},\"assets\":[],\"entities\":[%s]}"),
			*Settings, *Entities);
		return FMantlePlaceParser::ParseFromString(Json);
	}

	/** One ItemSpawn entity at (10, 20, 30) turned 90 degrees, with the given properties body. */
	FMantleParseResult ParseSpawn(const FString& Properties)
	{
		return ParsePlace(FString(), FString::Printf(
			TEXT("{\"id\":\"ent_spawn\",\"type\":\"ItemSpawn\",\"name\":\"Spawn\",\"parent\":null,"
				 "\"transform\":{\"location\":[10,20,30],\"rotation\":[0,90,0],\"scale\":[1,1,1]},\"properties\":{%s}}"), *Properties));
	}

	bool HasErrorAt(const FMantleParseResult& Result, const FString& Path)
	{
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error) { return Error.Path == Path; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWorldItemsSettingsParseTest,
	"Mantle.Runtime.Place.Parser.WorldItemsSettings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWorldItemsSettingsParseTest::RunTest(const FString& Parameters)
{
	using namespace MantleWorldItemsParseTestPrivate;

	const FMantleParseResult Default = ParsePlace(FString(), FString());
	if (!TestTrue(TEXT("a place without settings.worldItems parses"), Default.bSuccess))
	{
		AddError(Default.ErrorSummary());
		return false;
	}
	TestTrue(TEXT("nothing is saved by default"), Default.Place.Settings.WorldItems.Persistence == EMantleWorldItemsPersistence::None);

	const FMantleParseResult None = ParsePlace(TEXT("\"worldItems\":{\"persistence\":\"none\"}"), FString());
	TestTrue(TEXT("persistence none parses"), None.bSuccess);
	TestTrue(TEXT("persistence none is none"), None.Place.Settings.WorldItems.Persistence == EMantleWorldItemsPersistence::None);

	const FMantleParseResult Server = ParsePlace(TEXT("\"worldItems\":{\"persistence\":\"server\"}"), FString());
	TestTrue(TEXT("persistence server parses"), Server.bSuccess);
	TestTrue(TEXT("persistence server is server"), Server.Place.Settings.WorldItems.Persistence == EMantleWorldItemsPersistence::Server);
	TestTrue(TEXT("an empty settings.worldItems parses"), ParsePlace(TEXT("\"worldItems\":{}"), FString()).bSuccess);

	TestTrue(TEXT("persistence place is refused"),
		HasErrorAt(ParsePlace(TEXT("\"worldItems\":{\"persistence\":\"place\"}"), FString()), TEXT("/settings/worldItems/persistence")));
	TestTrue(TEXT("persistence in another case is refused"),
		HasErrorAt(ParsePlace(TEXT("\"worldItems\":{\"persistence\":\"Server\"}"), FString()), TEXT("/settings/worldItems/persistence")));
	TestTrue(TEXT("an unknown key is refused"),
		HasErrorAt(ParsePlace(TEXT("\"worldItems\":{\"maxItems\":10}"), FString()), TEXT("/settings/worldItems/maxItems")));
	TestTrue(TEXT("settings.worldItems must be an object"),
		HasErrorAt(ParsePlace(TEXT("\"worldItems\":\"server\""), FString()), TEXT("/settings/worldItems")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleItemSpawnParseTest,
	"Mantle.Runtime.Place.Parser.ItemSpawn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleItemSpawnParseTest::RunTest(const FString& Parameters)
{
	using namespace MantleWorldItemsParseTestPrivate;

	const FMantleParseResult Minimal = ParseSpawn(TEXT("\"item\":\"bandage\""));
	if (!TestTrue(TEXT("an ItemSpawn with only an item parses"), Minimal.bSuccess))
	{
		AddError(Minimal.ErrorSummary());
		return false;
	}
	const FMantleItemSpawnProps* Props = Minimal.Place.Entities.Num() == 1 ? Minimal.Place.Entities[0].Props.TryGet<FMantleItemSpawnProps>() : nullptr;
	if (!TestNotNull(TEXT("the entity carries ItemSpawn properties"), Props))
	{
		return false;
	}
	TestEqual(TEXT("the item"), Props->Item, FString(TEXT("bandage")));
	TestEqual(TEXT("one by default"), Props->Count, 1);
	TestEqual(TEXT("60 s by default"), Props->RespawnSeconds, 60.0);
	TestTrue(TEXT("no model by default"), Props->Model.IsEmpty());
	TestTrue(TEXT("on by default"), Props->bEnabled);

	const FMantleParseResult Full = ParseSpawn(TEXT("\"item\":\"rifle_ammo\",\"count\":9999,\"respawnSeconds\":86400,\"model\":\"ast_crate_mesh\",\"enabled\":false"));
	if (TestTrue(TEXT("an ItemSpawn at the limits parses"), Full.bSuccess))
	{
		const FMantleItemSpawnProps* FullProps = Full.Place.Entities[0].Props.TryGet<FMantleItemSpawnProps>();
		TestEqual(TEXT("count at its limit"), FullProps->Count, FMantlePlaceLimits::MaxItemSpawnCount);
		TestEqual(TEXT("respawn at its limit"), FullProps->RespawnSeconds, FMantlePlaceLimits::MaxItemSpawnRespawnSeconds);
		TestEqual(TEXT("the model"), FullProps->Model, FString(TEXT("ast_crate_mesh")));
		TestFalse(TEXT("off"), FullProps->bEnabled);
	}
	TestTrue(TEXT("respawn at its least parses"), ParseSpawn(TEXT("\"item\":\"a\",\"respawnSeconds\":5")).bSuccess);

	TestTrue(TEXT("a missing item is refused"), HasErrorAt(ParseSpawn(TEXT("\"count\":1")), TEXT("/entities/0/properties/item")));
	TestTrue(TEXT("an item with a capital is refused"), HasErrorAt(ParseSpawn(TEXT("\"item\":\"Bandage\"")), TEXT("/entities/0/properties/item")));
	TestTrue(TEXT("an item starting with _ (reserved) is refused"), HasErrorAt(ParseSpawn(TEXT("\"item\":\"_bag\"")), TEXT("/entities/0/properties/item")));
	TestTrue(TEXT("an item with a space is refused"), HasErrorAt(ParseSpawn(TEXT("\"item\":\"big bandage\"")), TEXT("/entities/0/properties/item")));
	TestTrue(TEXT("an item over 48 characters is refused"),
		HasErrorAt(ParseSpawn(FString::Printf(TEXT("\"item\":\"%s\""), *FString::ChrN(49, TEXT('a')))), TEXT("/entities/0/properties/item")));
	TestTrue(TEXT("count 0 is refused"), HasErrorAt(ParseSpawn(TEXT("\"item\":\"a\",\"count\":0")), TEXT("/entities/0/properties/count")));
	TestTrue(TEXT("count 10000 is refused"), HasErrorAt(ParseSpawn(TEXT("\"item\":\"a\",\"count\":10000")), TEXT("/entities/0/properties/count")));
	TestTrue(TEXT("respawn under 5 s is refused"), HasErrorAt(ParseSpawn(TEXT("\"item\":\"a\",\"respawnSeconds\":4")), TEXT("/entities/0/properties/respawnSeconds")));
	TestTrue(TEXT("respawn over a day is refused"), HasErrorAt(ParseSpawn(TEXT("\"item\":\"a\",\"respawnSeconds\":86401")), TEXT("/entities/0/properties/respawnSeconds")));
	TestTrue(TEXT("a model that is not an asset id is refused"), HasErrorAt(ParseSpawn(TEXT("\"item\":\"a\",\"model\":\"crate\"")), TEXT("/entities/0/properties/model")));
	TestTrue(TEXT("an unknown key is refused"), HasErrorAt(ParseSpawn(TEXT("\"item\":\"a\",\"weapon\":\"x\"")), TEXT("/entities/0/properties/weapon")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
