// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleApiClient.h"
#include "MantlePlaceTypes.h"

class FJsonObject;

/** The decoded body of a storage resolve answer. FMantleResolvedStorageEntry is in MantlePlaceTypes.h. */
struct FMantleStorageExpansion
{
	TArray<FMantleResolvedStorageEntry> Entries;

	/** Item versions the backend does not know, or the place's owner may not use. */
	TArray<FMantleStorageRef> Missing;

	/** The entry for an item version, or null. Item is compared case-sensitively. */
	const FMantleResolvedStorageEntry* Find(const FString& Item, int32 Version) const;

	/** True when the item version was reported missing. */
	bool IsMissing(const FString& Item, int32 Version) const;
};

/** The outcome of one storage resolve call. */
struct FMantleStorageResolveResult
{
	bool bSuccess = false;

	/** Written for a log: what went wrong when bSuccess is false. */
	FString Error;

	FMantleStorageExpansion Expansion;
};

/** What to do with one storage resolve answer (FMantleStorage::Classify). */
enum class EMantleStorageAnswer : uint8
{
	/** A readable 2xx answer: apply it. */
	Apply,
	/** A transient failure (the request never reached the API, or a 5xx): try again, up to FMantleStorage::MaxRetries times. */
	Retry,
	/** A failure that trying again will not fix (a 4xx, or a 2xx body that cannot be read): fail the load. */
	Fail,
};

/** The outcome of FMantleStorage::ExpandAsync. */
struct FMantleStorageExpandResult
{
	/** False when the storage could not be resolved: the load must fail, not continue without it. */
	bool bSuccess = false;

	/** Written for a log and a player: why, when bSuccess is false. */
	FString Error;

	/** The place with its stored spawns rewritten. Meaningful only when bSuccess is true. */
	FMantlePlace Place;
};

/**
 * One stored script package a machine runs (#838): the place's storage name for it (what Script.Package reads and
 * the catalogue mounts it under, Packages/<StorageName>/), the files for this machine's sides plus Shared, and
 * its manifest.
 */
struct FMantleStoredScriptPackage
{
	FString StorageName;
	TArray<FMantleStoredScriptFile> Files;
	FMantleScriptPackageManifest Manifest;
	/** Stage 6: the package's stored settings, raw text (Script.Settings types and defaults them by Manifest). */
	TMap<FString, FString> Settings;
};

/** One problem with a place's storage entries, located by a JSON-pointer-style path. */
struct FMantleStorageProblem
{
	FString Path;
	FString Message;
};

/**
 * Place storage, the ServerStorage analogue (#614 stage 4): the library items a place pins, each by name, and what
 * it takes to load them. The place file carries only {name, item, version}; at load the entries are expanded through
 * the storage resolve routes and every VehicleSpawn that names a stored item is rewritten to the item's primary
 * asset id (Apply), so asset preparation and the gameplay bridge see an ordinary uploaded vehicle.
 */
class MANTLERUNTIME_API FMantleStorage
{
public:
	/**
	 * The schema's rules for a storage array, shared by the parser, the exporter and the editor validator: at most
	 * FMantlePlaceLimits::MaxStorageEntries entries, names matching FMantlePlaceLimits::StorageNamePattern and
	 * unique (case-sensitively), items itm_u plus 32 lowercase hex digits and each stored once (a place pins one
	 * version of an item: a repeat is reported at its own "/storage/<index>/item"), versions of at least 1, and (#838 stage 6)
	 * settings of at most FMantlePlaceLimits::MaxStorageSettings keys, each a setting name ("/storage/<index>/settings/<key>")
	 * with a value of at most MaxStorageSettingValueLength characters. Paths are "/storage" or "/storage/<index>/<field>".
	 */
	static TArray<FMantleStorageProblem> CheckEntries(const TArray<FMantleStorageEntry>& Entries);

	/** The distinct item versions Entries pin, in first-seen order: what a resolve request asks about. */
	static TArray<FMantleStorageRef> RefsOf(const TArray<FMantleStorageEntry>& Entries);

	/**
	 * Reads {"entries": [...], "missing": [{"item", "version"}]}. False, with Out left empty, when the text is not
	 * that shape.
	 */
	static bool ParseResponse(const FString& Json, FMantleStorageExpansion& Out);

	/** Retries after the first attempt for a transient failure (so a resolve is sent at most MaxRetries + 1 times). */
	static constexpr int32 MaxRetries = 2;

	/** Seconds to wait before retry number Retry (1-based): 1 s, then 2 s. */
	static float RetryDelaySeconds(int32 Retry);

	/**
	 * Decides what a storage resolve answer means. StatusCode is the HTTP status (0 when the request never reached
	 * the API; TransportMessage then says why), Body the text of the answer. A 2xx with a readable body is Apply and
	 * fills Out; a transport failure or a 5xx is Retry; everything else (a 4xx, a 2xx whose body cannot be read, a
	 * status outside 2xx, 4xx and 5xx) is Fail. OutError is set for Retry and Fail. Pure: no I/O, no logging.
	 */
	static EMantleStorageAnswer Classify(int32 StatusCode, const FString& Body, const FString& TransportMessage,
		FMantleStorageExpansion& Out, FString& OutError);

	/**
	 * Resolves Place.Storage and rewrites the place to match, then calls OnDone (on the game thread, from the HTTP
	 * completion or a ticker) with the result. Returns at once; never blocks. With no storage it calls OnDone before
	 * returning. bServer picks POST /server/storage/resolve with the server token over POST
	 * /places/{PlaceId}/storage/resolve with the player's. A transport failure or 5xx is retried up to MaxRetries
	 * times after a short delay (a game-thread ticker); one that still fails, a 4xx, or an unreadable answer reports
	 * bSuccess false, so the load fails visibly: a client must never go on without assets the server prepared.
	 * Entries the API lists as missing (or of the wrong kind) are a successful answer; Apply turns those spawns off.
	 */
	static void ExpandAsync(const FString& PlaceId, bool bServer, FMantlePlace Place, TFunction<void(FMantleStorageExpandResult)> OnDone);

