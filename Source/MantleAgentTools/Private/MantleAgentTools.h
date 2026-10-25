// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class FMantleCosmeticUploadSession;
class FMantlePlacePublishSession;
class UToolCallAsyncResultString;
class UWorld;
struct FMantleCosmeticPackage;
struct FMantlePublishResult;

/**
 * What UMantleToolset's tools return, on a given world or session. Each is a thin read of an existing MantleEditor result as one JSON object; none
 * of them reads or writes the creator's token.
 */
struct FMantleAgentTools
{
	/**
	 * PublishPlace on World: a refusal (CheckPublishAllowed) that completes at once, or the publish
	 * it started, whose result completes with DescribePublishResult when that publish ends.
	 */
	static UToolCallAsyncResultString* PublishPlace(UWorld* World, const FString& Notes);

	/**
	 * UploadCosmetic: a refusal that completes at once (CheckUploadCosmeticAllowed, then
	 * BuildCosmeticPackage's, then check_failed with the check's issues when it finds errors), or
	 * the upload it started, whose result completes with DescribeCosmeticUpload when that ends.
	 */
	static UToolCallAsyncResultString* UploadCosmetic(const FString& AssetPath, const FString& Slot, const FString& Name, const FString& Sections, const FString& ItemId,
		const FString& FitSex);

	/** FMantlePlaceValidator::Validate, as CheckPlace describes it. */
	static FString CheckPlace(UWorld* World);

	/** FMantlePlaceExporter::Export with offline options, summarised as ExportPreview describes it. */
	static FString ExportPreview(UWorld* World);

	/**
	 * Why PublishPlace would not start on World, as a refusal object; empty when it may start.
	 * Signed in, a level, the API configured, and no publish already running (the panel's, or one
	 * PublishPlace started).
	 */
	static FString CheckPublishAllowed(UWorld* World);

	/** A finished publish, as PublishPlace describes it. */
	static FString DescribePublishResult(const FMantlePublishResult& Result);

	/** ListMyPlaces: refused when signed out or unconfigured, otherwise GET /places/mine (blocking). */
	static FString ListMyPlaces();

	/** UploadStatus on Session (null: none has run). */
	static FString UploadStatus(const FMantlePlacePublishSession* Session, const FString& AssetName);

	/** ExplainIssue: World's validator issues whose message, actor or remedy contains Query. */
	static FString ExplainIssue(UWorld* World, const FString& Query);

	/** ScriptApiRequirement: Export's recorded scriptApi and the validator's per-script scan. */
	static FString ScriptApiRequirement(UWorld* World);

	/** PreviewPlace: FMantlePlacePreview::Build, described and destroyed, as the menu does. */
	static FString PreviewPlace(UWorld* World);

	/**
	 * The Cosmetics panel's package for a skeletal mesh (FMantleCosmeticExport::Build on
	 * FMantleEditorActions::MakeCosmeticDraft). False with OutRefusal when there is none to build:
	 * no contract, no such asset, not a skeletal mesh, a slot no skeletal-mesh item takes, or a FitSex
	 * other than "", auto, female or male. OutObjectPath is the mesh's full object path.
	 */
	static bool BuildCosmeticPackage(const FString& AssetPath, const FString& Slot, const FString& Name, const FString& Sections,
		const FString& ItemId, const FString& FitSex, FMantleCosmeticPackage& OutPackage, FString& OutObjectPath, FString& OutRefusal);

	/** MeasureHead, as the toolset describes it. */
	static FString MeasureHead(const FString& BodyPath, const FString& FacePath);

	/** CheckCosmetic: BuildCosmeticPackage, as CheckCosmetic describes it. */
	static FString CheckCosmetic(const FString& AssetPath, const FString& Slot, const FString& Name, const FString& Sections, const FString& FitSex);

	/** Why UploadCosmetic would not start, as a refusal object; empty when it may: signed in, configured, none of its own running. */
	static FString CheckUploadCosmeticAllowed();

	/** A finished cosmetic upload, as UploadCosmetic describes it. */
	static FString DescribeCosmeticUpload(const FMantleCosmeticUploadSession& Session);

	/** The refusal object every tool uses: {"ok": false, "refused": Code, "message": Message}. */
	static FString Refusal(const FString& Code, const FString& Message);

	/** The level the tools work on: the editor world, or null. */
	static UWorld* EditorWorld();

	/** Removes the tickers still waiting for a publish or upload to end (module shutdown). */
	static void StopWaiting();
};
