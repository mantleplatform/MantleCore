// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAssetUploadQueue.h"
#include "MantleApiClient.h"
#include "MantlePublisher.h"
#include "MantleRuntime.h"
#include "MantleSha256.h"

#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleAssetUploadQueuePrivate
{
	FString Size(int64 Bytes)
	{
		return Bytes >= 1024 * 1024
			? FString::Printf(TEXT("%.1f MB"), Bytes / (1024.0 * 1024.0))
			: FString::Printf(TEXT("%.0f KB"), FMath::Max(1.0, Bytes / 1024.0));
	}

	FString Minutes(double Seconds)
	{
		const int32 Whole = FMath::Max(1, FMath::CeilToInt(Seconds / 60.0));
		return FString::Printf(TEXT("%d minute%s"), Whole, Whole == 1 ? TEXT("") : TEXT("s"));
	}

	/** The server's rule (assets.go): one hash is one row; a texture cooked for a role serves only that role. */
	bool FitsUpload(const FMantleUploadedAsset& Asset, const FMantleAssetUploadOptions& Wanted)
	{
		if (!Asset.Kind.IsEmpty() && Asset.Kind != Wanted.Kind)
		{
			return false;
		}
		return Asset.CookOptions.Role.IsEmpty() || Asset.CookOptions.Role == Wanted.Role;
	}

	FString RoleWords(const FString& Role)
	{
		if (Role == TEXT("color")) return TEXT("a colour map");
		if (Role == TEXT("normal")) return TEXT("a normal map");
		if (Role == TEXT("data")) return TEXT("a data map (roughness, metal, occlusion)");
		return TEXT("another use");
	}
}

FMantleAssetUploadQueue::FMantleAssetUploadQueue(TSharedRef<IMantleUploadTransport> InTransport, TFunction<double()> InClock)
	: Transport(InTransport)
	, Clock(InClock ? MoveTemp(InClock) : TFunction<double()>([] { return FPlatformTime::Seconds(); }))
{
}

// ---------------------------------------------------------------------------------------------
// Sentences

bool FMantleAssetUploadQueue::IsRunLevelFailure(const FMantleApiResult& Result)
{
	return Result.StatusCode == 401
		|| Result.Code == TEXT("creator_token_scope")
		|| Result.Code == TEXT("not_configured")
		|| ((Result.StatusCode == 404 || Result.StatusCode == 405) && Result.Code.IsEmpty());
}

bool FMantleAssetUploadQueue::IsUploadBusy(const FMantleApiResult& Result)
{
	return Result.StatusCode == 503 && (Result.Code == TEXT("upload_busy") || Result.RetryAfterSeconds > 0.0f);
}

FString FMantleAssetUploadQueue::DescribeRunFailure(const FMantleApiResult& Result, const FString& While)
{
	if (Result.StatusCode == 401)
	{
		return FMantlePublisher::DescribeApiFailure(Result);
	}
	if (Result.StatusCode == 0)
	{
		return FString::Printf(TEXT("Could not reach Mantle while %s. Check your connection, then choose Publish again: ")
			TEXT("it continues where this stopped, and nothing is uploaded twice."), *While);
	}
	if (Result.Code == TEXT("creator_token_scope"))
	{
		return TEXT("Your creator token is not allowed to upload assets. Create a new creator token in Mantle's Settings > Account and sign in with it.");
	}
	if (Result.Code == TEXT("not_configured"))
	{
		return TEXT("This Mantle server cannot take uploads right now (its storage is not set up). Try again later.");
	}
	if ((Result.StatusCode == 404 || Result.StatusCode == 405) && Result.Code.IsEmpty())
	{
		return FString::Printf(TEXT("The Mantle address %s did not answer like the Mantle API while %s. Check Editor Preferences > Plugins > Mantle."),
			*FMantleApiClient::BaseUrlResolver(), *While);
	}
	if (Result.StatusCode >= 500)
	{
		return FString::Printf(TEXT("Mantle is having trouble right now (it answered %d while %s). Everything uploaded so far is kept: ")
			TEXT("choose Publish again in a few minutes and it continues from here."), Result.StatusCode, *While);
	}
	if (!Result.Message.IsEmpty())
	{
		return FString::Printf(TEXT("Mantle stopped the upload while %s: %s"), *While, *Result.Message);
	}
	return FString::Printf(TEXT("Mantle stopped the upload while %s (HTTP %d)."), *While, Result.StatusCode);
}

