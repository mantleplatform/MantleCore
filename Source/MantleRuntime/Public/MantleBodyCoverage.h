// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleSkinnedMesh.h"

struct FMantleSkinMask;

/**
 * A garment surface as the body side of the fit reads it: positions, one normal per vertex (orients
 * each triangle: its face normal is flipped to agree with the sum of its corners' normals) and a
 * triangle list. Views, so a caller can pass a fitted LOD at rest or its posed copy without copying.
 */
struct FMantleGarmentSurface
{
	TConstArrayView<FVector3f> Positions;
	TConstArrayView<FVector3f> Normals;
	TConstArrayView<uint32> Indices;

	FMantleGarmentSurface() = default;

	FMantleGarmentSurface(TConstArrayView<FVector3f> InPositions, TConstArrayView<FVector3f> InNormals, TConstArrayView<uint32> InIndices)
		: Positions(InPositions)
		, Normals(InNormals)
		, Indices(InIndices)
	{
	}

	/** LOD's positions, normals and whole triangle list (every section). */
	explicit FMantleGarmentSurface(const FMantleSkinnedMeshLOD& LOD)
		: Positions(LOD.Positions)
		, Normals(LOD.Normals)
		, Indices(LOD.Indices)
	{
	}
};

/**
 * Which body vertices a FITTED garment covers on its wearer (the ray rule). Computed per wearer at fit time, because the bits of the authoring body miss
 * skin the fit brings under the garment (a heavy belly filling a hem, a thin shoulder the sleeve passes
 * under).
 *
 * One ray per body vertex along its unit normal. The segment starts InsideReachCm under the skin:
 *   - a garment triangle hit between RayStartOffsetCm and RayStartOffsetCm + MaxDistanceCm out along the
 *     normal, either facing, covers the vertex (inner layers count: the inside of a folded collar faces
 *     the neck); a vertex just past a hem casts a ray that misses the garment, so no hem margin is needed;
 *   - otherwise a hit within InsideReachCm UNDER the skin whose triangle faces the skin's way covers it
 *     too (P16: the garment passes under the skin there; the MetaHuman shirt is modelled up to 3.5 cm
 *     inside the shoulder tips of its own authoring body, what MetaHuman's hidden-face mask removes by
 *     hand). A triangle under the skin facing the other way is another part's garment seen through a
 *     thin part (a finger, an ear) and does not count.
 * Garment triangles are bucketed in a uniform grid (RayCellCm); each ray tests the triangles of the cells
 * its segment's bounds overlap (two-sided Moller-Trumbore). Holds no UObjects; may run on any thread.
 */
struct MANTLERUNTIME_API FMantleGarmentCoverage
{
	/** A body vertex is covered when its ray hits the garment within this distance past the ray's start. */
	static constexpr float DefaultMaxDistanceCm = 6.0f;

	/** The outward ray starts this far out along the body normal. */
	static constexpr float RayStartOffsetCm = 0.2f;

	/** P16: the segment also runs this far into the skin (a garment surface there facing the skin's way covers the vertex). */
	static constexpr float InsideReachCm = 4.0f;

	/** Cell of the garment-triangle grid. */
	static constexpr float RayCellCm = 4.0f;

	/** Diagnostic: an uncovered vertex whose ray met the garment within this far under the skin (facing away) counts in MissedTight. */
	static constexpr float TightBandCm = 0.5f;

	/** Diagnostic: a covered vertex whose nearest hit faces away with no second layer within this distance behind it counts in CoveredAcrossGap. */
	static constexpr float LayerGapCm = 1.0f;

	/**
	 * #877: the barycentric edge tolerance of the skin-mask rule's rays (MantleGarmentGeometry::RayHitsTriangle), so a ray
	 * through a corner several garment triangles share never slips between them. About a micrometre on a 1 cm triangle.
	 */
	static constexpr float SkinMaskEdgeTolerance = 1.0e-4f;

	/** Two hits closer than this along a ray are the same surface (a triangle edge shared by two cells' lists). */
	static constexpr float SameSurfaceCm = 0.01f;

	/** One bit per body vertex, set where the garment covers the skin. */
	TBitArray<> Covered;

	/** #877: one bit per body vertex, set where it is covered only from under the skin (P16; the CoveredUnder ones). */
	TBitArray<> Under;
	int32 NumCovered = 0;
	float MaxDistanceCm = DefaultMaxDistanceCm;

	/** Body vertices whose ray covered nothing (MissedTight included). */
	int32 NotHit = 0;

