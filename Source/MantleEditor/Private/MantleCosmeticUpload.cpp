// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCosmeticUpload.h"
#include "MantleAssetUploadQueue.h"
#include "MantlePublisher.h"
#include "MantleRuntime.h"
#include "MantleSha256.h"

#include "Dom/JsonObject.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCosmeticUploadPrivate
{
	TSharedPtr<FJsonObject> ParseObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Object))
		{
			return nullptr;
		}
		return Object;
	}

	FString StringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		FString Value;
		if (Object.IsValid())
		{
			Object->TryGetStringField(Field, Value);
		}
		return Value;
	}

	bool BoolField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		bool Value = false;
		if (Object.IsValid())
		{
			Object->TryGetBoolField(Field, Value);
		}
		return Value;
	}

	FString Megabytes(int64 Bytes)
	{
		return Bytes >= 1024 * 1024
			? FString::Printf(TEXT("%.1f MB"), Bytes / (1024.0 * 1024.0))
			: FString::Printf(TEXT("%.0f KB"), FMath::Max(1.0, Bytes / 1024.0));
	}
}

void FMantleApiUploadTransport::Send(const FMantleUploadCallRequest& Request, FMantleApiCallback OnDone)
{
	switch (Request.Call)
	{
	case EMantleUploadCall::DiffAssets:
		FMantleApiClient::DiffAssetsAsync(Request.Hashes, OnDone);
		return;
	case EMantleUploadCall::UploadAsset:
		{
			static const TArray<uint8> Empty;
			FMantleApiClient::UploadAssetAsync(Request.Options, Request.Bytes ? *Request.Bytes : Empty, OnDone);
		}
		return;
	case EMantleUploadCall::GetAsset:
		FMantleApiClient::GetAssetAsync(Request.Id, OnDone);
		return;
	case EMantleUploadCall::CreateItem:
		FMantleApiClient::CreateAvatarItemAsync(Request.Json, Request.IdempotencyKey, OnDone);
		return;
	case EMantleUploadCall::GetItem:
		FMantleApiClient::GetAvatarCreationAsync(Request.Id, OnDone);
		return;
	case EMantleUploadCall::UploadScript:
		{
			// Raw UTF-8 text, as FMantleApiClient::UploadScript sends it.
			const FTCHARToUTF8 Utf8(*Request.Json);
			const TArray<uint8> Source(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
			FMantleApiClient::SendBytesAsync(TEXT("POST"), TEXT("/scripts?language=") + FGenericPlatformHttp::UrlEncode(Request.Language),
				Source, TEXT("text/plain; charset=utf-8"), OnDone);
		}
		return;
	case EMantleUploadCall::CreatePlace:
		FMantleApiClient::SendAsync(TEXT("POST"), TEXT("/places"), Request.Json, OnDone);
		return;
	case EMantleUploadCall::SaveDraft:
		FMantleApiClient::SendAsync(TEXT("PUT"), FString::Printf(TEXT("/places/%s/draft"), *Request.Id), Request.Json, OnDone);
		return;
	case EMantleUploadCall::PublishPlace:
		FMantleApiClient::SendAsync(TEXT("POST"), FString::Printf(TEXT("/places/%s/publish"), *Request.Id), Request.Json, OnDone);
		return;
	case EMantleUploadCall::UpdateItem:
		FMantleApiClient::SendAsync(TEXT("PATCH"), TEXT("/avatar/items/") + FGenericPlatformHttp::UrlEncode(Request.Id), Request.Json, OnDone);
		return;
	case EMantleUploadCall::SetAttributions:
		FMantleApiClient::SendAsync(TEXT("PUT"), TEXT("/assets/attributions"), Request.Json, OnDone);
		return;
	case EMantleUploadCall::CreateLibraryItem:
		{
			// The key names one creation across retries: a repeat returns the item the first made.
			TMap<FString, FString> Headers;
			if (!Request.IdempotencyKey.IsEmpty())
			{
				Headers.Add(FMantleApiClient::IdempotencyKeyHeader, Request.IdempotencyKey);
			}
			FMantleApiClient::SendWithHeadersAsync(TEXT("POST"), TEXT("/library/items"), Request.Json, Headers, OnDone);
		}
		return;
	case EMantleUploadCall::GetLibraryItem:
		FMantleApiClient::SendAsync(TEXT("GET"), TEXT("/library/items/") + FGenericPlatformHttp::UrlEncode(Request.Id), FString(), OnDone);
		return;
	case EMantleUploadCall::CreateLibraryVersion:
		FMantleApiClient::SendAsync(TEXT("POST"), FString::Printf(TEXT("/library/items/%s/versions"), *FGenericPlatformHttp::UrlEncode(Request.Id)), Request.Json, OnDone);
		return;
	case EMantleUploadCall::UpdateLibraryItem:
		FMantleApiClient::SendAsync(TEXT("PATCH"), TEXT("/library/items/") + FGenericPlatformHttp::UrlEncode(Request.Id), Request.Json, OnDone);
		return;
	default:
		{
			FMantleApiResult Unknown;
			Unknown.Message = TEXT("The plugin asked for a call it does not know.");
			OnDone.ExecuteIfBound(Unknown);
		}
		return;
	}
}

FMantleCosmeticUploadSession::FMantleCosmeticUploadSession(FMantleCosmeticPackage InPackage, TSharedRef<IMantleUploadTransport> InTransport,
	FString InResumeFile, TFunction<double()> InClock)
	: Package(MoveTemp(InPackage))
	, Transport(InTransport)
	, ResumeFile(MoveTemp(InResumeFile))
	, Clock(InClock ? MoveTemp(InClock) : TFunction<double()>([] { return FPlatformTime::Seconds(); }))
{
	for (const FMantleCosmeticUploadAsset& Asset : Package.Assets)
	{
		FMantleUploadAssetProgress& Progress = Assets.AddDefaulted_GetRef();
		Progress.Key = Asset.Key;
		Progress.Label = Asset.Label;
		Progress.Message = TEXT("Waiting");
	}
}

FString FMantleCosmeticUploadSession::Fingerprint(const FString& Json)
{
	const FTCHARToUTF8 Utf8(*Json);
	return FMantleSha256::HexDigest(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
}

FString FMantleCosmeticUploadSession::DefaultResumeFile(const FString& SourceAssetPath, const FString& Slot)
{
	FString Name = SourceAssetPath + TEXT("_") + Slot;
	for (TCHAR& Character : Name)
	{
		if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-'))
		{
			Character = TEXT('_');
		}
	}
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Mantle"), TEXT("CosmeticUploads"), Name.Right(120) + TEXT(".json"));
}

bool FMantleCosmeticUploadSession::IsRetryable(const FMantleApiResult& Result)
{
	return Result.StatusCode == 0 || Result.StatusCode == 429 || Result.StatusCode >= 500;
}

FString FMantleCosmeticUploadSession::DescribeFailure(const FMantleApiResult& Result, const FString& What)
{
	const FString& Code = Result.Code;
	FString ApiMessage = Result.Message;
	FString Field;
	if (const TSharedPtr<FJsonObject> Body = MantleCosmeticUploadPrivate::ParseObject(Result.Body))
	{
		Body->TryGetStringField(TEXT("field"), Field);
	}

	if (Result.StatusCode == 401)
	{
		return FMantlePublisher::DescribeApiFailure(Result);
	}
	if (Result.StatusCode == 0)
	{
		return FString::Printf(TEXT("Could not reach Mantle while sending %s. Check your connection; choosing Upload again resumes where this stopped."), *What);
	}
	if (Code == TEXT("creator_token_scope"))
	{
		// Creator tokens are accepted on every upload call, so only an older Mantle server answers this.
		return TEXT("This Mantle server refused your creator token for an avatar item upload; it is likely running an older version of Mantle. ")
			TEXT("Your files are uploaded and kept: choose Upload again later and it continues from here.");
	}
	const FString Moderation = FMantlePublisher::DescribeModerationFailure(Result);
	if (!Moderation.IsEmpty())
	{
		return FString::Printf(TEXT("Mantle refused %s. %s"), *What, *Moderation);
	}
	if (Code == TEXT("claim_taken_down"))
	{
		return FString::Printf(TEXT("A moderator removed %s from your account (usually after a licence complaint), so it cannot be uploaded or used again. Use different content."), *What);
	}
	if (Code == TEXT("assets_not_owned"))
	{
		return FString::Printf(TEXT("%s names a file your account holds no claim on (a moderator removed it, or it was never uploaded from this project). Upload the item again from your own files."), *What);
	}
	if (Code == TEXT("uploaded_differently"))
	{
		return FString::Printf(TEXT("%s: these exact bytes were uploaded before as a different kind of file (for example a texture used as a colour map in one item and as a mask in another), and one upload is one kind. Make a copy of the source asset for the second use."), *What);
	}
	if (Code == TEXT("source_not_ready"))
	{
		return FString::Printf(TEXT("%s waits for its body shape to be approved, and it was not approved in time. Choose Upload again in a minute."), *What);
	}
	if (Code == TEXT("too_large"))
	{
		return FString::Printf(TEXT("%s is too large for Mantle. %s"), *What, *ApiMessage);
	}
	if (Code == TEXT("rate_limited"))
	{
		return FString::Printf(TEXT("Mantle limits how much one account uploads per hour. %s"), *ApiMessage);
	}
	if (Code == TEXT("item_kind_unavailable"))
	{
		return TEXT("The Mantle server this editor talks to cannot read avatar and garment meshes yet: it is older than this plugin. Try again once it is updated.");
	}
	if (Code == TEXT("invalid_request") || Code == TEXT("invalid_options") || Code == TEXT("unsupported_kind") || Code == TEXT("empty_upload")
		|| Code == TEXT("item_field_not_allowed") || Code == TEXT("item_field_required") || Code == TEXT("wrong_asset_kind")
		|| Code == TEXT("invalid_asset_id") || Code == TEXT("material_count_mismatch"))
	{
		return FString::Printf(TEXT("Mantle refused %s: %s%s This is a problem in the plugin, not in your content: please report it with the editor log."),
			*What, *ApiMessage, Field.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Field));
	}
	if (!ApiMessage.IsEmpty())
	{
		return FString::Printf(TEXT("Mantle refused %s: %s%s"), *What, *ApiMessage,
			Field.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Field));
	}
	return FString::Printf(TEXT("Mantle refused %s (HTTP %d)."), *What, Result.StatusCode);
}