FString FMantleAssetUploadQueue::DescribeRefusal(const FMantleApiResult& Result, const FString& Name, FString& OutCode)
{
	OutCode = Result.Code.IsEmpty() ? FString::Printf(TEXT("http_%d"), Result.StatusCode) : Result.Code;
	const FString& Code = Result.Code;
	const FString Moderation = FMantlePublisher::DescribeModerationFailure(Result);
	if (!Moderation.IsEmpty())
	{
		return FString::Printf(TEXT("Mantle refused '%s'. %s"), *Name, *Moderation);
	}
	if (Code == TEXT("claim_taken_down"))
	{
		return FString::Printf(TEXT("'%s' was removed from your account by a Mantle moderator (usually after a licence complaint), ")
			TEXT("so it cannot be uploaded or used again. Remove it from the level or replace it, then Publish again."), *Name);
	}
	if (Code == TEXT("uploaded_differently"))
	{
		return FString::Printf(TEXT("'%s' has exactly the same pixels as an image already on Mantle for another use, and Mantle prepares ")
			TEXT("each image for one use only."), *Name);
	}
	if (Code == TEXT("too_large"))
	{
		return FString::Printf(TEXT("'%s' is too large for Mantle (%s). Lower its resolution or simplify it, then Publish again."),
			*Name, *Result.Message.TrimChar(TEXT('.')));
	}
	if (Code == TEXT("assets_not_owned"))
	{
		return FString::Printf(TEXT("'%s' uses a texture your account holds no claim on (a moderator removed it). Replace that texture, then Publish again."), *Name);
	}
	if (Code == TEXT("invalid_options") || Code == TEXT("unsupported_kind") || Code == TEXT("empty_upload") || Code == TEXT("invalid_request")
		|| Code == TEXT("source_not_ready") || Code == TEXT("wrong_asset_kind") || Code == TEXT("invalid_asset_id"))
	{
		return FString::Printf(TEXT("Mantle refused '%s': %s This is a problem in the plugin, not in your content: please report it with the editor log."),
			*Name, *Result.Message);
	}
	if (!Result.Message.IsEmpty())
	{
		return FString::Printf(TEXT("Mantle refused '%s': %s"), *Name, *Result.Message);
	}
	return FString::Printf(TEXT("Mantle refused '%s' (HTTP %d)."), *Name, Result.StatusCode);
}

FString FMantleAssetUploadQueue::DescribeRejection(const FMantleUploadedAsset& Asset, const FString& Name)
{
	if (Asset.CookState == TEXT("failed"))
	{
		return Asset.CookError.IsEmpty()
			? FString::Printf(TEXT("Mantle could not process '%s', although the plugin's checks passed: a rule the plugin does not know yet. ")
				TEXT("Please report it with the editor log."), *Name)
			: FString::Printf(TEXT("Mantle could not process '%s': %s. Fix it as that says, or replace it, then Publish again."),
				*Name, *Asset.CookError.TrimChar(TEXT('.')));
	}
	return FString::Printf(TEXT("'%s' did not pass Mantle's content screening, so a place cannot go live with it. ")
		TEXT("Replace it or remove what uses it, then Publish again."), *Name);
}

// ---------------------------------------------------------------------------------------------
// Running

void FMantleAssetUploadQueue::AddLog(const FString& Line)
{
	Log.Add(Line);
	UE_LOG(LogMantle, Log, TEXT("MANTLE_F4 %s"), *Line);
}

void FMantleAssetUploadQueue::Fail(const FString& Why)
{
	if (IsFinished())
	{
		return;
	}
	++Generation;
	InFlight = 0;
	bDiffInFlight = false;
	Outcome = EMantleUploadQueueOutcome::Failed;
	Failure = Why;
	AddLog(TEXT("Stopped: ") + Why);
}

const FMantleQueuedUploadStatus* FMantleAssetUploadQueue::FindStatus(const FString& Key) const
{
	return Statuses.FindByPredicate([&Key](const FMantleQueuedUploadStatus& Status) { return Status.Key == Key; });
}

void FMantleAssetUploadQueue::Start(TArray<FMantleQueuedUpload> InItems)
{
	if (Outcome != EMantleUploadQueueOutcome::Idle)
	{
		return;
	}
	Outcome = EMantleUploadQueueOutcome::Running;
	Items.Reset(InItems.Num());
	Statuses.Reset(InItems.Num());
	for (FMantleQueuedUpload& Upload : InItems)
	{
		FItem& Item = Items.AddDefaulted_GetRef();
		Item.Upload = MoveTemp(Upload);
		FMantleQueuedUploadStatus& Status = Statuses.AddDefaulted_GetRef();
		Status.Key = Item.Upload.Key;
		Status.Name = Item.Upload.Name;
		Status.Stage = Item.Upload.Stage;
		Status.Message = TEXT("Waiting");
		Stages.AddUnique(Item.Upload.Stage);
		Progress.AssetsTotal += 1;

		// Bytes known up front are hashed and diffed at once, whatever their stage: an unchanged
		// republish then learns everything it needs from the first diff.
		if (!Item.Upload.Prepare)
		{
			PrepareItem(Items.Num() - 1);
		}
	}
	Stages.Sort();
	AddLog(FString::Printf(TEXT("%d file(s) in %d stage(s)"), Items.Num(), Stages.Num()));
	UpdateProgress();
	Tick();
}

