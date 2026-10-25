// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleGraphSourceMap.h"
#include "MantlePlaceTypes.h" // FMantleScriptPackageManifest (#838)
#include "MantleScriptSide.h"

// The script catalog's file rules, shared by the editor and the game: which files under a place's
// Scripts/ and Packages/ are scripts, their language and side, and how require() resolves.

/**
 * How a script's source text should be interpreted. Luau, JavaScript and Python each run on
 * their own runtime; TypeScript shares the JavaScript runtime slot (see
 * UMantleScriptWorldSubsystem::EnsureLanguageRuntime). A .wasm file under Scripts/ never gets a
 * catalogue entry.
 */
enum class EMantleScriptCatalogLanguage : uint8
{
	Unknown,
	Luau,
	JavaScript,
	TypeScript,
	Python
};

MANTLESCRIPTTOOLS_API EMantleScriptCatalogLanguage MantleScriptLanguageFromExtension(const FString& Extension);

/** One script or module file a catalogue source knows about. */
struct MANTLESCRIPTTOOLS_API FMantleScriptCatalogEntry
{
	/** Stable for the lifetime of the owning source; never persisted or sent over the network. */
	int32 Id = 0;

	/**
	 * Path relative to the place's Scripts/ folder, with the exact case it has on disk, e.g.
	 * "Server/KillBricks.luau". Every path comparison against a require() request is
	 * case-sensitive (#129) -- this field is the ground truth for "the file's real spelling".
	 *
	 * #838: a stored script package's file is mounted at "Packages/<storage name>/<its path>", e.g.
	 * "Packages/KillBricks/Server/Main.luau" (MantleScriptPackageOf reads the package back out of it).
	 */
	FString Path;

	/** The file's base name without extension, e.g. "KillBricks". */
	FString Name;

	EMantleSide Side = EMantleSide::Both;
	EMantleScriptCatalogLanguage Language = EMantleScriptCatalogLanguage::Unknown;

	/**
	 * True for anything under Scripts/Shared/ or under a folder literally named "Modules" at any
	 * depth: require()-only, never started on its own.
	 */
	bool bModuleOnly = false;

	FString Source;
	/** The source was compiled from a graph. Kept separately from the map because a valid empty
	 * graph has no node-owned Luau lines but still needs its published .graph identity. */
	bool bGraphAuthored = false;
	/** Source is already runnable JavaScript although Language says TypeScript (a published or previewed
	 *  .ts script holds the editor's compiled output), so the VM runs it as is instead of transpiling.
	 *  Disk sources leave this false: their .ts is authored text, transpiled in an editor binary only. */
	bool bPrecompiledJavaScript = false;
	/** The TypeScript source map (v3 JSON) for Source when it is precompiled JavaScript and the source
	 *  carries one (a previewed .ts script); the VM maps error lines back to the .ts file with it.
	 *  Empty for published scripts, which carry none, and for disk sources (transpiled with their own). */
	FString SourceMap;
	/** Empty for handwritten source. Parsed from generated graph Luau's versioned header by the
	 *  catalogue source, so the runtime never imports MantleEditor to interpret a traceback. */
	TArray<FMantleGraphSourceMapEntry> GraphSourceMap;
	FDateTime LastModified;
};

/**
 * A source of scripts for one world's script host. S1.2b's only source is the disk folder
 * beside a -MantlePlace file, non-Shipping builds only (FMantleDiskScriptCatalogSource). S1.10
 * adds a second source reading published, fetched script sources behind this same interface, so
 * the host never has to know which kind of place it loaded.
 */
class MANTLESCRIPTTOOLS_API IMantleScriptCatalogSource
{
public:
	virtual ~IMantleScriptCatalogSource() = default;

	/** False when there is nothing to offer at all (e.g. no Scripts/ folder next to this place). */
	virtual bool IsAvailable() const = 0;

	/** Every known entry, in a stable order. Valid until the next Poll(). */
	virtual const TArray<FMantleScriptCatalogEntry>& GetEntries() const = 0;

	/**
	 * Re-reads the underlying source, at most once per call. Returns true if anything changed
	 * since the previous call.
	 *
	 * OutChangedIds holds ids whose Source text changed in place (safe to hot-swap just that
	 * script). bOutStructureChanged is true when a file appeared, disappeared, or a module's
	 * content changed -- callers must treat that as "start over": module caches, and anything
	 * that required them, can silently hold stale state otherwise.
	 */
	virtual bool Poll(TArray<int32>& OutChangedIds, bool& bOutStructureChanged) = 0;

	/**
	 * #838: the manifest of the script package mounted as Packages/<Package>/ (Package compared case-sensitively),
	 * or null when this source has no such package or it carries no manifest. Script Point parameters are typed
	 * by it (MantleScriptPointAttributes).
	 */
	virtual const FMantleScriptPackageManifest* FindPackageManifest(const FString& Package) const { return nullptr; }

	/**
	 * #838 stage 6: the stored settings of the script package mounted as Packages/<Package>/ (raw text, by key), or null when
	 * this source does not carry them (a disk or preview source: the host then reads the loaded place's storage entry).
	 */
	virtual const TMap<FString, FString>* FindPackageSettings(const FString& Package) const { return nullptr; }
};

/**
 * S1.2b's only catalogue source: <PlaceDir>/Scripts/{Server,Client,Shared}, where any folder
 * named exactly "Modules" (at any depth, case-sensitive) is require-only. Polled for hot reload
 * every 0.5s by the owning world subsystem. Never compiled into a Shipping build -- see
 * MantleScriptWorldSubsystem.cpp, which only constructs this behind #if !UE_BUILD_SHIPPING.
 *
 * #838: also <PlaceDir>/Packages/<Name>/{Server,Client,Shared}, the script packages a creator authors beside the
 * level, mounted at "Packages/<Name>/..." exactly as a stored package is mounted under its storage name, so a package
 * runs locally as it will once published. <Name> must be a storage name (a letter, then letters, digits or '_');
 * Packages/<Name>/package.json, when present, is the package's manifest.
 */
