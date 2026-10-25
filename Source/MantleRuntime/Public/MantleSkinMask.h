// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleBodyCoverage.h"

/** FMantleSkinMask::Derive's tuning (#877, D:\Mantle\Untracked\skin-hide-1022\DESIGN.md section 5). */
struct FMantleSkinMaskParams
{
	/** Rays per body vertex: its normal plus Rays - 1 spread over the outer hemisphere (a cosine-weighted Fibonacci set). */
	int32 Rays = 32;

	/** Every ray starts this far out along the vertex's normal (FMantleGarmentCoverage::RayStartOffsetCm). */
	float RayStartCm = FMantleGarmentCoverage::RayStartOffsetCm;

	/** A ray that meets neither garment nor body within this far escapes. */
	float RayReachCm = 20.0f;

	/** The normal ray's reach: FMantleGarmentCoverage's, with its P16 under-skin rule and the #879 near-half rule. */
	float NormalReachCm = FMantleGarmentCoverage::DefaultMaxDistanceCm;

	/** Rays that may escape while the vertex still counts as enclosed (numerical noise at grazing angles). */
	int32 EscapeSlack = 1;

	/** The near-half rule's share (FMantleDressedBodyParams::NearShare). */
	float NearShare = 0.5f;

	/** Hidden's border (FMantleSkinMask::Hides takes any other). */
	float BorderCm = 2.5f;
};

/**
 * #877: which skin of a garment's own source body the garment hides, decided where the fit is exact. Every
 * garment-compatible body shares one vertex order (schema/avatar/body-topology-v1.json), so these per-vertex bits
 * carry to every wearer; FMantleDressedBody ANDs them with the wearer's own coverage (the safety net).
 *
 * 1. Enclosure. A body vertex is enclosed when its normal ray is covered (FMantleGarmentCoverage: the garment within
 *    NormalReachCm over the skin, or under the skin by P16) and, of Rays - 1 more rays over the outer hemisphere, at
 *    most EscapeSlack meet neither a garment triangle (either side) nor the body itself (armpits, between the legs)
 *    within RayReachCm. Skin beside a strap, inside a neckline, armhole or cutout, or under a loose hem sees out
 *    sideways and is kept. A vertex covered only from under the skin (P16: the garment passes inside a shoulder tip)
 *    counts as enclosed: it stands outside the garment, so removing it shows the garment, never a hole.
 * 2. Border. DepthMm is each vertex's straight distance on the source body to the nearest vertex that is not
 *    enclosed, that vertex found over the surface (SurfaceDepthMm), in mm, 255 for 25.5 cm or more (or none
 *    reachable). A vertex is hidden when its depth is at least the border: skin stays BorderCm deep inside every edge.
 *
 * Normals are the source body's own (area-weighted face normals, welded by position), oriented by HintNormals (the
 * wearer's: the shared winding decides, as in FMantleCatalogGarmentFit::DeriveAuthoredClearance). Deterministic: the
 * same inputs give the same bits on every machine and run. Holds no UObjects; may run on any thread.
 */
struct MANTLERUNTIME_API FMantleSkinMask
{
	/** Bumped whenever the rule changes what it hides. */
	static constexpr uint32 RuleVersion = 1;

	/** The border every garment keeps inside its edges, and footwear's (a toe is about 1.5 cm thick; the near-half rule guards it). */
	static constexpr float DefaultBorderCm = 2.5f;
	static constexpr float FootwearBorderCm = 1.5f;

	/** DepthMm of a vertex 25.5 cm or more from visible skin, or with none reachable. */
	static constexpr uint8 FarDepthMm = 255;

	/** One bit per body vertex: enclosed (step 1). */
	TBitArray<> Enclosed;

	/** One bit per body vertex: hidden at BorderCm (Hides(Vertex, BorderCm)). */
	TBitArray<> Hidden;

	/** Per body vertex, mm to the nearest vertex that is not enclosed (0 for those; SurfaceDepthMm). */
	TArray<uint8> DepthMm;

	float BorderCm = DefaultBorderCm;

	/** FMantleBodyShape::PositionsHash of the source body (0 when the caller did not give it). */
	uint32 SourcePositionsHash = 0;

	/** Vertices whose normal ray was covered, of those the ones covered from under the skin (P16), enclosed, hidden. */
	int32 NormalCovered = 0;
	int32 CoveredUnder = 0;
	int32 NumEnclosed = 0;
	int32 NumHidden = 0;