bool FMantleAssetUploadQueue::PrepareItem(int32 Index)
{
	FItem& Item = Items[Index];
	FMantleQueuedUploadStatus& Status = Statuses[Index];
	if (Item.bPrepared)
	{
		return true;
	}
	Item.bPrepared = true;
	if (Item.Upload.Prepare)
	{
		FString WhyNot;
		if (!Item.Upload.Prepare(Item.Upload, WhyNot))
		{
			Refuse(Index, TEXT("blocked"), WhyNot.IsEmpty() ? FString::Printf(TEXT("'%s' could not be prepared."), *Item.Upload.Name) : WhyNot);
			return false;
		}
	}
	const bool bFromFile = Item.Upload.Bytes.Num() == 0 && !Item.Upload.File.IsEmpty();
	if (Item.Upload.Hash.IsEmpty())
	{
		if (bFromFile)
		{
			// A spilled file with no hash given: read once, here, to name it.
			TArray<uint8> Contents;
			if (!FFileHelper::LoadFileToArray(Contents, *Item.Upload.File))
			{
				Refuse(Index, TEXT("unreadable"), FString::Printf(
					TEXT("'%s' was prepared, but the file it was kept in is gone, so it could not be sent."), *Item.Upload.Name));
				return false;
			}
			Item.Upload.Hash = FMantleSha256::AssetHash(Contents);
		}
		else
		{
			Item.Upload.Hash = FMantleSha256::AssetHash(Item.Upload.Bytes);
		}
	}
	Status.Hash = Item.Upload.Hash;
	Status.Bytes = bFromFile ? Item.Upload.FileSize : Item.Upload.Bytes.Num();
	Progress.BytesTotal += Status.Bytes;
	Item.bNeedsDiff = true;
	return true;
}

bool FMantleAssetUploadQueue::IsStageSettled(int32 Stage) const
{
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		if (Items[Index].Upload.Stage == Stage && !Statuses[Index].HasId() && Statuses[Index].State != EMantleQueuedUploadState::Refused)
		{
			return false;
		}
	}
	return true;
}

void FMantleAssetUploadQueue::OpenStages()
{
	while (IsRunning() && Stages.IsValidIndex(OpenStageIndex + 1))
	{
		if (OpenStageIndex != INDEX_NONE && !IsStageSettled(Stages[OpenStageIndex]))
		{
			return;
		}
		++OpenStageIndex;
		const int32 Stage = Stages[OpenStageIndex];
		int32 Count = 0;
		for (int32 Index = 0; Index < Items.Num(); ++Index)
		{
			if (Items[Index].Upload.Stage == Stage)
			{
				Items[Index].bOpen = true;
				PrepareItem(Index);
				++Count;
			}
		}
		AddLog(FString::Printf(TEXT("Stage %d open: %d file(s)"), Stage, Count));
		UpdateProgress();
	}
}

void FMantleAssetUploadQueue::Tick()
{
	if (!IsRunning())
	{
		return;
	}
	OpenStages();
	if (!IsRunning())
	{
		return;
	}
	const double Now = Clock();
	while (IsRunning() && InFlight < FMath::Max(1, Settings.MaxConcurrentRequests))
	{
		if (!bDiffInFlight && Now >= NextDiffAt && (SendCheck() || SendPoll(Now)))
		{
			continue;
		}
		if (SendUpload(Now))
		{
			continue;
		}
		break;
	}
	CheckFinished();
}

void FMantleAssetUploadQueue::Cancel()
{
	if (IsFinished())
	{
		return;
	}
	++Generation;
	InFlight = 0;
	bDiffInFlight = false;
	Outcome = EMantleUploadQueueOutcome::Cancelled;
	AddLog(TEXT("Cancelled. Everything already uploaded stays on Mantle; the next Publish continues from there."));
}

bool FMantleAssetUploadQueue::IsHashBusy(const FString& Hash, int32 Except) const
{
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		if (Index != Except && Items[Index].bInFlight && Items[Index].Upload.Hash == Hash)
		{
			return true;
		}
	}
	return false;
}

bool FMantleAssetUploadQueue::SendCheck()
{
	TArray<int32> Batch;
	TSet<FString> Hashes;
	FMantleUploadCallRequest Request;
	Request.Call = EMantleUploadCall::DiffAssets;
	for (int32 Index = 0; Index < Items.Num() && Request.Hashes.Num() < FMath::Max(1, Settings.DiffBatchSize); ++Index)
	{
		FItem& Item = Items[Index];
		if (!Item.bNeedsDiff || Item.bInFlight)
		{
			continue;
		}
		if (!Hashes.Contains(Item.Upload.Hash))
		{
			Hashes.Add(Item.Upload.Hash);
			Request.Hashes.Add(Item.Upload.Hash);
		}
		Batch.Add(Index);
	}
	// Files with the same bytes as one in this batch ride along even past the batch size.
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		if (Items[Index].bNeedsDiff && !Items[Index].bInFlight && !Batch.Contains(Index) && Hashes.Contains(Items[Index].Upload.Hash))
		{
			Batch.Add(Index);
		}
	}
	if (Batch.Num() == 0)
	{
		return false;
	}
	for (const int32 Index : Batch)
	{
		Items[Index].bNeedsDiff = false;
		Items[Index].bInFlight = true;
		Statuses[Index].Message = TEXT("Checking whether Mantle already has it");
	}
	Send(Request, ERequestKind::Check, MoveTemp(Batch));
	return true;
}

