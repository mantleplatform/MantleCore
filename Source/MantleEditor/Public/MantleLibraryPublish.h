// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleApiClient.h"
#include "MantlePlaceTypes.h"
#include "MantleProvenance.h"

/**
 * Who may use the vehicles a go-live publish turns into library items (#614 stage 6, #689). Public is the
 * default and needs the rights declaration; Private needs none and is the quieter choice.
 * One choice covers every vehicle in the publish.
 */
struct MANTLEEDITOR_API FMantleVehicleLibraryChoice
{
	bool bPublic = true;

	/** The creator ticked the declaration (MantleRights::DeclarationVersion's text). Only meaningful when bPublic. */
	bool bRightsDeclared = false;

	/**
	 * Set when bPublic is false only because no one can make the declaration (an agent's or a blocking publish), not because the creator chose
	 * private: an item that already exists keeps the visibility it has, and only a new item is created private.
	 */
	bool bKeepExistingVisibility = false;

	/** True when the choice can be sent: Private, or Public with the declaration ticked. */
	bool IsReady() const { return !bPublic || bRightsDeclared; }
};

/** One vehicle (or, #789, one weapon) this publish uploaded and the library item version that now holds it. */
struct MANTLEEDITOR_API FMantleLibraryVehicleRef
{
	/** The uploaded vehicle asset (ast_u...), as the exported place file names it. */
	FString AssetId;

	/** The vehicle source asset's name; the storage name is made from it (StorageNameFor). */
	FString Name;

	/** The library item (itm_u...) and the version the place pins. */
	FString ItemId;
	int32 Version = 0;
};

/** A vehicle the rewrite stored (RewritePlace's output): the name its spawns now use, and what the level should adopt. */
struct MANTLEEDITOR_API FMantleStoredVehicle
{
	/** The uploaded vehicle asset the place file named before the rewrite. */
	FString AssetId;

	/**
	 * The object paths of every vehicle source asset that uploaded as AssetId (set by the session): identical vehicles share
	 * one upload, and the level's spawns are matched by any of them.
	 */
	TArray<FString> Paths;

	/** The storage entry name the spawns use now. */
	FString StorageName;

	FString ItemId;
	int32 Version = 0;
};

/**
 * A script package (#838) a go-live publish made a library item version of: stored in the place under its folder's name, which
 * its Script Points and require("@Name/...") already use.
 */
struct MANTLEEDITOR_API FMantleLibraryPackageRef
{
	/** The Packages/ folder's name: the storage name. */
	FString Name;

	/** The library item (itm_u...) and the version the place pins. */
	FString ItemId;
	int32 Version = 0;

	/** The manifest it was published with, as JSON (FMantleScriptPackages::ManifestToJson): the level keeps it for a Script Point's hints. */
	FString Manifest;
};

/**
 * The pure half of making a published vehicle a library item (#614 stage 6): the storage name, the request
 * bodies, and the rewrite of the exported place file so its VehicleSpawns name storage instead of an uploaded
 * asset. No I/O, so every rule here is an automation test.
 */
class MANTLEEDITOR_API FMantleLibraryPublish
{
public:
	/**
	 * A storage name (an ASCII letter then up to 63 letters, digits or '_') from a vehicle source asset's name:
	 * other characters become '_', a name that does not start with a letter gets "Vehicle_" in front, and an
	 * empty one is "Vehicle". Never longer than 64 characters.
	 */
	static FString StorageNameFor(const FString& SourceName, const TCHAR* Fallback = TEXT("Vehicle"));

	/**
	 * The uploaded vehicle ids the place file's VehicleSpawns name (properties.vehicle), each once, in
	 * entity order. Empty for text that is not a place file.
	 */
	static TArray<FString> VehicleAssetIdsIn(const FString& PlaceJson);

	/** #789: the uploaded weapon ids the place file's WeaponSpawns name (properties.weapon), each once, in entity order. */
	static TArray<FString> WeaponAssetIdsIn(const FString& PlaceJson);

	/**
	 * Rewrites the exported place file for the vehicles Vehicles lists. For each one a VehicleSpawn uses:
	 * - storage[] gets {name, item, version}: an entry the creator already declared for the same item has its
	 *   version replaced (and keeps its name), otherwise a new entry takes StorageNameFor(Name), made unique
	 *   (a numeric suffix) among the names already there. A place pins one version of an item.
	 * - every VehicleSpawn that named the asset names that entry in storageItem instead (vehicle is dropped).
	 * - the asset leaves assets[]: a stored vehicle is expanded from its item at load, not carried by the place.
	 *   The vehicle's materials, meshes and textures stay (their other users cannot be told apart here).
	 * A vehicle no spawn uses is ignored. With nothing to rewrite OutJson is PlaceJson, byte for byte.
	 * False, with OutError in plain words, when the place would store more than FMantlePlaceLimits::
	 * MaxStorageEntries items, a ref is not a library item version, or the resulting storage breaks
	 * FMantleStorage::CheckEntries.
	 */
	static bool RewritePlace(const FString& PlaceJson, const TArray<FMantleLibraryVehicleRef>& Vehicles, FString& OutJson, FString& OutError,
		TArray<FMantleStoredVehicle>* OutStored = nullptr);

