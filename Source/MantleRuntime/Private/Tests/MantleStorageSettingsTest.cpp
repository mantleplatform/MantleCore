// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceStorage.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Package settings in place storage: the parser's and CheckEntries' rules for a storage entry's settings, the resolve
// answer's sealed values, which values a stored package runs with, and the manifest reader's option rules (choice, min, max,
// required, the settings list).

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleStorageSettingsTestPrivate
{
	const TCHAR* const SettingsTestItem = TEXT("itm_u0123456789abcdef0123456789abcdef");

	/** A complete place with no entities and Storage (a JSON array). */
	FString SettingsTestPlace(const FString& Storage)
	{
		return FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_settings_test\",\"revision\":{\"number\":1,\"state\":\"draft\"},")
				TEXT("\"metadata\":{\"name\":\"Settings test\",\"authorId\":\"usr_test\"},\"settings\":{},\"assets\":[],\"entities\":[],\"storage\":%s}"),
			*Storage);
	}

	/** One storage entry for KillBricks with Settings (the text of a JSON value, or empty for no key). */
	FString SettingsTestEntry(const FString& Settings)
	{
		return FString::Printf(TEXT("[{\"name\":\"KillBricks\",\"item\":\"%s\",\"version\":3%s}]"), SettingsTestItem,
			Settings.IsEmpty() ? TEXT("") : *(TEXT(",\"settings\":") + Settings));
	}

	bool SettingsTestHasErrorAt(const FMantleParseResult& Result, const FString& Path)
	{
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error) { return Error.Path.Equals(Path, ESearchCase::CaseSensitive); });
	}

	const FMantleScriptPackageParameter* SettingsTestFindOption(const TArray<FMantleScriptPackageParameter>& Options, const TCHAR* Name)
	{
		return Options.FindByPredicate([Name](const FMantleScriptPackageParameter& Option) { return Option.Name.Equals(Name, ESearchCase::CaseSensitive); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStorageSettingsParserTest,
	"Mantle.PlaceStorage.Settings.ParserReadsAndCheckEntriesHoldsTheShape",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStorageSettingsParserTest::RunTest(const FString& Parameters)
{
	using namespace MantleStorageSettingsTestPrivate;
	const FMantleParseResult Read = FMantlePlaceParser::ParseFromString(SettingsTestPlace(SettingsTestEntry(TEXT("{\"RoundLength\":\"300\",\"Mode\":\"Hard\"}"))));
	if (!TestTrue(TEXT("an entry with settings parses"), Read.bSuccess))
	{
		AddError(Read.ErrorSummary());
		return false;
	}
	if (TestEqual(TEXT("one entry"), Read.Place.Storage.Num(), 1))
	{
		const FMantleStorageEntry& Entry = Read.Place.Storage[0];
		TestEqual(TEXT("two settings"), Entry.Settings.Num(), 2);
		TestTrue(TEXT("each value as text"), Entry.FindSetting(TEXT("RoundLength")) && *Entry.FindSetting(TEXT("RoundLength")) == TEXT("300"));
		TestNull(TEXT("FindSetting is case-sensitive"), Entry.FindSetting(TEXT("roundlength")));

		// Settings count in an entry's equality.
		FMantleStorageEntry Other = Entry;
		TestTrue(TEXT("an entry equals its copy"), Other == Entry);
		Other.Settings.Add(TEXT("Mode"), TEXT("hard"));
		TestFalse(TEXT("a value that differs in case is a different entry"), Other == Entry);
	}
	TestTrue(TEXT("an empty settings object parses"), FMantlePlaceParser::ParseFromString(SettingsTestPlace(SettingsTestEntry(TEXT("{}")))).bSuccess);
	TestTrue(TEXT("an entry without settings still parses"), FMantlePlaceParser::ParseFromString(SettingsTestPlace(SettingsTestEntry(FString()))).bSuccess);

	const auto ExpectRefused = [this](const TCHAR* Case, const FString& Settings, const FString& Path)
	{
		const FMantleParseResult Result = FMantlePlaceParser::ParseFromString(SettingsTestPlace(SettingsTestEntry(Settings)));
		TestFalse(FString::Printf(TEXT("%s is refused"), Case), Result.bSuccess);
		TestTrue(FString::Printf(TEXT("%s is reported at %s (got: %s)"), Case, *Path, *Result.ErrorSummary()), SettingsTestHasErrorAt(Result, Path));
	};
	ExpectRefused(TEXT("settings that are not an object"), TEXT("[\"a\"]"), TEXT("/storage/0/settings"));
	ExpectRefused(TEXT("a value that is not text"), TEXT("{\"RoundLength\":300}"), TEXT("/storage/0/settings/RoundLength"));
	ExpectRefused(TEXT("a key that is not a setting name"), TEXT("{\"1bad\":\"x\"}"), TEXT("/storage/0/settings/1bad"));
	ExpectRefused(TEXT("a value longer than 256 characters"),
		FString::Printf(TEXT("{\"Title\":\"%s\"}"), *FString::ChrN(FMantlePlaceLimits::MaxStorageSettingValueLength + 1, TEXT('x'))), TEXT("/storage/0/settings/Title"));
	FString TooMany = TEXT("{");
	for (int32 Index = 0; Index <= FMantlePlaceLimits::MaxStorageSettings; ++Index)
	{
		TooMany += FString::Printf(TEXT("%s\"S%d\":\"v\""), Index == 0 ? TEXT("") : TEXT(","), Index);
	}
	TooMany += TEXT("}");
	ExpectRefused(TEXT("more than 16 settings"), TooMany, TEXT("/storage/0/settings"));

	// CheckEntries, which the exporter and the validator share, holds the same shape on entries built in memory.
	TArray<FMantleStorageEntry> Entries;
	FMantleStorageEntry& Entry = Entries.AddDefaulted_GetRef();
	Entry.Name = TEXT("KillBricks");
	Entry.Item = SettingsTestItem;
	Entry.Version = 1;
	Entry.Settings.Add(TEXT("Good"), TEXT("fine"));
	TestEqual(TEXT("good settings pass"), FMantleStorage::CheckEntries(Entries).Num(), 0);
	Entry.Settings.Add(TEXT("bad key"), TEXT("x"));
	Entry.Settings.Add(TEXT("Long"), FString::ChrN(FMantlePlaceLimits::MaxStorageSettingValueLength + 1, TEXT('x')));
	const TArray<FMantleStorageProblem> Problems = FMantleStorage::CheckEntries(Entries);
	TestTrue(TEXT("a bad key is named at its own path"), Problems.ContainsByPredicate([](const FMantleStorageProblem& Problem) { return Problem.Path == TEXT("/storage/0/settings/bad key"); }));
	TestTrue(TEXT("a long value is named at its own path"), Problems.ContainsByPredicate([](const FMantleStorageProblem& Problem)
	{
		return Problem.Path == TEXT("/storage/0/settings/Long") && Problem.Message.Contains(TEXT("longer than 256"));
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStorageSettingsResolveTest,
	"Mantle.PlaceStorage.Settings.ResolveAnswerCarriesTheSealedValues",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStorageSettingsResolveTest::RunTest(const FString& Parameters)
{
	using namespace MantleStorageSettingsTestPrivate;
	const FString Answer = FString::Printf(
		TEXT("{\"entries\":[{\"name\":\"KillBricks\",\"item\":\"%s\",\"version\":3,\"kind\":\"script\",\"status\":\"active\",")
			TEXT("\"primaryAssetId\":\"\",\"dependencyAssetIds\":[],\"scripts\":[],")
			TEXT("\"manifest\":{\"name\":\"Kill bricks\",\"description\":\"\",\"parameters\":[],")
				TEXT("\"settings\":[{\"name\":\"RoundLength\",\"type\":\"number\",\"default\":\"120\",\"help\":\"\",\"required\":false,\"min\":10,\"max\":600}]},")
			TEXT("\"settings\":{\"RoundLength\":\"300\",\"1bad\":\"x\",\"Count\":5}}]}"),
		SettingsTestItem);
	FMantleStorageExpansion Expansion;
	if (!TestTrue(TEXT("the answer reads"), FMantleStorage::ParseResponse(Answer, Expansion)) || !TestEqual(TEXT("one entry"), Expansion.Entries.Num(), 1))
	{
		return false;
	}
	const FMantleResolvedStorageEntry& Resolved = Expansion.Entries[0];
	TestTrue(TEXT("the answer carried settings"), Resolved.bHasSettings);
	TestEqual(TEXT("the well-formed one is kept, the rest left out"), Resolved.Settings.Num(), 1);
	TestEqual(TEXT("as text"), Resolved.Settings.FindRef(TEXT("RoundLength")), FString(TEXT("300")));
	if (TestEqual(TEXT("the manifest's settings list is read"), Resolved.Manifest.Settings.Num(), 1))
	{
		const FMantleScriptPackageParameter& Round = Resolved.Manifest.Settings[0];
		TestTrue(TEXT("with its range"), Round.bHasMin && Round.bHasMax && Round.Min == 10.0 && Round.Max == 600.0);
		TestTrue(TEXT("and its default"), Round.bHasDefault && Round.Default == TEXT("120"));
	}

	// The package a machine runs takes the sealed values over the place file's own.
	FMantlePlace Place;
	FMantleStorageEntry& Stored = Place.Storage.AddDefaulted_GetRef();
	Stored.Name = TEXT("KillBricks");
	Stored.Item = SettingsTestItem;
	Stored.Version = 3;
	Stored.Settings.Add(TEXT("RoundLength"), TEXT("60"));
	FMantleStorage::Apply(Place, Expansion);
	TArray<FMantleStoredScriptPackage> Packages = FMantleStorage::StoredScriptPackages(Place, true, false);
	if (TestEqual(TEXT("one stored package"), Packages.Num(), 1))
	{
		TestEqual(TEXT("it runs with the sealed value"), Packages[0].Settings.FindRef(TEXT("RoundLength")), FString(TEXT("300")));
	}

	// An older backend's answer carries no settings: the place file's entry is what runs.
	FMantleStorageExpansion Older = Expansion;
	Older.Entries[0].bHasSettings = false;
	Older.Entries[0].Settings.Reset();
	FMantleStorage::Apply(Place, Older);
	Packages = FMantleStorage::StoredScriptPackages(Place, true, false);
	if (TestEqual(TEXT("still one stored package"), Packages.Num(), 1))
	{
		TestEqual(TEXT("it runs with the place file's value"), Packages[0].Settings.FindRef(TEXT("RoundLength")), FString(TEXT("60")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStorageSettingsManifestTest,
	"Mantle.PlaceStorage.Settings.ManifestReaderHoldsTheOptionRules",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStorageSettingsManifestTest::RunTest(const FString& Parameters)
{
	using namespace MantleStorageSettingsTestPrivate;
	// package.json as a creator writes it on disk (JSON numbers and booleans) and the backend's text defaults side by side.
	const FMantleScriptPackageManifest Manifest = FMantleStorage::ReadScriptPackageManifestJson(
		TEXT("{\"name\":\"Kill bricks\",\"parameters\":[")
			TEXT("{\"name\":\"Mode\",\"type\":\"choice\",\"options\":[\"Easy\",\"Hard\",\"Easy\",\"\"],\"default\":\"Hard\"},")
			TEXT("{\"name\":\"Lonely\",\"type\":\"choice\",\"options\":[\"Only\"]},")
			TEXT("{\"name\":\"BadDefault\",\"type\":\"choice\",\"options\":[\"A\",\"B\"],\"default\":\"C\"},")
			TEXT("{\"name\":\"Speed\",\"type\":\"number\",\"min\":1,\"max\":10,\"default\":5},")
			TEXT("{\"name\":\"OutOfRange\",\"type\":\"number\",\"min\":1,\"max\":10,\"default\":\"50\"},")
			TEXT("{\"name\":\"Upside\",\"type\":\"number\",\"min\":10,\"max\":1,\"default\":5},")
			TEXT("{\"name\":\"Label\",\"type\":\"string\",\"min\":1,\"options\":[\"x\",\"y\"]},")
			TEXT("{\"name\":\"Target\",\"type\":\"string\",\"required\":true,\"default\":\"\"},")
			TEXT("{\"name\":\"Note\",\"type\":\"string\",\"default\":\"\"},")
			TEXT("{\"name\":\"speed\",\"type\":\"number\",\"default\":1}],")
		TEXT("\"settings\":[")
			TEXT("{\"name\":\"RoundLength\",\"type\":\"number\",\"default\":\"120\",\"required\":true},")
			TEXT("{\"name\":\"Hardcore\",\"type\":\"boolean\",\"default\":false}]}"));

	const FMantleScriptPackageParameter* Mode = SettingsTestFindOption(Manifest.Parameters, TEXT("Mode"));
	if (TestNotNull(TEXT("a choice reads"), Mode))
	{
		TestTrue(TEXT("with its options, repeats and empties left out"), Mode->Options.Num() == 2 && Mode->Options[0] == TEXT("Easy") && Mode->Options[1] == TEXT("Hard"));
		TestTrue(TEXT("and a default that is one of them"), Mode->bHasDefault && Mode->Default == TEXT("Hard"));
	}
	TestNull(TEXT("a choice with fewer than two options is left out"), SettingsTestFindOption(Manifest.Parameters, TEXT("Lonely")));
	const FMantleScriptPackageParameter* BadDefault = SettingsTestFindOption(Manifest.Parameters, TEXT("BadDefault"));
	TestTrue(TEXT("a choice's default that is none of its options is dropped"), BadDefault && !BadDefault->bHasDefault);
	const FMantleScriptPackageParameter* Speed = SettingsTestFindOption(Manifest.Parameters, TEXT("Speed"));
	TestTrue(TEXT("a number keeps its min, max and default"), Speed && Speed->bHasMin && Speed->Min == 1.0 && Speed->bHasMax && Speed->Max == 10.0
		&& Speed->bHasDefault && Speed->Default == TEXT("5"));
	const FMantleScriptPackageParameter* OutOfRange = SettingsTestFindOption(Manifest.Parameters, TEXT("OutOfRange"));
	TestTrue(TEXT("a default outside min and max is dropped"), OutOfRange && !OutOfRange->bHasDefault);
	const FMantleScriptPackageParameter* Upside = SettingsTestFindOption(Manifest.Parameters, TEXT("Upside"));
	TestTrue(TEXT("a min above max bounds nothing"), Upside && !Upside->bHasMin && !Upside->bHasMax && Upside->bHasDefault);
	const FMantleScriptPackageParameter* Label = SettingsTestFindOption(Manifest.Parameters, TEXT("Label"));
	TestTrue(TEXT("min and options mean nothing on a string"), Label && !Label->bHasMin && Label->Options.Num() == 0);
	const FMantleScriptPackageParameter* Target = SettingsTestFindOption(Manifest.Parameters, TEXT("Target"));
	TestTrue(TEXT("a required string's \"\" is no default"), Target && Target->bRequired && !Target->bHasDefault && Target->NeedsValue());
	const FMantleScriptPackageParameter* Note = SettingsTestFindOption(Manifest.Parameters, TEXT("Note"));
	TestTrue(TEXT("an optional string's \"\" is the empty default"), Note && Note->bHasDefault && Note->Default.IsEmpty() && !Note->NeedsValue());
	TestNull(TEXT("a name repeated in another letter case is left out"), SettingsTestFindOption(Manifest.Parameters, TEXT("speed")));

	if (TestEqual(TEXT("the settings list reads beside the parameters"), Manifest.Settings.Num(), 2))
	{
		const FMantleScriptPackageParameter* Round = Manifest.FindSetting(TEXT("RoundLength"));
		TestTrue(TEXT("a required setting with a default is satisfied by it"), Round && Round->bRequired && Round->bHasDefault && !Round->NeedsValue());
		const FMantleScriptPackageParameter* Hardcore = Manifest.FindSetting(TEXT("Hardcore"));
		TestTrue(TEXT("a JSON boolean default reads as its text"), Hardcore && Hardcore->bHasDefault && Hardcore->Default == TEXT("false"));
		TestNull(TEXT("settings are not parameters"), Manifest.FindParameter(TEXT("RoundLength")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
