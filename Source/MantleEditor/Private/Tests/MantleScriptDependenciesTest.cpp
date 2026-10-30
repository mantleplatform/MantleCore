// Copyright Joshua Eger. All Rights Reserved.
#include "MantleLibraryPublish.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceSettingsActor.h"
#include "MantlePlaceStorage.h"
#include "MantlePlaceTypes.h"
#include "MantlePlaceValidator.h"
#include "MantleScriptPackages.h"

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

// Package dependencies (mantle#134) in the editor: package.json's "dependencies" read and written, a package's require() of a
// dependency's Shared module accepted, and Check this place and Export refusing a dependency the place does not store.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleScriptDependenciesTestPrivate
{
	FMantleScriptCatalogEntry Entry(const TCHAR* Path, const EMantleSide Side, const bool bModuleOnly)
	{
		FMantleScriptCatalogEntry Result;
		Result.Path = Path;
		Result.Name = FPaths::GetBaseFilename(Path);
		Result.Side = Side;
		Result.Language = EMantleScriptCatalogLanguage::Luau;
		Result.bModuleOnly = bModuleOnly;
		return Result;
	}

	FMantleScriptDependencyNode Node(const TCHAR* Name, const TArray<FString>& Dependencies, const bool bKnown = true)
	{
		FMantleScriptDependencyNode Result;
		Result.Name = Name;
		Result.bManifestKnown = bKnown;
		Result.Manifest.Dependencies = Dependencies;
		return Result;
	}

	FString ReadProblems(const TCHAR* Json)
	{
		FMantleScriptPackageManifest Manifest;
		TArray<FString> Problems;
		FMantleScriptPackages::ReadManifest(Json, Manifest, Problems);
		return FString::Join(Problems, TEXT("\n"));
	}

	struct FFolder
	{
		FString Root;
		FFolder()
		{
			Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("MantleScriptDependenciesTest"),
				FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			IFileManager::Get().MakeDirectory(*(Root / TEXT("Packages")), true);
		}
		~FFolder() { IFileManager::Get().DeleteDirectory(*Root, false, true); }
		void Write(const FString& Relative, const FString& Text) const { FFileHelper::SaveStringToFile(Text, *(Root / Relative)); }
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScriptDependenciesManifestTest,
	"Mantle.ScriptPackages.Dependencies.ManifestReadsWritesAndRefuses",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScriptDependenciesManifestTest::RunTest(const FString& Parameters)
{
	using namespace MantleScriptDependenciesTestPrivate;
	FMantleScriptPackageManifest Manifest;
	TArray<FString> Problems;
	TestTrue(TEXT("a manifest with dependencies reads"),
		FMantleScriptPackages::ReadManifest(TEXT("{\"name\": \"Hunger\", \"dependencies\": [\"QBCore\", \"Rounds\"]}"), Manifest, Problems));
	TestEqual(TEXT("in order"), FString::Join(Manifest.Dependencies, TEXT(",")), FString(TEXT("QBCore,Rounds")));
	TestTrue(TEXT("DependsOn is exact"), Manifest.DependsOn(TEXT("QBCore")) && !Manifest.DependsOn(TEXT("qbcore")) && !Manifest.DependsOn(TEXT("Other")));

	TestTrue(TEXT("the JSON carries them"), FMantleScriptPackages::ManifestToJson(Manifest).Contains(TEXT("\"dependencies\":[\"QBCore\",\"Rounds\"]")));
	FMantleScriptPackageManifest Plain;
	Plain.Name = TEXT("Plain");
	TestFalse(TEXT("a package without them writes no key, so existing packages keep the JSON they had"),
		FMantleScriptPackages::ManifestToJson(Plain).Contains(TEXT("dependencies")));
	TestEqual(TEXT("the hints reader round-trips them"),
		FString::Join(FMantleScriptPackages::ReadManifestForHints(FMantleScriptPackages::ManifestToJson(Manifest)).Dependencies, TEXT(",")), FString(TEXT("QBCore,Rounds")));

	TestTrue(TEXT("not a list"), ReadProblems(TEXT("{\"name\": \"A\", \"dependencies\": \"QBCore\"}")).Contains(TEXT("dependencies is not a list")));
	TestTrue(TEXT("not text"), ReadProblems(TEXT("{\"name\": \"A\", \"dependencies\": [\"QBCore\", 5]}")).Contains(TEXT("dependency number 2 is not text")));
	TestTrue(TEXT("not a package name"), ReadProblems(TEXT("{\"name\": \"A\", \"dependencies\": [\"QB Core\"]}")).Contains(TEXT("'QB Core' is not a package name")));
	TestTrue(TEXT("listed twice, ignoring case"), ReadProblems(TEXT("{\"name\": \"A\", \"dependencies\": [\"QBCore\", \"qbcore\"]}")).Contains(TEXT("lists the dependency 'qbcore' twice")));
	FString Many = TEXT("{\"name\": \"A\", \"dependencies\": [");
	for (int32 Index = 0; Index <= FMantlePlaceLimits::MaxScriptPackageDependencies; ++Index)
	{
		Many += FString::Printf(TEXT("%s\"P%d\""), Index ? TEXT(",") : TEXT(""), Index);
	}
	Many += TEXT("]}");
	TestTrue(TEXT("more than 16"), ReadProblems(*Many).Contains(TEXT("lists 17 dependencies; a package has at most 16")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScriptDependenciesCollectTest,
	"Mantle.ScriptPackages.Dependencies.CollectStampsEntriesAndRefusesSelf",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScriptDependenciesCollectTest::RunTest(const FString& Parameters)
{
	using namespace MantleScriptDependenciesTestPrivate;
	FFolder Folder;
	Folder.Write(TEXT("Packages/Hunger/package.json"), TEXT("{\"name\": \"Hunger\", \"dependencies\": [\"QBCore\"]}"));
	Folder.Write(TEXT("Packages/Hunger/Server/Main.luau"), TEXT("local C = require(\"@QBCore/Config\")"));
	Folder.Write(TEXT("Packages/QBCore/package.json"), TEXT("{\"name\": \"QBCore\"}"));
	Folder.Write(TEXT("Packages/QBCore/Shared/Config.luau"), TEXT("return {}"));
	Folder.Write(TEXT("Packages/Selfish/package.json"), TEXT("{\"name\": \"Selfish\", \"dependencies\": [\"Selfish\"]}"));
	Folder.Write(TEXT("Packages/Selfish/Server/Main.luau"), TEXT("print(1)"));

	TArray<FMantleLocalScriptPackage> Packages;
	TArray<FMantleScriptPackageProblem> Problems;
	FMantleScriptPackages::Collect(Folder.Root / TEXT("Packages"), Packages, Problems);
	const FMantleLocalScriptPackage* Hunger = Packages.FindByPredicate([](const FMantleLocalScriptPackage& P) { return P.Name == TEXT("Hunger"); });
	const FMantleLocalScriptPackage* QBCore = Packages.FindByPredicate([](const FMantleLocalScriptPackage& P) { return P.Name == TEXT("QBCore"); });
	const FMantleLocalScriptPackage* Selfish = Packages.FindByPredicate([](const FMantleLocalScriptPackage& P) { return P.Name == TEXT("Selfish"); });
	if (!TestTrue(TEXT("all three are collected"), Hunger && QBCore && Selfish))
	{
		return false;
	}
	TestTrue(TEXT("Hunger is valid"), Hunger->bValid);
	TestTrue(TEXT("each of its files carries the dependency list"),
		Hunger->Entries.Num() > 0 && !Hunger->Entries.ContainsByPredicate([](const FMantleScriptCatalogEntry& E) { return E.PackageDependencies != TArray<FString>{ TEXT("QBCore") }; }));
	TestTrue(TEXT("QBCore's files carry none"), !QBCore->Entries.ContainsByPredicate([](const FMantleScriptCatalogEntry& E) { return E.PackageDependencies.Num() > 0; }));
	TestFalse(TEXT("a package listing itself is not valid"), Selfish->bValid);
	TestTrue(TEXT("and says why"), Problems.ContainsByPredicate([](const FMantleScriptPackageProblem& P) { return P.Problem.Contains(TEXT("lists Selfish as its own dependency")); }));

	// A package's require() of a dependency's Shared module, by the runtime's rules.
	TArray<FMantleScriptCatalogEntry> Entries;
	Entries.Append(Hunger->Entries);
	Entries.Append(QBCore->Entries);
	const FMantleScriptCatalogEntry* Target = nullptr;
	FString Error;
	TestTrue(TEXT("Hunger reaches QBCore's Shared module"),
		FMantleScriptPackages::ResolveStaticRequire(Entries, Hunger->Entries[0], TEXT("@QBCore/Config"), {}, Target, Error) && Target && Target->Path == TEXT("Packages/QBCore/Shared/Config.luau"));
	TestFalse(TEXT("QBCore does not list Hunger"),
		FMantleScriptPackages::ResolveStaticRequire(Entries, QBCore->Entries[0], TEXT("@Hunger/Server"), {}, Target, Error));
	TestTrue(TEXT("with the dependencies field in the message"), Error.Contains(TEXT("does not list Hunger as a dependency")));
	// A listed dependency that is only stored (no local folder) is accepted unseen: its files arrive when the place runs.
	TArray<FMantleScriptCatalogEntry> HungerOnly = Hunger->Entries;
	TestTrue(TEXT("a stored dependency is accepted unseen"),
		FMantleScriptPackages::ResolveStaticRequire(HungerOnly, Hunger->Entries[0], TEXT("@QBCore/Whatever"), { TEXT("QBCore") }, Target, Error) && Target == nullptr);
	TestFalse(TEXT("but a stored package it does not list is not"),
		FMantleScriptPackages::ResolveStaticRequire(HungerOnly, Hunger->Entries[0], TEXT("@Other/Whatever"), { TEXT("Other") }, Target, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScriptDependenciesCheckTest,
	"Mantle.ScriptPackages.Dependencies.CheckFindsMissingKindsAndCycles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScriptDependenciesCheckTest::RunTest(const FString& Parameters)
{
	using namespace MantleScriptDependenciesTestPrivate;
	TMap<FString, FString> Others;
	Others.Add(TEXT("Truck"), TEXT("vehicle"));

	TArray<FMantleScriptDependencyNode> Fine = { Node(TEXT("Hunger"), { TEXT("QBCore") }), Node(TEXT("QBCore"), {}), Node(TEXT("Typed"), {}, false), Node(TEXT("Uses"), { TEXT("Typed") }) };
	TestEqual(TEXT("a stored dependency, and a hand-typed one, are fine"), FMantleScriptPackages::CheckDependencies(Fine, Others).Num(), 0);

	const TArray<FMantleScriptDependencyIssue> Missing = FMantleScriptPackages::CheckDependencies({ Node(TEXT("Hunger"), { TEXT("QBCore") }) }, Others);
	if (TestEqual(TEXT("a dependency nothing holds"), Missing.Num(), 1))
	{
		TestTrue(TEXT("is Missing"), Missing[0].Problem == EMantleScriptDependencyProblem::Missing);
		TestTrue(TEXT("and named"), Missing[0].Message.Contains(TEXT("Hunger needs the package QBCore, which this place does not store")));
		TestTrue(TEXT("with the exact-name remedy"), Missing[0].Remedy.Contains(TEXT("must be exactly QBCore")));
	}

	const TArray<FMantleScriptDependencyIssue> NotScript = FMantleScriptPackages::CheckDependencies({ Node(TEXT("Hunger"), { TEXT("Truck") }) }, Others);
	if (TestEqual(TEXT("a dependency held by a vehicle"), NotScript.Num(), 1))
	{
		TestTrue(TEXT("is NotAScript"), NotScript[0].Problem == EMantleScriptDependencyProblem::NotAScript && NotScript[0].Message.Contains(TEXT("is a vehicle")));
	}

	const TArray<FMantleScriptDependencyIssue> Cycle = FMantleScriptPackages::CheckDependencies(
		{ Node(TEXT("B"), { TEXT("A") }), Node(TEXT("A"), { TEXT("B") }), Node(TEXT("C"), { TEXT("A") }) }, Others);
	if (TestEqual(TEXT("two packages listing each other give one warning"), Cycle.Num(), 1))
	{
		TestTrue(TEXT("a Cycle"), Cycle[0].Problem == EMantleScriptDependencyProblem::Cycle && Cycle[0].Message.Contains(TEXT("A and B list each other")));
	}
	TestEqual(TEXT("a chain is not a cycle"),
		FMantleScriptPackages::CheckDependencies({ Node(TEXT("A"), { TEXT("B") }), Node(TEXT("B"), { TEXT("C") }), Node(TEXT("C"), {}) }, Others).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScriptDependenciesPlaceTest,
	"Mantle.ScriptPackages.Dependencies.CheckAndExportRefuseAMissingDependency",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScriptDependenciesPlaceTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleScriptDependenciesTestWorld"));
	if (!TestNotNull(TEXT("a world"), World))
	{
		return false;
	}
	AMantlePlaceSettingsActor* Settings = World->SpawnActor<AMantlePlaceSettingsActor>();
	if (TestNotNull(TEXT("settings"), Settings))
	{
		FMantleStorageSetting Hunger;
		Hunger.Name = TEXT("Hunger");
		Hunger.Item = TEXT("itm_u00000000000000000000000000000001");
		Hunger.Version = 1;
		Hunger.Kind = TEXT("script");
		Hunger.Manifest = TEXT("{\"name\":\"Hunger\",\"description\":\"\",\"dependencies\":[\"QBCore\"],\"parameters\":[],\"settings\":[]}");
		Settings->Storage = { Hunger };

		const auto HasNeed = [](const FMantleValidationResult& Result)
		{
			return Result.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
			{
				return Issue.Severity == EMantleIssueSeverity::Error && Issue.Problem.Contains(TEXT("Hunger needs the package QBCore"));
			});
		};
		TestTrue(TEXT("Check this place refuses a dependency the place does not store"), HasNeed(FMantlePlaceValidator::Validate(World)));
		const FMantleExportResult Exported = FMantlePlaceExporter::Export(World, FMantleExportOptions());
		TestTrue(TEXT("and so does Export"), Exported.Errors.ContainsByPredicate([](const FMantleParseError& Error) { return Error.Message.Contains(TEXT("Hunger needs the package QBCore")); }));

		FMantleStorageSetting Core;
		Core.Name = TEXT("QBCore");
		Core.Item = TEXT("itm_u00000000000000000000000000000002");
		Core.Version = 1;
		Core.Kind = TEXT("script");
		Settings->Storage.Add(Core);
		TestFalse(TEXT("storing QBCore clears it"), HasNeed(FMantlePlaceValidator::Validate(World)));
		const FMantleExportResult Again = FMantlePlaceExporter::Export(World, FMantleExportOptions());
		TestFalse(TEXT("for Export too"), Again.Errors.ContainsByPredicate([](const FMantleParseError& Error) { return Error.Message.Contains(TEXT("needs the package QBCore")); }));

		Settings->Storage[1].Kind = TEXT("vehicle");
		TestTrue(TEXT("a vehicle named QBCore is no script package"), FMantlePlaceValidator::Validate(World).Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
		{
			return Issue.Problem.Contains(TEXT("the storage entry named QBCore is a vehicle"));
		}));
	}
	World->DestroyWorld(/*bInformEngineOfWorld*/ false);
	if (World->IsRooted())
	{
		World->RemoveFromRoot();
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
