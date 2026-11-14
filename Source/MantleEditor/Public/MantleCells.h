// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"

struct FMantleInstancedMeshGroup;

/**
 * Cells (large places, stage 1): the exporter's side of the cells format, BP\contracts\cells-format.md.
 *
 * The scenery of a place (StaticMesh and InstancedMesh entities that nothing names) is written to cell files, one grid cell at a
 * time, and the root place file keeps what must always be loaded. Everything here is pure: it works on a built FMantlePlace, so
 * it is tested without a level.
 *
 * Section numbers below are the contract's.
 */

/** One level of the grid (4.1): cells of CellSize holding content drawn up to MaxDrawDistance. Whole centimeters. */
struct FMantleCellLevel
{
	int64 CellSize = 0;
	int64 MaxDrawDistance = 0;
};

/** A cell's key (4.1): the level and the cell's square, floor(pivot / cellSize) on X and Y. */
struct FMantleCellKey
{
	int32 Level = 0;
	int32 X = 0;
	int32 Y = 0;

	bool operator==(const FMantleCellKey& Other) const { return Level == Other.Level && X == Other.X && Y == Other.Y; }
	bool operator<(const FMantleCellKey& Other) const
	{
		return Level != Other.Level ? Level < Other.Level : (X != Other.X ? X < Other.X : Y < Other.Y);
	}
	friend uint32 GetTypeHash(const FMantleCellKey& Key) { return HashCombineFast(HashCombineFast(::GetTypeHash(Key.Level), ::GetTypeHash(Key.X)), ::GetTypeHash(Key.Y)); }

	/** "L0 (12,-3)", for logs and Check. */
	FString ToString() const { return FString::Printf(TEXT("L%d (%d,%d)"), Level, X, Y); }
};

/** The grid (4.1, 4.2): 1 to 4 levels, both numbers strictly rising. Written into the cell index; the game reads it from there. */
struct MANTLEEDITOR_API FMantleCellGrid
{
	static constexpr int32 MaxLevels = 4;
	static constexpr int64 MinCellSize = 1000;
	static constexpr int64 MaxCellSize = 1000000;
	static constexpr int64 MaxDrawDistanceLimit = 1000000;
	static constexpr int32 MaxCoordinate = 1000000;

	TArray<FMantleCellLevel> Levels;

	/** The exporter's numbers until stage 0 reports (4.2): 64 m cells to 150 m, 256 m to 600 m, 1 km to 2.5 km. */
	static FMantleCellGrid Default();

	/** Whether the numbers follow the format's rules (4.2). */
	bool IsValid(FString* OutWhy = nullptr) const;

	/** The top level's draw distance: content drawn farther than this, or never culled, is always loaded. 0 for no levels. */
	int64 TopMaxDrawDistance() const;

	/** The smallest level whose MaxDrawDistance is at least DrawDistance (above 0); INDEX_NONE when none is, or DrawDistance is not above 0. */
	int32 LevelFor(double DrawDistance) const;

	/** The cell of a pivot (X, Y, in cm) at Level. */
	FMantleCellKey KeyFor(int32 Level, double X, double Y) const;
};

/** Why content stayed in the root place file, for Check (5.2). In the order the rules are tried. */
enum class EMantleCellStay : uint8
{
	/** The type is not StaticMesh or InstancedMesh (lights, decals, triggers, spawns, scripts' points...). */
	OtherType,
	NoDrawDistance,
	DrawDistanceTooFar,
	Dynamic,
	Tagged,
	Physics,
	Interactable,
	/** Another entity names it: a parent, a seat, a constraint, a crowd point, an interactable part. */
	Named,
	/** Its name is a quoted string in a script, package or UI file. */
	ScriptLiteral,
	NotSpatiallyLoaded,
	/** Its Group chain holds something that is not a plain Group. */
	ParentNotPlain,
	/** Its world transform is sheared, mirrored or not a valid transform. */
	TransformNotFlat,
	/** Its cell is outside the +-1,000,000 squares a key can name. */
	OutOfGrid,
	Count
};

/** The creator-facing words for a reason, phrased to follow a count ("12 meshes stay always loaded: ..."). */
MANTLEEDITOR_API const TCHAR* MantleCellStayText(EMantleCellStay Reason);

