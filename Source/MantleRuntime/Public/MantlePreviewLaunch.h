// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h" // FMantleScriptPackageManifest (#838)

/**
 * One script a preview bundle lists (#563). Path is relative to Scripts/ ("Server/Health.luau"),
 * the same spelling a published place and the disk catalogue use.
 */
struct FMantlePreviewScript
{
	/** Which side the manifest says the script runs on; always agrees with Path's top folder. */
	enum class ESide : uint8
	{
		Server,
		Client,
		Shared
	};

	FString Path;
	ESide Side = ESide::Shared;

	/**
	 * The file extension without its dot (luau, lua, js, ts, py), one of the ones
	 * MantleScriptLanguageFromExtension knows. MantleScript depends on MantleRuntime, so this module
	 * hands the extension over and the script system picks the language from it, as it does for
	 * every other source.
	 */
	FString Extension;

	FString Source;

	/**
	 * A schema 2 .ts entry's optional TypeScript source map (v3 JSON) for Source, the compiled
	 * JavaScript, so a script error reports the .ts line. Empty when the manifest has none.
	 */
	FString SourceMap;
};

/** One UI document or stylesheet a schema 2 bundle lists. Path is the place's spelling ("UI/Shop.rml"). */
struct FMantlePreviewUIDocument
{
	FString Path;
	FString Text;
};

/**
 * One cooked asset a schema 2 bundle carries: the file holds the bytes a published asset's cooked object
 * would, so the client reads it as it reads a download. The store verifies size and blake3 when it seeds it.
 */
struct FMantlePreviewAsset
{
	/** ast_u and 32 lowercase hex digits. */
	FString Id;

	/** The string POST /assets/resolve returns as kind for this asset type. */
	FString Kind;

	/** Absolute path of the cooked bytes. */
	FString File;

	/** "blake3:" and 64 lowercase hex digits over File. */
	FString CookedHash;
	int64 CookedBytes = 0;

	/** Optional, as the upload's cookOptions role (a texture). */
	FString Role;

	/** Materials only: slot name to the id of another asset of this bundle. */
	TMap<FString, FString> Textures;
};

/**
 * One weapon document a schema 2 bundle carries (#729): the .weapon.json text, and the map from each local name the
 * document uses to the id of an asset of this bundle (a cooked skeletal mesh) that stands for it.
 */
struct FMantlePreviewWeapon
{
	/** The manifest's spelling of the file ("weapons/My_Rifle.weapon.json"), for messages. */
	FString File;

	/** The file's text. */
	FString Text;

	/** Local name to the id of an asset in FMantlePreviewManifest::Assets. */
	TMap<FString, FString> Assets;

	/** Optional: the cooked material asset id for each slot of the weapon's model, in slot order; an empty entry leaves the slot alone. */
	TArray<FString> Materials;

	/** True when the player should be handed this weapon, equipped, once they spawn. */
	bool bEquip = false;
};

/**
 * One script package a schema 2 bundle carries (#838): its storage name (the Packages/<Name>/ its scripts are listed under)
 * and its package.json, read as a stored package's manifest is (FMantleStorage::ReadScriptPackageManifest), so a Script
 * Point's parameters are typed and defaulted in a preview exactly as in a published place.
 */
struct FMantlePreviewPackage
{
	FString Name;
	FMantleScriptPackageManifest Manifest;
};

/** What a preview manifest resolved to. When bSuccess is false only Error is meaningful. */
struct FMantlePreviewManifest
{
	bool bSuccess = false;
	int32 SchemaVersion = 0;

	/** The absolute path of the manifest, and of the bundle folder it sits in. */
	FString ManifestFile;
	FString BundleDirectory;

	/** The absolute path of the place file; the builder reads it as a local -MantlePlace file would. */
	FString PlaceFile;

	TArray<FMantlePreviewScript> Scripts;

	/** Schema 2: the place's UI documents and stylesheets, read as text. Empty for schema 1. */
	TArray<FMantlePreviewUIDocument> UIDocuments;

	/** Schema 2: every uploaded asset the place (and its materials) names. Empty for schema 1. */
	TArray<FMantlePreviewAsset> Assets;

	/** Schema 2: the weapon documents (#729) to register and, when asked, equip. Empty when the manifest has no "weapons". */
	TArray<FMantlePreviewWeapon> Weapons;

