// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleCoverage.h"
#include "MantleTargetFeatures.h"

class UWorld;
struct FMantleCosmeticPackage;
struct FMantleLocalScriptPackage;

/**
 * A weapon the weapon builder made, to bundle with a preview (#729). The model is cooked like any skeletal mesh in a level
 * (with its materials and textures) and the settings file travels as text; the client builds and equips the weapon from them.
 */
struct FMantleWeaponToTry
{
	/** The weapon's folder name under /Game/MantleWeapons (the builder's safe name); also the bundle file's name. */
	FString Name;

	/** The model's object path, /Game/MantleWeapons/<Name>/SKM_<Name>.SKM_<Name>. */
	FString MeshObjectPath;

	/** The weapon's .weapon.json on disk. */
	FString DocumentFile;

	/** True: the client puts the weapon in the player's hands once they spawn. */
	bool bEquip = true;

	/** The weapon the builder made under the folder convention: Content/MantleWeapons/<Name>/{SKM_<Name>, <Name>.weapon.json}. */
	static FMantleWeaponToTry FromBuilderName(const FString& SafeName);

	/** weapons/<Name>.weapon.json: where the settings file sits in the bundle. */
	FString BundleFile() const { return TEXT("weapons/") + Name + TEXT(".weapon.json"); }
};

/** One entry of the manifest's "weapons" array (#729), as the export writes it. */
struct FMantlePreviewBundleWeapon
{
	/** Bundle-relative: weapons/<Name>.weapon.json. */
	FString File;

	/** The settings file's local model name to the cooked asset's id (also listed in the manifest's assets). */
	TMap<FString, FString> Assets;

	/** The model's material asset id per slot, in the mesh's slot order ("" leaves a slot alone, as a library material does); at most 64. Every non-empty id is listed in the manifest's assets. */
	TArray<FString> Materials;

	bool bEquip = true;
};

/** What the preview export is asked to do (#563 stage 2). Everything has a default. */
struct FMantlePreviewExportOptions
{
	/** #729: weapons to bundle and equip. Needs a client that reads the weaponDocument feature (Features); otherwise the export refuses. */
	TArray<FMantleWeaponToTry> Weapons;

	/**
	 * A checked avatar the creator has not uploaded (the Cosmetics panel's Test locally): the preview player wears it instead of the
	 * chosen catalog avatar. Its files are cooked here with the cook tool and bundled as a local draft
	 * (MantlePreviewAvatarExport::WriteLocalDraft); nothing is fetched from the catalog.
	 */
	TSharedPtr<const FMantleCosmeticPackage> DraftAvatar;

	/** The local cook tool (mantle-cook.exe). Empty: the MANTLE_COOK_TOOL environment variable. */
	FString CookTool;

	/** The folder bundles go under, as <root>/<session>/<generation>/ (one session folder per editor process, one more generation per export). Empty: <Project>/Saved/Mantle/Preview. */
	FString OutputRoot;

	/**
	 * #628: the gated features the client this preview will run in reads (its capability answer's "features"). The cook is told
	 * to let their keys past the release check, and what the client lacks is left out and reported as needing a newer client.
	 * Default: every feature this build reads, which is what a bare export (no client) is: a dev export.
	 */
	FMantleTargetFeatures Features;
};

/** What the export did. When bSuccess is false only Errors is meaningful, and no generation folder is left behind. */
struct FMantlePreviewExportResult
{
	bool bSuccess = false;

	/** The creator cancelled the export. bSuccess is false, Errors is just "Cancelled.", and the generation folder is gone. */
	bool bCancelled = false;

	/** The generation folder, and the manifest.json inside it (present only on success). */
	FString BundleDirectory;
	FString ManifestFile;

	int32 NumAssets = 0;
	int32 NumScripts = 0;
	int32 NumUIDocuments = 0;
	int32 NumWeapons = 0;
	bool bHasAvatar = false;

	/**
	 * #628: what the export kept, approximated and dropped (also when it was refused: a level that fails Check still says
	 * what it would lose), and the coverage.json written beside the manifest (present only on success).
	 */
	FMantleCoverage Coverage;
	FString CoverageFile;

	/** One line each, naming the actor or asset. */
	TArray<FString> Errors;
	TArray<FString> Warnings;
};

/**
 * Writes a complete offline preview bundle for the open level (#563 stage 2): the place with
 * provisional ids, every uploaded asset cooked locally with mantle-cook, the scripts and UI
 * files, and the creator's chosen avatar. Entirely offline: no sign-in, no network, no upload.
 * The manifest is written last (as manifest.pending.json, validated, then renamed), so a
 * manifest.json in the folder means the bundle is complete.
 */
class FMantlePreviewExport
{
public:
	/**
	 * Runs the whole export for World, which must be a saved /Game level, under a progress dialog the creator can cancel
	 * (checked between stages and between cooks; a cook already running is stopped). On failure or cancel the generation
	 * folder is deleted; a cancel sets OutResult.bCancelled.
	 */
	static bool Run(UWorld* World, const FMantlePreviewExportOptions& Options, FMantlePreviewExportResult& OutResult);

	/** Override when it names a file; else MANTLE_COOK_TOOL. False, with OutError saying what to set, when neither exists. */
	static bool FindCookTool(const FString& Override, FString& OutTool, FString& OutError);

	/** <Project>/Saved/Mantle/Preview, absolute. */
	static FString DefaultOutputRoot();

	/** #729: true when the client these features describe reads a weapon settings file (the weaponDocument feature). */
	static bool ClientReadsWeapons(const FMantleTargetFeatures& Features);

	/** #729: what the creator is told when the Mantle app is too old for a weapon preview, in plain words. */
	static FString WeaponsNeedNewerClientMessage();

	/** #729: the manifest's "weapons" array as JSON text (condensed): [{"file":..,"assets":{..},"materials":[..],"equip":..}]. */
	static FString WeaponsToJson(const TArray<FMantlePreviewBundleWeapon>& Weapons);

	/**
	 * #838: the manifest's "packages" array as JSON text (condensed): [{"name":..,"manifest":{..}}], one per package with at least
	 * one script (the bundle lists its files) and a valid package.json (bHasManifest), in Packages' order. The manifest is written
	 * exactly as Publish sends it to the API (FMantleScriptPackages::ManifestToJsonObject, every default as text) and the client
	 * reads it as it reads a stored package's, so Script Point parameters are typed and defaulted as in the published place.
	 * Empty (no text at all) when no package qualifies, so the key is left out.
	 */
	static FString PackagesToJson(const TArray<FMantleLocalScriptPackage>& Packages);

	/** #729: the local model name a settings file's JSON text names ("mesh"); empty when it names none. */
	static FString ReadWeaponMeshName(const FString& DocumentText);
};
