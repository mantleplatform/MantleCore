// Copyright Joshua Eger. All Rights Reserved.
// Far sets: the part of FMantleCellStreamer that keeps a thinned twin of each cell's big entities standing past the cell's drawDistance (stage 3a of large places).
#include "MantleCellStreamer.h"

#include "MantleAssetLibrary.h"
#include "MaterialShared.h"
#include "MantleRuntime.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleCellStreamerFarPrivate
{
	/** The far set's assets are asked for under this holder: the cell's index plus this, so a cell's real entities and its twins are two holders of the same assets. */
	constexpr int32 FarHolderBase = MantleAssetTiers::FarHolderBase;

	/** A far-only cell is read while fewer than this many far cells are waiting for their assets or their build. */
	constexpr int32 MaxFarCellsInFlight = 4;

	/** A twin that takes this long to build is named in the log. */
	constexpr double SlowFarUnitSeconds = 0.010;

	bool HasCollision(const FMantleEntity& Entity)
	{
		const FMantleStaticMeshProps* Mesh = MantleMeshPropsOf(Entity);
		return Mesh && Mesh->Collision != EMantleCollision::None;
	}

	/** Draws or hides the twin's actor. True when that changed what is registered. */
	bool SetShown(AActor* Actor, const bool bShow)
	{
		USceneComponent* Root = IsValid(Actor) ? Actor->GetRootComponent() : nullptr;
		if (!Root)
		{
			return false;
		}
		if (bShow && !Root->IsRegistered())
		{
			Actor->RegisterAllComponents();
			return true;
		}
		if (!bShow && Root->IsRegistered())
		{
			Actor->UnregisterAllComponents();
			return true;
		}
		return false;
	}

	bool IsShown(const AActor* Actor)
	{
		const USceneComponent* Root = IsValid(Actor) ? Actor->GetRootComponent() : nullptr;
		return Root && Root->IsRegistered();
	}
}

bool FMantleCellStreamer::FarEnabled() const
{
	return Settings.FarSets.bEnabled && !Settings.bKeepEveryCell && !BuildsOnlySolids(Settings.Entities);
}

void FMantleCellStreamer::MakeFarSet(const int32 CellIndex, const TArray<FMantleEntity>& Source)
{
	using namespace MantleCellStreamerFarPrivate;
	if (!FarEnabled() || FarSets.Contains(CellIndex))
	{
		return;
	}
	const FMantleCellInfo& Info = InfoOf(CellIndex);
	if (!FMantleFarSetPlan::CellHasFarSet(Info, Settings.FarSets))
	{
		return;
	}
	const double Reach = FMantleFarSetPlan::ReachOf(Info, Settings.FarSets);
	FFarSet Set;
	for (const FMantleEntity& Entity : Source)
	{
		FMantleEntity Twin;
		if (!FMantleFarSetPlan::MakeTwin(Entity, Reach, Settings.FarSets, Twin))
		{
			continue;
		}
		// What was done to the entity here (destroyed, hidden, moved, another look) is not known to a copy made from the file: no copy.
		if (EntityChanged && EntityChanged(Entity.Id))
		{
			++Stats.FarEntitiesChanged;
			continue;
		}
		// What the asset set is asked for: the twin's meshes and materials, without its instances.
		FMantleEntity& Need = Set.Needs.AddDefaulted_GetRef();
		Need.Id = Twin.Id;
		Need.Type = Twin.Type;
		const FMantleStaticMeshProps* MeshProps = MantleMeshPropsOf(Twin);
		if (Twin.Type == EMantleEntityType::InstancedMesh)
		{
			FMantleInstancedMeshProps Instanced;
			Instanced.Mesh = *MeshProps;
			Need.Props.Emplace<FMantleInstancedMeshProps>(MoveTemp(Instanced));
		}
		else
		{
			Need.Props.Emplace<FMantleStaticMeshProps>(*MeshProps);
		}
		FFarEntity& Far = Set.Entities.AddDefaulted_GetRef();
		Far.BaseId = Twin.Id;
		// A standalone game's picture streamer twins the solid things too (the solid streamer keeps them built, but each draws only to its own distance).
		Far.bRealByOther = Settings.Entities == EMantleCellEntities::PicturesOnly && HasCollision(Entity);
		Far.Twin = MoveTemp(Twin);
	}
	if (Set.Entities.IsEmpty())
	{
		return;
	}
	Set.Rank = RankFor(CellIndex);
	Set.bWanted = FMantleCellStreamPlan::IsWanted(Info, LastPath, Settings);
	FarSets.Add(CellIndex, MoveTemp(Set));
	++Stats.FarSetsMade;
	bRefreshNeeded = true;
}

