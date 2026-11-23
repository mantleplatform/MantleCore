// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCellStreamer.h"
#include "HAL/PlatformMemory.h"

#include "MantleMergedInstancesComponent.h"
#include "MantleRuntime.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"

namespace MantleCellStreamerPrivate
{
	/** The view is re-planned when it has moved this far (cm) since the last plan, or this long (seconds) has passed. */
	constexpr double RefreshDistanceCm = 100.0;
	/** Cells within this far of the player count as under it (the solid ground its own character stands on). */
	constexpr double OwnGroundRadiusCm = 300.0;
	constexpr double RefreshSeconds = 0.25;

	/** A piece that takes this long is named in the log. */
	constexpr double SlowUnitSeconds = 0.010;

	/** At most this many build problems are logged by name in a run (the count is in the stats). */
	constexpr int32 MaxLoggedBuildErrors = 20;

	bool IsSolidMesh(const FMantleEntity& Entity)
	{
		const FMantleStaticMeshProps* Mesh = MantleMeshPropsOf(Entity);
		return Mesh && Mesh->Collision != EMantleCollision::None;
	}

	/**
	 * The entity a piece belongs to and which piece of it this is: a piece cut from a long list is "<id>~<n>" (see PrepareCell); anything else is its own entity, piece 0.
	 */
	FString BaseEntityId(const FString& PieceId, int32& OutPieceIndex)
	{
		OutPieceIndex = 0;
		int32 Tilde = INDEX_NONE;
		if (PieceId.FindLastChar(TEXT('~'), Tilde) && Tilde > 0 && Tilde + 1 < PieceId.Len())
		{
			for (int32 At = Tilde + 1; At < PieceId.Len(); ++At)
			{
				if (!FChar::IsDigit(PieceId[At]))
				{
					return PieceId;
				}
			}
			OutPieceIndex = FCString::Atoi(*PieceId.Mid(Tilde + 1));
			return PieceId.Left(Tilde);
		}
		return PieceId;
	}
}

FMantleCellStreamer::FMantleCellStreamer(const FMantleCellStreamSettings& InSettings)
	: Settings(InSettings)
{
	SliceOptions.bTransient = true;
	SliceOptions.bRuntimeEnvironment = false;
	SliceOptions.bCellSlice = true;
}

FMantleCellStreamer::~FMantleCellStreamer()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	}
}

TSharedRef<FMantleCellStreamer> FMantleCellStreamer::Start(
	UWorld* World,
	const FMantlePlace& Root,
	const FMantleCellIndex* KnownIndex,
	const FMantleCellLoaderSettings& LoaderSettings,
	FViewProvider ViewProvider,
	const FVector& InitialView,
	const FMantleCellStreamSettings& Settings)
{
	check(IsInGameThread());
	TSharedRef<FMantleCellStreamer> Streamer = MakeShareable(new FMantleCellStreamer(Settings));
	Streamer->World = World;
	Streamer->ViewProvider = MoveTemp(ViewProvider);
	Streamer->View.Location = InitialView;
	Streamer->LastSample = InitialView;
	Streamer->LastRefreshLocation = InitialView;
	Streamer->SlicePlace.PlaceId = Root.PlaceId;

	const TWeakPtr<FMantleCellStreamer> Weak = Streamer;
	Streamer->Loader = FMantleCellLoader::Open(Root, LoaderSettings,
		FMantleCellIndexReady::CreateLambda([Weak]()
		{
			if (const TSharedPtr<FMantleCellStreamer> Pinned = Weak.Pin())
			{
				Pinned->OnIndexReady();
			}
		}),
		KnownIndex);

	Streamer->bPumpLoader = !LoaderSettings.bAutoPump;
	Streamer->SetHolding(true, TEXT("join"));
	if (Settings.bAutoTick)
	{
		Streamer->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak](const float DeltaSeconds)
		{
			if (const TSharedPtr<FMantleCellStreamer> Pinned = Weak.Pin())
			{
				Pinned->Tick(DeltaSeconds);
				return !Pinned->bStopped;
			}
			return false;
		}));
	}
	return Streamer;
}

void FMantleCellStreamer::OnIndexReady()
{
	const FMantleCellIndex& Index = Loader->GetIndex();
	FMantleCellStreamPlan::LevelOverhangs(Index, LevelOverhangs);
	RefreshLevelRadii();
	FarLevelRadii.Reset();
	for (const FMantleCellGridLevel& Level : Index.Levels)
	{
		FarLevelRadii.Add(FMath::Min(Level.MaxDrawDistance * Settings.FarSets.ReachFactor, Settings.FarSets.MaxReachCm));
	}
	bIndexReady = true;
	bRefreshNeeded = true;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CELLS_STREAM started: %d cells in %d level(s), view at (%.0f, %.0f, %.0f)"),
		Index.Cells.Num(), Index.Levels.Num(), View.Location.X, View.Location.Y, View.Location.Z);
}

// ---- Tick ---------------------------------------------------------------------------------------------

