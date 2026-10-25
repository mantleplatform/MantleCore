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
#include "MantlePreviewLaunch.h"
#include "MantleScriptPackages.h"
#include "MantleScriptPoint.h"
#include "MantleScriptPointDetails.h"

#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// Script packages and Script Points in the editor: a Packages/ folder's packages read and checked by the backend's
// and the runtime's rules, a place script's require("@Name/...") accepted, the script API floor, a Script Point exported, refused
// and offered its packages, and the bodies and rewrites of publishing a package as a library item. Pure, on a folder of its own
// under Intermediate, or on a transient editor world.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleScriptPackagesTestPrivate
{
	const TCHAR* const ValidManifest =
		TEXT("{\"name\": \"Kill Bricks\", \"description\": \"Touch one and respawn.\", \"parameters\": [")
		TEXT("{\"name\": \"Damage\", \"type\": \"number\", \"default\": 100, \"help\": \"How much a touch takes.\"},")
		TEXT("{\"name\": \"Lethal\", \"type\": \"boolean\", \"default\": true},")
		TEXT("{\"name\": \"Message\", \"type\": \"string\", \"default\": \"Ouch\"},")
		TEXT("{\"name\": \"Label\", \"type\": \"string\"}]}");

	/** A folder of its own under Intermediate, deleted with the fixture. */
	struct FPackagesFolder
	{
		FString Root;

		FPackagesFolder()
		{
			Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("MantleScriptPackagesTest"),
				FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			IFileManager::Get().MakeDirectory(*Packages(), /*Tree*/ true);
		}

		~FPackagesFolder()
		{
			IFileManager::Get().DeleteDirectory(*Root, /*RequireExists*/ false, /*Tree*/ true);
		}

		FString Packages() const { return Root / TEXT("Packages"); }

		bool Write(const FString& Relative, const FString& Text) const
		{
			return FFileHelper::SaveStringToFile(Text, *(Root / Relative));
		}
	};

	FMantleScriptCatalogEntry MakeEntry(const TCHAR* Path, const EMantleSide Side, const bool bModuleOnly, const TCHAR* Source)
	{
		FMantleScriptCatalogEntry Entry;
		Entry.Path = Path;
		Entry.Name = FPaths::GetBaseFilename(Path);
		Entry.Side = Side;
		Entry.Language = EMantleScriptCatalogLanguage::Luau;
		Entry.bModuleOnly = bModuleOnly;
		Entry.Source = Source;
		return Entry;
	}

	FMantleStorageSetting MakeSetting(const TCHAR* Name, const int32 Number, const TCHAR* Kind)
	{
		FMantleStorageSetting Setting;
		Setting.Name = Name;
		Setting.Item = FString::Printf(TEXT("itm_u%032x"), Number);
		Setting.Version = 1;
		Setting.Kind = Kind;
		return Setting;
	}

	/** An empty editor world for one test, destroyed with it. */
	struct FScriptPointWorld
	{
		UWorld* World = nullptr;

		FScriptPointWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleScriptPointTestWorld"));
		}

		~FScriptPointWorld()
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

		AMantlePlaceSettingsActor* AddSettings(const TArray<FMantleStorageSetting>& Storage) const
		{
			AMantlePlaceSettingsActor* Settings = World->SpawnActor<AMantlePlaceSettingsActor>();
			if (Settings)
			{
				Settings->Storage = Storage;
			}
			return Settings;
		}
	};

	bool HasIssue(const FMantleValidationResult& Result, const EMantleIssueSeverity Severity, const FString& Actor, const TCHAR* Contains)
	{
		return Result.Issues.ContainsByPredicate([&](const FMantleValidationIssue& Issue)
		{
			return Issue.Severity == Severity && Issue.Actor.Equals(Actor, ESearchCase::CaseSensitive) && Issue.Problem.Contains(Contains);
		});
	}

	TSharedPtr<FJsonObject> ReadJsonObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Root;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root);
		return Root;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScriptPackagesManifestReadsTest,
	"Mantle.ScriptPackages.Manifest.ReadsAValidManifest",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScriptPackagesManifestReadsTest::RunTest(const FString& Parameters)
{
	using namespace MantleScriptPackagesTestPrivate;
	FMantleScriptPackageManifest Manifest;
	TArray<FString> Problems;
	if (!TestTrue(TEXT("a valid package.json reads"), FMantleScriptPackages::ReadManifest(ValidManifest, Manifest, Problems)))
	{
		AddError(FString::Join(Problems, TEXT("\n")));
		return false;
	}
	TestEqual(TEXT("name"), Manifest.Name, FString(TEXT("Kill Bricks")));
	TestEqual(TEXT("description"), Manifest.Description, FString(TEXT("Touch one and respawn.")));
	if (!TestEqual(TEXT("four parameters"), Manifest.Parameters.Num(), 4))
	{
		return false;
	}
	TestTrue(TEXT("a number"), Manifest.Parameters[0].Type == EMantleScriptPackageParameterType::Number);
	TestEqual(TEXT("whose default is its text"), Manifest.Parameters[0].Default, FString(TEXT("100")));
	TestEqual(TEXT("with its help"), Manifest.Parameters[0].Help, FString(TEXT("How much a touch takes.")));
	TestEqual(TEXT("a boolean's default"), Manifest.Parameters[1].Default, FString(TEXT("true")));
	TestEqual(TEXT("a string's default"), Manifest.Parameters[2].Default, FString(TEXT("Ouch")));
	TestFalse(TEXT("a string may leave its default out"), Manifest.Parameters[3].bHasDefault);

	// What the API takes: every default as text, an absent one as "".
	const TSharedPtr<FJsonObject> Body = ReadJsonObject(FMantleScriptPackages::ManifestToJson(Manifest));
	const TArray<TSharedPtr<FJsonValue>>* Written = nullptr;
	if (TestTrue(TEXT("the manifest writes parameters"), Body.IsValid() && Body->TryGetArrayField(TEXT("parameters"), Written) && Written->Num() == 4))
	{
		TestEqual(TEXT("a number's default goes as text"), (*Written)[0]->AsObject()->GetStringField(TEXT("default")), FString(TEXT("100")));
		TestEqual(TEXT("an absent default goes empty"), (*Written)[3]->AsObject()->GetStringField(TEXT("default")), FString());
	}
	// And the hints reader takes the API's text defaults back.
	const FMantleScriptPackageManifest Hints = FMantleScriptPackages::ReadManifestForHints(FMantleScriptPackages::ManifestToJson(Manifest));
	TestEqual(TEXT("hints read the same parameters"), Hints.Parameters.Num(), 4);
	TestTrue(TEXT("with their defaults"), Hints.Parameters.Num() == 4 && Hints.Parameters[0].Default == TEXT("100") && Hints.Parameters[1].Default == TEXT("true"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScriptPackagesManifestRefusesTest,
	"Mantle.ScriptPackages.Manifest.RefusesWhatTheBackendRefusesInPlainWords",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScriptPackagesManifestRefusesTest::RunTest(const FString& Parameters)
{
	FString TooManyParameters = TEXT("{\"name\": \"Many\", \"parameters\": [");
	for (int32 Index = 0; Index < FMantlePlaceLimits::MaxScriptPointParameters + 1; ++Index)
	{
		TooManyParameters += FString::Printf(TEXT("%s{\"name\": \"P%d\", \"type\": \"string\"}"), Index == 0 ? TEXT("") : TEXT(","), Index);
	}
	TooManyParameters += TEXT("]}");

	const TArray<TPair<FString, FString>> Cases = {
		{ TEXT("[1, 2]"), TEXT("not a JSON object") },
		{ TEXT("{\"description\": \"no name\"}"), TEXT("has no name") },
		{ FString::Printf(TEXT("{\"name\": \"%s\"}"), *FString::ChrN(65, TEXT('n'))), TEXT("longer than 64") },
		{ FString::Printf(TEXT("{\"name\": \"N\", \"description\": \"%s\"}"), *FString::ChrN(1001, TEXT('d'))), TEXT("longer than 1000") },
		{ TooManyParameters, TEXT("at most 16") },
		{ TEXT("{\"name\": \"N\", \"parameters\": [{\"name\": \"1bad\", \"type\": \"string\"}]}"), TEXT("a parameter name is a letter") },
		{ TEXT("{\"name\": \"N\", \"parameters\": [{\"name\": \"A\", \"type\": \"string\"}, {\"name\": \"A\", \"type\": \"string\"}]}"), TEXT("twice") },
		{ TEXT("{\"name\": \"N\", \"parameters\": [{\"name\": \"A\", \"type\": \"vector\"}]}"), TEXT("\"string\", \"number\", \"boolean\" or \"choice\"") },
		{ TEXT("{\"name\": \"N\", \"parameters\": [{\"name\": \"A\", \"type\": \"number\", \"default\": \"5\"}]}"), TEXT("without quotes") },
		{ TEXT("{\"name\": \"N\", \"parameters\": [{\"name\": \"A\", \"type\": \"boolean\"}]}"), TEXT("has no default") },
		{ TEXT("{\"name\": \"N\", \"parameters\": [{\"name\": \"A\", \"type\": \"string\", \"default\": \" padded \"}]}"), TEXT("starting or ending with a space") },
		{ FString::Printf(TEXT("{\"name\": \"N\", \"parameters\": [{\"name\": \"A\", \"type\": \"string\", \"help\": \"%s\"}]}"), *FString::ChrN(201, TEXT('h'))), TEXT("longer than 200") },
	};
	for (const TPair<FString, FString>& Case : Cases)
	{
		FMantleScriptPackageManifest Manifest;
		TArray<FString> Problems;
		const bool bRead = FMantleScriptPackages::ReadManifest(Case.Key, Manifest, Problems);
		TestFalse(FString::Printf(TEXT("refused: %s"), *Case.Value), bRead);
		TestTrue(FString::Printf(TEXT("saying '%s'"), *Case.Value), Problems.ContainsByPredicate([&Case](const FString& Problem) { return Problem.Contains(Case.Value); }));
		TestTrue(TEXT("in words that name package.json"), Problems.Num() > 0 && Problems[0].Contains(TEXT("package.json")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScriptPackagesCollectTest,
	"Mantle.ScriptPackages.Collect.ReadsPackagesBesideTheLevelAndKeepsThemOutOfPlaceScripts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScriptPackagesCollectTest::RunTest(const FString& Parameters)
{
	using namespace MantleScriptPackagesTestPrivate;
	const FPackagesFolder Folder;
	TestTrue(TEXT("fixture written"),
		Folder.Write(TEXT("Packages/KillBricks/package.json"), ValidManifest)
		&& Folder.Write(TEXT("Packages/KillBricks/Server/Main.luau"), TEXT("local Bricks = require(\"Bricks\")\n"))
		&& Folder.Write(TEXT("Packages/KillBricks/Client/Hud.js"), TEXT("export const shown = true;\n"))
		&& Folder.Write(TEXT("Packages/KillBricks/Shared/Bricks.luau"), TEXT("return {}\n"))
		&& Folder.Write(TEXT("Scripts/Server/Own.luau"), TEXT("local Bricks = require(\"@KillBricks/Bricks\")\n")));

	TArray<FMantleLocalScriptPackage> Packages;
	TArray<FMantleScriptPackageProblem> Problems;
	FMantleScriptPackages::Collect(Folder.Packages(), Packages, Problems);
	TestEqual(TEXT("no problems"), Problems.Num(), 0);
	if (!TestEqual(TEXT("one package"), Packages.Num(), 1))
	{
		return false;
	}
	const FMantleLocalScriptPackage& Package = Packages[0];
	TestEqual(TEXT("named after its folder"), Package.Name, FString(TEXT("KillBricks")));
	TestTrue(TEXT("valid"), Package.bValid);
	TestTrue(TEXT("its manifest read"), Package.bHasManifest && Package.Manifest.Name == TEXT("Kill Bricks"));
	if (TestEqual(TEXT("its three scripts"), Package.Entries.Num(), 3))
	{
		TestEqual(TEXT("mounted as the runtime mounts them, sorted"), Package.Entries[0].Path, FString(TEXT("Packages/KillBricks/Client/Hud.js")));
		TestEqual(TEXT("the path inside the package"), FMantleScriptPackages::PathInPackage(Package.Entries[0].Path), FString(TEXT("Client/Hud.js")));
		TestTrue(TEXT("Server runs"), !Package.Entries[1].bModuleOnly && Package.Entries[1].Path.EndsWith(TEXT("Server/Main.luau")));
		TestTrue(TEXT("Shared is require-only"), Package.Entries[2].bModuleOnly && Package.Entries[2].Path.EndsWith(TEXT("Shared/Bricks.luau")));
		TestTrue(TEXT("the place's own script is not one of them"), !Package.Entries.ContainsByPredicate([](const FMantleScriptCatalogEntry& Entry) { return Entry.Path.Contains(TEXT("Own")); }));
	}

	// The files Publish sends: path in the package, side, language.
	TArray<FMantleScriptPackageFile> Files;
	FString Error;
	if (TestTrue(TEXT("the files are prepared"), FMantleScriptPackages::PrepareFiles(Package, Files, Error)) && TestEqual(TEXT("three files"), Files.Num(), 3))
	{
		TestTrue(TEXT("a client JavaScript file"), Files[0].Path == TEXT("Client/Hud.js") && Files[0].Side == TEXT("client") && Files[0].Language == TEXT("javascript")
			&& Files[0].UploadLanguage == TEXT("javascript"));
		TestTrue(TEXT("a server Luau script"), Files[1].Path == TEXT("Server/Main.luau") && Files[1].Side == TEXT("server") && Files[1].Language == TEXT("luau"));
		TestTrue(TEXT("a shared Luau module"), Files[2].Side == TEXT("shared") && Files[2].Language == TEXT("luau"));
	}

	// Package files are never place scripts.
	TArray<FMantleScriptCatalogEntry> PlaceScripts;
	TArray<FMantleScriptCollectionIssue> Issues;
	FMantlePlaceExporter::CollectScriptFiles(Folder.Root / TEXT("Scripts"), PlaceScripts, Issues);
	TestEqual(TEXT("the level's Scripts/ holds only its own script"), PlaceScripts.Num(), 1);
	TestFalse(TEXT("no package file among the place's scripts"),
		PlaceScripts.ContainsByPredicate([](const FMantleScriptCatalogEntry& Entry) { return Entry.Path.Contains(TEXT("Packages")); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScriptPackagesCollectProblemsTest,
	"Mantle.ScriptPackages.Collect.ReportsMalformedPackagesInPlainWords",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScriptPackagesCollectProblemsTest::RunTest(const FString& Parameters)
{
	using namespace MantleScriptPackagesTestPrivate;
	const FPackagesFolder Folder;
	TestTrue(TEXT("fixture written"),
		Folder.Write(TEXT("Packages/Bad Name/package.json"), TEXT("{\"name\": \"Bad\"}"))
		&& Folder.Write(TEXT("Packages/NoManifest/Server/A.luau"), TEXT("print(1)\n"))
		&& Folder.Write(TEXT("Packages/Empty/package.json"), TEXT("{\"name\": \"Empty\"}"))
		&& Folder.Write(TEXT("Packages/Odd/package.json"), TEXT("{\"name\": \"Odd\"}"))
		&& Folder.Write(TEXT("Packages/Odd/Server/Main.luau"), TEXT("print(1)\n"))
		&& Folder.Write(TEXT("Packages/Odd/Server/Notes.txt"), TEXT("not a script\n"))
		&& Folder.Write(TEXT("Packages/Odd/Main.luau"), TEXT("print(2)\n"))
		&& Folder.Write(TEXT("Packages/stray.luau"), TEXT("print(3)\n")));
	// The runtime's own discovery logs the file it skips; Check names it as well.
	AddExpectedErrorPlain(TEXT("is not inside Packages/<Name>/Server/"), EAutomationExpectedErrorFlags::Contains, 0);

	TArray<FMantleLocalScriptPackage> Packages;
	TArray<FMantleScriptPackageProblem> Problems;
	FMantleScriptPackages::Collect(Folder.Packages(), Packages, Problems);
	auto Has = [&Problems](const TCHAR* Package, const TCHAR* Contains)
	{
		return Problems.ContainsByPredicate([Package, Contains](const FMantleScriptPackageProblem& Problem)
		{
			return Problem.bError && Problem.Package.Equals(Package, ESearchCase::CaseSensitive) && Problem.Problem.Contains(Contains) && !Problem.Remedy.IsEmpty();
		});
	};
	TestTrue(TEXT("a folder name that is not a storage name"), Has(TEXT("Bad Name"), TEXT("not a usable package name")));
	TestFalse(TEXT("is left out"), Packages.ContainsByPredicate([](const FMantleLocalScriptPackage& Package) { return Package.Name == TEXT("Bad Name"); }));
	TestTrue(TEXT("a package without package.json"), Has(TEXT("NoManifest"), TEXT("has no package.json")));
	TestTrue(TEXT("a package without scripts"), Has(TEXT("Empty"), TEXT("has no scripts")));
	TestTrue(TEXT("a file with no script language"), Has(TEXT("Odd"), TEXT("Packages/Odd/Server/Notes.txt has an extension")));
	TestTrue(TEXT("a script outside Server, Client and Shared"), Has(TEXT("Odd"), TEXT("Packages/Odd/Main.luau is not inside")));
	TestTrue(TEXT("a loose file under Packages/"), Has(TEXT(""), TEXT("Packages/stray.luau is not inside a package folder")));
	for (const FMantleLocalScriptPackage& Package : Packages)
	{
		TestFalse(FString::Printf(TEXT("%s is not valid"), *Package.Name), Package.bValid);
		TArray<FMantleScriptPackageFile> Files;
		FString Error;
		TestFalse(FString::Printf(TEXT("%s cannot be prepared for publishing"), *Package.Name), FMantleScriptPackages::PrepareFiles(Package, Files, Error));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScriptPackagesRequireTest,
	"Mantle.ScriptPackages.Require.APlaceScriptReachesALocalOrStoredPackage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScriptPackagesRequireTest::RunTest(const FString& Parameters)
{
	using namespace MantleScriptPackagesTestPrivate;
	TArray<FMantleScriptCatalogEntry> Entries;
	Entries.Add(MakeEntry(TEXT("Server/Main.luau"), EMantleSide::Server, false, TEXT("")));
	Entries.Add(MakeEntry(TEXT("Shared/Config.luau"), EMantleSide::Both, true, TEXT("return {}")));
	Entries.Add(MakeEntry(TEXT("Packages/KillBricks/Shared/Bricks.luau"), EMantleSide::Both, true, TEXT("return {}")));
	Entries.Add(MakeEntry(TEXT("Packages/KillBricks/Server/Main.luau"), EMantleSide::Server, false, TEXT("")));
	const TArray<FString> Stored = { TEXT("DoubleJump") };
	const FMantleScriptCatalogEntry PlaceScript = Entries[0];
	const FMantleScriptCatalogEntry PackageScript = Entries[3];

	const FMantleScriptCatalogEntry* Target = nullptr;
	FString Error;
	TestTrue(TEXT("@KillBricks/Bricks reaches the local package's Shared module"),
		FMantleScriptPackages::ResolveStaticRequire(Entries, PlaceScript, TEXT("@KillBricks/Bricks"), Stored, Target, Error));
	TestTrue(TEXT("naming it"), Target && Target->Path == TEXT("Packages/KillBricks/Shared/Bricks.luau"));

	TestTrue(TEXT("@DoubleJump/Thing names a stored package, accepted unseen"),
		FMantleScriptPackages::ResolveStaticRequire(Entries, PlaceScript, TEXT("@DoubleJump/Thing"), Stored, Target, Error));
	TestNull(TEXT("with no file to name"), Target);

	TestFalse(TEXT("@Nowhere/Thing names nothing"), FMantleScriptPackages::ResolveStaticRequire(Entries, PlaceScript, TEXT("@Nowhere/Thing"), Stored, Target, Error));
	TestTrue(TEXT("and says so"), Error.Contains(TEXT("Nowhere")));
	TestFalse(TEXT("a module the local package lacks is refused"),
		FMantleScriptPackages::ResolveStaticRequire(Entries, PlaceScript, TEXT("@KillBricks/Missing"), Stored, Target, Error));
	TestTrue(TEXT("the place's own modules still resolve"),
		FMantleScriptPackages::ResolveStaticRequire(Entries, PlaceScript, TEXT("Config"), Stored, Target, Error) && Target && Target->Path == TEXT("Shared/Config.luau"));

	// Inside a package, bare names stay in the package and the place's own scripts are out of reach.
	TestTrue(TEXT("a package's bare name resolves inside it"),
		FMantleScriptPackages::ResolveStaticRequire(Entries, PackageScript, TEXT("Bricks"), Stored, Target, Error) && Target && Target->Path.StartsWith(TEXT("Packages/KillBricks/")));
	TestFalse(TEXT("a package cannot require the place's scripts"),
		FMantleScriptPackages::ResolveStaticRequire(Entries, PackageScript, TEXT("Config"), Stored, Target, Error));

	FString Package;
	TestTrue(TEXT("@Name/Thing parses"), FMantleScriptPackages::ParsePackageRequest(TEXT("@KillBricks/Bricks"), Package) && Package == TEXT("KillBricks"));
	TestFalse(TEXT("a bare name is no package request"), FMantleScriptPackages::ParsePackageRequest(TEXT("Bricks"), Package));
	TestFalse(TEXT("nor is @Name with nothing after it"), FMantleScriptPackages::ParsePackageRequest(TEXT("@KillBricks/"), Package));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScriptPackagesScriptApiTest,
	"Mantle.ScriptPackages.ScriptApi.APlaceUsingPackagesRecordsAtLeast136",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScriptPackagesScriptApiTest::RunTest(const FString& Parameters)
{
	const int32 Floor = FMath::Min(FMantleScriptPackages::MinScriptApiMinor, FMantleApiManifest::Get().Minor);
	TestEqual(TEXT("packages raise a low need to 1.36"), FMantleScriptPackages::RequiredScriptApiMinor(10, /*bUsesPackages*/ true), Floor);
	TestEqual(TEXT("without packages the need stands"), FMantleScriptPackages::RequiredScriptApiMinor(10, /*bUsesPackages*/ false), 10);
	TestEqual(TEXT("a higher need stands"), FMantleScriptPackages::RequiredScriptApiMinor(FMantleApiManifest::Get().Minor, true), FMantleApiManifest::Get().Minor);
	TestEqual(TEXT("never above this editor's newest"), FMantleScriptPackages::RequiredScriptApiMinor(FMantleApiManifest::Get().Minor + 5, true), FMantleApiManifest::Get().Minor);

	// A place with no scripts of its own but a package-using point writes scriptApi; one with neither does not.
	FMantlePlace Place;
	Place.SchemaVersion = MANTLE_PLACE_SCHEMA_VERSION;
	Place.PlaceId = TEXT("plc_scriptapi01");
	Place.Revision.Number = 1;
	Place.Revision.State = EMantleRevisionState::Draft;
	Place.Metadata.Name = TEXT("Script API floor");
	Place.Metadata.AuthorId = TEXT("usr_test");
	// The quoted key, case-sensitively: the place id above would otherwise match a case-insensitive Contains.
	TestFalse(TEXT("no scripts, no packages: no scriptApi"), FMantlePlaceExporter::ToJson(Place).Contains(TEXT("\"scriptApi\""), ESearchCase::CaseSensitive));
	Place.ScriptApi.Major = FMantleApiManifest::Get().Major;
	Place.ScriptApi.Minor = Floor;
	const TSharedPtr<FJsonObject> Root = MantleScriptPackagesTestPrivate::ReadJsonObject(FMantlePlaceExporter::ToJson(Place));
	const TSharedPtr<FJsonObject>* ScriptApi = nullptr;
	if (TestTrue(TEXT("packages alone write scriptApi"), Root.IsValid() && Root->TryGetObjectField(TEXT("scriptApi"), ScriptApi)))
	{
		TestEqual(TEXT("at the floor"), static_cast<int32>((*ScriptApi)->GetNumberField(TEXT("minor"))), Floor);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScriptPointExportTest,
	"Mantle.ScriptPoint.Editor.Export.WritesTheEntityWithSortedParameters",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScriptPointExportTest::RunTest(const FString& Parameters)
{
	using namespace MantleScriptPackagesTestPrivate;
	FScriptPointWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World) || !TestNotNull(TEXT("settings"), Fixture.AddSettings({ MakeSetting(TEXT("KillBricks"), 1, TEXT("script")) })))
	{
		return false;
	}
	AMantleScriptPoint* Point = Fixture.AddPoint(TEXT("Brick A"), TEXT("KillBricks"));
	if (!TestNotNull(TEXT("the point"), Point))
	{
		return false;
	}
	Point->Parameters.Add(TEXT("Message"), TEXT("Ouch"));
	Point->Parameters.Add(TEXT("Damage"), TEXT("100"));
	Point->bEnabled = false;
	TestTrue(TEXT("the exporter classifies it as a ScriptPoint"), [Point]()
	{
		EMantleEntityType Type = EMantleEntityType::Group;
		return FMantlePlaceExporter::ClassifyActor(Point, Type) && Type == EMantleEntityType::ScriptPoint;
	}());

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestFalse(TEXT("the export does not refuse the point"), Result.Errors.ContainsByPredicate([](const FMantleParseError& Error)
	{
		return Error.Path.Contains(TEXT("storageItem")) || Error.Path.Contains(TEXT("parameters"));
	}));
	const FMantleEntity* Entity = Result.Place.Entities.FindByPredicate([](const FMantleEntity& Candidate) { return Candidate.Type == EMantleEntityType::ScriptPoint; });
	const FMantleScriptPointProps* Props = Entity ? Entity->Props.TryGet<FMantleScriptPointProps>() : nullptr;
	if (!TestNotNull(TEXT("a ScriptPoint entity with its properties"), Props))
	{
		return false;
	}
	TestEqual(TEXT("its package"), Props->StorageItem, FString(TEXT("KillBricks")));
	TestFalse(TEXT("its Enabled box"), Props->bEnabled);
	TestTrue(TEXT("its parameters, sorted by key"), Props->Parameters.Num() == 2 && Props->Parameters[0].Name == TEXT("Damage") && Props->Parameters[1].Value == TEXT("Ouch"));
	TestEqual(TEXT("a place with a Script Point records the package floor"), Result.Place.ScriptApi.Minor,
		FMantleScriptPackages::RequiredScriptApiMinor(0, /*bUsesPackages*/ true));

	// What goes out parses back equal, keys in order.
	FMantlePlace Place = Result.Place;
	Place.SchemaVersion = MANTLE_PLACE_SCHEMA_VERSION;
	Place.PlaceId = TEXT("plc_scriptpoint01");
	Place.Revision.Number = 1;
	Place.Revision.State = EMantleRevisionState::Draft;
	Place.Metadata.Name = TEXT("Script Point");
	Place.Metadata.AuthorId = TEXT("usr_test");
	const FString Json = FMantlePlaceExporter::ToJson(Place);
	TestTrue(TEXT("Damage is written before Message"), Json.Find(TEXT("\"Damage\"")) != INDEX_NONE && Json.Find(TEXT("\"Damage\"")) < Json.Find(TEXT("\"Message\"")));
	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
	if (!TestTrue(TEXT("what the exporter wrote parses"), Parsed.bSuccess))
	{
		AddError(Parsed.ErrorSummary());
		return false;
	}
	const FMantleEntity* Back = Parsed.Place.Entities.FindByPredicate([](const FMantleEntity& Candidate) { return Candidate.Type == EMantleEntityType::ScriptPoint; });
	const FMantleScriptPointProps* BackProps = Back ? Back->Props.TryGet<FMantleScriptPointProps>() : nullptr;
	if (TestNotNull(TEXT("the point reads back"), BackProps))
	{
		TestEqual(TEXT("same package"), BackProps->StorageItem, Props->StorageItem);
		TestFalse(TEXT("still off"), BackProps->bEnabled);
		TestTrue(TEXT("same parameters"), BackProps->Parameters.Num() == 2 && BackProps->Parameters[0].Name == TEXT("Damage") && BackProps->Parameters[0].Value == TEXT("100"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScriptPointRefusalsTest,
	"Mantle.ScriptPoint.Editor.Validate.RefusesPointsItCannotRun",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScriptPointRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace MantleScriptPackagesTestPrivate;
	FScriptPointWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World)
		|| !TestNotNull(TEXT("settings"), Fixture.AddSettings({ MakeSetting(TEXT("Truck"), 1, TEXT("vehicle")), MakeSetting(TEXT("Bricks"), 2, TEXT("script")),
			MakeSetting(TEXT("Typed"), 3, TEXT("")) })))
	{
		return false;
	}
	Fixture.AddPoint(TEXT("No package"), FString());
	Fixture.AddPoint(TEXT("Names a truck"), TEXT("Truck"));
	Fixture.AddPoint(TEXT("Names nothing"), TEXT("Nothing"));
	Fixture.AddPoint(TEXT("Typed by hand"), TEXT("Typed"));
	if (AMantleScriptPoint* BadKey = Fixture.AddPoint(TEXT("Bad key"), TEXT("Bricks")))
	{
		BadKey->Parameters.Add(TEXT("1bad"), TEXT("x"));
	}
	if (AMantleScriptPoint* LongValue = Fixture.AddPoint(TEXT("Long value"), TEXT("Bricks")))
	{
		LongValue->Parameters.Add(TEXT("Message"), FString::ChrN(FMantlePlaceLimits::MaxScriptPointParameterValueLength + 1, TEXT('x')));
	}
	Fixture.AddPoint(TEXT("Fine"), TEXT("Bricks"));

	const FMantleValidationResult Result = FMantlePlaceValidator::Validate(Fixture.World);
	TestTrue(TEXT("a point naming no package"), HasIssue(Result, EMantleIssueSeverity::Error, TEXT("No package"), TEXT("names no script package")));
	TestTrue(TEXT("a point naming a vehicle"), HasIssue(Result, EMantleIssueSeverity::Error, TEXT("Names a truck"), TEXT("not a script package")));
	TestTrue(TEXT("a point naming nothing stored"), HasIssue(Result, EMantleIssueSeverity::Error, TEXT("Names nothing"), TEXT("neither Mantle Place Settings > Storage nor")));
	TestTrue(TEXT("a hand-typed entry warns that the backend checks it"), HasIssue(Result, EMantleIssueSeverity::Warning, TEXT("Typed by hand"), TEXT("Mantle checks that when you publish")));
	TestTrue(TEXT("a bad parameter key"), HasIssue(Result, EMantleIssueSeverity::Error, TEXT("Bad key"), TEXT("parameter named '1bad'")));
	TestTrue(TEXT("a value too long"), HasIssue(Result, EMantleIssueSeverity::Error, TEXT("Long value"), TEXT("longer than 256")));
	TestFalse(TEXT("a good point is not refused"), Result.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Actor == TEXT("Fine") && Issue.Severity == EMantleIssueSeverity::Error;
	}));

	// Past the cap.
	FScriptPointWorld Crowded;
	if (Crowded.World && Crowded.AddSettings({ MakeSetting(TEXT("Bricks"), 2, TEXT("script")) }))
	{
		for (int32 Index = 0; Index < FMantlePlaceLimits::MaxScriptPoints + 1; ++Index)
		{
			Crowded.AddPoint(*FString::Printf(TEXT("Point %d"), Index + 1), TEXT("Bricks"));
		}
		const FMantleValidationResult Many = FMantlePlaceValidator::Validate(Crowded.World);
		TestTrue(TEXT("Script Point 65 is refused"), Many.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
		{
			return Issue.Severity == EMantleIssueSeverity::Error && Issue.Problem.Contains(TEXT("is Script Point 65; a place has at most 64"));
		}));
		const FMantleExportResult Exported = FMantlePlaceExporter::Export(Crowded.World, FMantleExportOptions());
		TestTrue(TEXT("and the export refuses it too"), Exported.Errors.ContainsByPredicate([](const FMantleParseError& Error) { return Error.Message.Contains(TEXT("a place has at most 64")); }));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScriptPointChoicesTest,
	"Mantle.ScriptPoint.Editor.Details.OffersScriptPackagesFirstAndNoVehicles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScriptPointChoicesTest::RunTest(const FString& Parameters)
{
	using namespace MantleScriptPackagesTestPrivate;
	TArray<FMantleStorageSetting> Storage = { MakeSetting(TEXT("Hand"), 1, TEXT("")), MakeSetting(TEXT("Truck"), 2, TEXT("vehicle")), MakeSetting(TEXT("Bricks"), 3, TEXT("script")) };
	Storage[2].Manifest = TEXT("{\"name\":\"Stored bricks\",\"parameters\":[{\"name\":\"Damage\",\"type\":\"number\",\"default\":\"5\"}]}");
	TArray<FMantleLocalScriptPackage> Local;
	FMantleLocalScriptPackage& Jump = Local.AddDefaulted_GetRef();
	Jump.Name = TEXT("Jump");
	FMantleLocalScriptPackage& Truck = Local.AddDefaulted_GetRef();
	Truck.Name = TEXT("Truck");

	const TArray<FMantleScriptPointChoice> Choices = FMantleScriptPointDetails::MakeChoices(Storage, Local);
	if (!TestEqual(TEXT("three choices"), Choices.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("the script package first"), Choices[0].Name, FString(TEXT("Bricks")));
	TestTrue(TEXT("with its stored manifest"), Choices[0].bHasManifest && Choices[0].Manifest.Parameters.Num() == 1 && Choices[0].Manifest.Parameters[0].Default == TEXT("5"));
	TestTrue(TEXT("then a package folder not stored yet"), Choices[1].Name == TEXT("Jump") && Choices[1].bLocal && !Choices[1].bStored);
	TestTrue(TEXT("then an entry typed by hand"), Choices[2].Name == TEXT("Hand") && Choices[2].Kind.IsEmpty());
	TestFalse(TEXT("never a vehicle, nor a folder a vehicle's name holds"), Choices.ContainsByPredicate([](const FMantleScriptPointChoice& Choice) { return Choice.Name == TEXT("Truck"); }));
	TestTrue(TEXT("the label says where a folder package comes from"), Choices[1].Label().Contains(TEXT("stored when you publish")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScriptPackagesPublishTest,
	"Mantle.ScriptPackages.Publish.BodiesVersionsAndStorage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScriptPackagesPublishTest::RunTest(const FString& Parameters)
{
	using namespace MantleScriptPackagesTestPrivate;
	FMantleScriptPackageManifest Manifest;
	TArray<FString> Problems;
	FMantleScriptPackages::ReadManifest(ValidManifest, Manifest, Problems);
	TArray<FMantleLibraryScriptFile> Files;
	FMantleLibraryScriptFile& Main = Files.AddDefaulted_GetRef();
	Main.Path = TEXT("Server/Main.luau");
	Main.Side = TEXT("server");
	Main.Language = TEXT("luau");
	Main.Source = TEXT("src_0123456789abcdef0123456789abcdef");

	// The create body: kind script, files and manifest, no asset.
	const TSharedPtr<FJsonObject> Item = ReadJsonObject(FMantleLibraryPublish::MakeScriptItemBody(Manifest, /*bPublic*/ true, Files));
	if (TestTrue(TEXT("the item body is JSON"), Item.IsValid()))
	{
		TestEqual(TEXT("kind"), Item->GetStringField(TEXT("kind")), FString(TEXT("script")));
		TestEqual(TEXT("named after the manifest"), Item->GetStringField(TEXT("name")), FString(TEXT("Kill Bricks")));
		TestFalse(TEXT("no primary asset"), Item->HasField(TEXT("primaryAssetId")));
		TestTrue(TEXT("public names the declaration"), Item->HasField(TEXT("rightsDeclarationVersion")));
		TestEqual(TEXT("one file"), Item->GetArrayField(TEXT("scripts")).Num(), 1);
		TestTrue(TEXT("and the manifest"), Item->HasTypedField<EJson::Object>(TEXT("manifest")));
	}
	const TSharedPtr<FJsonObject> PrivateItem = ReadJsonObject(FMantleLibraryPublish::MakeScriptItemBody(Manifest, /*bPublic*/ false, Files));
	TestTrue(TEXT("private sends no declaration"), PrivateItem.IsValid() && !PrivateItem->HasField(TEXT("rightsDeclarationVersion"))
		&& PrivateItem->GetStringField(TEXT("visibility")) == TEXT("private"));
	const TSharedPtr<FJsonObject> Version = ReadJsonObject(FMantleLibraryPublish::MakeScriptVersionBody(Manifest, Files));
	TestTrue(TEXT("a version is files and manifest"), Version.IsValid() && Version->Values.Num() == 2 && Version->HasField(TEXT("scripts")));

	TestEqual(TEXT("the same package keys the same create"),
		FMantleLibraryPublish::MakeScriptIdempotencyKey(TEXT("package:Content/Maps/Packages/KillBricks"), Manifest, Files, true),
		FMantleLibraryPublish::MakeScriptIdempotencyKey(TEXT("package:Content/Maps/Packages/KillBricks"), Manifest, Files, true));
	TestNotEqual(TEXT("a changed file keys another"),
		FMantleLibraryPublish::MakeScriptIdempotencyKey(TEXT("package:Content/Maps/Packages/KillBricks"), Manifest, Files, true),
		FMantleLibraryPublish::MakeScriptIdempotencyKey(TEXT("package:Content/Maps/Packages/KillBricks"), Manifest, {}, true));

	// An unchanged republish finds the version that already holds these files and manifest.
	const FString Detail = FString::Printf(TEXT("{\"id\":\"itm_u%032x\",\"versions\":[{\"version\":1,\"scripts\":[],\"manifest\":{}},{\"version\":2,\"scripts\":%s,\"manifest\":%s}]}"),
		7, *FString(TEXT("[{\"path\":\"Server/Main.luau\",\"side\":\"server\",\"language\":\"luau\",\"source\":\"src_0123456789abcdef0123456789abcdef\"}]")),
		*FMantleScriptPackages::ManifestToJson(Manifest));
	int32 Found = 0;
	TestTrue(TEXT("the unchanged version is found"), FMantleLibraryPublish::FindVersionWithScripts(Detail, Manifest, Files, Found) && Found == 2);
	FMantleScriptPackageManifest Renamed = Manifest;
	Renamed.Description = TEXT("Changed.");
	TestFalse(TEXT("a changed manifest is a new version"), FMantleLibraryPublish::FindVersionWithScripts(Detail, Renamed, Files, Found));

	// The place stores the package under its folder's name, replacing what held that name, keeping everything else.
	FMantleLibraryPackageRef Ref;
	Ref.Name = TEXT("KillBricks");
	Ref.ItemId = FString::Printf(TEXT("itm_u%032x"), 7);
	Ref.Version = 2;
	const FString PlaceJson = FString::Printf(
		TEXT("{\"schemaVersion\":1,\"entities\":[],\"storage\":[{\"name\":\"Truck\",\"item\":\"itm_u%032x\",\"version\":1},{\"name\":\"KillBricks\",\"item\":\"itm_u%032x\",\"version\":1}]}"), 1, 9);
	FString Rewritten;
	FString Error;
	if (TestTrue(TEXT("the rewrite succeeds"), FMantleLibraryPublish::RewriteScriptPackages(PlaceJson, { Ref }, Rewritten, Error)))
	{
		const TSharedPtr<FJsonObject> Root = ReadJsonObject(Rewritten);
		const TArray<TSharedPtr<FJsonValue>>& Storage = Root->GetArrayField(TEXT("storage"));
		if (TestEqual(TEXT("two entries"), Storage.Num(), 2))
		{
			TestEqual(TEXT("the vehicle stays"), Storage[0]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("Truck")));
			TestEqual(TEXT("the package's name now holds its item"), Storage[1]->AsObject()->GetStringField(TEXT("item")), Ref.ItemId);
			TestEqual(TEXT("at its version"), static_cast<int32>(Storage[1]->AsObject()->GetNumberField(TEXT("version"))), 2);
		}
	}
	FString Same;
	TestTrue(TEXT("nothing to store leaves the file byte for byte"), FMantleLibraryPublish::RewriteScriptPackages(PlaceJson, {}, Same, Error) && Same == PlaceJson);

	// A Save keeps out the points of packages not stored yet.
	const FString Draft = FString::Printf(
		TEXT("{\"schemaVersion\":1,\"entities\":[{\"id\":\"ent_a\",\"name\":\"Kept\",\"type\":\"ScriptPoint\",\"properties\":{\"storageItem\":\"Truck\"}},")
		TEXT("{\"id\":\"ent_b\",\"name\":\"Unstored\",\"type\":\"ScriptPoint\",\"properties\":{\"storageItem\":\"Jump\"}}],\"storage\":[{\"name\":\"Truck\",\"item\":\"itm_u%032x\",\"version\":1}]}"), 1);
	FString Kept;
	TArray<FString> Dropped;
	FMantleLibraryPublish::DropUnstoredScriptPoints(Draft, Kept, Dropped);
	TestTrue(TEXT("the unstored point is dropped, by name"), Dropped.Num() == 1 && Dropped[0] == TEXT("Unstored"));
	TestTrue(TEXT("the stored one stays"), Kept.Contains(TEXT("ent_a")) && !Kept.Contains(TEXT("ent_b")));

	// The level's own list: the folder's name wins, nothing else changes.
	TArray<FMantleStorageEntry> Level;
	FMantleStorageEntry Old;
	Old.Name = TEXT("Bricks_Old");
	Old.Item = Ref.ItemId;
	Old.Version = 1;
	Level.Add(Old);
	FMantleLibraryPublish::MergePackagesIntoStorage(Level, { Ref });
	TestTrue(TEXT("an entry holding the item is renamed to the folder and re-pinned"), Level.Num() == 1 && Level[0].Name == TEXT("KillBricks") && Level[0].Version == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScriptPackagesPreviewManifestTest,
	"Mantle.ScriptPackages.TestLocally.BundleCarriesEachPackageManifest",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScriptPackagesPreviewManifestTest::RunTest(const FString& Parameters)
{
	using namespace MantleScriptPackagesTestPrivate;
	const FPackagesFolder Folder;
	bool bWritten = Folder.Write(TEXT("Packages/KillBricks/package.json"), ValidManifest);
	bWritten &= Folder.Write(TEXT("Packages/KillBricks/Server/Main.luau"), TEXT("print(Script.Package)"));
	// A package without package.json still runs in a test, untyped: it gets no "packages" entry.
	bWritten &= Folder.Write(TEXT("Packages/Plain/Server/Main.luau"), TEXT("print('plain')"));
	if (!TestTrue(TEXT("the fixture is written"), bWritten))
	{
		return false;
	}
	TArray<FMantleLocalScriptPackage> Packages;
	TArray<FMantleScriptPackageProblem> Problems;
	FMantleScriptPackages::Collect(Folder.Packages(), Packages, Problems);
	if (!TestEqual(TEXT("two packages"), Packages.Num(), 2))
	{
		return false;
	}

	TestTrue(TEXT("no package with a manifest writes no key"), FMantlePreviewExport::PackagesToJson(TArray<FMantleLocalScriptPackage>()).IsEmpty());
	const FString Json = FMantlePreviewExport::PackagesToJson(Packages);
	TestTrue(TEXT("KillBricks is listed"), Json.Contains(TEXT("\"name\":\"KillBricks\"")));
	TestFalse(TEXT("a package without package.json is not"), Json.Contains(TEXT("\"name\":\"Plain\"")));

	// The bundle the export writes around it, read back by the client's loader.
	const FString Bundle = Folder.Root / TEXT("Bundle");
	bWritten = FFileHelper::SaveStringToFile(TEXT("{}"), *(Bundle / TEXT("place.json")));
	bWritten &= FFileHelper::SaveStringToFile(TEXT("print(Script.Package)"), *(Bundle / TEXT("Scripts/Packages/KillBricks/Server/Main.luau")));
	bWritten &= FFileHelper::SaveStringToFile(TEXT("print('plain')"), *(Bundle / TEXT("Scripts/Packages/Plain/Server/Main.luau")));
	bWritten &= FFileHelper::SaveStringToFile(FString::Printf(
		TEXT("{\"schemaVersion\":2,\"placeFile\":\"place.json\",\"scripts\":[")
			TEXT("{\"path\":\"Packages/KillBricks/Server/Main.luau\",\"side\":\"server\",\"file\":\"Scripts/Packages/KillBricks/Server/Main.luau\"},")
			TEXT("{\"path\":\"Packages/Plain/Server/Main.luau\",\"side\":\"server\",\"file\":\"Scripts/Packages/Plain/Server/Main.luau\"}],")
			TEXT("\"packages\":%s}"), *Json), *(Bundle / TEXT("manifest.json")));
	if (!TestTrue(TEXT("the bundle is written"), bWritten))
	{
		return false;
	}
	const FMantlePreviewManifest Loaded = MantlePreviewLaunch::LoadManifest(Bundle / TEXT("manifest.json"));
	if (!TestTrue(TEXT("the client loads the bundle"), Loaded.bSuccess))
	{
		AddError(Loaded.Error);
		return false;
	}
	if (!TestEqual(TEXT("with one package manifest"), Loaded.Packages.Num(), 1))
	{
		return false;
	}
	const FMantleScriptPackageManifest& Manifest = Loaded.Packages[0].Manifest;
	TestEqual(TEXT("under the folder's name"), Loaded.Packages[0].Name, FString(TEXT("KillBricks")));
	TestEqual(TEXT("the manifest's name"), Manifest.Name, FString(TEXT("Kill Bricks")));
	if (TestEqual(TEXT("every parameter"), Manifest.Parameters.Num(), 4))
	{
		const FMantleScriptPackageParameter* Damage = Manifest.FindParameter(TEXT("Damage"));
		TestTrue(TEXT("a number keeps its default"), Damage && Damage->Type == EMantleScriptPackageParameterType::Number && Damage->bHasDefault
			&& Damage->Default.Equals(TEXT("100"), ESearchCase::CaseSensitive));
		const FMantleScriptPackageParameter* Lethal = Manifest.FindParameter(TEXT("Lethal"));
		TestTrue(TEXT("a boolean keeps its default"), Lethal && Lethal->Type == EMantleScriptPackageParameterType::Boolean && Lethal->bHasDefault
			&& Lethal->Default.Equals(TEXT("true"), ESearchCase::CaseSensitive));
		const FMantleScriptPackageParameter* Message = Manifest.FindParameter(TEXT("Message"));
		TestTrue(TEXT("a string keeps its default"), Message && Message->Type == EMantleScriptPackageParameterType::String
			&& Message->Default.Equals(TEXT("Ouch"), ESearchCase::CaseSensitive));
	}
	return true;
}

#endif