bool FMantleAssetUploadQueue::SendPoll(double Now)
{
	if (!Settings.bWaitForApproval || NextPollAt <= 0.0 || Now < NextPollAt)
	{
		return false;
	}
	TArray<int32> Batch;
	TSet<FString> Hashes;
	FMantleUploadCallRequest Request;
	Request.Call = EMantleUploadCall::DiffAssets;
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		if (Statuses[Index].State != EMantleQueuedUploadState::Processing || Items[Index].bInFlight)
		{
			continue;
		}
		const FString& Hash = Items[Index].Upload.Hash;
		if (!Hashes.Contains(Hash))
		{
			if (Request.Hashes.Num() >= FMath::Max(1, Settings.DiffBatchSize))
			{
				continue;
			}
			Hashes.Add(Hash);
			Request.Hashes.Add(Hash);
		}
		Batch.Add(Index);
	}
	if (Batch.Num() == 0)
	{
		NextPollAt = 0.0;
		return false;
	}
	for (const int32 Index : Batch)
	{
		Items[Index].bInFlight = true;
	}
	Send(Request, ERequestKind::Poll, MoveTemp(Batch));
	return true;
}

bool FMantleAssetUploadQueue::SendUpload(double Now)
{
	if (Now < PausedUntil)
	{
		return false;
	}
	// Large uploads already out: Mantle lets one creator hold only so many at once (an item in flight that needs its upload is one).
	int32 LargeInFlight = 0;
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		LargeInFlight += Items[Index].bInFlight && Items[Index].bNeedsUpload && Statuses[Index].Bytes >= Settings.LargeUploadBytes ? 1 : 0;
	}
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		FItem& Item = Items[Index];
		if (!Item.bOpen || !Item.bNeedsUpload || Item.bInFlight || Item.NextActionAt > Now || IsHashBusy(Item.Upload.Hash, Index))
		{
			continue;
		}
		if (Statuses[Index].Bytes >= Settings.LargeUploadBytes && LargeInFlight >= FMath::Max(1, Settings.MaxConcurrentLargeUploads))
		{
			continue; // a small one further on may still go
		}
		FMantleQueuedUploadStatus& Status = Statuses[Index];
		// A spilled file is read now and its bytes let go again when the answer comes.
		const TArray<uint8>* Payload = &Item.Upload.Bytes;
		if (Item.Upload.Bytes.Num() == 0 && !Item.Upload.File.IsEmpty())
		{
			// The size and the hash must be what was written: the file is Mantle's content address, and a file someone else
			// touched would be stored under the hash of other bytes.
			if (!FFileHelper::LoadFileToArray(Item.SendBuffer, *Item.Upload.File) || Item.SendBuffer.Num() != Item.Upload.FileSize
				|| FMantleSha256::AssetHash(Item.SendBuffer) != Item.Upload.Hash)
			{
				Item.SendBuffer.Empty();
				Refuse(Index, TEXT("unreadable"), FString::Printf(
					TEXT("'%s' was prepared, but the file it was kept in is gone or changed, so it could not be sent."), *Item.Upload.Name));
				UpdateProgress();
				continue;
			}
			Payload = &Item.SendBuffer;
		}
		Status.State = EMantleQueuedUploadState::Uploading;
		Status.Message = FString::Printf(TEXT("Uploading (%s)"), *MantleAssetUploadQueuePrivate::Size(Status.Bytes));
		Progress.CurrentName = Item.Upload.Name;
		Item.bInFlight = true;
		FMantleUploadCallRequest Request;
		Request.Call = EMantleUploadCall::UploadAsset;
		Request.Options = Item.Upload.Options;
		Request.Bytes = Payload;
		Send(Request, ERequestKind::Upload, { Index });
		UpdateProgress();
		return true;
	}
	return false;
}

void FMantleAssetUploadQueue::Send(const FMantleUploadCallRequest& Request, ERequestKind Kind, TArray<int32> Indices)
{
	++InFlight;
	if (Kind != ERequestKind::Upload)
	{
		bDiffInFlight = true;
	}
	const uint32 ForGeneration = Generation;
	const TWeakPtr<FMantleAssetUploadQueue> Weak = AsShared();
	Transport->Send(Request, FMantleApiCallback::CreateLambda([Weak, ForGeneration, Kind, Indices](const FMantleApiResult& Result)
	{
		if (const TSharedPtr<FMantleAssetUploadQueue> Queue = Weak.Pin())
		{
			Queue->OnResponse(ForGeneration, Kind, Indices, Result);
		}
	}));
}

