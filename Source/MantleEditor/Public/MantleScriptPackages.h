// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"
#include "MantleScriptCatalogFiles.h"

class FJsonObject;
class UWorld;

/** Something about a level's Packages/ folder that Check, Test locally or Publish refuses or warns about (#838), in plain words. */
struct MANTLEEDITOR_API FMantleScriptPackageProblem
{
	/** The package folder's name, or empty for the Packages/ folder as a whole. */
	FString Package;

	/** What is wrong, naming the file or folder ("Packages/KillBricks has no package.json."). */
	FString Problem;

	/** What to do about it. */
	FString Remedy;

	/** False for a warning: the package still runs and publishes. */
	bool bError = true;
};

/** One file of a script package as Publish sends it (#838): a library item version's {path, side, language, source}, with the text to upload. */
struct MANTLEEDITOR_API FMantleScriptPackageFile
{
	/** Relative to the package folder: "Server/Main.luau". */
	FString Path;

	/** "server", "client" or "shared": the path's top folder. */
	FString Side;

	/** The version file's language: "luau", "javascript", "typescript" or "python". */
	FString Language;

	/** POST /scripts' language for Source: a TypeScript file uploads its compiled JavaScript, as a place's TypeScript script does. */
	FString UploadLanguage;

	/** The text uploaded and run: the file as written, or for TypeScript the plugin-compiled JavaScript. */
	FString Source;
};

/** What is wrong with one value a place gives a package option (#838 stage 6): a storage setting or a Script Point parameter. */
enum class EMantleScriptOptionProblem : uint8
{
	/** The package declares no option of that name (storage_setting_unknown). */
	Unknown,
	/** The value does not fit the option's type, range or options (storage_setting_invalid). */
	Invalid,
	/** A required option with no default has no value (storage_setting_required). */
	Required,
};

/** One problem CheckOptionValues found, in plain words naming the package and the option. */
struct MANTLEEDITOR_API FMantleScriptOptionIssue
{
	/** The option's key as the place wrote it (or, for a missing required one, as the package declares it). */
	FString Key;
	EMantleScriptOptionProblem Problem = EMantleScriptOptionProblem::Invalid;
	/** "KillBricks needs RoundLength.", "KillBricks's Speed must be between 1 and 10." */
	FString Message;
};

/** A Packages/<Name>/ folder beside a level (#838): a script package a creator authors, tests locally, and publishes as a library item. */
struct MANTLEEDITOR_API FMantleLocalScriptPackage
{
	/** The folder's name: the storage name the place stores the package under, what Script Points and require("@Name/...") name. */
	FString Name;

	/** The absolute folder. */
	FString Folder;

	/** package.json read and valid; Manifest is then what it declares. */
	bool bHasManifest = false;
	FMantleScriptPackageManifest Manifest;

	/** The package's scripts, mounted as the runtime mounts them ("Packages/<Name>/Server/Main.luau"), sorted by path. */
	TArray<FMantleScriptCatalogEntry> Entries;

	/** No problem with the package is an error: it runs locally and can be published. */
	bool bValid = false;
};

/**
 * Script packages authored beside a level (#838): the Packages/ folder next to Scripts/ and UI/, one folder per package, each with
 * a package.json manifest and Server/, Client/ and Shared/ folders laid out and written exactly as a level's Scripts/ are. The rules
 * here are the backend's (gamelib.checkScriptPackage and checkManifest) and the runtime's (FMantleDiskScriptCatalogSource), applied
 * before anything is uploaded so Check and Publish say the same thing. Everything but GetPackagesFolder is pure or reads only the
 * folder it is given, so the rules are automation tests.
 */
class MANTLEEDITOR_API FMantleScriptPackages
{
public:
	/** The longest manifest name, description and parameter help (the API's MaxNameRunes, MaxDescriptionRunes, MaxParameterHelpRunes). */
	static constexpr int32 MaxManifestNameLength = 64;
	static constexpr int32 MaxManifestDescriptionLength = 1000;
	static constexpr int32 MaxParameterHelpLength = 200;

	/**
	 * The script API minor a place that uses script packages or Script Points records at least: 1.36 (#838 stage 6), the first
	 * that reads package settings (Script.Settings) and the richer option types, so a package's scripts may rely on them.
	 */
	static constexpr int32 MinScriptApiMinor = 36;

