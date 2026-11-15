// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "MantleCellArea.h"
#include "MantleCellAssets.h"
#include "MantleCellLoader.h"
#include "MantleCellStreamPlan.h"
#include "MantlePlaceBuilder.h"
#include "MantlePlaceCells.h"
#include "UObject/WeakObjectPtrTemplates.h"

class AActor;
class UWorld;

/** What the streamer has done and is doing, for the join screen, the logs and the measurements. */
struct MANTLERUNTIME_API FMantleCellStreamStats
{
	/** Cells now: waiting on a read, read and waiting for their assets, read and waiting for the builder, standing, being taken down, and ones that can never be read. */
	int32 CellsLoading = 0;
	int32 CellsAssetWait = 0;
	int32 CellsReady = 0;
	int32 CellsBuilt = 0;
	int32 CellsRemoving = 0;
	int32 CellsFailed = 0;

	/** Wanted cells (inside their draw distance of the view or its path) that do not stand yet: the backlog. */
	int32 CellsPending = 0;

	/** The nearest of them to the view, cm, with and without solid things in them; negative when nothing is pending. */
	double NearestPendingCm = -1.0;
	double NearestPendingSolidCm = -1.0;

	/** The nearest of them with solid things that is in front of the view (its nearest point is ahead of the view's velocity); negative when none is, or the view is not moving. */
	double NearestPendingAheadSolidCm = -1.0;

	/** Since the start: pieces built (one entity or one piece of a solid list), their instances and solid things, actors taken down, and cells built and released whole. */
	int64 UnitsBuilt = 0;
	int64 InstancesBuilt = 0;
	int64 SolidBuilt = 0;
	int64 ActorsRemoved = 0;
	int64 CellsBuiltEver = 0;
	int64 CellsReleasedEver = 0;
	int32 BuildErrors = 0;
	/** Plans made since the start (a Refresh that went through every cell's wants; one that was skipped because everything of ours already stands is not counted). */
	int32 PlansRun = 0;
	/** Later pieces of a long solid list that were skipped because a script destroyed the entity after its first piece was built. */
	int32 PiecesSkippedDestroyed = 0;
	/** Cells whose assets were asked for, and entities left out because their mesh could not be had (cells whose assets did not all arrive are built without them). */
	int64 CellAssetsAsked = 0;
	int64 EntitiesWithoutMesh = 0;
	int32 AssetsMissing = 0;
	/** Cells that gave up waiting for an asset (the timeout) and were built without it. */
	int32 AssetWaitTimeouts = 0;
	/** Longest a cell waited for its assets, seconds. */
	double LongestAssetWaitSeconds = 0.0;

	/** Game-thread seconds spent building and removing since the start, this tick's, and the worst tick's. */
	double BuildSeconds = 0.0;
	double RemoveSeconds = 0.0;
	double LastWorkSeconds = 0.0;
	double WorstWorkSeconds = 0.0;
	/** Pieces that took 2, 5, 10 and 20 ms or more (each counted in every bucket it reaches). */
	int64 PiecesOver2Ms = 0;
	int64 PiecesOver5Ms = 0;
	int64 PiecesOver10Ms = 0;
	int64 PiecesOver20Ms = 0;
	/** The longest single piece, and its entity. */
	double LongestUnitSeconds = 0.0;
	FString LongestUnitEntity;

	/** The budget this tick was given, seconds. */
	double LastBudgetSeconds = 0.0;
};

/**
 * Builds a place's scenery cells around the player as they move and takes them down behind them (stage 2 of large places).
 *
 * The place's root (everything not in a cell) stands already. This keeps the cell index and reads the cell files of the cells that are wanted:
 * a cell is wanted when the view, or the path it is travelling (FMantleCellStreamPlan), is within the cell's drawDistance of its bounds. Wanted cells are read
 * (FMantleCellLoader's stream mode), then built a piece at a time within a per-frame time budget, nearest to the path first, solid things before
 * pictures. Each piece is one FMantlePlaceBuilder::Build of one entity (a long list of solid instances is cut into pieces first), so what the physics
 * solver first sees of it is final: static, in place. A built cell is taken down once the view is well past its drawDistance (the hysteresis in FMantleCellStreamSettings).
 *
 * The join and a teleport hold: BeginHold stays on until every cell around the view that wants to be built is, then OnHoldChanged(false), so the
 * owner can keep the player still meanwhile. A jump of the view (more than TeleportDistanceCm in a tick) begins one itself.
 *
 * Game thread. Driven by the engine's core ticker (like FMantlePlaceBuildRunner) unless bAutoTick is off. Log markers: MANTLE_CELLS_*.
 * Scenery only: cells hold StaticMesh and InstancedMesh entities with no parent, tags or script names (contract 5.2), so nothing a script can name is
 * ever missing because its cell is not built.
 */
