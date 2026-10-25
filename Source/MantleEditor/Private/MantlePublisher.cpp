// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePublisher.h"
#include "MantleApiClient.h"
#include "MantleConnection.h"
#include "MantleCosmeticUpload.h"
#include "MantlePlacePublishSession.h"
#include "MantleRuntime.h"

#include "Engine/World.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonWriter.h"

FMantlePublishMemory FMantlePublishMemory::Sample()
{
	constexpr uint64 Megabyte = 1024 * 1024;
	const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();
	FMantlePublishMemory Memory;
	Memory.PeakCommitMB = static_cast<int64>(Stats.PeakUsedVirtual / Megabyte);
	Memory.PeakWorkingSetMB = static_cast<int64>(Stats.PeakUsedPhysical / Megabyte);
	Memory.UsedCommitMB = static_cast<int64>(Stats.UsedVirtual / Megabyte);
	return Memory;
}

FString FMantlePublishMemory::ToFields(const bool bWithUsed) const
{
	if (!IsKnown())
	{
		return FString();
	}
	FString Fields = FString::Printf(TEXT(" peakMB=%lld peakWsMB=%lld"), PeakCommitMB, PeakWorkingSetMB);
	if (bWithUsed)
	{
		Fields += FString::Printf(TEXT(" usedMB=%lld"), UsedCommitMB);
	}
	return Fields;
}

FString FMantlePublisher::DescribeApiFailure(const FMantleApiResult& Failed)
{
	return DescribeApiFailure(Failed, FMantleConnection::IsAuthTokenFromEnvironment(), FMantleApiClient::BaseUrlResolver());
}

FString FMantlePublisher::DescribeApiFailure(
	const FMantleApiResult& Failed, const bool bTokenFromEnvironment, const FString& ApiAddress)
{
	if (Failed.StatusCode != 401)
	{
		return Failed.Message;
	}
	// The API's 401 codes for a creator token; anything else (a developer token a local
	// API refused, an older API) reads as not accepted.
	const TCHAR* Why = Failed.Code == TEXT("expired_token")
		? TEXT("has expired")
		: Failed.Code == TEXT("account_deleted")
			? TEXT("belongs to a Mantle account that has been deleted")
			: Failed.Code == TEXT("invalid_token")
				? TEXT("was revoked or is not a valid creator token")
				: TEXT("was not accepted");
	if (bTokenFromEnvironment)
	{
		// Signing out cannot help: the variable takes precedence over anything pasted.
		return FString::Printf(
			TEXT("The MANTLE_AUTH_TOKEN this editor was started with %s (Mantle at %s). ")
			TEXT("Create a new creator token in Mantle's Settings > Account, set MANTLE_AUTH_TOKEN to it and restart the editor."),
			Why, *ApiAddress);
	}
	// FMantleCreatorSignIn has already forgotten the refused token (it listens for the 401), so
	// the next Save or Publish asks for a new one.
	return FString::Printf(
		TEXT("Your creator token %s (Mantle at %s), so you have been signed out. ")
		TEXT("Create a new one in Mantle's Settings > Account, then choose Publish again to paste it."),
		Why, *ApiAddress);
}

FString FMantlePublisher::DescribeModerationFailure(const FMantleApiResult& Failed)
{
	if (Failed.bSuccess)
	{
		return FString();
	}
	if (Failed.Code.Equals(TEXT("public_publishing_revoked"), ESearchCase::CaseSensitive))
	{
		return TEXT("Public publishing is turned off for this account after repeated copyright strikes. You can still publish privately.");
	}
	if (Failed.Code.Equals(TEXT("public_duplicate_claim"), ESearchCase::CaseSensitive))
	{
		return TEXT("Another account uploaded this content first. Publish it privately, or contact support if it is yours.");
	}
	return FString();
}

bool FMantlePublisher::IsPlaceGone(const FMantleApiResult& Failed)
{
	// The code, not only the status: a 404 from something that is not the Mantle API (a
	// wrong address, a proxy) must not make the editor forget a place that still exists.
	return !Failed.bSuccess && Failed.StatusCode == 404 && Failed.Code == TEXT("not_found");
}

bool FMantlePublisher::IsScriptsPending(const FMantleApiResult& Failed)
{
	// Only pending. Rejected, missing or incompatible scripts will not change by waiting.
	return !Failed.bSuccess && Failed.Code == TEXT("scripts_pending");
}

bool FMantlePublisher::IsAssetsPending(const FMantleApiResult& Failed)
{
	// Only pending: rejected, missing or not-owned assets will not change by waiting.
	return !Failed.bSuccess && Failed.Code == TEXT("assets_pending");
}