void FMantleCellStreamer::Tick(const double DeltaSeconds)
{
	if (bStopped)
	{
		return;
	}
	const UWorld* const LiveWorld = World.Get();
	if (!LiveWorld || LiveWorld->bIsTearingDown)
	{
		// The world takes its actors with it.
		bStopped = true;
		return;
	}
	// Step 1 of the big-map plan: a clock read between phases (a few tens of nanoseconds each), so a slow tick can say where its time went.
	using FPhase = FMantleCellTickReport;
	TickReport.Reset();
	TickReport.DeltaSeconds = DeltaSeconds;
	const double ReportStart = FPlatformTime::Seconds();
	double PhaseMark = ReportStart;
	const auto Lap = [this, &PhaseMark](const FMantleCellTickReport::EPhase Phase)
	{
		const double Now = FPlatformTime::Seconds();
		TickReport.Seconds[Phase] += Now - PhaseMark;
		PhaseMark = Now;
	};
	if (bPumpLoader)
	{
		Loader->Pump();
	}
	Lap(FPhase::Pump);
	if (Loader->HasFailed())
	{
		// The index could not be read: nothing more can be streamed. Say so once and let the hold go (a place whose scenery cannot be had is no reason to freeze a player).
		UE_LOG(LogMantle, Error, TEXT("MANTLE_CELLS_STREAM index failed:\n%s"), *Loader->GetStreamResult().ErrorSummary());
		SetHolding(false, TEXT("index failed"));
		bStopped = true;
		return;
	}

	const double TickStart = FPlatformTime::Seconds();
	PhaseMark = TickStart;
	SampleView(DeltaSeconds);
	// Before the plan, so the first plan already has the View Distance setting's ring and a change in the settings screen re-plans this tick.
	UpdateViewScale();
	Lap(FPhase::View);
	SampleProgress(DeltaSeconds);
	Lap(FPhase::Progress);
	if (bIndexReady)
	{
		SecondsSinceRefresh += DeltaSeconds;
		const bool bMoved = FVector::DistSquared(View.Location, LastRefreshLocation) > MantleCellStreamerPrivate::RefreshDistanceCm * MantleCellStreamerPrivate::RefreshDistanceCm;
		if (bRefreshNeeded || bMoved || SecondsSinceRefresh >= MantleCellStreamerPrivate::RefreshSeconds)
		{
			Refresh();
		}
		Lap(FPhase::Refresh);
		TakeFinishedCells();
		Lap(FPhase::TakeFinished);
		TakeAssetCells();
		Lap(FPhase::TakeAssets);
		TakeFinishedFarCells();
		Lap(FPhase::TakeFarFinished);
		TakeFarAssetCells();
		Lap(FPhase::TakeFarAssets);
		double Budget = FMantleCellStreamPlan::BudgetSeconds(Settings, FastestViewSpeed(), bHolding);
		Stats.LastBudgetSeconds = Budget;
		if (CellAssets.IsValid())
		{
			// The assets' builds and the pieces share the tick's budget: while a cell waits for its assets they get their share of it.
			const double AssetSlice = AssetCells.IsEmpty() ? Settings.AssetIdleSliceSeconds : Budget * Settings.AssetBudgetShare;
			CellAssets->SetBuildSlice(AssetSlice);
			// #189: near materials get their textures' full files; the rest stay on their low companions.
			CellAssets->SetView(View.Location);
			Budget = FMath::Max(Budget - (AssetCells.IsEmpty() ? 0.0 : AssetSlice), 0.0);
		}
		TickReport.BudgetSeconds = Budget;
		Lap(FPhase::AssetView);
		{
			// The work step's parts come from the clocks its own steps keep (RemoveSome, BuildOne, BuildOneFar, RemoveFarSome); its counts from the running totals.
			const double RemoveBefore = Stats.RemoveSeconds;
			const double BuildBefore = Stats.BuildSeconds;
			const double FarBuildBefore = Stats.FarBuildSeconds;
			const double FarRemoveBefore = Stats.FarRemoveSeconds;
			const int64 RemovedBefore = Stats.ActorsRemoved;
			const int64 ReleasedBefore = Stats.CellsReleasedEver;
			const int64 PiecesBefore = Stats.UnitsBuilt;
			const int64 InstancesBefore = Stats.InstancesBuilt;
			const int64 CellsBuiltBefore = Stats.CellsBuiltEver;
			const int64 TwinsBefore = Stats.FarEntitiesBuilt;
			const int64 TwinInstancesBefore = Stats.FarInstancesBuilt;
			const int64 FarSetsBuiltBefore = Stats.FarSetsBuiltEver;
			const int64 FarSetsReleasedBefore = Stats.FarSetsReleased;
			RunWork(Budget);
			const double WorkEnd = FPlatformTime::Seconds();
			TickReport.Seconds[FPhase::Remove] = Stats.RemoveSeconds - RemoveBefore;
			TickReport.Seconds[FPhase::Build] = Stats.BuildSeconds - BuildBefore;
			TickReport.Seconds[FPhase::FarBuild] = Stats.FarBuildSeconds - FarBuildBefore;
			TickReport.Seconds[FPhase::FarRemove] = Stats.FarRemoveSeconds - FarRemoveBefore;
			TickReport.Seconds[FPhase::WorkRest] = FMath::Max((WorkEnd - PhaseMark) - TickReport.Seconds[FPhase::Remove] - TickReport.Seconds[FPhase::Build]
				- TickReport.Seconds[FPhase::FarBuild] - TickReport.Seconds[FPhase::FarRemove], 0.0);
			PhaseMark = WorkEnd;
			TickReport.ActorsRemoved = Stats.ActorsRemoved - RemovedBefore;
			TickReport.CellsReleased = Stats.CellsReleasedEver - ReleasedBefore;
			TickReport.PiecesBuilt = Stats.UnitsBuilt - PiecesBefore;
			TickReport.InstancesBuilt = Stats.InstancesBuilt - InstancesBefore;
			TickReport.CellsBuilt = Stats.CellsBuiltEver - CellsBuiltBefore;
			TickReport.TwinsBuilt = Stats.FarEntitiesBuilt - TwinsBefore;
			TickReport.TwinInstancesBuilt = Stats.FarInstancesBuilt - TwinInstancesBefore;
			TickReport.FarSetsBuilt = Stats.FarSetsBuiltEver - FarSetsBuiltBefore;
			TickReport.FarSetsReleased = Stats.FarSetsReleased - FarSetsReleasedBefore;
		}
		TickReport.PendingCells = PendingCells.Num();
		UpdateStats();
		Lap(FPhase::Stats);
		const int32 LooksBefore = Stats.BudgetLooks;
		UpdateBudget(DeltaSeconds);
		TickReport.BudgetLooks = Stats.BudgetLooks - LooksBefore;
		Lap(FPhase::Budget);
		// The twins, looked at once a second: each is drawn exactly when it should be.
		FarAuditSeconds += DeltaSeconds;
		if (FarAuditSeconds >= 1.0 && !FarSets.IsEmpty())
		{
			FarAuditSeconds = 0.0;
			TickReport.FarSetsAudited = AuditFarSets(TEXT("tick")).Sets;
		}
		Lap(FPhase::Audit);
	}
	// The player's own ground (the collision rule, made general): a character that stands where a solid cell is not built here (a late download, a view that
	// outran the loader without being a teleport) is held until it stands, as a join or a teleport holds it. A cell that can never be read does not hold it.
	if (bIndexReady && !bHolding && !Settings.bKeepEveryCell && Settings.Entities != EMantleCellEntities::PicturesOnly)
	{
		FMantleCellAreaQuery OwnGround;
		OwnGround.Point = View.Location;
		OwnGround.RadiusCm = MantleCellStreamerPrivate::OwnGroundRadiusCm;
		OwnGround.bSolidOnly = true;
		const FMantleCellAreaState Under = QueryArea(OwnGround);
		TickReport.GroundLooks = 1;
		const bool bUnderStands = Under.Built + Under.Failed >= Under.Cells;
		bOwnGroundGaveUp = bOwnGroundGaveUp && !bUnderStands;
		if (!bUnderStands && !bOwnGroundGaveUp)
		{
			UE_LOG(LogMantle, Log, TEXT("MANTLE_CELLS_OWN_GROUND the solid cells under the player are not built (%d of %d stand, %d on their way) at (%.0f, %.0f, %.0f)"),
				Under.Built, Under.Cells, Under.Pending, View.Location.X, View.Location.Y, View.Location.Z);
			BeginHold(TEXT("ground"));
		}
	}
	Lap(FPhase::Ground);
	// Outside the index check: a hold must end even when the index never arrives (a store that does not answer).
	CheckHold();
	Lap(FPhase::Hold);
	Stats.LastWorkSeconds = PhaseMark - TickStart;
	Stats.WorstWorkSeconds = FMath::Max(Stats.WorstWorkSeconds, Stats.LastWorkSeconds);
	TickReport.TotalSeconds = PhaseMark - ReportStart;
	if (TickReport.TotalSeconds >= Settings.SlowTickLogSeconds)
	{
		// Only now is anything counted or written: a fast tick ends at the comparison above.
		TickReport.bHolding = bHolding;
		TickReport.Ladder = Ladder;
		TickReport.CellsStanding = BuiltCells.Num();
		TickReport.CellsReady = ReadyCells.Num();
		TickReport.CellsAssetWait = AssetCells.Num();
		TickReport.CellsLoading = LoadingCells.Num();
		TickReport.CellsRemoving = RemovingCells.Num();
		TickReport.FarSets = FarSets.Num();
		++Stats.SlowTicks;
		UE_LOG(LogMantle, Log, TEXT("%s"), *TickReport.ToLogLine());
	}
}

void FMantleCellStreamer::SampleView(const double DeltaSeconds)
{
	FVector Location = View.Location;
	const bool bGot = ViewProvider && ViewProvider(Location);
	if (!bGot)
	{
		Location = View.Location;
	}
	if (bGot && DeltaSeconds > UE_DOUBLE_SMALL_NUMBER)
	{
		const FVector Delta = Location - LastSample;
		if (FMantleCellStreamPlan::IsTeleport(Delta, DeltaSeconds, Settings))
		{
			// Not travel: the player was moved (a long frame is allowed the distance a fast player covers in it). Hold until what is around the new spot stands.
			View.Velocity = FVector::ZeroVector;
			bRefreshNeeded = true;
			// With every cell kept there is nothing to wait for: everything stands or is on its way.
			if (!Settings.bKeepEveryCell)
			{
				UE_LOG(LogMantle, Log, TEXT("MANTLE_CELLS_TELEPORT from (%.0f, %.0f, %.0f) to (%.0f, %.0f, %.0f)"),
					LastSample.X, LastSample.Y, LastSample.Z, Location.X, Location.Y, Location.Z);
				BeginHold(TEXT("teleport"));
			}
		}
		else
		{
			View.Velocity = FMantleCellStreamPlan::SmoothVelocity(View.Velocity, Delta / DeltaSeconds, DeltaSeconds, Settings);
		}
	}
	View.Location = Location;
	if (bGot)
	{
		LastSample = Location;
	}
	SampleExtraViews(DeltaSeconds);
}

void FMantleCellStreamer::SampleExtraViews(const double DeltaSeconds)
{
	TArray<FExtraView> Given;
	if (ExtraViewsProvider)
	{
		ExtraViewsProvider(Given);
	}
	bool bChanged = false;
	TMap<int32, FTrackedView> Next;
	Next.Reserve(Given.Num());
	for (const FExtraView& Extra : Given)
	{
		// A view at the player adds nothing the player's own does not.
		if (Next.Contains(Extra.Key) || FVector::DistSquared(Extra.Location, View.Location) <= Settings.SameViewDistanceCm * Settings.SameViewDistanceCm)
		{
			continue;
		}
		FTrackedView* Previous = ExtraViews.Find(Extra.Key);
		FTrackedView& Tracked = Next.Add(Extra.Key);
		if (!Previous)
		{
			Tracked.View.Location = Extra.Location;
			Tracked.LastSample = Extra.Location;
			Tracked.LastRefreshLocation = Extra.Location;
			bChanged = true;
			continue;
		}
		Tracked = *Previous;
		const FVector Delta = Extra.Location - Tracked.LastSample;
		if (DeltaSeconds > UE_DOUBLE_SMALL_NUMBER)
		{
			// A jump of an extra view holds nothing (the character did not move): the cells around its new place are wanted, and that is all.
			Tracked.View.Velocity = FMantleCellStreamPlan::IsTeleport(Delta, DeltaSeconds, Settings)
				? FVector::ZeroVector
				: FMantleCellStreamPlan::SmoothVelocity(Tracked.View.Velocity, Delta / DeltaSeconds, DeltaSeconds, Settings);
		}
		Tracked.View.Location = Extra.Location;
		Tracked.LastSample = Extra.Location;
		if (FVector::DistSquared(Extra.Location, Tracked.LastRefreshLocation) > MantleCellStreamerPrivate::RefreshDistanceCm * MantleCellStreamerPrivate::RefreshDistanceCm)
		{
			bChanged = true;
		}
	}
	bChanged = bChanged || Next.Num() != ExtraViews.Num();
	ExtraViews = MoveTemp(Next);
	bRefreshNeeded = bRefreshNeeded || bChanged;
}

