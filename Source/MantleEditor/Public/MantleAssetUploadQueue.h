// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleApiClient.h"
#include "MantleCosmeticUpload.h"

/**
 * One file for FMantleAssetUploadQueue (F4 #338). Stage orders the files: a stage opens once every
 * file of the stages before it has an id on Mantle (or was refused), so a material, whose bytes
 * name its textures' ids, is built only after its textures have them.
 */
struct MANTLEEDITOR_API FMantleQueuedUpload
{
	/** The caller's local key (for places, FMantlePlaceUpload::Key). Unique in one queue. */
	FString Key;

	/** Lower stages first. Places: EMantlePlaceUploadStage (texture, material, mesh). */
	int32 Stage = 0;

	/** The Unreal asset, for messages: never an id or a hash. */
	FString Name;

	FMantleAssetUploadOptions Options;
	TArray<uint8> Bytes;

	/**
	 * #804: the bytes on disk instead of in Bytes (the publish spilled them), and their size. The queue reads the file when it
	 * sends the item and lets the bytes go again once Mantle has answered. Used only when Bytes is empty; Hash must be given.
	 */
	FString File;
	int64 FileSize = 0;

	/** "sha256:" + hex of Bytes; computed when empty. */
	FString Hash;

	/**
	 * Bytes that depend on earlier stages (a material naming its textures' ids): called when the
	 * item's stage opens, to fill Bytes (and Hash). False with a plain sentence refuses the item
	 * ("blocked") without sending anything. Unset: the bytes are given up front.
	 */
	TFunction<bool(FMantleQueuedUpload& Item, FString& OutWhyNot)> Prepare;
};

enum class EMantleQueuedUploadState : uint8
{
	/** Its stage has not opened yet, or Mantle is being asked whether it already has it. */
	Queued,
	/** Mantle does not have it: waiting for a free slot, being sent, or waiting to retry. */
	Uploading,
	/** On Mantle: being cooked and screened. */
	Processing,
	/** Approved: a place may go live with it. */
	Approved,
	/** Final: Mantle will not take it (Code and Message say why and what to do). */
	Refused,
};

/** One file's progress and outcome, for the panel and the caller. */
struct MANTLEEDITOR_API FMantleQueuedUploadStatus
{
	FString Key;
	FString Name;
	int32 Stage = 0;
	EMantleQueuedUploadState State = EMantleQueuedUploadState::Queued;

	/** ast_u id once Mantle has it (known at the diff, or uploaded). */
	FString AssetId;
	FString Hash;
	int64 Bytes = 0;

	/** Mantle already had these bytes under this creator's claim: nothing was sent. */
	bool bAlreadyOnMantle = false;

	/** A moderator has to look at it (moderationState review): it can take a while. */
	bool bInReview = false;

	/**
	 * Why it was refused: the API's code (claim_taken_down, uploaded_differently, too_large,
	 * assets_not_owned, ...), "rejected" (the cook or screening refused it after upload) or
	 * "blocked" (Prepare refused it).
	 */
	FString Code;

	/** Plain words: what it is doing, or why it was refused and what to do. */
	FString Message;

	/** The cook's own reason, when the cook refused it. */
	FString CookError;

	/** Sends that failed for a reason time can fix. */
	int32 Attempts = 0;

	bool HasId() const { return !AssetId.IsEmpty(); }
	bool IsFinal() const { return State == EMantleQueuedUploadState::Approved || State == EMantleQueuedUploadState::Refused; }
};

/**
 * The counts behind the progress bar. Every field only ever grows: totals grow when a later
 * stage's bytes are built, and a finished run ends with each done count at its total.
 */
struct MANTLEEDITOR_API FMantleUploadQueueProgress
{
	int32 AssetsTotal = 0;
	/** Files Mantle has (known or uploaded), plus files refused. */
	int32 AssetsSent = 0;
	/** Files approved or refused. */
	int32 AssetsFinished = 0;
	int64 BytesTotal = 0;
	/** Bytes of the files counted in AssetsSent. */
	int64 BytesDone = 0;
	/** The Unreal asset being sent most recently. */
	FString CurrentName;

