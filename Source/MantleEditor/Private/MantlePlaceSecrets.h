// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

struct FMantleApiResult;

/**
 * Place secrets (GhzGarage/mantle#1011): named values a creator stores for a place, read by its server scripts on
 * Mantle-hosted servers with Secrets:Get. The backend never returns a value, and neither this code nor the panel
 * ever keeps one: a value lives in the field the creator types in and in the one request that sends it, and
 * nothing here logs it. The API calls live here because Source/MantleRuntime is a shared copy of the game's.
 */
namespace MantlePlaceSecrets
{
	/** The limits GET /places/{id}/secrets reports; these are the defaults until it does. */
	constexpr int32 DefaultMaxSecrets = 50;
	constexpr int32 DefaultMaxValueBytes = 8192;

	/** The file a creator keeps local test values in, beside the place file; copied into a preview bundle, never uploaded. */
	const TCHAR* LocalFileName();

	/** What the API lists for one secret: never a value. */
	struct FEntry
	{
		FString Name;
		FString CreatedAt;
		FString UpdatedAt;
	};

	struct FList
	{
		TArray<FEntry> Entries;
		int32 MaxSecrets = DefaultMaxSecrets;
		int32 MaxValueBytes = DefaultMaxValueBytes;
	};

	/** Trimmed and upper-cased, as the name field shows it while the creator types. */
	FString NormalizeName(const FString& Typed);

	/** ^[A-Z][A-Z0-9_]{0,63}$ . OutWhy is plain text for the creator. */
	bool CheckName(const FString& Name, FString& OutWhy);

	/** 1 to MaxBytes UTF-8 bytes (not characters), no control characters except tab, line feed and carriage return. */
	bool CheckValue(const FString& Value, int32 MaxBytes, FString& OutWhy);

	/** A new name past MaxSecrets is refused; replacing one is not. */
	bool CheckCount(int32 CurrentCount, int32 MaxSecrets, bool bNameExists, FString& OutWhy);

	/** The "n / max" caption. */
	FString CountText(int32 Count, int32 Max);

	/** Reads a 200 list body. False for anything that is not one. */
	bool ParseList(const FString& Body, FList& OutList);

	/** The body of a PUT: {"value": "..."} and nothing else. */
	FString MakeSetBody(const FString& Value);

	/** The paths, with the place id and name URL-encoded. */
	FString ListPath(const FString& PlaceId);
	FString ItemPath(const FString& PlaceId, const FString& Name);

	/** The store is off on this backend (503 secrets_unavailable): the panel disables itself. */
	bool IsStoreOff(const FMantleApiResult& Result);

	/** What to show for a failed call: the backend's own message when it sent one, else a plain line per status. */
	FString DescribeFailure(const FMantleApiResult& Result);

	/** Shown when the level has no place on the backend yet. */
	FString NoPlaceMessage();

	/** Shown in place of the panel when the store is off. */
	FString StoreOffMessage();

	/** Where a level's local test file lives: <Project>/MantleSecrets/<LevelName>/secrets.local.json. Empty for no level. */
	FString LocalFilePathFor(const FString& LevelName);

	/** Creates the file as {} when missing and makes sure the project's .gitignore lists it. False with OutWhy on failure. */
	bool EnsureLocalFile(const FString& LevelName, const FString& ProjectDir, FString& OutPath, FString& OutWhy);

	/** True when the .gitignore text already ignores the file; adds the line otherwise. Pure, for tests. */
	bool AddToGitIgnore(FString& InOutText);

	/** Copies SourceFile (if it exists) into BundleDir; false only when it exists and could not be copied. */
	bool CopyToBundle(const FString& SourceFile, const FString& BundleDir, FString& OutWhy);

	/** Deletes the copy in BundleDir. Safe when there is none. */
	void RemoveFromBundle(const FString& BundleDir);

	/** The place's list/set/delete calls, async; OnDone runs on the game thread. */
	void List(const FString& PlaceId, TFunction<void(const FMantleApiResult&)> OnDone);
	void Set(const FString& PlaceId, const FString& Name, const FString& Value, TFunction<void(const FMantleApiResult&)> OnDone);
	void Delete(const FString& PlaceId, const FString& Name, TFunction<void(const FMantleApiResult&)> OnDone);
}
