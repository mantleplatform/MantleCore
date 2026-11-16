// Copyright Joshua Eger. All Rights Reserved.
// A streaming player's machine and a Terrain (stage 3 of large places, the far ground).
//
// Such a machine builds the terrain's lowest level of detail for every chunk at the join (FMantleTerrainBuildParams::bFarOnly): cheap, no collision, no shadow, drawn
// everywhere. This component then builds a chunk in full (every LOD, cooked collision, shadows: what a terrain is everywhere today) while a view is near it and swaps the
// two chunk by chunk: the full one is registered and the far one unregistered in the same game-thread step, and the other way round when the view leaves, so a chunk is
// never drawn twice and never missing. A server keeps every chunk in full (it never has this component), and so does a standalone game.

#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Components/ActorComponent.h"
#include "Engine/StaticMesh.h"
#include "MantleHeightfieldReader.h"
#include "MantleTerrainBuilder.h"

#include "MantleTerrainStream.generated.h"

class UMaterialInterface;
class UStaticMeshComponent;
class UWorld;

/** A terrain's chunk grid and where each chunk is in the terrain's own space (cm). Pure. */
struct MANTLERUNTIME_API FMantleTerrainChunkGrid
{
	int32 ChunksX = 0;
	int32 ChunksY = 0;
	int32 ChunkQuads = 128;
	int32 Width = 0;
	int32 Height = 0;
	float SpacingX = 0.0f;
	float SpacingY = 0.0f;

	static FMantleTerrainChunkGrid Make(const FMantleHeightfield& Field, int32 ChunkQuads);

	int32 Num() const { return ChunksX * ChunksY; }
	int32 IndexOf(const int32 X, const int32 Y) const { return Y * ChunksX + X; }
	FIntPoint CoordOf(const int32 Index) const { return FIntPoint(Index % FMath::Max(ChunksX, 1), Index / FMath::Max(ChunksX, 1)); }

	/** The chunk's rectangle: its first and last sample, times the spacing. */
	void RectOf(int32 Index, FVector2D& OutMin, FVector2D& OutMax) const;

	/** How far Point is from the chunk's rectangle, cm; 0 inside it. */
	double DistanceTo(int32 Index, const FVector2D& Point) const;

	/** The shorter side of a whole chunk, cm. */
	double ChunkSizeCm() const { return static_cast<double>(ChunkQuads) * FMath::Min(SpacingX, SpacingY); }
};

/** What a chunk is doing. Far: only its lowest level of detail stands. Building: its full version is being made. Near: the full version stands (and the far one is off). */
enum class EMantleTerrainChunkState : uint8
{
	Far,
	Building,
	Near,
	/** The full version stands and is on its way out: it still draws and still collides, but it does not count as standing (a body above it stops simulating first). */
	Releasing,
	/** Nothing to show (a chunk lost to holes). */
	Empty,
	/** The full version could not be made; the far one stays. */
	Failed,
};

/** Which chunks are built in full: pure, no world. */
struct MANTLERUNTIME_API FMantleTerrainStreamPlan
{
	/** Full chunks stand this far from a view: the largest draw distance of the place's cell levels, kept within these. */
	static constexpr double MinNearRadiusCm = 25600.0;
	static constexpr double MaxNearRadiusCm = 60000.0;
	static double NearRadiusFor(double TopLevelDrawDistanceCm);

	/** The ground that must stand under a join or a teleport before the player is let go: this far from the view (or the near radius, when smaller). */
	static constexpr double HoldRadiusCm = 15000.0;

	/** A chunk is wanted in full when it is within RadiusCm of a view, or (when it is full already) within RadiusCm + MarginCm: the margin keeps a chunk at the edge from flapping. */
	static bool Wants(double DistanceCm, bool bNearNow, double RadiusCm, double MarginCm);

	/** The smallest distance from any of Points (terrain space) to the chunk; a very large number when there are no points. */
	static double NearestDistance(const FMantleTerrainChunkGrid& Grid, int32 Index, TConstArrayView<FVector2D> Points);

	/**
	 * The plan. OutBuild: the chunks that are Far and wanted, nearest first (ties by index). OutRelease: the chunks that are Near and no longer wanted (with the margin).
	 * Chunks that are Building, Releasing, Empty or Failed are left alone.
	 */
	static void Make(const FMantleTerrainChunkGrid& Grid, TConstArrayView<FVector2D> Points, double RadiusCm, double MarginCm,
		TConstArrayView<EMantleTerrainChunkState> States, TArray<int32>& OutBuild, TArray<int32>& OutRelease);
};

/** The terrain hold's state between ticks (see FMantleTerrainHold). */
struct FMantleTerrainHoldState
{
	bool bHolding = false;
	/** The hold timed out: no new hold starts until the ground stands once. */
	bool bGaveUp = false;
};