FString FMantleCosmeticUploadSession::DescribeUpdateFailure(const FMantleApiResult& Result, const FString& TargetItemId)
{
	if (Result.StatusCode == 404 && Result.Code == TEXT("not_found"))
	{
		return FString::Printf(TEXT("Mantle could not find %s to update; confirm that the item is still in the catalog."),
			*TargetItemId);
	}
	return DescribeFailure(Result, TEXT("the new version"));
}

void FMantleCosmeticUploadSession::AddLog(const FString& Line)
{
	Log.Add(Line);
	UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_UPLOAD %s"), *Line);
}

void FMantleCosmeticUploadSession::Fail(const FString& Why)
{
	Phase = EMantleUploadPhase::Failed;
	Failure = Why;
	AddLog(TEXT("Stopped: ") + Why);
}

void FMantleCosmeticUploadSession::Finish(const FString& Message)
{
	Phase = EMantleUploadPhase::Done;
	// A long garment is published, not refused: the result line says so.
	FinalMessage = bItemMayClipInMotion ? Message + TEXT(" ") + MayClipInMotionNote() : Message;
	AddLog(FinalMessage);
}

FString FMantleCosmeticUploadSession::MayClipInMotionNote()
{
	return TEXT("It hangs below the knee, so Mantle marks it \"may clip in motion\": the legs can show through it when the wearer moves.");
}