void FMantleCellStreamer::CollectFarWants(const FMantleCellStreamPlan::FPath& Path, const TArray<int32>& RealWants, TArray<int32>& OutFarWants)
{
	using namespace MantleCellStreamerFarPrivate;
	OutFarWants.Reset();
	if (!FarEnabled())
	{
		FarLoadingCells.Reset();
		return;
	}
	const FMantleCellIndex& Index = Loader->GetIndex();
	const TSet<int32> Real(RealWants);
	// A far-only cell the real flow now wants is the real flow's to read: it makes the twin from the same file.
	FarLoadingCells.RemoveAll([&Real](const int32 CellIndex) { return Real.Contains(CellIndex); });

	TArray<int32> Candidates;
	FMantleCellStreamPlan::CellsNear(Index, LevelOverhangs, FarLevelRadii, Path, Candidates);
	struct FFarWant
	{
		int32 CellIndex;
		FMantleCellStreamPlan::FRank Rank;
	};
	TArray<FFarWant> Pool;
	for (const int32 CellIndex : Candidates)
	{
		if (Cells.Contains(CellIndex) || FarSets.Contains(CellIndex) || Real.Contains(CellIndex) || Loader->IsCellFailed(CellIndex))
		{
			continue;
		}
		const FMantleCellInfo& Info = Index.Cells[CellIndex];
		// Only cells past their own drawDistance are the far set's; inside it the cell is the real flow's (and if it has no room yet, it waits there).
		if (!FMantleFarSetPlan::IsWanted(Info, Path, Settings.FarSets) || FMantleCellStreamPlan::IsWanted(Info, Path, Settings))
		{
			continue;
		}
		Pool.Add(FFarWant{ CellIndex, RankFor(CellIndex) });
	}
	Pool.Sort([](const FFarWant& A, const FFarWant& B) { return FMantleCellStreamPlan::BuildsBefore(A.Rank, B.Rank); });
	const int32 Room = FMath::Max(MaxFarCellsInFlight - FarAssetCells.Num() - FarReadyCells.Num(), 0);
	for (int32 At = 0; At < Pool.Num() && At < Room; ++At)
	{
		OutFarWants.Add(Pool[At].CellIndex);
	}
	FarLoadingCells = OutFarWants;
}