/** One cell as planned: its entities (sorted by id) and what the index says about it. */
struct FMantlePlannedCell
{
	FMantleCellKey Key;
	TArray<FMantleEntity> Entities;

	/** The union of the content's world bounding boxes (4.1); a point at the pivot where a mesh's bounds are unknown. */
	FBox Bounds = FBox(ForceInit);

	/** The largest drawDistance of its content (6.1). */
	double DrawDistance = 0.0;

	/** entities, instances and solid, as 6.1 defines them. */
	int32 NumEntities() const { return Entities.Num(); }
	int64 NumInstances() const;
	int64 NumSolid() const;
};

/** What the planner did, for Check and the logs. */
struct FMantleCellReport
{
	/** Occupied cells per level. */
	int32 CellsPerLevel[FMantleCellGrid::MaxLevels] = { 0, 0, 0, 0 };

	/** Entities and instances written to cells (an InstancedMesh counts once as an entity). */
	int64 CellEntities = 0;
	int64 CellInstances = 0;

	/** Static mesh actors folded into instance sets inside cells (5.4), and the sets that came of them. */
	int32 FoldedActors = 0;

	/** Plain Groups dropped because every child moved to a cell (5.3). */
	int32 DroppedGroups = 0;

	/** Entities (and instance sets' instances) kept in the root, by the first reason found. Instances count one each. */
	int64 StayCounts[static_cast<int32>(EMantleCellStay::Count)] = {};

	/** The cells holding the most entities, biggest first (at most 5), for Check. */
	TArray<FMantleCellKey> BiggestCells;

	int32 NumCells() const;
	int64 NumStayed() const;
};

/** What the exporter knows that the entities alone do not say. */
struct FMantleCellPlanInput
{
	FMantleCellGrid Grid = FMantleCellGrid::Default();

	/** Fold plain static mesh actors of one set into instance lists (5.4). On unless a test wants the actors as they are. */
	bool bFoldStaticMeshes = true;

	/** Ids of the entities whose actor has "Is Spatially Loaded" off (5.2 rule 6). */
	TSet<FString> NotSpatiallyLoaded;

	/** Every quoted string in the level's local script, package and UI files (5.2 rule 5). */
	TSet<FString> ScriptLiterals;

	/** A mesh's bounds in its own space (cm), by the id the entities name it by; used for cell bounds. */
	TMap<FString, FBox> MeshBounds;
};

struct FMantleCellPlan
{
	/** Sorted by key. */
	TArray<FMantlePlannedCell> Cells;
	FMantleCellReport Report;
};

/** Cell planning (5.2 to 5.5). */
class MANTLEEDITOR_API FMantleCellPlanner
{
public:
	/**
	 * Moves the scenery of Place into cells. Eligible StaticMesh entities (all of 5.2's rules) leave Place.Entities, flattened to
	 * their world transform where a plain Group chain held them (5.3); a plain Group left with no child is dropped. Eligible
	 * instances leave Groups (each group's WorldInstances; a group with a draw distance within the grid). Within a cell, plain
	 * StaticMesh entities of one set fold into the instance list when the cell holds MinGroupSize of them (5.4). What stays is
	 * untouched, for the root (FMantlePlaceInstancing::GroupStaticMeshes and ChunkInstancedMeshes run after this).
	 *
	 * Groups' RootOnlyInstances are never moved. Deterministic: the same place gives the same cells, whatever order Unreal loaded
	 * actors in.
	 */
	static FMantleCellPlan Plan(FMantlePlace& Place, TArray<FMantleInstancedMeshGroup>& Groups, const FMantleCellPlanInput& Input);

	/** The id of a cell's instance chunk (5.5): ent_ism_ + 24 hex of MD5(key + "|L" + level + "|" + x + "|" + y) + _ + chunk. */
	static FString InstancedEntityId(const FString& GroupKey, const FMantleCellKey& Cell, int32 Chunk);
};

/** One cell file as written: the MCF1 bytes and what the index needs to say about them. */
struct FMantleCellFileBlob
{
	FMantleCellKey Key;

	/** 0 for the first file of a cell (3.2: a cell over the size limit gets more files). */
	int32 Part = 0;

	/** MCF1 container, ready to upload. */
	TArray<uint8> Bytes;

	/** "sha256:" + hex of Bytes. */
	FString Hash;