	/** Hemisphere rays cast (a vertex stops casting once more than EscapeSlack escaped). */
	int64 RaysCast = 0;
	double Ms = 0.0;

	int32 Num() const { return DepthMm.Num(); }

	/** The border in DepthMm's unit: a vertex hides at DepthMm >= BorderMm(BorderCm). */
	static uint8 BorderMm(const float InBorderCm) { return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32(InBorderCm * 10.0f), 1, 255)); }

	/** Whether Vertex hides with BorderCm of skin kept inside the garment's edges. */
	bool Hides(const int32 Vertex, const float InBorderCm) const { return DepthMm.IsValidIndex(Vertex) && DepthMm[Vertex] >= BorderMm(InBorderCm); }

	/**
	 * Fills Out for Garment (its LOD0 on its own source body, every triangle) over the source body: SourcePositions
	 * over TopologyIndices (the reference topology's LOD0 triangles, the whole body), HintNormals one per vertex (any
	 * body of that topology: they only orient the triangles). False, with the reason, on mismatched or empty inputs.
	 */
	static bool Derive(const FMantleGarmentSurface& Garment, TConstArrayView<FVector3f> SourcePositions, TConstArrayView<FVector3f> HintNormals,
		TConstArrayView<uint32> TopologyIndices, const FMantleSkinMaskParams& Params, FMantleSkinMask& Out, FString* OutError = nullptr);

	/**
	 * The border step on its own: per vertex of Positions, the straight distance to the nearest vertex NOT set in Inner,
	 * in mm (FarDepthMm past 25.5 cm or with none reachable; 0 for vertices outside Inner). The nearest is found by a
	 * Dijkstra over the welded edge graph of Indices that carries each point's nearest such vertex, so only skin joined
	 * over the surface counts, and the depth never reads deeper than it is (an edge path would, up to 1.4 times across a
	 * grid's diagonals). Also FMantleDressedBody's fallback on the wearer's own body.
	 */
	static void SurfaceDepthMm(TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Indices, const TBitArray<>& Inner, TArray<uint8>& OutDepthMm);

	/** "enclosed 1800 of 8568 (normal-covered 2400, under 30), hidden 1100 at 2.5 cm, 52000 rays, 95.10 ms, rule 1". */
	FString ToString() const;
};

/**
 * #877's see-through audit (DESIGN.md section 8): looks at the body as built, every LOD, for holes. From each of
 * Directions camera directions spread over the sphere, a ray runs from far outside to the centre of every DROPPED body
 * triangle (in Body.Indices but not in KeptIndices). The ray is blocked by any garment triangle or kept body
 * triangle before it gets there (either side). It is also fine when it meets a garment within UnderReachCm past the
 * centre (P16: the garment lies just under that skin, so it shows the garment). Any other ray is a see-through hole:
 * the camera looks through missing skin into the body. A dropped triangle every one of whose rays is blocked is
 * invisible. Cameras within MinGrazingDeg of a dropped triangle's plane are not asked: along it the missing triangle
 * is a sliver no pixel shows. Test utility; any thread.
 */
struct MANTLERUNTIME_API FMantleSeeThroughAudit
{
	static constexpr int32 DefaultDirections = 128;
	static constexpr float UnderReachCm = FMantleGarmentCoverage::InsideReachCm;
	static constexpr float MinGrazingDeg = 10.0f;

	int32 DroppedTriangles = 0;
	int32 RaysCast = 0;

	/** Rays that saw through, and the dropped triangles with at least one such ray. */
	int32 HoleRays = 0;
	int32 HoleTriangles = 0;

	/** Up to MaxExamples of the holes: the dropped triangle (its index in Body.Indices / 3) and the camera direction. */
	static constexpr int32 MaxExamples = 8;
	TArray<TPair<int32, FVector3f>> Examples;

	double Ms = 0.0;

	/**
	 * Audits Body's positions with KeptIndices (what the built LOD draws: a subset of Body's triangles) under Garments
	 * (in the same space). False, with the reason, on bad inputs.
	 */
	static bool Run(const FMantleSkinnedMeshLOD& Body, TConstArrayView<uint32> KeptIndices, TConstArrayView<FMantleGarmentSurface> Garments,
		FMantleSeeThroughAudit& Out, int32 Directions = DefaultDirections, FString* OutError = nullptr);

	/** "dropped 3300 tris, 422400 rays: 0 see-through (0 tris), 812.00 ms". */
	FString ToString() const;
};