void FMantleAssetUploadQueue::OnResponse(uint32 ForGeneration, ERequestKind Kind, const TArray<int32>& Indices, const FMantleApiResult& Result)
{
	if (ForGeneration != Generation || !IsRunning())
	{
		return;
	}
	InFlight = FMath::Max(0, InFlight - 1);
	for (const int32 Index : Indices)
	{
		if (Items.IsValidIndex(Index))
		{
			Items[Index].bInFlight = false;
			// The request is answered: a spilled file's bytes read for it go (a retry reads the file again).
			Items[Index].SendBuffer.Empty();
		}
	}
	if (Kind == ERequestKind::Upload)
	{
		HandleUpload(Indices[0], Result);
	}
	else
	{
		bDiffInFlight = false;
		HandleDiff(Kind, Indices, Result);
	}
	UpdateProgress();
	// Answers can arrive while the caller is not ticking (a blocking publish pumps HTTP): keep
	// the pool full from here too.
	Tick();
}

bool FMantleAssetUploadQueue::Backoff(int32& InOutAttempts, double& OutNextAt, const FMantleApiResult& Result, double Now)
{
	if (InOutAttempts >= Settings.RetryDelays.Num())
	{
		return false;
	}
	const float Delay = FMath::Max(Settings.RetryDelays[InOutAttempts], Result.RetryAfterSeconds);
	++InOutAttempts;
	OutNextAt = Now + Delay;
	AddLog(FString::Printf(TEXT("%s; trying again in %.0f s (attempt %d of %d)"),
		Result.StatusCode == 0 ? TEXT("No answer from Mantle") : *FString::Printf(TEXT("Mantle answered %d"), Result.StatusCode),
		Delay, InOutAttempts + 1, Settings.RetryDelays.Num() + 1));
	return true;
}

void FMantleAssetUploadQueue::PauseForRateLimit(const FMantleApiResult& Result, double Now)
{
	const float Wait = Result.RetryAfterSeconds > 0.0f ? Result.RetryAfterSeconds : Settings.RateLimitedDelay;
	if (Wait > Settings.MaxRetryAfter)
	{
		Fail(FString::Printf(TEXT("Mantle limits how much one account uploads per hour, and this account can upload again in about %s. ")
			TEXT("Everything uploaded so far is kept: choose Publish again then and it continues from here."),
			*MantleAssetUploadQueuePrivate::Minutes(Wait)));
		return;
	}
	const double Until = Now + Wait;
	if (Until > PausedUntil)
	{
		PausedUntil = Until;
		AddLog(FString::Printf(TEXT("Mantle's hourly upload limit reached; uploads resume in %.0f s"), Wait));
	}
}

void FMantleAssetUploadQueue::HandleDiff(ERequestKind Kind, const TArray<int32>& Indices, const FMantleApiResult& Result)
{
	const double Now = Clock();
	const bool bPoll = Kind == ERequestKind::Poll;
	if (!Result.bSuccess)
	{
		if (IsRunLevelFailure(Result) || !FMantleCosmeticUploadSession::IsRetryable(Result))
		{
			Fail(DescribeRunFailure(Result, bPoll ? TEXT("following the files Mantle is processing") : TEXT("checking which files Mantle already has")));
			return;
		}
		if (bPoll)
		{
			// A look at processing files that failed costs nothing: look again later. Only a
			// line that stays down stops the run.
			if (++PollFailures > Settings.RetryDelays.Num())
			{
				Fail(DescribeRunFailure(Result, TEXT("following the files Mantle is processing")));
				return;
			}
			NextPollAt = Now + FMath::Max(Settings.PollEvery, Result.RetryAfterSeconds);
			return;
		}
		for (const int32 Index : Indices)
		{
			Items[Index].bNeedsDiff = true;
		}
		if (!Backoff(DiffAttempts, NextDiffAt, Result, Now))
		{
			Fail(DescribeRunFailure(Result, TEXT("checking which files Mantle already has")));
		}
		return;
	}
	TArray<FMantleUploadedAsset> Known;
	TArray<FString> Unknown;
	if (!FMantleApiClient::ParseDiffAssets(Result.Body, Known, Unknown))
	{
		Fail(TEXT("Mantle's answer about which files it has could not be read. Is the API address right (Editor Preferences > Plugins > Mantle)?"));
		return;
	}
	if (bPoll)
	{
		PollFailures = 0;
		++PollRound;
	}
	else
	{
		DiffAttempts = 0;
	}
	int32 AlreadyThere = 0;
	int32 ToSend = 0;
	for (const int32 Index : Indices)
	{
		FMantleQueuedUploadStatus& Status = Statuses[Index];
		if (Status.State == EMantleQueuedUploadState::Refused || Status.State == EMantleQueuedUploadState::Approved)
		{
			continue;
		}
		const FString& Hash = Items[Index].Upload.Hash;
		const FMantleUploadedAsset* Found = Known.FindByPredicate([&Hash](const FMantleUploadedAsset& Asset) { return Asset.Hash == Hash; });
		if (Found)
		{
			if (!bPoll)
			{
				Status.bAlreadyOnMantle = true;
				++AlreadyThere;
			}
			ApplyAsset(Index, *Found);
			continue;
		}
		// Unknown: never uploaded, or only by someone else (an upload is then what gives this
		// creator a claim), or a claim that went away while processing.
		Items[Index].bNeedsUpload = true;
		Items[Index].NextActionAt = 0.0;
		Status.State = EMantleQueuedUploadState::Uploading;
		Status.Message = Items[Index].bOpen ? TEXT("Waiting to upload") : TEXT("Waiting for the files it depends on");
		++ToSend;
	}
	if (!bPoll)
	{
		AddLog(FString::Printf(TEXT("Mantle already has %d of %d file(s); %d to upload"), AlreadyThere, Indices.Num(), ToSend));
	}
	if (bPoll)
	{
		SchedulePoll(Now);
	}
}

