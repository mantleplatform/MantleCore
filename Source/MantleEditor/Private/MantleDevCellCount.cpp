// Copyright Joshua Eger. All Rights Reserved.
//
// Maintainer command: Mantle.Dev.CellCount [X:Y[:Z] ...]
// Exports the open level as Publish would (every World Partition cell loaded, nothing uploaded, nothing saved) and counts what the
// cells hold and what a player would load, by the game's own cell rules (FMantleCellStreamPlan, FMantleFarSetPlan). The proof for
// step 3 of the big-map plan (fable-review.md 9) before any publish. A piece is a StaticMesh entity or one instance of an
// InstancedMesh. Lines:
//   MANTLE_CELLCOUNT_LEVEL   level, cell size, cells, pieces and visible pieces, the cells' draw distances
//   MANTLE_CELLCOUNT_ROOT    the always-loaded root's pieces
//   MANTLE_CELLCOUNT_DIST    every piece by the draw distance it publishes with (60 m band, no cut-off, a creator's own, none)
//   MANTLE_CELLCOUNT_AT      at a point (the first PlayerStart, then each X:Y[:Z] argument, cm) and a reach scale (1 = full ladder, 0.25 = the
//                            ladder's bottom): visible pieces loaded per level and in the root, how many of those are inside their
//                            own cut-off, and the far twins' pieces wanted there
//   MANTLE_CELLCOUNT_DONE
#include "MantleCells.h"
#include "MantleCellStreamPlan.h"
#include "MantleDrawDistance.h"
#include "MantleFarSetPlan.h"
#include "MantleFarSetSettings.h"
#include "MantlePlaceCells.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceWorldPartition.h"
#include "MantleRuntime.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDevCellCountPrivate
{
	/** The pieces of an entity (a StaticMesh 1, an InstancedMesh its instances), 0 for anything else. */
	int64 PiecesOf(const FMantleEntity& Entity)
	{
		if (Entity.Props.TryGet<FMantleStaticMeshProps>())
		{
			return 1;
		}
		if (const FMantleInstancedMeshProps* Instanced = Entity.Props.TryGet<FMantleInstancedMeshProps>())
		{
			return Instanced->Instances.Num();
		}
		return 0;
	}

	/** Each piece's world pivot: an InstancedMesh's instances are written relative to its anchor (the entity's location). */
	template <typename FVisit>
	void ForEachPivot(const FMantleEntity& Entity, FVisit&& Visit)
	{
		if (Entity.Props.TryGet<FMantleStaticMeshProps>())
		{
			Visit(Entity.Transform.GetLocation());
		}
		else if (const FMantleInstancedMeshProps* Instanced = Entity.Props.TryGet<FMantleInstancedMeshProps>())
		{
			for (const FTransform& Instance : Instanced->Instances)
			{
				Visit(Entity.Transform.TransformPosition(Instance.GetLocation()));
			}
		}
	}

	/** Where the first PlayerStart is, its Group chain composed; false when the place has none. */
	bool FindSpawn(const FMantlePlace& Place, FVector& OutSpawn)
	{
		TMap<FString, const FMantleEntity*> ById;
		for (const FMantleEntity& Entity : Place.Entities)
		{
			ById.Add(Entity.Id, &Entity);
		}
		for (const FMantleEntity& Entity : Place.Entities)
		{
			if (Entity.Type != EMantleEntityType::PlayerStart)
			{
				continue;
			}
			FTransform World = Entity.Transform;
			FString Parent = Entity.Parent;
			for (int32 Hops = 0; !Parent.IsEmpty() && Hops < 64; ++Hops)
			{
				const FMantleEntity* const* Found = ById.Find(Parent);
				if (!Found)
				{
					break;
				}
				World = World * (*Found)->Transform;
				Parent = (*Found)->Parent;
			}
			OutSpawn = World.GetLocation();
			return true;
		}
		return false;
	}

	void Run(const TArray<FString>& Args, UWorld* World)
	{
		if (!World)
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_CELLCOUNT_FAILED no level is open"));
			return;
		}
		const double Started = FPlatformTime::Seconds();
		FMantlePlaceWorldPartition::FLoadResult Loaded = FMantlePlaceWorldPartition::LoadAll(World);
		FMantleExportOptions Options;
		Options.Name = World->GetName();
		const FMantleExportResult Exported = FMantlePlaceExporter::Export(World, Options);
		FMantlePlaceWorldPartition::ReleaseLoaded(World, Loaded);
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CELLCOUNT_EXPORT success=%d errors=%d seconds=%.0f"), Exported.bSuccess ? 1 : 0, Exported.Errors.Num(),
			FPlatformTime::Seconds() - Started);

		const FMantleCellGrid& Grid = Exported.CellGrid;

		// Per level, and every piece by its distance.
		TArray<int64> LevelPieces, LevelVisible, LevelCells;
		TArray<double> LevelMinDraw, LevelMaxDraw;
		LevelPieces.SetNumZeroed(Grid.Levels.Num());
		LevelVisible.SetNumZeroed(Grid.Levels.Num());
		LevelCells.SetNumZeroed(Grid.Levels.Num());
		LevelMinDraw.Init(TNumericLimits<double>::Max(), Grid.Levels.Num());
		LevelMaxDraw.SetNumZeroed(Grid.Levels.Num());
		int64 Band = 0, NoCutOff = 0, Own = 0, None = 0, OwnFarther = 0;
		auto CountDistance = [&](const FMantleEntity& Entity)
		{
			const FMantleStaticMeshProps* Mesh = MantleMeshPropsOf(Entity);
			const int64 Pieces = PiecesOf(Entity);
			if (!Mesh || Pieces == 0)
			{
				return;
			}
			const double Distance = Mesh->DrawDistance;
			if (Distance <= 0.0)
			{
				None += Pieces;
			}
			else if (FMantleDrawDistance::IsNoCutOff(Distance))
			{
				NoCutOff += Pieces;
			}
			else if (Distance == FMantleDrawDistance::Bands()[0].Draw)
			{
				Band += Pieces;
			}
			else
			{
				Own += Pieces;
				OwnFarther += Distance > FMantleDrawDistance::Bands()[0].Draw ? Pieces : 0;
			}
		};

		TArray<FMantleCellInfo> Infos;
		TArray<int64> InfoVisible;
		for (const FMantlePlannedCell& Cell : Exported.Cells)
		{
			const int32 Level = Cell.Key.Level;
			int64 Visible = 0;
			for (const FMantleEntity& Entity : Cell.Entities)
			{
				const FMantleStaticMeshProps* Mesh = MantleMeshPropsOf(Entity);
				const int64 Pieces = PiecesOf(Entity);
				if (Grid.Levels.IsValidIndex(Level))
				{
					LevelPieces[Level] += Pieces;
				}
				Visible += Mesh && Mesh->bVisible ? Pieces : 0;
				CountDistance(Entity);
			}
			if (Grid.Levels.IsValidIndex(Level))
			{
				LevelVisible[Level] += Visible;
				++LevelCells[Level];
				LevelMinDraw[Level] = FMath::Min(LevelMinDraw[Level], Cell.DrawDistance);
				LevelMaxDraw[Level] = FMath::Max(LevelMaxDraw[Level], Cell.DrawDistance);
			}
			FMantleCellInfo& Info = Infos.AddDefaulted_GetRef();
			Info.Level = Level;
			Info.X = Cell.Key.X;
			Info.Y = Cell.Key.Y;
			Info.Bounds = Cell.Bounds;
			Info.DrawDistance = Cell.DrawDistance;
			InfoVisible.Add(Visible);
		}
		int64 RootPieces = 0, RootVisible = 0;
		for (const FMantleEntity& Entity : Exported.Place.Entities)
		{
			const FMantleStaticMeshProps* Mesh = MantleMeshPropsOf(Entity);
			const int64 Pieces = PiecesOf(Entity);
			RootPieces += Pieces;
			RootVisible += Mesh && Mesh->bVisible ? Pieces : 0;
			CountDistance(Entity);
		}

		for (int32 Level = 0; Level < Grid.Levels.Num(); ++Level)
		{
			UE_LOG(LogMantle, Log, TEXT("MANTLE_CELLCOUNT_LEVEL level=%d cellSize=%lld maxDrawDistance=%lld cells=%lld pieces=%lld visible=%lld cellDrawDistance=%.0f..%.0f%s"),
				Level, Grid.Levels[Level].CellSize, Grid.Levels[Level].MaxDrawDistance, LevelCells[Level], LevelPieces[Level], LevelVisible[Level],
				LevelCells[Level] > 0 ? LevelMinDraw[Level] : 0.0, LevelMaxDraw[Level], Level == Grid.NoCutOffLevel ? TEXT(" noCutOffLevel") : TEXT(""));
		}
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CELLCOUNT_ROOT pieces=%lld visible=%lld stayed=%lld"), RootPieces, RootVisible, Exported.CellReport.NumStayed());
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CELLCOUNT_DIST band60m=%lld noCutOff=%lld own=%lld ownFartherThan60m=%lld none=%lld (root no-cut-off pieces count as own: held to the ring there)"),
			Band, NoCutOff, Own, OwnFarther, None);

		// The points: the spawn, then each argument.
		TArray<TPair<FString, FVector>> Points;
		FVector Spawn = FVector::ZeroVector;
		const bool bSpawn = FindSpawn(Exported.Place, Spawn);
		if (bSpawn)
		{
			Points.Emplace(TEXT("spawn"), Spawn);
		}
		for (const FString& Arg : Args)
		{
			// X:Y[:Z]; commas also work where nothing splits commands (-ExecCmds does).
			TArray<FString> Parts;
			Arg.Replace(TEXT(","), TEXT(":")).ParseIntoArray(Parts, TEXT(":"));
			if (Parts.Num() < 2)
			{
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_CELLCOUNT_SKIPPED '%s' is not X:Y or X:Y:Z (cm)"), *Arg);
				continue;
			}
			const double Z = Parts.Num() > 2 ? FCString::Atod(*Parts[2]) : Spawn.Z;
			Points.Emplace(Arg, FVector(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), Z));
		}

		FMantleFarSetSettings Far;
		Far.bEnabled = true;
		for (const TPair<FString, FVector>& Point : Points)
		{
			FMantleCellStreamPlan::FPath Path;
			Path.Add(Point.Value);
			for (const double Scale : { 1.0, 0.25 })
			{
				FMantleCellStreamSettings Settings;
				Settings.ReachScale = Scale;
				Far.ReachScale = Scale >= 1.0 ? 1.0 : 0.0;
				TArray<int64> Wanted;
				Wanted.SetNumZeroed(Grid.Levels.Num());
				int64 Drawn = 0, Twinned = 0, TwinCells = 0;
				for (int32 Index = 0; Index < Infos.Num(); ++Index)
				{
					const FMantleCellInfo& Info = Infos[Index];
					const FMantlePlannedCell& Cell = Exported.Cells[Index];
					if (FMantleCellStreamPlan::IsWanted(Info, Path, Settings))
					{
						if (Wanted.IsValidIndex(Info.Level))
						{
							Wanted[Info.Level] += InfoVisible[Index];
						}
						for (const FMantleEntity& Entity : Cell.Entities)
						{
							const FMantleStaticMeshProps* Mesh = MantleMeshPropsOf(Entity);
							if (!Mesh || !Mesh->bVisible)
							{
								continue;
							}
							ForEachPivot(Entity, [&](const FVector& Pivot)
							{
								Drawn += Mesh->DrawDistance <= 0.0 || FVector::Dist(Pivot, Point.Value) <= Mesh->DrawDistance ? 1 : 0;
							});
						}
					}
					else if (FMantleFarSetPlan::IsWanted(Info, Path, Far))
					{
						++TwinCells;
						for (const FMantleEntity& Entity : Cell.Entities)
						{
							const FMantleStaticMeshProps* Mesh = MantleMeshPropsOf(Entity);
							Twinned += Mesh && Mesh->bVisible && Mesh->DrawDistance >= Far.MinDrawDistanceCm ? PiecesOf(Entity) : 0;
						}
					}
				}
				int64 Total = RootVisible;
				FString PerLevel;
				for (int32 Level = 0; Level < Wanted.Num(); ++Level)
				{
					Total += Wanted[Level];
					PerLevel += FString::Printf(TEXT(" L%d=%lld"), Level, Wanted[Level]);
				}
				UE_LOG(LogMantle, Log, TEXT("MANTLE_CELLCOUNT_AT %s (%.0f, %.0f, %.0f) reachScale=%.2f loaded=%lld root=%lld%s insideOwnCutOff=%lld farTwinPieces=%lld farCells=%lld"),
					*Point.Key, Point.Value.X, Point.Value.Y, Point.Value.Z, Scale, Total, RootVisible, *PerLevel, Drawn + RootVisible, Twinned, TwinCells);
			}
		}
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CELLCOUNT_DONE cells=%d spawn=%d seconds=%.0f"), Exported.Cells.Num(), bSpawn ? 1 : 0, FPlatformTime::Seconds() - Started);
	}

	FAutoConsoleCommandWithWorldAndArgs GMantleDevCellCountCommand(
		TEXT("Mantle.Dev.CellCount"),
		TEXT("Mantle.Dev.CellCount [X:Y[:Z] ...] -- exports the open level as Publish would (nothing uploaded or saved) and logs its pieces per cell level and ")
		TEXT("the pieces loaded at the first PlayerStart and at each X:Y[:Z] (cm; Z defaults to the spawn's) at the full ladder and its bottom. MANTLE_CELLCOUNT_ lines."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			Run(Args, World);
		}));
}
