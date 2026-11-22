// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleFarSetSettings.h"
#include "MantlePlaceCells.h"
#include "MantleStreamBudget.h"

/** Which of a cell's entities a streamer builds: all of them, the ones with collision, or the ones without (the pictures). */
enum class EMantleCellEntities : uint8
{
	All,
	SolidOnly,
	PicturesOnly,
};

/**
 * The numbers the cell streamer works to (stage 2 of large places). Every one is from stage 0's measurements (S0-M34, Epic, 60 fps), named where it is.
 */
struct MANTLERUNTIME_API FMantleCellStreamSettings
{
	/**
	 * A cell is wanted when it is within its own drawDistance of the view; once built it stays until the view is farther than
	 * drawDistance x ReleaseMargin and also farther than drawDistance + MinReleaseMarginCm. Building a cell costs about 20 times what
	 * removing it does (0.34 ms against 0.016 ms an entity, S0-M34 N1), so a view that wanders along an edge must not rebuild: 1.2 is
	 * what stage 0's harness ran with, and the floor keeps the band at least 25 m wide for a cell whose meshes draw only 60 m.
	 */
	double ReleaseMargin = 1.2;
	double MinReleaseMarginCm = 2500.0;

	/**
	 * Look-ahead: the view's velocity carries it LookAheadSeconds ahead (at most MaxLookAheadCm, in all three directions, so a diving aircraft
	 * counts), and cells near that path are wanted and built first. Below MinLookAheadSpeedCmPerSec the view is taken as standing still.
	 */
	double LookAheadSeconds = 4.0;
	double MaxLookAheadCm = 40000.0;
	double MinLookAheadSpeedCmPerSec = 500.0;

	/**
	 * The game-thread time a frame gives the loader (building and removing together), measured by elapsed time: 2 ms is what S0-M34 found
	 * removes every hitch (N4). While the view moves fast the budget rises to FastBudgetSeconds, still well inside a 60 fps frame (the
	 * frame is about 8 ms without the loader); Holding is the join or teleport hold, when nothing moves and the frame rate matters less.
	 */
	double BaseBudgetSeconds = 0.002;
	double FastBudgetSeconds = 0.006;
	double HoldBudgetSeconds = 0.008;
	double FastBudgetFromCmPerSec = 1500.0;
	double FastBudgetFullAtCmPerSec = 6000.0;

	/** The order cells are built in looks this long ahead along the velocity (see FMantleCellStreamPlan::FRank), at most MaxOrderShiftCm: half a second, and 50 m. It is part of look-ahead: with LookAheadSeconds 0 the order is nearest first. */
	double OrderAheadSeconds = 0.5;
	double MaxOrderShiftCm = 5000.0;

	/** A solid InstancedMesh is cut into pieces of at most this many instances, one piece a step: about 2 ms (S0-M34: 888 solid instances cost 18 ms). */
	int32 MaxSolidInstancesPerPiece = 100;
	/** Cells read ahead of the builder: decoded cell files wait in memory until their turn. */
	int32 MaxCellsReadAhead = 8;

	/**
	 * What a teleport and the player's own ground wait for: every cell within this distance of the spot that the cell wants at all (a cell within its own
	 * drawDistance of it) is built. 100 m: the floor and the walls round the player. The player sees the world while these hold (there is no loading
	 * screen), so they wait for what the player stands in and the rest streams in as in play.
	 */
	double HoldRadiusCm = 10000.0;

	/**
	 * What the join waits for (any hold until the owner calls NoteJoinDone): every cell within this distance of the spot that the cell wants at all. The join is
	 * behind the loading screen, so it waits for the whole first view: by default every cell the spot wants (each cell's own drawDistance, under the live budget,
	 * is the limit), so nothing the player looks at first pops in after the screen comes down.
	 */
	double JoinHoldRadiusCm = UE_DOUBLE_BIG_NUMBER;

	/**
	 * A hold lets go on its own once nothing has moved for this long: no cell file read, no piece or cell built, no byte of the place's assets arrived (a cell that
	 * cannot be fetched must not hold a player for good). While files keep arriving it keeps holding, however long a first visit's download takes.
	 */
	double HoldIdleTimeoutSeconds = 60.0;

	/**
	 * A cell waiting for its assets gives up on the ones not here once the place's downloads have not moved for this long (since it asked, at the earliest); it is then built
	 * without them. A download that never ends must not keep a cell waiting for good, but a long queue that is still moving is not a reason to build a cell with holes.
	 */
	double AssetIdleTimeoutSeconds = 90.0;

	/** How often the place's download progress (IMantleCellAssets::GetProgressMark, a walk over the asset set) is looked at while a hold or an asset wait needs it. */
	double ProgressSampleSeconds = 0.5;