void FMantleCellStreamer::RefreshFar(const FMantleCellStreamPlan::FPath& Path)
{
	using namespace MantleCellStreamerFarPrivate;
	LastPath = Path;
	if (!FarEnabled() || FarSets.IsEmpty())
	{
		return;
	}
	// The twins are asked about only when something that could change an answer has changed (a serial the script world keeps), and the look is timed.
	bool bScan = EntityChanged && !EntityChangeSerial;
	if (EntityChanged && EntityChangeSerial)
	{
		const uint64 Serial = EntityChangeSerial();
		if (!bChangeSerialKnown || Serial != LastChangeSerial)
		{
			bChangeSerialKnown = true;
			LastChangeSerial = Serial;
			bScan = true;
		}
	}
	if (bScan)
	{
		const double ScanStart = FPlatformTime::Seconds();
		FMantleCellPartClock ScanClock(TickReport, FMantleCellTickReport::FarScan);
		DropChangedTwins();
		const double ScanSeconds = FPlatformTime::Seconds() - ScanStart;
		++Stats.FarChangeScans;
		Stats.FarChangeScanSeconds += ScanSeconds;
		Stats.WorstFarChangeScanSeconds = FMath::Max(Stats.WorstFarChangeScanSeconds, ScanSeconds);
	}
	const FMantleCellIndex& Index = Loader->GetIndex();
	TArray<int32> Release;
	for (TPair<int32, FFarSet>& Pair : FarSets)
	{
		const int32 CellIndex = Pair.Key;
		FFarSet& Set = Pair.Value;
		const FMantleCellInfo& Info = Index.Cells[CellIndex];
		if (Set.bDestroying)
		{
			continue;
		}
		const bool bWanted = FMantleCellStreamPlan::IsWanted(Info, Path, Settings);
		if (Set.bWanted != bWanted)
		{
			Set.bWanted = bWanted;
			if (Set.State == EFarState::Built || Set.Cursor > 0)
			{
				FMantleCellPartClock VisibilityClock(TickReport, FMantleCellTickReport::FarVisibility);
				ApplyFarVisibility(CellIndex, Set, bWanted ? TEXT("cell wanted") : TEXT("cell not wanted"));
			}
		}
		if (!Cells.Contains(CellIndex) && FMantleFarSetPlan::IsReleased(Info, Path, Settings.FarSets))
		{
			Release.Add(CellIndex);
			continue;
		}
		if (Set.State == EFarState::Assets || Set.State == EFarState::Ready)
		{
			Set.Rank = RankFor(CellIndex);
		}
		// Not during a hold: the join and a teleport wait for the real cells, and the assets of twins would queue behind theirs (the next plan after the hold asks).
		if (Set.State == EFarState::Idle && !bHolding && FMantleFarSetPlan::NeedsBuilding(Info, Path, Settings.FarSets))
		{
			Set.Rank = RankFor(CellIndex);
			if (CellAssets.IsValid())
			{
				// Asking for the assets of one set costs milliseconds (a join finds 140 sets at once: 611 ms in one refresh, City Sample 2026-11-23): the sets wait here and ask a few a tick, nearest first.
				if (!Set.bAcquireQueued)
				{
					Set.bAcquireQueued = true;
					FarAcquireCells.Add(CellIndex);
				}
			}
			else
			{
				Set.State = EFarState::Ready;
				FarReadyCells.Add(CellIndex);
			}
		}
	}
	FMantleCellPartClock ReleaseClock(TickReport, FMantleCellTickReport::FarRelease);
	for (const int32 CellIndex : Release)
	{
		FFarSet& Set = FarSets.FindChecked(CellIndex);
		FarAssetCells.RemoveSingle(CellIndex);
		FarReadyCells.RemoveSingle(CellIndex);
		FarAcquireCells.RemoveSingle(CellIndex);
		Set.bAcquireQueued = false;
		bool bAnyBuilt = false;
		for (const FFarEntity& Far : Set.Entities)
		{
			bAnyBuilt = bAnyBuilt || Far.Actor.IsValid();
		}
		if (bAnyBuilt)
		{
			Set.bDestroying = true;
			Set.DestroyCursor = 0;
			FarRemovingCells.Add(CellIndex);
		}
		else if (Set.bAssetsHeld)
		{
			// Nothing of it is drawn (so it can double no draw and leave no gap), but giving its assets back drops their records, which costs a few milliseconds to tens a set: a ladder step
			// lets go of dozens at once (925 ms in one refresh, City Sample 2026-11-22). They wait in the removal queue and go a few a tick inside the work budget, like a twin that stands.
			Set.bDestroying = true;
			Set.DestroyCursor = Set.Entities.Num();
			FarRemovingCells.Add(CellIndex);
		}
		else
		{
			FarSets.Remove(CellIndex);
			++Stats.FarSetsReleased;
		}
	}
}

void FMantleCellStreamer::DropChangedTwins()
{
	if (!EntityChanged)
	{
		return;
	}
	for (TPair<int32, FFarSet>& Pair : FarSets)
	{
		if (Pair.Value.bDestroying)
		{
			continue;
		}
		for (FFarEntity& Far : Pair.Value.Entities)
		{
			if (Far.bDropped || !EntityChanged(Far.BaseId))
			{
				continue;
			}
			Far.bDropped = true;
			++Stats.FarEntitiesChanged;
			if (AActor* Actor = Far.Actor.Get())
			{
				Actor->Destroy();
			}
			Far.Actor.Reset();
			Far.bBuilt = false;
			Far.Twin = FMantleEntity();
			UE_LOG(LogMantle, Log, TEXT("MANTLE_FARSET_CHANGED cell=%s entity=%s: changed on this machine, its far copy is taken down"), *InfoOf(Pair.Key).KeyString(), *Far.BaseId);
		}
	}
}

