// Copyright Joshua Eger. All Rights Reserved.
// Live budgets: the part of FMantleCellStreamer that counts what its cells hold and loads less far when that is over the limits for the graphics setting (stage 8 of large places).
// The numbers and their basis: BP\gates\S8-A.budgets.md. Nothing here refuses anything: the ladder only shortens how far cells are wanted from.
#include "MantleCellStreamer.h"

#include "MantleRuntime.h"
#include "MantleStreamBudget.h"

#include "GameFramework/Actor.h"
#include "HAL/PlatformMemory.h"

namespace MantleCellStreamerBudgetPrivate
{
	/** A byte limit for the budget line, in MB, or "no limit". */
	FString MegabytesOrNone(const int64 Bytes)
	{
		return Bytes > 0 && Bytes < MAX_int64 ? FString::Printf(TEXT("%.0f"), Bytes / (1024.0 * 1024.0)) : FString(TEXT("no limit"));
	}
}

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
		Next.ReachScale = FMantleStreamBudget::CellReachScale(NextLadder, ViewScale, Settings.BudgetTuning);
		Next.FarSets.ReachScale = FMantleStreamBudget::FarReachScale(NextLadder, ViewScale);
		const FMantleCellIndex& Index = Loader->GetIndex();
		// The cells the longer reach would newly want (the ones already standing or on their way are in Use).
		TArray<int32> Candidates;
		FMantleCellStreamPlan::CellsNear(Index, LevelOverhangs, LevelRadii, LastPath, Candidates);
		int32 NewCells = 0;
		for (const int32 CellIndex : Candidates)
		{
			if (!Cells.Contains(CellIndex) && !Loader->IsCellFailed(CellIndex) && FMantleCellStreamPlan::IsWanted(Index.Cells[CellIndex], LastPath, Next))
			{
				AddCellToUse(Index.Cells[CellIndex], Projected);
				++NewCells;
			}
		}
		// #189: what a cell's textures cost is not in the index, so each new cell is taken to cost what a standing one does on average (textures shared between cells make that high,
		// so the ladder rises less readily: the side that does not cycle).
		const int32 Standing = BuiltCells.Num() + ReadyCells.Num() + AssetCells.Num() + LoadingCells.Num();
		if (Use.TextureBytes > 0 && Standing > 0 && NewCells > 0)
		{
			Projected.TextureBytes += Use.TextureBytes / Standing * NewCells;
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
	// Once the memory is read the counts no longer press, so the memory it would grow to decides instead.
	return FMath::Max(FMantleStreamBudget::CountPressure(Limits, Projected), FMantleStreamBudget::ProjectedMemoryPressure(Limits, Use, Projected));
}

