// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleSkinnedMesh.h"

/** One fitted garment of a look and where it sits in the layers (FMantleAvatarContract::LayerRank: low is next to the skin). */
struct MANTLERUNTIME_API FMantleLayerGarment
{
	TSharedPtr<const FMantleSkinnedMesh> Mesh;
	int32 Rank = 0;

	/**
	 * An overlay is not cloth: it is never pushed and nothing is pushed out of it (a push would only bulge the glove over a
	 * fingertip it hides anyway), and it is hidden under the garments that cover overlays (bCoversOverlay) with no kept band
	 * at their edges: the part of it they cover goes, all of it. Nails are one, gloves cover them.
	 */
	bool bOverlay = false;

	/** The garment hides overlays under it (gloves hide nails). Otherwise an ordinary garment. */
	bool bCoversOverlay = false;
};

/** What one layer pass did. */
struct MANTLERUNTIME_API FMantleLayerFitStats
{
	/** Garments with a mesh to place. */
	int32 Garments = 0;

	/** (Outer, under) pairs whose boxes came within reach of each other and were measured. */
	int32 Pairs = 0;

	/** Garments that were moved (they got a mesh of their own), and garments left as they were because a measure or a restore refused. */
	int32 Pushed = 0;
	int32 Failed = 0;

	/** Over the pairs: LOD0 vertices less than MaxDepthCm inside an under-garment or less than GapCm off it (the ones that need a push). */
	int32 Below = 0;

	/** Vertices moved, over every LOD, and the largest push (the clearance gained along the under-garment's normal), cm. */
	int32 Moved = 0;
	double MaxPushCm = 0.0;

	/** Hide and sink (only when Apply is given OutShown): (under, over) pairs whose coverage was measured, garments edited (they got a mesh of their own), and coverage measures refused. */
	int32 HidePairs = 0;
	int32 Edited = 0;
	int32 HideFailed = 0;

	/** Triangles cut out of the under-garments, in LOD0 and over every LOD. */
	int32 HiddenLOD0 = 0;
	int32 Hidden = 0;

	/** Points moved under the garment over them, over every LOD, and the deepest sink, cm. */
	int32 Sunk = 0;
	double MaxSunkCm = 0.0;

	/** The whole pass, and the hide and sink step inside it. */
	double Ms = 0.0;
	double HideMs = 0.0;

	FString ToString() const;
};

/**
 * #65 clothing layers, the layer pass of one look. The garments are fitted to their wearer one by one and shared by
 * everyone on that body (FMantleGarmentFitter), so a fit knows nothing of the garment under it. This pass runs once
 * per look after the fits: it walks the garments from the skin outward and pushes each one out of the garments
 * already placed under it, so a jacket cut too tight no longer pokes through the shirt.
 *
 * For an outer garment and one under-garment (the outermost one under it first), every LOD of the outer garment is
 * measured against the under-garment's LOD0 (FMantleGarmentClearance::Compute; vertices whose nearest point is on the
 * under-garment's open edge, a hem or a cuff, are not over it and are left out). A vertex less than MaxDepthCm inside
 * it, or less than GapCm off it, is pushed out to GapCm by FMantleClearanceRestore::Restore, unchanged, with no
 * slack and no floor, so its smoothing and flip limiter, and the section seams staying closed, are the fit's own.
 * Vertices MaxDepthCm or more inside are not this garment's to fix: nothing measures them, so they stay unless the
 * smoothing of a push beside them reaches them.
 *
 * Then, for the look that is merged (OutShown given), each garment is edited against the garments over it, in the reference
 * pose (the garments slide against each other about as cloth on skin, so the kept band is wider than the body's 2.5 cm):
 *  - HIDE: a triangle whose three corners the garment over it covers (FMantleGarmentCoverage's rays, the under-garment's
 *    points standing in for the body's, the hit counting only on the outer layer) and lie more than KeptBandCm from that
 *    garment's open edges (hems, cuffs, collar, waist) is cut from the index list and the sections' ranges are fixed
 *    (FMantleHairHatFit::ApplyDrops). Nothing is cut where the garment over it is open, or does not reach.
 *  - SINK: the covered points of the triangles that stay (the ones in the band, and the ones beside the cut) end SinkUnderCm
 *    under the garment over them, moved SinkMaxCm at the most, smoothed SinkSmoothIters passes (FMantleBodySink, the body's own
 *    sink, with the under-garment standing in for the body), so nothing peeks out at the edges. Points not covered never move.
 * An LOD that would be left with no triangle keeps one tiny one (a mesh needs a triangle): the nails under gloves.
 *
 * Holds no UObjects; runs on any thread. The garments handed in are never changed: a garment that needs a push or an edit
 * comes back as a copy, one that does not as the same pointer. The body is not part of the surface: the fit has already
 * cleared it, and a push only moves a garment outward.
 */
struct MANTLERUNTIME_API FMantleLayerFit
{
	/** The outer garment is kept at least this far off the garment under it (below the 0.4 cm skin floor: up to four layers stack). */
	static constexpr float GapCm = 0.3f;

	/** Only vertices less than this deep inside an under-garment are pushed; deeper ones are left to hiding it (L4). */
	static constexpr float MaxDepthCm = 3.0f;

	/** An under-garment's triangles are cut only where they are more than this far from the edges of the garment over them (the body's is 2.5 cm; layers slide more). */
	static constexpr float KeptBandCm = 3.0f;

	/** Covered points of an under-garment end this far under the garment over them... */
	static constexpr float SinkUnderCm = 0.5f;

	/** ...moved at most this far... */
	static constexpr float SinkMaxCm = 1.0f;

	/** ...with this many smoothing passes (the body's). */
	static constexpr int32 SinkSmoothIters = 4;

	/**
	 * Places Garments from the skin outward (by Rank, ties in the order given) and fills OutPlaced, one per entry of
	 * Garments in the same order: the garment as it shows once it clears the garments under it. A garment without a mesh
	 * or without triangles in LOD0 is passed through and is not a layer.
	 *
	 * OutShown (optional), one per entry in the same order: OutPlaced's garments with their hidden triangles cut and their
	 * edge points sunk (see above), computed against OutPlaced's garments (the dressed body is computed from OutPlaced: the
	 * skin under a cut garment stays covered however the garment over it lies). Garments nothing was cut from or sunk under
	 * are the same pointer as in OutPlaced. Null: no hide and no sink.
	 */
	static void Apply(TConstArrayView<FMantleLayerGarment> Garments, TArray<TSharedPtr<const FMantleSkinnedMesh>>& OutPlaced, FMantleLayerFitStats& OutStats,
		TArray<TSharedPtr<const FMantleSkinnedMesh>>* OutShown = nullptr);
};