void FMantleCellStreamer::TakeFinishedFarCells()
{
	if (FarLoadingCells.IsEmpty())
	{
		return;
	}
	for (const int32 CellIndex : TArray<int32>(FarLoadingCells))
	{
		if (!Loader->IsCellReady(CellIndex))
		{
			if (Loader->IsCellFailed(CellIndex))
			{
				// Never to be read: the far ring stands without it.
				FarLoadingCells.RemoveSingle(CellIndex);
			}
			continue;
		}
		FMantleCellContent Content;
		if (!Loader->TakeCell(CellIndex, Content))
		{
			continue;
		}
		FarLoadingCells.RemoveSingle(CellIndex);
		++TickReport.FarCellsTaken;
		TickReport.FarCellEntitiesTaken += Content.Entities.Num();
		MakeFarSet(CellIndex, Content.Entities);
		bRefreshNeeded = true;
	}
}

void FMantleCellStreamer::TakeFarAssetCells()
{
	using namespace MantleCellStreamerFarPrivate;
	if (FarAssetCells.IsEmpty() || !CellAssets.IsValid())
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	TickReport.FarAssetCellsWaiting += FarAssetCells.Num();
	for (const int32 CellIndex : TArray<int32>(FarAssetCells))
	{
		FFarSet* Set = FarSets.Find(CellIndex);
		if (!Set)
		{
			FarAssetCells.RemoveSingle(CellIndex);
			continue;
		}
		// As for a real cell (TakeAssetCells): one cheap look a tick while it waits, the lists once it has settled or stalled. Before, every waiting set's every
		// need was looked up each tick: 160 far sets took 125 ms a frame (City Sample, 2026-11-22).
		if (!CellAssets->IsSettled(FarHolderBase + CellIndex) && !FMantleCellStreamPlan::HasStalled(Now, Set->AssetsAskedAt, LastAssetProgressSeconds, Settings.AssetIdleTimeoutSeconds))
		{
			continue;
		}
		TArray<FString> Pending;
		TArray<FString> Failed;
		CellAssets->GetState(FarHolderBase + CellIndex, Pending, Failed);
		if (!Pending.IsEmpty())
		{
			// As for a real cell (TakeAssetCells): only downloads that have stood still give up.
			if (!FMantleCellStreamPlan::HasStalled(Now, Set->AssetsAskedAt, LastAssetProgressSeconds, Settings.AssetIdleTimeoutSeconds))
			{
				continue;
			}
			UE_LOG(LogMantle, Warning, TEXT("MANTLE_FARSET_ASSET_TIMEOUT cell=%s: %d asset(s) had not arrived after %.0f s, and the place's downloads had not moved for %.0f s; the twin is built without them"),
				*InfoOf(CellIndex).KeyString(), Pending.Num(), Now - Set->AssetsAskedAt, Now - FMath::Max(Set->AssetsAskedAt, LastAssetProgressSeconds));
			Failed.Append(Pending);
		}
		FarAssetCells.RemoveSingle(CellIndex);
		++TickReport.FarAssetCellsDone;
		if (!Failed.IsEmpty())
		{
			// The cell's real entities named the same assets and said so once (MANTLE_CELLS_ASSET_MISSING); the twins that use them are left out.
			const TSet<FString> Gone(Failed);
			const int32 Before = Set->Entities.Num();
			Set->Entities.RemoveAll([&Gone](const FFarEntity& Far)
			{
				const FMantleStaticMeshProps* Props = MantleMeshPropsOf(Far.Twin);
				return Props && Gone.Contains(Props->Mesh);
			});
			Stats.FarEntitiesLeftOut += Before - Set->Entities.Num();
			for (FFarEntity& Far : Set->Entities)
			{
				FMantleStaticMeshProps* Props = Far.Twin.Props.TryGet<FMantleStaticMeshProps>();
				if (!Props)
				{
					if (FMantleInstancedMeshProps* Instanced = Far.Twin.Props.TryGet<FMantleInstancedMeshProps>())
					{
						Props = &Instanced->Mesh;
					}
				}
				if (Props)
				{
					for (FString& Material : Props->Materials)
					{
						if (Gone.Contains(Material))
						{
							Material.Reset();
						}
					}
				}
			}
		}
		if (Set->Entities.IsEmpty())
		{
			ReleaseFarAssets(CellIndex, *Set);
			FarSets.Remove(CellIndex);
			continue;
		}
		Set->State = EFarState::Ready;
		Set->Rank = RankFor(CellIndex);
		FarReadyCells.Add(CellIndex);
	}
}

