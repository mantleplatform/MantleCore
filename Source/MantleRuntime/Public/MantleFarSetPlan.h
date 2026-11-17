// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleCellStreamPlan.h"
#include "MantleFarSetSettings.h"
#include "MantlePlaceCells.h"
#include "MantlePlaceTypes.h"

/**
 * What far sets decide, as plain functions with no engine state (Mantle.Runtime.FarSet.*).
 */
class MANTLERUNTIME_API FMantleFarSetPlan
{
public:
	/** The cell has entities that can have a twin: its drawDistance (the largest of its content's) is at least MinDrawDistanceCm. */
	static bool CellHasFarSet(const FMantleCellInfo& Cell, const FMantleFarSetSettings& Settings);

	/** How far from the cell's bounds its twin is drawn, cm: drawDistance x ReachFactor, at most MaxReachCm. */
	static double ReachOf(const FMantleCellInfo& Cell, const FMantleFarSetSettings& Settings);

	/** The reach under the live budget (Settings.ReachScale): ReachOf x the scale. The twin itself is made with the whole reach, so it draws out to it again when the budget allows. */
	static double LiveReachOf(const FMantleCellInfo& Cell, const FMantleFarSetSettings& Settings);

	/** A twin is wanted: the cell has one and the path is within its (live) reach of the cell's bounds. A scale of 0 wants none. */
	static bool IsWanted(const FMantleCellInfo& Cell, const FMantleCellStreamPlan::FPath& Path, const FMantleFarSetSettings& Settings);

	/** A built twin goes: the path is beyond reach x ReleaseMargin and beyond reach + MinReleaseMarginCm. */
	static bool IsReleased(const FMantleCellInfo& Cell, const FMantleCellStreamPlan::FPath& Path, const FMantleFarSetSettings& Settings);

	/** A twin is worth building now: the path is farther from the cell than BuildFromShare x its drawDistance, and a budget has not given the far sets up (scale 0). */
	static bool NeedsBuilding(const FMantleCellInfo& Cell, const FMantleCellStreamPlan::FPath& Path, const FMantleFarSetSettings& Settings);

	/** Every how-manyth instance an entity of this drawDistance keeps: 1 at FullDensityFromCm and above, ThinnedStride below. */
	static int32 StrideFor(double EntityDrawDistanceCm, const FMantleFarSetSettings& Settings);

	/**
	 * The twin of a StaticMesh or InstancedMesh entity (no parent, as every cell entity), or false when it has none: another type, a drawDistance below MinDrawDistanceCm (or none),
	 * or an instance list with nothing left after thinning. The twin has the entity's id, mesh and materials, no collision, no shadow, no surface and its drawDistance set to ReachCm; an instance list
	 * keeps every StrideFor-th instance from the first (the cell's lists are in Morton order, so that is an even spread). Its name has " (far)" added.
	 */
	static bool MakeTwin(const FMantleEntity& Source, double ReachCm, const FMantleFarSetSettings& Settings, FMantleEntity& OutTwin);

	/** A twin entity is drawn: not when its cell is wanted and the real entity stands (then the real one draws, and the twin would draw the same thing twice); otherwise yes. */
	static bool ShouldShow(bool bCellWanted, bool bRealStanding);
};
