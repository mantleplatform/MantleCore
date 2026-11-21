// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHeadlessPublish.h"
#include "MantleApiClient.h"
#include "MantleCosmeticUpload.h"
#include "MantlePlacePublishSession.h"
#include "MantleRuntime.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Containers/Ticker.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "RenderingThread.h"
#include "Misc/CoreDelegates.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "UObject/Package.h"

FMantleHeadlessPublish::FArgs FMantleHeadlessPublish::ParseArgs(const TArray<FString>& Args)
{
	FArgs Parsed;
	int32 First = 0;
	static const FString TimeoutPrefix = TEXT("-TimeoutMinutes=");
	// Leading options, in any order; the first word that is not one starts the notes.
	for (; First < Args.Num(); ++First)
	{
		if (Args[First].StartsWith(TimeoutPrefix, ESearchCase::IgnoreCase))
		{
			const FString Value = Args[First].RightChop(TimeoutPrefix.Len());
			const double Minutes = Value.IsNumeric() ? FCString::Atod(*Value) : 0.0;
			if (Minutes <= 0.0)
			{
				Parsed.Error = FString::Printf(TEXT("-TimeoutMinutes wants a number of minutes above zero, not '%s'."), *Value);
				return Parsed;
			}
			Parsed.TimeoutSeconds = Minutes * 60.0;
		}
		else
		{
			break;
		}
	}
	TArray<FString> Words;
	for (int32 Index = First; Index < Args.Num(); ++Index)
	{
		if (!Args[Index].IsEmpty())
		{
			Words.Add(Args[Index]);
		}
	}
	Parsed.Notes = FString::Join(Words, TEXT(" "));
	return Parsed;
}

namespace MantleHeadlessPublishPrivate
{
	/** One line for a log a harness greps: no line breaks, no runs of spaces. */
	FString OneLine(const FString& Text)
	{
		FString Line = Text;
		Line.ReplaceInline(TEXT("\r"), TEXT(" "));
		Line.ReplaceInline(TEXT("\n"), TEXT(" "));
		Line.ReplaceInline(TEXT("\t"), TEXT(" "));
		while (Line.ReplaceInline(TEXT("  "), TEXT(" ")) > 0)
		{
		}
		return Line.TrimStartAndEnd();
	}
}

FString FMantleHeadlessPublish::FailureLine(const FString& Reason)
{
	const FString Why = MantleHeadlessPublishPrivate::OneLine(Reason);
	return FString::Printf(TEXT("MANTLE_PUBLISH_FAILED %s"), Why.IsEmpty() ? TEXT("the publish stopped without saying why") : *Why);
}

FString FMantleHeadlessPublish::OutcomeLine(const FMantlePublishResult& Result, const double Seconds, const FMantlePublishMemory& Memory)
{
	if (Result.bSuccess && Result.bWentLive && !Result.PlaceId.IsEmpty() && Result.RevisionNumber > 0)
	{
		return FString::Printf(TEXT("MANTLE_PUBLISH_DONE %s %d seconds=%.1f%s"), *Result.PlaceId, Result.RevisionNumber, Seconds,
			*Memory.ToFields(/*bWithUsed*/ false));
	}
	FString Reason = Result.Message;
	if (Result.bSuccess && !Result.bWentLive)
	{
		// A publish that "succeeded" without going live (a save) is not what was asked for.
		Reason = TEXT("saved but did not go live: ") + Reason;
	}
	for (const FMantleValidationIssue& Issue : Result.Attention)
	{
		if (Issue.Severity == EMantleIssueSeverity::Error)
		{
			Reason += TEXT(" | ") + Issue.ToString();
		}
	}
	return FailureLine(Reason);
}

