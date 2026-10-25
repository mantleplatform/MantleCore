// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAgentTools.h"
#include "MantleToolset.h"

#include "MantleApiManifest.h"
#include "MantleApiClient.h"
#include "MantleAssetUploadQueue.h"
#include "MantleAvatarContract.h"
#include "MantleAvatarConverter.h"
#include "MantleCosmeticExport.h"
#include "MantleCosmeticPackage.h"
#include "MantleCosmeticUpload.h"
#include "MantleCosmeticValidation.h"
#include "MantleEditorActions.h"
#include "MantleGarmentOutfit.h"
#include "MantleHeadFit.h"
#include "MantleParametricOutfit.h"
// The editor's FMantlePlaceAssets arrives through MantlePlacePublishSession.h. Never include
// "MantlePlaceAssets.h" here: MantleRuntime has a header of the same name defining a class of the
// same name, and this module sees both include paths.
#include "MantlePlaceExporter.h"
#include "MantlePlacePreview.h"
#include "MantlePlacePublishSession.h"
#include "MantlePlaceTypes.h"
#include "MantlePlaceValidator.h"
#include "MantlePublisher.h"
#include "MantleRuntime.h"
#include "MantleScriptApiUsage.h"

#include "Animation/AnimSequence.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ToolsetRegistry/ToolCallAsyncResultString.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace MantleAgentToolsPrivate
{
	/** The publish PublishPlace started last (the panel owns it). */
	TWeakPtr<FMantlePlacePublishSession> AgentPublish;

	/** The cosmetic upload UploadCosmetic started last (the waiting ticker drives it, as the panel's active timer does). */
	TWeakPtr<FMantleCosmeticUploadSession> AgentCosmeticUpload;

	/**
	 * One ticker per PublishPlace or UploadCosmetic call still waiting for its work to end. Each
	 * call keeps its own until it completes: a new call must never drop an earlier one's ticker,
	 * or that call's result would never complete (the work can end between two of its looks).
	 */
	TArray<FTSTicker::FDelegateHandle> AgentWaitHandles;

	/** Set while PreviewPlace builds, which can pump the network (the uploaded-asset diff). */
	bool bAgentPreviewRunning = false;

	/** Every Interval seconds, Poll: the result's JSON once the work has ended, then Result completes with it. */
	void WaitForAgentResult(UToolCallAsyncResultString* Result, TFunction<TOptional<FString>()> Poll, const float Interval)
	{
		AgentWaitHandles.RemoveAll([](const FTSTicker::FDelegateHandle& Handle) { return !Handle.IsValid(); });
		TStrongObjectPtr<UToolCallAsyncResultString> Pending(Result);
		AgentWaitHandles.Add(FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([Pending, PollWork = MoveTemp(Poll)](float) -> bool
			{
				const TOptional<FString> Done = PollWork();
				if (!Done.IsSet())
				{
					return true;
				}
				Pending->SetValue(Done.GetValue());
				return false;
			}),
			Interval));
	}

	/** The Mantle Publish panel's publish of World (the menu's Publish). */
	TSharedPtr<FMantlePlacePublishSession> StartAgentPublish(UWorld* World, const FString& Notes)
	{
		const TSharedPtr<FMantlePlacePublishSession> Before = FMantleEditorActions::GetPublishSession();
		FMantleEditorActions::StartPublish(World, /*bGoLive*/ true, Notes);
		const TSharedPtr<FMantlePlacePublishSession> Started = FMantleEditorActions::GetPublishSession();
		const TSharedPtr<FMantlePlacePublishSession> Session = Started != Before ? Started : TSharedPtr<FMantlePlacePublishSession>();
		if (Session.IsValid())
		{
			// An agent cannot make the rights declaration a public library item needs for the creator (as UploadCosmetic
			// forces private), so new vehicles it publishes are private. Without this the Library step would wait forever.
			// An item the creator already made public keeps that: the agent must not undo their choice.
			Session->VehicleChoice.bPublic = false;
			Session->VehicleChoice.bKeepExistingVisibility = true;
			UE_LOG(LogMantle, Log, TEXT("Mantle toolset: PublishPlace publishes new vehicles as private library items (an agent cannot make the rights declaration) and leaves existing items' visibility alone."));
		}
		return Session;
	}

	FString WriteAgentJson(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	/**
	 * The end of BuildCosmeticPackage for every kind: ItemId checked, then the panel's own preparation (the avatar's
	 * publication mode for the route its body was converted on, its converted face, the fit-size refusal) and its build.
	 * A refused preparation is a package holding only those errors, as the panel shows them.
	 */
	bool FinishCosmeticPackage(FMantleCosmeticDraft& Draft, const FString& ItemId, const FMantleAvatarContract& Contract,
		FMantleCosmeticPackage& OutPackage, FString& OutObjectPath, FString& OutRefusal)
	{
		Draft.PublishIntoItemId = ItemId.TrimStartAndEnd();
		if (!Draft.PublishIntoItemId.IsEmpty() && !Draft.PublishIntoItemId.StartsWith(TEXT("itm_")))
		{
			OutRefusal = FMantleAgentTools::Refusal(TEXT("bad_item_id"), FString::Printf(
				TEXT("\"%s\" is not an item id. Pass the id of the item to update (itm_...), or \"\" to create a new item."), *ItemId));
			return false;
		}
		OutObjectPath = Draft.Source.IsValid() ? Draft.Source->GetPathName() : FString();
		TArray<FMantleCosmeticIssue> Blockers;
		if (!FMantleEditorActions::PrepareCosmeticBuild(Draft, FMantleEditorActions::CosmeticRouteOf(Draft.Source.Get()), Blockers))
		{
			OutPackage = FMantleCosmeticPackage();
			OutPackage.Class = Draft.Class;
			OutPackage.Slot = Draft.Slot;
			OutPackage.Name = Draft.Name.TrimStartAndEnd();
			OutPackage.Issues = MoveTemp(Blockers);
			return true;
		}
		OutPackage = FMantleCosmeticExport::Build(Draft, Contract);
		return true;
	}

	TSharedRef<FJsonObject> AgentIssueJson(const FMantleValidationIssue& Issue)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("severity"), Issue.Severity == EMantleIssueSeverity::Error ? TEXT("error") : TEXT("warning"));
		Object->SetStringField(TEXT("actor"), Issue.Actor);
		Object->SetStringField(TEXT("message"), Issue.Problem);
		Object->SetStringField(TEXT("remedy"), Issue.Remedy);
		return Object;
	}

	void AddAgentIssues(const TArray<FMantleValidationIssue>& Issues, TArray<TSharedPtr<FJsonValue>>& OutValues)
	{
		for (const FMantleValidationIssue& Issue : Issues)
		{
			OutValues.Add(MakeShared<FJsonValueObject>(AgentIssueJson(Issue)));
		}
	}

	/** The place schema's asset kind names (MantlePlaceTypes.cpp keeps its own table private). */
	const TCHAR* AgentAssetKindName(const EMantleAssetKind Kind)
	{
		switch (Kind)
		{
		case EMantleAssetKind::StaticMesh: return TEXT("StaticMesh");
		case EMantleAssetKind::Material:   return TEXT("Material");
		case EMantleAssetKind::Texture:    return TEXT("Texture");
		case EMantleAssetKind::Sound:      return TEXT("Sound");
		case EMantleAssetKind::SkeletalMesh: return TEXT("SkeletalMesh");
		case EMantleAssetKind::AnimClip:   return TEXT("AnimClip");
		case EMantleAssetKind::Heightfield: return TEXT("Heightfield");
		case EMantleAssetKind::Vehicle:    return TEXT("Vehicle");
		case EMantleAssetKind::Weapon:     return TEXT("Weapon");
		}
		return TEXT("Unknown");
	}

	const TCHAR* AgentUploadStateName(const EMantleQueuedUploadState State)
	{
		switch (State)
		{
		case EMantleQueuedUploadState::Queued:     return TEXT("queued");
		case EMantleQueuedUploadState::Uploading:  return TEXT("uploading");
		case EMantleQueuedUploadState::Processing: return TEXT("processing");
		case EMantleQueuedUploadState::Approved:   return TEXT("approved");
		case EMantleQueuedUploadState::Refused:    return TEXT("refused");
		}
		return TEXT("unknown");
	}

	const TCHAR* AgentPublishPhaseName(const EMantlePublishPhase Phase)
	{
		switch (Phase)
		{
		case EMantlePublishPhase::Idle:          return TEXT("idle");
		case EMantlePublishPhase::Checking:      return TEXT("checking");
		case EMantlePublishPhase::Converting:    return TEXT("converting");
		case EMantlePublishPhase::Uploading:     return TEXT("uploading");
		case EMantlePublishPhase::Crediting:     return TEXT("crediting");
		case EMantlePublishPhase::Scripts:       return TEXT("scripts");
		case EMantlePublishPhase::Exporting:     return TEXT("exporting");
		case EMantlePublishPhase::Library:       return TEXT("library");
		case EMantlePublishPhase::CreatingPlace: return TEXT("creatingPlace");
		case EMantlePublishPhase::Saving:        return TEXT("saving");
		case EMantlePublishPhase::Publishing:    return TEXT("publishing");
		case EMantlePublishPhase::Done:          return TEXT("done");
		case EMantlePublishPhase::Failed:        return TEXT("failed");
		case EMantlePublishPhase::Cancelled:     return TEXT("cancelled");
		}
		return TEXT("unknown");
	}

	TSharedRef<FJsonObject> AgentCountsJson(const TMap<FString, int32>& Counts)
	{
		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		for (const TPair<FString, int32>& Count : Counts)
		{
			Object->SetNumberField(Count.Key, Count.Value);
		}
		return Object;
	}

	/** The item classes a skeletal mesh can be uploaded as, by the id the tools report. */
	const TCHAR* AgentCosmeticClassId(const EMantleCosmeticClass Class)
	{
		switch (Class)
		{
		case EMantleCosmeticClass::Garment: return TEXT("garment");
		case EMantleCosmeticClass::Hair:    return TEXT("hair");
		case EMantleCosmeticClass::Avatar:  return TEXT("avatar");
		case EMantleCosmeticClass::Emote:   return TEXT("emote");
		default:                            return LexToString(Class);
		}
	}

	const TCHAR* AgentCosmeticSeverityName(const EMantleCosmeticSeverity Severity)
	{
		switch (Severity)
		{
		case EMantleCosmeticSeverity::Error:   return TEXT("error");
		case EMantleCosmeticSeverity::Warning: return TEXT("warning");
		case EMantleCosmeticSeverity::Info:    return TEXT("info");
		}
		return TEXT("unknown");
	}

	const TCHAR* AgentCosmeticFileStateName(const EMantleUploadAssetState State)
	{
		switch (State)
		{
		case EMantleUploadAssetState::Waiting:    return TEXT("waiting");
		case EMantleUploadAssetState::Processing: return TEXT("processing");
		case EMantleUploadAssetState::Done:       return TEXT("done");
		case EMantleUploadAssetState::Failed:     return TEXT("failed");
		}
		return TEXT("unknown");
	}

	/** What CheckCosmetic returns for a built package (ok true); UploadCosmetic reuses it for its check_failed refusal. */
	TSharedRef<FJsonObject> AgentCosmeticPackageJson(const FMantleCosmeticPackage& Package, const FString& ObjectPath)
	{
		TArray<TSharedPtr<FJsonValue>> Issues;
		for (const FMantleCosmeticIssue& Issue : Package.Issues)
		{
			TSharedRef<FJsonObject> IssueObject = MakeShared<FJsonObject>();
			IssueObject->SetStringField(TEXT("severity"), AgentCosmeticSeverityName(Issue.Severity));
			IssueObject->SetStringField(TEXT("message"), Issue.Message);
			IssueObject->SetStringField(TEXT("hint"), Issue.Hint);
			Issues.Add(MakeShared<FJsonValueObject>(IssueObject));
		}
		TArray<TSharedPtr<FJsonValue>> Notes;
		for (const FString& Note : Package.Notes)
		{
			Notes.Add(MakeShared<FJsonValueString>(Note));
		}
		TArray<TSharedPtr<FJsonValue>> Files;
		for (const FMantleCosmeticUploadAsset& Asset : Package.Assets)
		{
			TSharedRef<FJsonObject> File = MakeShared<FJsonObject>();
			File->SetStringField(TEXT("key"), Asset.Key);
			File->SetStringField(TEXT("label"), Asset.Label);
			File->SetNumberField(TEXT("bytes"), Asset.Bytes.Num());
			Files.Add(MakeShared<FJsonValueObject>(File));
		}

		TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetBoolField(TEXT("ok"), true);
		Object->SetStringField(TEXT("asset"), ObjectPath);
		Object->SetStringField(TEXT("class"), AgentCosmeticClassId(Package.Class));
		Object->SetStringField(TEXT("slot"), Package.Slot);
		Object->SetStringField(TEXT("name"), Package.Name);
		Object->SetBoolField(TEXT("canUpload"), !Package.HasErrors());
		Object->SetArrayField(TEXT("issues"), Issues);
		Object->SetArrayField(TEXT("notes"), Notes);
		Object->SetArrayField(TEXT("files"), Files);
		return Object;
	}

	const TCHAR* const NotSignedInMessage =
		TEXT("Not signed in to Mantle. Ask the creator to choose Tools > Mantle > Sign in with creator token... ")
		TEXT("in the Unreal editor, then try again. Never ask them for the token itself.");
}