/**
 * The decision of the player's hold on the ground under the view (UMantleCellStreamSubsystem::TickTerrainHold), pure. A hold begins when the ground under the view is not built
 * (the join, a teleport, anything that outruns the builds), ends when it stands, and gives up after TimeoutSeconds so a chunk that cannot be built never freezes the player; after
 * giving up no hold begins again until the ground has stood once.
 */
struct MANTLERUNTIME_API FMantleTerrainHold
{
	static constexpr double TimeoutSeconds = 60.0;

	enum class EChange : uint8 { None, Begin, End, GiveUp };

	/** Moves State on by one tick: bGroundStands is the answer for the view now, HeldSeconds how long the hold has lasted (0 when none does). */
	static EChange Next(FMantleTerrainHoldState& State, bool bGroundStands, double HeldSeconds);
};

/** What the far ground and its swaps have done so far, for the logs and the measurements. */
struct FMantleTerrainStreamStats
{
	int32 Chunks = 0;
	int32 NearNow = 0;
	int32 BuildingNow = 0;
	int32 FailedNow = 0;
	int64 SwapsToNear = 0;
	int64 SwapsToFar = 0;
	int64 Discarded = 0;
	int64 BuildsStarted = 0;
	int64 AuditsRun = 0;
	int64 AuditFailures = 0;
	/** Game-thread seconds: making a mesh (FinishChunk), swapping a chunk in, and the longest of each. Worker seconds: preparing and cooking, measured from start to ready. */
	double FinishSeconds = 0.0;
	double WorstFinishSeconds = 0.0;
	double SwapInSeconds = 0.0;
	double WorstSwapInSeconds = 0.0;
	double SwapOutSeconds = 0.0;
	double WorstSwapOutSeconds = 0.0;
	double LongestBuildSeconds = 0.0;
};

/** Every chunk of a terrain counted by what draws it. */
struct FMantleTerrainAudit
{
	int32 Chunks = 0;
	/** Chunks with something to show, and how many are drawn by their full version, by the far one, by both (a double draw) and by neither (a gap). */
	int32 WithMesh = 0;
	int32 DrawnFull = 0;
	int32 DrawnFar = 0;
	int32 DrawnBoth = 0;
	int32 DrawnNeither = 0;
	bool IsClean() const { return DrawnBoth == 0 && DrawnNeither == 0; }
};

/**
 * On the terrain's actor, on a client of a streaming place. Created by the place builder after a far-only spawn (FMantleTerrainSpawn). Each tick it asks ViewsResolver
 * where the views are; with none (the world is not streaming a place) it does nothing.
 */
UCLASS(ClassGroup = (Mantle))
class MANTLERUNTIME_API UMantleTerrainStreamComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UMantleTerrainStreamComponent();

	/** Chunks that may be in the making at once, and what the game thread may do in one step: one mesh made, this many swaps, this many releases. */
	static constexpr int32 MaxBuilding = 4;
	static constexpr int32 MaxSwapsPerStep = 2;
	static constexpr int32 MaxReleasesPerStep = 4;

	/**
	 * Where the views are (world space) and how far full chunks stand from them, for the world this component is in; false when that world streams nothing yet. Set by the module
	 * that owns the cell streamer (UMantleCellStreamSubsystem), like FMantleCellArea::Resolver.
	 */
	using FViewsResolver = TFunction<bool(UWorld* World, TArray<FVector>& OutViews, double& OutNearRadiusCm)>;
	static FViewsResolver ViewsResolver;

	/** Every terrain stream of a world, in the order they started. */
	static void ForEach(const UWorld* World, TFunctionRef<void(UMantleTerrainStreamComponent&)> Visit);

	/**
	 * Gives the component what the far-only spawn made. Field is the terrain's heightfield (kept); NearParams is what a full chunk is built with (collision on, LODs on, the hole
	 * grid, Outer the actor). FarComponents is by chunk index, null where a chunk has no far mesh. Every chunk starts Far.
	 */
	void Init(TSharedPtr<const FMantleHeightfield> InField, const FMantleTerrainBuildParams& NearParams, UMaterialInterface* InMaterial, bool bInStatic, bool bInCastShadow,
		TArray<TWeakObjectPtr<UStaticMeshComponent>>&& FarComponents);

	/**
	 * One step of work for these views (world space): plan, start chunks, take what has finished (one mesh made, a few swapped in), release what is out of range. The tick calls it with
	 * what ViewsResolver says; a test calls it by hand. NearRadiusCm of 0 builds nothing in full.
	 */
	void StepStream(const TArray<FVector>& WorldViews, double NearRadiusCm);

	/** Every chunk within RadiusCm of every one of Points (world space) is Near, Empty or Failed. False until the first step has run. */
	bool AreaStands(const TArray<FVector>& WorldPoints, double RadiusCm) const;

	/**
	 * What the chunks within RadiusCm of a world point are doing, for the collision gate (see FMantleCellAreaState): how many chunks with something to stand on, how many stand
	 * (Near), how many can never (Failed), how many are wanted and not there yet. Chunks that are Releasing do not stand.
	 */
	void CountArea(const FVector& WorldPoint, double RadiusCm, int32& OutCells, int32& OutBuilt, int32& OutFailed, int32& OutPending) const;

	/** Changes whenever the answer of CountArea can: a chunk stands, is released, fails. */
	uint64 GetAreaSerial() const { return AreaSerial; }

	/** Changes only when ground can go: a chunk was handed to its release, or failed. */
	uint64 GetTakeDownSerial() const { return TakeDownSerial + static_cast<uint64>(Stats.FailedNow); }

	/** Counts every chunk by what draws it (registered components). Logs and counts a failure. */
	FMantleTerrainAudit Audit();

	const FMantleTerrainStreamStats& GetStats() const { return Stats; }
	const FMantleTerrainChunkGrid& GetGrid() const { return Grid; }
	EMantleTerrainChunkState GetState(const int32 Index) const { return Chunks.IsValidIndex(Index) ? Chunks[Index].State : EMantleTerrainChunkState::Empty; }
	bool IsConfigured() const { return bConfigured; }

	/** Chunks whose collision is being cooked off the game thread now. */
	int32 GetCookingCount() const { return CookingMeshes.Num(); }

	/** One MANTLE_TERRAIN_STREAM_SUMMARY line. */
	void LogSummary(const TCHAR* Why) const;

	//~ Begin UActorComponent
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
	virtual void BeginDestroy() override;
	//~ End UActorComponent

