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

	/** A cell's far reach is its drawDistance times this, at most MaxReachCm. 600 m cells reach 1.8 km: a 10 m model is still 3 px there. */
	double ReachFactor = 3.0;
	double MaxReachCm = 200000.0;

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

	/** An uploaded mesh without Nanite data is left out of a far set (it would draw plain, far away). Tests with plain meshes turn this off. */
	bool bRequireNanite = true;
};