class MANTLERUNTIME_API FMantleCellStreamer : public TSharedFromThis<FMantleCellStreamer>
{
public:
	UE_NONCOPYABLE(FMantleCellStreamer);

	/**
	 * Where the player is, when it is known yet (false: use the last place, or the start's). This is the view the join and a teleport hold for: its jumps
	 * are teleports, and the cells around it are what the hold waits for. It is the local character's place (or the camera's, when there is no character).
	 */
	using FViewProvider = TFunction<bool(FVector& OutLocation)>;

	/**
	 * Another place the scenery must stand around, which holds nothing: the camera when it is not at the player (a script can put it far from a
	 * character that still moves), and later whatever else the scenery must stand under. Key names the thing from tick to tick, so its velocity can be
	 * followed; a key that is not given one tick is forgotten.
	 */
	struct FExtraView
	{
		int32 Key = 0;
		FVector Location = FVector::ZeroVector;
	};
	using FExtraViewsProvider = TFunction<void(TArray<FExtraView>& OutViews)>;

	/** Called when the hold begins (true) and ends (false). */
	using FHoldChanged = TFunction<void(bool bHolding)>;

	/**
	 * A piece of a cell's entity stands: the entity's id (without the "~N" of a piece cut from a long list), its name and the actor. Called once for each piece, after it is
	 * built. The script world keeps a record of the entity while any piece of it stands.
	 */
	using FPieceBuilt = TFunction<void(const FString& EntityId, const FString& Name, AActor* Actor)>;

	/** A cell is being taken down: called once for each entity that had a piece standing in it, before its actors are destroyed. Also for every standing cell when the streamer stops. */
	using FEntityReleased = TFunction<void(const FString& EntityId)>;

	/**
	 * Starts streaming Root's cells into World. KnownIndex is the index when the caller has read it already. InitialView is where the view is until the
	 * provider has one. The streamer holds from the start (reason "join") until the area around the view stands.
	 */
	static TSharedRef<FMantleCellStreamer> Start(
		UWorld* World,
		const FMantlePlace& Root,
		const FMantleCellIndex* KnownIndex,
		const FMantleCellLoaderSettings& LoaderSettings,
		FViewProvider ViewProvider,
		const FVector& InitialView,
		const FMantleCellStreamSettings& Settings = FMantleCellStreamSettings());

	~FMantleCellStreamer();

	/** One engine tick of work. The core ticker calls it unless Settings.bAutoTick is off (tests tick by hand). */
	void Tick(double DeltaSeconds);

	/** Takes down every cell it built, and stops. Not needed when the world itself is going. */
	void Stop();

	/** One MANTLE_CELLS_SUMMARY line: what was built and taken down, the longest piece and the worst tick. Logged by Stop and when the world goes. */
	void LogSummary(const TCHAR* Why) const;

	void SetHoldChangedCallback(FHoldChanged InCallback) { HoldChanged = MoveTemp(InCallback); }

	/** Who is told what stands (see FPieceBuilt, FEntityReleased). */
	void SetEntityCallbacks(FPieceBuilt InBuilt, FEntityReleased InReleased)
	{
		PieceBuilt = MoveTemp(InBuilt);
		EntityReleased = MoveTemp(InReleased);
	}

	/**
	 * Where the cells' meshes and materials come from. With it, a cell that has been read asks for its assets and is built when they have arrived (or failed:
	 * built without them) and gives them back when it comes down; without it (tests, a place whose assets are all prepared) a cell is built as soon as it is read.
	 */
	void SetCellAssets(TSharedPtr<IMantleCellAssets> InAssets) { CellAssets = MoveTemp(InAssets); }

	/** Holds until the cells around the view stand (or HoldTimeoutSeconds). A no-op when already holding. */
	void BeginHold(const TCHAR* Reason);
	bool IsHolding() const { return bHolding; }

	/**
	 * Every cell that has a point within RadiusCm of Point and wants to be built from there (Point is within its drawDistance of it) stands, or can
	 * never be read. False until the index has been read.
	 */
	bool IsAreaBuilt(const FVector& Point, double RadiusCm) const;

	/** Every cell this streamer builds stands, or can never be read (what the join of a streamer that keeps every cell waits for). False until the index has been read and planned. */
	bool IsEverythingBuilt() const;

