// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePublisher.h"

class UWorld;

/**
 * Mantle.Publish [-TimeoutMinutes=N] [-Thin=auto] [notes...] (#344): Tools > Mantle > Publish for automation.
 *
 * The menu asks for notes and runs FMantlePlacePublishSession in the background under the Mantle
 * Publish panel; this runs the same session on the open level (FMantlePublisher::PublishToCloud:
 * the same level source, converter, upload queue and transport), blocking until it ends, so a
 * script can publish headlessly (the end-to-end harness runs it in a pythonscript commandlet with
 * -AllowCommandletRendering). It ends with exactly one line a harness can wait for:
 *
 *   MANTLE_PUBLISH_DONE <placeId> <revision> seconds=<s> peakMB=<n> peakWsMB=<n>
 *   MANTLE_PUBLISH_FAILED <reason on one line>
 *
 * peakMB is the editor process's peak commit (the "private" memory, FPlatformMemoryStats::PeakUsedVirtual) and peakWsMB its peak
 * working set (PeakUsedPhysical), in megabytes, over the process's whole life (#804): a run in an editor that already did more
 * reads that. MANTLE_F6 PUBLISHED carries the same two and usedMB, the commit at that moment, for a publish in a running editor.
 *
 * It blocks the editor while it runs, so creators use the menu.
 */
struct MANTLEEDITOR_API FMantleHeadlessPublish
{
	/** How long a run waits for Mantle when -TimeoutMinutes is not given. */
	static constexpr double DefaultTimeoutMinutes = 30.0;

	struct FArgs
	{
		FString Notes;
		double TimeoutSeconds = DefaultTimeoutMinutes * 60.0;
		/** -Thin=auto (GhzGarage/mantle#818): remove instances evenly when the place is over the instance or file-size limit. */
		bool bThin = false;
		/** Set when the arguments cannot be used; nothing is published then. */
		FString Error;
	};

	/** The console arguments: optional leading -TimeoutMinutes=N (N > 0) and -Thin=auto|off, in any order, then the notes, joined by spaces. */
	static FArgs ParseArgs(const TArray<FString>& Args);

	/**
	 * The final line for a finished publish (see above). Reasons are collapsed onto one line. Memory is appended to the DONE
	 * line when it was measured (FMantlePublishMemory::Sample); the default is not measured.
	 */
	static FString OutcomeLine(const FMantlePublishResult& Result, double Seconds, const FMantlePublishMemory& Memory = FMantlePublishMemory());

	/** The final line for a publish that never started (bad arguments, no level). */
	static FString FailureLine(const FString& Reason);

	/** Parses the arguments, publishes World (the editor world when null) and logs every line; returns the result. */
	static FMantlePublishResult Run(UWorld* World, const TArray<FString>& Args);
};