	/**
	 * #789: RewritePlace for the WeaponSpawns: each weapon a spawn uses becomes a storage entry (named from the weapon's name) and
	 * its spawns name it in storageItem instead of weapon. The refs and the stored list are the same types; a ref for an asset no
	 * WeaponSpawn uses (a vehicle's) is ignored, so one list can go through both rewrites.
	 */
	static bool RewriteWeaponSpawns(const FString& PlaceJson, const TArray<FMantleLibraryVehicleRef>& Weapons, FString& OutJson, FString& OutError,
		TArray<FMantleStoredVehicle>* OutStored = nullptr);

	/**
	 * Merges what a publish stored into the level's own storage list (Place Settings > Storage), removing nothing:
	 * an entry for the same item has its version replaced and keeps its name, otherwise an entry is added under
	 * StorageName, made unique if another item already holds it. Each Stored's StorageName is set to the name its
	 * entry ended up with, which is what the level's spawns should name.
	 */
	static void MergeIntoStorage(TArray<FMantleStorageEntry>& Storage, TArray<FMantleStoredVehicle>& Stored);

	/**
	 * POST /library/items: a vehicle item named Name. Public (the API's default) names the rights declaration; private sends
	 * visibility "private" and no declaration. A public item also sends Provenance (#614 stage 7) when it has any; a private
	 * item never does. ThumbnailAssetId, when not empty, is the uploaded 512 x 512 PNG the library card shows (#821); empty sends none.
	 */
	static FString MakeItemBody(const FString& Name, bool bPublic, const FString& PrimaryAssetId, const FMantleProvenance& Provenance = FMantleProvenance(),
		const TCHAR* Kind = TEXT("vehicle"), const FString& ThumbnailAssetId = FString());

	/** POST /library/items/{id}/versions. Provenance goes with it only when bPublic (the visibility the item has once this publish is done) and it has any. */
	static FString MakeVersionBody(const FString& PrimaryAssetId, bool bPublic = false, const FMantleProvenance& Provenance = FMantleProvenance());

	/**
	 * The Idempotency-Key for creating an item for one uploaded vehicle: the same for the same source asset,
	 * uploaded bytes, visibility and Generation, so a retried create names the item the first one made. Generation
	 * counts how often the vehicle's link was forgotten (UMantleSettings): a new item made after a delisted or deleted
	 * one needs a key the old creation never used. 0 is the first item and keeps the key it always had. 1 to 128
	 * printable characters, no spaces.
	 */
	static FString MakeIdempotencyKey(const FString& SourcePath, const FString& AssetId, bool bPublic, int32 Generation = 0);

	/**
	 * PATCH /library/items/{id}: the visibility (and, for public, the rights declaration), and the card's picture when ThumbnailAssetId is not empty (#821); nothing
	 * else. A picture the item already has is not changed by naming it again, so a republish may send it every time.
	 */
	static FString MakeVisibilityPatchBody(bool bPublic, const FString& ThumbnailAssetId = FString());

	/** Reads GET /library/items/{id}: its visibility ("public" or "private") and status ("active" or "delisted"); false when it is not an item. */
	static bool ReadItemState(const FString& ItemBody, FString& OutVisibility, FString& OutStatus);

	/** True when an item's visibility is a known one that is not the choice (an unknown or empty one is left alone). */
	static bool VisibilityDiffers(const FString& ItemVisibility, bool bWantPublic);

	/**
	 * Whether an existing item should be public after this publish: the item's own visibility when Choice.bKeepExistingVisibility and it is a known one,
	 * otherwise the choice. A new item has no visibility yet (pass empty), so it takes the choice.
	 */
	static bool ResolveExistingItemPublic(const FString& ItemVisibility, const FMantleVehicleLibraryChoice& Choice);

	/** PATCH /library/items/{id} body that sets only the card's picture and leaves the item's visibility alone. */
	static FString MakeThumbnailPatchBody(const FString& ThumbnailAssetId);

	/** True for status "delisted": the owner removed the item, so it takes no versions and no edits. */
	static bool IsDelisted(const FString& ItemStatus);

	/**
	 * True for status "taken_down" (#614 stage 7): a moderator removed the item after a copyright complaint. It takes no
	 * versions and no edits, and unlike a delisted item it is never replaced by a new item for the same content.
	 */
	static bool IsTakenDown(const FString& ItemStatus);