bool FMantleCellStreamer::BuildOneFar()
{
	using namespace MantleCellStreamerFarPrivate;
	int32 Best = INDEX_NONE;
	for (const int32 CellIndex : FarReadyCells)
	{
		if (Best == INDEX_NONE || FMantleCellStreamPlan::BuildsBefore(FarSets.FindChecked(CellIndex).Rank, FarSets.FindChecked(Best).Rank))
		{
			Best = CellIndex;
		}
	}
	UWorld* const LiveWorld = World.Get();
	if (Best == INDEX_NONE || !LiveWorld)
	{
		return false;
	}
	FFarSet& Set = FarSets.FindChecked(Best);
	if (!Set.Entities.IsValidIndex(Set.Cursor))
	{
		Set.State = EFarState::Built;
		FarReadyCells.RemoveSingle(Best);
		return true;
	}
	const double Start = FPlatformTime::Seconds();
	FFarEntity& Far = Set.Entities[Set.Cursor];
	const FMantleStaticMeshProps* MeshProps = MantleMeshPropsOf(Far.Twin);
	bool bBuild = !Far.bDropped;
	if (bBuild && EntityChanged && EntityChanged(Far.BaseId))
	{
		// Changed since the set was made: no copy.
		Far.bDropped = true;
		bBuild = false;
		++Stats.FarEntitiesChanged;
	}
	if (bBuild && MeshProps && Settings.FarSets.bRequireNanite && FMantlePlaceAssets::IsUploadedId(MeshProps->Mesh))
	{
		// An uploaded mesh without Nanite data (a machine that cannot run it, a file that was skipped) would draw plain, far away: it is left out.
		FString Error;
		const UStaticMesh* Mesh = FMantleAssetLibrary::Get().LoadMesh(MeshProps->Mesh, Error);
		if (!Mesh || !Mesh->HasValidNaniteData())
		{
			// A see-through material (road paint, decals) is why the converter publishes a mesh without Nanite data: counted apart, so the summary shows what is a hole and what is not.
			bool bSeeThrough = false;
			for (const FString& MaterialId : MeshProps->Materials)
			{
				FString MaterialError;
				const UMaterialInterface* Material = FMantlePlaceAssets::IsUploadedId(MaterialId) ? FMantleAssetLibrary::Get().LoadMaterial(MaterialId, MaterialError) : nullptr;
				if (Material && IsTranslucentBlendMode(*Material))
				{
					bSeeThrough = true;
					break;
				}
			}
			bBuild = false;
			++Stats.FarEntitiesLeftOut;
			++Stats.FarLeftOutNoNanite;
			if (FMantleFarSetPlan::ClassifyNaniteLeftOut(false, bSeeThrough) == FMantleFarSetPlan::ENaniteLeftOut::SeeThrough)
			{
				const FMantleInstancedMeshProps* Listed = Far.Twin.Props.TryGet<FMantleInstancedMeshProps>();
				++Stats.FarLeftOutSeeThrough;
				Stats.FarLeftOutSeeThroughInstances += Listed ? Listed->Instances.Num() : 1;
			}
		}
	}
	int64 InstanceCount = 0;
	if (const FMantleInstancedMeshProps* Instanced = Far.Twin.Props.TryGet<FMantleInstancedMeshProps>())
	{
		InstanceCount = Instanced->Instances.Num();
	}
	const FString EntityId = Far.Twin.Id;
	if (bBuild)
	{
		SlicePlace.Entities.Reset();
		SlicePlace.Entities.Add(MoveTemp(Far.Twin));
		Far.Twin = FMantleEntity();
		FMantleBuildResult Built = FMantlePlaceBuilder::Build(LiveWorld, SlicePlace, SliceOptions);
		SlicePlace.Entities.Reset();
		AActor* Actor = nullptr;
		if (AActor** Found = Built.Actors.Find(EntityId))
		{
			Actor = *Found;
		}
		if (IsValid(Actor))
		{
			Far.Actor = Actor;
			Far.bBuilt = true;
			++Stats.FarEntitiesBuilt;
			Stats.FarInstancesBuilt += InstanceCount;
			// Drawn unless the real entity stands in a wanted cell (then the real one draws).
			const bool bStanding = Far.bRealByOther || Set.Standing.Contains(Far.BaseId);
			SetShown(Actor, FMantleFarSetPlan::ShouldShow(Set.bWanted, bStanding));
		}
		for (const FMantleParseError& Error : Built.Errors)
		{
			if (Stats.BuildErrors < 20)
			{
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_CELLS_BUILD_ERROR cell=%s far entity=%s: %s"), *InfoOf(Best).KeyString(), *EntityId, *Error.ToString());
			}
			++Stats.BuildErrors;
		}
	}
	++Set.Cursor;
	if (Set.Cursor >= Set.Entities.Num())
	{
		Set.State = EFarState::Built;
		FarReadyCells.RemoveSingle(Best);
		++Stats.FarSetsBuiltEver;
	}
	const double Seconds = FPlatformTime::Seconds() - Start;
	Stats.FarBuildSeconds += Seconds;
	Stats.LongestFarUnitSeconds = FMath::Max(Stats.LongestFarUnitSeconds, Seconds);
	if (Seconds >= SlowFarUnitSeconds)
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_FARSET_SLOW_UNIT cell=%s entity=%s instances=%lld ms=%.1f"), *InfoOf(Best).KeyString(), *EntityId, InstanceCount, Seconds * 1000.0);
	}
	return true;
}