	bool operator==(const FMantleUploadQueueProgress& Other) const
	{
		return AssetsTotal == Other.AssetsTotal && AssetsSent == Other.AssetsSent && AssetsFinished == Other.AssetsFinished
			&& BytesTotal == Other.BytesTotal && BytesDone == Other.BytesDone && CurrentName == Other.CurrentName;
	}
	bool operator!=(const FMantleUploadQueueProgress& Other) const { return !(*this == Other); }
};

enum class EMantleUploadQueueOutcome : uint8
{
	Idle,
	Running,
	/** Every file is approved or refused (or, without waiting for approval, has an id or was refused). */
	Completed,
	/** Everything else is done; what is left waits for a human moderator. Publish again later. */
	WaitingForReview,
	/** The run stopped for a reason that is not one file's (no connection, sign-in, Mantle down). Resumable. */
	Failed,
	Cancelled,
};

/**
 * The bulk upload behind Publish (F4 #338): a Fab level's first publish is hundreds of files, and a
 * creator must never have to babysit it. On top of A2.4's transport (IMantleUploadTransport,
 * FMantleApiClient::DiffAssetsAsync / UploadAssetAsync):
 *
 * - diffs in batches (POST /assets/diff, DiffBatchSize hashes a request), every file whose bytes
 *   are known up front at once, then each later stage's as it opens;
 * - uploads what Mantle does not have in stage order, at most MaxConcurrentRequests requests in
 *   flight (diffs included); the same bytes are never sent twice at once, and a second file with
 *   bytes already sent is settled from the first answer;
 * - retries a dropped connection or a 5xx with backoff (RetryDelays), and a 429 pauses every
 *   upload for the server's Retry-After (RateLimitedDelay when it sends none) without using up
 *   attempts; a 503 upload_busy (the server is taking all the large uploads it allows) makes only
 *   that file wait, for the Retry-After, also without using up attempts (up to MaxBusyWait);
 * - then follows cooking and screening by re-diffing in batches (diff returns known files with
 *   their status) until each is approved or rejected: no cutoff, only Cancel;
 * - resumes after an editor restart for free: Mantle's diff answers "known" for everything
 *   already uploaded, so only the remainder is sent (nothing is stored locally);
 * - a file Mantle refuses (a takedown, the same pixels cooked for another role, too large,
 *   rejected) is refused on its own with a plain sentence; the run carries on with the rest.
 *
 * Driven by Tick (the publish flow calls it every frame; tests call it with a fake clock). The
 * same queue serves place sounds (#324).
 */
class MANTLEEDITOR_API FMantleAssetUploadQueue : public TSharedFromThis<FMantleAssetUploadQueue>
{
public:
	struct FSettings
	{
		/** Requests in flight at once, diffs and uploads together. */
		int32 MaxConcurrentRequests = 4;
		/**
		 * #804: uploads of LargeUploadBytes or more in flight at once. Mantle gives one creator this many slots for large uploads
		 * (UploadSlotsPerCreator) and answers the rest 503 upload_busy, so sending more only has them wait. Small files still use
		 * every request slot. The busy wait stays as the safety net.
		 */
		int32 MaxConcurrentLargeUploads = 2;
		int64 LargeUploadBytes = 1024 * 1024;
		/** Hashes per POST /assets/diff (the API takes up to 4096). */
		int32 DiffBatchSize = 500;
		/** Waits before each retry of a request that failed for a reason time can fix; then the run stops (resumable). */
		TArray<float> RetryDelays = { 2.0f, 5.0f, 10.0f, 20.0f, 40.0f, 60.0f };
		/** A 429 that says nothing about when: wait this long. */
		float RateLimitedDelay = 60.0f;
		/** A Retry-After longer than this stops the run with a sentence instead of waiting (the API's window is an hour). */
		float MaxRetryAfter = 3900.0f;
		/**
		 * A 503 upload_busy (Mantle is taking all the large uploads it allows at once, #799), or any 503 that sends a Retry-After,
		 * is not a failed attempt: that file waits at least this long (longer when the server says so) while the others carry on.
		 */
		float BusyMinWait = 2.0f;
		/** A random extra wait of up to this many seconds, so files turned away together do not come back together. */
		float BusyJitter = 1.0f;
		/** The most one file waits on busy answers in all; past it a 503 is an ordinary failure again (RetryDelays, then the run stops). */
		float MaxBusyWait = 600.0f;
		/** Waits before the first looks at files being processed; then every PollEvery seconds. */
		TArray<float> PollDelays = { 2.0f, 3.0f, 5.0f };
		float PollEvery = 10.0f;
		/** Follow cooking and screening until every file is approved or rejected (Publish); false stops once every file has an id (Save). */
		bool bWaitForApproval = true;
		/** Stop (WaitingForReview) once only files waiting for a human moderator are left. */
		bool bStopWhenOnlyReviewLeft = true;
	};

