// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceBuilder.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceValidator.h"
#include "MantlePreparedPlaceAssets.h"

struct FMantlePlaceAssetSet;
struct FMantleUploadedAsset;
class UWorld;

struct FMantlePreviewResult
{
	bool bSuccess = false;

	/** The reconstructed world. Owned by the caller — pass it back to Destroy(). */
	UWorld* World = nullptr;

	FMantleValidationResult Validation;
	FMantleBuildResult Build;

	/** Actors in the creator's level that Mantle can publish. */
	int32 SourceEntityCount = 0;

	/** Actors that actually appear for players. */
	int32 PreviewActorCount = 0;

	/**
	 * Of SourceEntityCount, the objects left out of the preview because a mesh or material they
	 * use from the creator's project is not on Mantle yet (Publish uploads it): not a failure.
	 */
	int32 NotUploadedCount = 0;

	/**
	 * Everything in the creator's level that will not reach players, one line each
	 * ("'Label' is a Landscape..."), from ListContentLeftOut.
	 */
	TArray<FString> LeftOut;

	/** The JSON players would receive, kept for inspection. */
	FString PlaceJson;

	/** The place's uploaded meshes and materials, as players download them; alive until Destroy (#394). */
	FMantlePreparedPlaceAssets PreparedAssets;
};

/** How Build finds the level's uploaded (ast_u) meshes and materials. */
struct FMantlePreviewOptions
{
	/**
	 * The level's converted assets with the ids Mantle already holds them under
	 * (FindUploadedAssets). Null: every mesh or material from the creator's project counts as not
	 * uploaded yet, and nothing is asked of Mantle.
	 */
	const FMantlePlaceAssetSet* Assets = nullptr;

	/** How long Build waits for the uploaded assets to download and build. */
	float AssetTimeoutSeconds = 60.0f;
};

/** POST /assets/diff for the preview: Hashes in, what Mantle holds out. False with OutError when it could not ask. */
using FMantlePreviewDiff = TFunction<bool(const TArray<FString>& Hashes, TArray<FMantleUploadedAsset>& OutKnown, FString& OutError)>;

/**
 * Shows the creator what players will actually receive.
 *
 * Stock Play-In-Editor plays the *editor's* level. Players get a place reconstructed from
 * a data file. Those two drift, and without a preview that goes through the real path,
 * every divergence becomes a bug discovered after publishing.
 *
 * So the preview deliberately takes the long way round: export the level, serialise it to
 * JSON, parse that JSON back, prepare its uploaded meshes and materials the way a client does
 * (FMantlePlaceAssets::Prepare, through FMantlePreparedPlaceAssets), and build from the result.
 * Anything lost in serialisation is lost in the preview too, which is the entire point.
 */
class MANTLEEDITOR_API FMantlePlacePreview
{
public:
	/**
	 * Builds a preview world from a source level.
	 *
	 * Proceeds even when the level has unpublishable content — showing the creator a world
	 * with their unsupported actors missing is far more informative than refusing to
	 * preview at all. Those problems come back in Validation.
	 */
	static FMantlePreviewResult Build(UWorld* SourceWorld, const FMantlePreviewOptions& Options);

	static void Destroy(FMantlePreviewResult& Result);

	/**
	 * Converts the level's own meshes and materials (the local conversion cache makes a repeat
	 * cheap) and records, in OutSet, the ones Mantle already holds approved (RecordKnownUploads):
	 * what Build needs to load them as players will. False with OutProblem when Mantle could not
	 * be asked; OutSet then has what was converted, and no ids.
	 */
	static bool FindUploadedAssets(UWorld* SourceWorld, FMantlePlaceAssetSet& OutSet, const FMantlePreviewDiff& Diff, FString& OutProblem);

	/**
	 * Asks Diff which of Set's converted textures, then materials (whose bytes name their
	 * textures' ids), then meshes Mantle holds, and records each approved one in Set.Uploaded.
	 * A material whose textures are not all on Mantle is not asked about.
	 */
	static bool RecordKnownUploads(FMantlePlaceAssetSet& Set, const FMantlePreviewDiff& Diff, FString& OutProblem);

	/** The API's POST /assets/diff, blocking, as FindUploadedAssets's Diff for the editor. */
	static FMantlePreviewDiff ApiDiff();

	/**
	 * The actors in a level that will not reach players: unsupported types, content Mantle
	 * cannot publish yet (FMantlePlaceExporter::DescribeUnpublishedContent) and the sky actors
	 * Mantle supplies itself. The same classification the validator and exporter use, listed
	 * by outliner label so the creator can find each one.
	 */
	static TArray<FString> ListContentLeftOut(UWorld* SourceWorld);

	/**
	 * The creator-facing answer to "what will players receive": how many objects will reach
	 * them, and what will not, at most MaxListed lines of it. Validation issues are not
	 * repeated here; the caller shows those alongside.
	 */
	static FString DescribeForCreator(const FMantlePreviewResult& Result, int32 MaxListed = 12);
};