UWorld* FMantleAgentTools::EditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

void FMantleAgentTools::StopWaiting()
{
	for (const FTSTicker::FDelegateHandle& Handle : MantleAgentToolsPrivate::AgentWaitHandles)
	{
		FTSTicker::RemoveTicker(Handle);
	}
	MantleAgentToolsPrivate::AgentWaitHandles.Reset();
}

FString FMantleAgentTools::Refusal(const FString& Code, const FString& Message)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("ok"), false);
	Object->SetStringField(TEXT("refused"), Code);
	Object->SetStringField(TEXT("message"), Message);
	return MantleAgentToolsPrivate::WriteAgentJson(Object);
}

FString FMantleAgentTools::CheckPlace(UWorld* World)
{
	using namespace MantleAgentToolsPrivate;
	if (!World)
	{
		return Refusal(TEXT("no_level"), TEXT("There is no level open in the editor."));
	}

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(World);
	const int32 Errors = Validation.NumErrors();

	TArray<TSharedPtr<FJsonValue>> Issues;
	AddAgentIssues(Validation.Issues, Issues);

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("ok"), true);
	Object->SetStringField(TEXT("level"), World->GetName());
	Object->SetBoolField(TEXT("ready"), Errors == 0);
	Object->SetNumberField(TEXT("errors"), Errors);
	Object->SetNumberField(TEXT("warnings"), Validation.Issues.Num() - Errors);
	Object->SetArrayField(TEXT("issues"), Issues);
	return WriteAgentJson(Object);
}