FString FMantlePublisher::MakePublishBody(const FString& Notes)
{
	FString Trimmed = Notes.TrimStartAndEnd();
	if (Trimmed.Len() > MaxRevisionNotesLength)
	{
		Trimmed.LeftInline(MaxRevisionNotesLength);
	}

	FString Body;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("notes"), Trimmed);
	Writer->WriteObjectEnd();
	Writer->Close();
	return Body;
}

FMantlePublishResult FMantlePublisher::PublishToCloud(UWorld* World, const FString& Notes, const double MaxBlockingSeconds, const bool bThinInstances)
{
	return Run(World, /*bGoLive*/ true, Notes, MaxBlockingSeconds, bThinInstances);
}

FString FMantlePublisher::DescribeWait(const double Seconds)
{
	const int64 Whole = FMath::Max<int64>(FMath::RoundToInt64(Seconds), 0);
	if (Whole < 120)
	{
		return FString::Printf(TEXT("%lld second%s"), Whole, Whole == 1 ? TEXT("") : TEXT("s"));
	}
	if (Whole < 2 * 60 * 60)
	{
		return FString::Printf(TEXT("%lld minutes"), (Whole + 30) / 60);
	}
	return FString::Printf(TEXT("%lld hours"), (Whole + 30 * 60) / (60 * 60));
}

FMantlePublishResult FMantlePublisher::RunToEnd(const TSharedRef<FMantlePlacePublishSession>& Session, const double MaxBlockingSeconds,
	const TFunctionRef<void()> Pump, const TFunctionRef<double()> Clock)
{
	// No cutoff while Mantle cooks and screens (the panel's publish has none either), but a
	// blocking caller (tests, automation) must come back some time.
	const double GiveUpAt = Clock() + FMath::Max(MaxBlockingSeconds, 0.0);
	while (!Session->IsFinished())
	{
		Pump();
		Session->Tick();
		if (!Session->IsFinished() && Clock() > GiveUpAt && Session->Cancel())
		{
			FMantlePublishResult Stopped = Session->GetResult();
			Stopped.Message = FString::Printf(TEXT("Stopped waiting for Mantle after %s. Nothing was published; Publish again continues from here."),
				*DescribeWait(MaxBlockingSeconds));
			return Stopped;
		}
	}
	return Session->GetResult();
}

FMantlePublishResult FMantlePublisher::Run(UWorld* World, bool bGoLive, const FString& Notes, const double MaxBlockingSeconds, const bool bThinInstances)
{
	FMantlePublishResult Result;

	if (!World)
	{
		Result.Message = TEXT("There is no level open to publish.");
		return Result;
	}

	FString WhyNot;
	if (!FMantleApiClient::IsConfigured(WhyNot))
	{
		Result.Message = WhyNot;
		return Result;
	}

	// The same session the editor's Publish runs in the background, run to the end here:
	// one implementation of the flow, whoever starts it. The editor is not ticking while this
	// runs, so the HTTP manager is driven directly, as FMantleApiClient::Send does.
	const TSharedRef<FMantleLevelPublishSource> Source = MakeShared<FMantleLevelPublishSource>(World, Notes);
	Source->SetThinInstances(bThinInstances);
	const TSharedRef<FMantlePlacePublishSession> Session = MakeShared<FMantlePlacePublishSession>(
		Source, MakeShared<FMantleApiUploadTransport>(), bGoLive, Notes);
	// A blocking run has nobody to make the rights declaration a public library item needs, so vehicles it publishes are
	// private (as UploadCosmetic's are). Without this the Library step would wait for a choice no one can make.
	Session->VehicleChoice.bPublic = false;
	Session->VehicleChoice.bKeepExistingVisibility = true;
	UE_LOG(LogMantle, Log, TEXT("Publish: no one can make the rights declaration here, so new vehicles become private library items and existing ones keep their visibility."));
	// This run blocks the editor, so nothing else collects garbage while a big level converts (the panel's publish runs
	// under the editor's own ticks and leaves it at 0). The session's source pins what the steps still need.
	Session->Timing.CollectGarbageEverySteps = CollectGarbageEverySteps;
	Session->Start();
	bool bFirstPump = true;
	return RunToEnd(Session, MaxBlockingSeconds,
		[&bFirstPump]
		{
			// The same rhythm as before: HTTP, tick, a short sleep, HTTP again.
			if (!bFirstPump)
			{
				FPlatformProcess::Sleep(0.005f);
			}
			bFirstPump = false;
			FHttpModule::Get().GetHttpManager().Tick(0.016f);
		},
		[] { return FPlatformTime::Seconds(); });
}