	/** Diagnostic: uncovered, but the garment lies within TightBandCm under the skin .. RayStartOffsetCm over it. */
	int32 MissedTight = 0;

	/** Mean height of the covering hit over the skin, cm (negative for the ones covered from under the skin). */
	double MeanHitCm = 0.0;

	/** Covered vertices whose nearest outward hit faces away from them (inner layers, e.g. the folded collar). */
	int32 CoveredFacingAway = 0;

	/** Of those, the ones with no second layer within LayerGapCm behind the hit. */
	int32 CoveredAcrossGap = 0;

	/** Covered only by a garment surface under the skin (P16). */
	int32 CoveredUnder = 0;

	/** #877: vertices a garment surface under the skin would have covered, refused by the near-half rule (NearShare). */
	int32 UnderFarSide = 0;

	/** Non-degenerate garment triangles and the grid's cells. */
	int32 GarmentTriangles = 0;
	int32 GridCells = 0;
	double Ms = 0.0;

	/**
	 * Fills Out for Garment (the fitted garment, in the wearer's reference pose) over the body
	 * (BodyPositions, BodyNormals: one per body vertex; a zero normal is never covered). False, with
	 * the reason (Out.Covered all false, one bit per body vertex), on mismatched or empty inputs.
	 *
	 * #877, the near-half rule (on with BodyIndices, the whole body surface, and NearShare > 0): a garment surface under
	 * the skin covers the vertex only when it lies less than NearShare of the body's thickness deep, the thickness being
	 * where the ray, going back from the vertex, first meets the body's surface. A sandal's footbed under a toe 1.5 cm
	 * thick is the far side's garment, not this skin's; P16's shirt 3.5 cm inside a shoulder still covers it.
	 *
	 * EdgeTolerance: RayHitsTriangle's, for the garment hits (0, the default, is the exact test the rule before #877 uses).
	 */
	static bool Compute(const FMantleGarmentSurface& Garment, TConstArrayView<FVector3f> BodyPositions, TConstArrayView<FVector3f> BodyNormals,
		FMantleGarmentCoverage& Out, float ReachCm = DefaultMaxDistanceCm, FString* OutError = nullptr,
		TConstArrayView<uint32> BodyIndices = TConstArrayView<uint32>(), float NearShare = 0.0f, float EdgeTolerance = 0.0f);

	/** "covered 1486 of 8568 (under 120, facing away 300, across a gap 12), not hit 7082 (tight 40), mean hit 1.23 cm, 18822 tris, 3.10 ms". */
	FString ToString() const;
};

/** What a welded, fold-limited sink did (FMantleBodySinkApply::Sink). */
struct MANTLERUNTIME_API FMantleSinkApplyStats
{
	/** Halving passes of the fold limiter before the still-folding points are set back to no sink. */
	static constexpr int32 MaxHalvings = 4;

	/** Dot of a triangle's face normals before / after below which it counts as steep (60 degrees). */
	static constexpr float SteepDot = 0.5f;

	/** Distinct positions moved (split copies welded into one point). */
	int32 Points = 0;

	/** Vertices moved with a point without an amount of their own (split copies of a sunk vertex). */
	int32 TwinVertices = 0;

	/** Kept triangles with a moved corner. */
	int32 AffectedTriangles = 0;

	/** Vertices with an amount whose sink the limiter scaled back, and of those the ones set back to none. */
	int32 LimitedVertices = 0;
	int32 ZeroedVertices = 0;

	/** Halving passes that found a folded triangle. */
	int32 HalvingPasses = 0;

	/** The result over AffectedTriangles: folded (0 by construction), steep, largest turn in degrees. */
	int32 Folded = 0;
	int32 Steep = 0;
	float MaxTurnDeg = 0.0f;
};

/**
 * The welded, fold-limited inward move the seam shrink and the body sink share.
 */
struct MANTLERUNTIME_API FMantleBodySinkApply
{
	/**
	 * OutPositions = Base with every vertex of AmountCm[V] > 0 moved inward. Vertices are welded by
	 * position (in Base) into points together with every other vertex at that position (split copies at
	 * UV or normal seams), so the copies move as one and no crack opens; a point sinks along the
	 * normalised sum of its copies' Normals by the largest amount among its copies, times a scale, 1 at
	 * first. A kept triangle (KeptIndices) whose face normal then points against its Base one (or its
	 * Original one, when Original is given: the unmoved mesh when Base was already sunk) is folded: it
	 * halves the scale of its moved points, up to MaxHalvings passes, then the points of triangles still
	 * folded go back to no sink (repeated until nothing folds). Base, Normals and AmountCm (and Original,
	 * when not empty) must be one per vertex; otherwise OutPositions = Base and nothing moves.
	 */
	static void Sink(TConstArrayView<FVector3f> Base, TConstArrayView<FVector3f> Normals, TConstArrayView<uint32> KeptIndices, TConstArrayView<float> AmountCm,
		TArray<FVector3f>& OutPositions, FMantleSinkApplyStats& OutStats, TConstArrayView<FVector3f> Original = TConstArrayView<FVector3f>());