FString FMantleAgentTools::ExportPreview(UWorld* World)
{
	using namespace MantleAgentToolsPrivate;
	if (!World)
	{
		return Refusal(TEXT("no_level"), TEXT("There is no level open in the editor."));
	}

	// Offline options, as Check and the preview export: no place id, no uploaded asset ids.
	const FMantleExportResult Export = FMantlePlaceExporter::Export(World, FMantleExportOptions());
	const FMantlePlace& Place = Export.Place;

	TMap<FString, int32> EntitiesByType;
	for (const FMantleEntity& Entity : Place.Entities)
	{
		++EntitiesByType.FindOrAdd(MantleEntityTypeToString(Entity.Type));
	}

	TMap<FString, int32> AssetsByKind;
	int64 AssetBytes = 0;
	for (const FMantleAssetRef& Asset : Place.Assets)
	{
		++AssetsByKind.FindOrAdd(AgentAssetKindName(Asset.Kind));
		AssetBytes += Asset.Bytes;
	}

	int64 PlaceJsonBytes = 0;
	if (Export.bSuccess)
	{
		const FTCHARToUTF8 Utf8(*FMantlePlaceExporter::ToJson(Place));
		PlaceJsonBytes = Utf8.Length();
	}

	TSharedRef<FJsonObject> Entities = MakeShared<FJsonObject>();
	Entities->SetNumberField(TEXT("total"), Place.Entities.Num());
	Entities->SetObjectField(TEXT("byType"), AgentCountsJson(EntitiesByType));

	TSharedRef<FJsonObject> Assets = MakeShared<FJsonObject>();
	Assets->SetNumberField(TEXT("total"), Place.Assets.Num());
	Assets->SetNumberField(TEXT("bytes"), static_cast<double>(AssetBytes));
	Assets->SetObjectField(TEXT("byKind"), AgentCountsJson(AssetsByKind));

	TSharedRef<FJsonObject> ScriptApi = MakeShared<FJsonObject>();
	ScriptApi->SetNumberField(TEXT("major"), Place.ScriptApi.Major);
	ScriptApi->SetNumberField(TEXT("minor"), Place.ScriptApi.Minor);
	ScriptApi->SetBoolField(TEXT("analysed"), Export.ScriptApiNeed.bAnalysed);

	TArray<TSharedPtr<FJsonValue>> Errors;
	for (const FMantleParseError& Error : Export.Errors)
	{
		TSharedRef<FJsonObject> ErrorObject = MakeShared<FJsonObject>();
		ErrorObject->SetStringField(TEXT("path"), Error.Path);
		ErrorObject->SetStringField(TEXT("message"), Error.Message);
		Errors.Add(MakeShared<FJsonValueObject>(ErrorObject));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("ok"), true);
	Object->SetStringField(TEXT("level"), World->GetName());
	Object->SetBoolField(TEXT("exported"), Export.bSuccess);
	Object->SetNumberField(TEXT("placeJsonBytes"), static_cast<double>(PlaceJsonBytes));
	Object->SetObjectField(TEXT("entities"), Entities);
	Object->SetObjectField(TEXT("assets"), Assets);
	Object->SetNumberField(TEXT("scripts"), Place.Scripts.Num());
	Object->SetObjectField(TEXT("scriptApi"), ScriptApi);
	Object->SetArrayField(TEXT("errors"), Errors);
	return WriteAgentJson(Object);
}

FString FMantleAgentTools::CheckPublishAllowed(UWorld* World)
{
	// Signed in first: nothing else is looked at, and no dialog opens, for a creator who is not.
	if (!FMantleEditorActions::IsSignedIn())
	{
		return Refusal(TEXT("not_signed_in"), MantleAgentToolsPrivate::NotSignedInMessage);
	}
	if (!World)
	{
		return Refusal(TEXT("no_level"), TEXT("There is no level open in the editor."));
	}
	FString WhyNot;
	if (!FMantleApiClient::IsConfigured(WhyNot))
	{
		return Refusal(TEXT("not_configured"), WhyNot);
	}
	// The panel's publish, which is PublishPlace's own in the editor; the agent's own publish is also
	// asked after, so publishes run one at a time.
	const TSharedPtr<FMantlePlacePublishSession> Own = MantleAgentToolsPrivate::AgentPublish.Pin();
	if (FMantleEditorActions::IsPublishRunning() || (Own.IsValid() && !Own->IsFinished()))
	{
		return Refusal(TEXT("publish_running"),
			TEXT("A publish is already running in this editor. Follow it with UploadStatus and try again once it ends."));
	}
	return FString();
}

FString FMantleAgentTools::DescribePublishResult(const FMantlePublishResult& Result)
{
	using namespace MantleAgentToolsPrivate;
	TArray<TSharedPtr<FJsonValue>> Issues;
	AddAgentIssues(Result.Validation.Issues, Issues);
	AddAgentIssues(Result.Attention, Issues);

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("ok"), Result.bSuccess);
	Object->SetStringField(TEXT("placeId"), Result.PlaceId);
	Object->SetNumberField(TEXT("revision"), Result.RevisionNumber);
	Object->SetBoolField(TEXT("live"), Result.bWentLive);
	Object->SetBoolField(TEXT("waitingForReview"), Result.bWaitingForReview);
	Object->SetBoolField(TEXT("cancelled"), Result.bCancelled);
	Object->SetStringField(TEXT("message"), Result.Message);
	Object->SetArrayField(TEXT("issues"), Issues);
	return WriteAgentJson(Object);
}

FString FMantleAgentTools::ListMyPlaces()
{
	using namespace MantleAgentToolsPrivate;
	if (!FMantleEditorActions::IsSignedIn())
	{
		return Refusal(TEXT("not_signed_in"), NotSignedInMessage);
	}
	FString WhyNot;
	if (!FMantleApiClient::IsConfigured(WhyNot))
	{
		return Refusal(TEXT("not_configured"), WhyNot);
	}

	const FMantleApiResult Response = FMantleApiClient::ListMine();
	if (!Response.bSuccess)
	{
		TSharedRef<FJsonObject> Failed = MakeShared<FJsonObject>();
		Failed->SetBoolField(TEXT("ok"), false);
		Failed->SetStringField(TEXT("refused"), TEXT("api_error"));
		Failed->SetNumberField(TEXT("status"), Response.StatusCode);
		Failed->SetStringField(TEXT("code"), Response.Code);
		Failed->SetStringField(TEXT("message"), FMantlePublisher::DescribeApiFailure(Response));
		return WriteAgentJson(Failed);
	}

	TSharedPtr<FJsonObject> Body;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<TCHAR>::Create(Response.Body), Body) || !Body.IsValid())
	{
		return Refusal(TEXT("bad_response"), TEXT("Mantle answered, but not with a list of places."));
	}

	TArray<TSharedPtr<FJsonValue>> Places;
	const TArray<TSharedPtr<FJsonValue>>* Listed = nullptr;
	if (Body->TryGetArrayField(TEXT("places"), Listed) && Listed)
	{
		Places = *Listed;
	}
	FString NextCursor;
	Body->TryGetStringField(TEXT("nextCursor"), NextCursor);

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("ok"), true);
	Object->SetArrayField(TEXT("places"), Places);
	Object->SetBoolField(TEXT("more"), !NextCursor.IsEmpty());
	return WriteAgentJson(Object);
}

