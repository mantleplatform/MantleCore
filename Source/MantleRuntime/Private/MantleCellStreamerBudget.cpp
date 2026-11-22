// Copyright Joshua Eger. All Rights Reserved.
// Live budgets: the part of FMantleCellStreamer that counts what its cells hold and loads less far when that is over the limits for the graphics setting (stage 8 of large places).
// The numbers and their basis: BP\gates\S8-A.budgets.md. Nothing here refuses anything: the ladder only shortens how far cells are wanted from.
#include "MantleCellStreamer.h"

#include "MantleRuntime.h"
#include "MantleStreamBudget.h"

#include "GameFramework/Actor.h"

void FMantleCellStreamer::AddCellToUse(const FMantleCellInfo& Info, FMantleBudgetUse& Use) const
{
	if (Settings.Entities == EMantleCellEntities::PicturesOnly)
	{
		// A standalone game's picture streamer builds no solid thing (its solid streamer keeps them): the index's instances less its solid ones. That also takes off the solid
		// models, which are not instances, so it counts a little low: the lenient side.
		Use.Instances += FMath::Max<int64>(Info.Instances - Info.Solid, 0);
	}
	else
	{
		Use.Instances += Info.Instances;
		Use.SolidThings += Info.Solid;
	}
}

FMantleBudgetUse FMantleCellStreamer::MeasureBudgetUse() const
{
	FMantleBudgetUse Use;
	if (!bIndexReady || !Loader.IsValid())
	{
		return Use;
	}
	const FMantleCellIndex& Index = Loader->GetIndex();
	// What stands, is being built, or is on its way to standing (being read, or waiting for assets or the builder). A cell being taken down is already going.
	for (const int32 CellIndex : BuiltCells)
	{
		AddCellToUse(Index.Cells[CellIndex], Use);
	}
	for (const int32 CellIndex : ReadyCells)
	{
		AddCellToUse(Index.Cells[CellIndex], Use);
	}
	for (const int32 CellIndex : AssetCells)
	{
		AddCellToUse(Index.Cells[CellIndex], Use);
	}
	for (const int32 CellIndex : LoadingCells)
	{
		AddCellToUse(Index.Cells[CellIndex], Use);
	}
	for (const TPair<int32, FFarSet>& Pair : FarSets)
	{
		for (const FFarEntity& Far : Pair.Value.Entities)
		{
			if (!Far.Actor.IsValid())
			{
				continue;
			}
			const FMantleInstancedMeshProps* Instanced = Far.Twin.Props.TryGet<FMantleInstancedMeshProps>();
			Use.FarInstances += Instanced ? Instanced->Instances.Num() : 1;
		}
	}
	if (CellAssets.IsValid())
	{
		Use.NaniteRootPages = CellAssets->GetNaniteRootPagesAttached();
	}
	return Use;
}

double FMantleCellStreamer::ProjectedCountPressure(const FMantleBudgetLimits& Limits, const FMantleBudgetUse& Use, const double NextLadder) const
{
	FMantleBudgetUse Projected = Use;
	if (bIndexReady && Loader.IsValid() && !LastPath.IsEmpty())
	{
		FMantleCellStreamSettings Next = Settings;
		Next.ReachScale = FMantleStreamBudget::NearScale(NextLadder, Settings.BudgetTuning);
		Next.FarSets.ReachScale = FMantleStreamBudget::FarScale(NextLadder);
		const FMantleCellIndex& Index = Loader->GetIndex();
		// The cells the longer reach would newly want (the ones already standing or on their way are in Use).
		TArray<int32> Candidates;
		FMantleCellStreamPlan::CellsNear(Index, LevelOverhangs, LevelRadii, LastPath, Candidates);
		for (const int32 CellIndex : Candidates)
		{
			if (!Cells.Contains(CellIndex) && !Loader->IsCellFailed(CellIndex) && FMantleCellStreamPlan::IsWanted(Index.Cells[CellIndex], LastPath, Next))
			{
				AddCellToUse(Index.Cells[CellIndex], Projected);
			}
		}
		// The twins it would newly want and build.
		if (FarEnabled())
		{
			for (const TPair<int32, FFarSet>& Pair : FarSets)
			{
				const FFarSet& Set = Pair.Value;
				if (Set.State != EFarState::Idle || Set.bDestroying)
				{
					continue;
				}
				const FMantleCellInfo& Info = Index.Cells[Pair.Key];
				if (FMantleFarSetPlan::IsWanted(Info, LastPath, Next.FarSets) && FMantleFarSetPlan::NeedsBuilding(Info, LastPath, Next.FarSets))
				{
					for (const FFarEntity& Far : Set.Entities)
					{
						const FMantleInstancedMeshProps* Instanced = Far.Twin.Props.TryGet<FMantleInstancedMeshProps>();
						Projected.FarInstances += Instanced ? Instanced->Instances.Num() : 1;
					}
				}
			}
		}
	}
	return FMantleStreamBudget::CountPressure(Limits, Projected);
}