	/** The seam shrink (P3b, P12): every vertex set in Seam (one bit per vertex) by ShrinkCm through Sink. */
	static void ShrinkSeam(TConstArrayView<FVector3f> Base, TConstArrayView<FVector3f> Normals, TConstArrayView<uint32> KeptIndices, const TBitArray<>& Seam, float ShrinkCm,
		TArray<FVector3f>& OutPositions, FMantleSinkApplyStats& OutStats);
};

/** The body sink's tuning. */
struct FMantleBodySinkParams
{
	/** Every rendered skin vertex ends at least this far under the outermost garment surface (0: measure only, the skin-poke check). */
	float UnderCm = 1.0f;

	/** The ray starts this far inside the skin: a garment deeper than that is out of reach. */
	float BackCm = 4.0f;

	/** The largest sink of one vertex. */
	float MaxSinkCm = 4.0f;

	/** Smoothing passes over the sink field (never below a vertex's required sink). */
	int32 SmoothIters = 4;

	/**
	 * #877, the near-half rule (FMantleGarmentCoverage::Compute's, needs the across-body indices): an outermost garment
	 * hit under the skin counts only when it lies less than NearShare of the body's thickness deep. 0: every hit counts.
	 */
	float NearShare = 0.0f;
};

/**
 * The body sink. With transferred weights a tight garment drifts about 1 cm from
 * the skin point it was bound over during animation (the skin's skinning is not affine across a triangle
 * whose corners follow different bones), so every rendered body vertex sinks to UnderCm under the
 * outermost garment surface along its normal.
 *
 * Per rendered vertex (used by a kept triangle) a ray from BackCm inside the skin along its normal,
 * BackCm + UnderCm + ChainReachCm long, meets the garments' triangles; the garment's height t over the
 * skin is that of the OUTERMOST hit whose triangle faces outward (the way the skin does): skin between a
 * closed double surface's inner and outer layer, or under a layer chording through a convex part, is not
 * seen. A ray meeting only inward-facing surfaces gives no sink. The required sink is UnderCm - t where
 * t < UnderCm, clamped to MaxSinkCm. The field is smoothed over the kept triangles' adjacency (welded by
 * position; s' = max(required, 0.5 s + 0.5 neighbour mean)), and applied with FMantleBodySinkApply::Sink.
 *
 * With UnderCm 0 it is the skin-poke measure (P16): Inside counts the rendered vertices outside the
 * outermost garment layer (skin poking through), MaxRequiredCm the deepest. The face is never sunk (P17:
 * a sunk neck is a visible dent); callers pass the body only.
 */
struct MANTLERUNTIME_API FMantleBodySink
{
	/** Past UnderCm the ray keeps looking this far for an outer layer. */
	static constexpr float ChainReachCm = 6.0f;

	/** Cell of the garment-triangle grid the rays walk. */
	static constexpr float CellCm = 4.0f;

	/** Most smoothing passes a caller may ask for. */
	static constexpr int32 MaxSmoothIters = 50;

	/** Rendered body vertices tested ... */
	int32 Tested = 0;

	/** ... whose ray met an outward-facing garment surface ... */
	int32 Hit = 0;

	/** ... lying inside the skin there (t < 0: with UnderCm 0, the skin pokes) ... */
	int32 Inside = 0;

	/** ... and the vertices with a sink after the smoothing. */
	int32 Sunk = 0;

	/** Largest required sink before the smoothing; largest and mean sink after it (over the sunk vertices), cm. */
	float MaxRequiredCm = 0.0f;
	float MaxSinkCm = 0.0f;
	float MeanSinkCm = 0.0f;

	float UnderCm = 0.0f;
	int32 SmoothIters = 0;
	int32 GarmentTriangles = 0;
	double Ms = 0.0;

	/**
	 * With AcrossBodyIndices: vertices whose outermost outward garment hit lay under the skin but
	 * with a body surface between it and the vertex, i.e. another body part's garment seen through a thin part (a
	 * hand hanging by the shorts: the 4 cm backward ray passes through the finger and meets the shorts on the thigh).
	 * Not counted in Hit or Inside, no sink.
	 */
	int32 AcrossBody = 0;