	/**
	 * Rewrites every VehicleSpawn that names a StorageItem (and likewise every WeaponSpawn; a ScriptPoint (#838) keeps
	 * its properties and is only turned off when its entry is undeclared, missing, taken down, not a script, or not
	 * active or delisted). A stored vehicle that resolved becomes the spawn's
	 * Vehicle (the one place that happens: Vehicle holds the resolved id from then on, StorageItem stays as the
	 * place file had it). A spawn whose entry is missing, is not a vehicle, is not active or delisted (a taken-down item is logged as such), or has no
	 * usable primary asset id keeps no Vehicle, is turned off, and logs a warning naming it. Spawns without a
	 * StorageItem are untouched. Returns how many spawns were turned off.
	 *
	 * Also keeps what resolved (#614 stage 5): Place.StorageResolved becomes one entry per declared item
	 * that the answer lists as an active or delisted vehicle or weapon with a usable primary asset id, or as an
	 * active or delisted script package (#838, which has no primary asset), in
	 * declaration order. Missing, unanswered and wrong-status versions are left out, which is what scripts are
	 * authorized against (FindAuthorized), whether or not a VehicleSpawn names them.
	 */
	static int32 Apply(FMantlePlace& Place, const FMantleStorageExpansion& Expansion);

	/** The Kind of a resolved vehicle entry. */
	static constexpr const TCHAR* KindVehicle = TEXT("vehicle");

	/** The Kind of a resolved weapon entry. */
	static constexpr const TCHAR* KindWeapon = TEXT("weapon");

	/** The Kind of a resolved script package entry (#838). It has no primary asset. */
	static constexpr const TCHAR* KindScript = TEXT("script");

	/**
	 * Reads a script package manifest (#838): {name, description, parameters: [option], settings: [option]} (settings: stage 6),
	 * each option {name, type, default, help, required, min, max, options}, the resolve answer's manifest and a package folder's
	 * package.json alike. Lenient: a missing or wrong-typed field is left empty, and an option whose name is not a parameter key,
	 * whose type is not "string", "number", "boolean" or "choice", or that repeats an earlier name of its list ignoring case, is
	 * left out. At most FMantlePlaceLimits::MaxScriptPointParameters parameters and MaxStorageSettings settings are kept. A
	 * default is kept only when it reads as the declared type (a number or boolean as JSON or as the backend's text); a number's
	 * also only within min and max, a choice's only when it is one of the options, and a required string's "" is no default.
	 * min and max are read for a number only (finite JSON numbers; a min above max drops both); options for a choice only (text
	 * entries of 1 to 64 characters, repeats left out, at most 32), and a choice left with fewer than 2 is left out. Pure.
	 */
	static FMantleScriptPackageManifest ReadScriptPackageManifest(const TSharedPtr<FJsonObject>& Object);

	/** ReadScriptPackageManifest over JSON text; an empty manifest when the text is not a JSON object. */
	static FMantleScriptPackageManifest ReadScriptPackageManifestJson(const FString& Json);

	/**
	 * The stored script packages a machine runs (#838), in storage declaration order: one per declared entry whose
	 * pinned version resolved as a script (Place.StorageResolved), with its files of the server side when
	 * bServerSide, the client side when bClientSide, and Shared always. A package with no files for this machine
	 * is still listed (its manifest describes its points). Pure.
	 */
	static TArray<FMantleStoredScriptPackage> StoredScriptPackages(const FMantlePlace& Place, bool bServerSide, bool bClientSide);

	/** The storage entry whose Name is exactly Name (case-sensitive: 'Truck' and 'truck' are two names), or null. */
	static const FMantleStorageEntry* FindByName(const FMantlePlace& Place, const FString& Name);

	/**
	 * The resolved entry that authorizes a script to use ItemId as a Kind (KindVehicle or KindWeapon), or null.
	 * Non-null only when ItemId is itm_u plus 32 lowercase hex digits (anything else, a raw asset id, a built-in
	 * name or an object path, is refused before any lookup), is the Item of a storage entry the place declares,
	 * that entry's pinned version resolved (Place.StorageResolved, see Apply), and its kind is Kind. Pure.
	 */
	static const FMantleResolvedStorageEntry* FindAuthorized(const FMantlePlace& Place, const FString& ItemId, const FString& Kind);

	/**
	 * Why FindAuthorized refuses ItemId as a Kind, worded for a script error, or empty when it would not.
	 * Pure: the script API reports exactly this, so a test of it is a test of what a creator reads.
	 */
	static FString WhyNotAuthorized(const FMantlePlace& Place, const FString& ItemId, const FString& Kind);

	/**
	 * The primary asset ids of the place's resolved vehicles (Place.StorageResolved), distinct, in order. These
	 * are what asset preparation takes for stored vehicles no VehicleSpawn uses, so a script's spawn finds the
	 * vehicle built on the server and the client has its definition. Pure.
	 */
	static TArray<FString> StoredVehicleAssetIds(const FMantlePlace& Place);

	/**
	 * The primary asset ids of the place's resolved weapons (Place.StorageResolved), distinct, in order. These are
	 * prepared even when no WeaponSpawn uses them, so Backpack:Grant can build the same registered definition on every machine.
	 */
	static TArray<FString> StoredWeaponAssetIds(const FMantlePlace& Place);
};