	/** Schema 2: the script packages' manifests (#838), by storage name. Empty when the manifest has no "packages". */
	TArray<FMantlePreviewPackage> Packages;

	/** Schema 2: the avatar's descriptor file and builtin folder, absolute; both empty when the manifest has no avatar. */
	FString AvatarDescriptorFile;
	FString AvatarBuiltinDirectory;

	/** Written for the creator; says which entry was refused and why. */
	FString Error;
};

/**
 * The bounds of a local preview bundle. Place and script sizes are the publishing limits (the
 * backend's places.MaxPlaceFileBytes and scripts.MaxScriptSourceBytes), so a bundle that previews is a
 * bundle that publishes; the manifest and script count only keep the bundle finite.
 */
namespace MantlePreviewLimits
{
	constexpr int64 MaxManifestBytes = 1024 * 1024;
	constexpr int64 MaxPlaceFileBytes = 8 * 1024 * 1024;
	constexpr int64 MaxScriptBytes = 1024 * 1024;
	constexpr int64 MaxTotalScriptBytes = 8 * 1024 * 1024;
	constexpr int32 MaxScripts = 256;

	/** Schema 2: the asset count, one asset file and all asset files together. */
	constexpr int32 MaxAssets = 4096;
	constexpr int64 MaxAssetBytes = 128ll * 1024 * 1024;
	constexpr int64 MaxTotalAssetBytes = 2048ll * 1024 * 1024;

	/** Schema 2: the weapon documents a bundle may carry (#729), and one document's size. */
	constexpr int32 MaxWeapons = 8;
	constexpr int64 MaxWeaponBytes = 256 * 1024;
}

/** The offline preview launch policy: a Mantle process started with -MantlePreview. */
namespace MantlePreviewLaunch
{
	/**
	 * True when the command line has any -MantlePreview token, with or without =value, or the
	 * -MantlePreviewCapabilities=<path> query below. A malformed value still means offline.
	 */
	MANTLERUNTIME_API bool IsRequested(const TCHAR* CommandLine);

	/** This process's policy, read from the command line once and cached; safe from any thread. */
	MANTLERUNTIME_API bool IsRequested();

	/** The value of -MantlePreview=<path> in CommandLine, or empty when the token is absent or has no value. */
	MANTLERUNTIME_API FString ManifestPathFromCommandLine(const TCHAR* CommandLine);

	/**
	 * Reads and validates a manifest: { "schemaVersion": 1, "placeFile": "place.json", "scripts":
	 * [ { "path": "Server/Health.luau", "side": "server", "file": "Server/Health.luau" } ] }.
	 * Every file is resolved against the manifest's own folder and read here. Never touches the network.
	 *
	 * Schema 2 adds optional "uiDocuments" ([ { "path": "UI/Shop.rml", "file": "UI/Shop.rml" } ], read here as
	 * text), "assets" ([ { "id", "kind", "file", "cookedHash", "cookedBytes", "role"?, "textures"? } ]; the file's
	 * size is checked here and its blake3 when the asset store seeds it) and "avatar" ({ "descriptor", "builtin" },
	 * resolved only). Schema 1 manifests are unchanged.
	 *
	 * #838: schema 2 may add "packages" ([ { "name": "KillBricks", "manifest": { ...package.json... } } ]): name a storage
	 * name, listed once, under which the scripts list at least one Packages/<name>/ file; manifest an object, read leniently
	 * as a stored package's manifest is. Optional, so a bundle without packages reads as before.
	 */
	MANTLERUNTIME_API FMantlePreviewManifest LoadManifest(const FString& ManifestFile);

	/**
	 * This process's manifest, loaded once from -MantlePreview=<path> on first use. A missing or
	 * invalid manifest comes back with bSuccess false and an Error; it never falls back online.
	 * The first call also starts FMantlePreviewStatus beside the manifest and records a load
	 * failure there.
	 */
	MANTLERUNTIME_API const FMantlePreviewManifest& ForThisProcess();

	/** Strict lexical bundle-relative path: no absolute path, drive letter, UNC, "..", empty segment or backslash. */
	MANTLERUNTIME_API bool IsBundleRelativePath(const FString& Path);

	/** What preview protocol 1 is: stages 1 to 3 of #563 (offline run, status file, capabilities query). */
	constexpr int32 PreviewProtocol = 1;