	/** #877: the vertices whose outermost hit the near-half rule (Params.NearShare) refused beyond AcrossBody's. No sink. */
	int32 FarSide = 0;

	/** A body triangle this close to either end of the tested stretch of the ray is the vertex's own skin (or the hit's), not a surface between. */
	static constexpr float AcrossBodyEpsilonCm = 0.05f;

	/**
	 * Fills OutSinkCm (one per body vertex, 0 = not moved) and Out for the body (positions, normals, its
	 * kept triangles) under Garments. False, with the reason (OutSinkCm all 0), on bad inputs; true with
	 * all 0 when there is no garment triangle.
	 *
	 * #877, SinkOnly (one bit per body vertex): only these vertices sink, and the smoothing never spreads the sink onto
	 * the others (welded points with no copy set stay at 0), so skin that is not under cloth never moves. Null: all.
	 */
	static bool Compute(TConstArrayView<FVector3f> BodyPositions, TConstArrayView<FVector3f> BodyNormals, TConstArrayView<uint32> KeptIndices,
		TConstArrayView<FMantleGarmentSurface> Garments, const FMantleBodySinkParams& Params, TArray<float>& OutSinkCm, FMantleBodySink& Out, FString* OutError = nullptr,
		TConstArrayView<uint32> AcrossBodyIndices = TConstArrayView<uint32>(), const TBitArray<>* SinkOnly = nullptr);

	/** "tested 6900 rendered verts against 25258 garment tris: reached 1200 (inside 80), sunk 900 (max 1.85, mean 0.31, required max 1.60, under 1.00, 4 passes), 3.60 ms". */
	FString ToString() const;
};

/** The body side's tuning. */
struct FMantleDressedBodyParams
{
	/** FMantleGarmentCoverage's reach. */
	float CoverageReachCm = FMantleGarmentCoverage::DefaultMaxDistanceCm;

	/** Covered vertices still on a kept triangle (the seam under a hem) sink this far (AVATAR_CATALOG_PLAN.md section 0.1 item 4; 0 = off). */
	float SeamShrinkCm = 0.8f;

	/** The body sink; UnderCm 0 turns it off. */
	FMantleBodySinkParams Sink;

	/**
	 * #877: the near-half rule for both the coverage and the sink (their NearShare; 0 = off). Garment found under the
	 * skin past the middle of a thin part (a sandal's footbed under a toe) is the far side's, so it neither removes nor
	 * sinks this skin.
	 */
	float NearShare = 0.5f;

	/**
	 * #877, Mantle.Avatar.SkinMask: a vertex is hidden only where its garment's source-body mask (FMantleDressedMask)
	 * and this wearer's coverage both hide it; without a mask, where the wearer's coverage hides it at least the mask's
	 * border deep over the wearer's own surface. The sink then moves only skin under cloth on this wearer
	 * (FMantleBodySink's SinkOnly), and the lower LODs keep a wider border (FMantleDressedBody::LowerLODHidden). Off:
	 * the rule before #877 (the masks are ignored).
	 */
	bool bSkinMask = false;

	/** The coverage rays' edge tolerance (FMantleGarmentCoverage::Compute's; the skin-mask rule's is SkinMaskEdgeTolerance). */
	float EdgeTolerance = 0.0f;

	/** The skin-mask rule with its tuning (DESIGN.md section 6): no seam shrink; a sink to 0.5 cm under the cloth, at most 1 cm. */
	static FMantleDressedBodyParams SkinMaskDefaults()
	{
		FMantleDressedBodyParams Params;
		Params.bSkinMask = true;
		Params.EdgeTolerance = FMantleGarmentCoverage::SkinMaskEdgeTolerance;
		Params.SeamShrinkCm = 0.0f;
		Params.Sink.UnderCm = 0.5f;
		Params.Sink.MaxSinkCm = 1.0f;
		return Params;
	}
};

/** #877: one worn garment's source-body mask for FMantleDressedBodyParams::bSkinMask, in the order of the garments. */
struct FMantleDressedMask
{
	/** Null (or not one bit per body vertex): the wearer fallback. */
	const FMantleSkinMask* Mask = nullptr;

	/** The skin kept inside the garment's edges (FMantleSkinMask::DefaultBorderCm; FootwearBorderCm for footwear). */
	float BorderCm = 2.5f;
};

