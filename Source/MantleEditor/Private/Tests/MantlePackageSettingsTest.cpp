// Copyright Joshua Eger. All Rights Reserved.
#include "MantleApiManifest.h"
#include "MantleLibraryPublish.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceSettingsActor.h"
#include "MantlePlaceStorage.h"
#include "MantlePlaceTypes.h"
#include "MantlePlaceValidator.h"
#include "MantlePreviewExport.h"
#include "MantleScriptOptionRows.h"
#include "MantleScriptPackages.h"
#include "MantleScriptPoint.h"

#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// Package settings in the editor: package.json's settings and richer option types, the plain-words checks the
// validator and the exporter share, the storage entry's settings written and refused, the scriptApi floor, the option rows' pure
// model, and the publish paths that keep a creator's settings. Pure, or on a transient editor world.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePackageSettingsTestPrivate
{
	const TCHAR* const PkgSettingsManifestJson =
		TEXT("{\"name\": \"Kill Bricks\", \"description\": \"Touch one and respawn.\",")
		TEXT(" \"parameters\": [")
		TEXT("{\"name\": \"Mode\", \"type\": \"choice\", \"options\": [\"Slow\", \"Fast\"], \"default\": \"Slow\"},")
		TEXT("{\"name\": \"Speed\", \"type\": \"number\", \"min\": 1, \"max\": 10, \"default\": 2},")
		TEXT("{\"name\": \"Target\", \"type\": \"string\", \"required\": true}],")
		TEXT(" \"settings\": [")
		TEXT("{\"name\": \"RoundLength\", \"type\": \"number\", \"min\": 10, \"max\": 600, \"required\": true, \"help\": \"Seconds per round.\"},")
		TEXT("{\"name\": \"Hardcore\", \"type\": \"boolean\", \"default\": false},")
		TEXT("{\"name\": \"Difficulty\", \"type\": \"choice\", \"options\": [\"Easy\", \"Hard\"], \"default\": \"Easy\"}]}");

	FMantleScriptPackageManifest PkgSettingsManifest()
	{
		FMantleScriptPackageManifest Manifest;
		TArray<FString> Problems;
		FMantleScriptPackages::ReadManifest(PkgSettingsManifestJson, Manifest, Problems);
		return Manifest;
	}

	FMantleStorageSetting PkgSettingsEntry(const TCHAR* Name, const int32 Number, const TCHAR* Kind, const FString& Manifest = FString())
	{
		FMantleStorageSetting Setting;
		Setting.Name = Name;
		Setting.Item = FString::Printf(TEXT("itm_u%032x"), Number);
		Setting.Version = 1;
		Setting.Kind = Kind;
		Setting.Manifest = Manifest;
		return Setting;
	}

	/** An empty editor world for one test, destroyed with it. */
	struct FPkgSettingsWorld
	{
		UWorld* World = nullptr;

		FPkgSettingsWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantlePackageSettingsTestWorld"));
		}

		~FPkgSettingsWorld()
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

		AMantlePlaceSettingsActor* AddSettings(const TArray<FMantleStorageSetting>& Storage) const
		{
			AMantlePlaceSettingsActor* Settings = World->SpawnActor<AMantlePlaceSettingsActor>();
			if (Settings)
			{
				Settings->Storage = Storage;
			}
			return Settings;
		}

		AMantleScriptPoint* AddPoint(const TCHAR* Label, const FString& StorageItem) const
		{
			AMantleScriptPoint* Point = World->SpawnActor<AMantleScriptPoint>(FVector(100.0, 0.0, 0.0), FRotator::ZeroRotator);
			if (Point)
			{
				Point->SetActorLabel(Label);
				Point->StorageItem = StorageItem;
			}
			return Point;
		}
	};

	bool PkgSettingsHasError(const FMantleExportResult& Result, const FString& Path, const TCHAR* Contains)
	{
		return Result.Errors.ContainsByPredicate([&Path, Contains](const FMantleParseError& Error)
		{
			return Error.Path.Equals(Path, ESearchCase::CaseSensitive) && Error.Message.Contains(Contains, ESearchCase::CaseSensitive);
		});
	}

	bool PkgSettingsHasIssue(const FMantleValidationResult& Result, const TCHAR* Contains)
	{
		return Result.Issues.ContainsByPredicate([Contains](const FMantleValidationIssue& Issue)
		{
			return Issue.Severity == EMantleIssueSeverity::Error && Issue.Problem.Contains(Contains, ESearchCase::CaseSensitive);
		});
	}

	bool PkgSettingsHasProblem(const TArray<FString>& Problems, const TCHAR* Contains)
	{
		return Problems.ContainsByPredicate([Contains](const FString& Problem) { return Problem.Contains(Contains, ESearchCase::CaseSensitive); });
	}

	TSharedPtr<FJsonObject> PkgSettingsReadObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Root;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root);
		return Root;
	}

	/** A place file fit to parse around Place's content. */
	void PkgSettingsMakeParseable(FMantlePlace& Place)
	{
		Place.SchemaVersion = MANTLE_PLACE_SCHEMA_VERSION;
		Place.PlaceId = TEXT("plc_pkgsettings01");
		Place.Revision.Number = 1;
		Place.Revision.State = EMantleRevisionState::Draft;
		Place.Metadata.Name = TEXT("Package settings");
		Place.Metadata.AuthorId = TEXT("usr_test");
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePackageSettingsManifestReadsTest,
	"Mantle.ScriptPackages.Settings.ManifestReadsSettingsAndRicherOptions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePackageSettingsManifestReadsTest::RunTest(const FString& Parameters)
{
	using namespace MantlePackageSettingsTestPrivate;
	FMantleScriptPackageManifest Manifest;
	TArray<FString> Problems;
	if (!TestTrue(TEXT("a package.json with settings and richer options reads"), FMantleScriptPackages::ReadManifest(PkgSettingsManifestJson, Manifest, Problems)))
	{
		AddError(FString::Join(Problems, TEXT("\n")));
		return false;
	}
	if (!TestEqual(TEXT("three parameters"), Manifest.Parameters.Num(), 3) || !TestEqual(TEXT("three settings"), Manifest.Settings.Num(), 3))
	{
		return false;
	}
	const FMantleScriptPackageParameter* Mode = Manifest.FindParameter(TEXT("Mode"));
	TestTrue(TEXT("a choice with its options and default"), Mode && Mode->Type == EMantleScriptPackageParameterType::Choice && Mode->Options.Num() == 2 && Mode->Default == TEXT("Slow"));
	const FMantleScriptPackageParameter* Speed = Manifest.FindParameter(TEXT("Speed"));
	TestTrue(TEXT("a number with min and max"), Speed && Speed->bHasMin && Speed->Min == 1.0 && Speed->bHasMax && Speed->Max == 10.0);
	const FMantleScriptPackageParameter* Target = Manifest.FindParameter(TEXT("Target"));
	TestTrue(TEXT("a required string with no default"), Target && Target->bRequired && Target->NeedsValue());
	const FMantleScriptPackageParameter* Round = Manifest.FindSetting(TEXT("RoundLength"));
	TestTrue(TEXT("a required number needs no default"), Round && Round->bRequired && !Round->bHasDefault);

	// What the API takes, and what it answers read back the same.
	const TSharedPtr<FJsonObject> Body = PkgSettingsReadObject(FMantleScriptPackages::ManifestToJson(Manifest));
	const TArray<TSharedPtr<FJsonValue>>* Settings = nullptr;
	if (TestTrue(TEXT("the manifest writes settings"), Body.IsValid() && Body->TryGetArrayField(TEXT("settings"), Settings) && Settings->Num() == 3))
	{
		const TSharedPtr<FJsonObject> First = (*Settings)[0]->AsObject();
		TestTrue(TEXT("with required"), First->GetBoolField(TEXT("required")));
		TestEqual(TEXT("min as a number"), First->GetNumberField(TEXT("min")), 10.0);
		TestEqual(TEXT("max as a number"), First->GetNumberField(TEXT("max")), 600.0);
		TestEqual(TEXT("no default as \"\""), First->GetStringField(TEXT("default")), FString());
		TestFalse(TEXT("no options on a number"), First->HasField(TEXT("options")));
		TestEqual(TEXT("a choice writes its options"), (*Settings)[2]->AsObject()->GetArrayField(TEXT("options")).Num(), 2);
	}
	const FMantleScriptPackageManifest Hints = FMantleScriptPackages::ReadManifestForHints(FMantleScriptPackages::ManifestToJson(Manifest));
	TestEqual(TEXT("the hints read every setting back"), Hints.Settings.Num(), 3);
	TestEqual(TEXT("and write back identically"), FMantleScriptPackages::ManifestToJson(Hints), FMantleScriptPackages::ManifestToJson(Manifest));

	// A republish that only changes a setting is a new version.
	TArray<FMantleLibraryScriptFile> Files;
	const FString Detail = FString::Printf(TEXT("{\"id\":\"itm_u%032x\",\"versions\":[{\"version\":4,\"scripts\":[],\"manifest\":%s}]}"),
		7, *FMantleScriptPackages::ManifestToJson(Manifest));
	int32 Found = 0;
	TestTrue(TEXT("the unchanged version is found"), FMantleLibraryPublish::FindVersionWithScripts(Detail, Manifest, Files, Found) && Found == 4);
	FMantleScriptPackageManifest Changed = Manifest;
	Changed.Settings[0].Max = 900.0;
	TestFalse(TEXT("a changed setting's max is a new version"), FMantleLibraryPublish::FindVersionWithScripts(Detail, Changed, Files, Found));
	Changed = Manifest;
	Changed.Settings.RemoveAt(1);
	TestFalse(TEXT("a removed setting is a new version"), FMantleLibraryPublish::FindVersionWithScripts(Detail, Changed, Files, Found));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePackageSettingsManifestRefusesTest,
	"Mantle.ScriptPackages.Settings.ManifestRefusesBadOptionsInPlainWords",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePackageSettingsManifestRefusesTest::RunTest(const FString& Parameters)
{
	using namespace MantlePackageSettingsTestPrivate;
	FString TooManySettings = TEXT("{\"name\": \"Many\", \"settings\": [");
	for (int32 Index = 0; Index < FMantlePlaceLimits::MaxStorageSettings + 1; ++Index)
	{
		TooManySettings += FString::Printf(TEXT("%s{\"name\": \"S%d\", \"type\": \"string\"}"), Index == 0 ? TEXT("") : TEXT(","), Index);
	}
	TooManySettings += TEXT("]}");

	const TArray<TPair<FString, FString>> Cases = {
		{ TooManySettings, TEXT("declares 17 settings; a package declares at most 16") },
		{ TEXT("{\"name\": \"N\", \"settings\": [{\"name\": \"A\", \"type\": \"vector\"}]}"), TEXT("\"string\", \"number\", \"boolean\" or \"choice\"") },
		{ TEXT("{\"name\": \"N\", \"settings\": [{\"name\": \"Speed\", \"type\": \"string\"}, {\"name\": \"speed\", \"type\": \"string\"}]}"), TEXT("unique ignoring letter case") },
		{ TEXT("{\"name\": \"N\", \"settings\": [{\"name\": \"A\", \"type\": \"choice\", \"options\": [\"Only\"], \"required\": true}]}"), TEXT("lists 1 options; a choice lists 2 to 32") },
		{ TEXT("{\"name\": \"N\", \"settings\": [{\"name\": \"A\", \"type\": \"choice\", \"options\": [\"X\", \"Y\"], \"default\": \"Z\"}]}"), TEXT("is not one of its options") },
		{ TEXT("{\"name\": \"N\", \"settings\": [{\"name\": \"A\", \"type\": \"choice\", \"options\": [\"X\", \"X\"], \"default\": \"X\"}]}"), TEXT("lists the option 'X' twice") },
		{ TEXT("{\"name\": \"N\", \"settings\": [{\"name\": \"A\", \"type\": \"string\", \"min\": 1}]}"), TEXT("only a number setting has one") },
		{ TEXT("{\"name\": \"N\", \"settings\": [{\"name\": \"A\", \"type\": \"string\", \"options\": [\"X\", \"Y\"]}]}"), TEXT("only a choice setting has them") },
		{ TEXT("{\"name\": \"N\", \"settings\": [{\"name\": \"A\", \"type\": \"number\", \"min\": 10, \"max\": 1, \"default\": 5}]}"), TEXT("has a max (1) below its min (10)") },
		{ TEXT("{\"name\": \"N\", \"settings\": [{\"name\": \"A\", \"type\": \"number\", \"min\": 1, \"max\": 10, \"default\": 50}]}"), TEXT("outside its min and max") },
		{ TEXT("{\"name\": \"N\", \"settings\": [{\"name\": \"A\", \"type\": \"number\", \"max\": \"10\", \"default\": 5}]}"), TEXT("has a max that is not a number") },
		{ TEXT("{\"name\": \"N\", \"settings\": [{\"name\": \"A\", \"type\": \"boolean\", \"required\": \"yes\", \"default\": true}]}"), TEXT("required that is not true or false") },
		{ TEXT("{\"name\": \"N\", \"settings\": [{\"name\": \"A\", \"type\": \"choice\", \"options\": [\"X\", \"Y\"]}]}"), TEXT("has no default") },
	};
	for (const TPair<FString, FString>& Case : Cases)
	{
		FMantleScriptPackageManifest Manifest;
		TArray<FString> Problems;
		TestFalse(FString::Printf(TEXT("refused: %s"), *Case.Value), FMantleScriptPackages::ReadManifest(Case.Key, Manifest, Problems));
		TestTrue(FString::Printf(TEXT("saying '%s' (got: %s)"), *Case.Value, *FString::Join(Problems, TEXT(" | "))), PkgSettingsHasProblem(Problems, *Case.Value));
	}

	// A required number, boolean or choice needs no default: the place must set it.
	FMantleScriptPackageManifest Manifest;
	TArray<FString> Problems;
	TestTrue(TEXT("a required choice without a default reads"), FMantleScriptPackages::ReadManifest(
		TEXT("{\"name\": \"N\", \"settings\": [{\"name\": \"A\", \"type\": \"choice\", \"options\": [\"X\", \"Y\"], \"required\": true}]}"), Manifest, Problems));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePackageSettingsValuesTest,
	"Mantle.ScriptPackages.Settings.ValuesAreCheckedInPlainWords",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePackageSettingsValuesTest::RunTest(const FString& Parameters)
{
	using namespace MantlePackageSettingsTestPrivate;
	const FMantleScriptPackageManifest Manifest = PkgSettingsManifest();
	auto Messages = [](const TArray<FMantleScriptOptionIssue>& Issues)
	{
		TArray<FString> Out;
		for (const FMantleScriptOptionIssue& Issue : Issues)
		{
			Out.Add(Issue.Message);
		}
		return Out;
	};

	TMap<FString, FString> Good;
	Good.Add(TEXT("RoundLength"), TEXT("300"));
	Good.Add(TEXT("Difficulty"), TEXT("Hard"));
	TestEqual(TEXT("fitting values pass"), FMantleScriptPackages::CheckOptionValues(TEXT("KillBricks"), Manifest.Settings, Good, TEXT("setting")).Num(), 0);

	const TArray<FMantleScriptOptionIssue> Missing = FMantleScriptPackages::CheckOptionValues(TEXT("KillBricks"), Manifest.Settings, TMap<FString, FString>(), TEXT("setting"));
	if (TestEqual(TEXT("a required setting with no default left out is one problem"), Missing.Num(), 1))
	{
		TestEqual(TEXT("in plain words"), Missing[0].Message, FString(TEXT("KillBricks needs RoundLength.")));
		TestTrue(TEXT("of the required kind"), Missing[0].Problem == EMantleScriptOptionProblem::Required);
	}

	TMap<FString, FString> Bad;
	Bad.Add(TEXT("RoundLength"), TEXT("5"));
	Bad.Add(TEXT("Hardcore"), TEXT("yes"));
	Bad.Add(TEXT("Difficulty"), TEXT("Medium"));
	Bad.Add(TEXT("roundlenght"), TEXT("1"));
	Bad.Add(TEXT("hardCORE2"), TEXT("1"));
	const TArray<FMantleScriptOptionIssue> Issues = FMantleScriptPackages::CheckOptionValues(TEXT("KillBricks"), Manifest.Settings, Bad, TEXT("setting"));
	const TArray<FString> Said = Messages(Issues);
	TestTrue(TEXT("out of range"), Said.Contains(TEXT("KillBricks's RoundLength must be between 10 and 600.")));
	TestTrue(TEXT("not a boolean"), PkgSettingsHasProblem(Said, TEXT("KillBricks's Hardcore must be true or false")));
	TestTrue(TEXT("not an option"), PkgSettingsHasProblem(Said, TEXT("KillBricks's Difficulty must be one of Easy, Hard")));
	TestTrue(TEXT("an unknown key"), PkgSettingsHasProblem(Said, TEXT("KillBricks has no setting called roundlenght")));
	TestTrue(TEXT("each problem names its key"), Issues.ContainsByPredicate([](const FMantleScriptOptionIssue& Issue)
	{
		return Issue.Key == TEXT("Difficulty") && Issue.Problem == EMantleScriptOptionProblem::Invalid;
	}));

	TMap<FString, FString> NearMiss;
	NearMiss.Add(TEXT("hardcore"), TEXT("true"));
	const TArray<FString> NearSaid = Messages(FMantleScriptPackages::CheckOptionValues(TEXT("KillBricks"), Manifest.Settings, NearMiss, TEXT("setting")));
	TestTrue(TEXT("a key in another letter case says so"), PkgSettingsHasProblem(NearSaid, TEXT("Did you mean Hardcore? Names match letter case exactly.")));

	TMap<FString, FString> Words;
	Words.Add(TEXT("Speed"), TEXT("fast"));
	TestTrue(TEXT("a parameter that is not a number"), PkgSettingsHasProblem(
		Messages(FMantleScriptPackages::CheckOptionValues(TEXT("KillBricks"), Manifest.Parameters, Words, TEXT("parameter"))), TEXT("KillBricks's Speed must be a number")));
	TestTrue(TEXT("a package with no settings refuses any"), PkgSettingsHasProblem(
		Messages(FMantleScriptPackages::CheckOptionValues(TEXT("Plain"), TArray<FMantleScriptPackageParameter>(), Good, TEXT("setting"))), TEXT("Plain has no settings")));

	double Number = 0.0;
	TestTrue(TEXT("a number reads"), FMantleScriptPackages::ParseOptionNumber(TEXT("-12.5e1"), Number) && Number == -125.0);
	TestFalse(TEXT("surrounding spaces are not a number (the backend's rule)"), FMantleScriptPackages::ParseOptionNumber(TEXT(" 5"), Number));
	TestFalse(TEXT("nor is text"), FMantleScriptPackages::ParseOptionNumber(TEXT("5x"), Number));
	TestEqual(TEXT("a whole bound is written without .0"), FMantleScriptPackages::FormatOptionNumber(10.0), FString(TEXT("10")));

	// A new version that drops a setting drops its value from the entry.
	TMap<FString, FString> Kept;
	Kept.Add(TEXT("RoundLength"), TEXT("300"));
	Kept.Add(TEXT("Gone"), TEXT("x"));
	const TArray<FString> Dropped = FMantleScriptPackages::PruneSettings(Kept, Manifest);
	TestTrue(TEXT("the undeclared key is dropped and named"), Dropped.Num() == 1 && Dropped[0] == TEXT("Gone") && Kept.Num() == 1 && Kept.Contains(TEXT("RoundLength")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePackageSettingsExportTest,
	"Mantle.ScriptPackages.Settings.ExportWritesSettingsAndRefusesBadValues",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePackageSettingsExportTest::RunTest(const FString& Parameters)
{
	using namespace MantlePackageSettingsTestPrivate;
	const FString ManifestJson = FMantleScriptPackages::ManifestToJson(PkgSettingsManifest());
	FPkgSettingsWorld Fixture;
	AMantlePlaceSettingsActor* Settings = Fixture.World ? Fixture.AddSettings({
		PkgSettingsEntry(TEXT("KillBricks"), 1, TEXT("script"), ManifestJson), PkgSettingsEntry(TEXT("Truck"), 2, TEXT("vehicle")) }) : nullptr;
	if (!TestNotNull(TEXT("a world with place settings"), Settings))
	{
		return false;
	}
	Settings->Storage[0].Settings.Add(TEXT("RoundLength"), TEXT("300"));
	Settings->Storage[0].Settings.Add(TEXT("Difficulty"), TEXT("Hard"));

	const FMantleExportResult Fine = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestFalse(TEXT("fitting settings are not refused"), Fine.Errors.ContainsByPredicate([](const FMantleParseError& Error) { return Error.Path.StartsWith(TEXT("/storage")); }));
	TestEqual(TEXT("a script storage entry raises scriptApi to 1.36"), Fine.Place.ScriptApi.Minor, FMath::Min(36, FMantleApiManifest::Get().Minor));
	TestEqual(TEXT("the package floor is 1.36"), FMantleScriptPackages::MinScriptApiMinor, 36);
	if (!TestEqual(TEXT("two storage entries"), Fine.Place.Storage.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("the entry carries its settings"), Fine.Place.Storage[0].Settings.Num(), 2);
	TestEqual(TEXT("the vehicle carries none"), Fine.Place.Storage[1].Settings.Num(), 0);

	// What goes out: settings on the script entry only, and it parses back equal.
	FMantlePlace Place = Fine.Place;
	PkgSettingsMakeParseable(Place);
	const FString Json = FMantlePlaceExporter::ToJson(Place);
	const TSharedPtr<FJsonObject> Root = PkgSettingsReadObject(Json);
	const TArray<TSharedPtr<FJsonValue>>* Storage = nullptr;
	if (TestTrue(TEXT("the file has storage"), Root.IsValid() && Root->TryGetArrayField(TEXT("storage"), Storage) && Storage->Num() == 2))
	{
		const TSharedPtr<FJsonObject>* Written = nullptr;
		TestTrue(TEXT("the script entry writes settings"), (*Storage)[0]->AsObject()->TryGetObjectField(TEXT("settings"), Written)
			&& (*Written)->GetStringField(TEXT("RoundLength")) == TEXT("300"));
		TestFalse(TEXT("the vehicle entry writes no settings key"), (*Storage)[1]->AsObject()->HasField(TEXT("settings")));
	}
	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
	if (TestTrue(TEXT("what the exporter wrote parses"), Parsed.bSuccess) && TestEqual(TEXT("both entries read back"), Parsed.Place.Storage.Num(), 2))
	{
		TestTrue(TEXT("equal, settings included"), Parsed.Place.Storage[0] == Fine.Place.Storage[0]);
	}
	else
	{
		AddError(Parsed.ErrorSummary());
	}

	// Values that do not fit, a key the package does not declare, and settings on a vehicle.
	Settings->Storage[0].Settings.Reset();
	Settings->Storage[0].Settings.Add(TEXT("RoundLength"), TEXT("5"));
	Settings->Storage[0].Settings.Add(TEXT("Difficulty"), TEXT("Medium"));
	Settings->Storage[0].Settings.Add(TEXT("Speedd"), TEXT("1"));
	Settings->Storage[1].Settings.Add(TEXT("Colour"), TEXT("Red"));
	const FMantleExportResult Bad = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestTrue(TEXT("out of range"), PkgSettingsHasError(Bad, TEXT("/storage/0/settings/RoundLength"), TEXT("KillBricks's RoundLength must be between 10 and 600.")));
	TestTrue(TEXT("not one of the options"), PkgSettingsHasError(Bad, TEXT("/storage/0/settings/Difficulty"), TEXT("must be one of Easy, Hard")));
	TestTrue(TEXT("an unknown key"), PkgSettingsHasError(Bad, TEXT("/storage/0/settings/Speedd"), TEXT("has no setting called Speedd")));
	TestTrue(TEXT("settings on a vehicle"), PkgSettingsHasError(Bad, TEXT("/storage/1/settings"), TEXT("only a script package takes settings")));

	// A required setting with no default, left out.
	Settings->Storage[0].Settings.Reset();
	Settings->Storage[1].Settings.Reset();
	const FMantleExportResult Missing = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestTrue(TEXT("a missing required setting"), PkgSettingsHasError(Missing, TEXT("/storage/0/settings/RoundLength"), TEXT("KillBricks needs RoundLength.")));

	// Script Point parameters by the same rules.
	Settings->Storage[0].Settings.Add(TEXT("RoundLength"), TEXT("300"));
	AMantleScriptPoint* Point = Fixture.AddPoint(TEXT("Brick"), TEXT("KillBricks"));
	if (TestNotNull(TEXT("a point"), Point))
	{
		Point->Parameters.Add(TEXT("Mode"), TEXT("Medium"));
		Point->Parameters.Add(TEXT("Speed"), TEXT("50"));
		const FMantleExportResult Points = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
		auto HasPointError = [&Points](const TCHAR* Key, const TCHAR* Contains)
		{
			return Points.Errors.ContainsByPredicate([Key, Contains](const FMantleParseError& Error)
			{
				return Error.Path.EndsWith(FString(TEXT("/properties/parameters/")) + Key, ESearchCase::CaseSensitive) && Error.Message.Contains(Contains, ESearchCase::CaseSensitive);
			});
		};
		TestTrue(TEXT("a choice parameter that is none of its options"), HasPointError(TEXT("Mode"), TEXT("KillBricks's Mode must be one of Slow, Fast")));
		TestTrue(TEXT("a number parameter out of range"), HasPointError(TEXT("Speed"), TEXT("KillBricks's Speed must be between 1 and 10.")));
		TestTrue(TEXT("a required parameter left out"), HasPointError(TEXT("Target"), TEXT("KillBricks needs Target.")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePackageSettingsValidatorTest,
	"Mantle.ScriptPackages.Settings.CheckThisPlaceRefusesWhatPublishWould",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePackageSettingsValidatorTest::RunTest(const FString& Parameters)
{
	using namespace MantlePackageSettingsTestPrivate;
	const FString ManifestJson = FMantleScriptPackages::ManifestToJson(PkgSettingsManifest());
	FPkgSettingsWorld Fixture;
	AMantlePlaceSettingsActor* Settings = Fixture.World ? Fixture.AddSettings({
		PkgSettingsEntry(TEXT("KillBricks"), 1, TEXT("script"), ManifestJson), PkgSettingsEntry(TEXT("Truck"), 2, TEXT("vehicle")) }) : nullptr;
	if (!TestNotNull(TEXT("a world with place settings"), Settings))
	{
		return false;
	}
	Settings->Storage[0].Settings.Add(TEXT("Hardcore"), TEXT("maybe"));
	Settings->Storage[1].Settings.Add(TEXT("Colour"), TEXT("Red"));
	if (AMantleScriptPoint* Point = Fixture.AddPoint(TEXT("Brick"), TEXT("KillBricks")))
	{
		Point->Parameters.Add(TEXT("Target"), TEXT("Spawn"));
		Point->Parameters.Add(TEXT("Speed"), TEXT("0"));
		Point->Parameters.Add(TEXT("Colour"), TEXT("Red"));
	}

	const FMantleValidationResult Result = FMantlePlaceValidator::Validate(Fixture.World);
	TestTrue(TEXT("a missing required setting"), PkgSettingsHasIssue(Result, TEXT("KillBricks needs RoundLength.")));
	TestTrue(TEXT("a wrong-typed setting"), PkgSettingsHasIssue(Result, TEXT("KillBricks's Hardcore must be true or false")));
	TestTrue(TEXT("settings on a vehicle"), PkgSettingsHasIssue(Result, TEXT("Truck is a vehicle, and only a script package takes settings.")));
	TestTrue(TEXT("a parameter out of range"), PkgSettingsHasIssue(Result, TEXT("KillBricks's Speed must be between 1 and 10.")));
	TestTrue(TEXT("a parameter the package does not declare is now an error"), PkgSettingsHasIssue(Result, TEXT("KillBricks has no parameter called Colour")));
	TestFalse(TEXT("a required parameter the point sets is fine"), PkgSettingsHasIssue(Result, TEXT("KillBricks needs Target.")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePackageSettingsRowsTest,
	"Mantle.ScriptPackages.Settings.OptionRowsPickTheirEditorAndNotes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePackageSettingsRowsTest::RunTest(const FString& Parameters)
{
	using namespace MantlePackageSettingsTestPrivate;
	const FMantleScriptPackageManifest Manifest = PkgSettingsManifest();
	const FMantleScriptPackageParameter& Round = *Manifest.FindSetting(TEXT("RoundLength"));
	const FMantleScriptPackageParameter& Hardcore = *Manifest.FindSetting(TEXT("Hardcore"));
	const FMantleScriptPackageParameter& Difficulty = *Manifest.FindSetting(TEXT("Difficulty"));
	FMantleScriptPackageParameter Loose = Round;
	Loose.bHasMax = false;
	FMantleScriptPackageParameter Title;
	Title.Name = TEXT("Title");
	Title.Type = EMantleScriptPackageParameterType::String;
	Title.bHasDefault = true;
	Title.Default = TEXT("Kill Bricks");

	const FMantleScriptOptionRow Unset = FMantleScriptOptionRow::Make(TEXT("KillBricks"), Round, TOptional<FString>());
	TestTrue(TEXT("a number with min and max is a slider"), Unset.Widget == EMantleScriptOptionWidget::Slider && Unset.SliderMin == 10.0 && Unset.SliderMax == 600.0);
	TestEqual(TEXT("its label names the type, range and required"), Unset.Label, FString(TEXT("RoundLength (number, 10 to 600, required)")));
	TestEqual(TEXT("a required setting with no value and no default has a red note"), Unset.Note, FString(TEXT("KillBricks needs RoundLength.")));
	TestEqual(TEXT("its help shows"), Unset.Help, FString(TEXT("Seconds per round.")));
	const FMantleScriptOptionRow Set = FMantleScriptOptionRow::Make(TEXT("KillBricks"), Round, FString(TEXT("300")));
	TestTrue(TEXT("a fitting value clears the note"), Set.Note.IsEmpty() && Set.bIsSet && Set.Shown == TEXT("300"));
	const FMantleScriptOptionRow Far = FMantleScriptOptionRow::Make(TEXT("KillBricks"), Round, FString(TEXT("5")));
	TestEqual(TEXT("an out-of-range value is named"), Far.Note, FString(TEXT("KillBricks's RoundLength must be between 10 and 600.")));
	TestTrue(TEXT("a number without both bounds is a number box"), FMantleScriptOptionRow::Make(TEXT("KillBricks"), Loose, TOptional<FString>()).Widget == EMantleScriptOptionWidget::Number);

	const FMantleScriptOptionRow Check = FMantleScriptOptionRow::Make(TEXT("KillBricks"), Hardcore, TOptional<FString>());
	TestTrue(TEXT("a boolean is a checkbox showing its default"), Check.Widget == EMantleScriptOptionWidget::CheckBox && Check.Shown == TEXT("false") && Check.Note.IsEmpty());
	const FMantleScriptOptionRow Drop = FMantleScriptOptionRow::Make(TEXT("KillBricks"), Difficulty, TOptional<FString>());
	TestTrue(TEXT("a choice is a dropdown of its options"), Drop.Widget == EMantleScriptOptionWidget::Dropdown && Drop.Choices.Num() == 2 && Drop.Choices[1] == TEXT("Hard"));
	TestEqual(TEXT("showing its default"), Drop.Shown, FString(TEXT("Easy")));
	const FMantleScriptOptionRow Text = FMantleScriptOptionRow::Make(TEXT("KillBricks"), Title, TOptional<FString>());
	TestTrue(TEXT("a string is a text box with the default as a hint"), Text.Widget == EMantleScriptOptionWidget::Text && Text.Hint == TEXT("Kill Bricks (default)") && !Text.bIsSet);

	TOptional<FString> Value;
	TestTrue(TEXT("empty text clears the value"), FMantleScriptOptionRow::Commit(Round, FString(), Value) && !Value.IsSet());
	TestTrue(TEXT("a number is kept, out of range included"), FMantleScriptOptionRow::Commit(Round, TEXT("5000"), Value) && Value.IsSet() && Value.GetValue() == TEXT("5000"));
	TestFalse(TEXT("text is not kept in a number"), FMantleScriptOptionRow::Commit(Round, TEXT("long"), Value));
	TestFalse(TEXT("a choice keeps only its options"), FMantleScriptOptionRow::Commit(Difficulty, TEXT("hard"), Value));
	TestTrue(TEXT("one of them is kept"), FMantleScriptOptionRow::Commit(Difficulty, TEXT("Hard"), Value) && Value.GetValue() == TEXT("Hard"));
	TestFalse(TEXT("a boolean keeps only true or false"), FMantleScriptOptionRow::Commit(Hardcore, TEXT("yes"), Value));

	// Which storage entries draw typed rows.
	FMantleStorageSetting Script = PkgSettingsEntry(TEXT("KillBricks"), 1, TEXT("script"), FMantleScriptPackages::ManifestToJson(Manifest));
	FMantleScriptPackageManifest Drawn;
	TestTrue(TEXT("a script entry with a recorded manifest draws its settings"), FMantleStorageSettingCustomization::ManifestFor(Script, {}, Drawn) && Drawn.Settings.Num() == 3);
	TestFalse(TEXT("a vehicle draws none"), FMantleStorageSettingCustomization::ManifestFor(PkgSettingsEntry(TEXT("Truck"), 2, TEXT("vehicle")), {}, Drawn));
	TestFalse(TEXT("a script entry whose manifest is unknown draws none"), FMantleStorageSettingCustomization::ManifestFor(PkgSettingsEntry(TEXT("Other"), 3, TEXT("script")), {}, Drawn));
	TestFalse(TEXT("a hand-typed entry draws none"), FMantleStorageSettingCustomization::ManifestFor(PkgSettingsEntry(TEXT("Hand"), 4, TEXT("")), {}, Drawn));
	TArray<FMantleLocalScriptPackage> Local;
	FMantleLocalScriptPackage& Folder = Local.AddDefaulted_GetRef();
	Folder.Name = TEXT("Hand");
	Folder.bHasManifest = true;
	Folder.Manifest.Settings.Add(Title);
	TestTrue(TEXT("a hand-typed entry with a Packages/ folder of its name draws the folder's settings"),
		FMantleStorageSettingCustomization::ManifestFor(PkgSettingsEntry(TEXT("Hand"), 4, TEXT("")), Local, Drawn) && Drawn.Settings.Num() == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePackageSettingsPublishTest,
	"Mantle.ScriptPackages.Settings.PublishAndLibraryKeepTheCreatorsSettings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePackageSettingsPublishTest::RunTest(const FString& Parameters)
{
	using namespace MantlePackageSettingsTestPrivate;
	FMantleScriptPackageManifest Next = PkgSettingsManifest();
	Next.Settings.RemoveAll([](const FMantleScriptPackageParameter& Setting) { return Setting.Name == TEXT("Hardcore"); });
	FMantleLibraryPackageRef Ref;
	Ref.Name = TEXT("KillBricks");
	Ref.ItemId = FString::Printf(TEXT("itm_u%032x"), 7);
	Ref.Version = 2;
	Ref.Manifest = FMantleScriptPackages::ManifestToJson(Next);

	// The level's list: the re-pinned entry keeps what the creator set, less what the new version no longer declares.
	TArray<FMantleStorageEntry> Level;
	FMantleStorageEntry& Entry = Level.AddDefaulted_GetRef();
	Entry.Name = TEXT("KillBricks");
	Entry.Item = Ref.ItemId;
	Entry.Version = 1;
	Entry.Settings.Add(TEXT("RoundLength"), TEXT("300"));
	Entry.Settings.Add(TEXT("Hardcore"), TEXT("true"));
	FMantleLibraryPublish::MergePackagesIntoStorage(Level, { Ref });
	TestTrue(TEXT("re-pinned"), Level.Num() == 1 && Level[0].Version == 2);
	TestEqual(TEXT("the declared value stays"), Level[0].Settings.FindRef(TEXT("RoundLength")), FString(TEXT("300")));
	TestFalse(TEXT("the dropped setting goes"), Level[0].Settings.Contains(TEXT("Hardcore")));

	// The exported place file: the same, written back as settings.
	const FString PlaceJson = FString::Printf(
		TEXT("{\"schemaVersion\":1,\"entities\":[],\"storage\":[{\"name\":\"KillBricks\",\"item\":\"%s\",\"version\":1,\"settings\":{\"Hardcore\":\"true\",\"RoundLength\":\"300\"}}]}"),
		*Ref.ItemId);
	FString Rewritten;
	FString Error;
	if (TestTrue(TEXT("the rewrite succeeds"), FMantleLibraryPublish::RewriteScriptPackages(PlaceJson, { Ref }, Rewritten, Error)))
	{
		const TSharedPtr<FJsonObject> Root = PkgSettingsReadObject(Rewritten);
		const TSharedPtr<FJsonObject>* Written = nullptr;
		const TArray<TSharedPtr<FJsonValue>>& Storage = Root->GetArrayField(TEXT("storage"));
		if (TestTrue(TEXT("the entry keeps a settings object"), Storage.Num() == 1 && Storage[0]->AsObject()->TryGetObjectField(TEXT("settings"), Written)))
		{
			TestEqual(TEXT("with the declared value"), (*Written)->GetStringField(TEXT("RoundLength")), FString(TEXT("300")));
			TestFalse(TEXT("and not the dropped one"), (*Written)->HasField(TEXT("Hardcore")));
		}
	}

	// The level's Place Settings carry settings to the exporter and back from a place file.
	FPkgSettingsWorld Fixture;
	AMantlePlaceSettingsActor* Settings = Fixture.World ? Fixture.AddSettings({ PkgSettingsEntry(TEXT("KillBricks"), 7, TEXT("script")) }) : nullptr;
	if (TestNotNull(TEXT("place settings"), Settings))
	{
		Settings->Storage[0].Settings.Add(TEXT("RoundLength"), TEXT("300"));
		TArray<FMantleStorageEntry> Applied;
		AMantlePlaceSettingsActor::ApplyStorageTo(Fixture.World, Applied);
		TestTrue(TEXT("ApplyStorageTo carries settings"), Applied.Num() == 1 && Applied[0].Settings.FindRef(TEXT("RoundLength")) == TEXT("300"));
		Applied[0].Version = 2;
		Settings->ReplaceStorageKeepingKinds(Applied);
		TestTrue(TEXT("a rebuilt list keeps the kind and the settings"), Settings->Storage.Num() == 1 && Settings->Storage[0].Kind == TEXT("script")
			&& Settings->Storage[0].Settings.FindRef(TEXT("RoundLength")) == TEXT("300"));
	}

	// Test locally's bundle lists each package's settings beside its parameters.
	TArray<FMantleLocalScriptPackage> Local;
	FMantleLocalScriptPackage& Package = Local.AddDefaulted_GetRef();
	Package.Name = TEXT("KillBricks");
	Package.bHasManifest = true;
	Package.Manifest = PkgSettingsManifest();
	FMantleScriptCatalogEntry& Main = Package.Entries.AddDefaulted_GetRef();
	Main.Path = TEXT("Packages/KillBricks/Server/Main.luau");
	TArray<TSharedPtr<FJsonValue>> Bundle;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FMantlePreviewExport::PackagesToJson(Local)), Bundle);
	const TSharedPtr<FJsonObject>* BundleManifest = nullptr;
	if (TestTrue(TEXT("the bundle lists the package with its manifest"), Bundle.Num() == 1 && Bundle[0]->AsObject()->TryGetObjectField(TEXT("manifest"), BundleManifest)))
	{
		const FMantleScriptPackageManifest Read = FMantleStorage::ReadScriptPackageManifest(*BundleManifest);
		TestEqual(TEXT("the client reads its settings"), Read.Settings.Num(), 3);
		const FMantleScriptPackageParameter* Round = Read.FindSetting(TEXT("RoundLength"));
		TestTrue(TEXT("with their range and required"), Round && Round->bHasMin && Round->bHasMax && Round->bRequired);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