FMantleCellStreamPlan::FRank FMantleCellStreamer::RankFor(const int32 CellIndex) const
{
	const FMantleCellInfo& Info = InfoOf(CellIndex);
	FMantleCellStreamPlan::FRank Best = FMantleCellStreamPlan::RankOf(Info, CellIndex, View, Settings);
	for (const TPair<int32, FTrackedView>& Pair : ExtraViews)
	{
		const FMantleCellStreamPlan::FRank Other = FMantleCellStreamPlan::RankOf(Info, CellIndex, Pair.Value.View, Settings);
		if (FMantleCellStreamPlan::BuildsBefore(Other, Best))
		{
			Best = Other;
		}
	}
	return Best;
}

double FMantleCellStreamer::FastestViewSpeed() const
{
	double Fastest = View.Velocity.Size();
	for (const TPair<int32, FTrackedView>& Pair : ExtraViews)
	{
		Fastest = FMath::Max(Fastest, Pair.Value.View.Velocity.Size());
	}
	return Fastest;
}

// ---- Planning -----------------------------------------------------------------------------------------

void FMantleCellStreamer::Refresh()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MantleCellStreamer_Refresh);
	if (Settings.bKeepEveryCell && bKeepAllSettled)
	{
		// Every cell with something of ours stands (or can never be read), and which cells those are does not depend on the view: there is nothing to plan.
		LastRefreshLocation = View.Location;
		SecondsSinceRefresh = 0.0;
		bRefreshNeeded = false;
		for (TPair<int32, FTrackedView>& Pair : ExtraViews)
		{
			Pair.Value.LastRefreshLocation = Pair.Value.View.Location;
		}
		return;
	}
	++Stats.PlansRun;
	++TickReport.Plans;
	const FMantleCellIndex& Index = Loader->GetIndex();
	// What is wanted is what the player's path wants and what each extra view's path does.
	FMantleCellStreamPlan::FPath Path;
	FMantleCellStreamPlan::PathOf(View, Settings, Path);
	for (TPair<int32, FTrackedView>& Pair : ExtraViews)
	{
		FMantleCellStreamPlan::FPath ExtraPath;
		FMantleCellStreamPlan::PathOf(Pair.Value.View, Settings, ExtraPath);
		Path.Append(ExtraPath);
		Pair.Value.LastRefreshLocation = Pair.Value.View.Location;
	}
	LastRefreshLocation = View.Location;
	SecondsSinceRefresh = 0.0;
	bRefreshNeeded = false;

	// Cells waiting on a read or for the builder that the path has left are dropped; standing ones are taken down once well past their distance.
	{
		const TArray<int32> Waiting = [this]() { TArray<int32> All = LoadingCells; All.Append(AssetCells); All.Append(ReadyCells); return All; }();
		for (const int32 CellIndex : Waiting)
		{
			FCell& Cell = Cells.FindChecked(CellIndex);
			if (!Settings.bKeepEveryCell && FMantleCellStreamPlan::IsReleased(InfoOf(CellIndex), Path, Settings))
			{
				LoadingCells.RemoveSingle(CellIndex);
				AssetCells.RemoveSingle(CellIndex);
				ReadyCells.RemoveSingle(CellIndex);
				Cell.Content = FMantleCellContent();
				Cell.Cursor = 0;
				if (Cell.Actors.IsEmpty())
				{
					ReleaseAssets(CellIndex);
					Cells.Remove(CellIndex);
				}
				else
				{
					// Half built: what stands comes down like any cell.
					BuiltCells.AddUnique(CellIndex);
					QueueRemoval(CellIndex);
				}
			}
		}
		const TArray<int32> Standing = Settings.bKeepEveryCell ? TArray<int32>() : BuiltCells;
		for (const int32 CellIndex : Standing)
		{
			if (FMantleCellStreamPlan::IsReleased(InfoOf(CellIndex), Path, Settings))
			{
				QueueRemoval(CellIndex);
			}
		}
	}

	// Who is wanted: the cells that are loading, and cells idle now whose drawDistance the path reaches.
	TArray<int32> Candidates;
	if (Settings.bKeepEveryCell)
	{
		// Every cell with something of ours in it (the index counts the solid things: a cell with none has nothing for a SolidOnly streamer).
		Candidates.Reserve(Index.Cells.Num());
		for (int32 CellIndex = 0; CellIndex < Index.Cells.Num(); ++CellIndex)
		{
			if (Settings.Entities != EMantleCellEntities::SolidOnly || Index.Cells[CellIndex].Solid > 0)
			{
				Candidates.Add(CellIndex);
			}
		}
	}
	else
	{
		FMantleCellStreamPlan::CellsNear(Index, LevelOverhangs, LevelRadii, Path, Candidates);
	}

	struct FWanted
	{
		int32 CellIndex;
		FMantleCellStreamPlan::FRank Rank;
	};
	TArray<FWanted> Pool;
	Pool.Reserve(Candidates.Num() + LoadingCells.Num());
	for (const int32 CellIndex : Candidates)
	{
		if (Cells.Contains(CellIndex) || Loader->IsCellFailed(CellIndex))
		{
			continue;
		}
		const FMantleCellInfo& Info = Index.Cells[CellIndex];
		if (Settings.bKeepEveryCell || FMantleCellStreamPlan::IsWanted(Info, Path, Settings))
		{
			Pool.Add(FWanted{ CellIndex, RankFor(CellIndex) });
		}
	}
	for (const int32 CellIndex : LoadingCells)
	{
		Pool.Add(FWanted{ CellIndex, RankFor(CellIndex) });
	}
	TickReport.PlanNearCells += Candidates.Num();
	TickReport.PlanWantedCells += Pool.Num();
	Pool.Sort([](const FWanted& A, const FWanted& B)
	{
		return FMantleCellStreamPlan::BuildsBefore(A.Rank, B.Rank);
	});

	// The nearest few are read; the rest wait their turn. Cells read and waiting to be built count against the read-ahead, so memory stays bounded.
	const int32 Room = FMath::Max(Settings.MaxCellsReadAhead - ReadyCells.Num() - AssetCells.Num(), 0);
	TArray<int32> WantList;
	WantList.Reserve(Room);
	TSet<int32> InList;
	for (int32 At = 0; At < Pool.Num() && At < Room; ++At)
	{
		WantList.Add(Pool[At].CellIndex);
		InList.Add(Pool[At].CellIndex);
	}
	for (const int32 CellIndex : TArray<int32>(LoadingCells))
	{
		if (!InList.Contains(CellIndex))
		{
			// Pushed out by nearer ones: given up (a download in flight is cancelled by the loader).
			Cells.Remove(CellIndex);
			LoadingCells.RemoveSingle(CellIndex);
		}
	}
	for (const int32 CellIndex : WantList)
	{
		if (!Cells.Contains(CellIndex))
		{
			FCell& Cell = Cells.Add(CellIndex);
			Cell.State = ECellState::Loading;
			LoadingCells.Add(CellIndex);
		}
	}
	// Loading cells in the order the loader works them.
	LoadingCells = WantList;
	// The far-only cells follow the real flow's in the loader's order.
	TArray<int32> FarWants;
	CollectFarWants(Path, WantList, FarWants);
	TArray<int32> AllWants = WantList;
	AllWants.Append(FarWants);
	Loader->SetWanted(AllWants);

	// Distances for the cells waiting to be built (the builder takes the nearest first), and the backlog for the stats.
	PendingCells.Reset();
	for (const FWanted& Wanted : Pool)
	{
		PendingCells.Add(Wanted.CellIndex);
	}
	for (const int32 CellIndex : AssetCells)
	{
		FCell& Cell = Cells.FindChecked(CellIndex);
		Cell.Rank = RankFor(CellIndex);
		PendingCells.Add(CellIndex);
	}
	for (const int32 CellIndex : ReadyCells)
	{
		FCell& Cell = Cells.FindChecked(CellIndex);
		Cell.Rank = RankFor(CellIndex);
		PendingCells.Add(CellIndex);
	}
	// A keep-every-cell streamer with nothing left to read or wait for has planned for the last time.
	bKeepAllSettled = Settings.bKeepEveryCell && Pool.IsEmpty() && LoadingCells.IsEmpty() && AssetCells.IsEmpty() && ReadyCells.IsEmpty();
	// Which far sets are wanted, which go, which are built next.
	RefreshFar(Path);
}