FString FMantleAgentTools::UploadStatus(const FMantlePlacePublishSession* Session, const FString& AssetName)
{
	using namespace MantleAgentToolsPrivate;
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("ok"), true);

	TArray<TSharedPtr<FJsonValue>> Assets;
	if (!Session)
	{
		Object->SetField(TEXT("publish"), MakeShared<FJsonValueNull>());
		Object->SetArrayField(TEXT("assets"), Assets);
		Object->SetStringField(TEXT("message"), TEXT("No publish or save has run in this editor since it started."));
		return WriteAgentJson(Object);
	}

	TSharedRef<FJsonObject> Publish = MakeShared<FJsonObject>();
	Publish->SetStringField(TEXT("phase"), AgentPublishPhaseName(Session->GetPhase()));
	Publish->SetBoolField(TEXT("running"), !Session->IsFinished());
	Publish->SetBoolField(TEXT("goLive"), Session->IsGoLive());
	Publish->SetNumberField(TEXT("progress"), Session->GetProgress());
	Publish->SetStringField(TEXT("status"), Session->GetStatusText());
	Publish->SetStringField(TEXT("placeId"), Session->GetResult().PlaceId);
	Publish->SetNumberField(TEXT("revision"), Session->GetResult().RevisionNumber);
	Object->SetObjectField(TEXT("publish"), Publish);

	if (const FMantleAssetUploadQueue* Queue = Session->GetQueue())
	{
		for (const FMantleQueuedUploadStatus& Status : Queue->GetStatuses())
		{
			// Names and paths match loosely, as a creator types them; an ast_ id only exactly.
			const bool bMatches = AssetName.IsEmpty()
				|| Status.Name.Contains(AssetName)
				|| Status.Key.Contains(AssetName)
				|| Status.AssetId.Equals(AssetName, ESearchCase::CaseSensitive);
			if (!bMatches)
			{
				continue;
			}
			TSharedRef<FJsonObject> Asset = MakeShared<FJsonObject>();
			Asset->SetStringField(TEXT("name"), Status.Name);
			Asset->SetStringField(TEXT("state"), AgentUploadStateName(Status.State));
			Asset->SetStringField(TEXT("assetId"), Status.AssetId);
			Asset->SetNumberField(TEXT("bytes"), static_cast<double>(Status.Bytes));
			Asset->SetBoolField(TEXT("alreadyOnMantle"), Status.bAlreadyOnMantle);
			Asset->SetBoolField(TEXT("inReview"), Status.bInReview);
			Asset->SetStringField(TEXT("code"), Status.Code);
			Asset->SetStringField(TEXT("message"), Status.Message);
			Asset->SetStringField(TEXT("cookError"), Status.CookError);
			Assets.Add(MakeShared<FJsonValueObject>(Asset));
		}
	}
	Object->SetArrayField(TEXT("assets"), Assets);
	return WriteAgentJson(Object);
}

FString FMantleAgentTools::ExplainIssue(UWorld* World, const FString& Query)
{
	using namespace MantleAgentToolsPrivate;
	const FString Trimmed = Query.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		return Refusal(TEXT("no_query"),
			TEXT("Say which issue: part of its message, its actor's label or its remedy, as CheckPlace returned it."));
	}
	if (!World)
	{
		return Refusal(TEXT("no_level"), TEXT("There is no level open in the editor."));
	}

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(World);
	TArray<FMantleValidationIssue> Matching;
	for (const FMantleValidationIssue& Issue : Validation.Issues)
	{
		// Loosely, as a person or an agent quotes it back.
		if (Issue.Problem.Contains(Trimmed) || Issue.Actor.Contains(Trimmed) || Issue.Remedy.Contains(Trimmed))
		{
			Matching.Add(Issue);
		}
	}
	TArray<TSharedPtr<FJsonValue>> Matches;
	AddAgentIssues(Matching, Matches);

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("ok"), true);
	Object->SetStringField(TEXT("query"), Trimmed);
	Object->SetArrayField(TEXT("matches"), Matches);
	if (Matching.Num() == 0)
	{
		Object->SetStringField(TEXT("message"), TEXT("No issue in the level matches now. Run CheckPlace for the current list."));
	}
	return WriteAgentJson(Object);
}

FString FMantleAgentTools::ScriptApiRequirement(UWorld* World)
{
	using namespace MantleAgentToolsPrivate;
	if (!World)
	{
		return Refusal(TEXT("no_level"), TEXT("There is no level open in the editor."));
	}

	// What a publish records: Export's scriptApi (offline options, as ExportPreview).
	const FMantleExportResult Export = FMantlePlaceExporter::Export(World, FMantleExportOptions());

	// Each script's own need, found the way Check this place finds them (#394).
	TArray<TSharedPtr<FJsonValue>> PerScript;
	auto AddScript = [&PerScript](const FString& Path, const FMantleScriptApiNeed& Need)
	{
		TSharedRef<FJsonObject> Script = MakeShared<FJsonObject>();
		Script->SetStringField(TEXT("path"), Path);
		Script->SetNumberField(TEXT("minor"), Need.Minor);
		Script->SetStringField(TEXT("neededFor"), Need.NeededFor);
		Script->SetBoolField(TEXT("analysed"), Need.bAnalysed);
		FString Problem;
		FString Remedy;
		if (!Need.bAnalysed)
		{
			FMantleScriptApiUsage::DescribeUnanalysable(Path, Need, Problem, Remedy);
		}
		Script->SetStringField(TEXT("problem"), Problem);
		Script->SetStringField(TEXT("remedy"), Remedy);
		PerScript.Add(MakeShared<FJsonValueObject>(Script));
	};
	const FString ScriptsFolder = FMantlePlaceExporter::GetScriptsFolder(World);
	if (!ScriptsFolder.IsEmpty())
	{
		TArray<FMantleScriptCatalogEntry> Entries;
		TArray<FMantleScriptCollectionIssue> CollectionIssues;
		FMantlePlaceExporter::CollectScriptFiles(ScriptsFolder, Entries, CollectionIssues);
		TArray<FMantleGraphScriptEntry> GraphEntries;
		TArray<FMantleParseError> GraphErrors;
		TArray<FString> GraphPaths;
		FMantlePlaceExporter::CollectGraphScripts(ScriptsFolder, GraphEntries, GraphErrors, GraphPaths);
		FMantlePlaceExporter::FilterGraphGeneratedFiles(Entries, CollectionIssues, GraphPaths);
		for (const FMantleScriptCatalogEntry& Entry : Entries)
		{
			AddScript(Entry.Path, FMantleScriptApiUsage::ForSource(Entry.Source, Entry.Language));
		}
		for (const FMantleGraphScriptEntry& Entry : GraphEntries)
		{
			AddScript(Entry.Path, Entry.ApiNeed);
		}
	}

	TSharedRef<FJsonObject> Recorded = MakeShared<FJsonObject>();
	Recorded->SetNumberField(TEXT("major"), Export.Place.ScriptApi.Major);
	Recorded->SetNumberField(TEXT("minor"), Export.Place.ScriptApi.Minor);

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("ok"), true);
	Object->SetNumberField(TEXT("scripts"), Export.Place.Scripts.Num());
	Object->SetObjectField(TEXT("recorded"), Recorded);
	Object->SetNumberField(TEXT("editorMinor"), FMantleApiManifest::Get().Minor);
	Object->SetBoolField(TEXT("analysed"), Export.ScriptApiNeed.bAnalysed);
	Object->SetStringField(TEXT("neededFor"), Export.ScriptApiNeed.NeededFor);
	Object->SetArrayField(TEXT("perScript"), PerScript);
	return WriteAgentJson(Object);
}