	/** Clock: seconds, FPlatformTime::Seconds when unset. */
	explicit FMantleAssetUploadQueue(TSharedRef<IMantleUploadTransport> InTransport, TFunction<double()> InClock = nullptr);

	FSettings Settings;

	/** Takes the files and begins; Tick does the work. Keys must be unique. */
	void Start(TArray<FMantleQueuedUpload> InItems);

	/** Sends what is due. Cheap when there is nothing to do. */
	void Tick();

	/** Stops sending and ignores answers still on their way. What was uploaded stays; the next run resumes from it. */
	void Cancel();

	EMantleUploadQueueOutcome GetOutcome() const { return Outcome; }
	bool IsRunning() const { return Outcome == EMantleUploadQueueOutcome::Running; }
	bool IsFinished() const { return Outcome != EMantleUploadQueueOutcome::Idle && Outcome != EMantleUploadQueueOutcome::Running; }

	/** Why the run stopped (Failed), for the creator. */
	const FString& GetFailure() const { return Failure; }

	const TArray<FMantleQueuedUploadStatus>& GetStatuses() const { return Statuses; }
	const FMantleQueuedUploadStatus* FindStatus(const FString& Key) const;
	const FMantleUploadQueueProgress& GetProgress() const { return Progress; }

	/** When uploads are paused by Mantle's hourly limit: until when (Clock seconds), else 0. */
	double GetPausedUntil() const { return PausedUntil; }

	/** One plain line about what the queue is doing now. */
	FString GetStatusText() const;

	/** What happened, oldest first. */
	const TArray<FString>& GetLog() const { return Log; }

	/** Every change in the counts, in order (never backwards). */
	TFunction<void(const FMantleUploadQueueProgress&)> OnProgress;

	/** A file has its id (known at the diff, or uploaded): record it. Fires once per file, before any later stage opens. */
	TFunction<void(const FMantleQueuedUploadStatus&)> OnAssetId;

	/** A file was refused. */
	TFunction<void(const FMantleQueuedUploadStatus&)> OnRefused;

	/**
	 * #804: how many bytes the queue holds for its files right now (the files' own bytes, and a spilled file read for a send in
	 * flight). A file's bytes go once Mantle has it approved or refused it; a file still being processed keeps them, because a
	 * look at it that finds Mantle has forgotten it sends it again. Tests hold it to that.
	 */
	int64 GetHeldBytes() const;

	/** #804: lets every file's bytes go (the run is over). Statuses, hashes, ids and the log stay. Does nothing while the queue is running. */
	void ReleaseBytes();

	/**
	 * The sentence for a file Mantle refused at upload, naming the Unreal asset and the fix.
	 * OutCode is the API's code (or a stand-in when it sent none).
	 */
	static FString DescribeRefusal(const FMantleApiResult& Result, const FString& Name, FString& OutCode);