void FMantleCellStreamer::TakeFinishedCells()
{
	if (LoadingCells.IsEmpty())
	{
		return;
	}
	for (const int32 CellIndex : TArray<int32>(LoadingCells))
	{
		FCell& Cell = Cells.FindChecked(CellIndex);
		if (!Loader->IsCellReady(CellIndex))
		{
			if (Loader->IsCellFailed(CellIndex))
			{
				// Never to be read: the place stands without it, and nothing waits for it.
				LoadingCells.RemoveSingle(CellIndex);
				Cells.Remove(CellIndex);
				++Stats.CellsFailed;
				++TickReport.CellsFailed;
				bRefreshNeeded = true;
			}
			continue;
		}
		if (!Loader->TakeCell(CellIndex, Cell.Content))
		{
			continue;
		}
		++TickReport.CellsTaken;
		TickReport.CellEntitiesTaken += Cell.Content.Entities.Num();
		PrepareCell(CellIndex, Cell);
		Cell.Rank = RankFor(CellIndex);
		LoadingCells.RemoveSingle(CellIndex);
		bRefreshNeeded = true;
		if (Cell.Content.Entities.IsEmpty())
		{
			// Nothing to build (the index promises at least one entity, but a cell must never wait for ever on a promise): it stands.
			Cell.State = ECellState::Built;
			BuiltCells.Add(CellIndex);
			++Stats.CellsBuiltEver;
			continue;
		}
		bool bAskForAssets = CellAssets.IsValid();
#if !UE_BUILD_SHIPPING
		bAskForAssets = bAskForAssets && !bTestSkipBuild;
#endif
		if (bAskForAssets)
		{
			// The cell's meshes and materials are asked for now; it is built when they are here.
			CellAssets->Acquire(CellIndex, Cell.Content.Entities);
			Cell.State = ECellState::Assets;
			Cell.AssetsAskedAt = FPlatformTime::Seconds();
			AssetCells.Add(CellIndex);
			++Stats.CellAssetsAsked;
			continue;
		}
		Cell.State = ECellState::Ready;
		ReadyCells.Add(CellIndex);
	}
}

void FMantleCellStreamer::TakeAssetCells()
{
	if (AssetCells.IsEmpty() || !CellAssets.IsValid())
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	TickReport.AssetCellsWaiting += AssetCells.Num();
	for (const int32 CellIndex : TArray<int32>(AssetCells))
	{
		FCell& Cell = Cells.FindChecked(CellIndex);
		// A cell that waits costs one cheap look a tick (IsSettled); its lists are read once it has settled or its wait has stalled.
		if (!CellAssets->IsSettled(CellIndex) && !FMantleCellStreamPlan::HasStalled(Now, Cell.AssetsAskedAt, LastAssetProgressSeconds, Settings.AssetIdleTimeoutSeconds))
		{
			continue;
		}
		TArray<FString> Pending;
		TArray<FString> Failed;
		CellAssets->GetState(CellIndex, Pending, Failed);
		const double Waited = Now - Cell.AssetsAskedAt;
		if (!Pending.IsEmpty())
		{
			// A queue that is still moving is waited for, however long a first visit's download takes; only one that has stood still gives up.
			if (!FMantleCellStreamPlan::HasStalled(Now, Cell.AssetsAskedAt, LastAssetProgressSeconds, Settings.AssetIdleTimeoutSeconds))
			{
				continue;
			}
			// Built without what has not arrived: nothing waits for good on a download.
			++Stats.AssetWaitTimeouts;
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_CELLS_ASSET_TIMEOUT cell=%s: %d asset(s) had not arrived after %.0f s, and the place's downloads had not moved for %.0f s; the cell is built without them"),
				*InfoOf(CellIndex).KeyString(), Pending.Num(), Waited, Now - FMath::Max(Cell.AssetsAskedAt, LastAssetProgressSeconds));
			Failed.Append(Pending);
		}
		Stats.LongestAssetWaitSeconds = FMath::Max(Stats.LongestAssetWaitSeconds, Waited);
		AssetCells.RemoveSingle(CellIndex);
		++TickReport.AssetCellsDone;
		if (!Failed.IsEmpty())
		{
			LeaveOutMissing(CellIndex, Cell, Failed);
		}
		Cell.Rank = RankFor(CellIndex);
		bRefreshNeeded = true;
		if (Cell.Content.Entities.IsEmpty())
		{
			// Nothing of it can be built: it stands, and has no use for assets.
			ReleaseAssets(CellIndex);
			Cell.State = ECellState::Built;
			BuiltCells.Add(CellIndex);
			++Stats.CellsBuiltEver;
			continue;
		}
		Cell.State = ECellState::Ready;
		ReadyCells.Add(CellIndex);
	}
}

void FMantleCellStreamer::LeaveOutMissing(const int32 CellIndex, FCell& Cell, const TArray<FString>& Missing)
{
	const TSet<FString> Gone(Missing);
	for (const FString& Id : Missing)
	{
		bool bAlreadyLogged = false;
		MissingAssetsLogged.Add(Id, &bAlreadyLogged);
		if (!bAlreadyLogged)
		{
			++Stats.AssetsMissing;
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_CELLS_ASSET_MISSING asset=%s first seen in cell=%s: cells that use it are built without it (the reason is in the MANTLE_PLACE_ASSETS line for it)"), *Id, *InfoOf(CellIndex).KeyString());
		}
	}
	const int32 Before = Cell.Content.Entities.Num();
	Cell.Content.Entities.RemoveAll([&Gone](const FMantleEntity& Entity)
	{
		const FMantleStaticMeshProps* Props = MantleMeshPropsOf(Entity);
		return Props && Gone.Contains(Props->Mesh);
	});
	Stats.EntitiesWithoutMesh += Before - Cell.Content.Entities.Num();
	// A material that could not be had leaves the slot empty, which keeps the mesh's own.
	for (FMantleEntity& Entity : Cell.Content.Entities)
	{
		FMantleStaticMeshProps* Props = Entity.Props.TryGet<FMantleStaticMeshProps>();
		if (!Props)
		{
			if (FMantleInstancedMeshProps* Instanced = Entity.Props.TryGet<FMantleInstancedMeshProps>())
			{
				Props = &Instanced->Mesh;
			}
		}
		if (!Props)
		{
			continue;
		}
		for (FString& Material : Props->Materials)
		{
			if (Gone.Contains(Material))
			{
				Material.Reset();
			}
		}
	}
}

void FMantleCellStreamer::ReleaseAssets(const int32 CellIndex)
{
	if (CellAssets.IsValid())
	{
		CellAssets->Release(CellIndex);
	}
}

void FMantleCellStreamer::PrepareCell(const int32 CellIndex, FCell& Cell)
{
	// Long lists of solid instances are cut so that one piece is about one step of the budget; solid things are built before pictures. (A list with no collision is at most 4,096 instances, about 0.5 ms: one piece.)
	// The cell's far set is made from everything it holds, before the entities are cut and filtered for this streamer's own flow.
	MakeFarSet(CellIndex, Cell.Content.Entities);
	Cell.PiecesLeft.Reset();
	TArray<FMantleEntity> Solid;
	TArray<FMantleEntity> Visual;
	Solid.Reserve(Cell.Content.Entities.Num());
	for (FMantleEntity& Entity : Cell.Content.Entities)
	{
		const bool bSolid = MantleCellStreamerPrivate::IsSolidMesh(Entity);
		if ((Settings.Entities == EMantleCellEntities::SolidOnly && !bSolid) || (Settings.Entities == EMantleCellEntities::PicturesOnly && bSolid))
		{
			continue; // the other streamer's (a standalone game runs one for each)
		}
		FMantleInstancedMeshProps* Instanced = Entity.Props.TryGet<FMantleInstancedMeshProps>();
		const int32 Pieces = Instanced ? FMantleCellStreamPlan::PieceCount(Instanced->Instances.Num(), bSolid, Settings) : 1;
		Cell.PiecesLeft.Add(Entity.Id, Pieces);
		if (Pieces <= 1)
		{
			(bSolid ? Solid : Visual).Add(MoveTemp(Entity));
			continue;
		}
		const TArray<FTransform> All = MoveTemp(Instanced->Instances);
		Instanced->Instances.Reset();
		for (int32 Piece = 0; Piece < Pieces; ++Piece)
		{
			int32 First = 0;
			int32 Count = 0;
			FMantleCellStreamPlan::PieceRange(All.Num(), Pieces, Piece, First, Count);
			FMantleEntity Cut = Entity;
			Cut.Id = FString::Printf(TEXT("%s~%d"), *Entity.Id, Piece);
			FMantleInstancedMeshProps* CutProps = Cut.Props.TryGet<FMantleInstancedMeshProps>();
			CutProps->Instances.Append(All.GetData() + First, Count);
			Solid.Add(MoveTemp(Cut));
		}
	}
	Cell.bSolid = InfoOf(CellIndex).Solid > 0;
	Cell.SolidPieces = Solid.Num();
	Cell.Content.Entities = MoveTemp(Solid);
	Cell.Content.Entities.Append(MoveTemp(Visual));
	Cell.Cursor = 0;
}

// ---- Work ---------------------------------------------------------------------------------------------

