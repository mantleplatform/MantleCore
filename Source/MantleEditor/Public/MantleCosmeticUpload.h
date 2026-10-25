// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleApiClient.h"
#include "MantleCosmeticPackage.h"

/**
 * The calls a creator upload makes (FMantleApiClient's creator upload calls): the first five are
 * a cosmetic upload's (A2.4 #283); the next four are a place Publish's (F6 #340), so the place
 * flow's whole conversation with Mantle goes through the same transport and the same fake, as is
 * SetAttributions (#398).
 */
enum class EMantleUploadCall : uint8
{
	DiffAssets,
	UploadAsset,
	GetAsset,
	CreateItem,
	GetItem,
	/** POST /scripts?language=<Language>, the source text (Json) as the body. */
	UploadScript,
	/** POST /places, Json = {"name": ...}. */
	CreatePlace,
	/** PUT /places/<Id>/draft, Json = the place file. */
	SaveDraft,
	/** POST /places/<Id>/publish, Json = FMantlePublisher::MakePublishBody. */
	PublishPlace,
	/**
	 * PATCH /avatar/items/<Id>, Json = FMantleCosmeticPackage::MakeItemPatchJson: a new version of an
	 * explicitly selected existing catalog item instead of CreateItem.
	 */
	UpdateItem,
	/** PUT /assets/attributions, Json = FMantleAttribution::MakeRequestBody: a place Publish's credits (#398). */
	SetAttributions,
	/** POST /library/items, Json = FMantleLibraryPublish::MakeItemBody; IdempotencyKey names the creation across retries (#614 stage 6). */
	CreateLibraryItem,
	/** GET /library/items/<Id>: the item with its versions (#614 stage 6). */
	GetLibraryItem,
	/** POST /library/items/<Id>/versions, Json = FMantleLibraryPublish::MakeVersionBody (#614 stage 6). */
	CreateLibraryVersion,
	/** PATCH /library/items/<Id>, Json = FMantleLibraryPublish::MakeVisibilityPatchBody: the visibility an existing item takes (#614 stage 6). */
	UpdateLibraryItem,
};

/** One call as the session asks for it. Bytes points into the session's package and lives as long as the session. */
struct MANTLEEDITOR_API FMantleUploadCallRequest
{
	EMantleUploadCall Call = EMantleUploadCall::DiffAssets;
	TArray<FString> Hashes;
	FMantleAssetUploadOptions Options;
	const TArray<uint8>* Bytes = nullptr;
	FString Id;
	FString Json;
	/** UploadScript: luau, javascript, typescript, python or graph; rml or rcss for a place's UI file (#549). */
	FString Language;
	/** CreateItem: the Idempotency-Key, the same on every retry of one item's creation (#394). */
	FString IdempotencyKey;
};

/**
 * How the session reaches the API. The editor uses FMantleApiUploadTransport; the automation tests
 * use a fake that answers from a scripted server, so every path (retries, resumes, refusals) is
 * tested with no network and no GUI.
 */
class MANTLEEDITOR_API IMantleUploadTransport
{
public:
	virtual ~IMantleUploadTransport() = default;
	virtual void Send(const FMantleUploadCallRequest& Request, FMantleApiCallback OnDone) = 0;
};

/** FMantleApiClient::DiffAssetsAsync / UploadAssetAsync / GetAssetAsync / CreateAvatarItemAsync / GetAvatarCreationAsync. */
class MANTLEEDITOR_API FMantleApiUploadTransport : public IMantleUploadTransport
{
public:
	virtual void Send(const FMantleUploadCallRequest& Request, FMantleApiCallback OnDone) override;
};

enum class EMantleUploadAssetState : uint8
{
	/** Not sent yet (or waiting for the body shape it names). */
	Waiting,
	/** Sent; Mantle is cooking and screening it. */
	Processing,
	/** Approved: the item may name it. */
	Done,
	Failed,
};

/** One file's progress, for the panel. */
struct MANTLEEDITOR_API FMantleUploadAssetProgress
{
	FString Key;
	FString Label;
	FString AssetId;
	EMantleUploadAssetState State = EMantleUploadAssetState::Waiting;

	/** True when Mantle already had these bytes under this creator's claim (nothing was sent). */
	bool bAlreadyOnMantle = false;

	/** Written for the creator: what it is doing, or why it failed. */
	FString Message;