FMantlePublishResult FMantleHeadlessPublish::Run(UWorld* World, const TArray<FString>& Args)
{
	FMantlePublishResult Result;
	const FArgs Parsed = ParseArgs(Args);
	if (!Parsed.Error.IsEmpty())
	{
		Result.Message = Parsed.Error;
		UE_LOG(LogMantle, Error, TEXT("%s"), *FailureLine(Result.Message));
		return Result;
	}

	// The console hands over the world it ran in; from a commandlet or -ExecCmds that can be
	// none, and the level open in the editor is the one Tools > Mantle > Publish would publish.
	if ((!World || World->WorldType != EWorldType::Editor) && GEditor)
	{
		World = GEditor->GetEditorWorldContext().World();
	}
	if (!World)
	{
		Result.Message = TEXT("There is no level open to publish.");
		UE_LOG(LogMantle, Error, TEXT("%s"), *FailureLine(Result.Message));
		return Result;
	}

	// A commandlet may still be discovering assets. The converter's cache key follows package
	// dependencies through the asset registry, so a half-scanned registry would only cost cache
	// hits; waiting makes a headless republish hit the cache the way the editor's does.
	IAssetRegistry& Registry = IAssetRegistry::GetChecked();
	if (Registry.IsLoadingAssets())
	{
		UE_LOG(LogMantle, Display, TEXT("MANTLE_PUBLISH waiting for the asset registry"));
		Registry.WaitForCompletion();
	}

	UE_LOG(LogMantle, Display, TEXT("MANTLE_PUBLISH_START level=%s api=%s timeout=%.0fs"),
		*World->GetOutermost()->GetName(), FMantleApiClient::BaseUrlResolver ? *FMantleApiClient::BaseUrlResolver() : TEXT("(default)"),
		Parsed.TimeoutSeconds);
	const double Started = FPlatformTime::Seconds();
	Result = FMantlePublisher::PublishToCloud(World, Parsed.Notes, Parsed.TimeoutSeconds);
	const double Seconds = FPlatformTime::Seconds() - Started;

	for (const FMantleValidationIssue& Issue : Result.Validation.Issues)
	{
		UE_LOG(LogMantle, Display, TEXT("MANTLE_PUBLISH_CHECK %s"), *MantleHeadlessPublishPrivate::OneLine(Issue.ToString()));
	}
	for (const FMantleValidationIssue& Issue : Result.Attention)
	{
		UE_LOG(LogMantle, Display, TEXT("MANTLE_PUBLISH_ATTENTION %s"), *MantleHeadlessPublishPrivate::OneLine(Issue.ToString()));
	}

	const FString Line = OutcomeLine(Result, Seconds, FMantlePublishMemory::Sample());
	if (Line.StartsWith(TEXT("MANTLE_PUBLISH_DONE")))
	{
		UE_LOG(LogMantle, Display, TEXT("%s"), *Line);
	}
	else
	{
		UE_LOG(LogMantle, Error, TEXT("%s"), *Line);
	}
	return Result;
}

namespace MantleHeadlessPublishPrivate
{
	/** The running Mantle.PublishAsync, advanced once per editor frame. */
	TSharedPtr<FMantlePlacePublishSession> GAsyncSession;
	double GAsyncStarted = 0.0;
	double GAsyncGiveUpAt = 0.0;
	/** The level's actors whose components are still registered; taken out a batch per frame before conversion starts. */
	TArray<TWeakObjectPtr<AActor>> GAsyncToUnregister;
	int32 GAsyncUnregistered = 0;

	void LogOutcome(const FMantlePublishResult& Result, const double Seconds)
	{
		for (const FMantleValidationIssue& Issue : Result.Validation.Issues)
		{
			UE_LOG(LogMantle, Display, TEXT("MANTLE_PUBLISH_CHECK %s"), *OneLine(Issue.ToString()));
		}
		for (const FMantleValidationIssue& Issue : Result.Attention)
		{
			UE_LOG(LogMantle, Display, TEXT("MANTLE_PUBLISH_ATTENTION %s"), *OneLine(Issue.ToString()));
		}
		const FString Line = FMantleHeadlessPublish::OutcomeLine(Result, Seconds, FMantlePublishMemory::Sample());
		if (Line.StartsWith(TEXT("MANTLE_PUBLISH_DONE")))
		{
			UE_LOG(LogMantle, Display, TEXT("%s"), *Line);
		}
		else
		{
			UE_LOG(LogMantle, Error, TEXT("%s"), *Line);
		}
	}