private:
	/** One full chunk being made: prepared on a worker, made into a mesh here, cooked on a worker, then swapped in. */
	struct FJob
	{
		enum class EPhase : uint8 { Preparing, Cooking };
		EPhase Phase = EPhase::Preparing;
		TFuture<TUniquePtr<FMantleTerrainPreparedChunk>> Prepared;
		/** The mesh made for it: held by CookingMeshes (not rooted) while its collision is cooked. */
		FMantleTerrainChunk Built;
		bool bCookDone = false;
		bool bCookOk = false;
		double StartedSeconds = 0.0;
	};

	struct FChunk
	{
		EMantleTerrainChunkState State = EMantleTerrainChunkState::Far;
		bool bHasFarMesh = false;
		/** Wanted by the last plan (inside the radius or, for a chunk that is Near, the margin). */
		bool bWanted = false;
		TWeakObjectPtr<UStaticMeshComponent> FarComponent;
		TWeakObjectPtr<UStaticMeshComponent> NearComponent;
		TSharedPtr<FJob> Job;
		/** Releasing: the frame it may be taken down at (one whole frame after it stopped counting as standing). */
		uint64 ReleaseAtFrame = 0;
	};

	void WorldToTerrain(const TArray<FVector>& WorldPoints, TArray<FVector2D>& OutLocal) const;
	void StartBuild(int32 Index);
	/** Takes the prepared description and makes its mesh (game thread), then starts the cook. Returns false when the chunk is gone from the plan. */
	void FinishPrepared(int32 Index);
	void SwapIn(int32 Index);
	void BeginRelease(int32 Index);
	void EndRelease(int32 Index);
	void Discard(int32 Index, const TCHAR* Why);
	void Fail(int32 Index, const FString& Why);
	/** Asks the cook of the chunk's mesh to stop, and lets the mesh go. */
	void AbortCook(int32 Index);
	void CancelAllJobs();

	TSharedPtr<const FMantleHeightfield> Field;
	FMantleTerrainBuildParams Params;
	FMantleTerrainChunkGrid Grid;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> Material = nullptr;
	bool bStatic = true;
	bool bCastShadow = true;

	TArray<FChunk> Chunks;

	/**
	 * The meshes whose collision is being cooked, by chunk index. A property, so the garbage collector sees them and nothing roots them: a world that ends while a cook runs is
	 * collected with its actor, and the engine's cook for a mesh that is gone is dropped (UBodySetup aborts it).
	 */
	UPROPERTY()
	TMap<int32, TObjectPtr<UStaticMesh>> CookingMeshes;

	FMantleTerrainStreamStats Stats;
	uint64 AreaSerial = 1;
	uint64 TakeDownSerial = 0;
	bool bConfigured = false;
	double LastRadiusCm = 0.0;

	/** The views of the last plan (terrain space) and when it ran, so a still view does not plan again. */
	TArray<FVector2D> PlannedViews;
	double PlannedRadiusCm = -1.0;
	double LastPlanSeconds = -1.0e9;
	bool bPlanDirty = true;
	double LastAuditSeconds = -1.0e9;
	int32 BuildingCount = 0;
	FString TerrainName;
};
