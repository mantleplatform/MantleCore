// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"

class IMantleAssetResolver;
class FMantleAssetStore;
struct FMantlePreparedPlaceAssetsState;

/**
 * FMantlePlaceAssets::Prepare for a tool that waits for it: the editor's "Check what players
 * receive" (#394), which builds a place the way the client does, uploaded (ast_u) meshes and
 * materials included.
 *
 * Its own name and header on purpose: the editor module has a class of its own called
 * FMantlePlaceAssets (the upload rules), in a header of the same name that shadows this module's
 * in every editor translation unit, so editor code cannot name the loader directly. Nothing here
 * mentions it.
 *
 * Keep the value alive while the place built from it exists: FMantleAssetLibrary answers the
 * prepared ids only while the set lives. Game thread.
 */
struct MANTLERUNTIME_API FMantlePreparedPlaceAssets
{
	/** The place's entities use an uploaded mesh or material, so Prepare ran. */
	bool bNeeded = false;

	/** Everything finished (built or failed) before the time ran out. */
	bool bComplete = false;

	/** One per asset that could not be resolved, downloaded or built, and one for a timeout. */
	TArray<FMantleParseError> Errors;

	/** ast_u and 32 lowercase hex digits (FMantlePlaceAssets::IsUploadedId). */
	static bool IsUploadedId(const FString& Id);

	/**
	 * Prepares Place's uploaded assets and pumps until they are done or TimeoutSeconds pass. The
	 * process's resolver and store unless Resolver or Store is given (tests).
	 */
	static FMantlePreparedPlaceAssets PrepareAndWait(const FMantlePlace& Place, float TimeoutSeconds,
		TSharedPtr<IMantleAssetResolver> Resolver = nullptr, TSharedPtr<FMantleAssetStore> Store = nullptr);

	/** Lets the prepared set go; the library stops answering its ids once nothing else holds them. */
	void Release();

private:
	TSharedPtr<FMantlePreparedPlaceAssetsState> State;
};
