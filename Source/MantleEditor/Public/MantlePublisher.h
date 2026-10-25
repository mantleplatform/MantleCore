// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceValidator.h"

class UWorld;
class FMantlePlacePublishSession;
struct FMantleApiResult;

struct FMantlePublishResult
{
	bool bSuccess = false;

	/** The place this level belongs to, once it has one. */
	FString PlaceId;

	/** Set when a revision went live. */
	int32 RevisionNumber = 0;
	bool bWentLive = false;

	FMantleValidationResult Validation;

	/** Written for the creator. */
	FString Message;

	/**
	 * Uploaded assets that need the creator, one row each, naming the Unreal asset and
	 * the fix: errors stopped the publish (rejected in screening, removed by a moderator, too
	 * large, refused by the place gate); warnings went live with a loss (a material published
	 * without one of its maps). Empty for a level built only from Mantle library assets.
	 */
	TArray<FMantleValidationIssue> Attention;

	/** Everything else is done, and some uploads wait for a human moderator: Publish again later. */
	bool bWaitingForReview = false;

	/** The creator cancelled. Nothing was published; what was uploaded stays for the next Publish. */
	bool bCancelled = false;

	/**
	 * GhzGarage/mantle#818: set when the place is over a limit that even thinning can fix: what thinning would do, in plain words. The
	 * panel offers it (Thin and publish); Mantle.Publish -Thin=auto chooses it up front.
	 */
	FString ThinOffer;

	/** The place file that was sent, kept for inspection and tests. */
	FString PlaceJson;
};

/**
 * #804: this process's memory, in megabytes, for the lines a harness greps (MANTLE_PUBLISH_DONE, MANTLE_F6 PUBLISHED). The peaks
 * are for the process's whole life, so a run that follows a bigger one reads that one's; the current figure is beside them.
 * On Windows the peak is the peak commit (the "private" figure Task Manager shows) and the working set the peak resident size.
 */
struct MANTLEEDITOR_API FMantlePublishMemory
{
	/** FPlatformMemoryStats::PeakUsedVirtual; -1 when not measured. */
	int64 PeakCommitMB = -1;

	/** FPlatformMemoryStats::PeakUsedPhysical. */
	int64 PeakWorkingSetMB = -1;

	/** FPlatformMemoryStats::UsedVirtual, now. */
	int64 UsedCommitMB = -1;

	/** The process now. */
	static FMantlePublishMemory Sample();

	bool IsKnown() const { return PeakCommitMB >= 0; }

	/** " peakMB=<n> peakWsMB=<n>" (a leading space), and " usedMB=<n>" after them when bWithUsed; empty when not measured. */
	FString ToFields(bool bWithUsed) const;
};

/**
 * The publish loop, from a level to something players can join.
 *
 * Validate, convert, upload, export, save, publish — in that order, stopping at the first
 * failure. The whole point of Mantle is that this is one action, so it lives in one place
 * rather than being assembled by each caller: FMantlePlacePublishSession. The editor's
 * menu runs it in the background with the Mantle Publish panel; PublishToCloud runs the same session
 * to the end before returning (tests, automation).
 */
class MANTLEEDITOR_API FMantlePublisher
{
public:
	/**
	 * Uploads the level and makes it live. Notes are the revision notes; empty is fine. Blocks until done or MaxBlockingSeconds pass.
	 * bThinInstances (GhzGarage/mantle#818): when the place is over the instance or file-size limit, remove instances evenly until it
	 * fits and say how many; off, such a place is refused before anything is uploaded.
	 */
	static FMantlePublishResult PublishToCloud(UWorld* World, const FString& Notes, double MaxBlockingSeconds, bool bThinInstances = false);

	/** #804: a blocking run collects garbage after this many conversion steps (FMantlePlacePublishSession::FTiming::CollectGarbageEverySteps). */
	static constexpr int32 CollectGarbageEverySteps = 100;

	/**
	 * Runs a started session to its end: Pump between ticks (the HTTP manager, or a test's fake
	 * Mantle), and once Clock passes MaxBlockingSeconds from the call, cancels it and returns. A
	 * session whose publish request is already on its way cannot be cancelled and runs to its end.
	 */
	static FMantlePublishResult RunToEnd(const TSharedRef<FMantlePlacePublishSession>& Session, double MaxBlockingSeconds,
		TFunctionRef<void()> Pump, TFunctionRef<double()> Clock);

	/** "90 seconds", "25 minutes", "3 hours": how long a blocking publish waited before giving up. */
	static FString DescribeWait(double Seconds);

	/** Longest revision note sent; the API reads at most 8 KiB of publish request. */
	static constexpr int32 MaxRevisionNotesLength = 1000;

	/**
	 * A failed API call, phrased for someone in the editor. The shared client's 401 text tells a
	 * player to sign in with Epic again, which means nothing here (#94), so a 401 says which
	 * token was refused and why (revoked, expired, a deleted account): the pasted creator token,
	 * which is forgotten so the next action asks for another, or MANTLE_AUTH_TOKEN, which only a
	 * new value and a restart can change. Everything else passes through.
	 */
	static FString DescribeApiFailure(const FMantleApiResult& Failed);
	/**
	 * The plain words for a public-moderation refusal (#614 stage 7), or empty for any other failure:
	 * 403 public_publishing_revoked (the account lost public publishing after repeated copyright strikes) and
	 * 409 public_duplicate_claim (another account uploaded the same content first). Both say the way out.
	 * Matched on the code, not the status.
	 */
	static FString DescribeModerationFailure(const FMantleApiResult& Failed);

	/** The same with the token's source and the API address given, so both wordings are testable. */
	static FString DescribeApiFailure(const FMantleApiResult& Failed, bool bTokenFromEnvironment, const FString& ApiAddress);

	/** True when the API says the level's remembered place no longer exists (deleted, or not this creator's). */
	static bool IsPlaceGone(const FMantleApiResult& Failed);

	/** True when publishing was refused only because moderation has not finished with new scripts yet. */
	static bool IsScriptsPending(const FMantleApiResult& Failed);

	/** True when the place gate refused only because uploaded assets are still being processed. */
	static bool IsAssetsPending(const FMantleApiResult& Failed);

	/** The publish request body: the notes, trimmed and capped, as properly escaped JSON. */
	static FString MakePublishBody(const FString& Notes);

private:
	static FMantlePublishResult Run(UWorld* World, bool bGoLive, const FString& Notes, double MaxBlockingSeconds, bool bThinInstances = false);
};
