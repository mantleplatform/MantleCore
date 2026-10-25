// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"

/**
 * Parses and validates Mantle place files.
 *
 * Validation is strict by design: unknown entity types, unknown properties, missing
 * required fields and out-of-range values are all rejected rather than ignored. That
 * strictness is what makes a published place a lossless record of its own content.
 *
 * All errors are collected, not just the first, so creators see everything wrong at once.
 */
class MANTLERUNTIME_API FMantlePlaceParser
{
public:
	/** Parse from a JSON string. */
	static FMantleParseResult ParseFromString(const FString& Json);

	/**
	 * #838: ParseFromString, except that a ScriptPoint whose storageItem names none of the place's storage entries
	 * is accepted when it names one of DiskScriptPackages exactly (case-sensitive): the Packages/<Name>/ folders
	 * beside a local -MantlePlace file (FMantlePlaceLoader::LoadFromFile), which the disk script catalogue mounts.
	 * Every other rule is unchanged; a published place is never parsed this way.
	 */
	static FMantleParseResult ParseFromString(const FString& Json, const TArray<FString>& DiskScriptPackages);

	/**
	 * #679: the same, and with bAcceptDraftVehicleIds a VehicleSpawn may name a raw uploaded vehicle ("vehicle") instead of a
	 * storageItem. Only an offline preview's bundle (Test Locally) is parsed this way: it holds the level's converted vehicle,
	 * where a published place only parks stored ones.
	 */
	static FMantleParseResult ParseFromString(const FString& Json, const TArray<FString>& DiskScriptPackages, bool bAcceptDraftVehicleIds);

	/** Parse from a file on disk. */
	static FMantleParseResult ParseFromFile(const FString& FilePath);

	/** ParseFromFile with ParseFromString's DiskScriptPackages exception and bAcceptDraftVehicleIds. */
	static FMantleParseResult ParseFromFile(const FString& FilePath, const TArray<FString>& DiskScriptPackages, bool bAcceptDraftVehicleIds);

	/**
	 * A string's length as JSON Schema's maxLength, and the backend, count it: in code points.
	 * FString::Len() counts UTF-16 units, so an emoji would count twice. Shared so the editor's
	 * validator measures names and tags exactly as the parser does.
	 */
	static int32 CodePointLength(const FString& Value);
};