void FMantleCellStreamer::RunWork(const double BudgetSeconds)
{
	const double Start = FPlatformTime::Seconds();
	const double Deadline = Start + BudgetSeconds;
	// Taking cells down is cheap (0.016 ms an entity against 0.34 to build), but a long view jump releases thousands at once: while anything waits to be built
	// it gets a quarter of the budget, and builds get the rest.
	const bool bBuildsWaiting = !ReadyCells.IsEmpty();
	RemoveSome(bBuildsWaiting ? Start + BudgetSeconds * 0.25 : Deadline);
	// The far sets keep a share of the budget while any waits to be built, so a flight keeps the far ring filled; what the real flow does not use is theirs too.
	const double RealDeadline = (FarReadyCells.IsEmpty() || bHolding) ? Deadline : Start + BudgetSeconds * (1.0 - Settings.FarSets.MinShareOfBudget);
	// At least one piece a tick, so a long frame never stops the loader; then pieces until the budget is spent.
	bool bDidOne = false;
	while (!ReadyCells.IsEmpty() && (!bDidOne || FPlatformTime::Seconds() < RealDeadline))
	{
		bDidOne = true;
		if (!BuildOne())
		{
			break;
		}
	}
	// Not during a hold: the join and a teleport wait for the real cells, which get all of the budget; the twins come after.
	while (!bHolding && !FarReadyCells.IsEmpty() && FPlatformTime::Seconds() < Deadline)
	{
		if (!BuildOneFar())
		{
			break;
		}
	}
	RemoveSome(Deadline);
	RemoveFarSome(Deadline);
}

bool FMantleCellStreamer::RemoveSome(const double Deadline)
{
	if (RemovingCells.IsEmpty())
	{
		return false;
	}
	const double Start = FPlatformTime::Seconds();
	bool bAny = false;
	while (!RemovingCells.IsEmpty() && FPlatformTime::Seconds() < Deadline)
	{
		const int32 CellIndex = RemovingCells[0];
		FCell& Cell = Cells.FindChecked(CellIndex);
		// A handful at a time between looks at the clock: a destroy is about 0.016 ms.
		for (int32 Count = 0; Count < 8 && Cell.RemoveCursor < Cell.Actors.Num(); ++Count)
		{
			if (AActor* Actor = Cell.Actors[Cell.RemoveCursor].Get())
			{
				Actor->Destroy();
			}
			++Cell.RemoveCursor;
			++Stats.ActorsRemoved;
			bAny = true;
		}
		if (Cell.RemoveCursor >= Cell.Actors.Num())
		{
			RemovingCells.RemoveAt(0);
			ReleaseAssets(CellIndex);
			Cells.Remove(CellIndex);
			++Stats.CellsReleasedEver;
			bRefreshNeeded = true;
		}
	}
	Stats.RemoveSeconds += FPlatformTime::Seconds() - Start;
	return bAny;
}

bool FMantleCellStreamer::BuildOne()
{
	// The cell that is nearest the path builds first; a nearer one that arrives while another is half built takes over, and the other waits.
	int32 Best = INDEX_NONE;
	for (const int32 CellIndex : ReadyCells)
	{
		if (Best == INDEX_NONE)
		{
			Best = CellIndex;
			continue;
		}
		const FCell& A = Cells.FindChecked(CellIndex);
		const FCell& B = Cells.FindChecked(Best);
		if (FMantleCellStreamPlan::BuildsBefore(A.Rank, B.Rank))
		{
			Best = CellIndex;
		}
	}
	if (Best == INDEX_NONE)
	{
		return false;
	}
	UWorld* const LiveWorld = World.Get();
	FCell& Cell = Cells.FindChecked(Best);
	if (!LiveWorld || !Cell.Content.Entities.IsValidIndex(Cell.Cursor))
	{
		return false;
	}

	const double Start = FPlatformTime::Seconds();
	FMantleEntity& Entity = Cell.Content.Entities[Cell.Cursor];
	const FString EntityId = Entity.Id;
	const FString EntityName = Entity.Name;
	int64 InstanceCount = 1;
	const bool bInstanced = Entity.Type == EMantleEntityType::InstancedMesh;
	if (const FMantleInstancedMeshProps* Instanced = Entity.Props.TryGet<FMantleInstancedMeshProps>())
	{
		InstanceCount = Instanced->Instances.Num();
	}
	const bool bSolid = MantleCellStreamerPrivate::IsSolidMesh(Entity);
	// Which mesh, and whether this is the first time this streamer builds it (the first body of a mesh may cost more: the slow-piece log says).
	const FMantleStaticMeshProps* MeshProps = MantleMeshPropsOf(Entity);
	const FString MeshId = MeshProps ? MeshProps->Mesh : FString();
	bool bFirstOfMesh = false;
	MeshesBuilt.Add(MeshId, &bFirstOfMesh);
	bFirstOfMesh = !bFirstOfMesh;

	int32 PieceIndex = 0;
	const FString BaseId = MantleCellStreamerPrivate::BaseEntityId(EntityId, PieceIndex);
	FMantleBuildResult Built;
	if (PieceIndex > 0)
	{
		// The later pieces of a long list of solid instances go into the component the first piece made, one piece a step: the entity is one actor, and its bodies are made as its
		// instances are added, each static and in place. (The first piece of a list is built as any entity is.)
		FMantleInstancedMeshProps* Instanced = Entity.Props.TryGet<FMantleInstancedMeshProps>();
		const TWeakObjectPtr<AActor>* First = Cell.PieceActors.Find(BaseId);
		AActor* FirstActor = First ? First->Get() : nullptr;
		UInstancedStaticMeshComponent* Component = (FirstActor && IsValid(FirstActor)) ? FirstActor->FindComponentByClass<UInstancedStaticMeshComponent>() : nullptr;
		// A dedicated server's first piece of a static, solid set is a merged component (a few bodies, many shapes each): the later pieces go into it the same way.
		UMantleMergedInstancesComponent* MergedComponent = (FirstActor && IsValid(FirstActor) && !Component) ? FirstActor->FindComponentByClass<UMantleMergedInstancesComponent>() : nullptr;
#if !UE_BUILD_SHIPPING
		if (!bTestSkipBuild)
#endif
		{
			if (Instanced && Component)
			{
				Component->AddInstances(Instanced->Instances, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ false);
			}
			else if (Instanced && MergedComponent)
			{
				MergedComponent->AddInstances(Instanced->Instances);
			}
			else if (First && !(FirstActor && IsValid(FirstActor)))
			{
				// The first piece was built and is gone: a script destroyed the entity (the server's destroy is applied the moment a piece of it stands). That is
				// the entity's end, not an error; its later pieces have nowhere to go and are skipped.
				++Stats.PiecesSkippedDestroyed;
			}
			else
			{
				++Stats.BuildErrors;
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_CELLS_BUILD_ERROR cell=%s entity=%s: its first piece is gone, so the piece could not be added"), *InfoOf(Best).KeyString(), *EntityId);
			}
		}
		Entity = FMantleEntity();
	}
	else
	{
		// One entity as a place of its own: Build runs all of the builder's passes for it in this call, so what the physics solver first sees is final.
		SlicePlace.Entities.Reset();
		SlicePlace.Entities.Add(MoveTemp(Entity));
		Entity = FMantleEntity();
#if !UE_BUILD_SHIPPING
		if (!bTestSkipBuild)
#endif
		{
			Built = FMantlePlaceBuilder::Build(LiveWorld, SlicePlace, SliceOptions);
		}
		SlicePlace.Entities.Reset();
	}

	for (const TPair<FString, AActor*>& Pair : Built.Actors)
	{
		if (IsValid(Pair.Value))
		{
			Cell.Actors.Add(Pair.Value);
			if (EntityId != BaseId)
			{
				Cell.PieceActors.Add(BaseId, Pair.Value);
			}
			if (PieceBuilt)
			{
				Cell.EntityIds.AddUnique(BaseId);
				PieceBuilt(BaseId, EntityName, Pair.Value);
			}
		}
	}
	if (!Built.Errors.IsEmpty())
	{
		for (const FMantleParseError& Error : Built.Errors)
		{
			if (Stats.BuildErrors < MantleCellStreamerPrivate::MaxLoggedBuildErrors)
			{
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_CELLS_BUILD_ERROR cell=%s entity=%s: %s"), *InfoOf(Best).KeyString(), *EntityId, *Error.ToString());
			}
			++Stats.BuildErrors;
		}
	}
	// An entity stands when its last piece is built; its far twin, if it is drawn, is handed off in this step. An entity whose first piece did not build never stands.
	if (PieceIndex == 0 && Built.Actors.IsEmpty())
	{
		Cell.PiecesLeft.Remove(BaseId);
	}
	else if (int32* PiecesLeft = Cell.PiecesLeft.Find(BaseId))
	{
		if (--(*PiecesLeft) <= 0)
		{
			Cell.PiecesLeft.Remove(BaseId);
			OnRealEntityStanding(Best, BaseId);
		}
	}
	++Cell.Cursor;
	++Stats.UnitsBuilt;
	Stats.InstancesBuilt += bInstanced ? InstanceCount : 0;
	Stats.SolidBuilt += bSolid ? InstanceCount : 0;

	const double Seconds = FPlatformTime::Seconds() - Start;
	Stats.BuildSeconds += Seconds;
	TickReport.LongestPieceSeconds = FMath::Max(TickReport.LongestPieceSeconds, Seconds);
	Stats.PiecesOver2Ms += Seconds >= 0.002 ? 1 : 0;
	Stats.PiecesOver5Ms += Seconds >= 0.005 ? 1 : 0;
	Stats.PiecesOver10Ms += Seconds >= 0.010 ? 1 : 0;
	Stats.PiecesOver20Ms += Seconds >= 0.020 ? 1 : 0;
	if (Seconds > Stats.LongestUnitSeconds)
	{
		Stats.LongestUnitSeconds = Seconds;
		Stats.LongestUnitEntity = EntityId;
	}
	if (Seconds >= MantleCellStreamerPrivate::SlowUnitSeconds)
	{
		// Where the time went (the builder's own clocks): loading the mesh and materials, spawning the actor, applying the props (the physics body is made there); the rest is the bookkeeping around them.
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CELLS_SLOW_UNIT cell=%s entity=%s mesh=%s first_of_mesh=%d instances=%lld solid=%d ms=%.1f load=%.1f spawn=%.1f props=%.1f other=%.1f"),
			*InfoOf(Best).KeyString(), *EntityId, *MeshId, bFirstOfMesh ? 1 : 0, InstanceCount, bSolid ? 1 : 0, Seconds * 1000.0, Built.LoadSeconds * 1000.0, Built.SpawnSeconds * 1000.0, Built.PropsSeconds * 1000.0,
			(Seconds - Built.SpawnSeconds - Built.PropsSeconds) * 1000.0);
	}

	if (Cell.Cursor >= Cell.Content.Entities.Num())
	{
		Cell.Content = FMantleCellContent();
		Cell.Cursor = 0;
		Cell.State = ECellState::Built;
		ReadyCells.RemoveSingle(Best);
		BuiltCells.Add(Best);
		++Stats.CellsBuiltEver;
		bRefreshNeeded = true;
	}
	return true;
}