void FMantleCellStreamer::UpdateBudget(const double DeltaSeconds)
{
	// A keep-every-cell streamer (a server's, a standalone game's solid things) is never limited: collision stands whatever the picture costs. A teleport's hold builds the spot the
	// player is put in, and the budget is looked at once it is over. The join's is different: it waits for the whole first view (JoinHoldRadiusCm), which on a big place is minutes of
	// building, so the budget keeps looking meanwhile; a lower ladder only shortens the reach the join waits for (IsAreaBuilt counts a cell wanted within ReachOf).
	if (!Settings.BudgetLimits || Settings.bKeepEveryCell || !bIndexReady || (bHolding && !bJoinPending))
	{
		return;
	}
	SecondsSinceBudgetLook += DeltaSeconds;
	SecondsSinceMemoryLower += DeltaSeconds;
	const FMantleBudgetTuning& Tuning = Settings.BudgetTuning;
	if (SecondsSinceBudgetLook < Tuning.StepSeconds)
	{
		return;
	}
	SecondsSinceBudgetLook = 0.0;

	const FMantleBudgetLimits Limits = Settings.BudgetLimits();
	FMantleBudgetUse Use = MeasureBudgetUse();
	Use.ProcessMemoryShare = Settings.BudgetMemoryShare ? Settings.BudgetMemoryShare() : 0.0;
	const double CountPressure = FMantleStreamBudget::CountPressure(Limits, Use);
	const double MemoryPressure = FMantleStreamBudget::MemoryPressure(Limits, Use);
	const double Pressure = FMath::Max(CountPressure, MemoryPressure);

	const double Before = Ladder;
	if (CountPressure > Tuning.LowerAbove)
	{
		Ladder = FMantleStreamBudget::NextLadder(Ladder, CountPressure, Tuning);
	}
	else if (MemoryPressure > Tuning.LowerAbove)
	{
		// Memory comes back slowly: it lowers the ladder only now and then (see FMantleBudgetTuning::MemoryLowerEverySeconds).
		if (SecondsSinceMemoryLower >= Tuning.MemoryLowerEverySeconds)
		{
			SecondsSinceMemoryLower = 0.0;
			Ladder = FMantleStreamBudget::NextLadder(Ladder, MemoryPressure, Tuning);
		}
	}
	else if (Pressure < Tuning.RaiseBelow)
	{
		// Rising loads more, so it first looks at what the longer reach would bring in: it rises only if that still fits. Else a cell at the edge that does not fit would be built, found
		// over the limit, let go and wanted again, round and round.
		const double Next = FMantleStreamBudget::NextLadder(Ladder, Pressure, Tuning);
		if (Next > Ladder && ProjectedCountPressure(Limits, Use, Next) <= Tuning.LowerAbove)
		{
			Ladder = Next;
		}
	}

	++Stats.BudgetLooks;
	Stats.BudgetPressure = Pressure;
	Stats.BudgetWorstPressure = FMath::Max(Stats.BudgetWorstPressure, Pressure);
	Stats.BudgetLadder = Ladder;
	Stats.BudgetLowestLadder = FMath::Min(Stats.BudgetLowestLadder, Ladder);
	Stats.BudgetInstances = Use.Instances;
	Stats.BudgetSolidThings = Use.SolidThings;
	Stats.BudgetFarInstances = Use.FarInstances;
	Stats.BudgetNaniteRootPages = Use.NaniteRootPages;
	Stats.BudgetMemoryShare = Use.ProcessMemoryShare;

	if (Ladder == Before)
	{
		return;
	}
	Settings.ReachScale = FMantleStreamBudget::NearScale(Ladder, Tuning);
	Settings.FarSets.ReachScale = FMantleStreamBudget::FarScale(Ladder);
	// The cells are planned again now: what is beyond the new reach goes the usual way (a far set built, a cell released after its margin).
	bRefreshNeeded = true;
	if (FMath::FloorToInt(Ladder * 10.0) != FMath::FloorToInt(Before * 10.0))
	{
		UE_LOG(LogMantle, Log,
			TEXT("MANTLE_CELLS_BUDGET ladder %.2f -> %.2f (far reach x%.2f, cell reach x%.2f), pressure %.2f (memory %.2f): instances %lld of %lld, solid %lld of %lld, far instances %lld of %lld, Nanite root pages %lld of %lld, process memory %.3f of %.2f of the machine"),
			Before, Ladder, Settings.FarSets.ReachScale, Settings.ReachScale, Pressure, MemoryPressure, Use.Instances, Limits.Instances, Use.SolidThings, Limits.SolidThings, Use.FarInstances, Limits.FarInstances,
			Use.NaniteRootPages, Limits.NaniteRootPages, Use.ProcessMemoryShare, Limits.ProcessMemoryShare);
	}
}