FString FMantleAgentTools::PreviewPlace(UWorld* World)
{
	using namespace MantleAgentToolsPrivate;
	if (!World)
	{
		return Refusal(TEXT("no_level"), TEXT("There is no level open in the editor."));
	}
	if (bAgentPreviewRunning)
	{
		return Refusal(TEXT("preview_running"), TEXT("A preview is already being built. Try again when it has finished."));
	}
	TGuardValue<bool> Running(bAgentPreviewRunning, true);

	// As the menu: signed in, the level's own meshes and materials load by the ids Mantle holds (#394).
	FMantlePlaceAssetSet Uploaded;
	FMantlePreviewOptions Options;
	if (FMantleEditorActions::IsSignedIn())
	{
		FString Problem;
		if (!FMantlePlacePreview::FindUploadedAssets(World, Uploaded, FMantlePlacePreview::ApiDiff(), Problem))
		{
			UE_LOG(LogMantle, Warning, TEXT("Mantle toolset: PreviewPlace could not ask Mantle which files it holds: %s"), *Problem);
		}
		Options.Assets = &Uploaded;
	}
	FMantlePreviewResult Preview = FMantlePlacePreview::Build(World, Options);

	TArray<TSharedPtr<FJsonValue>> LeftOut;
	for (const FString& Line : Preview.LeftOut)
	{
		LeftOut.Add(MakeShared<FJsonValueString>(Line));
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	AddAgentIssues(Preview.Validation.Issues, Issues);

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("ok"), true);
	Object->SetStringField(TEXT("level"), World->GetName());
	Object->SetBoolField(TEXT("built"), Preview.bSuccess);
	Object->SetNumberField(TEXT("sourceEntities"), Preview.SourceEntityCount);
	Object->SetNumberField(TEXT("previewActors"), Preview.PreviewActorCount);
	Object->SetNumberField(TEXT("notUploaded"), Preview.NotUploadedCount);
	Object->SetArrayField(TEXT("leftOut"), LeftOut);
	Object->SetStringField(TEXT("summary"), FMantlePlacePreview::DescribeForCreator(Preview));
	Object->SetArrayField(TEXT("issues"), Issues);

	// Torn down straight away, as the menu does: a report, not a world to walk around in.
	FMantlePlacePreview::Destroy(Preview);
	return WriteAgentJson(Object);
}