/**
 * The body side of the garment fit (A0.5b), in this order: per-wearer coverage of every worn fitted garment, their union, the
 * triangle filter (a triangle whose three vertices are covered is dropped: FMantleRuntimeSkeletalMeshBuilder's
 * rule), the seam shrink on the covered vertices of kept triangles, then the body sink measured against
 * the fitted garments on the shrunk skin and applied fold-limited against both the shrunk and the original
 * faces. Normals, UVs, weights and everything else of the body stay as they are.
 *
 * The output feeds FMantleRuntimeSkeletalMeshBuilder: the body with LOD0's positions replaced by
 * Positions, built with HiddenVertices = Hidden. Holds no UObjects; may run on any thread.
 *
 * #877, with Params.bSkinMask: per garment, its source mask's hidden bits (at its border) AND its coverage on this
 * wearer, then the union; the seam shrink is off and the sink moves only WearerCovered skin.
 */
struct MANTLERUNTIME_API FMantleDressedBody
{
	/** The union of the garments' coverage: one bit per body vertex (the builder's HiddenVertices). */
	TBitArray<> Hidden;

	/** The body's LOD0 positions after the seam shrink and the sink. */
	TArray<FVector3f> Positions;

	/** The kept triangles (the builder drops the same ones). */
	TArray<uint32> KeptIndices;

	/** Per garment, in the order given. */
	TArray<FMantleGarmentCoverage> Coverage;

	/** #877 (bSkinMask): the union of the garments' coverage on this wearer, one bit per body vertex; the only skin the sink moves. */
	TBitArray<> WearerCovered;

	/**
	 * #877 (bSkinMask): per hidden body vertex, how far past its border it lies, in mm (the largest over the garments
	 * hiding it); 0 elsewhere. Empty without bSkinMask. LowerLODHidden's input.
	 */
	TArray<uint8> SlackMm;

	/** #877 (bSkinMask): vertices a garment's mask (or its wearer fallback) hides before the AND, the ones this wearer's coverage kept, and the garments without a mask. */
	int32 MaskHidden = 0;
	int32 MaskKeptByWearer = 0;
	int32 FallbackGarments = 0;

	int32 CoveredVertices = 0;
	int32 KeptTriangles = 0;
	int32 DroppedTriangles = 0;

	/** Covered vertices still used by a kept triangle: the seam ring the shrink moves. */
	int32 SeamVertices = 0;

	FMantleSinkApplyStats Seam;
	FMantleBodySink Sink;
	FMantleSinkApplyStats SinkApplied;

	/** Coverage of every garment and the union; the triangle filter, the seam shrink and the sink; everything. */
	double CoverageMs = 0.0;
	double SinkMs = 0.0;
	double TotalMs = 0.0;

	/**
	 * Fills Out for Body (a reference-topology body LOD: positions, normals, indices) under Garments (the
	 * fitted garments in the wearer's reference pose, the same space as Body). No garment: nothing hidden,
	 * nothing moved. False, with the reason, on bad inputs or when a garment's coverage fails.
	 */
	static bool Compute(const FMantleSkinnedMeshLOD& Body, TConstArrayView<FMantleGarmentSurface> Garments, const FMantleDressedBodyParams& Params,
		FMantleDressedBody& Out, FString* OutError = nullptr, TConstArrayView<FMantleDressedMask> Masks = TConstArrayView<FMantleDressedMask>());

	/**
	 * #877: the hidden bits of WearerBody's lower LODs (entry 0 is LOD1; FMantleSkinnedMeshBuildParams::LowerLODHiddenVertices).
	 * A vertex of LOD k hides when its nearest LOD0 vertex (TransferHiddenVertices) is hidden at least half of LOD k's
	 * mean edge length past its border (SlackMm), so the longer triangles of a coarse LOD never reach an edge. Out
	 * empty (the builder's own transfer) when Dressed has no SlackMm or WearerBody one LOD. False with the reason.
	 */
	static bool LowerLODHidden(const FMantleSkinnedMesh& WearerBody, const FMantleDressedBody& Dressed, TArray<TBitArray<>>& Out, FString* OutError = nullptr);

	/** "covered 2100 of 8568, dropped 3300 of 15196 tris, seam 160 verts (limited 2, zeroed 0), sink: <FMantleBodySink::ToString>". */
	FString ToString() const;

	/**
	 * WearerBody (every LOD, A3.8) with LOD0's positions replaced by Dressed's (the seam shrink and the sink) when the
	 * counts agree: what the builder builds with HiddenVertices = Dressed.Hidden (LOD0's bits; the builder carries
	 * them to the lower LODs). Any thread.
	 */
	static FMantleSkinnedMesh MakeDressedMesh(const FMantleSkinnedMesh& WearerBody, const FMantleDressedBody& Dressed);
};