	/**
	 * What the cells within Query.RadiusCm of Query.Point are doing: how many there are, how many stand (for a solid-only question, with something solid in them, whose
	 * solid pieces stand; their pictures may still be coming), which can never be read, and which are wanted and still on their way. Bodies that must not simulate
	 * without ground under them, and reflection probes that must not capture before their surroundings stand, ask this through FMantleCellArea.
	 */
	FMantleCellAreaState QueryArea(const FMantleCellAreaQuery& Query) const;

	/** Changes whenever the answer to a QueryArea can: a piece built, a cell standing, taken down or failed. Two reads that match mean nothing changed between them. */
	uint64 GetAreaSerial() const;

	/**
	 * Changes only when ground can go: a cell was handed to its take-down (its state is "taking down", which is not standing, from that moment, before its first piece goes)
	 * or failed. A body asks the area again at once when this changes, so it stops simulating in the frame its ground is released and not up to a tenth of a second later.
	 */
	uint64 GetTakeDownSerial() const { return TakeDownSerial + static_cast<uint64>(Stats.CellsFailed); }

	bool IsIndexReady() const { return bIndexReady; }
	bool HasFailed() const { return Loader.IsValid() && Loader->HasFailed(); }
	/** Only while IsIndexReady() (it is false again after Stop). */
	const FMantleCellIndex& GetIndex() const { checkf(bIndexReady && Loader.IsValid(), TEXT("GetIndex before the index is read, or after Stop")); return Loader->GetIndex(); }
	const FMantleCellStreamStats& GetStats() const { return Stats; }
	/** Cell files read since the start (the join screen counts each as progress). */
	int32 GetFilesRead() const { return Loader.IsValid() ? Loader->GetFilesReadTotal() : 0; }
	/** Cell files read plus pieces built: the join screen counts a change in it as progress. */
	int64 GetJoinWork() const { return Stats.UnitsBuilt + GetFilesRead(); }
	const FVector& GetViewLocation() const { return View.Location; }
	const FMantleCellView& GetView() const { return View; }
	const FMantleCellStreamSettings& GetSettings() const { return Settings; }

	/** This cell stands (every piece built). */
	bool IsCellBuilt(int32 CellIndex) const;

#if !UE_BUILD_SHIPPING
	/**
	 * Test tools, not in a shipped build: changes one number of the settings by name (lookahead seconds, budget and fastbudget in ms, readahead cells, release margin, skipbuild 0 or 1, keepall 0 or 1)
	 * so a run can compare two values in one launch. False for a name it does not know.
	 */
	bool SetTestSetting(const FString& Name, double Value);
#endif

	/** Where the streamer takes the view from once its owner can say (before that it is the initial view): a join sets this when the player is placed. */
	void SetViewProvider(FViewProvider InProvider) { ViewProvider = MoveTemp(InProvider); }

	/** The extra views (see FExtraView): what is wanted is every cell any of them, or the player, wants; none of them holds or teleports. */
	void SetExtraViewsProvider(FExtraViewsProvider InProvider) { ExtraViewsProvider = MoveTemp(InProvider); }

private:
	enum class ECellState : uint8 { Loading, Assets, Ready, Built, Removing };

	/** A cell that is not idle. */
	struct FCell
	{
		ECellState State = ECellState::Loading;
		/** Where it stands in the build order, as of the last plan. */
		FMantleCellStreamPlan::FRank Rank;
		bool bSolid = false;
		/** Ready: how many of Content's entities, from the front, are solid (they are built first): the cell's solid part stands once Cursor has passed them. */
		int32 SolidPieces = 0;

		/** Assets: when its assets were asked for, on the platform clock. */
		double AssetsAskedAt = 0.0;

		/** Ready: the entities not built yet, in build order (solid first); Cursor is the next. Pieces of a cut list are entities of their own. */
		FMantleCellContent Content;
		int32 Cursor = 0;

		/** Built and being built: the actors, for taking down; Removing: the ones left (RemoveCursor is the next). */
		TArray<TWeakObjectPtr<AActor>> Actors;
		int32 RemoveCursor = 0;

		/** The entities with a piece standing here (told to FPieceBuilt), each once, for FEntityReleased. */
		TArray<FString> EntityIds;

		/** The actor the first piece of each long list made (by the entity's id): the later pieces add their instances to its component. */
		TMap<FString, TWeakObjectPtr<AActor>> PieceActors;
	};

	/** An extra view as tracked from tick to tick. */
	struct FTrackedView
	{
		FMantleCellView View;
		FVector LastSample = FVector::ZeroVector;
		FVector LastRefreshLocation = FVector::ZeroVector;
	};