	int32 Entities = 0;
	int64 Instances = 0;
	int64 Solid = 0;
};

/** What one cell's files are called once uploaded, for the index (6.1). */
struct FMantleCellFileRef
{
	FString Id;
	FString Hash;
	int64 Bytes = 0;
};

struct FMantleCellIndexEntry
{
	FMantleCellKey Key;
	FBox Bounds = FBox(ForceInit);
	double DrawDistance = 0.0;
	TArray<FMantleCellFileRef> Files;
	int32 Entities = 0;
	int64 Instances = 0;
	int64 Solid = 0;
};

/** The cell index's asset as the root names it (2). */
struct FMantleCellRoot
{
	FString IndexId;
	FString Hash;
	int64 Bytes = 0;
};

/** A file of an offline export: a cell file or the index, under the provisional id the exporter gives it. */
struct FMantleCellOfflineFile
{
	FString Id;
	bool bIndex = false;
	TArray<uint8> Bytes;

	/** "sha256:" + hex of Bytes. */
	FString Hash;
};

/** The containers (3.1) and the documents (6.1, 6.2). */
class MANTLEEDITOR_API FMantleCellFiles
{
public:
	static constexpr int32 FormatVersion = 1;

	/** 3.2: a cell file's compressed and inflated size, files a cell may have, and the index's size. */
	static constexpr int64 MaxCellFileBytes = 8 * 1024 * 1024;
	static constexpr int64 MaxCellRawBytes = 8 * 1024 * 1024;
	static constexpr int32 MaxFilesPerCell = 64;
	static constexpr int64 MaxIndexBytes = 64 * 1024 * 1024;
	static constexpr int64 MaxIndexRawBytes = 64 * 1024 * 1024;

	/** The containers' magics (3.1). */
	static const TCHAR* CellMagic() { return TEXT("MCF1"); }
	static const TCHAR* IndexMagic() { return TEXT("MCI1"); }

	/** magic + uint32 LE raw length + one zlib stream of Json (UTF-8, no BOM). */
	static bool MakeContainer(const TCHAR* Magic, const FString& Json, TArray<uint8>& OutBytes, FString& OutError);

	/** Reads one back, refusing a wrong magic, a length over MaxRaw, a mismatch or trailing bytes (the game's reader does the same). */
	static bool ReadContainer(TConstArrayView<uint8> Bytes, const TCHAR* Magic, int64 MaxRaw, FString& OutJson, FString& OutError);

	/** The cell's entities as the cell file's document (6.2): {"version":1,"entities":[...]}. */
	static FString CellDocument(TConstArrayView<FMantleEntity> Entities);

	/**
	 * Writes a cell as one or more MCF1 files (3.2): entities in id order, whole entities only, a new file when the next entity
	 * would take the inflated document past MaxRaw (default MaxCellRawBytes). False with a sentence when the cell cannot be written
	 * (over MaxFilesPerCell, or a file over MaxCellFileBytes compressed).
	 */
	static bool WriteCell(const FMantlePlannedCell& Cell, TArray<FMantleCellFileBlob>& OutFiles, FString& OutError, int64 MaxRaw = MaxCellRawBytes);

	/** The cell index's document (6.1) and its MCI1 bytes. Cells must be sorted by key. */
	static FString IndexDocument(const FMantleCellGrid& Grid, TConstArrayView<FMantleCellIndexEntry> Cells);
	static bool WriteIndex(const FMantleCellGrid& Grid, TConstArrayView<FMantleCellIndexEntry> Cells, TArray<uint8>& OutBytes, FString& OutError);

	/**
	 * Every cell file and the index of Cells as an offline export (preview, Check) writes them, under provisional ids (nothing is
	 * uploaded): the files in cell order, the index last, and Root naming the index.
	 */
	static bool BuildOffline(const FMantleCellGrid& Grid, TConstArrayView<FMantlePlannedCell> Cells, TArray<FMantleCellOfflineFile>& OutFiles, FMantleCellRoot& OutRoot, FString& OutError);

	/** An id for a file never uploaded (offline export, preview, Check): FMantlePlaceAssets::ProvisionalId("cell:" + sha256) or "cellindex:". */
	static FString ProvisionalCellId(const FString& Hash);
	static FString ProvisionalIndexId(const FString& Hash);
};
