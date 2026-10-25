// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHeadlessPublish.h"
#include "MantleApiClient.h"
#include "MantleRuntime.h"

#include "AssetRegistry/IAssetRegistry.h"
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
	static const FString ThinPrefix = TEXT("-Thin=");
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
		else if (Args[First].StartsWith(ThinPrefix, ESearchCase::IgnoreCase))
		{
			const FString Value = Args[First].RightChop(ThinPrefix.Len());
			if (Value.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
			{
				Parsed.bThin = true;
			}
			else if (!Value.Equals(TEXT("off"), ESearchCase::IgnoreCase))
			{
				Parsed.Error = FString::Printf(TEXT("-Thin wants auto (thin foliage and instances evenly if the place is too big) or off, not '%s'."), *Value);
				return Parsed;
			}
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

	UE_LOG(LogMantle, Display, TEXT("MANTLE_PUBLISH_START level=%s api=%s timeout=%.0fs thin=%s"),
		*World->GetOutermost()->GetName(), FMantleApiClient::BaseUrlResolver ? *FMantleApiClient::BaseUrlResolver() : TEXT("(default)"),
		Parsed.TimeoutSeconds, Parsed.bThin ? TEXT("auto") : TEXT("off"));
	const double Started = FPlatformTime::Seconds();
	Result = FMantlePublisher::PublishToCloud(World, Parsed.Notes, Parsed.TimeoutSeconds, Parsed.bThin);
	const double Seconds = FPlatformTime::Seconds() - Started;

	for (const FMantleValidationIssue& Issue : Result.Validation.Issues)
	{
		UE_LOG(LogMantle, Display, TEXT("MANTLE_PUBLISH_CHECK %s"), *MantleHeadlessPublishPrivate::OneLine(Issue.ToString()));
	}
	for (const FMantleValidationIssue& Issue : Result.Attention)
	{
		UE_LOG(LogMantle, Display, TEXT("MANTLE_PUBLISH_ATTENTION %s"), *MantleHeadlessPublishPrivate::OneLine(Issue.ToString()));
	}

	if (!Result.ThinOffer.IsEmpty())
	{
		UE_LOG(LogMantle, Display, TEXT("MANTLE_PUBLISH_THIN_OFFER %s Run Mantle.Publish -Thin=auto to publish it thinned."),
			*MantleHeadlessPublishPrivate::OneLine(Result.ThinOffer));
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
	FAutoConsoleCommandWithWorldAndArgs GMantlePublishCommand(
		TEXT("Mantle.Publish"),
		TEXT("Mantle.Publish [-TimeoutMinutes=N] [-Thin=auto] [notes...] -- publishes the open level exactly like Tools > Mantle > Publish ")
		TEXT("(check, convert, upload, wait for Mantle, go live), blocking the editor until it ends (default wait 30 minutes). ")
		TEXT("-Thin=auto thins foliage and instances evenly when the place is over the instance or file-size limit (never otherwise). ")
		TEXT("For automation; logs MANTLE_PUBLISH_DONE <placeId> <revision> or MANTLE_PUBLISH_FAILED <reason>."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			FMantleHeadlessPublish::Run(World, Args);
		}));
}