	explicit FMantleCellStreamer(const FMantleCellStreamSettings& InSettings);

	void OnIndexReady();
	void SampleView(double DeltaSeconds);
	void SampleExtraViews(double DeltaSeconds);
	/** Where a cell stands in the build order: its best rank over the player and the extra views. */
	FMantleCellStreamPlan::FRank RankFor(int32 CellIndex) const;
	/** The fastest of the views, cm a second: what the budget follows. */
	double FastestViewSpeed() const;
	void Refresh();
	void TakeFinishedCells();
	/** Cells waiting for their assets: those whose assets are in (or failed, or took too long) go on to the builder. */
	void TakeAssetCells();
	/** The cell's assets that could not be had are left out of what is built: entities whose mesh failed, a failed material's slot (the mesh's own is kept). Logged once per asset. */
	void LeaveOutMissing(int32 CellIndex, FCell& Cell, const TArray<FString>& Missing);
	/** Gives the cell's assets back. */
	void ReleaseAssets(int32 CellIndex);
	void RunWork(double BudgetSeconds);
	bool RemoveSome(double Deadline);
	bool BuildOne();
	void PrepareCell(int32 CellIndex, FCell& Cell);
	void QueueRemoval(int32 CellIndex);
	/** Tells EntityReleased about every entity of the cell that has a piece standing. */
	void ReleaseEntities(FCell& Cell);
	void UpdateStats();
	void CheckHold();
	void SetHolding(bool bNow, const TCHAR* Reason);

	const FMantleCellInfo& InfoOf(int32 CellIndex) const { return Loader->GetIndex().Cells[CellIndex]; }

	FMantleCellStreamSettings Settings;
	TWeakObjectPtr<UWorld> World;
	TSharedPtr<FMantleCellLoader> Loader;
	TSharedPtr<IMantleCellAssets> CellAssets;
	/** Assets already named in the log as missing (once each). */
	TSet<FString> MissingAssetsLogged;
	FViewProvider ViewProvider;
	FExtraViewsProvider ExtraViewsProvider;
	TMap<int32, FTrackedView> ExtraViews;
	FHoldChanged HoldChanged;
	FPieceBuilt PieceBuilt;
	FEntityReleased EntityReleased;
	/** The meshes this streamer has built a piece of (for the slow-piece log: is this the first time). */
	TSet<FString> MeshesBuilt;
	FTSTicker::FDelegateHandle TickerHandle;

	/** The root's place id (for the slice place the builder wants) and the builder's own place for one piece at a time. */
	FMantlePlace SlicePlace;
	FMantleBuildOptions SliceOptions;

	bool bIndexReady = false;
	TArray<double> LevelOverhangs;
	TArray<double> LevelRadii;

	/** Every cell that is not idle, by index position. */
	TMap<int32, FCell> Cells;
	/** The cells read and waiting to be built; cells loading, in the order handed to the loader; cells standing (for the release check). */
	TArray<int32> ReadyCells;
	/** Cells read and waiting for their assets. */
	TArray<int32> AssetCells;
	TArray<int32> LoadingCells;
	TArray<int32> BuiltCells;
	TArray<int32> RemovingCells;
	/** Wanted cells that do not stand, from the last Refresh (the backlog). */
	TArray<int32> PendingCells;

	FMantleCellView View;
	FVector LastSample = FVector::ZeroVector;
	FVector LastRefreshLocation = FVector::ZeroVector;
	double SecondsSinceRefresh = 0.0;
	bool bHaveSample = false;
	bool bRefreshNeeded = true;
	/** A keep-every-cell streamer whose last plan left nothing to read, build or wait for: its wants never change after that (they do not depend on the view), so it plans no more. */
	bool bKeepAllSettled = false;

	bool bHolding = false;
	/** The own-ground hold timed out with its cells still not standing: no new one starts until they do (a cell stuck on its assets must not freeze the player again every minute). */
	bool bOwnGroundGaveUp = false;
	/** Cells handed to QueueRemoval since the start (GetTakeDownSerial). */
	uint64 TakeDownSerial = 0;
	double HoldStartSeconds = 0.0;
	FString HoldReason;

	/** The loader is pumped from Tick (its settings did not give it a ticker of its own: tests). */
	bool bPumpLoader = false;

#if !UE_BUILD_SHIPPING
	/** Test tool: the cell's entities are read and counted but not built, so a run can tell what the reading costs in memory from what the building does. */
	bool bTestSkipBuild = false;
#endif

	bool bStopped = false;
	FMantleCellStreamStats Stats;
};