void FMantleAssetUploadQueue::DropBytes(int32 Index)
{
	// Empty, not Reset: the memory goes back, which is the point. A request still out for the item points into its bytes (the
	// answer of its own upload clears bInFlight first), so an item in flight is left alone.
	if (!Items[Index].bInFlight)
	{
		Items[Index].Upload.Bytes.Empty();
		Items[Index].SendBuffer.Empty();
	}
}

int64 FMantleAssetUploadQueue::GetHeldBytes() const
{
	int64 Held = 0;
	for (const FItem& Item : Items)
	{
		Held += Item.Upload.Bytes.Num() + Item.SendBuffer.Num();
	}
	return Held;
}

void FMantleAssetUploadQueue::ReleaseBytes()
{
	if (IsRunning())
	{
		return;
	}
	for (FItem& Item : Items)
	{
		Item.Upload.Bytes.Empty();
		Item.SendBuffer.Empty();
	}
}

void FMantleAssetUploadQueue::SchedulePoll(double Now)
{
	const bool bAnyProcessing = Statuses.ContainsByPredicate([](const FMantleQueuedUploadStatus& Status)
	{
		return Status.State == EMantleQueuedUploadState::Processing;
	});
	if (!bAnyProcessing || !Settings.bWaitForApproval)
	{
		NextPollAt = 0.0;
		return;
	}
	const float Delay = Settings.PollDelays.IsValidIndex(PollRound) ? Settings.PollDelays[PollRound] : Settings.PollEvery;
	NextPollAt = Now + Delay;
}

void FMantleAssetUploadQueue::Refuse(int32 Index, const FString& Code, const FString& Message)
{
	FItem& Item = Items[Index];
	FMantleQueuedUploadStatus& Status = Statuses[Index];
	Item.bNeedsUpload = false;
	Item.bNeedsDiff = false;
	Status.State = EMantleQueuedUploadState::Refused;
	Status.Code = Code;
	Status.Message = Message;
	// Final: nothing of it is sent again, so its bytes are not needed.
	DropBytes(Index);
	AddLog(FString::Printf(TEXT("Refused %s (%s): %s"), *Item.Upload.Name, *Code, *Message));
	if (OnRefused)
	{
		OnRefused(Status);
	}
}

void FMantleAssetUploadQueue::ApplyAsset(int32 Index, const FMantleUploadedAsset& Asset)
{
	FItem& Item = Items[Index];
	FMantleQueuedUploadStatus& Status = Statuses[Index];
	if (!MantleAssetUploadQueuePrivate::FitsUpload(Asset, Item.Upload.Options))
	{
		Refuse(Index, TEXT("uploaded_differently"), FString::Printf(
			TEXT("'%s' has exactly the same pixels as an image already on Mantle as %s, and Mantle prepares each image for one use only."),
			*Item.Upload.Name, *MantleAssetUploadQueuePrivate::RoleWords(Asset.CookOptions.Role)));
		return;
	}
	Item.bNeedsUpload = false;
	Status.AssetId = Asset.Id;
	if (!Item.bIdAnnounced)
	{
		Item.bIdAnnounced = true;
		if (OnAssetId)
		{
			OnAssetId(Status);
		}
	}
	if (Asset.IsApproved())
	{
		if (Status.State != EMantleQueuedUploadState::Approved)
		{
			AddLog(FString::Printf(TEXT("%s approved (%s)"), *Item.Upload.Name, *Asset.Id));
		}
		Status.State = EMantleQueuedUploadState::Approved;
		Status.bInReview = false;
		Status.Message = Status.bAlreadyOnMantle ? TEXT("Already on Mantle") : TEXT("Ready");
		// Mantle has it, cooked and screened: the status keeps the hash, the size and the id. A file still being
		// processed keeps its bytes, because a look that finds Mantle has forgotten it sends it again.
		DropBytes(Index);
		return;
	}
	if (Asset.IsRejected())
	{
		Status.CookError = Asset.CookError;
		Refuse(Index, TEXT("rejected"), DescribeRejection(Asset, Item.Upload.Name));
		return;
	}
	const bool bWasProcessing = Status.State == EMantleQueuedUploadState::Processing;
	Status.State = EMantleQueuedUploadState::Processing;
	Status.bInReview = Asset.ModerationState == TEXT("review");
	Status.Message = Status.bInReview
		? FString(TEXT("Waiting for a Mantle moderator to look at it"))
		: Asset.CookState == TEXT("passed") ? FString(TEXT("Being screened")) : FString(TEXT("Being processed"));
	if (!bWasProcessing && NextPollAt <= 0.0)
	{
		// The first file to wait on Mantle since nothing was: start looking at the quick pace again.
		PollRound = 0;
		SchedulePoll(Clock());
	}
}