	/**
	 * Mantle.PublishAsync: the same publish as Mantle.Publish, but advanced a step per editor frame (as Tools > Mantle > Publish runs)
	 * instead of in one call that never lets the engine end a frame. A big level needs those frames: City Sample's Big City, published
	 * blocking, piled up the engine's frame work (shader compiles of the material bake copies, render cleanups, game-thread tasks)
	 * until it crashed ("Consumed 1048576 lock free links"). Logs the same lines as Mantle.Publish and asks the editor to exit when done.
	 */
	void RunAsync(UWorld* World, const TArray<FString>& Args)
	{
		if (GAsyncSession.IsValid())
		{
			UE_LOG(LogMantle, Error, TEXT("%s"), *FMantleHeadlessPublish::FailureLine(TEXT("A Mantle.PublishAsync is already running.")));
			return;
		}
		const FMantleHeadlessPublish::FArgs Parsed = FMantleHeadlessPublish::ParseArgs(Args);
		if (!Parsed.Error.IsEmpty())
		{
			UE_LOG(LogMantle, Error, TEXT("%s"), *FMantleHeadlessPublish::FailureLine(Parsed.Error));
			return;
		}
		if ((!World || World->WorldType != EWorldType::Editor) && GEditor)
		{
			World = GEditor->GetEditorWorldContext().World();
		}
		FString WhyNot;
		if (!World || !FMantleApiClient::IsConfigured(WhyNot))
		{
			UE_LOG(LogMantle, Error, TEXT("%s"), *FMantleHeadlessPublish::FailureLine(World ? WhyNot : FString(TEXT("There is no level open to publish."))));
			return;
		}
		IAssetRegistry& Registry = IAssetRegistry::GetChecked();
		if (Registry.IsLoadingAssets())
		{
			Registry.WaitForCompletion();
		}
		UE_LOG(LogMantle, Display, TEXT("MANTLE_PUBLISH_START level=%s api=%s timeout=%.0fs async=1"),
			*World->GetOutermost()->GetName(), FMantleApiClient::BaseUrlResolver ? *FMantleApiClient::BaseUrlResolver() : TEXT("(default)"),
			Parsed.TimeoutSeconds);

		// As FMantlePublisher::Run sets it up: no one can make a rights declaration, so new vehicles stay private.
		const TSharedRef<FMantleLevelPublishSource> Source = MakeShared<FMantleLevelPublishSource>(World, Parsed.Notes);
		GAsyncSession = MakeShared<FMantlePlacePublishSession>(Source, MakeShared<FMantleApiUploadTransport>(), /*bGoLive*/ true, Parsed.Notes);
		GAsyncSession->VehicleChoice.bPublic = false;
		GAsyncSession->VehicleChoice.bKeepExistingVisibility = true;
		GAsyncSession->Timing.CollectGarbageEverySteps = FMantlePublisher::CollectGarbageEverySteps;
		GAsyncSession->Start();
		GAsyncStarted = FPlatformTime::Seconds();
		GAsyncGiveUpAt = GAsyncStarted + Parsed.TimeoutSeconds;

		// Start() has checked and exported the level; conversion only reads assets. With a big level's every actor still registered,
		// each material bake copy's PostEditChange refreshes the render state of components across the world, which on City Sample's
		// Big City (89,586 actors, 8.8M instances) flooded the engine's queues in one step. The editor closes after this command, so
		// the level's components are unregistered first, a batch per frame so that this does not flood them either.
		GAsyncToUnregister.Reset();
		GAsyncUnregistered = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			GAsyncToUnregister.Add(*It);
		}
		UE_LOG(LogMantle, Display, TEXT("MANTLE_PUBLISH_ASYNC unregistering %d actors' components before conversion"), GAsyncToUnregister.Num());

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
		{
			if (!GAsyncSession.IsValid())
			{
				return false;
			}
			if (GAsyncToUnregister.Num() > 0)
			{
				// All of them before the editor renders another frame of the whole level (rendering it is what ran out), the render
				// thread flushed every 2,000 so the removals themselves do not pile up.
				for (int32 Index = 0; Index < GAsyncToUnregister.Num(); ++Index)
				{
					if (AActor* Actor = GAsyncToUnregister[Index].Get())
					{
						Actor->UnregisterAllComponents();
					}
					if (Index % 2000 == 1999)
					{
						FlushRenderingCommands();
					}
				}
				GAsyncUnregistered = GAsyncToUnregister.Num();
				GAsyncToUnregister.Reset();
				FlushRenderingCommands();
				UE_LOG(LogMantle, Display, TEXT("MANTLE_PUBLISH_ASYNC unregistered %d actors; converting"), GAsyncUnregistered);
				return true;
			}
			if (!GAsyncSession->IsFinished())
			{
				// Up to half a second of steps per frame: a step is one mesh or material, and one per frame left the editor idle
				// between frames (City Sample's materials went at about one every two seconds with the CPU at 8%).
				const double FrameUntil = FPlatformTime::Seconds() + 0.5;
				do
				{
					GAsyncSession->Tick();
				}
				while (!GAsyncSession->IsFinished() && FPlatformTime::Seconds() < FrameUntil);
				if (!GAsyncSession->IsFinished() && FPlatformTime::Seconds() > GAsyncGiveUpAt)
				{
					GAsyncSession->Cancel();
				}
				if (!GAsyncSession->IsFinished())
				{
					return true;
				}
			}
			LogOutcome(GAsyncSession->GetResult(), FPlatformTime::Seconds() - GAsyncStarted);
			GAsyncSession.Reset();
			RequestEngineExit(TEXT("Mantle.PublishAsync finished"));
			return false;
		}));
	}

	FAutoConsoleCommandWithWorldAndArgs GMantlePublishAsyncCommand(
		TEXT("Mantle.PublishAsync"),
		TEXT("Mantle.PublishAsync [-TimeoutMinutes=N] [notes...] -- Mantle.Publish advanced one step per editor frame instead of blocking, ")
		TEXT("for big levels; logs the same MANTLE_PUBLISH_DONE / MANTLE_PUBLISH_FAILED line and asks the editor to exit when it ends."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			RunAsync(World, Args);
		}));

	FAutoConsoleCommandWithWorldAndArgs GMantlePublishCommand(
		TEXT("Mantle.Publish"),
		TEXT("Mantle.Publish [-TimeoutMinutes=N] [notes...] -- publishes the open level exactly like Tools > Mantle > Publish ")
		TEXT("(check, convert, upload, wait for Mantle, go live), blocking the editor until it ends (default wait 30 minutes). ")
		TEXT("For automation; logs MANTLE_PUBLISH_DONE <placeId> <revision> or MANTLE_PUBLISH_FAILED <reason>."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			FMantleHeadlessPublish::Run(World, Args);
		}));
}
