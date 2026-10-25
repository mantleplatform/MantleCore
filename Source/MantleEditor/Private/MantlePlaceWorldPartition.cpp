// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceWorldPartition.h"
#include "MantlePlaceTypes.h"
#include "MantlePlaceValidator.h"
#include "MantleRuntime.h"

#include "Engine/World.h"
#include "EngineDefines.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "LevelInstance/LevelInstanceInterface.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "WorldPartition/WorldPartition.h"
#include "HAL/PlatformMemory.h"
#include "WorldPartition/LoaderAdapter/LoaderAdapterShape.h"
#include "WorldPartition/WorldPartitionEditorLoaderAdapter.h"
#include "WorldPartition/WorldPartitionHelpers.h"
#if WITH_EDITOR
#include "WorldPartition/HLOD/HLODActor.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#endif

int32 FMantlePlaceWorldPartition::CountUnloaded(const TArray<FActorEntry>& Entries, TArray<FString>& OutExamples)
{
	OutExamples.Reset();
	TArray<FString> Labels;
	for (const FActorEntry& Entry : Entries)
	{
		if (Entry.bLoaded || Entry.bRuntimeOnly || Entry.bHLOD)
		{
			continue;
		}
		Labels.Add(Entry.Label);
	}

	Labels.Sort();
	for (int32 Index = 0; Index < Labels.Num() && Index < MaxNamedExamples; ++Index)
	{
		OutExamples.Add(Labels[Index]);
	}
	return Labels.Num();
}

TArray<FMantlePlaceWorldPartition::FActorEntry> FMantlePlaceWorldPartition::GatherEntries(UWorld* World)
{
	TArray<FActorEntry> Entries;
#if WITH_EDITOR
	// A game (PIE) world streams its cells from generated data rather than the editor's actor
	// descriptors; Check and Export only ever read the editor's level.
	if (!IsValid(World) || World->IsGameWorld())
	{
		return Entries;
	}

	UWorldPartition* WorldPartition = World->GetWorldPartition();
	if (!WorldPartition || !WorldPartition->IsInitialized())
	{
		return Entries;
	}

	FWorldPartitionHelpers::ForEachActorDescInstance(WorldPartition,
		[&Entries](const FWorldPartitionActorDescInstance* Desc)
		{
			if (Desc)
			{
				const UClass* NativeClass = Desc->GetActorNativeClass();
				FActorEntry Entry;
				Entry.Label = Desc->GetActorLabelOrName().ToString();
				Entry.bLoaded = Desc->IsLoaded();
				Entry.bRuntimeOnly = Desc->GetActorIsRuntimeOnly();
				Entry.bHLOD = NativeClass && NativeClass->IsChildOf(AWorldPartitionHLOD::StaticClass());
				Entries.Add(MoveTemp(Entry));
			}
			return true;
		});
#endif
	return Entries;
}

FString FMantlePlaceWorldPartition::DescribeProblem(int32 Count, const TArray<FString>& Examples)
{
	FString Named;
	if (Examples.Num() > 0)
	{
		Named = FString::Printf(TEXT(" (%s%s)"),
			*FString::Join(Examples, TEXT(", ")),
			Count > Examples.Num() ? TEXT(", and others") : TEXT(""));
	}
	return FString::Printf(
		TEXT("This is a World Partition level, and %d actor%s in cells the editor has not loaded%s would be left out of the place."),
		Count,
		Count == 1 ? TEXT("") : TEXT("s"),
		*Named);
}

FString FMantlePlaceWorldPartition::DescribeRemedy()
{
	return TEXT("Publish loads every cell itself. To check now, load the whole level: in the World Partition window select every cell and choose Load Region From Selection, or turn off Enable Streaming in World Settings.");
}

void FMantlePlaceWorldPartition::CheckLevel(UWorld* World, TArray<FMantleValidationIssue>& OutIssues)
{
	TArray<FString> Examples;
	const int32 Count = CountUnloaded(GatherEntries(World), Examples);
	if (Count == 0)
	{
		return;
	}

	OutIssues.Add(FMantleValidationIssue{
		EMantleIssueSeverity::Error,
		FString(),
		DescribeProblem(Count, Examples),
		DescribeRemedy() });
}