	/** A package folder's manifest file. */
	static constexpr const TCHAR* ManifestFileName = TEXT("package.json");

	/** World's Packages/ folder, beside its Scripts/ and UI/ (Content/Foo/Packages/ for /Game/Foo/MyLevel); empty when it has none. */
	static FString GetPackagesFolder(UWorld* World);

	/**
	 * Reads every package under PackagesFolder (a folder named Packages). Each subfolder is one package: its name must be a storage
	 * name, it needs a valid package.json (ReadManifest), and 1 to FMantlePlaceLimits::MaxScriptPackageFiles scripts classified exactly
	 * as the runtime classifies them, each path held to the place's script path rules. A file the runtime would skip, a loose file
	 * directly under Packages/, two paths that differ only in case: each is a problem naming it. A visual-script graph (.uasset) whose
	 * compiled .luau sits beside it runs and publishes as that Luau, with a warning saying so. A folder whose name is not a storage name
	 * is reported and left out of OutPackages (the runtime never mounts it); every other folder is in OutPackages, bValid saying whether
	 * it had errors. Sorted by name.
	 */
	static void Collect(const FString& PackagesFolder, TArray<FMantleLocalScriptPackage>& OutPackages, TArray<FMantleScriptPackageProblem>& OutProblems);

	/**
	 * Reads package.json by the backend's rules: a JSON object with a name (1 to 64 characters, no control characters), an optional
	 * description (at most 1000), optional parameters (at most FMantlePlaceLimits::MaxScriptPointParameters) and (#838 stage 6) optional
	 * settings (at most MaxStorageSettings), both lists of options {name, type, default, help, required, min, max, options}: a Script
	 * Point parameter key, unique in its list ignoring letter case; type "string", "number", "boolean" or "choice"; a default written as
	 * that JSON type (5, true, "text"; a choice's as text), at most 256 characters with no leading or trailing spaces, and needed for a
	 * number, a boolean or a choice unless the option is required; help at most 200 characters on one line; required true or false;
	 * min and max finite numbers on a number only, min at most max, the default between them; options on a choice only, 2 to 32 texts
	 * of 1 to 64 characters, unique (case-sensitive), the default one of them. Other keys are ignored. False, with every problem in
	 * plain words, when anything breaks a rule; OutManifest holds what was read either way (texts trimmed, a default as its text).
	 */
	static bool ReadManifest(const FString& Json, FMantleScriptPackageManifest& OutManifest, TArray<FString>& OutProblems);

	/**
	 * A manifest read for display (a Script Point's or a storage entry's rows), never refusing: package.json and the API's manifest
	 * alike, by the runtime's lenient rules (FMantleStorage::ReadScriptPackageManifest), so a default may be written as its type or as
	 * text ("5").
	 */
	static FMantleScriptPackageManifest ReadManifestForHints(const FString& Json);

	/**
	 * The manifest as the API takes and answers it: {name, description, parameters: [option], settings: [option]}, each option {name,
	 * type, default, help, required} plus min and max (a number that has them) and options (a choice), every default as text.
	 */
	static TSharedRef<FJsonObject> ManifestToJsonObject(const FMantleScriptPackageManifest& Manifest);
	static FString ManifestToJson(const FMantleScriptPackageManifest& Manifest);

	/** "string", "number", "boolean" or "choice". */
	static const TCHAR* ParameterTypeName(EMantleScriptPackageParameterType Type);

	/**
	 * A number as an option value is written (#838 stage 6), the backend's rule: no surrounding spaces, an optional sign, digits with an
	 * optional fraction and exponent, finite. True with OutNumber set when Text is one.
	 */
	static bool ParseOptionNumber(const FString& Text, double& OutNumber);

	/** A number written for a creator: 10 not 10.0, 2.5 as is. */
	static FString FormatOptionNumber(double Number);

	/**
	 * Why Value does not fit Option (#838 stage 6), in plain words naming the package, or empty when it fits: a number that is not one
	 * or is outside min and max ("KillBricks's Speed must be between 1 and 10."), a boolean that is not exactly true or false, a choice
	 * that is not exactly one of the options, any value longer than 256 characters. A string fits otherwise. An empty value is judged as
	 * any other (a required option's empty value is CheckOptionValues' to report as missing).
	 */
	static FString OptionValueProblem(const FString& Package, const FMantleScriptPackageParameter& Option, const FString& Value);

