// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleThumbnailChoice.h"

/**
 * Where a creator's thumbnail choice (#124) is kept for a source we do not own (a mesh, an animation, a groom, a texture) and for the plugin's own sources whose class
 * is shared with the game (a vehicle, a weapon): a small ini in the PROJECT's Config folder, one section per source and class. It travels with the project and
 * with source control, and never alters the creator's asset. (A place keeps its choice on its own settings actor.) The first version kept it under
 * Saved/Config, which is per machine; Load moves any entry it finds there.
 */
struct FMantleThumbnailStore
{
	/** <Project>/Config/MantleThumbnailChoices.ini: goes with the project. */
	static FString DefaultConfigFile();

	/** <Project>/Saved/Config/MantleThumbnailChoices.ini: where the first version kept choices; read once and moved. */
	static FString LegacyConfigFile();

	/** The section name for a source object path and class name ("Garment", "Emote"...). */
	static FString SectionFor(const FString& SourcePath, const FString& ClassName);

	/**
	 * Fills OutChoice from ConfigFile and returns true when this source and class have a saved choice; OutChoice is left Automatic otherwise. Without one in
	 * ConfigFile, LegacyFile (when given; the default file takes the legacy one) is tried, and an entry found there is saved into ConfigFile and removed from it.
	 */
	static bool Load(const FString& SourcePath, const FString& ClassName, FMantleThumbnailChoice& OutChoice, const FString& ConfigFile,
		const FString& LegacyFile = FString());

	/** Saves Choice (an Automatic choice removes the entry). */
	static void Save(const FString& SourcePath, const FString& ClassName, const FMantleThumbnailChoice& Choice, const FString& ConfigFile);
};
