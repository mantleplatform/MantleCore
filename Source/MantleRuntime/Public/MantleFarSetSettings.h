// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * The numbers far sets work to (stage 3a of large places). A far set is the twin of a cell's big entities, drawn past the cell's drawDistance, built from the cell file the streamer
 * already reads: nothing about the place file, the cell index or the backend changes.
 */
struct MANTLERUNTIME_API FMantleFarSetSettings
{
	/** Far sets are made at all (a player's machine, and a standalone game's pictures streamer; never a server). */
	bool bEnabled = false;

	/**
	 * Only entities whose own drawDistance is at least this have a twin, and only cells whose drawDistance is. 150 m: the three upper size bands of the plugin's draw distances (a radius of
	 * 1 m and up, which is 250 m and 600 m, and the 164 m a creator's own Max Draw Distance gave Medieval's trees). Seen from 540 r / d pixels at 1080p, anything smaller is under 3 px past its own distance.
	 */
	double MinDrawDistanceCm = 15000.0;

	/**
	 * A cell's far reach is its drawDistance times this, at most MaxReachCm: 600 m cells reach 1.5 km, the cap for every cell. A 10 m model is still 3 to 4 px there.
	 * Joshua chose 1.5 km (2026-11-23) over the 1.8 km the factor 3 gave (cap 2 km). Counted on City Sample (pieces of 30 cm or more, 256 m cells, beyond the 600 m ring, at the spawn):
	 * 3.11 M twin pieces out to 1.8 km, 2.23 M out to 1.5 km (29% fewer), 1.46 M out to 1.2 km. At the measured cost of a twin instance (step 3, rev 11: about 3.5 GB of working set,
	 * 1.7 GB of card and 5.3 GB of private memory a million) that is about 3 GB of working set and 1.5 GB of card less. What goes is the ring from 1.5 to 1.8 km: the outline of the tallest
	 * towers there; nothing nearer changes.
	 */
	double ReachFactor = 2.5;
	double MaxReachCm = 150000.0;

	/** A twin is built only for a cell the view is farther from than this share of the cell's drawDistance (the cells around a join spend nothing on it). */
	double BuildFromShare = 0.5;

	/** A cell's twin is let go once the view is farther than reach x ReleaseMargin and reach + MinReleaseMarginCm (the streamer's own rule for cells). */
	double ReleaseMargin = 1.2;
	double MinReleaseMarginCm = 2500.0;

	/**
	 * Entities with a drawDistance of at least this keep every instance; the ones below it (down to MinDrawDistanceCm) keep every ThinnedStride-th. Measured (S3-B, Epic, 60 fps cap): 217,000 twin instances
	 * standing cost no GPU time or frame time that shows against 55,000 or none (GPU 8.2 ms in each) and memory of the order of 0.3 to 1 KB each, so no thinning is the default; the stride stays as the knob a live budget
	 * (stage 8) turns when a place is over it ("drops far sets first").
	 */
	double FullDensityFromCm = 25000.0;
	int32 ThinnedStride = 1;

	/** Twins get at least this share of a tick's budget while any is waiting to be built, so a flight keeps the far ring filled. */
	double MinShareOfBudget = 0.25;

	/**
	 * Live budgets (stage 8), set by the streamer, not by the owner. ReachScale shrinks the far reach (1 = the reach above; 0 = no twin is wanted or built: the first thing a budget gives up, before the real cells'
	 * reach shrinks at all). A twin made while it is 0 still exists as data (it is built only when wanted), so a cell that stands through it has its twin when the budget lets far sets back.
	 */
	double ReachScale = 1.0;

	/** An uploaded mesh without Nanite data is left out of a far set (it would draw plain, far away). Tests with plain meshes turn this off. */
	bool bRequireNanite = true;
};