void FMantlePlaceWorldPartition::CheckExport(UWorld* World, TArray<FMantleParseError>& OutErrors)
{
	TArray<FString> Examples;
	const int32 Count = CountUnloaded(GatherEntries(World), Examples);
	if (Count == 0)
	{
		return;
	}

	UE_LOG(LogMantle, Warning, TEXT("Export refused: %d World Partition actor(s) are not loaded."), Count);
	OutErrors.Add(FMantleParseError{
		TEXT("/worldPartition"),
		DescribeProblem(Count, Examples) + TEXT(" ") + DescribeRemedy() });
}

FMantlePlaceWorldPartition::FLoadResult FMantlePlaceWorldPartition::LoadAll(UWorld* World)
{
	FLoadResult Result;
#if WITH_EDITOR
	if (!IsValid(World) || World->IsGameWorld())
	{
		return Result;
	}
	// Cells first: a Level Instance may sit in a cell that was not loaded.
	LoadWorldPartitionCells(World, Result);
	LoadLevelInstances(World, Result);
#endif
	return Result;
}

void FMantlePlaceWorldPartition::LoadWorldPartitionCells(UWorld* World, FLoadResult& Result)
{
#if WITH_EDITOR
	UWorldPartition* WorldPartition = World->GetWorldPartition();
	if (!WorldPartition || !WorldPartition->IsInitialized())
	{
		return;
	}
	TArray<FString> Examples;
	Result.UnloadedBefore = CountUnloaded(GatherEntries(World), Examples);
	if (Result.UnloadedBefore == 0)
	{
		return;
	}

	Result.CommitBeforeMB = static_cast<int64>(FPlatformMemory::GetStats().UsedVirtual >> 20);
	UE_LOG(LogMantle, Log, TEXT("World Partition: loading %d actors in unloaded cells for the publish (committed memory %lld MB)"),
		Result.UnloadedBefore, Result.CommitBeforeMB);
	// The same adapter the World Partition window's Load Region makes, over the whole world.
	UWorldPartitionEditorLoaderAdapter* Adapter = WorldPartition->CreateEditorLoaderAdapter<FLoaderAdapterShape>(
		World, FBox(FVector(-HALF_WORLD_MAX), FVector(HALF_WORLD_MAX)), TEXT("Mantle publish"));
	Adapter->GetLoaderAdapter()->Load();
	Result.Adapter = Adapter;

	Result.UnloadedAfter = CountUnloaded(GatherEntries(World), Examples);
	Result.CommitAfterMB = static_cast<int64>(FPlatformMemory::GetStats().UsedVirtual >> 20);
	UE_LOG(LogMantle, Log, TEXT("World Partition: %s"), *DescribeLoad(Result));
#endif
}

void FMantlePlaceWorldPartition::LoadLevelInstances(UWorld* World, FLoadResult& Result)
{
#if WITH_EDITOR
	ULevelInstanceSubsystem* Subsystem = IsValid(World) && !World->IsGameWorld() ? World->GetSubsystem<ULevelInstanceSubsystem>() : nullptr;
	if (!Subsystem)
	{
		return;
	}
	TSet<const AActor*> Tried;
	for (int32 Depth = 0; Depth < MaxLevelInstanceDepth; ++Depth)
	{
		// The unloaded ones first, then load them: loading changes the world's actor list.
		TArray<AActor*> Unloaded;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			ILevelInstanceInterface* Instance = IsValid(Actor) ? Cast<ILevelInstanceInterface>(Actor) : nullptr;
			if (!Instance || Tried.Contains(Actor) || !Instance->IsLoadingEnabled() || !Instance->IsWorldAssetValid()
				|| Instance->IsLoaded() || Subsystem->IsLoading(Instance) || Instance->IsInAnyEditMode())
			{
				continue;
			}
			// A Packed Level Actor publishes as instanced meshes; its level is not loaded for it.
			bool bPacked = false;
			for (const UClass* Class = Actor->GetClass(); Class && !bPacked; Class = Class->GetSuperClass())
			{
				bPacked = Class->GetFName() == FName(TEXT("PackedLevelActor"));
			}
			if (!bPacked)
			{
				Unloaded.Add(Actor);
			}
		}
		if (Unloaded.Num() == 0)
		{
			break;
		}
		for (AActor* Actor : Unloaded)
		{
			Tried.Add(Actor);
			if (!IsValid(Actor))
			{
				continue;
			}
			ILevelInstanceInterface* Instance = CastChecked<ILevelInstanceInterface>(Actor);
			Subsystem->BlockLoadLevelInstance(Instance);
			if (Instance->IsLoaded())
			{
				Result.LevelInstances.Add(Actor);
				UE_LOG(LogMantle, Log, TEXT("Level Instance: loaded '%s' (%s) for the publish"), *Actor->GetActorLabel(), *Instance->GetWorldAsset().GetAssetName());
			}
			else
			{
				++Result.LevelInstancesFailed;
				UE_LOG(LogMantle, Warning, TEXT("Level Instance: '%s' (%s) would not load"), *Actor->GetActorLabel(), *Instance->GetWorldAsset().GetAssetName());
			}
		}
	}