	/**
	 * Asset builds (a mesh, a material, a texture: game-thread work in the place's asset set) and cell pieces share the tick's budget: while any cell waits for its assets, this share of the
	 * budget goes to the assets and the rest to pieces; with none waiting the assets get AssetIdleSliceSeconds and the pieces all of it.
	 */
	double AssetBudgetShare = 0.5;
	double AssetIdleSliceSeconds = 0.0005;

	/**
	 * A move of the player bigger than this in one tick is a teleport, not travel (the fastest thing in a place is far below 100 m a tick). A long frame
	 * is allowed to travel farther: the limit is the larger of this and TeleportAllowedSpeedCmPerSec x the frame's seconds (300 m/s, above anything
	 * a place moves at: a 0.5 s hitch at 200 m/s is 100 m of travel, not a teleport).
	 */
	double TeleportDistanceCm = 10000.0;
	double TeleportAllowedSpeedCmPerSec = 30000.0;

	/** An extra view (the camera, when the player is somewhere else) closer than this to the player's own adds nothing to what is wanted. */
	double SameViewDistanceCm = 1000.0;

	/** The velocity is a running average over about this long, so one stuttering frame does not swing the look-ahead. */
	double VelocitySmoothingSeconds = 0.25;

	/**
	 * A standalone game is the server and the player in one process: everything with collision stands always (as on a server: NPCs, cars and props never fall, a server script's ray
	 * hits anywhere), and the pictures follow the view (as for a player). So it runs two streamers: one builds SolidOnly with bKeepEveryCell, the other PicturesOnly by distance. A
	 * player's machine runs one with All, as before.
	 */
	EMantleCellEntities Entities = EMantleCellEntities::All;

	/**
	 * Every cell with something to build is wanted from the start (nearest the view first) and none is released. The hold of the join lasts until all of them stand, and
	 * a teleport holds nothing (everything already stands).
	 */
	bool bKeepEveryCell = false;

	/** The streamer ticks itself from the engine's core ticker. Tests turn it off and tick by hand. */
	bool bAutoTick = true;

	/** Far sets: the twins of a cell's big entities, drawn past the cell's drawDistance (stage 3a). Off unless the owner turns them on. */
	FMantleFarSetSettings FarSets;

	/**
	 * Live budgets (stage 8): every half second the streamer reads what stands against BudgetLimits() and moves a ladder (FMantleStreamBudget): over budget the far sets' reach falls first, then the
	 * real cells'. ReachScale is the real cells' scale now (1 = every cell is wanted from its own drawDistance); the streamer sets it. No BudgetLimits (the default, and every test) means
	 * no limits: the scale stays 1. BudgetMemoryShare gives the process's memory over the machine's RAM (without it that is not checked).
	 */
	double ReachScale = 1.0;
	FMantleBudgetTuning BudgetTuning;
	TFunction<FMantleBudgetLimits()> BudgetLimits;
	TFunction<double()> BudgetMemoryShare;
};

/** Where the player looks from and how fast that is moving, in world cm and cm a second. */
struct FMantleCellView
{
	FVector Location = FVector::ZeroVector;
	FVector Velocity = FVector::ZeroVector;
};

/**
 * What the cell streamer decides, as plain functions with no engine state: which cells are wanted, released, built first, how much time a frame
 * gives them, how a big solid list is cut. Tested alone (Mantle.Runtime.CellStream.*).
 */
class MANTLERUNTIME_API FMantleCellStreamPlan
{
public:
	/** The most points the view's path is sampled at: where it is now, and three more along the look-ahead. */
	static constexpr int32 MaxPathPoints = 4;
	using FPath = TArray<FVector, TInlineAllocator<MaxPathPoints>>;

	/**
	 * The points along the view's path: its own location, then three more at 1/3, 2/3 and the whole of the look-ahead distance in the direction of
	 * its velocity (speed x LookAheadSeconds, at most MaxLookAheadCm). A view slower than MinLookAheadSpeedCmPerSec has only its own location.
	 */
	static void PathOf(const FMantleCellView& View, const FMantleCellStreamSettings& Settings, FPath& OutPath);

	/** cm from Point to the cell's bounds (zero inside them), in all three dimensions. */
	static double DistanceToCell(const FMantleCellInfo& Cell, const FVector& Point);

	/** The nearest of Path to the cell. */
	static double DistanceToCell(const FMantleCellInfo& Cell, const FPath& Path);

	/** The cell's drawDistance is reached from some point of the path. */
	static bool IsWanted(const FMantleCellInfo& Cell, const FPath& Path);