int32 FMantleCosmeticUploadSession::FindAsset(const FString& Key) const
{
	return Assets.IndexOfByPredicate([&Key](const FMantleUploadAssetProgress& Progress) { return Progress.Key == Key; });
}

void FMantleCosmeticUploadSession::Start()
{
	if (Phase != EMantleUploadPhase::Idle)
	{
		return;
	}
	for (const FMantleCosmeticIssue& Issue : Package.Issues)
	{
		if (Issue.Severity == EMantleCosmeticSeverity::Error)
		{
			Fail(Issue.ToString());
			return;
		}
	}
	if (Assets.Num() == 0)
	{
		Fail(TEXT("There is nothing to upload."));
		return;
	}
	Phase = EMantleUploadPhase::Checking;
	NextActionAt = 0.0;
	AddLog(FString::Printf(TEXT("Checking which of %d files Mantle already has"), Assets.Num()));
	Tick();
}

void FMantleCosmeticUploadSession::Cancel()
{
	if (IsFinished())
	{
		return;
	}
	++Generation;
	bInFlight = false;
	Phase = EMantleUploadPhase::Cancelled;
	AddLog(TEXT("Cancelled. Everything already uploaded stays on Mantle; Upload again continues from there."));
}

void FMantleCosmeticUploadSession::Send(const FMantleUploadCallRequest& Request)
{
	bInFlight = true;
	InFlightCall = Request.Call;
	const uint32 ForGeneration = Generation;
	const TWeakPtr<FMantleCosmeticUploadSession> Weak = AsShared();
	Transport->Send(Request, FMantleApiCallback::CreateLambda([Weak, ForGeneration](const FMantleApiResult& Result)
	{
		if (const TSharedPtr<FMantleCosmeticUploadSession> Session = Weak.Pin())
		{
			Session->OnResponse(ForGeneration, Result);
		}
	}));
}

void FMantleCosmeticUploadSession::OnResponse(uint32 ForGeneration, const FMantleApiResult& Result)
{
	if (ForGeneration != Generation || !bInFlight || IsFinished())
	{
		return;
	}
	bInFlight = false;
	const int32 AssetIndex = InFlightAsset;
	InFlightAsset = INDEX_NONE;
	switch (InFlightCall)
	{
	case EMantleUploadCall::DiffAssets: HandleDiff(Result); break;
	case EMantleUploadCall::UploadAsset: HandleUpload(AssetIndex, Result); break;
	case EMantleUploadCall::GetAsset: HandleGetAsset(AssetIndex, Result); break;
	case EMantleUploadCall::CreateItem: HandleCreateItem(Result); break;
	case EMantleUploadCall::UpdateItem: HandleCreateItem(Result); break;
	case EMantleUploadCall::GetItem: HandleGetItem(Result); break;
	default: break;
	}
}

bool FMantleCosmeticUploadSession::RetryLater(int32& InOutAttempts, double& OutNextActionAt, const FMantleApiResult& Result, double Now)
{
	if (!IsRetryable(Result) || InOutAttempts >= Timing.RetryDelays.Num())
	{
		return false;
	}
	float Delay = Result.StatusCode == 429 ? Timing.RateLimitedDelay : Timing.RetryDelays[InOutAttempts];
	if (Result.StatusCode == 503)
	{
		// Mantle's "busy with other uploads" and its other 503s say when to come back.
		Delay = FMath::Max(Delay, Result.RetryAfterSeconds);
	}
	++InOutAttempts;
	OutNextActionAt = Now + Delay;
	AddLog(FString::Printf(TEXT("%s; trying again in %.0f s (attempt %d of %d)"),
		Result.StatusCode == 0 ? TEXT("No answer from Mantle") : *FString::Printf(TEXT("Mantle answered %d"), Result.StatusCode),
		Delay, InOutAttempts + 1, Timing.RetryDelays.Num() + 1));
	return true;
}