void FMantleAssetUploadQueue::SettleSameBytes(int32 Index, const FMantleUploadedAsset& Asset)
{
	// Other files with these bytes waiting to be sent: Mantle's answer for the first is theirs
	// too (one hash is one asset), or a refusal when they want it cooked another way.
	const FString Hash = Items[Index].Upload.Hash;
	for (int32 Other = 0; Other < Items.Num(); ++Other)
	{
		if (Other != Index && Items[Other].bNeedsUpload && !Items[Other].bInFlight && Items[Other].Upload.Hash == Hash)
		{
			ApplyAsset(Other, Asset);
		}
	}
}

void FMantleAssetUploadQueue::HandleUpload(int32 Index, const FMantleApiResult& Result)
{
	const double Now = Clock();
	if (!Items.IsValidIndex(Index))
	{
		return;
	}
	FItem& Item = Items[Index];
	FMantleQueuedUploadStatus& Status = Statuses[Index];
	if (!Result.bSuccess)
	{
		if (IsRunLevelFailure(Result))
		{
			Fail(DescribeRunFailure(Result, FString::Printf(TEXT("sending '%s'"), *Item.Upload.Name)));
			return;
		}
		if (Result.StatusCode == 429)
		{
			// The whole account is over its hourly limit: every upload waits, and waiting is not a failed attempt.
			PauseForRateLimit(Result, Now);
			Status.Message = TEXT("Waiting for Mantle's hourly upload limit");
			return;
		}
		if (IsUploadBusy(Result))
		{
			// Mantle is taking all the large uploads it allows at once. Not a failure and not this
			// file's fault: it waits for its own turn (the server's Retry-After) and the others keep sending.
			const float Jitter = Settings.BusyJitter > 0.0f ? FMath::FRand() * Settings.BusyJitter : 0.0f;
			const float Wait = FMath::Max(Result.RetryAfterSeconds, Settings.BusyMinWait) + Jitter;
			if (Item.BusyWaited + Wait <= Settings.MaxBusyWait)
			{
				if (Item.BusyWaited <= 0.0)
				{
					AddLog(FString::Printf(TEXT("Mantle is busy with other uploads; '%s' waits for its turn"), *Item.Upload.Name));
				}
				Item.BusyWaited += Wait;
				Item.NextActionAt = Now + Wait;
				Status.Message = TEXT("Mantle is busy with other uploads; waiting for a turn");
				return;
			}
		}
		if (FMantleCosmeticUploadSession::IsRetryable(Result))
		{
			if (!Backoff(Status.Attempts, Item.NextActionAt, Result, Now))
			{
				Fail(DescribeRunFailure(Result, FString::Printf(TEXT("sending '%s'"), *Item.Upload.Name)));
				return;
			}
			Status.Message = TEXT("Retrying shortly");
			return;
		}
		FString Code;
		const FString Why = DescribeRefusal(Result, Item.Upload.Name, Code);
		Refuse(Index, Code, Why);
		// The same bytes in another file would be refused the same way.
		for (int32 Other = 0; Other < Items.Num(); ++Other)
		{
			if (Other != Index && Items[Other].bNeedsUpload && !Items[Other].bInFlight && Items[Other].Upload.Hash == Item.Upload.Hash
				&& Items[Other].Upload.Options.Kind == Item.Upload.Options.Kind && Items[Other].Upload.Options.Role == Item.Upload.Options.Role)
			{
				FString OtherCode;
				Refuse(Other, Code, DescribeRefusal(Result, Items[Other].Upload.Name, OtherCode));
			}
		}
		return;
	}
	FMantleUploadedAsset Asset;
	if (!FMantleApiClient::ParseUploadedAsset(Result.Body, Asset))
	{
		Fail(FString::Printf(TEXT("Mantle stored '%s' but its answer could not be read. Is the API address right (Editor Preferences > Plugins > Mantle)?"),
			*Item.Upload.Name));
		return;
	}
	Status.Attempts = 0;
	AddLog(FString::Printf(TEXT("%s %s (%s, %s)"), *Item.Upload.Name, Asset.bCreated ? TEXT("uploaded") : TEXT("was already on Mantle; claimed"),
		*Asset.Id, *MantleAssetUploadQueuePrivate::Size(Status.Bytes)));
	if (Asset.Hash.IsEmpty())
	{
		Asset.Hash = Item.Upload.Hash;
	}
	// The answer describes the asset as Mantle holds it; this upload's own options are what it was sent with.
	if (Asset.Kind.IsEmpty())
	{
		Asset.Kind = Item.Upload.Options.Kind;
	}
	ApplyAsset(Index, Asset);
	SettleSameBytes(Index, Asset);
}