	/**
	 * Whether a refused library request is sent again. A 429 was not processed, so every step repeats it. Other
	 * retryable answers (no answer, 5xx) may have reached Mantle, so only a step that is safe to repeat (a read, an
	 * idempotent create or edit; not a new version) does.
	 */
	static bool ShouldRetry(const FMantleApiResult& Refused, bool bSafeToRepeat);

	/** Reads {"itemId", "version"} (the answer to creating an item or a version). */
	static bool ReadCreated(const FString& Body, FString& OutItemId, int32& OutVersion);

	/** Reads GET /library/items/{id}: true with the number of the version whose primaryAssetId is AssetId, when it has one. */
	static bool FindVersionWithAsset(const FString& ItemBody, const FString& AssetId, int32& OutVersion);

	/** One plain sentence for a library request Mantle refused, naming the vehicle. */
	static FString DescribeFailure(const FMantleApiResult& Refused, const FString& VehicleName);

	// --- #838: script packages ---------------------------------------------------------------------------------------------

	/**
	 * POST /library/items for a script package: kind "script", the manifest's name and description as the item's, its files
	 * ({path, side, language, source}, source the uploaded src_ id) and its manifest. No primary asset, no thumbnail, no provenance.
	 * Public names the rights declaration; private sends visibility "private" and none.
	 */
	static FString MakeScriptItemBody(const FMantleScriptPackageManifest& Manifest, bool bPublic, const TArray<FMantleLibraryScriptFile>& Files);

	/** POST /library/items/{id}/versions for a script package: {scripts, manifest}. */
	static FString MakeScriptVersionBody(const FMantleScriptPackageManifest& Manifest, const TArray<FMantleLibraryScriptFile>& Files);

	/**
	 * The Idempotency-Key for creating a package's item: the same for the same package (MemoryKey), files, manifest, visibility and
	 * Generation (MakeIdempotencyKey's rule), so a retried create names the item the first one made.
	 */
	static FString MakeScriptIdempotencyKey(const FString& MemoryKey, const FMantleScriptPackageManifest& Manifest, const TArray<FMantleLibraryScriptFile>& Files,
		bool bPublic, int32 Generation = 0);

	/**
	 * Reads GET /library/items/{id}: true with the highest version whose files (the same paths, sides, languages and sources, in
	 * order) and manifest (name, description, and every parameter and setting, each field of each) are exactly these, when one is.
	 * A republish with nothing changed then pins it instead of adding a version.
	 */
	static bool FindVersionWithScripts(const FString& ItemBody, const FMantleScriptPackageManifest& Manifest, const TArray<FMantleLibraryScriptFile>& Files,
		int32& OutVersion);

	/**
	 * Puts each package in Packages into Storage under its Name, removing nothing else: an entry already named Name takes its item
	 * and version (and an entry of another name that held the same item goes, as a place stores an item once); otherwise an entry
	 * holding the same item is renamed to Name and re-pinned; otherwise one is added. A re-pinned entry keeps its settings (#838
	 * stage 6) less the keys the package's Manifest no longer declares, each drop logged.
	 */
	static void MergePackagesIntoStorage(TArray<FMantleStorageEntry>& Storage, const TArray<FMantleLibraryPackageRef>& Packages);

	/**
	 * Rewrites the exported place file's storage[] with MergePackagesIntoStorage. With nothing to store OutJson is PlaceJson, byte for
	 * byte. False, with OutError in plain words, for a ref that is not a library item version, more than FMantlePlaceLimits::
	 * MaxStorageEntries entries, or storage FMantleStorage::CheckEntries refuses.
	 */
	static bool RewriteScriptPackages(const FString& PlaceJson, const TArray<FMantleLibraryPackageRef>& Packages, FString& OutJson, FString& OutError);

	/**
	 * A Save that does not go live stores no package (#838), so the draft leaves out every ScriptPoint whose storageItem the file's
	 * storage[] does not declare (it would name nothing): OutJson is the file without them, OutDropped the dropped points' names.
	 * With none to drop OutJson is PlaceJson, byte for byte.
	 */
	static void DropUnstoredScriptPoints(const FString& PlaceJson, FString& OutJson, TArray<FString>& OutDropped);

private:
	/** RewritePlace for any spawn type: SpawnType the entity type ("VehicleSpawn"), Field its asset key ("vehicle"), Fallback the storage name of a nameless item. */
	static bool RewriteSpawns(const TCHAR* SpawnType, const TCHAR* Field, const TCHAR* Fallback, const FString& PlaceJson,
		const TArray<FMantleLibraryVehicleRef>& Refs, FString& OutJson, FString& OutError, TArray<FMantleStoredVehicle>* OutStored);
};
