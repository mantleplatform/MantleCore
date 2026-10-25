// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtrTemplates.h"

struct FMantleParseError;
struct FMantleValidationIssue;
class UObject;
class UWorld;

/**
 * World Partition levels (#450). Check and Export walk the level's loaded actors, and in a World
 * Partition level an actor in a cell the editor has not loaded is not in the world at all, so it
 * would be missing from the place without a word. Check this place does not change what the creator
 * has loaded, so it names World Partition and how to load the rest; Export refuses a partial level.
 * Publish (GhzGarage/mantle#818) loads every cell itself first (LoadAll) and lets them go when it ends.
 * LoadAll also loads the Level Instances the editor has not loaded (what is inside an unloaded one is otherwise left out), and
 * ReleaseLoaded unloads those again.
 */
class FMantlePlaceWorldPartition
{
public:
	/** One actor descriptor, reduced to what decides whether its absence loses content. */
	struct FActorEntry
	{
		FString Label;
		bool bLoaded = false;
		/** Runtime-only actors (HLODs among them) never exist in the editor and are never exported. */
		bool bRuntimeOnly = false;
		/** HLOD proxies are generated from the level's own actors, not placed content. */
		bool bHLOD = false;
	};

	/** How many examples a message names before it stops listing. */
	static constexpr int32 MaxNamedExamples = 5;

	/**
	 * The actors among Entries whose absence would lose content: the count, and up to
	 * MaxNamedExamples labels in a stable (sorted) order.
	 */
	static int32 CountUnloaded(const TArray<FActorEntry>& Entries, TArray<FString>& OutExamples);

	/** The actor descriptors of World's World Partition; empty for a level without one, or a game world. */
	static TArray<FActorEntry> GatherEntries(UWorld* World);

	/** The creator-facing problem sentence for Count unloaded actors. */
	static FString DescribeProblem(int32 Count, const TArray<FString>& Examples);

	/** The remedy that goes with DescribeProblem. */
	static FString DescribeRemedy();

	/** Check's half: an error on the place when World has unloaded actors. */
	static void CheckLevel(UWorld* World, TArray<FMantleValidationIssue>& OutIssues);

	/** Export's half: the same refusal as an export error, so no path publishes a partial place. */
	static void CheckExport(UWorld* World, TArray<FMantleParseError>& OutErrors);

	/** What LoadAll did. */
	struct FLoadResult
	{
		/** Actors in unloaded cells before, and still unloaded after (a data layer that is off, or a cell that would not load). */
		int32 UnloadedBefore = 0;
		int32 UnloadedAfter = 0;

		/** The process's committed memory (FPlatformMemory UsedVirtual) before and after, in megabytes; -1 when nothing was loaded. */
		int64 CommitBeforeMB = -1;
		int64 CommitAfterMB = -1;

		/** The adapter holding the loaded cells; ReleaseLoaded lets it go. */
		TWeakObjectPtr<UObject> Adapter;

		/** The Level Instance actors LoadAll loaded (ReleaseLoaded unloads them, nested ones first), and how many it tried and could not load. */
		TArray<TWeakObjectPtr<UObject>> LevelInstances;
		int32 LevelInstancesFailed = 0;

		bool bLoaded() const { return Adapter.IsValid() || LevelInstances.Num() > 0; }
		int32 Loaded() const { return UnloadedBefore - UnloadedAfter; }
	};

	/**
	 * Loads every cell of World's World Partition (GhzGarage/mantle#818), so Publish exports the whole level instead of refusing the
	 * unloaded part. Check this place never calls it: Check does not change what the creator has loaded. Does nothing, and
	 * allocates no adapter, for a level without one or with everything loaded. Editor only; logs what it loaded and the memory.
	 */
	static FLoadResult LoadAll(UWorld* World);

	/** How many levels deep LoadLevelInstances follows Level Instances inside Level Instances. */
	static constexpr int32 MaxLevelInstanceDepth = 8;

	/**
	 * Loads every Level Instance of World the editor has not loaded (GhzGarage/mantle#818), nested ones too, into Result.LevelInstances.
	 * A Packed Level Actor is not one (it publishes as instanced meshes). One that will not load is counted in LevelInstancesFailed and
	 * stays as it was: Export then says what is inside it is left out. Editor only.
	 */
	static void LoadLevelInstances(UWorld* World, FLoadResult& Result);

	/** LoadAll's World Partition half: loads every cell of World's World Partition, if it has one with unloaded actors. */
	static void LoadWorldPartitionCells(UWorld* World, FLoadResult& Result);

	/** Lets go of the cells LoadAll loaded, which unloads them (the cells the creator loaded themselves stay), and unloads the Level Instances it loaded. */
	static void ReleaseLoaded(UWorld* World, FLoadResult& Result);

	/** The plain-words note for a publish that loaded cells: how many actors, and the memory before and after. */
	static FString DescribeLoad(const FLoadResult& Result);
};