bool FMantleAgentTools::BuildCosmeticPackage(const FString& AssetPath, const FString& Slot, const FString& Name, const FString& Sections,
	const FString& ItemId, const FString& FitSex, FMantleCosmeticPackage& OutPackage, FString& OutObjectPath, FString& OutRefusal)
{
	// The panel's "Made for": which standard bodies (women's or men's) an Outfit's sizes are fitted to.
	EMantleFitSex Sex = EMantleFitSex::Auto;
	if (!FMantleEditorActions::ParseFitSex(FitSex, Sex))
	{
		OutRefusal = Refusal(TEXT("unknown_fit_sex"), FString::Printf(
			TEXT("\"%s\" is not a FitSex. Use \"female\" or \"male\" for the body the clothing was made on, or \"\" (or \"auto\") to let Mantle choose."), *FitSex));
		return false;
	}
	FString ContractError;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!Contract)
	{
		OutRefusal = Refusal(TEXT("no_contract"), ContractError.IsEmpty()
			? FString(TEXT("Mantle's avatar rules (limits.json) could not be read.")) : ContractError);
		return false;
	}

	// "/Game/A/B" or "/Game/A/B.B": the object path, checked on disk before loading so a wrong
	// path is a refusal, not a load warning.
	FString ObjectPath = AssetPath.TrimStartAndEnd();
	if (!ObjectPath.IsEmpty() && !FPackageName::GetShortName(ObjectPath).Contains(TEXT(".")))
	{
		ObjectPath += TEXT(".") + FPackageName::GetShortName(ObjectPath);
	}
	const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
	UObject* Asset = nullptr;
	if (!PackageName.IsEmpty() && FPackageName::IsValidLongPackageName(PackageName, /*bIncludeReadOnlyRoots*/ true))
	{
		// An asset already in the editor counts, saved or not, as the panel's picker lists it.
		Asset = FindObject<UObject>(nullptr, *ObjectPath);
		if (!Asset && FPackageName::DoesPackageExist(PackageName))
		{
			Asset = LoadObject<UObject>(nullptr, *ObjectPath);
		}
	}
	if (!Asset)
	{
		OutRefusal = Refusal(TEXT("not_found"), FString::Printf(
			TEXT("There is no asset at \"%s\". Give its path as the Content Browser shows it, e.g. /Game/Clothing/SKM_Jacket."), *AssetPath));
		return false;
	}
	// An animation sequence uploads as an emote, in the emote slot, exactly as the panel's Emote class exports it (retargeted
	// onto the UEFN mannequin from the MetaHuman, UE5 or UE4 Mannequin skeleton).
	if (Cast<UAnimSequence>(Asset))
	{
		const TArray<FString> EmoteSlots = CosmeticSlotOptions(*Contract, EMantleCosmeticClass::Emote);
		const FString EmoteSlot = Slot.TrimStartAndEnd().IsEmpty() && EmoteSlots.Num() > 0 ? EmoteSlots[0] : Slot.TrimStartAndEnd();
		if (!EmoteSlots.Contains(EmoteSlot))
		{
			OutRefusal = Refusal(TEXT("animation_not_emote"), FString::Printf(
				TEXT("%s is an animation, which uploads as an emote. Use the slot %s, or \"\"."), *Asset->GetName(), *FString::Join(EmoteSlots, TEXT(", "))));
			return false;
		}
		FMantleCosmeticDraft Draft = FMantleEditorActions::MakeCosmeticDraft(EMantleCosmeticClass::Emote, Asset, EmoteSlot, Name, *Contract, Sex);
		return MantleAgentToolsPrivate::FinishCosmeticPackage(Draft, ItemId, *Contract, OutPackage, OutObjectPath, OutRefusal);
	}

	// A texture uploads as an overlay (a tattoo, scar or makeup mask), in an overlay slot, exactly as the panel's Overlay class
	// exports it from a mask texture.
	if (Cast<UTexture2D>(Asset))
	{
		const TArray<FString> OverlaySlots = CosmeticSlotOptions(*Contract, EMantleCosmeticClass::Overlay);
		const FString OverlaySlot = Slot.TrimStartAndEnd().IsEmpty() && OverlaySlots.Num() > 0 ? OverlaySlots[0] : Slot.TrimStartAndEnd();
		if (!OverlaySlots.Contains(OverlaySlot))
		{
			OutRefusal = Refusal(TEXT("texture_not_overlay"), FString::Printf(
				TEXT("%s is a texture, which uploads as an overlay. Use the slot %s, or \"\"."), *Asset->GetName(), *FString::Join(OverlaySlots, TEXT(", "))));
			return false;
		}
		FMantleCosmeticDraft Draft = FMantleEditorActions::MakeCosmeticDraft(EMantleCosmeticClass::Overlay, Asset, OverlaySlot, Name, *Contract, Sex);
		return MantleAgentToolsPrivate::FinishCosmeticPackage(Draft, ItemId, *Contract, OutPackage, OutObjectPath, OutRefusal);
	}

	// A MetaHuman Outfit Asset (a parametric .mhpkg outfit) is clothing too: the panel exports it the same way.
	const bool bOutfit = FMantleParametricOutfit::IsSource(Asset);
	// A Chaos cloth asset (not an Outfit) is clothing too: it is wrapped as an Outfit for its sizes (FMantleParametricOutfit::WrapGarment).
	const bool bClothAsset = FMantleParametricOutfit::IsGarmentSource(Asset) && !Cast<USkeletalMesh>(Asset);
	// Hair the panel uploads from a Groom (its cards) or a hair cards static mesh, in a hair slot only.
	const bool bHairCardsSource = FMantleCosmeticExport::IsGroomAsset(Asset) || Cast<UStaticMesh>(Asset);
	if (!Cast<USkeletalMesh>(Asset) && !bOutfit && !bClothAsset && !bHairCardsSource)
	{
		OutRefusal = Refusal(TEXT("not_skeletal_mesh"), FString::Printf(
			TEXT("%s is a %s, not a skeletal mesh, a Chaos cloth asset, a MetaHuman Outfit, a Groom, a hair cards static mesh, a texture or an animation sequence. These tools check and upload clothing, hair and avatars made from skeletal meshes, clothing from Chaos cloth assets and MetaHuman Outfits, hair from Grooms and cards meshes, overlays from textures and emotes from animation sequences. A MetaHuman Creator character or a Blueprint goes through ConvertCosmetic first."),
			*Asset->GetName(), *Asset->GetClass()->GetName()));
		return false;
	}

	// The item class the slot belongs to, among those a skeletal mesh can be; the panel's default
	// (3D clothing, its first slot) when none is given.
	const EMantleCosmeticClass Classes[] = { EMantleCosmeticClass::Garment, EMantleCosmeticClass::Hair, EMantleCosmeticClass::Avatar, EMantleCosmeticClass::Accessory };
	EMantleCosmeticClass Class = EMantleCosmeticClass::Garment;
	FString ChosenSlot = Slot.TrimStartAndEnd();
	TArray<FString> Known;
	bool bFound = false;
	for (const EMantleCosmeticClass Candidate : Classes)
	{
		const TArray<FString> Options = CosmeticSlotOptions(*Contract, Candidate);
		if (ChosenSlot.IsEmpty() && Candidate == EMantleCosmeticClass::Garment && Options.Num() > 0)
		{
			ChosenSlot = Options[0];
		}
		if (!bFound && Options.ContainsByPredicate([&ChosenSlot](const FString& Option) { return Option.Equals(ChosenSlot, ESearchCase::CaseSensitive); }))
		{
			Class = Candidate;
			bFound = true;
		}
		Known.Append(Options);
	}
	if (!bFound)
	{
		OutRefusal = Refusal(TEXT("unknown_slot"), FString::Printf(
			TEXT("\"%s\" is not a slot for clothing, hair, an avatar or an accessory. Use one of: %s."), *Slot, *FString::Join(Known, TEXT(", "))));
		return false;
	}

	if (Class == EMantleCosmeticClass::Avatar && !Cast<USkeletalMesh>(Asset))
	{
		OutRefusal = Refusal(TEXT("convert_first"), FString::Printf(
			TEXT("%s is a %s. An avatar uploads the body ConvertCosmetic makes: run ConvertCosmetic on it with bApply true, then check the converted body it lists."),
			*Asset->GetName(), *Asset->GetClass()->GetName()));
		return false;
	}
	if (bOutfit && Class != EMantleCosmeticClass::Garment && Class != EMantleCosmeticClass::Accessory)
	{
		OutRefusal = Refusal(TEXT("outfit_not_clothing"), FString::Printf(
			TEXT("%s is a MetaHuman Outfit, which uploads as clothing, or as a rigid accessory such as a hat. Use one of: %s, %s."),
			*Asset->GetName(), *FString::Join(CosmeticSlotOptions(*Contract, EMantleCosmeticClass::Garment), TEXT(", ")),
			*FString::Join(CosmeticSlotOptions(*Contract, EMantleCosmeticClass::Accessory), TEXT(", "))));
		return false;
	}
	if (bHairCardsSource && Class != EMantleCosmeticClass::Hair)
	{
		OutRefusal = Refusal(TEXT("cards_not_hair"), FString::Printf(
			TEXT("%s is a Groom or a cards mesh, which uploads as hair. Use one of: %s."),
			*Asset->GetName(), *FString::Join(CosmeticSlotOptions(*Contract, EMantleCosmeticClass::Hair), TEXT(", "))));
		return false;
	}
	if (bClothAsset && Class != EMantleCosmeticClass::Garment)
	{
		OutRefusal = Refusal(TEXT("cloth_not_clothing"), FString::Printf(
			TEXT("%s is a Chaos cloth asset, which uploads as clothing. Use one of: %s."),
			*Asset->GetName(), *FString::Join(CosmeticSlotOptions(*Contract, EMantleCosmeticClass::Garment), TEXT(", "))));
		return false;
	}
	if (!bOutfit && Class == EMantleCosmeticClass::Accessory)
	{
		OutRefusal = Refusal(TEXT("accessory_needs_outfit"), FString::Printf(
			TEXT("%s is a skeletal mesh; these tools upload accessories from MetaHuman Outfits. Use Tools > Mantle > Cosmetics... for a static mesh accessory."),
			*Asset->GetName()));
		return false;
	}

	FMantleCosmeticDraft Draft = FMantleEditorActions::MakeCosmeticDraft(Class, Asset, ChosenSlot, Name, *Contract, Sex);
	TArray<FString> Wanted;
	Sections.ParseIntoArray(Wanted, TEXT(","), true);
	if (!Wanted.IsEmpty())
	{
		// The panel's section ticks: material slots named by their material or slot name.
		const USkinnedAsset* Skinned = Cast<USkinnedAsset>(Asset);
		const TArray<FSkeletalMaterial> Materials = Skinned ? Skinned->GetMaterials() : TArray<FSkeletalMaterial>();
		TArray<int32> Keep;
		TArray<FString> SectionNames;
		for (FString& Want : Wanted)
		{
			Want.TrimStartAndEndInline();
			bool bMatched = false;
			for (int32 Index = 0; Index < Materials.Num(); ++Index)
			{
				const FString MaterialName = Materials[Index].MaterialInterface ? Materials[Index].MaterialInterface->GetName() : FString();
				if (Want.Equals(MaterialName, ESearchCase::IgnoreCase) || Want.Equals(Materials[Index].MaterialSlotName.ToString(), ESearchCase::IgnoreCase))
				{
					Keep.AddUnique(Index);
					bMatched = true;
				}
			}
			if (!bMatched)
			{
				for (const FSkeletalMaterial& Material : Materials)
				{
					SectionNames.AddUnique(Material.MaterialInterface ? Material.MaterialInterface->GetName() : Material.MaterialSlotName.ToString());
				}
				OutRefusal = Refusal(TEXT("unknown_section"), FString::Printf(
					TEXT("%s has no section \"%s\". Its sections: %s."), *Asset->GetName(), *Want, *FString::Join(SectionNames, TEXT(", "))));
				return false;
			}
		}
		Draft.KeepSlots = MoveTemp(Keep);
	}
	return MantleAgentToolsPrivate::FinishCosmeticPackage(Draft, ItemId, *Contract, OutPackage, OutObjectPath, OutRefusal);
}