	/** The sentence for a file the cook or screening rejected after upload. */
	static FString DescribeRejection(const FMantleUploadedAsset& Asset, const FString& Name);

	/** The sentence for a failure that stops the whole run (connection, sign-in, Mantle itself). */
	static FString DescribeRunFailure(const FMantleApiResult& Result, const FString& While);

	/** A failure that is about the run, not one file: sign-in, a wrong address, a token that may not upload. */
	static bool IsRunLevelFailure(const FMantleApiResult& Result);

	/** A 503 that means "try this file again in a moment": upload_busy, or any 503 that sends a Retry-After. */
	static bool IsUploadBusy(const FMantleApiResult& Result);

private:
	struct FItem
	{
		FMantleQueuedUpload Upload;
		/** A spilled file's bytes (Upload.File) while its request is out: sent from here, then let go. */
		TArray<uint8> SendBuffer;
		bool bOpen = false;
		bool bPrepared = false;
		bool bNeedsDiff = false;
		bool bNeedsUpload = false;
		bool bInFlight = false;
		bool bIdAnnounced = false;
		bool bCountedSent = false;
		bool bCountedFinished = false;
		double NextActionAt = 0.0;
		/** Seconds spent waiting on busy answers so far (Settings.MaxBusyWait). */
		double BusyWaited = 0.0;
	};

	enum class ERequestKind : uint8 { Check, Poll, Upload };

	void OpenStages();
	bool IsStageSettled(int32 Stage) const;
	bool PrepareItem(int32 Index);
	bool SendCheck();
	bool SendPoll(double Now);
	bool SendUpload(double Now);
	void Send(const FMantleUploadCallRequest& Request, ERequestKind Kind, TArray<int32> Indices);
	void OnResponse(uint32 ForGeneration, ERequestKind Kind, const TArray<int32>& Indices, const FMantleApiResult& Result);
	void HandleDiff(ERequestKind Kind, const TArray<int32>& Indices, const FMantleApiResult& Result);
	void HandleUpload(int32 Index, const FMantleApiResult& Result);
	void ApplyAsset(int32 Index, const FMantleUploadedAsset& Asset);
	void SettleSameBytes(int32 Index, const FMantleUploadedAsset& Asset);
	void Refuse(int32 Index, const FString& Code, const FString& Message);
	/** #804: an item Mantle has settled (approved or refused) no longer needs its bytes. */
	void DropBytes(int32 Index);
	void SchedulePoll(double Now);
	/** Waits out a failure time can fix; false when the retries are used up (then the run stops). */
	bool Backoff(int32& InOutAttempts, double& OutNextAt, const FMantleApiResult& Result, double Now);
	void PauseForRateLimit(const FMantleApiResult& Result, double Now);
	void CheckFinished();
	void Fail(const FString& Why);
	void UpdateProgress();
	void AddLog(const FString& Line);
	bool IsHashBusy(const FString& Hash, int32 Except) const;

	TSharedRef<IMantleUploadTransport> Transport;
	TFunction<double()> Clock;

	EMantleUploadQueueOutcome Outcome = EMantleUploadQueueOutcome::Idle;
	TArray<FItem> Items;
	TArray<FMantleQueuedUploadStatus> Statuses;
	TArray<int32> Stages;
	int32 OpenStageIndex = INDEX_NONE;

	uint32 Generation = 0;
	int32 InFlight = 0;
	bool bDiffInFlight = false;

	int32 DiffAttempts = 0;
	double NextDiffAt = 0.0;
	int32 PollRound = 0;
	int32 PollFailures = 0;
	double NextPollAt = 0.0;
	double PausedUntil = 0.0;

	FMantleUploadQueueProgress Progress;
	FMantleUploadQueueProgress LastEmitted;
	bool bProgressEmitted = false;
	FString Failure;
	TArray<FString> Log;
};
