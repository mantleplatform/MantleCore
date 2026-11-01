// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVoxelize.h"
#include "MantleVoxelWorldActor.h"

#include "Editor.h"
#include "Engine/Selection.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleDevVoxelize, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevVoxelizePrivate
{
	// Voxelize for headless runs (#168 step 6): the editor's selection, or every visible static mesh actor with "all".
	// Logs MANTLE_VOXELIZE ok=/error= and the world's problems; nothing is saved, so a run on a real level leaves it as it was on disk.
	void Run(const TArray<FString>& Args, UWorld* World)
	{
		if (!World)
		{
			UE_LOG(LogMantleDevVoxelize, Error, TEXT("MANTLE_VOXELIZE error=no level open"));
			return;
		}
		TArray<AActor*> Sources;
		if (Args.Contains(TEXT("all")))
		{
			for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
			{
				if (It->IsHidden() || It->IsTemporarilyHiddenInEditor())
				{
					continue;
				}
				// A sky dome or ground plane wider than a whole block world can hold is backdrop, not blocks.
				const FVector Size = It->GetComponentsBoundingBox(/*bNonColliding*/ true).GetSize();
				if (Size.X > 102400.0 || Size.Y > 102400.0 || Size.Z > 25600.0)
				{
					UE_LOG(LogMantleDevVoxelize, Display, TEXT("MANTLE_VOXELIZE skipped %s (%s cm, bigger than a block world)"), *It->GetActorLabel(), *Size.ToCompactString());
					continue;
				}
				Sources.Add(*It);
			}
		}
		else if (GEditor && GEditor->GetSelectedActors())
		{
			GEditor->GetSelectedActors()->GetSelectedObjects<AActor>(Sources);
		}
		FMantleVoxelizeOptions Options;
		Options.TileFolder = FMantleVoxelize::TileFolderFor(World);
		const FMantleVoxelizeResult Result = FMantleVoxelize::Voxelize(World, Sources, Options);
		if (!Result.IsOk())
		{
			UE_LOG(LogMantleDevVoxelize, Error, TEXT("MANTLE_VOXELIZE error=%s sources=%d"), *Result.Error, Sources.Num());
			return;
		}
		UE_LOG(LogMantleDevVoxelize, Display, TEXT("MANTLE_VOXELIZE ok sources=%d blocks=%lld types=%d size=%dx%dx%d"),
			Result.SourceActors, Result.Blocks, Result.TypeNames.Num(), Result.Size.X, Result.Size.Y, Result.Size.Z);
		if (!Result.Warning.IsEmpty())
		{
			UE_LOG(LogMantleDevVoxelize, Warning, TEXT("MANTLE_VOXELIZE warning=%s"), *Result.Warning);
		}
		for (const FMantleVoxelWorldProblem& Problem : Result.World->DescribeProblems())
		{
			UE_LOG(LogMantleDevVoxelize, Warning, TEXT("MANTLE_VOXELIZE problem=%s"), *Problem.What);
		}
	}

	FAutoConsoleCommandWithWorldAndArgs GMantleDevVoxelizeCommand(
		TEXT("Mantle.Dev.Voxelize"),
		TEXT("Mantle.Dev.Voxelize [all] -- voxelizes the selected actors (or every visible static mesh actor with 'all') into a block world, ")
		TEXT("as Tools > Mantle > Voxelize does, and logs MANTLE_VOXELIZE ok/error and the world's problems. Saves nothing."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&Run));
}
