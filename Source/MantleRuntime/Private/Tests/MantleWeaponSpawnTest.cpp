// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"
#include "MantleWeaponDocument.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// WeaponSpawn (#789 stage 3): the parser's weapon / storageItem rules, the 32 budget, the Weapon asset kind, and the
// weapon document's optional materials list.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleWeaponSpawnTestPrivate
{
	const TCHAR* const WeaponAsset = TEXT("ast_u7c1e0d2f9a8b4c3d5e6f708192a3b4c5");
	const TCHAR* const ItemGun = TEXT("itm_u11111111111111111111111111111111");

	FString SpawnEntity(const int32 Number, const FString& Properties)
	{
		return FString::Printf(
			TEXT("{\"id\":\"ent_w%d\",\"type\":\"WeaponSpawn\",\"name\":\"W%d\",\"parent\":null,")
				TEXT("\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},")
				TEXT("\"properties\":{%s}}"),
			Number, Number, *Properties);
	}

	FString PlaceJson(const FString& Assets, const FString& Entities, const FString& Storage)
	{
		const FString StorageKey = Storage.IsEmpty() ? FString() : TEXT(",\"storage\":") + Storage;
		return FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_weapon_test\",\"revision\":{\"number\":1,\"state\":\"draft\"},")
				TEXT("\"metadata\":{\"name\":\"Weapon test\",\"authorId\":\"usr_test\"},\"settings\":{},\"assets\":[%s],\"entities\":[%s]%s}"),
			*Assets, *Entities, *StorageKey);
	}

	FString WeaponAssetEntry()
	{
		return FString::Printf(TEXT("{\"id\":\"%s\",\"kind\":\"Weapon\",\"hash\":\"md5:0123456789abcdef\",\"bytes\":1024}"), WeaponAsset);
	}

	FString WeaponDocument(const FString& Extra)
	{
		return FString::Printf(
			TEXT("{\"schemaVersion\":1,\"name\":\"Test rifle\",\"archetype\":\"rifle\",\"mesh\":\"SKM_Test\"%s,")
				TEXT("\"sockets\":{\"sight\":{\"bone\":\"root\",\"location\":[1,0,2]},\"muzzle\":{\"bone\":\"root\",\"location\":[50,0,2]},")
				TEXT("\"gripR\":{\"bone\":\"root\",\"location\":[0,0,0]},\"gripL\":{\"bone\":\"root\",\"location\":[30,0,0]}},")
				TEXT("\"fire\":{\"interval\":0.1,\"automatic\":true,\"modes\":[\"semi\",\"auto\"],\"damage\":10,\"maxRange\":25000,")
				TEXT("\"traceSweepRadius\":5,\"pellets\":1,\"pelletSpread\":1,\"plainRecoil\":0.3},")
				TEXT("\"magazine\":{\"size\":30,\"startingReserve\":90},\"reload\":{\"style\":\"magazine\",\"seconds\":1.7}}"),
			*Extra);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponSpawnParseTest,
	"Mantle.WeaponSpawn.Parser.ReadsBothForms",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponSpawnParseTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponSpawnTestPrivate;
	const FString Storage = FString::Printf(TEXT("[{\"name\":\"Gun\",\"item\":\"%s\",\"version\":1}]"), ItemGun);
	const FString Entities = SpawnEntity(1, FString::Printf(TEXT("\"weapon\":\"%s\",\"respawnSeconds\":30,\"enabled\":false"), WeaponAsset))
		+ TEXT(",") + SpawnEntity(2, TEXT("\"storageItem\":\"Gun\""));
	const FMantleParseResult Result = FMantlePlaceParser::ParseFromString(PlaceJson(WeaponAssetEntry(), Entities, Storage));
	if (!TestTrue(TEXT("a place with weapon spawns parses"), Result.bSuccess))
	{
		AddError(Result.ErrorSummary());
		return false;
	}
	if (TestEqual(TEXT("one asset"), Result.Place.Assets.Num(), 1))
	{
		TestTrue(TEXT("the Weapon asset kind is read"), Result.Place.Assets[0].Kind == EMantleAssetKind::Weapon);
	}
	const FMantleWeaponSpawnProps* Direct = Result.Place.Entities[0].Props.TryGet<FMantleWeaponSpawnProps>();
	const FMantleWeaponSpawnProps* Stored = Result.Place.Entities[1].Props.TryGet<FMantleWeaponSpawnProps>();
	TestTrue(TEXT("type is WeaponSpawn"), Result.Place.Entities[0].Type == EMantleEntityType::WeaponSpawn);
	if (TestNotNull(TEXT("direct spawn"), Direct) && TestNotNull(TEXT("stored spawn"), Stored))
	{
		TestTrue(TEXT("weapon id"), Direct->Weapon.Equals(WeaponAsset, ESearchCase::CaseSensitive));
		TestEqual(TEXT("respawn"), Direct->RespawnSeconds, 30.0);
		TestFalse(TEXT("disabled"), Direct->bEnabled);
		TestEqual(TEXT("stored names its entry"), Stored->StorageItem, FString(TEXT("Gun")));
		TestTrue(TEXT("and has no weapon yet"), Stored->Weapon.IsEmpty());
		TestEqual(TEXT("default respawn"), Stored->RespawnSeconds, static_cast<double>(MANTLE_DEFAULT_WEAPON_RESPAWN_SECONDS));
		TestTrue(TEXT("enabled by default"), Stored->bEnabled);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponSpawnRefuseTest,
	"Mantle.WeaponSpawn.Parser.RefusesInvalid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponSpawnRefuseTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponSpawnTestPrivate;
	const FString Storage = FString::Printf(TEXT("[{\"name\":\"Gun\",\"item\":\"%s\",\"version\":1}]"), ItemGun);
	const FString Good = FString::Printf(TEXT("\"weapon\":\"%s\""), WeaponAsset);
	TestTrue(TEXT("the base place parses"), FMantlePlaceParser::ParseFromString(PlaceJson(WeaponAssetEntry(), SpawnEntity(1, Good), Storage)).bSuccess);

	auto Refused = [&](const FString& Properties, const FString& StorageJson)
	{
		return !FMantlePlaceParser::ParseFromString(PlaceJson(WeaponAssetEntry(), SpawnEntity(1, Properties), StorageJson)).bSuccess;
	};
	TestTrue(TEXT("both weapon and storageItem"), Refused(Good + TEXT(",\"storageItem\":\"Gun\""), Storage));
	TestTrue(TEXT("neither"), Refused(TEXT("\"enabled\":true"), Storage));
	TestTrue(TEXT("a built-in id is not an uploaded weapon"), Refused(TEXT("\"weapon\":\"wpn_rifle\""), Storage));
	TestTrue(TEXT("respawnSeconds over 600"), Refused(Good + TEXT(",\"respawnSeconds\":601"), Storage));
	TestTrue(TEXT("respawnSeconds below 0"), Refused(Good + TEXT(",\"respawnSeconds\":-1"), Storage));
	TestTrue(TEXT("an unknown key"), Refused(Good + TEXT(",\"livery\":1"), Storage));
	TestTrue(TEXT("storageItem not declared"), Refused(TEXT("\"storageItem\":\"Gun\""), FString()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponSpawnBudgetTest,
	"Mantle.WeaponSpawn.Parser.EnforcesBudgetOf32",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponSpawnBudgetTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponSpawnTestPrivate;
	const FString Props = FString::Printf(TEXT("\"weapon\":\"%s\""), WeaponAsset);
	auto Build = [&](const int32 Count)
	{
		FString Entities;
		for (int32 i = 0; i < Count; ++i)
		{
			Entities += (i ? TEXT(",") : TEXT("")) + SpawnEntity(i, Props);
		}
		return FMantlePlaceParser::ParseFromString(PlaceJson(WeaponAssetEntry(), Entities, FString()));
	};
	TestTrue(TEXT("32 spawns parse"), Build(FMantlePlaceLimits::MaxWeaponSpawnsPerPlace).bSuccess);
	const FMantleParseResult Over = Build(FMantlePlaceLimits::MaxWeaponSpawnsPerPlace + 1);
	TestFalse(TEXT("33 spawns are refused"), Over.bSuccess);
	TestTrue(TEXT("and the error names the 33rd"), Over.Errors.ContainsByPredicate([](const FMantleParseError& Error)
	{
		return Error.Path.Equals(TEXT("/entities/32/type"), ESearchCase::CaseSensitive);
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWeaponDocMaterialsTest,
	"Mantle.WeaponSpawn.Document.MaterialsRoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWeaponDocMaterialsTest::RunTest(const FString& Parameters)
{
	using namespace MantleWeaponSpawnTestPrivate;
	FMantleWeaponDesc Plain;
	FString Error;
	if (!TestTrue(TEXT("a document without materials parses"), FMantleWeaponDocument::Parse(WeaponDocument(FString()), Plain, Error)))
	{
		AddError(Error);
		return false;
	}
	TestFalse(TEXT("and leaves materials unset"), Plain.Materials.bSet);
	TestFalse(TEXT("the writer writes none"), FMantleWeaponDocument::Write(Plain).Contains(TEXT("\"materials\"")));

	FMantleWeaponDesc With;
	if (!TestTrue(TEXT("a document with materials parses"),
		FMantleWeaponDocument::Parse(WeaponDocument(TEXT(",\"materials\":[\"M_Body\",\"ast_u7c1e0d2f9a8b4c3d5e6f708192a3b4c5\"]")), With, Error)))
	{
		AddError(Error);
		return false;
	}
	if (TestTrue(TEXT("materials are set"), With.Materials.bSet) && TestEqual(TEXT("two names"), With.Materials.Value.Num(), 2))
	{
		TestEqual(TEXT("slot 0"), With.Materials.Value[0], FString(TEXT("M_Body")));
	}
	const FString Written = FMantleWeaponDocument::Write(With);
	FMantleWeaponDesc Again;
	if (TestTrue(TEXT("the written text parses back"), FMantleWeaponDocument::Parse(Written, Again, Error)))
	{
		TestTrue(TEXT("with the same materials"), Again.Materials.bSet && Again.Materials.Value == With.Materials.Value);
		TestEqual(TEXT("and writes the same text"), FMantleWeaponDocument::Write(Again), Written);
	}

	// Strict: a path, a non-array, a non-string entry and more than 64 names are refused.
	FMantleWeaponDesc Refused;
	TestFalse(TEXT("a path is not a name"), FMantleWeaponDocument::Parse(WeaponDocument(TEXT(",\"materials\":[\"/Game/M\"]")), Refused, Error));
	TestFalse(TEXT("an object is not a list"), FMantleWeaponDocument::Parse(WeaponDocument(TEXT(",\"materials\":{}")), Refused, Error));
	TestFalse(TEXT("a number is not a name"), FMantleWeaponDocument::Parse(WeaponDocument(TEXT(",\"materials\":[1]")), Refused, Error));
	FString Many;
	for (int32 i = 0; i <= FMantlePlaceLimits::MaxWeaponMaterials; ++i)
	{
		Many += (i ? TEXT(",\"M") : TEXT("\"M")) + FString::FromInt(i) + TEXT("\"");
	}
	TestFalse(TEXT("65 names are refused"), FMantleWeaponDocument::Parse(WeaponDocument(TEXT(",\"materials\":[") + Many + TEXT("]")), Refused, Error));
	return true;
}

#endif