void FMantleCellStreamer::ReleaseEntities(FCell& Cell)
{
	if (EntityReleased)
	{
		for (const FString& EntityId : Cell.EntityIds)
		{
			EntityReleased(EntityId);
		}
	}
	Cell.EntityIds.Reset();
}

void FMantleCellStreamer::QueueRemoval(const int32 CellIndex)
{
	FCell& Cell = Cells.FindChecked(CellIndex);
	// The script world forgets the entities before their actors go (the actors are destroyed a few a frame).
	ReleaseEntities(Cell);
	// What stood here goes: the cell's twins are drawn from now on (the real entities draw nothing past their own distance anyway).
	OnRealCellLeaving(CellIndex);
	BuiltCells.RemoveSingle(CellIndex);
	Cell.State = ECellState::Removing;
	++TakeDownSerial;
	// Only a plan hands a cell to its take-down.
	++TickReport.PlanReleasedCells;
	Cell.RemoveCursor = 0;
	RemovingCells.Add(CellIndex);
}

// ---- Hold ---------------------------------------------------------------------------------------------

void FMantleCellStreamer::BeginHold(const TCHAR* Reason)
{
	if (bHolding)
	{
		return;
	}
	SetHolding(true, Reason);
}

void FMantleCellStreamer::SetHolding(const bool bNow, const TCHAR* Reason)
{
	if (bHolding == bNow)
	{
		return;
	}
	bHolding = bNow;
	const double Now = FPlatformTime::Seconds();
	if (bNow)
	{
		HoldStartSeconds = Now;
		HoldReason = Reason;
		const double Radius = HoldRadiusNow();
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CELLS_HOLD begun: %s (until %s stand; gives up after %.0f s with nothing arriving)"),
			Reason,
			Settings.bKeepEveryCell ? TEXT("every cell") : (Radius >= UE_DOUBLE_BIG_NUMBER ? TEXT("the cells the view wants") : *FString::Printf(TEXT("the cells within %.0f m of the view"), Radius / 100.0)),
			Settings.HoldIdleTimeoutSeconds);
	}
	else
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CELLS_HOLD ended: %s after %.2f s (%s), cells standing %d, backlog %d"),
			Reason, Now - HoldStartSeconds, *HoldReason, BuiltCells.Num(), PendingCells.Num());
	}
	// A hold that ends lets the far sets start the work they held back.
	bRefreshNeeded = true;
	if (HoldChanged)
	{
		HoldChanged(bNow);
	}
}

void FMantleCellStreamer::CheckHold()
{
	if (!bHolding)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	if (IsHoldAreaBuilt())
	{
		SetHolding(false, TEXT("built"));
	}
	else if (FMantleCellStreamPlan::HasStalled(Now, HoldStartSeconds, LastProgressSeconds, Settings.HoldIdleTimeoutSeconds))
	{
		const FAreaCount Area = CountHoldArea();
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_CELLS_HOLD gave up after %.0f s: nothing arrived for %.0f s with the area not built (%d of %d cells stand, %d pending): the player is let go"),
			Now - HoldStartSeconds, Now - FMath::Max(HoldStartSeconds, LastProgressSeconds), Area.Done, Area.Cells, PendingCells.Num());
		bOwnGroundGaveUp = bOwnGroundGaveUp || HoldReason == TEXT("ground");
		bJoinHoldGaveUp = bJoinHoldGaveUp || bJoinPending;
		SetHolding(false, TEXT("timeout"));
	}
}

bool FMantleCellStreamer::IsHoldAreaBuilt() const
{
	return Settings.bKeepEveryCell ? IsEverythingBuilt() : IsAreaBuilt(View.Location, HoldRadiusNow());
}

void FMantleCellStreamer::SampleProgress(const double DeltaSeconds)
{
	const double Now = FPlatformTime::Seconds();
	// This streamer's own work, cheap to count: a cell file read, a piece or a cell built, a cell found unreadable (a hold waits for it no longer).
	const int64 OwnMark = Stats.UnitsBuilt + Stats.CellsBuiltEver + Stats.CellsFailed + GetFilesRead();
	if (OwnMark != LastOwnProgressMark)
	{
		LastOwnProgressMark = OwnMark;
		LastProgressSeconds = Now;
	}
	// The place's downloads: a walk over the asset set, so only while a hold or an asset wait needs it, and a few times a second.
	SecondsSinceAssetSample += DeltaSeconds;
	const bool bWaitingOnAssets = bHolding || !AssetCells.IsEmpty() || !FarAssetCells.IsEmpty();
	if (!CellAssets.IsValid() || !bWaitingOnAssets || (bAssetProgressMarkKnown && SecondsSinceAssetSample < Settings.ProgressSampleSeconds))
	{
		return;
	}
	SecondsSinceAssetSample = 0.0;
	const int64 AssetMark = CellAssets->GetProgressMark();
	if (!bAssetProgressMarkKnown || AssetMark != LastAssetProgressMark)
	{
		// The first look counts as progress: the waits begin from when they began, not from a mark never seen.
		bAssetProgressMarkKnown = true;
		LastAssetProgressMark = AssetMark;
		LastAssetProgressSeconds = Now;
		LastProgressSeconds = Now;
	}
}

bool FMantleCellStreamer::IsAreaBuilt(const FVector& Point, const double RadiusCm) const
{
	if (!bIndexReady)
	{
		return false;
	}
	const FAreaCount Area = CountArea(Point, RadiusCm);
	return Area.Done >= Area.Cells;
}

FMantleCellStreamer::FAreaCount FMantleCellStreamer::CountArea(const FVector& Point, const double RadiusCm) const
{
	FAreaCount Area;
	if (!bIndexReady || bStopped)
	{
		return Area;
	}
	const FMantleCellIndex& Index = Loader->GetIndex();
	FMantleCellStreamPlan::FPath Path;
	Path.Add(Point);
	// No cell is wanted from farther than its level's largest drawDistance, so a radius without a limit (the join's) looks no farther than that.
	TArray<double> Radii;
	Radii.SetNumUninitialized(Index.Levels.Num());
	for (int32 Level = 0; Level < Index.Levels.Num(); ++Level)
	{
		Radii[Level] = LevelRadii.IsValidIndex(Level) ? FMath::Min(RadiusCm, LevelRadii[Level]) : RadiusCm;
	}
	TArray<int32> Near;
	FMantleCellStreamPlan::CellsNear(Index, LevelOverhangs, Radii, Path, Near);
	for (const int32 CellIndex : Near)
	{
		const FMantleCellInfo& Info = Index.Cells[CellIndex];
		const double Distance = FMantleCellStreamPlan::DistanceToCell(Info, Point);
		// A cell the streamer wants at all from here: within its reach, which a live budget (stage 8) may have shortened, so a hold never waits for a cell that is not wanted.
		if (Distance > RadiusCm || Distance > FMantleCellStreamPlan::ReachOf(Info, Settings))
		{
			continue;
		}
		++Area.Cells;
		const FCell* Cell = Cells.Find(CellIndex);
		if (Loader->IsCellFailed(CellIndex) || (Cell && Cell->State == ECellState::Built))
		{
			++Area.Done;
		}
	}
	return Area;
}

FMantleCellStreamer::FAreaCount FMantleCellStreamer::CountHoldArea() const
{
	if (!Settings.bKeepEveryCell)
	{
		return CountArea(View.Location, HoldRadiusNow());
	}
	// Every cell with something for this streamer to build: what stands, against that and what is still wanted.
	FAreaCount Area;
	Area.Done = BuiltCells.Num() + Stats.CellsFailed;
	Area.Cells = Area.Done + PendingCells.Num();
	return Area;
}