void FMantleCosmeticUploadSession::Tick()
{
	if (bInFlight || IsFinished() || Phase == EMantleUploadPhase::Idle)
	{
		return;
	}
	const double Now = Clock();
	if (Now < NextActionAt)
	{
		return;
	}

	switch (Phase)
	{
	case EMantleUploadPhase::Checking:
		{
			FMantleUploadCallRequest Request;
			Request.Call = EMantleUploadCall::DiffAssets;
			for (const FMantleCosmeticUploadAsset& Asset : Package.Assets)
			{
				Request.Hashes.AddUnique(Asset.Hash);
			}
			Send(Request);
		}
		return;

	case EMantleUploadPhase::Uploading:
		TickAssets(Now);
		return;

	case EMantleUploadPhase::CreatingItem:
		{
			TMap<FString, FString> Ids;
			for (const FMantleUploadAssetProgress& Progress : Assets)
			{
				Ids.Add(Progress.Key, Progress.AssetId);
			}
			// A new version of an explicitly selected existing item is a PATCH of it.
			const bool bUpdate = !Package.TargetItemId.IsEmpty();
			FString Error;
			if (!(bUpdate ? Package.MakeItemPatchJson(Ids, ItemJson, Error) : Package.MakeItemJson(Ids, ItemJson, Error)))
			{
				Fail(TEXT("The item could not be put together: ") + Error);
				return;
			}
			ItemFingerprint = Fingerprint(ItemJson);

			FString RememberedFingerprint, RememberedItem;
			if (ItemId.IsEmpty() && ReadResume(RememberedFingerprint, RememberedItem) && RememberedFingerprint == ItemFingerprint
				&& !RememberedItem.IsEmpty() && (!bUpdate || RememberedItem == Package.TargetItemId))
			{
				// The same item from the same files was already created (or this version already
				// published) by an earlier run.
				ItemId = RememberedItem;
				bFollowingRemembered = true;
				AddLog(FString::Printf(TEXT("This item was already published (%s); following it"), *ItemId));
				Phase = EMantleUploadPhase::Screening;
				ScreeningSince = Now;
				ItemPolls = 0;
				NextActionAt = Now;
				Tick();
				return;
			}
			if (bUpdate)
			{
				// No idempotency key: a PATCH repeated after a lost answer publishes the same content
				// once more, one version higher, and changes nothing a player sees.
				AddLog(FString::Printf(TEXT("Publishing a new version of %s"), *Package.TargetItemId));
				FMantleUploadCallRequest Request;
				Request.Call = EMantleUploadCall::UpdateItem;
				Request.Id = Package.TargetItemId;
				Request.Json = ItemJson;
				Send(Request);
				return;
			}
			AddLog(TEXT("Creating the item"));
			// One key per item this run creates, the same on every retry: a create that
			// reached Mantle but lost its answer is handed back, not made a second time.
			if (ItemIdempotencyKey.IsEmpty())
			{
				ItemIdempotencyKey = TEXT("mcu1-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
			}
			FMantleUploadCallRequest Request;
			Request.Call = EMantleUploadCall::CreateItem;
			Request.Json = ItemJson;
			Request.IdempotencyKey = ItemIdempotencyKey;
			Send(Request);
		}
		return;

	case EMantleUploadPhase::Screening:
		{
			if (Now - ScreeningSince > Timing.ScreeningTimeout)
			{
				Finish(FString::Printf(TEXT("Published %s. Mantle is still screening it; it appears in the catalog once it passes."), *ItemId));
				return;
			}
			FMantleUploadCallRequest Request;
			Request.Call = EMantleUploadCall::GetItem;
			Request.Id = ItemId;
			Send(Request);
		}
		return;

	default:
		return;
	}
}

void FMantleCosmeticUploadSession::TickAssets(double Now)
{
	// 1. A file ready to send: not sent yet, and its body shape (if any) approved.
	for (int32 Index = 0; Index < Assets.Num(); ++Index)
	{
		FMantleUploadAssetProgress& Progress = Assets[Index];
		if (Progress.State != EMantleUploadAssetState::Waiting || Progress.NextActionAt > Now)
		{
			continue;
		}
		const FMantleCosmeticUploadAsset& Asset = Package.Assets[Index];
		FMantleAssetUploadOptions Options = Asset.Options;
		if (!Asset.SourceKey.IsEmpty())
		{
			const int32 Source = FindAsset(Asset.SourceKey);
			if (!Assets.IsValidIndex(Source))
			{
				Progress.State = EMantleUploadAssetState::Failed;
				Progress.Message = TEXT("names a body shape the package does not have");
				continue;
			}
			if (Assets[Source].State == EMantleUploadAssetState::Failed)
			{
				Progress.State = EMantleUploadAssetState::Failed;
				Progress.Message = FString::Printf(TEXT("needs %s, which failed"), *Assets[Source].Label);
				continue;
			}
			if (Assets[Source].State != EMantleUploadAssetState::Done)
			{
				Progress.Message = FString::Printf(TEXT("Waiting for %s to be approved"), *Assets[Source].Label);
				continue;
			}
			Options.Source = Assets[Source].AssetId;
		}
		Progress.Message = FString::Printf(TEXT("Uploading (%s)"), *MantleCosmeticUploadPrivate::Megabytes(Asset.Bytes.Num()));
		AddLog(FString::Printf(TEXT("Uploading %s (%s)"), *Progress.Label, *MantleCosmeticUploadPrivate::Megabytes(Asset.Bytes.Num())));
		FMantleUploadCallRequest Request;
		Request.Call = EMantleUploadCall::UploadAsset;
		Request.Options = Options;
		Request.Bytes = &Asset.Bytes;
		InFlightAsset = Index;
		Send(Request);
		return;
	}

	// 2. A file being processed whose next look is due.
	for (int32 Index = 0; Index < Assets.Num(); ++Index)
	{
		FMantleUploadAssetProgress& Progress = Assets[Index];
		if (Progress.State != EMantleUploadAssetState::Processing || Progress.NextActionAt > Now)
		{
			continue;
		}
		if (Now - Progress.ProcessingSince > Timing.ProcessingTimeout)
		{
			Progress.State = EMantleUploadAssetState::Failed;
			Progress.Message = TEXT("still processing");
			Fail(FString::Printf(TEXT("Mantle is still processing %s. Nothing is lost: choose Upload again later and it continues from here."), *Progress.Label));
			return;
		}
		FMantleUploadCallRequest Request;
		Request.Call = EMantleUploadCall::GetAsset;
		Request.Id = Progress.AssetId;
		InFlightAsset = Index;
		Send(Request);
		return;
	}

	// 3. Nothing to send now: finished, stuck, or waiting.
	bool bAllDone = true;
	double Earliest = TNumericLimits<double>::Max();
	const FMantleUploadAssetProgress* FirstFailed = nullptr;
	for (const FMantleUploadAssetProgress& Progress : Assets)
	{
		if (Progress.State == EMantleUploadAssetState::Failed)
		{
			FirstFailed = FirstFailed ? FirstFailed : &Progress;
		}
		if (Progress.State != EMantleUploadAssetState::Done)
		{
			bAllDone = false;
		}
		if (Progress.State == EMantleUploadAssetState::Processing
			|| (Progress.State == EMantleUploadAssetState::Waiting && Progress.NextActionAt > Now))
		{
			Earliest = FMath::Min(Earliest, Progress.NextActionAt);
		}
	}
	if (bAllDone)
	{
		AddLog(TEXT("Every file is approved"));
		Phase = EMantleUploadPhase::CreatingItem;
		NextActionAt = Now;
		Attempts = 0;
		Tick();
		return;
	}
	if (FirstFailed)
	{
		Fail(FString::Printf(TEXT("%s: %s"), *FirstFailed->Label, *FirstFailed->Message));
		return;
	}
	if (Timing.WaitLogEvery > 0.0f && Now - LastWaitLogAt >= Timing.WaitLogEvery)
	{
		// A file held for screening or a moderator is otherwise silent: say what the run is waiting on.
		LastWaitLogAt = Now;
		FString Open;
		int32 OpenCount = 0;
		for (const FMantleUploadAssetProgress& Progress : Assets)
		{
			if (Progress.State != EMantleUploadAssetState::Done && ++OpenCount <= 5)
			{
				Open += FString::Printf(TEXT("%s%s (%s)"), Open.IsEmpty() ? TEXT("") : TEXT("; "), *Progress.Label, *Progress.Message);
			}
		}
		AddLog(FString::Printf(TEXT("Still waiting on %d of %d files: %s"), OpenCount, Assets.Num(), *Open));
	}
	NextActionAt = Earliest == TNumericLimits<double>::Max() ? Now + Timing.PollEvery : Earliest;
}

void FMantleCosmeticUploadSession::SchedulePoll(FMantleUploadAssetProgress& Progress, double Now)
{
	const float Delay = Timing.PollDelays.IsValidIndex(Progress.Polls) ? Timing.PollDelays[Progress.Polls] : Timing.PollEvery;
	++Progress.Polls;
	Progress.NextActionAt = Now + Delay;
}

void FMantleCosmeticUploadSession::ApplyAssetStatus(FMantleUploadAssetProgress& Progress, const FMantleUploadedAsset& Asset, double Now)
{
	Progress.AssetId = Asset.Id;
	if (Asset.IsApproved())
	{
		if (Progress.State != EMantleUploadAssetState::Done)
		{
			AddLog(FString::Printf(TEXT("%s approved (%s)"), *Progress.Label, *Asset.Id));
		}
		Progress.State = EMantleUploadAssetState::Done;
		Progress.Message = Progress.bAlreadyOnMantle ? TEXT("Already on Mantle") : TEXT("Approved");
		return;
	}
	if (Asset.IsRejected())
	{
		Progress.State = EMantleUploadAssetState::Failed;
		if (Asset.CookState != TEXT("failed"))
		{
			Progress.Message = TEXT("moderation rejected it.");
		}
		else if (!Asset.CookError.IsEmpty())
		{
			// The cook's own reason, written for the creator (a body shape without warp weights, say).
			Progress.Message = FString::Printf(TEXT("Mantle's processing refused it: %s"), *Asset.CookError);
		}
		else
		{
			Progress.Message = TEXT("Mantle's processing refused it although the plugin's checks passed: a rule the plugin does not know yet. Please report it with the editor log.");
		}
		return;
	}
	if (Progress.State != EMantleUploadAssetState::Processing)
	{
		Progress.State = EMantleUploadAssetState::Processing;
		Progress.ProcessingSince = Now;
		Progress.Polls = 0;
	}
	const FString Waiting = Asset.ModerationState == TEXT("review")
		? FString(TEXT("Waiting for a moderator"))
		: Asset.CookState == TEXT("passed") ? FString(TEXT("Being screened")) : FString(TEXT("Being processed"));
	if (Progress.Message != Waiting)
	{
		AddLog(FString::Printf(TEXT("%s: %s"), *Progress.Label, *Waiting));
	}
	Progress.Message = Waiting;
	SchedulePoll(Progress, Now);
}

void FMantleCosmeticUploadSession::HandleDiff(const FMantleApiResult& Result)
{
	const double Now = Clock();
	if (!Result.bSuccess)
	{
		if (RetryLater(Attempts, NextActionAt, Result, Now))
		{
			return;
		}
		Fail(DescribeFailure(Result, TEXT("the list of files")));
		return;
	}
	TArray<FMantleUploadedAsset> Known;
	TArray<FString> Unknown;
	if (!FMantleApiClient::ParseDiffAssets(Result.Body, Known, Unknown))
	{
		Fail(TEXT("Mantle's answer about which files it has could not be read. Is the API address right (Editor Preferences > Plugins > Mantle)?"));
		return;
	}
	Attempts = 0;
	int32 AlreadyThere = 0;
	for (int32 Index = 0; Index < Assets.Num(); ++Index)
	{
		const FString& Hash = Package.Assets[Index].Hash;
		const FMantleUploadedAsset* Found = Known.FindByPredicate([&Hash](const FMantleUploadedAsset& Asset) { return Asset.Hash == Hash; });
		if (!Found)
		{
			continue;
		}
		// Bytes already uploaded as another kind or for another role (a texture cooked as a
		// mask cannot serve as a colour map) are not this upload. A texture first uploaded with no
		// role keeps its PNG, which serves any role, so that one is fine.
		const FMantleAssetUploadOptions& Wanted = Package.Assets[Index].Options;
		if (Found->Kind != Wanted.Kind || (!Found->CookOptions.Role.IsEmpty() && Found->CookOptions.Role != Wanted.Role))
		{
			Assets[Index].State = EMantleUploadAssetState::Failed;
			Assets[Index].Message = FString::Printf(
				TEXT("these exact bytes are already on Mantle as a %s%s, and one upload is one kind. Make a copy of the source asset for this use."),
				*Found->Kind, Found->CookOptions.Role.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Found->CookOptions.Role));
			continue;
		}
		Assets[Index].bAlreadyOnMantle = true;
		++AlreadyThere;
		ApplyAssetStatus(Assets[Index], *Found, Now);
		if (Assets[Index].State == EMantleUploadAssetState::Processing)
		{
			Assets[Index].NextActionAt = Now;
		}
	}
	AddLog(FString::Printf(TEXT("Mantle already has %d of %d files; %d to upload"), AlreadyThere, Assets.Num(), Assets.Num() - AlreadyThere));
	for (const FMantleUploadAssetProgress& Progress : Assets)
	{
		if (Progress.State == EMantleUploadAssetState::Failed)
		{
			Fail(FString::Printf(TEXT("%s: %s"), *Progress.Label, *Progress.Message));
			return;
		}
	}
	Phase = EMantleUploadPhase::Uploading;
	LastWaitLogAt = Now;
	NextActionAt = Now;
}