class MANTLESCRIPTTOOLS_API FMantleDiskScriptCatalogSource : public IMantleScriptCatalogSource
{
public:
	/**
	 * PlaceFilePath is the -MantlePlace file itself; Scripts/ and Packages/ are sibling directories of it. #838:
	 * Packages/ is read only with bIncludePackages (the runtime's local launch); the exporter's place-script
	 * collection keeps the default and so never mistakes a package file for one of the place's own scripts.
	 */
	explicit FMantleDiskScriptCatalogSource(const FString& PlaceFilePath, bool bIncludePackages = false);

	virtual bool IsAvailable() const override { return !ScriptsRoot.IsEmpty() || !PackagesRoot.IsEmpty(); }
	virtual const TArray<FMantleScriptCatalogEntry>& GetEntries() const override { return Entries; }
	virtual bool Poll(TArray<int32>& OutChangedIds, bool& bOutStructureChanged) override;
	virtual const FMantleScriptPackageManifest* FindPackageManifest(const FString& Package) const override;

	/** The absolute Scripts/ directory this source reads from, or empty if there wasn't one. */
	const FString& GetScriptsRoot() const { return ScriptsRoot; }

	/** #838: the absolute Packages/ directory this source reads from, or empty if there wasn't one. */
	const FString& GetPackagesRoot() const { return PackagesRoot; }

	/**
	 * True when this process was launched to run a local -MantlePlace file, whose path is then OutPlaceFilePath.
	 * A hosted dedicated server also carries -MantlePlace=<its bootstrap file> (StartServer.sh, the community host),
	 * but with MANTLE_SERVER_TOKEN, and its scripts are the published ones (#836): false for it, so it never reads
	 * a Scripts/ folder beside a file the API wrote.
	 */
	static bool IsRequestedForLaunch(FString& OutPlaceFilePath);

private:
	/** One script file on disk: its catalogue path ("Server/X.luau" or "Packages/P/Server/X.luau") and absolute path. */
	struct FDiskScriptFile
	{
		FString CatalogPath;
		FString FullPath;
		/** Read from Packages/ (#838), so CatalogPath starts with "Packages/". */
		bool bPackage = false;
	};

	void Discover();

	/** Every file under Scripts/ and then Packages/ with a script extension and a non-dot name, each folder sorted. */
	TArray<FDiskScriptFile> GatherFiles() const;

	/** The side and require-only rule of a gathered file (MantleClassifyScriptCatalogPath's, by where it was read). */
	static bool ClassifyFile(const FDiskScriptFile& File, EMantleSide& OutSide, bool& bOutModuleOnly);

	/** The absolute path a catalogue path was read from. */
	FString FullPathOf(const FString& CatalogPath) const;

	FString ScriptsRoot;
	FString PackagesRoot;
	TArray<FMantleScriptCatalogEntry> Entries;
	/** #838: Packages/<Name>/package.json, by package name (compared case-sensitively). */
	TArray<TPair<FString, FMantleScriptPackageManifest>> PackageManifests;
	int32 NextId = 1;
};

/**
 * #838: the script package a catalogue path belongs to -- "KillBricks" for "Packages/KillBricks/Server/Main.luau" --
 * or empty for a place's own script (and for anything that is not Packages/<name>/<more>). Case-exact.
 */
MANTLESCRIPTTOOLS_API FString MantleScriptPackageOf(const FString& CatalogPath);

/**
 * The side and require-only rule of a catalogue path, the one rule every source applies: Server/ and Client/ run on
 * their side, Shared/ is require-only on both, and so is anything under a folder named exactly "Modules" at any
 * depth. #838: "Packages/<name>/<path>" is classified by <path> the same way, <name> being a storage name. False for
 * anything else (a top folder spelled in another case, a file directly under a top folder's parent).
 */
MANTLESCRIPTTOOLS_API bool MantleClassifyScriptCatalogPath(const FString& CatalogPath, EMantleSide& OutSide, bool& bOutModuleOnly);

/**
 * Case-sensitive require() resolution shared by every language binding: tries the literal path,
 * then Shared/<name>, then <RequestingSide>/<name> (skipping the alias in whichever of
 * Shared/Server/Client the calling script doesn't belong to), exactly as authored on disk.
 *
 * On failure, if a case-insensitive match exists, ExactCaseHint carries its real spelling so the
 * caller can build the #129 "did you mean" message; empty otherwise.
 *
 * #838, script packages (mounted at Packages/<name>/):
 *  - From a place script, require("@<name>/Thing") resolves to Packages/<name>/Shared/Thing, and only there, when a
 *    package of that name is mounted; otherwise the "@" is stripped as before ("@Shared/Config" is "Shared/Config").
 *    A place script never reaches a package file any other way (a literal Packages/ path is refused).
 *  - From a package file, a bare name resolves inside that package only: Packages/<name>/<Thing>, then its Shared/,
 *    then its own side's folder. A relative path must stay inside the package. A package cannot require the place's
 *    own scripts or another package (Mantle provides no modules of its own): either is refused with a message.
 *    "@<its own name>/Thing" reaches its own Shared/Thing; "@<another mounted package>/..." is refused; any other
 *    "@" is stripped, then resolved inside the package.
 */
MANTLESCRIPTTOOLS_API const FMantleScriptCatalogEntry* MantleResolveRequire(
	const TArray<FMantleScriptCatalogEntry>& Entries,
	const FString& FromPath,
	EMantleSide FromSide,
	const FString& Request,
	FString& OutError);
