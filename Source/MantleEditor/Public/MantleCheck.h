// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceValidator.h"
#include "MantleWaterBodyExport.h"

class UWorld;

/** What Mantle.Check found. */
struct MANTLEEDITOR_API FMantleCheckReport
{
	/** Check this place's findings, then the export's errors, in that order. */
	TArray<FMantleValidationIssue> Issues;

	/** The place file as Publish would send it (provisional ids); empty when the check found errors before exporting. */
	FString PlaceJson;

	/** The place file's size and its instanced mesh count; 0 when nothing was exported. */
	int64 PlaceBytes = 0;
	int64 Instances = 0;

	/** Mesh entities and instance sets that got a drawDistance (GhzGarage/mantle#630). */
	int32 DrawDistances = 0;

	/** Static mesh actors folded into instance sets, and instances removed by -Thin=auto. */
	int32 GroupedActors = 0;
	int64 ThinnedInstances = 0;

	/** When the place is too big and thinning would fix it: what it would do. */
	FString ThinOffer;

	/** The Water plugin's bodies and zone in the level (GhzGarage/mantle#997), published as WaterBody entities and settings.water. Empty when the plugin is off. */
	FMantleWaterBodies Water;

	/** The coverage report, when the level was exported. */
	FMantleCoverage Coverage;

	/** Where place.json and check.txt were written (-Export); empty for none. */
	FString ExportedTo;

	/** The World Partition note when the check loaded cells. */
	FString WorldPartitionNote;

	int32 NumErrors() const;
	int32 NumWarnings() const;
	bool IsReady() const { return NumErrors() == 0; }
};

/**
 * Mantle.Check [-Export=<dir>] [-Thin=auto] (GhzGarage/mantle#818): Publish's check and export on the open level, without
 * publishing and without contacting the server (nothing is signed in, converted or uploaded).
 *
 * It loads every World Partition cell as Publish does, runs Check this place, and, when that passes, exports the place file the
 * way Publish writes it first (the same grouping, thinning and size measurement; assets carry the provisional ids an offline
 * export gives them). -Export=<dir> writes place.json and check.txt there. -Thin=auto removes instances evenly when the place is
 * over a limit, as Mantle.Publish -Thin=auto does. It ends with one line a harness can grep:
 *
 *   MANTLE_CHECK_DONE ready=<1|0> errors=<n> warnings=<n> placeBytes=<n> instances=<n> grouped=<n> thinned=<n>
 *
 * and before it, one line per finding: MANTLE_CHECK_ERROR <text> and MANTLE_CHECK_WARNING <text>, plus the usual MANTLE_COVERAGE
 * lines. Mesh and material conversion is not run (it is Publish's Prepare step), so its findings are not in the report.
 */
struct MANTLEEDITOR_API FMantleCheck
{
	struct FArgs
	{
		/** -Export=<dir>; empty for none. */
		FString ExportDir;

		/** -Thin=auto. */
		bool bThin = false;

		/** Set when the arguments cannot be used; nothing is checked then. */
		FString Error;
	};

	static FArgs ParseArgs(const TArray<FString>& Args);

	/** Runs the check on World (the editor world when null) and logs every line. */
	static FMantleCheckReport Run(UWorld* World, const FArgs& Args);

	/** The check without logging or writing files: Check this place, then the export, as described above. */
	static FMantleCheckReport Evaluate(UWorld* World, bool bThin);

	/** The finishing line, as described above. */
	static FString DoneLine(const FMantleCheckReport& Report);

	/** "MANTLE_CHECK_ERROR ..." / "MANTLE_CHECK_WARNING ...", one line for each finding. */
	static TArray<FString> IssueLines(const FMantleCheckReport& Report);

	/** Adds the place-size finding to Validation when the level, exported, is over a limit (the Check this place menu uses it). */
	static void AppendPlaceBudget(UWorld* World, FMantleValidationResult& Validation);
};