bool FMantleCellStreamer::AcquireFarSome(const double Deadline)
{
	using namespace MantleCellStreamerFarPrivate;
	// Not during a hold (the join and a teleport wait for the real cells; the plan after it fills the queue again if it was left over).
	if (FarAcquireCells.IsEmpty() || bHolding || !CellAssets.IsValid())
	{
		return false;
	}
	const FMantleCellIndex& Index = Loader->GetIndex();
	bool bAny = false;
	// At least one a tick, so a budget the builds spent never leaves the queue standing; then until the budget is spent.
	bool bDidOne = false;
	while (!FarAcquireCells.IsEmpty() && (!bDidOne || FPlatformTime::Seconds() < Deadline))
	{
		// The nearest first; a set that left the queue's world (released, being taken down, no longer needing its twin) is dropped.
		int32 BestAt = INDEX_NONE;
		for (int32 At = FarAcquireCells.Num() - 1; At >= 0; --At)
		{
			const int32 CellIndex = FarAcquireCells[At];
			FFarSet* Set = FarSets.Find(CellIndex);
			if (!Set || Set->bDestroying || Set->State != EFarState::Idle || !FMantleFarSetPlan::NeedsBuilding(Index.Cells[CellIndex], LastPath, Settings.FarSets))
			{
				if (Set)
				{
					Set->bAcquireQueued = false;
				}
				FarAcquireCells.RemoveAt(At);
				if (BestAt != INDEX_NONE && BestAt > At)
				{
					--BestAt;
				}
				continue;
			}
			if (BestAt == INDEX_NONE || FMantleCellStreamPlan::BuildsBefore(Set->Rank, FarSets.FindChecked(FarAcquireCells[BestAt]).Rank))
			{
				BestAt = At;
			}
		}
		if (BestAt == INDEX_NONE)
		{
			break;
		}
		const int32 CellIndex = FarAcquireCells[BestAt];
		FarAcquireCells.RemoveAt(BestAt);
		FFarSet& Set = FarSets.FindChecked(CellIndex);
		Set.bAcquireQueued = false;
		{
			FMantleCellPartClock AcquireClock(TickReport, FMantleCellTickReport::FarAcquire);
			CellAssets->Acquire(FarHolderBase + CellIndex, Set.Needs);
		}
		Set.bAssetsHeld = true;
		Set.State = EFarState::Assets;
		Set.AssetsAskedAt = FPlatformTime::Seconds();
		FarAssetCells.Add(CellIndex);
		bDidOne = true;
		bAny = true;
	}
	return bAny;
}