void FMantleCosmeticUploadSession::HandleUpload(int32 AssetIndex, const FMantleApiResult& Result)
{
	const double Now = Clock();
	if (!Assets.IsValidIndex(AssetIndex))
	{
		return;
	}
	FMantleUploadAssetProgress& Progress = Assets[AssetIndex];
	if (!Result.bSuccess)
	{
		if (Result.Code == TEXT("source_not_ready") && Progress.Attempts < Timing.RetryDelays.Num())
		{
			Progress.NextActionAt = Now + Timing.RetryDelays[Progress.Attempts++];
			Progress.Message = TEXT("Waiting for its body shape");
			return;
		}
		if (FMantleAssetUploadQueue::IsUploadBusy(Result))
		{
			// Mantle is taking all the large uploads it allows at once: not a failure, so no attempt is
			// spent. The file waits for its turn, for as long as the server asks, up to Timing.MaxBusyWait.
			const float Jitter = Timing.BusyJitter > 0.0f ? FMath::FRand() * Timing.BusyJitter : 0.0f;
			const float Wait = FMath::Max(Result.RetryAfterSeconds, Timing.BusyMinWait) + Jitter;
			if (Progress.BusyWaited + Wait <= Timing.MaxBusyWait)
			{
				if (Progress.BusyWaited <= 0.0)
				{
					AddLog(FString::Printf(TEXT("Mantle is busy with other uploads; %s waits for its turn"), *Progress.Label));
				}
				Progress.BusyWaited += Wait;
				Progress.NextActionAt = Now + Wait;
				Progress.Message = TEXT("Mantle is busy with other uploads; waiting for a turn");
				return;
			}
		}
		if (RetryLater(Progress.Attempts, Progress.NextActionAt, Result, Now))
		{
			Progress.Message = TEXT("Retrying");
			return;
		}
		Progress.State = EMantleUploadAssetState::Failed;
		Progress.Message = DescribeFailure(Result, Progress.Label);
		Fail(Progress.Message);
		return;
	}
	FMantleUploadedAsset Asset;
	if (!FMantleApiClient::ParseUploadedAsset(Result.Body, Asset))
	{
		Progress.State = EMantleUploadAssetState::Failed;
		Progress.Message = TEXT("Mantle stored it but its answer could not be read.");
		Fail(FString::Printf(TEXT("%s: %s"), *Progress.Label, *Progress.Message));
		return;
	}
	Progress.Attempts = 0;
	AddLog(FString::Printf(TEXT("%s %s (%s)"), *Progress.Label, Asset.bCreated ? TEXT("uploaded") : TEXT("was already on Mantle; claimed"), *Asset.Id));
	ApplyAssetStatus(Progress, Asset, Now);
	if (Progress.State == EMantleUploadAssetState::Failed)
	{
		Fail(FString::Printf(TEXT("%s: %s"), *Progress.Label, *Progress.Message));
	}
}