	/**
	 * The backend's rules for the values a place gives a package's options (#838 stage 6, places.checkStorageSettings), applied to a
	 * storage entry's settings (Declared the manifest's Settings, Noun "setting") or a Script Point's parameters (its Parameters,
	 * "parameter"): a key Declared does not name exactly is Unknown (a near miss in letter case says so); a required option with no
	 * default that is set empty or not set at all is Required; a value OptionValueProblem refuses is Invalid. Keys in order; missing
	 * required options last, in declaration order. Pure.
	 */
	static TArray<FMantleScriptOptionIssue> CheckOptionValues(const FString& Package, const TArray<FMantleScriptPackageParameter>& Declared,
		const TMap<FString, FString>& Values, const TCHAR* Noun);

	/**
	 * Drops the settings a manifest does not declare (a new version that removed one), keys compared exactly. Returns the dropped keys,
	 * sorted, for the caller's log line.
	 */
	static TArray<FString> PruneSettings(TMap<FString, FString>& Settings, const FMantleScriptPackageManifest& Manifest);

	/**
	 * The manifest a place's package named Name is checked against (#838 stage 6): the Packages/<Name>/ folder's when Local has it with
	 * a valid package.json (publishing stores that folder under Name), else StoredManifestJson (a storage entry's recorded manifest)
	 * when there is one. False when neither is known: the backend checks the values at publish.
	 */
	static bool KnownManifest(const FString& Name, const TArray<FMantleLocalScriptPackage>& Local, const FString& StoredManifestJson,
		FMantleScriptPackageManifest& OutManifest);

	/** An entry's path inside its package ("Server/Main.luau" for "Packages/KillBricks/Server/Main.luau"); the path itself for a place script. */
	static FString PathInPackage(const FString& CatalogPath);

	/**
	 * The files Publish sends for Package, in path order: each script's path in the package, side, language and the text uploaded.
	 * TypeScript is compiled (FMantlePlaceExporter::CompileTypeScript) and uploads as JavaScript. False, with OutError naming the file,
	 * when the package is not valid or a TypeScript file does not compile.
	 */
	static bool PrepareFiles(const FMantleLocalScriptPackage& Package, TArray<FMantleScriptPackageFile>& OutFiles, FString& OutError);

	/** True with OutPackage set when Request is "@<storage name>/<more>", the way a place script requires a package's Shared/ module. */
	static bool ParsePackageRequest(const FString& Request, FString& OutPackage);

	/**
	 * Resolves one static require() of From against Entries (a level's own scripts and its local packages' files, as the runtime mounts
	 * them) with the runtime's rules (MantleResolveRequire). A place script's "@Name/Thing" whose Name is not a local package but is one
	 * of StoredPackages (storage entries that are, or may be, script packages) is accepted with no target: the package is fetched only
	 * when the place runs. False with OutError otherwise.
	 */
	static bool ResolveStaticRequire(const TArray<FMantleScriptCatalogEntry>& Entries, const FMantleScriptCatalogEntry& From, const FString& Request,
		const TArray<FString>& StoredPackages, const FMantleScriptCatalogEntry*& OutTarget, FString& OutError);

	/**
	 * The script API minor a place records: Need (what its scripts use), raised to MinScriptApiMinor when it uses script packages
	 * (local, stored as a script entry, or named by a Script Point), never above the API manifest's newest minor (FMantleApiManifest).
	 */
	static int32 RequiredScriptApiMinor(int32 Need, bool bUsesPackages);

	/** The key the package's library item is remembered under for an account and API (UMantleSettings): its folder, project-relative. */
	static FString MemoryKey(const FMantleLocalScriptPackage& Package);

	/**
	 * Test locally's placeholder storage for the local packages no entry names yet (a test cannot fetch a stored item, and a Script
	 * Point naming a package must name a storage entry to parse): one {Name, placeholder item, 1} per such package, appended to
	 * Storage, each placeholder item distinct ("itm_u" then 32 hex digits starting c0de). Never published. Returns how many were added.
	 */
	static int32 AddPreviewStorage(TArray<FMantleStorageEntry>& Storage, const TArray<FMantleLocalScriptPackage>& Packages);
};