#endif
}

void FMantlePlaceWorldPartition::ReleaseLoaded(UWorld* World, FLoadResult& Result)
{
#if WITH_EDITOR
	// The Level Instances this publish loaded go first, the last loaded (the nested ones) first; the ones the creator loaded stay.
	ULevelInstanceSubsystem* Subsystem = IsValid(World) ? World->GetSubsystem<ULevelInstanceSubsystem>() : nullptr;
	for (int32 Index = Result.LevelInstances.Num() - 1; Index >= 0; --Index)
	{
		AActor* Actor = Cast<AActor>(Result.LevelInstances[Index].Get());
		ILevelInstanceInterface* Instance = IsValid(Actor) ? Cast<ILevelInstanceInterface>(Actor) : nullptr;
		if (Subsystem && Instance && Instance->IsLoaded() && !Instance->IsInAnyEditMode())
		{
			Subsystem->BlockUnloadLevelInstance(Instance);
			UE_LOG(LogMantle, Log, TEXT("Level Instance: let go of '%s'"), *Actor->GetActorLabel());
		}
	}
	Result.LevelInstances.Reset();

	UWorldPartitionEditorLoaderAdapter* Adapter = Cast<UWorldPartitionEditorLoaderAdapter>(Result.Adapter.Get());
	Result.Adapter.Reset();
	if (!Adapter)
	{
		return;
	}
	if (IsValid(World))
	{
		if (UWorldPartition* WorldPartition = World->GetWorldPartition())
		{
			WorldPartition->ReleaseEditorLoaderAdapter(Adapter);
			UE_LOG(LogMantle, Log, TEXT("World Partition: let go of the cells the publish loaded"));
		}
	}
#endif
}

FString FMantlePlaceWorldPartition::DescribeLoad(const FLoadResult& Result)
{
	FString Text;
	if (Result.UnloadedBefore > 0)
	{
		Text = FString::Printf(TEXT("Loaded %d World Partition actors in cells that were not loaded, so they publish with the rest (committed memory %lld MB, was %lld MB)."),
			Result.Loaded(), Result.CommitAfterMB, Result.CommitBeforeMB);
		if (Result.UnloadedAfter > 0)
		{
			Text += FString::Printf(TEXT(" %d actors are still not loaded (a data layer that is off, or a cell that would not load)."), Result.UnloadedAfter);
		}
	}
	if (Result.LevelInstances.Num() > 0 || Result.LevelInstancesFailed > 0)
	{
		Text += Text.IsEmpty() ? TEXT("") : TEXT(" ");
		Text += FString::Printf(TEXT("Loaded %d Level Instance(s) that were not loaded in the editor, so what is inside them publishes with the rest."), Result.LevelInstances.Num());
		if (Result.LevelInstancesFailed > 0)
		{
			Text += FString::Printf(TEXT(" %d would not load, so what is inside them is left out."), Result.LevelInstancesFailed);
		}
	}
	return Text;
}