void FMantleCosmeticUploadSession::HandleGetAsset(int32 AssetIndex, const FMantleApiResult& Result)
{
	const double Now = Clock();
	if (!Assets.IsValidIndex(AssetIndex))
	{
		return;
	}
	FMantleUploadAssetProgress& Progress = Assets[AssetIndex];
	if (!Result.bSuccess)
	{
		if (IsRetryable(Result))
		{
			SchedulePoll(Progress, Now);
			return;
		}
		Progress.State = EMantleUploadAssetState::Failed;
		Progress.Message = DescribeFailure(Result, Progress.Label);
		Fail(Progress.Message);
		return;
	}
	FMantleUploadedAsset Asset;
	if (!FMantleApiClient::ParseUploadedAsset(Result.Body, Asset))
	{
		SchedulePoll(Progress, Now);
		return;
	}
	ApplyAssetStatus(Progress, Asset, Now);
	if (Progress.State == EMantleUploadAssetState::Failed)
	{
		Fail(FString::Printf(TEXT("%s: %s"), *Progress.Label, *Progress.Message));
	}
}

void FMantleCosmeticUploadSession::HandleCreateItem(const FMantleApiResult& Result)
{
	const double Now = Clock();
	if (!Result.bSuccess)
	{
		// An asset approved a moment ago can race the item check: worth one more look.
		if (Result.Code == TEXT("asset_unavailable") && Attempts < 2)
		{
			NextActionAt = Now + Timing.RetryDelays[FMath::Min(Attempts, Timing.RetryDelays.Num() - 1)];
			++Attempts;
			return;
		}
		if (RetryLater(Attempts, NextActionAt, Result, Now))
		{
			return;
		}
		if (!Package.TargetItemId.IsEmpty())
		{
			Fail(DescribeUpdateFailure(Result, Package.TargetItemId));
			return;
		}
		Fail(DescribeFailure(Result, TEXT("the item")));
		return;
	}
	const TSharedPtr<FJsonObject> Creation = MantleCosmeticUploadPrivate::ParseObject(Result.Body);
	ItemId = MantleCosmeticUploadPrivate::StringField(Creation, TEXT("id"));
	if (ItemId.IsEmpty())
	{
		Fail(TEXT("Mantle created the item but did not say which one. Look for it under your creations in the game."));
		return;
	}
	ItemStatus = MantleCosmeticUploadPrivate::StringField(Creation, TEXT("status"));
	ItemModerationState = MantleCosmeticUploadPrivate::StringField(Creation, TEXT("moderationState"));
	bItemMayClipInMotion = MantleCosmeticUploadPrivate::BoolField(Creation, TEXT("mayClipInMotion"));
	WriteResume(ItemFingerprint, ItemId);
	Phase = EMantleUploadPhase::Screening;
	ScreeningSince = Now;
	ItemPolls = 0;
	Attempts = 0;
	if (!Package.TargetItemId.IsEmpty())
	{
		// An existing item stays listed while its new version is screened.
		double Version = 0.0;
		if (Creation.IsValid())
		{
			Creation->TryGetNumberField(TEXT("version"), Version);
		}
		AddLog(FString::Printf(TEXT("New version published: %s version %d (%s)"), *ItemId, static_cast<int32>(Version), *ItemStatus));
		if (ItemStatus == TEXT("approved"))
		{
			Finish(FString::Printf(TEXT("Published version %d of %s. Players receive the update after Mantle approves it."),
				static_cast<int32>(Version), *ItemId));
			return;
		}
		NextActionAt = Now + (Timing.PollDelays.Num() > 0 ? Timing.PollDelays[0] : Timing.PollEvery);
		return;
	}
	AddLog(FString::Printf(TEXT("Item created: %s (%s)"), *ItemId, *ItemStatus));
	if (ItemStatus == TEXT("approved"))
	{
		Finish(FString::Printf(TEXT("Published %s. Players can find it in the catalog now."), *ItemId));
		return;
	}
	NextActionAt = Now + (Timing.PollDelays.Num() > 0 ? Timing.PollDelays[0] : Timing.PollEvery);
}