FMantleCellAreaState FMantleCellStreamer::QueryArea(const FMantleCellAreaQuery& Query) const
{
	FMantleCellAreaState State;
	if (!bIndexReady || bStopped)
	{
		State.bUnknown = true;
		return State;
	}
	const FMantleCellIndex& Index = Loader->GetIndex();
	FMantleCellStreamPlan::FPath Path;
	Path.Add(Query.Point);
	TArray<double> Radii;
	Radii.Init(Query.RadiusCm, Index.Levels.Num());
	TArray<int32> Near;
	FMantleCellStreamPlan::CellsNear(Index, LevelOverhangs, Radii, Path, Near);
	for (const int32 CellIndex : Near)
	{
		const FMantleCellInfo& Info = Index.Cells[CellIndex];
		if (FMantleCellStreamPlan::DistanceToCell(Info, Query.Point) > Query.RadiusCm || (Query.bSolidOnly && Info.Solid <= 0))
		{
			continue;
		}
		++State.Cells;
		if (Loader->IsCellFailed(CellIndex))
		{
			++State.Failed;
			continue;
		}
		const FCell* Cell = Cells.Find(CellIndex);
		const bool bStands = Cell && (Cell->State == ECellState::Built || (Query.bSolidOnly && Cell->State == ECellState::Ready && Cell->Cursor >= Cell->SolidPieces));
		if (bStands)
		{
			++State.Built;
			if (Query.bWantCellIds)
			{
				State.BuiltCellIds.Add(CellIndex);
			}
		}
		else if (Cell || PendingCells.Contains(CellIndex))
		{
			++State.Pending;
		}
	}
	State.bPlanStale = bRefreshNeeded;
	return State;
}

uint64 FMantleCellStreamer::GetAreaSerial() const
{
	return static_cast<uint64>(Stats.UnitsBuilt) + static_cast<uint64>(Stats.ActorsRemoved) + static_cast<uint64>(Stats.CellsBuiltEver)
		+ static_cast<uint64>(Stats.CellsReleasedEver) + static_cast<uint64>(Stats.CellsFailed) + (bIndexReady ? 1 : 0);
}

bool FMantleCellStreamer::IsEverythingBuilt() const
{
	// Planned since the last cell stood (a built cell asks for a new plan), and nothing is waiting to be read or built.
	return bIndexReady && !bRefreshNeeded && PendingCells.IsEmpty() && LoadingCells.IsEmpty() && AssetCells.IsEmpty() && ReadyCells.IsEmpty();
}

bool FMantleCellStreamer::IsCellBuilt(const int32 CellIndex) const
{
	const FCell* Cell = Cells.Find(CellIndex);
	return Cell && Cell->State == ECellState::Built;
}

// ---- Stats --------------------------------------------------------------------------------------------

const TCHAR* FMantleCellTickReport::PhaseName(const int32 Phase)
{
	switch (Phase)
	{
	case Pump: return TEXT("pump");
	case View: return TEXT("view");
	case Progress: return TEXT("progress");
	case Refresh: return TEXT("refresh");
	case TakeFinished: return TEXT("take");
	case TakeAssets: return TEXT("assets");
	case TakeFarFinished: return TEXT("far take");
	case TakeFarAssets: return TEXT("far assets");
	case AssetView: return TEXT("asset view");
	case Remove: return TEXT("remove");
	case Build: return TEXT("build");
	case FarBuild: return TEXT("far build");
	case FarRemove: return TEXT("far remove");
	case WorkRest: return TEXT("work rest");
	case Stats: return TEXT("stats");
	case Budget: return TEXT("budget");
	case Audit: return TEXT("audit");
	case Ground: return TEXT("ground");
	case Hold: return TEXT("hold");
	default: return TEXT("other");
	}
}

double FMantleCellTickReport::OtherSeconds() const
{
	double Sum = 0.0;
	for (int32 Phase = 0; Phase < PhaseCount; ++Phase)
	{
		Sum += Seconds[Phase];
	}
	return FMath::Max(TotalSeconds - Sum, 0.0);
}

int32 FMantleCellTickReport::SlowestPhase() const
{
	int32 Slowest = 0;
	for (int32 Phase = 1; Phase < PhaseCount; ++Phase)
	{
		if (Seconds[Phase] > Seconds[Slowest])
		{
			Slowest = Phase;
		}
	}
	return Slowest;
}

FString FMantleCellTickReport::ToLogLine() const
{
	const auto Ms = [this](const int32 Phase) { return Seconds[Phase] * 1000.0; };
	const int32 Slowest = SlowestPhase();
	FString Line = FString::Printf(TEXT("MANTLE_CELLS_SLOW_TICK %.1f ms (slowest: %s %.1f ms; frame %.1f ms, work budget %.1f ms):"),
		TotalSeconds * 1000.0, PhaseName(Slowest), Ms(Slowest), DeltaSeconds * 1000.0, BudgetSeconds * 1000.0);
	Line += FString::Printf(TEXT(" pump %.1f | view %.1f | progress %.1f"), Ms(Pump), Ms(View), Ms(Progress));
	Line += FString::Printf(TEXT(" | refresh %.1f (plans %d, near %d, wanted %d, released %d)"), Ms(Refresh), Plans, PlanNearCells, PlanWantedCells, PlanReleasedCells);
	Line += FString::Printf(TEXT(" | take %.1f (cells %d, entities %d, failed %d)"), Ms(TakeFinished), CellsTaken, CellEntitiesTaken, CellsFailed);
	Line += FString::Printf(TEXT(" | assets %.1f (waiting %d, done %d)"), Ms(TakeAssets), AssetCellsWaiting, AssetCellsDone);
	Line += FString::Printf(TEXT(" | far take %.1f (cells %d, entities %d)"), Ms(TakeFarFinished), FarCellsTaken, FarCellEntitiesTaken);
	Line += FString::Printf(TEXT(" | far assets %.1f (waiting %d, done %d)"), Ms(TakeFarAssets), FarAssetCellsWaiting, FarAssetCellsDone);
	Line += FString::Printf(TEXT(" | asset view %.1f"), Ms(AssetView));
	Line += FString::Printf(TEXT(" | remove %.1f (actors %lld, cells %lld)"), Ms(Remove), ActorsRemoved, CellsReleased);
	Line += FString::Printf(TEXT(" | build %.1f (pieces %lld, instances %lld, cells %lld, longest piece %.1f)"), Ms(Build), PiecesBuilt, InstancesBuilt, CellsBuilt, LongestPieceSeconds * 1000.0);
	Line += FString::Printf(TEXT(" | far build %.1f (twins %lld, instances %lld, sets %lld)"), Ms(FarBuild), TwinsBuilt, TwinInstancesBuilt, FarSetsBuilt);
	Line += FString::Printf(TEXT(" | far remove %.1f (twins %d, sets %lld)"), Ms(FarRemove), TwinsRemoved, FarSetsReleased);
	Line += FString::Printf(TEXT(" | work rest %.1f"), Ms(WorkRest));
	Line += FString::Printf(TEXT(" | stats %.1f (pending %d)"), Ms(Stats), PendingCells);
	Line += FString::Printf(TEXT(" | budget %.1f (looks %d)"), Ms(Budget), BudgetLooks);
	Line += FString::Printf(TEXT(" | audit %.1f (sets %d)"), Ms(Audit), FarSetsAudited);
	Line += FString::Printf(TEXT(" | ground %.1f (looks %d)"), Ms(Ground), GroundLooks);
	Line += FString::Printf(TEXT(" | hold %.1f"), Ms(Hold));
	Line += FString::Printf(TEXT(" | other %.1f"), OtherSeconds() * 1000.0);
	Line += FString::Printf(TEXT("; now holding %d, ladder %.2f, cells standing %d ready %d waiting for assets %d loading %d going %d, far sets %d"),
		bHolding ? 1 : 0, Ladder, CellsStanding, CellsReady, CellsAssetWait, CellsLoading, CellsRemoving, FarSets);
	return Line;
}

FString FMantleCellActorCount::ToLogText() const
{
	return FString::Printf(TEXT("actors %d (components %lld) in %d standing cells, %d (components %lld) in %d cells being built or taken down, far actors %d (components %lld) in %d far sets (counted in %.1f ms)"),
		StandingActors, StandingComponents, StandingCells, OtherActors, OtherComponents, OtherCells, FarActors, FarComponents, FarSets, Seconds * 1000.0);
}