FString FMantleAgentTools::MeasureHead(const FString& BodyPath, const FString& FacePath)
{
	USkeletalMesh* Body = LoadObject<USkeletalMesh>(nullptr, *BodyPath);
	USkeletalMesh* Face = FacePath.IsEmpty() ? nullptr : LoadObject<USkeletalMesh>(nullptr, *FacePath);
	if (!Body || (!FacePath.IsEmpty() && !Face))
	{
		return Refusal(TEXT("not_found"), FString::Printf(TEXT("There is no skeletal mesh at \"%s\"."), !Body ? *BodyPath : *FacePath));
	}
	const FTransform SocketInHead = FMantleHeadFit::HatSocketInHead(Body);
	FBox3f Cranium;
	int32 Vertices = 0;
	FString Error;
	if (!FMantleHeadFit::MeasureCranium(Body, Face, SocketInHead, Cranium, Vertices, Error))
	{
		return Refusal(TEXT("not_measured"), FString::Printf(TEXT("%s: %s."), *Body->GetName(), *Error));
	}
	auto Vector = [](const FVector3f& Value)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (int32 Axis = 0; Axis < 3; ++Axis) { Out.Add(MakeShared<FJsonValueNumber>(FMath::RoundToFloat(Value[Axis] * 100.0f) / 100.0f)); }
		return Out;
	};
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("ok"), true);
	Object->SetNumberField(TEXT("vertices"), Vertices);
	Object->SetArrayField(TEXT("min"), Vector(Cranium.Min));
	Object->SetArrayField(TEXT("max"), Vector(Cranium.Max));
	Object->SetArrayField(TEXT("size"), Vector(Cranium.GetSize()));
	FBox3f Reference;
	const bool bReference = FMantleHeadFit::ReferenceCranium(Reference);
	Object->SetBoolField(TEXT("reference"), bReference);
	if (bReference)
	{
		const FMantleCosmeticSocketOverride Hat = FMantleHeadFit::HatSocket(Reference, Cranium, SocketInHead);
		Object->SetArrayField(TEXT("location"), Vector(Hat.Location));
		Object->SetArrayField(TEXT("scale"), Vector(Hat.Scale));
	}
	return MantleAgentToolsPrivate::WriteAgentJson(Object);
}

FString FMantleAgentTools::CheckCosmetic(const FString& AssetPath, const FString& Slot, const FString& Name, const FString& Sections, const FString& FitSex)
{
	FMantleCosmeticPackage Package;
	FString ObjectPath;
	FString Refused;
	if (!BuildCosmeticPackage(AssetPath, Slot, Name, Sections, FString(), FitSex, Package, ObjectPath, Refused))
	{
		return Refused;
	}
	return MantleAgentToolsPrivate::WriteAgentJson(MantleAgentToolsPrivate::AgentCosmeticPackageJson(Package, ObjectPath));
}

FString FMantleAgentTools::CheckUploadCosmeticAllowed()
{
	if (!FMantleEditorActions::IsSignedIn())
	{
		return Refusal(TEXT("not_signed_in"), MantleAgentToolsPrivate::NotSignedInMessage);
	}
	FString WhyNot;
	if (!FMantleApiClient::IsConfigured(WhyNot))
	{
		return Refusal(TEXT("not_configured"), WhyNot);
	}
	const TSharedPtr<FMantleCosmeticUploadSession> Running = MantleAgentToolsPrivate::AgentCosmeticUpload.Pin();
	if (Running.IsValid() && !Running->IsFinished())
	{
		return Refusal(TEXT("upload_running"),
			TEXT("A cosmetic upload started from the agent tools is still running. Try again once it ends."));
	}
	return FString();
}

FString FMantleAgentTools::DescribeCosmeticUpload(const FMantleCosmeticUploadSession& Session)
{
	using namespace MantleAgentToolsPrivate;
	TArray<TSharedPtr<FJsonValue>> Files;
	for (const FMantleUploadAssetProgress& Asset : Session.GetAssets())
	{
		TSharedRef<FJsonObject> File = MakeShared<FJsonObject>();
		File->SetStringField(TEXT("label"), Asset.Label);
		File->SetStringField(TEXT("state"), AgentCosmeticFileStateName(Asset.State));
		File->SetStringField(TEXT("assetId"), Asset.AssetId);
		File->SetBoolField(TEXT("alreadyOnMantle"), Asset.bAlreadyOnMantle);
		File->SetStringField(TEXT("message"), Asset.Message);
		Files.Add(MakeShared<FJsonValueObject>(File));
	}

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetBoolField(TEXT("ok"), Session.Succeeded());
	Object->SetStringField(TEXT("itemId"), Session.GetItemId());
	Object->SetStringField(TEXT("itemStatus"), Session.GetItemStatus());
	Object->SetStringField(TEXT("moderationState"), Session.GetItemModerationState());
	Object->SetBoolField(TEXT("mayClipInMotion"), Session.GetItemMayClipInMotion());
	Object->SetStringField(TEXT("message"), Session.GetFailure().IsEmpty() ? Session.GetStatusText() : Session.GetFailure());
	Object->SetArrayField(TEXT("files"), Files);
	return WriteAgentJson(Object);
}

FString UMantleToolset::CheckPlace()
{
	return FMantleAgentTools::CheckPlace(FMantleAgentTools::EditorWorld());
}

FString UMantleToolset::ExportPreview()
{
	return FMantleAgentTools::ExportPreview(FMantleAgentTools::EditorWorld());
}

UToolCallAsyncResultString* FMantleAgentTools::PublishPlace(UWorld* World, const FString& Notes)
{
	using namespace MantleAgentToolsPrivate;
	UToolCallAsyncResultString* Result = NewObject<UToolCallAsyncResultString>();
	const FString Refused = CheckPublishAllowed(World);
	if (!Refused.IsEmpty())
	{
		Result->SetValue(Refused);
		return Result;
	}

	// The menu's own background publish, under the Mantle Publish panel: the creator sees it and
	// can cancel it there. This call completes when the session ends.
	const TSharedPtr<FMantlePlacePublishSession> Session = StartAgentPublish(World, Notes);
	if (!Session.IsValid())
	{
		Result->SetValue(Refusal(TEXT("not_started"),
			TEXT("The publish did not start. The Mantle Publish panel in the editor says why.")));
		return Result;
	}
	AgentPublish = Session;
	UE_LOG(LogMantle, Log, TEXT("Mantle toolset: PublishPlace started a publish of %s."), *World->GetName());

	const TSharedRef<FMantlePlacePublishSession> Running = Session.ToSharedRef();
	WaitForAgentResult(Result, [Running]() -> TOptional<FString>
		{
			if (!Running->IsFinished())
			{
				return TOptional<FString>();
			}
			return TOptional<FString>(FMantleAgentTools::DescribePublishResult(Running->GetResult()));
		},
		0.5f);
	return Result;
}

