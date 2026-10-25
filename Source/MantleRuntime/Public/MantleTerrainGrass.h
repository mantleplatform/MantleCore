// Copyright Joshua Eger. All Rights Reserved.
// GhzGarage/mantle#628: a Terrain's grass. Meshes every client scatters over the terrain near its camera,
// where a grass map's channel says so (a Fab landscape's LandscapeGrassType, exported as one entry per
// variety). Visual only: no collision, nothing replicated, nothing on a dedicated server.
//
// The terrain is divided into square cells. Around the local view, each entry grows grass in the cells
// within its cull distance: a worker thread generates a cell's instances (FMantleTerrainGrassGenerator,
// pure and deterministic from the entry's index and the cell's coordinates, so revisiting a cell grows the
// same grass) and the game thread gives each finished cell one instanced static mesh component, a few
// thousand instances at a time within a per-frame budget. Cells that leave range are released, with
// hysteresis so walking along an edge does not rebuild them.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MantlePlaceTypes.h"

#include "MantleTerrainGrass.generated.h"

class UInstancedStaticMeshComponent;
class UMaterialInterface;
class UStaticMesh;
class UWorld;
struct FMantleHeightfield;
struct FMantleTerrainWeights;

/** One cell's generated grass: instance transforms relative to Origin, in the terrain's own space (cm). */
struct FMantleGrassCellInstances
{
	/** The entry's index in the Terrain's grass[], and the cell's coordinates in that entry's grid. */
	int32 Entry = 0;
	FIntPoint Cell = FIntPoint::ZeroValue;

	/** The density scale it was generated at. */
	double DensityScale = 1.0;

	/** The cell's lowest-X, lowest-Y corner at height 0. Every transform is relative to it. */
	FVector Origin = FVector::ZeroVector;

	TArray<FTransform> Instances;

	/** Candidates the cell asked for before the map's weight and the terrain's edge thinned them. */
	int32 Candidates = 0;
};

/**
 * The pure part of terrain grass: which instances a cell holds. No world, no assets, no console variable,
 * any thread; the same inputs give the same instances.
 *
 * A cell asks for density x its area candidates (rounded). Candidate i of a cell has its own counter-based
 * random stream (SplitMix64 over the entry's index, the cell's coordinates and i), so a lower density scale
 * keeps a prefix of the same candidates and a cell never depends on another. A candidate is kept with
 * probability equal to the map's weight there, so weight 0 grows nothing and weight 1 keeps every one.
 */
class MANTLERUNTIME_API FMantleTerrainGrassGenerator
{
public:
	/** The side of a cell, cm: 32 m, halved for dense entries (CellSizeCm). */
	static constexpr double BaseCellCm = 3200.0;
	static constexpr double MinCellCm = 400.0;

	/** The most candidates one cell may ask for at density scale 1, which bounds what one cell adds in one frame. */
	static constexpr int32 MaxCandidatesPerCell = 16384;

	/** The side of Entry's cells: BaseCellCm, halved while a cell would ask for more than MaxCandidatesPerCell (never below MinCellCm). */
	static double CellSizeCm(const FMantleTerrainGrass& Entry);

	/** Cells along X and Y that cover the terrain's extent, at least 1 each. */
	static FIntPoint CellCount(const FMantleHeightfield& Field, double Side);

	/** The candidates a cell of Entry asks for at DensityScale (0 and below ask for none): density x area, rounded, capped at twice MaxCandidatesPerCell. */
	static int32 CandidateCount(const FMantleTerrainGrass& Entry, double DensityScale);

	/** The seed of a cell's candidates: a function of the entry's index and the cell, nothing else. */
	static uint64 CellSeed(int32 EntryIndex, const FIntPoint& Cell);

	/**
	 * The terrain's surface at (X, Y) in its own space: the height on the very triangles the chunk meshes draw
	 * (FMantleTerrainBuilder's diagonal) and the heightfield's smooth normal, interpolated. False for a heightfield
	 * too small to have a surface. (X, Y) is clamped to the terrain.
	 */
	static bool SurfaceAt(const FMantleHeightfield& Field, double X, double Y, float& OutZ, FVector3f& OutNormal);

	/**
	 * Fills Out with the grass of Cell for Entry (entry number EntryIndex), whose map is Map, which must hold pixels
	 * (a map without any grows nothing). The map is stretched over the whole terrain as the weightmap is, texel
	 * centres at (i + 0.5) / size. Instances are uniformly scaled between Entry's scale range, turned about the up
	 * axis when it asks for a random yaw, and tilted onto the surface normal when it asks to align.
	 */
	static void GenerateCell(const FMantleTerrainGrass& Entry, int32 EntryIndex, const FIntPoint& Cell, const FMantleHeightfield& Field,
		const FMantleTerrainWeights& Map, double DensityScale, FMantleGrassCellInstances& Out);
};