	/**
	 * True when CommandLine has the -MantlePreviewCapabilities token, with or without a value. The
	 * editor asks a client what it can preview this way before exporting: the process answers and exits
	 * without ever opening a window, signing in or reaching a service.
	 */
	MANTLERUNTIME_API bool IsCapabilitiesQuery(const TCHAR* CommandLine);

	/** The value of -MantlePreviewCapabilities=<path>, or empty when the token is absent or has no value. */
	MANTLERUNTIME_API FString CapabilitiesPathFromCommandLine(const TCHAR* CommandLine);

	/**
	 * Writes the capabilities file the editor reads: previewProtocol, manifestSchemas, scriptApi,
	 * clientVersion, cookTool and features (MantleClientFeatures::All, #628). Temp file then replace, so a reader never sees half of it.
	 * ScriptApiMajor and ScriptApiMinor are FMantleApi's, which MantleScript owns and this module
	 * can't see, so the caller hands them in. Returns false, writing nothing, when Path is not an
	 * absolute file path or can't be written.
	 */
	MANTLERUNTIME_API bool WriteCapabilities(const FString& Path, int32 ScriptApiMajor, int32 ScriptApiMinor);
}

/**
 * The preview process's report to whoever launched it: <manifest folder>/preview-status.json. A
 * Shipping client writes no log, so this file is how a harness or the editor learns the preview
 * is ready, what scripts printed and whether anything tried to reach a production service.
 *
 * Thread-safe: requests are refused on whatever thread issues them. Writes go to a temp file that
 * replaces the status file, at most every 250 ms (a state change writes at once) and once more on exit.
 * Nothing here relies on UE_LOG.
 */
class MANTLERUNTIME_API FMantlePreviewStatus
{
public:
	enum class EState : uint8
	{
		Starting,
		Ready,
		Failed
	};

	enum class ELevel : uint8
	{
		Log,
		Warn,
		Error
	};

	/** Messages kept in the file; the oldest are dropped past this. */
	static constexpr int32 MaxMessages = 500;

	static constexpr double MinWriteIntervalSeconds = 0.25;

	static FMantlePreviewStatus& Get();

	/** Starts reporting to <folder of ManifestFile>/preview-status.json and writes state "starting". Counts and messages from before this call are kept. */
	void Begin(const FString& ManifestFile);

	/**
	 * Marks one readiness stage done. The state becomes "ready" once geometry, scripts and spawn are all
	 * done and nothing failed.
	 */
	void MarkGeometryReady();
	void MarkScriptsReady();
	void MarkSpawnReady();

	/** State "failed", with the reason. Final: nothing moves a failed preview back to ready. */
	void Fail(const FString& Error);

	void AddMessage(ELevel Level, const FString& Source, const FString& Text);

	/** One request that was refused instead of sent. What names it for the first few messages; keep it free of tokens and query strings. */
	void NoteBlockedRequest(const FString& What);

	/** Writes now, whatever the throttle says. Also runs on exit. */
	void Flush();

	/** Fires once, on the thread that completed the last stage, when the state becomes "ready". The game starts
	    -MantlePreviewShots from it. */
	FSimpleMulticastDelegate OnReady;

	/** This snapshot as the file's JSON; what Flush writes. Public so tests can read it without a disk. */
	FString ToJson() const;

private:
	FMantlePreviewStatus() = default;

	/** Marks the snapshot dirty and writes it when the throttle allows. bForce skips the throttle. */
	void Touch(bool bForce);
	/** bFinal is the exit flush: it retries a replace that a reader is holding up. */
	void WriteNow(bool bFinal = false);
	void MarkReadyStage(bool& bStage);

	struct FMessage
	{
		double Seconds = 0.0;
		ELevel Level = ELevel::Log;
		FString Source;
		FString Text;
	};

	mutable FCriticalSection StateLock;
	FCriticalSection WriteLock;

	FString StatusFile;
	FString TempFile;
	EState State = EState::Starting;
	FString ErrorText;
	bool bGeometry = false;
	bool bScripts = false;
	bool bSpawn = false;
	TArray<FMessage> Messages;
	int32 BlockedRequests = 0;

	double StartSeconds = 0.0;
	double LastWriteSeconds = -1.0e9;
	bool bDirty = false;
	uint64 Sequence = 0;
	uint64 WrittenSequence = 0;
	bool bHooksRegistered = false;
};
