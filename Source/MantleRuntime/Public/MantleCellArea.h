// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UWorld;

/** A question about the cells around a point: which of them stand here. */
struct FMantleCellAreaQuery
{
	FVector Point = FVector::ZeroVector;
	/** Cells with a point within this many cm of Point are asked about. */
	double RadiusCm = 0.0;
	/** Only cells with something solid in them count; a cell is then standing when its solid pieces are (its pictures may still be coming). */
	bool bSolidOnly = false;
	/** Also say which cells stand (FMantleCellAreaState::BuiltCellIds): a reflection probe wants to know whether a cell it never saw has come. */
	bool bWantCellIds = false;
};

/** What the cells around a point are doing, as the streamer that builds them here sees it. */
struct FMantleCellAreaState
{
	/** Cells asked about: the cells of the index with a point within the radius (with something solid, for a solid-only question). */
	int32 Cells = 0;
	/** Of them: standing, and ones that can never be read. */
	int32 Built = 0;
	int32 Failed = 0;
	/** Of them: wanted by the streamer's player and not standing yet (loading, waiting for assets, waiting to be built, being taken down, or in the backlog). */
	int32 Pending = 0;
	/** The streamer has not read its index yet: what the cells are doing is not known. Counted as nothing standing. */
	bool bUnknown = false;
	/** The streamer has built or taken down a cell since it last planned, so which cells are wanted may change at its next tick. */
	bool bPlanStale = false;
	/** The index of each cell that stands, when the query asked for it (bWantCellIds); empty otherwise. */
	TArray<int32> BuiltCellIds;

	/** Every cell asked about stands. A cell that can never be read does not (a body must keep following the server then; a join hold, which must let the player go, counts it elsewhere). */
	bool AllStand() const { return !bUnknown && Built == Cells; }

	/**
	 * A reflection probe may capture: the plan is known, nothing near it is still on its way, and either no cell is near it (its place is the root's) or at least
	 * ONE stands (not all: a cell that failed never will), so a capture is never made of a world whose cells have not come yet.
	 */
	bool ReadyToCapture() const { return !bUnknown && !bPlanStale && Pending == 0 && (Cells == 0 || Built > 0); }
};

/**
 * The seam between what lives below the cell streamer (a vehicle, a prop, a character, a reflection probe: they only ask) and the module that owns the streamer.
 * The owner sets Resolver at startup; with none set, or when it says this world builds no cells for the asker (a dedicated server, which merges every cell, or a
 * place that streams nothing), the answer is "everything stands". A standalone game answers solid questions from its solid streamer, which builds every solid thing at
 * the join and keeps it: until then the ground is not there, and a body that waits for it (a physics prop at the join) is told so.
 */
class MANTLERUNTIME_API FMantleCellArea
{
public:
	/**
	 * Fills State for Query and Serial (changes whenever a piece is built, a cell stands or is taken down or fails: an answer is the same while it is), and returns
	 * true; false when this world has no cells to wait for here.
	 */
	using FResolver = TFunction<bool(UWorld* World, const FMantleCellAreaQuery& Query, FMantleCellAreaState& OutState, uint64& OutSerial)>;
	static FResolver Resolver;

	/** True with State and Serial when the world builds cells for this question; false when it does not (then everything stands). */
	static bool Ask(UWorld* World, const FMantleCellAreaQuery& Query, FMantleCellAreaState& OutState, uint64& OutSerial);

	/**
	 * A number that changes only when ground can go (a cell is handed to its take-down, or fails), for the streamer that answers this kind of question. A body that
	 * sees it change asks again at once, so it stops simulating in the frame its ground is released and not up to AskIntervalSeconds later.
	 */
	using FTakeDownResolver = TFunction<bool(UWorld* World, bool bSolidOnly, uint64& OutSerial)>;
	static FTakeDownResolver TakeDownResolver;
	static bool TakeDownSerial(UWorld* World, bool bSolidOnly, uint64& OutSerial);
};