	/** The same with the live budget's scale: wanted within FMantleStreamBudget::ScaledReach(drawDistance, Settings.ReachScale, Settings.BudgetTuning.MinReachCm); with scale 1 it is the one above. */
	static bool IsWanted(const FMantleCellInfo& Cell, const FPath& Path, const FMantleCellStreamSettings& Settings);

	/** How far from the cell's bounds it is wanted, cm, under the live budget's scale. */
	static double ReachOf(const FMantleCellInfo& Cell, const FMantleCellStreamSettings& Settings);

	/** How far from the whole path a built cell must be before it is released: its reach (ReachOf: the drawDistance, less under the live budget) x ReleaseMargin, at least MinReleaseMarginCm more than the reach. */
	static double ReleaseDistance(const FMantleCellInfo& Cell, const FMantleCellStreamSettings& Settings);
	static bool IsReleased(const FMantleCellInfo& Cell, const FPath& Path, const FMantleCellStreamSettings& Settings);

	/**
	 * Where a wanted cell stands in the build order, smaller first: its distance from where the view will be a moment from now (the view's own spot pushed ahead along its velocity
	 * by OrderAheadSeconds, at most MaxOrderShiftCm), so a cell in front of a moving view is built before one the same distance behind it, and a standing view builds nearest first.
	 * It stays a distance, so a cell beside the view is never starved by cells far ahead.
	 */
	struct FRank
	{
		/** cm from the point a moment ahead. */
		double Key = 0.0;

		/** cm from the view itself: the tie. */
		double ViewDistance = 0.0;

		/** The cell's place in the index: the last tie, so the order is the same every time. */
		int32 Index = 0;
	};

	static FRank RankOf(const FMantleCellInfo& Cell, int32 Index, const FMantleCellView& View, const FMantleCellStreamSettings& Settings);
	static bool BuildsBefore(const FRank& A, const FRank& B);

	/** The frame's loader budget in seconds: Base, rising linearly to Fast between the two speeds; Hold's while holding. */
	static double BudgetSeconds(const FMantleCellStreamSettings& Settings, double SpeedCmPerSec, bool bHolding);

	/** The speed after one tick: Previous moved toward Measured by DeltaSeconds / VelocitySmoothingSeconds (all of the way when that is 1 or more). */
	static FVector SmoothVelocity(const FVector& Previous, const FVector& Measured, double DeltaSeconds, const FMantleCellStreamSettings& Settings);

	/** The player moved Delta in DeltaSeconds: farther than a teleport can be told from travel (see Settings.TeleportDistanceCm). */
	static bool IsTeleport(const FVector& Delta, double DeltaSeconds, const FMantleCellStreamSettings& Settings);

	/**
	 * A wait that began at BeganSeconds has stalled: nothing has moved for IdleTimeoutSeconds, counted from the later of when it began and LastProgressSeconds (a wait never
	 * gives up on progress made before it began). What a hold (HoldIdleTimeoutSeconds) and a cell's asset wait (AssetIdleTimeoutSeconds) give up on.
	 */
	static bool HasStalled(double NowSeconds, double BeganSeconds, double LastProgressSeconds, double IdleTimeoutSeconds);

	/**
	 * How many pieces an InstancedMesh with this many instances is built in: one when it has no collision (a visual instance costs 0.00013 ms, and a list holds at
	 * most FMantlePlaceLimits::MaxInstancesPerEntity = 4,096 of them, about 0.5 ms, which the reader enforces for cell files too), otherwise
	 * ceil(Instances / MaxSolidInstancesPerPiece). Never less than one.
	 */
	static int32 PieceCount(int32 InstanceCount, bool bSolid, const FMantleCellStreamSettings& Settings);

	/** The piece's first instance and how many it holds when Instances are cut into PieceCount equal pieces (the first ones one longer when it does not divide). */
	static void PieceRange(int32 InstanceCount, int32 Pieces, int32 PieceIndex, int32& OutFirst, int32& OutCount);

	/**
	 * How far each grid level's cells reach outside their own square in x and y (cm): a big model near an edge makes the cell's bounds larger than the
	 * square, and a search by squares has to look that much farther. One per level of the index.
	 */
	static void LevelOverhangs(const FMantleCellIndex& Index, TArray<double>& OutOverhangs);

	/**
	 * The index positions of the cells that may have a point of Path within Radius(level) of their bounds, found by their grid squares (the index is
	 * sorted by level, x, y). May return a few more than that; never fewer. Radius is each level's own (usually its maxDrawDistance). Out has no
	 * duplicates, in index order.
	 */
	static void CellsNear(const FMantleCellIndex& Index, const TArray<double>& Overhangs, const TArray<double>& LevelRadii, const FPath& Path, TArray<int32>& OutCells);
};