FMantleCellActorCount FMantleCellStreamer::CountActors() const
{
	const double Start = FPlatformTime::Seconds();
	FMantleCellActorCount Count;
	for (const TPair<int32, FCell>& Pair : Cells)
	{
		int32 Actors = 0;
		int64 Components = 0;
		for (const TWeakObjectPtr<AActor>& Weak : Pair.Value.Actors)
		{
			// A destroyed actor (one a take-down has passed, or a script's) is gone from the count.
			const AActor* Actor = Weak.Get();
			if (Actor && IsValid(Actor))
			{
				++Actors;
				Components += Actor->GetComponents().Num();
			}
		}
		if (Pair.Value.State == ECellState::Built)
		{
			++Count.StandingCells;
			Count.StandingActors += Actors;
			Count.StandingComponents += Components;
		}
		else if (Actors > 0)
		{
			++Count.OtherCells;
			Count.OtherActors += Actors;
			Count.OtherComponents += Components;
		}
	}
	for (const TPair<int32, FFarSet>& Pair : FarSets)
	{
		int32 Actors = 0;
		for (const FFarEntity& Far : Pair.Value.Entities)
		{
			const AActor* Actor = Far.Actor.Get();
			if (Actor && IsValid(Actor))
			{
				++Actors;
				Count.FarComponents += Actor->GetComponents().Num();
			}
		}
		Count.FarActors += Actors;
		Count.FarSets += Actors > 0 ? 1 : 0;
	}
	Count.Seconds = FPlatformTime::Seconds() - Start;
	return Count;
}

void FMantleCellStreamer::UpdateStats()
{
	Stats.CellsLoading = LoadingCells.Num();
	Stats.CellsAssetWait = AssetCells.Num();
	Stats.CellsReady = ReadyCells.Num();
	Stats.CellsBuilt = BuiltCells.Num();
	Stats.CellsRemoving = RemovingCells.Num();
	Stats.FarSetsStanding = 0;
	for (const TPair<int32, FFarSet>& FarPair : FarSets)
	{
		Stats.FarSetsStanding += FarPair.Value.State == EFarState::Built ? 1 : 0;
	}
	Stats.CellsPending = 0;
	Stats.NearestPendingCm = -1.0;
	Stats.NearestPendingSolidCm = -1.0;
	Stats.NearestPendingAheadSolidCm = -1.0;
	const double Speed = View.Velocity.Size();
	const FVector Heading = Speed > UE_DOUBLE_SMALL_NUMBER ? View.Velocity / Speed : FVector::ZeroVector;
	const FMantleCellIndex& Index = Loader->GetIndex();
	for (const int32 CellIndex : PendingCells)
	{
		const FCell* Cell = Cells.Find(CellIndex);
		if (Cell && (Cell->State == ECellState::Built || Cell->State == ECellState::Removing))
		{
			continue;
		}
		++Stats.CellsPending;
		const FMantleCellInfo& Info = Index.Cells[CellIndex];
		const double Distance = FMantleCellStreamPlan::DistanceToCell(Info, View.Location);
		if (Stats.NearestPendingCm < 0.0 || Distance < Stats.NearestPendingCm)
		{
			Stats.NearestPendingCm = Distance;
		}
		if (Info.Solid > 0 && (Stats.NearestPendingSolidCm < 0.0 || Distance < Stats.NearestPendingSolidCm))
		{
			Stats.NearestPendingSolidCm = Distance;
		}
		if (Info.Solid > 0 && Speed >= Settings.MinLookAheadSpeedCmPerSec && (Stats.NearestPendingAheadSolidCm < 0.0 || Distance < Stats.NearestPendingAheadSolidCm)
			&& FVector::DotProduct(Info.Bounds.GetClosestPointTo(View.Location) - View.Location, Heading) >= 0.0)
		{
			Stats.NearestPendingAheadSolidCm = Distance;
		}
	}
}

void FMantleCellStreamer::LogSummary(const TCHAR* Why) const
{
	UE_LOG(LogMantle, Log,
		TEXT("MANTLE_CELLS_SUMMARY %s: cells built %lld released %lld standing %d, pieces %lld (over 2 ms %lld, 5 ms %lld, 10 ms %lld, 20 ms %lld), instances %lld, actors taken down %lld, build %.2f s remove %.2f s, longest piece %.2f ms (%s), worst tick %.2f ms, slow ticks %lld, build errors %d; cell assets asked %lld, longest wait %.2f s, timeouts %d, missing %d, entities left out %lld"),
		Why, Stats.CellsBuiltEver, Stats.CellsReleasedEver, Stats.CellsBuilt, Stats.UnitsBuilt, Stats.PiecesOver2Ms, Stats.PiecesOver5Ms, Stats.PiecesOver10Ms, Stats.PiecesOver20Ms,
		Stats.InstancesBuilt, Stats.ActorsRemoved, Stats.BuildSeconds, Stats.RemoveSeconds,
		Stats.LongestUnitSeconds * 1000.0, Stats.LongestUnitEntity.IsEmpty() ? TEXT("-") : *Stats.LongestUnitEntity, Stats.WorstWorkSeconds * 1000.0, Stats.SlowTicks, Stats.BuildErrors,
		Stats.CellAssetsAsked, Stats.LongestAssetWaitSeconds, Stats.AssetWaitTimeouts, Stats.AssetsMissing, Stats.EntitiesWithoutMesh);
	LogFarSummary(Why);
	if (Stats.BudgetLooks > 0)
	{
		UE_LOG(LogMantle, Log,
			TEXT("MANTLE_CELLS_BUDGET_SUMMARY %s: %d looks, ladder now %.2f lowest %.2f, pressure now %.2f worst %.2f; at the last look instances %lld, solid %lld, far instances %lld, Nanite root pages %lld, textures %.0f MB, process memory %.3f of the machine (%.0f MB; now working set %.0f MB, private %.0f MB), card %.0f of %.0f MB the driver gives (pressure %.2f), full texture detail %.0f m (shortest %.0f m); now %s"),
			Why, Stats.BudgetLooks, Stats.BudgetLadder, Stats.BudgetLowestLadder, Stats.BudgetPressure, Stats.BudgetWorstPressure, Stats.BudgetInstances, Stats.BudgetSolidThings,
			Stats.BudgetFarInstances, Stats.BudgetNaniteRootPages, Stats.BudgetTextureBytes / (1024.0 * 1024.0), Stats.BudgetMemoryShare, Stats.BudgetMemoryBytes / (1024.0 * 1024.0),
			FPlatformMemory::GetStats().UsedPhysical / (1024.0 * 1024.0), FPlatformMemory::GetStats().UsedVirtual / (1024.0 * 1024.0),
			Stats.BudgetVideoUsedBytes / (1024.0 * 1024.0), Stats.BudgetVideoBudgetBytes / (1024.0 * 1024.0), Stats.BudgetVideoPressure, Stats.BudgetTextureDetailCm / 100.0,
			Stats.BudgetLowestTextureDetailCm / 100.0, *CountActors().ToLogText());
	}
}

#if !UE_BUILD_SHIPPING
bool FMantleCellStreamer::SetTestSetting(const FString& Name, const double Value)
{
	if (Name == TEXT("lookahead"))
	{
		Settings.LookAheadSeconds = Value;
	}
	else if (Name == TEXT("budget"))
	{
		Settings.BaseBudgetSeconds = Value / 1000.0;
	}
	else if (Name == TEXT("fastbudget"))
	{
		Settings.FastBudgetSeconds = Value / 1000.0;
	}
	else if (Name == TEXT("readahead"))
	{
		Settings.MaxCellsReadAhead = FMath::Max(static_cast<int32>(Value), 1);
	}
	else if (Name == TEXT("release"))
	{
		Settings.ReleaseMargin = Value;
	}
	else if (Name == TEXT("skipbuild"))
	{
		bTestSkipBuild = Value != 0.0;
	}
	else if (Name == TEXT("farsets"))
	{
		Settings.FarSets.bEnabled = Value != 0.0;
	}
	else if (Name == TEXT("keepall"))
	{
		Settings.bKeepEveryCell = Value != 0.0;
	}
	else
	{
		return false;
	}
	bRefreshNeeded = true;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CELLS_SETTING %s = %g"), *Name, Value);
	return true;
}
#endif

void FMantleCellStreamer::Stop()
{
	if (bStopped && Cells.IsEmpty())
	{
		return;
	}
	LogSummary(TEXT("stop"));
	bStopped = true;
	// Nothing reads the index any more: the loader is going.
	bIndexReady = false;
	ExtraViews.Reset();
	int32 Destroyed = 0;
	for (TPair<int32, FCell>& Pair : Cells)
	{
		ReleaseEntities(Pair.Value);
		ReleaseAssets(Pair.Key);
		for (TWeakObjectPtr<AActor>& Weak : Pair.Value.Actors)
		{
			if (AActor* Actor = Weak.Get())
			{
				Actor->Destroy();
				++Destroyed;
			}
		}
	}
	for (TPair<int32, FFarSet>& Pair : FarSets)
	{
		ReleaseFarAssets(Pair.Key, Pair.Value);
		for (FFarEntity& Far : Pair.Value.Entities)
		{
			if (AActor* Actor = Far.Actor.Get())
			{
				Actor->Destroy();
				++Destroyed;
			}
		}
	}
	FarSets.Reset();
	FarLoadingCells.Reset();
	FarAssetCells.Reset();
	FarReadyCells.Reset();
	FarRemovingCells.Reset();
	Cells.Reset();
	ReadyCells.Reset();
	AssetCells.Reset();
	LoadingCells.Reset();
	BuiltCells.Reset();
	RemovingCells.Reset();
	PendingCells.Reset();
	if (Loader.IsValid())
	{
		Loader.Reset();
	}
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CELLS_STREAM stopped: %d actors taken down"), Destroyed);
}
