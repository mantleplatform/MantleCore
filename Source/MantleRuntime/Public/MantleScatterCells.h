// Copyright Joshua Eger. All Rights Reserved.
// mantleplatform/mantle#189: a Scatter of more than FMantleScatter::WholeInstances, built in cells near the camera, as terrain grass is
// (MantleTerrainGrass.h, #164). The Scatter's candidates are filed once in a position index (FMantleScatterCellIndex, built on a worker
// thread: every candidate's place is a pure function of its number); around the local view, each cell within the draw distance is read on
// a worker thread (FMantleScatter::GenerateCell: the whole stream's instances inside the cell, bit for bit, the minimum spacing exact across
// the cell's border) and the game thread gives each finished cell one instanced static mesh component per mesh, a few thousand instances
// at a time within a per-frame budget. Cells that leave range are released, with a cell's width of hysteresis, so walking along an edge
// does not rebuild them. The live-instance cap, the cells in flight and waiting and the per-frame budget are one budget shared by every
// cell Scatter in the world.
//
// Only a machine that draws, in a game world: a dedicated server builds no cells (its script queries read cells on demand, without
// components) and the editor preview builds the whole Scatter, as it always has.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MantleScatter.h"
#include "MantleTerrainGrass.h"

#include "MantleScatterCells.generated.h"

class UInstancedStaticMeshComponent;

/**
 * A cell Scatter's instances on its actor, near the local view. Added by AMantleScatterVolume::RefreshScatter on a machine that draws, never in
 * an editor world and never for a colliding Scatter (that one is built whole on the server and every client, under its own, far lower cap).
 *
 * Settings > Foliage scales it as it does a whole Scatter: the tier keeps a prefix of each candidate stream (Options.MaxCandidates) and a share
 * of the draw distance. Console variables: mantle.Scatter.MaxLiveInstances (the most standing at once, over every cell Scatter),
 * mantle.Scatter.MaxInstancesPerFrame (the most added to components in one frame). Log marker: MANTLE_SCATTER_CELLS, with the cells built,
 * the instances standing and the milliseconds a frame spent building.
 */
UCLASS(ClassGroup = Mantle, NotBlueprintable, Transient)
class MANTLERUNTIME_API UMantleScatterCellsComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UMantleScatterCellsComponent();

	/** The share of the draw distance at which an instance starts to fade out: the last fifth, as a whole Scatter's. */
	static constexpr double FadeStartShare = 0.8;

	/**
	 * Starts building the Scatter: Params (counted, above WholeInstances) with Options (the ground and the Foliage tier's candidate prefix), the
	 * mesh each mesh entry draws with (Assets[i] for Params.Meshes[i]; a null mesh leaves that mesh's instances out), and the distance from the
	 * view, cm, in which cells stand (the draw distance after the Foliage tier). Begins the index build at once.
	 */
	void Init(const FMantleScatterParams& InParams, const FMantleScatterGenerateOptions& InOptions, TArray<FMantleGrassEntryAssets> InAssets, double InReachCm);

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;

	/** Cells and instances standing now, and cells asked for that are not yet standing. */
	int32 GetLiveCells() const { return LiveCells; }
	int64 GetLiveInstances() const { return LiveInstances; }
	int32 GetPendingCells() const { return Wanted.Num() + InFlight + Ready.Num(); }

	/** Whether the position index is built (cells are asked for only after). */
	bool IsIndexReady() const { return Index.IsValid(); }

	/** Everything the index and the cells were built from has been read: the index is there and nothing is wanted, in flight or waiting. */
	bool IsSettled() const { return Index.IsValid() && Wanted.IsEmpty() && InFlight == 0 && Ready.IsEmpty(); }

	/** The view, in scatter space, that cells are chosen around; tests set it, a game takes the local player's. */
	void SetViewOverride(const TOptional<FVector>& InView) { ViewOverride = InView; }

	/** Instances currently standing across every cell Scatter in the world (the shared budget). */
	static int64 GetWorldLiveInstances();

private:
	struct FShared;

	enum class ECellState : uint8
	{
		/** A worker is reading it. */
		Generating,
		/** Its components stand (or it has no instance, and has none). */
		Live,
		/** Read, but the live-instance cap had no room; asked for again when there is. */
		Skipped
	};

	struct FCell
	{
		ECellState State = ECellState::Generating;
		TArray<TWeakObjectPtr<UInstancedStaticMeshComponent>> Components;
		int32 Instances = 0;
		FIntPoint Coord = FIntPoint::ZeroValue;
	};

	struct FWanted
	{
		FIntPoint Coord = FIntPoint::ZeroValue;
		double DistanceSquared = 0.0;
	};

	/** One finished cell, handed from a worker to the game thread. */
	struct FBuiltCell
	{
		FIntPoint Coord = FIntPoint::ZeroValue;
		int32 Generation = 0;
		double GenerateMs = 0.0;
		TArray<FMantleScatterInstance> Instances;
	};

	static uint64 CellKey(const FIntPoint& Coord);

	/** The local player's view location in scatter space (cm). */
	bool GetViewLocal(FVector& OutLocal) const;

	void Scan(const FVector& View);
	void StartGenerating(const FWanted& Want);
	void Apply(FBuiltCell&& Built);
	void ReleaseCell(uint64 Key);
	void ReleaseAll();
	void LogSummary(bool bStarted);

	FMantleScatterParams Params;
	FMantleScatterGenerateOptions Options;

	UPROPERTY(Transient)
	TArray<FMantleGrassEntryAssets> Assets;

	double ReachCm = 0.0;

	/** What a worker shares: the inputs it reads, the results it hands back, and whether the component is gone. */
	TSharedPtr<FShared, ESPMode::ThreadSafe> Shared;

	TSharedPtr<const FMantleScatterCellIndex> Index;
	FString IndexError;

	TMap<uint64, FCell> Cells;
	TArray<FWanted> Wanted;
	TArray<FBuiltCell> Ready;

	int32 InFlight = 0;
	int32 LiveCells = 0;
	int64 LiveInstances = 0;

	/** Bumped when everything is released, so a cell still being read for the old state is dropped when it returns. */
	int32 Generation = 0;

	TOptional<FVector> ViewOverride;

	double NextScanSeconds = 0.0;
	double NextLogSeconds = 0.0;
	double FirstScanSeconds = 0.0;
	bool bStartLogged = false;

	/** Counts and times since the last log line. */
	int32 BuiltSinceLog = 0;
	int32 ReleasedSinceLog = 0;
	int32 SkippedSinceLog = 0;
	double GenerateMsSinceLog = 0.0;
	double ApplyMsSinceLog = 0.0;
	double ApplyMsMaxFrame = 0.0;
	int32 ApplyFramesSinceLog = 0;
};