	int32 Attempts = 0;
	int32 Polls = 0;
	double NextActionAt = 0.0;
	double ProcessingSince = 0.0;

	/** Seconds spent waiting on "Mantle is busy with other uploads" answers (Timing.MaxBusyWait); these spend no attempt. */
	double BusyWaited = 0.0;
};

enum class EMantleUploadPhase : uint8
{
	Idle,
	/** POST /assets/diff: which files Mantle already has. */
	Checking,
	/** Sending and waiting on files. */
	Uploading,
	/** POST /avatar/items, or PATCH /avatar/items/<TargetItemId> for an explicitly selected version update. */
	CreatingItem,
	/** Following the item through screening. */
	Screening,
	Done,
	Failed,
	Cancelled,
};

/**
 * One click's worth of publishing (A2.4 #283): the plugin does the rest. It asks which files Mantle
 * already has (POST /assets/diff), uploads the others in dependency order (a garment's body shape
 * first, then the garment naming it as source=), waits for each to be cooked and approved, creates
 * the item (POST /avatar/items) and follows it through screening (GET /me/avatar/creations/:id).
 *
 * Friendly by construction:
 * - retries: a dropped connection, a 5xx or a rate limit waits and tries again (RetryDelays), so
 *   a flaky line never costs the creator a click;
 * - resumable: uploads are content-addressed, so a second run finds everything the first sent
 *   (the diff's known list) and sends only what is missing; the item itself is remembered in a
 *   small file under Saved/ with a fingerprint of its body, so the same item is never created twice;
 * - every refusal is a sentence for the creator (DescribeFailure), mapped from the API's codes.
 *
 * Driven by Tick (the panel calls it every frame; tests call it with a fake clock), one request in
 * flight at a time.
 */
class MANTLEEDITOR_API FMantleCosmeticUploadSession : public TSharedFromThis<FMantleCosmeticUploadSession>
{
public:
	struct FTiming
	{
		/** Waits before each retry of a request that failed for a reason time can fix; then it gives up. */
		TArray<float> RetryDelays = { 2.0f, 5.0f, 10.0f, 20.0f, 30.0f };
		/** Waits before the first polls of a file or the item; then every PollEvery seconds. */
		TArray<float> PollDelays = { 1.0f, 2.0f, 3.0f };
		float PollEvery = 5.0f;
		/** A file still processing after this long stops the run (resumable). */
		float ProcessingTimeout = 900.0f;
		/** The item is followed through screening this long, then left to finish on its own. */
		float ScreeningTimeout = 300.0f;
		/** A 429 waits this long (the API's Retry-After is minutes). */
		float RateLimitedDelay = 60.0f;
		/**
		 * A 503 upload_busy (Mantle is taking all the large uploads it allows, #799) is not a failed attempt: that file
		 * waits the server's Retry-After, at least this long. Mirrors FMantleAssetUploadQueue::FSettings.
		 */
		float BusyMinWait = 2.0f;
		/** A random extra wait of up to this many seconds, so uploads turned away together do not come back together. */
		float BusyJitter = 1.0f;
		/** The most one file waits on busy answers in all; past it a 503 is an ordinary failure again (RetryDelays, then the run stops). */
		float MaxBusyWait = 600.0f;
		/** While files are still being processed or screened, the log says which ones every this many seconds (0: never). */
		float WaitLogEvery = 30.0f;
	};

	/**
	 * ResumeFile: where the created item is remembered (empty: not remembered). Clock: seconds,
	 * FPlatformTime::Seconds when unset.
	 */
	FMantleCosmeticUploadSession(FMantleCosmeticPackage InPackage, TSharedRef<IMantleUploadTransport> InTransport,
		FString InResumeFile = FString(), TFunction<double()> InClock = nullptr);

	FTiming Timing;

	/** Begins; Tick does the work. Refuses a package with errors (Failed, with the first error). */
	void Start();

	/** Sends the next request when its time has come. Cheap when there is nothing to do. */
	void Tick();

	/** Stops sending. What was uploaded stays on Mantle; the next run resumes from it. */
	void Cancel();

	EMantleUploadPhase GetPhase() const { return Phase; }
	bool IsFinished() const { return Phase == EMantleUploadPhase::Done || Phase == EMantleUploadPhase::Failed || Phase == EMantleUploadPhase::Cancelled; }
	bool Succeeded() const { return Phase == EMantleUploadPhase::Done; }

	/** 0 to 1. */
	float GetProgress() const;