/** What one grass entry draws with, resolved on the game thread by the place builder. */
USTRUCT()
struct FMantleGrassEntryAssets
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UStaticMesh> Mesh = nullptr;

	/** By slot; null leaves the slot on the mesh's own. */
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInterface>> Materials;
};

/**
 * A built Terrain's grass (#628), on its actor: grows each entry's grass in the cells around the local view
 * and releases what the view leaves. Added by the place builder on a machine that draws, never on a dedicated
 * server or in an editor world; nothing of it replicates, since each machine builds the same terrain from the
 * same place.
 *
 * Console variables: mantle.Grass.DensityScale (1; 0 turns grass off), mantle.Grass.MaxInstancesPerFrame (the
 * most instances added to components in one frame) and mantle.Grass.MaxLiveInstances (the most standing at once).
 * Log marker: MANTLE_GRASS.
 */
UCLASS(ClassGroup = Mantle, NotBlueprintable, Transient)
class MANTLERUNTIME_API UMantleTerrainGrassComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UMantleTerrainGrassComponent();

	/**
	 * Entry i of Entries draws with Assets[i] and samples Maps[i] (its grass map's pixels). An entry whose mesh or
	 * map failed to load is passed with a null mesh or map and grows nothing. The heightfield is the terrain's.
	 */
	void Init(TSharedPtr<const FMantleHeightfield> InField, TArray<FMantleTerrainGrass> InEntries, TArray<FMantleGrassEntryAssets> InAssets,
		TArray<TSharedPtr<const FMantleTerrainWeights>> InMaps);

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;

private:
	struct FShared;

	enum class ECellState : uint8
	{
		/** A worker is generating it. */
		Generating,
		/** Its component stands (or it grew nothing, and has none). */
		Live,
		/** Generated, but the live-instance cap had no room; tried again when there is. */
		Skipped
	};

	struct FCell
	{
		ECellState State = ECellState::Generating;
		TWeakObjectPtr<UInstancedStaticMeshComponent> Component;
		int32 Instances = 0;
		int32 Entry = 0;
		FIntPoint Coord = FIntPoint::ZeroValue;
	};

	struct FWanted
	{
		uint64 Key = 0;
		int32 Entry = 0;
		FIntPoint Coord = FIntPoint::ZeroValue;
		double DistanceSquared = 0.0;
	};

	static uint64 CellKey(int32 Entry, const FIntPoint& Coord);

	/** The local player's view location, in the terrain's own space. */
	bool GetViewLocal(FVector& OutLocal) const;

	/** Finds the cells to grow and the cells to release around View. */
	void Scan(const FVector& View);

	void StartGenerating(const FWanted& Want);
	void Apply(FMantleGrassCellInstances&& Generated);
	void ReleaseCell(uint64 Key);
	void ReleaseAll();
	void LogSummary(bool bStarted);

	TSharedPtr<const FMantleHeightfield> Field;
	TArray<FMantleTerrainGrass> Entries;

	UPROPERTY(Transient)
	TArray<FMantleGrassEntryAssets> Assets;

	TArray<TSharedPtr<const FMantleTerrainWeights>> Maps;

	/** Cell side per entry, cm. */
	TArray<double> CellSizes;

	/** State a worker shares: the inputs it reads, the results it hands back, and whether the component is gone. */
	TSharedPtr<FShared, ESPMode::ThreadSafe> Shared;

	TMap<uint64, FCell> Cells;
	TArray<FWanted> Wanted;

	/** Finished cells not yet given a component, in the order they came back (nearest first, as they were asked for). */
	TArray<FMantleGrassCellInstances> Ready;

	int32 ActiveEntries = 0;

	/** Distinct grass maps the active entries read. */
	int32 MapCount = 0;
	int32 InFlight = 0;
	int32 LiveCells = 0;
	int64 LiveInstances = 0;

	/** The density scale the standing cells were made with; a change rebuilds them all. */
	double AppliedDensityScale = 1.0;

	double NextScanSeconds = 0.0;
	double NextLogSeconds = 0.0;

	/** When the view was first looked at (0 before that), for the start line's deadline. */
	double FirstScanSeconds = 0.0;
	bool bStartLogged = false;

	/** Counts since the last log line. */
	int32 GeneratedSinceLog = 0;
	int32 ReleasedSinceLog = 0;
	int32 SkippedSinceLog = 0;
};
