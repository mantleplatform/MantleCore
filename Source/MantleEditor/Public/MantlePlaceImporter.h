// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleApiClient.h"
#include "MantlePlaceBuilder.h"

class UWorld;

/**
 * Brings a place file into a level the creator can edit.
 *
 * The same builder the client uses, with editor semantics: actors are persistent so they
 * survive a save, and the whole import is one undo step.
 *
 * This is also how "New place" works. The starting template is a place file rather than a
 * `.umap`, so a new place travels the same path as every other place — one import
 * mechanism instead of two, and the template stays readable and diffable.
 */
class MANTLEEDITOR_API FMantlePlaceImporter
{
public:
	/** Imports a place file into World. */
	static FMantleBuildResult ImportFromFile(UWorld* World, const FString& FilePath);

	/** Imports an already-parsed place into World. Resolver is injectable for offline tests.
     * Restores authored native tags/mobility for lossless export. A non-dynamic descendant
     * beneath a dynamic ancestor cannot be represented by native editor mobility and is
     * rejected before source fetching or world mutation. Runtime reconstruction is unchanged. */
	static FMantleBuildResult Import(
		UWorld* World,
		const FMantlePlace& Place,
		const TFunction<FMantleApiResult(const FString&)>& ScriptSourceResolver = {});

	/** Path to the built-in new-place template. */
	static FString TemplatePath();
};