bool FMantleCellStreamer::RemoveFarSome(const double Deadline)
{
	using namespace MantleCellStreamerFarPrivate;
	if (FarRemovingCells.IsEmpty())
	{
		return false;
	}
	const double Start = FPlatformTime::Seconds();
	bool bAny = false;
	// At least one step a tick, so a tick whose budget the builds spent never leaves the queue standing; then steps until the budget is spent (a set's release is one step).
	bool bDidOne = false;
	while (!FarRemovingCells.IsEmpty() && (!bDidOne || FPlatformTime::Seconds() < Deadline))
	{
		bDidOne = true;
		const int32 CellIndex = FarRemovingCells[0];
		FFarSet* Set = FarSets.Find(CellIndex);
		if (!Set)
		{
			FarRemovingCells.RemoveAt(0);
			continue;
		}
		for (int32 Count = 0; Count < 8 && Set->DestroyCursor < Set->Entities.Num(); ++Count, ++Set->DestroyCursor)
		{
			if (AActor* Actor = Set->Entities[Set->DestroyCursor].Actor.Get())
			{
				FMantleCellPartClock Clock(TickReport, FMantleCellTickReport::FarRemoveDestroy);
				Actor->Destroy();
				++TickReport.TwinsRemoved;
			}
			bAny = true;
		}
		if (Set->DestroyCursor >= Set->Entities.Num())
		{
			FarRemovingCells.RemoveAt(0);
			{
				FMantleCellPartClock Clock(TickReport, FMantleCellTickReport::FarRemoveAssets);
				ReleaseFarAssets(CellIndex, *Set);
			}
			FarSets.Remove(CellIndex);
			++Stats.FarSetsReleased;
		}
	}
	Stats.FarRemoveSeconds += FPlatformTime::Seconds() - Start;
	return bAny;
}

void FMantleCellStreamer::ReleaseFarAssets(const int32 CellIndex, FFarSet& Set)
{
	if (Set.bAssetsHeld && CellAssets.IsValid())
	{
		CellAssets->Release(MantleCellStreamerFarPrivate::FarHolderBase + CellIndex);
	}
	Set.bAssetsHeld = false;
}

void FMantleCellStreamer::ApplyFarVisibility(const int32 CellIndex, FFarSet& Set, const TCHAR* Why)
{
	using namespace MantleCellStreamerFarPrivate;
	const double Start = FPlatformTime::Seconds();
	int32 NowShown = 0;
	int32 NowHidden = 0;
	for (FFarEntity& Far : Set.Entities)
	{
		if (!Far.bBuilt)
		{
			continue;
		}
		const bool bStanding = Far.bRealByOther || Set.Standing.Contains(Far.BaseId);
		const bool bShow = FMantleFarSetPlan::ShouldShow(Set.bWanted, bStanding);
		if (SetShown(Far.Actor.Get(), bShow))
		{
			(bShow ? NowShown : NowHidden) += 1;
		}
	}
	if (NowShown == 0 && NowHidden == 0)
	{
		return;
	}
	const double Seconds = FPlatformTime::Seconds() - Start;
	Stats.FarToggleSeconds += Seconds;
	Stats.WorstFarToggleSeconds = FMath::Max(Stats.WorstFarToggleSeconds, Seconds);
	Stats.FarBatchesShown += NowShown > 0 ? 1 : 0;
	Stats.FarBatchesHidden += NowHidden > 0 ? 1 : 0;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_FARSET_SWAP cell=%s why=%s wanted=%d to_far=%d to_real=%d frame=%llu ms=%.2f"),
		*InfoOf(CellIndex).KeyString(), Why, Set.bWanted ? 1 : 0, NowShown, NowHidden, static_cast<uint64>(GFrameCounter), Seconds * 1000.0);
}

void FMantleCellStreamer::OnRealEntityStanding(const int32 CellIndex, const FString& BaseId)
{
	using namespace MantleCellStreamerFarPrivate;
	FFarSet* Set = FarSets.Find(CellIndex);
	if (!Set)
	{
		return;
	}
	Set->Standing.Add(BaseId);
	for (FFarEntity& Far : Set->Entities)
	{
		if (!Far.bBuilt || Far.BaseId != BaseId)
		{
			continue;
		}
		const double Start = FPlatformTime::Seconds();
		if (SetShown(Far.Actor.Get(), FMantleFarSetPlan::ShouldShow(Set->bWanted, true)))
		{
			const double Seconds = FPlatformTime::Seconds() - Start;
			++Stats.FarHandOffs;
			Stats.FarToggleSeconds += Seconds;
			Stats.WorstFarToggleSeconds = FMath::Max(Stats.WorstFarToggleSeconds, Seconds);
			UE_LOG(LogMantle, Log, TEXT("MANTLE_FARSET_HANDOFF cell=%s entity=%s wanted=%d frame=%llu ms=%.3f"), *InfoOf(CellIndex).KeyString(), *BaseId, Set->bWanted ? 1 : 0, static_cast<uint64>(GFrameCounter), Seconds * 1000.0);
		}
	}
}

void FMantleCellStreamer::OnRealCellLeaving(const int32 CellIndex)
{
	FFarSet* Set = FarSets.Find(CellIndex);
	if (!Set)
	{
		return;
	}
	Set->Standing.Reset();
	ApplyFarVisibility(CellIndex, *Set, TEXT("real cell leaving"));
}