	/** One line for the panel's status bar. */
	FString GetStatusText() const;

	const TArray<FMantleUploadAssetProgress>& GetAssets() const { return Assets; }
	const FMantleCosmeticPackage& GetPackage() const { return Package; }

	FString GetItemId() const { return ItemId; }
	FString GetItemStatus() const { return ItemStatus; }
	FString GetItemModerationState() const { return ItemModerationState; }
	/** The item hangs below the knee and Mantle marked it as one that may clip in motion (its mayClipInMotion). */
	bool GetItemMayClipInMotion() const { return bItemMayClipInMotion; }

	/** The note the result line carries for an item that may clip in motion. */
	static FString MayClipInMotionNote();

	/** Why the run stopped, for the creator; empty unless Failed. */
	FString GetFailure() const { return Failure; }

	/** What happened, oldest first: the panel's details list. */
	const TArray<FString>& GetLog() const { return Log; }

	/** The creator-facing sentence for a failed call about What ("Garment mesh"). */
	static FString DescribeFailure(const FMantleApiResult& Result, const FString& What);

	/**
	 * The creator-facing sentence for a refused PATCH of TargetItemId; the rest is DescribeFailure's.
	 */
	static FString DescribeUpdateFailure(const FMantleApiResult& Result, const FString& TargetItemId);

	/** A dropped connection, a timeout, a 5xx or a 429: worth trying again. */
	static bool IsRetryable(const FMantleApiResult& Result);

	/** The SHA-256 of an item body: the same body means the same item (resume). */
	static FString Fingerprint(const FString& ItemJson);

	/** Saved/Mantle/CosmeticUploads/<a name made from the source asset and slot>.json. */
	static FString DefaultResumeFile(const FString& SourceAssetPath, const FString& Slot);

private:
	void Send(const FMantleUploadCallRequest& Request);
	void OnResponse(uint32 ForGeneration, const FMantleApiResult& Result);

	void HandleDiff(const FMantleApiResult& Result);
	void HandleUpload(int32 AssetIndex, const FMantleApiResult& Result);
	void HandleGetAsset(int32 AssetIndex, const FMantleApiResult& Result);
	void HandleCreateItem(const FMantleApiResult& Result);
	void HandleGetItem(const FMantleApiResult& Result);

	void TickAssets(double Now);
	void ApplyAssetStatus(FMantleUploadAssetProgress& Progress, const FMantleUploadedAsset& Asset, double Now);
	void SchedulePoll(FMantleUploadAssetProgress& Progress, double Now);
	bool RetryLater(int32& Attempts, double& NextActionAt, const FMantleApiResult& Result, double Now);
	int32 FindAsset(const FString& Key) const;
	void Fail(const FString& Why);
	void Finish(const FString& Message);
	void AddLog(const FString& Line);

	bool ReadResume(FString& OutFingerprint, FString& OutItemId) const;
	void WriteResume(const FString& InFingerprint, const FString& InItemId) const;
	void ForgetResume() const;

	FMantleCosmeticPackage Package;
	TSharedRef<IMantleUploadTransport> Transport;
	FString ResumeFile;
	TFunction<double()> Clock;

	EMantleUploadPhase Phase = EMantleUploadPhase::Idle;
	TArray<FMantleUploadAssetProgress> Assets;

	bool bInFlight = false;
	uint32 Generation = 0;
	/** What the request in flight is about: an index into Assets, or INDEX_NONE. */
	int32 InFlightAsset = INDEX_NONE;
	EMantleUploadCall InFlightCall = EMantleUploadCall::DiffAssets;

	double NextActionAt = 0.0;
	int32 Attempts = 0;
	int32 ItemPolls = 0;
	double ScreeningSince = 0.0;
	double LastWaitLogAt = 0.0;

	FString ItemJson;
	FString ItemFingerprint;
	/**
	 * Sent with POST /avatar/items (#394): made once per item this run creates and kept across its
	 * retries, so a create that reached Mantle but lost its answer returns the same item.
	 */
	FString ItemIdempotencyKey;
	FString ItemId;
	/** ItemId came from the resume note (an earlier run's item), not from a create this run made. */
	bool bFollowingRemembered = false;
	FString ItemStatus;
	FString ItemModerationState;
	bool bItemMayClipInMotion = false;
	FString Failure;
	FString FinalMessage;
	TArray<FString> Log;
};