void FMantleAssetUploadQueue::CheckFinished()
{
	if (!IsRunning() || InFlight > 0)
	{
		return;
	}
	if (Stages.IsValidIndex(OpenStageIndex + 1))
	{
		return;
	}
	bool bAllFinal = true;
	bool bAllHaveIds = true;
	bool bOnlyReviewLeft = true;
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		const FMantleQueuedUploadStatus& Status = Statuses[Index];
		if (Status.IsFinal())
		{
			continue;
		}
		bAllFinal = false;
		if (!Status.HasId())
		{
			bAllHaveIds = false;
		}
		if (!(Status.State == EMantleQueuedUploadState::Processing && Status.bInReview))
		{
			bOnlyReviewLeft = false;
		}
	}
	if (bAllFinal || (!Settings.bWaitForApproval && bAllHaveIds))
	{
		Outcome = EMantleUploadQueueOutcome::Completed;
		UpdateProgress();
		int32 Refused = 0;
		for (const FMantleQueuedUploadStatus& Status : Statuses)
		{
			Refused += Status.State == EMantleQueuedUploadState::Refused ? 1 : 0;
		}
		AddLog(FString::Printf(TEXT("Done: %d file(s), %d refused"), Items.Num(), Refused));
		return;
	}
	if (Settings.bWaitForApproval && Settings.bStopWhenOnlyReviewLeft && bOnlyReviewLeft && bAllHaveIds)
	{
		Outcome = EMantleUploadQueueOutcome::WaitingForReview;
		AddLog(TEXT("Only files waiting for a moderator are left"));
	}
}

void FMantleAssetUploadQueue::UpdateProgress()
{
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		FItem& Item = Items[Index];
		const FMantleQueuedUploadStatus& Status = Statuses[Index];
		const bool bRefused = Status.State == EMantleQueuedUploadState::Refused;
		if (!Item.bCountedSent && (Status.HasId() || bRefused))
		{
			Item.bCountedSent = true;
			Progress.AssetsSent += 1;
			Progress.BytesDone += Status.Bytes;
		}
		if (!Item.bCountedFinished && Status.IsFinal())
		{
			Item.bCountedFinished = true;
			Progress.AssetsFinished += 1;
		}
	}
	if (bProgressEmitted && Progress == LastEmitted)
	{
		return;
	}
	bProgressEmitted = true;
	LastEmitted = Progress;
	if (OnProgress)
	{
		OnProgress(Progress);
	}
}

FString FMantleAssetUploadQueue::GetStatusText() const
{
	using namespace MantleAssetUploadQueuePrivate;
	switch (Outcome)
	{
	case EMantleUploadQueueOutcome::Idle: return TEXT("Ready");
	case EMantleUploadQueueOutcome::Cancelled: return TEXT("Cancelled. Everything already uploaded stays on Mantle.");
	case EMantleUploadQueueOutcome::Failed: return Failure;
	case EMantleUploadQueueOutcome::WaitingForReview: return TEXT("Waiting for a Mantle moderator.");
	case EMantleUploadQueueOutcome::Completed: return TEXT("Every file is on Mantle.");
	default: break;
	}
	const double Now = Clock();
	if (PausedUntil > Now)
	{
		const FDateTime ResumeAt = FDateTime::Now() + FTimespan::FromSeconds(PausedUntil - Now);
		return FString::Printf(TEXT("Mantle's hourly upload limit is reached. Uploading continues by itself at %s (in about %s); ")
			TEXT("you can keep working meanwhile."), *ResumeAt.ToString(TEXT("%H:%M")), *Minutes(PausedUntil - Now));
	}
	if (Progress.AssetsSent < Progress.AssetsTotal)
	{
		return FString::Printf(TEXT("Uploading%s: %d of %d files on Mantle, %s of %s."),
			Progress.CurrentName.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" '%s'"), *Progress.CurrentName),
			Progress.AssetsSent, Progress.AssetsTotal, *Size(Progress.BytesDone), *Size(Progress.BytesTotal));
	}
	return FString::Printf(TEXT("Mantle is processing and screening the files: %d of %d ready."), Progress.AssetsFinished, Progress.AssetsTotal);
}