FMantleFarAudit FMantleCellStreamer::AuditFarSets(const TCHAR* Why)
{
	using namespace MantleCellStreamerFarPrivate;
	FMantleFarAudit Audit;
	for (const TPair<int32, FFarSet>& Pair : FarSets)
	{
		const FFarSet& Set = Pair.Value;
		if (Set.bDestroying)
		{
			continue;
		}
		++Audit.Sets;
		for (const FFarEntity& Far : Set.Entities)
		{
			if (!Far.bBuilt || !Far.Actor.IsValid())
			{
				continue;
			}
			++Audit.Entities;
			const bool bStanding = Far.bRealByOther || Set.Standing.Contains(Far.BaseId);
			const bool bShouldShow = FMantleFarSetPlan::ShouldShow(Set.bWanted, bStanding);
			const bool bShown = IsShown(Far.Actor.Get());
			Audit.Shown += bShown ? 1 : 0;
			if (bShown && !bShouldShow)
			{
				++Audit.DoubleDraws;
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_FARSET_AUDIT_FAIL double draw: cell=%s entity=%s is drawn while the cell is wanted and the real entity stands (%s)"), *InfoOf(Pair.Key).KeyString(), *Far.BaseId, Why);
			}
			else if (!bShown && bShouldShow)
			{
				++Audit.Gaps;
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_FARSET_AUDIT_FAIL gap: cell=%s entity=%s is not drawn and nothing else draws it (%s)"), *InfoOf(Pair.Key).KeyString(), *Far.BaseId, Why);
			}
		}
	}
	++Stats.FarAuditRuns;
	Stats.FarAuditDoubleDraws += Audit.DoubleDraws;
	Stats.FarAuditGaps += Audit.Gaps;
	return Audit;
}

FMantleCellStreamer::FFarSetView FMantleCellStreamer::GetFarSetView(const int32 CellIndex) const
{
	FFarSetView Result;
	const FFarSet* Set = FarSets.Find(CellIndex);
	if (!Set)
	{
		return Result;
	}
	Result.bExists = true;
	Result.Entities = Set->Entities.Num();
	Result.bWanted = Set->bWanted;
	Result.State = static_cast<int32>(Set->State);
	for (const FFarEntity& Far : Set->Entities)
	{
		Result.Built += Far.bBuilt ? 1 : 0;
		Result.Shown += (Far.bBuilt && MantleCellStreamerFarPrivate::IsShown(Far.Actor.Get())) ? 1 : 0;
	}
	return Result;
}

void FMantleCellStreamer::LogFarSummary(const TCHAR* Why) const
{
	if (!FarEnabled() && Stats.FarSetsMade == 0)
	{
		return;
	}
	UE_LOG(LogMantle, Log,
		TEXT("MANTLE_FARSET_SUMMARY %s: sets made %lld built %lld released %lld standing %d, entities built %lld (instances %lld), left out %lld (no Nanite %lld, of which see-through %lld with %lld instances, changed here %lld), hand-offs %lld, batches to far %lld to real %lld, build %.2f s remove %.2f s toggle %.3f s (worst %.2f ms), longest twin %.2f ms, change scans %lld (%.3f s, worst %.2f ms), audits %lld double draws %lld gaps %lld"),
		Why, Stats.FarSetsMade, Stats.FarSetsBuiltEver, Stats.FarSetsReleased, Stats.FarSetsStanding, Stats.FarEntitiesBuilt, Stats.FarInstancesBuilt, Stats.FarEntitiesLeftOut, Stats.FarLeftOutNoNanite, Stats.FarLeftOutSeeThrough, Stats.FarLeftOutSeeThroughInstances, Stats.FarEntitiesChanged,
		Stats.FarHandOffs, Stats.FarBatchesShown, Stats.FarBatchesHidden, Stats.FarBuildSeconds, Stats.FarRemoveSeconds, Stats.FarToggleSeconds, Stats.WorstFarToggleSeconds * 1000.0,
		Stats.LongestFarUnitSeconds * 1000.0, Stats.FarChangeScans, Stats.FarChangeScanSeconds, Stats.WorstFarChangeScanSeconds * 1000.0, Stats.FarAuditRuns, Stats.FarAuditDoubleDraws, Stats.FarAuditGaps);
}