void FMantleCellStreamer::UpdateBudget(const double DeltaSeconds)
{
	// A keep-every-cell streamer (a server's, a standalone game's solid things) is never limited: collision stands whatever the picture costs. A teleport's hold builds the spot the
	// player is put in, and the budget is looked at once it is over. The join's is different: it waits for everything near the spot (JoinHoldRadiusCm, 600 m), which on a big place is minutes of
	// building, so the budget keeps looking meanwhile; a lower ladder only shortens the reach the join waits for (IsAreaBuilt counts a cell wanted within ReachOf).
	if (!Settings.BudgetLimits || Settings.bKeepEveryCell || !bIndexReady || (bHolding && !bJoinPending))
	{
		return;
	}
	SecondsSinceBudgetLook += DeltaSeconds;
	SecondsSinceMemoryLower += DeltaSeconds;
	SecondsSinceTextureDetailMove += DeltaSeconds;
	const FMantleBudgetTuning& Tuning = Settings.BudgetTuning;
	if (SecondsSinceBudgetLook < Tuning.StepSeconds)
	{
		return;
	}
	SecondsSinceBudgetLook = 0.0;

	const FMantleBudgetLimits Limits = Settings.BudgetLimits();
	if (Limits.TextureShare != TextureShare)
	{
		// The Textures setting (FMantleBudgetLevels): one line when the share it gives the place's textures changes, the first look's included.
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CELLS_SETTINGS textures' share of the card %s -> %.2f (the Textures setting): textures may hold %s MB"),
			TextureShare < 0.0 ? TEXT("none") : *FString::Printf(TEXT("%.2f"), TextureShare), Limits.TextureShare, *MantleCellStreamerBudgetPrivate::MegabytesOrNone(Limits.TextureBytes));
		TextureShare = Limits.TextureShare;
	}
	Stats.BudgetTextureShare = Limits.TextureShare;
	FMantleBudgetUse Use = MeasureBudgetUse();
	Use.ProcessMemoryShare = Settings.BudgetMemoryShare ? Settings.BudgetMemoryShare() : 0.0;
	Use.ProcessMemoryBytes = Settings.BudgetMemoryBytes ? Settings.BudgetMemoryBytes() : 0;
	Use.TextureBytes = Settings.BudgetTextureBytes ? Settings.BudgetTextureBytes() : 0;
	if (Settings.BudgetVideoMemory)
	{
		Settings.BudgetVideoMemory(Use.VideoUsedBytes, Use.VideoBudgetBytes);
	}
	const double CountPressure = FMantleStreamBudget::CountPressure(Limits, Use);
	const double MemoryPressure = FMantleStreamBudget::MemoryPressure(Limits, Use);
	// #189: the card as the driver sees it comes back as slowly as the process's memory (a texture let go is freed at the next garbage collection), so it presses the same way.
	double VideoPressure = FMantleStreamBudget::VideoPressure(Limits, Use);
	// #189: the textures built whole (and the streamed ones' resident tails) take their part of the share; the engine's pool holds the streamed ones' top mips in the rest.
	// When the card is over its cushion the pool gives way first (its least needed mips go, the engine's choice), down to its floor; only then does the card's pressure
	// lower the ladder and the full-detail radius.
	if (Settings.ApplyTexturePool && Settings.BudgetTextureBytes)
	{
		const int64 Wanted = FMantleStreamBudget::TexturePoolBytes(Limits, Use.TextureBytes);
		if (Wanted > 0)
		{
			const int64 Cushion = static_cast<int64>(static_cast<double>(Use.VideoBudgetBytes) * Limits.VideoUseShare);
			const int64 OverBytes = Cushion > 0 && Use.VideoUsedBytes > 0 ? Use.VideoUsedBytes - Cushion : 0;
			TexturePoolBytes = FMantleStreamBudget::NextTexturePool(TexturePoolBytes, Wanted, OverBytes);
			Settings.ApplyTexturePool(TexturePoolBytes);
			if (OverBytes > 0 && TexturePoolBytes > FMantleStreamBudget::MinTexturePoolBytes)
			{
				VideoPressure = FMath::Min(VideoPressure, Tuning.LowerAbove);
			}
		}
	}
	Stats.BudgetTexturePoolBytes = TexturePoolBytes;
	const double SlowPressure = FMath::Max(MemoryPressure, VideoPressure);
	const double Pressure = FMath::Max(CountPressure, SlowPressure);
	const double VideoCushionBytes = static_cast<double>(Use.VideoBudgetBytes) * Limits.VideoUseShare;
	const double TexturesOfVideo = VideoCushionBytes > 0.0 ? static_cast<double>(Use.TextureBytes) / VideoCushionBytes : 0.0;
	const double TexturePressure = Limits.TextureBytes > 0 && Limits.TextureBytes < MAX_int64 ? static_cast<double>(Use.TextureBytes) / static_cast<double>(Limits.TextureBytes) : 0.0;
	// #189: while full texture detail reaches less than its whole way (textures were over at ladder 0), the ladder waits for it to come back before it rises.
	const bool bTextureDetailShort = TextureDetailCm < Tuning.TextureDetailMaxCm;

	const double Before = Ladder;
	if (CountPressure > Tuning.LowerAbove)
	{
		Ladder = FMantleStreamBudget::NextLadder(Ladder, CountPressure, Tuning);
	}
	else if (SlowPressure > Tuning.LowerAbove)
	{
		// Memory, the process's or the card's, comes back slowly: it lowers the ladder only now and then (see FMantleBudgetTuning::MemoryLowerEverySeconds).
		if (SecondsSinceMemoryLower >= Tuning.MemoryLowerEverySeconds)
		{
			SecondsSinceMemoryLower = 0.0;
			Ladder = FMantleStreamBudget::NextLadder(Ladder, SlowPressure, Tuning);
		}
	}
	else if (Pressure < Tuning.RaiseBelow && !bTextureDetailShort)
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
	Stats.BudgetTextureBytes = Use.TextureBytes;
	Stats.BudgetMemoryBytes = Use.ProcessMemoryBytes;
	Stats.BudgetVideoUsedBytes = Use.VideoUsedBytes;
	Stats.BudgetVideoBudgetBytes = Use.VideoBudgetBytes;
	Stats.BudgetVideoPressure = VideoPressure;

	// #189: below the ladder, how near full texture detail reaches (FMantleBudgetTuning::TextureDetailMaxCm). Down a step a second at most on the textures' own count, only as often
	// as memory lowers the ladder on the card's (its bytes come back at the next garbage collection), and up one every five seconds.
	const double NextDetail = FMantleStreamBudget::NextTextureDetailReach(TextureDetailCm, Ladder, TexturePressure, VideoPressure, TexturesOfVideo, Pressure, Tuning);
	const double DetailWait = NextDetail >= TextureDetailCm ? Tuning.TextureDetailRaiseEverySeconds
		: TexturePressure > Tuning.LowerAbove ? Tuning.TextureDetailLowerEverySeconds : FMath::Max(Tuning.TextureDetailLowerEverySeconds, Tuning.MemoryLowerEverySeconds);
	if (NextDetail != TextureDetailCm && SecondsSinceTextureDetailMove >= DetailWait)
	{
		const double DetailBefore = TextureDetailCm;
		TextureDetailCm = NextDetail;
		SecondsSinceTextureDetailMove = 0.0;
		if (CellAssets.IsValid())
		{
			CellAssets->SetTextureDetailReach(TextureDetailCm);
		}
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CELLS_BUDGET texture detail %.0f m -> %.0f m (ladder %.2f), textures %.0f of %s MB (pressure %.2f), card %.0f of %.0f MB the driver gives (pressure %.2f at %.0f%%), pressure %.2f"),
			DetailBefore / 100.0, TextureDetailCm / 100.0, Ladder, Use.TextureBytes / (1024.0 * 1024.0), *MantleCellStreamerBudgetPrivate::MegabytesOrNone(Limits.TextureBytes),
			TexturePressure, Use.VideoUsedBytes / (1024.0 * 1024.0), Use.VideoBudgetBytes / (1024.0 * 1024.0), VideoPressure, Limits.VideoUseShare * 100.0, Pressure);
	}
	Stats.BudgetTextureDetailCm = TextureDetailCm;
	Stats.BudgetLowestTextureDetailCm = FMath::Min(Stats.BudgetLowestTextureDetailCm, TextureDetailCm);

	if (Ladder == Before)
	{
		return;
	}
	ApplyReachScales();
	// The cells are planned again now: what is beyond the new reach goes the usual way (a far set built, a cell released after its margin).
	bRefreshNeeded = true;
	if (FMath::FloorToInt(Ladder * 10.0) != FMath::FloorToInt(Before * 10.0))
	{
		UE_LOG(LogMantle, Log,
			TEXT("MANTLE_CELLS_BUDGET ladder %.2f -> %.2f (far reach x%.2f, cell reach x%.2f), pressure %.2f (memory %.2f, card %.2f: %.0f of %.0f MB the driver gives): instances %lld of %lld, solid %lld of %lld, far instances %lld of %lld, Nanite root pages %lld of %lld, textures %.0f of %s MB, process memory %.3f of %.2f of the machine, %.0f of %s MB (working set %.0f MB, private %.0f MB); now %s"),
			Before, Ladder, Settings.FarSets.ReachScale, Settings.ReachScale, Pressure, MemoryPressure, VideoPressure, Use.VideoUsedBytes / (1024.0 * 1024.0),
			Use.VideoBudgetBytes / (1024.0 * 1024.0), Use.Instances, Limits.Instances, Use.SolidThings, Limits.SolidThings, Use.FarInstances, Limits.FarInstances,
			Use.NaniteRootPages, Limits.NaniteRootPages, Use.TextureBytes / (1024.0 * 1024.0), *MantleCellStreamerBudgetPrivate::MegabytesOrNone(Limits.TextureBytes), Use.ProcessMemoryShare, Limits.ProcessMemoryShare,
			Use.ProcessMemoryBytes / (1024.0 * 1024.0), *MantleCellStreamerBudgetPrivate::MegabytesOrNone(Limits.ProcessMemoryBytes),
			FPlatformMemory::GetStats().UsedPhysical / (1024.0 * 1024.0), FPlatformMemory::GetStats().UsedVirtual / (1024.0 * 1024.0), *CountActors().ToLogText());
	}
}