UToolCallAsyncResultString* FMantleAgentTools::UploadCosmetic(const FString& AssetPath, const FString& Slot, const FString& Name, const FString& Sections, const FString& ItemId,
	const FString& FitSex)
{
	using namespace MantleAgentToolsPrivate;
	UToolCallAsyncResultString* Result = NewObject<UToolCallAsyncResultString>();
	// Held until it is returned: building an Outfit's sized copies collects garbage, which would take the unreferenced
	// result with it (the toolset then reports it "failed to return a UToolCallAsyncResult instance").
	const TStrongObjectPtr<UToolCallAsyncResultString> KeepResult(Result);
	// Signed in first, as PublishPlace: nothing is loaded or exported for a creator who is not.
	const FString Refused = CheckUploadCosmeticAllowed();
	if (!Refused.IsEmpty())
	{
		Result->SetValue(Refused);
		return Result;
	}

	FMantleCosmeticPackage Package;
	FString ObjectPath;
	FString NotBuilt;
	const bool bBuilt = BuildCosmeticPackage(AssetPath, Slot, Name, Sections, ItemId, FitSex, Package, ObjectPath, NotBuilt);
	if (!bBuilt)
	{
		Result->SetValue(NotBuilt);
		return Result;
	}
	// A rights declaration is the creator's own act, ticked in the panel, so an agent never makes one:
	// what it uploads is private (#614). The creator can publish it publicly from the panel.
	Package.Visibility = TEXT("private");
	Package.RightsDeclarationVersion.Reset();
	if (Package.HasErrors())
	{
		// The panel stops here too: its Upload runs the check and sends nothing while it finds errors.
		const TSharedRef<FJsonObject> CheckFailed = AgentCosmeticPackageJson(Package, ObjectPath);
		CheckFailed->SetBoolField(TEXT("ok"), false);
		CheckFailed->SetStringField(TEXT("refused"), TEXT("check_failed"));
		CheckFailed->SetStringField(TEXT("message"), TEXT("The check found problems (see issues). Nothing was uploaded; fix them and ask again."));
		Result->SetValue(WriteAgentJson(CheckFailed));
		return Result;
	}

	// The panel's Upload to Mantle: the same session, transport and resume file, ticked here instead
	// of by the panel's active timer.
	const FString ResumeFile = FMantleCosmeticUploadSession::DefaultResumeFile(ObjectPath, Package.Slot);
	const TSharedRef<FMantleCosmeticUploadSession> Session =
		MakeShared<FMantleCosmeticUploadSession>(MoveTemp(Package), MakeShared<FMantleApiUploadTransport>(), ResumeFile);
	Session->Start();
	AgentCosmeticUpload = Session;
	UE_LOG(LogMantle, Log, TEXT("Mantle toolset: UploadCosmetic started an upload of %s."), *ObjectPath);

	WaitForAgentResult(Result, [Session]() -> TOptional<FString>
		{
			Session->Tick();
			if (!Session->IsFinished())
			{
				return TOptional<FString>();
			}
			UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_UPLOAD %s item=%s status=%s"),
				Session->Succeeded() ? TEXT("DONE") : TEXT("STOPPED"), *Session->GetItemId(), *Session->GetItemStatus());
			return TOptional<FString>(FMantleAgentTools::DescribeCosmeticUpload(*Session));
		},
		0.1f);
	return Result;
}

UToolCallAsyncResultString* UMantleToolset::PublishPlace(const FString& Notes)
{
	return FMantleAgentTools::PublishPlace(FMantleAgentTools::EditorWorld(), Notes);
}

FString UMantleToolset::ListMyPlaces()
{
	return FMantleAgentTools::ListMyPlaces();
}

FString UMantleToolset::UploadStatus(const FString& AssetName)
{
	const TSharedPtr<FMantlePlacePublishSession> Session = FMantleEditorActions::GetPublishSession();
	return FMantleAgentTools::UploadStatus(Session.Get(), AssetName);
}

FString UMantleToolset::ExplainIssue(const FString& Query)
{
	return FMantleAgentTools::ExplainIssue(FMantleAgentTools::EditorWorld(), Query);
}

FString UMantleToolset::ScriptApiRequirement()
{
	return FMantleAgentTools::ScriptApiRequirement(FMantleAgentTools::EditorWorld());
}

FString UMantleToolset::PreviewPlace()
{
	return FMantleAgentTools::PreviewPlace(FMantleAgentTools::EditorWorld());
}

FString UMantleToolset::MeasureHead(const FString& BodyPath, const FString& FacePath)
{
	return FMantleAgentTools::MeasureHead(BodyPath, FacePath);
}

FString UMantleToolset::CheckCosmetic(const FString& AssetPath, const FString& Slot, const FString& Name, const FString& Sections, const FString& FitSex)
{
	return FMantleAgentTools::CheckCosmetic(AssetPath, Slot, Name, Sections, FitSex);
}

UToolCallAsyncResultString* UMantleToolset::UploadCosmetic(const FString& AssetPath, const FString& Slot, const FString& Name, const FString& Sections, const FString& ItemId,
	const FString& FitSex)
{
	return FMantleAgentTools::UploadCosmetic(AssetPath, Slot, Name, Sections, ItemId, FitSex);
}

FString UMantleToolset::ConvertCosmetic(const FString& AssetPath, const FString& Slot, const bool bApply, const FString& TurnOff, const FString& Confirm,
	const FString& Route)
{
	// #610: the converter's own JSON (MantleEditor), so the panel, the toolset and the tests read one description.
	return FMantleAvatarConverter::RunForAgent(AssetPath, Slot, bApply, TurnOff, Confirm, Route);
}

FString UMantleToolset::MakeOutfitFromGarment(const FString& BodyPath, const FString& GarmentPaths, const FString& TurnOff, const FString& Confirm)
{
	return FMantleGarmentOutfit::RunForAgent(BodyPath, GarmentPaths, TurnOff, Confirm);
}

/** Registers UMantleToolset with the editor's Toolset Registry, which Unreal's MCP server serves. */
class FMantleAgentToolsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		if (!UToolsetRegistry::IsAvailable())
		{
			UE_LOG(LogMantle, Log, TEXT("Mantle toolset: the Toolset Registry is not available; no tools registered."));
			return;
		}
		if (!UToolsetRegistry::IsToolsetClassRegistered(UMantleToolset::StaticClass()))
		{
			UToolsetRegistry::RegisterToolsetClass(UMantleToolset::StaticClass());
		}
		UE_LOG(LogMantle, Log, TEXT("Mantle toolset registered with the Toolset Registry."));
	}

	virtual void ShutdownModule() override
	{
		FMantleAgentTools::StopWaiting();
		if (UObjectInitialized() && UToolsetRegistry::IsAvailable()
			&& UToolsetRegistry::IsToolsetClassRegistered(UMantleToolset::StaticClass()))
		{
			UToolsetRegistry::UnregisterToolsetClass(UMantleToolset::StaticClass());
		}
	}
};

IMPLEMENT_MODULE(FMantleAgentToolsModule, MantleAgentTools)