void FMantleCosmeticUploadSession::HandleGetItem(const FMantleApiResult& Result)
{
	const double Now = Clock();
	if (!Result.bSuccess)
	{
		if (Result.StatusCode == 404 && Result.Code == TEXT("not_found"))
		{
			// The remembered item is gone (deleted with its account, say): create it again.
			ForgetResume();
			AddLog(FString::Printf(TEXT("The item %s no longer exists; creating it again"), *ItemId));
			ItemId.Reset();
			Phase = EMantleUploadPhase::CreatingItem;
			NextActionAt = Now;
			return;
		}
		if (IsRetryable(Result))
		{
			NextActionAt = Now + Timing.PollEvery;
			return;
		}
		Fail(DescribeFailure(Result, TEXT("the item's status")));
		return;
	}
	const TSharedPtr<FJsonObject> Creation = MantleCosmeticUploadPrivate::ParseObject(Result.Body);
	ItemStatus = MantleCosmeticUploadPrivate::StringField(Creation, TEXT("status"));
	ItemModerationState = MantleCosmeticUploadPrivate::StringField(Creation, TEXT("moderationState"));
	bItemMayClipInMotion = MantleCosmeticUploadPrivate::BoolField(Creation, TEXT("mayClipInMotion"));
	if (ItemStatus == TEXT("approved"))
	{
		Finish(FString::Printf(TEXT("Published %s. Players can find it in the catalog now."), *ItemId));
		return;
	}
	if (ItemStatus == TEXT("rejected"))
	{
		Fail(FString::Printf(TEXT("Screening rejected the item %s (its thumbnail, name or description). Change what was flagged and upload again."), *ItemId));
		return;
	}
	if (ItemStatus == TEXT("delisted"))
	{
		if (bFollowingRemembered && Package.TargetItemId.IsEmpty())
		{
			// An earlier run's item that has been delisted since: publish a new one in this run.
			ForgetResume();
			AddLog(FString::Printf(TEXT("The item %s was delisted; creating a new one"), *ItemId));
			ItemId.Reset();
			ItemIdempotencyKey.Reset();
			bFollowingRemembered = false;
			Phase = EMantleUploadPhase::CreatingItem;
			NextActionAt = Now;
			return;
		}
		Fail(FString::Printf(TEXT("The item %s was delisted. Upload it again to publish a new one."), *ItemId));
		ForgetResume();
		return;
	}
	if (ItemModerationState == TEXT("review"))
	{
		Finish(FString::Printf(TEXT("Published %s. A moderator will look at it; it appears in the catalog once approved."), *ItemId));
		return;
	}
	const float Delay = Timing.PollDelays.IsValidIndex(ItemPolls) ? Timing.PollDelays[ItemPolls] : Timing.PollEvery;
	++ItemPolls;
	NextActionAt = Now + Delay;
}