void FMantleCellStreamer::ApplyReachScales()
{
	Settings.ReachScale = FMantleStreamBudget::CellReachScale(Ladder, ViewScale, Settings.BudgetTuning);
	Settings.FarSets.ReachScale = FMantleStreamBudget::FarReachScale(Ladder, ViewScale);
}

void FMantleCellStreamer::UpdateViewScale()
{
	// A keep-every-cell streamer (a server's, a standalone game's solid things) keeps every cell whatever the setting.
	if (!Settings.ViewDistanceScale || Settings.bKeepEveryCell)
	{
		return;
	}
	const double EngineScale = Settings.ViewDistanceScale();
	const double Next = FMantleStreamBudget::ViewScale(EngineScale);
	if (Next == ViewScale)
	{
		return;
	}
	const double Before = ViewScale;
	ViewScale = Next;
	Stats.ViewScale = ViewScale;
	ApplyReachScales();
	// What is beyond the new ring goes the usual way (a far set built, a cell released after its margin); a longer one is loaded.
	bRefreshNeeded = true;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CELLS_SETTINGS ring x%.2f -> x%.2f (the View Distance setting, the engine's view distance scale %.2f): cell reach x%.2f, far reach x%.2f (ladder %.2f)"),
		Before, ViewScale, EngineScale, Settings.ReachScale, Settings.FarSets.ReachScale, Ladder);
}