float FMantleCosmeticUploadSession::GetProgress() const
{
	switch (Phase)
	{
	case EMantleUploadPhase::Idle: return 0.0f;
	case EMantleUploadPhase::Checking: return 0.02f;
	case EMantleUploadPhase::CreatingItem: return 0.85f;
	case EMantleUploadPhase::Screening: return 0.92f;
	case EMantleUploadPhase::Done: return 1.0f;
	default: break;
	}
	float Sum = 0.0f;
	for (const FMantleUploadAssetProgress& Progress : Assets)
	{
		Sum += Progress.State == EMantleUploadAssetState::Done ? 1.0f : Progress.State == EMantleUploadAssetState::Processing ? 0.6f : 0.0f;
	}
	return 0.05f + 0.8f * (Assets.Num() > 0 ? Sum / Assets.Num() : 0.0f);
}

FString FMantleCosmeticUploadSession::GetStatusText() const
{
	switch (Phase)
	{
	case EMantleUploadPhase::Idle: return TEXT("Ready");
	case EMantleUploadPhase::Checking: return TEXT("Checking which files Mantle already has...");
	case EMantleUploadPhase::Uploading:
		{
			int32 Done = 0;
			for (const FMantleUploadAssetProgress& Progress : Assets)
			{
				Done += Progress.State == EMantleUploadAssetState::Done ? 1 : 0;
			}
			return FString::Printf(TEXT("Uploading and processing files: %d of %d ready..."), Done, Assets.Num());
		}
	case EMantleUploadPhase::CreatingItem: return TEXT("Creating the item...");
	case EMantleUploadPhase::Screening: return TEXT("Screening the item...");
	case EMantleUploadPhase::Done: return FinalMessage;
	case EMantleUploadPhase::Failed: return Failure;
	case EMantleUploadPhase::Cancelled: return TEXT("Cancelled. Upload again to continue where it stopped.");
	default: return FString();
	}
}

bool FMantleCosmeticUploadSession::ReadResume(FString& OutFingerprint, FString& OutItemId) const
{
	FString Text;
	if (ResumeFile.IsEmpty() || !FFileHelper::LoadFileToString(Text, *ResumeFile))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Object = MantleCosmeticUploadPrivate::ParseObject(Text);
	OutFingerprint = MantleCosmeticUploadPrivate::StringField(Object, TEXT("fingerprint"));
	OutItemId = MantleCosmeticUploadPrivate::StringField(Object, TEXT("itemId"));
	return !OutFingerprint.IsEmpty();
}

void FMantleCosmeticUploadSession::WriteResume(const FString& InFingerprint, const FString& InItemId) const
{
	if (ResumeFile.IsEmpty())
	{
		return;
	}
	FString Text;
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("fingerprint"), InFingerprint);
	Object->SetStringField(TEXT("itemId"), InItemId);
	Object->SetStringField(TEXT("slot"), Package.Slot);
	Object->SetStringField(TEXT("name"), Package.Name);
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Object, Writer);
	FFileHelper::SaveStringToFile(Text, *ResumeFile);
}

void FMantleCosmeticUploadSession::ForgetResume() const
{
	if (!ResumeFile.IsEmpty())
	{
		IFileManager::Get().Delete(*ResumeFile, false, false, true);
	}
}
