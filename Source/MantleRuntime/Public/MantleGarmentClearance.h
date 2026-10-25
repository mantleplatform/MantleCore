// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleSkinnedMesh.h"

/**
 * A garment's clearance from a surface (a body or a face). For every
 * garment vertex within SearchCm of the surface: the closest surface triangle, the barycentrics of the
 * closest point on it, and the signed distance d0 = dot(garment - closest, triangle normal) (negative:
 * the garment dips into the surface there, as the folded collar's inner layer does in the neck).
 *
 * Measured once over the body a garment was authored on, it is the authored clearance: every wearer
 * with the reference topology shares the triangle list, so the same triangle and barycentrics give the
 * corresponding point on the wearer (Measure), and Restore pushes a fitted vertex back out where the
 * wearer's clearance fell below the authored one. Measured over a wearer's face it is the collar's
 * clearance on that face (faces differ in vertices, so the face restore tracks the true nearest face
 * triangle per wearer and takes d0 from the authored face clearance per garment vertex).
 *
 * Holds no UObjects; may run on any thread.
 */
struct MANTLERUNTIME_API FMantleGarmentClearance
{
	/** Garment vertices with no surface within this distance are not tracked (loose cloth the warp alone places). */
	static constexpr float DefaultSearchCm = 10.0f;

	/** A fitted vertex is pushed when its clearance fell more than this below the authored one. */
	static constexpr float DefaultSlackCm = 0.2f;

	/** Outer-layer vertices authored at or just above the skin are kept at least this far off it. */
	static constexpr float MinKeptClearanceCm = 0.4f;

	/** The face restore's reach: garment vertices within this of a face are its (the collar and the neck opening). */
	static constexpr float FaceSearchCm = 6.0f;

	/** Restore: smoothing passes over the push field (scalars, then vectors, P15 and P17e) and its upper clamp. */
	static constexpr int32 DefaultSmoothIters = 4;
	static constexpr int32 MaxSmoothIters = 50;

	/** Restore pushes along the garment's own normal unless it dots below this with the surface triangle's normal. */
	static constexpr float NormalMinDot = 0.3f;

	/** Restore's flip limiter: passes that halve the push of a flipped triangle's points before they are zeroed. */
	static constexpr int32 MaxHalvings = 4;

	/** The surface's triangle list (absolute vertex indices; every compatible surface has the same vertex order). */
	TArray<uint32> SurfaceIndices;

	/** Per surface triangle, +1 or -1: the orientation of cross(B - A, C - A) that agrees with the surface's vertex normals. */
	TArray<int8> TriangleSign;

	int32 NumSurfaceVertices = 0;

	/** Per garment vertex: the closest surface triangle, INDEX_NONE when none lies within the search (not tracked). */
	TArray<int32> Triangle;

	/** Per garment vertex: barycentrics (weights of the triangle's corners 0, 1, 2) of the closest point. */
	TArray<FVector3f> Barycentrics;

	/** Per garment vertex: signed clearance in cm (0 when not tracked). */
	TArray<float> ClearanceCm;

	int32 NumTracked = 0;
	float MinClearanceCm = 0.0f;
	float MaxClearanceCm = 0.0f;
	float MeanClearanceCm = 0.0f;
	double Ms = 0.0;

	bool IsTracked(const int32 Vertex) const { return Triangle.IsValidIndex(Vertex) && Triangle[Vertex] != INDEX_NONE; }

	/** Stops tracking Vertex (no-op when it is not tracked). */
	void Untrack(int32 Vertex);

	/**
	 * Fills Out for a garment at GarmentPositions over Surface (positions, normals, indices; the normals
	 * orient the triangles). Surface triangles are bucketed in a uniform grid and searched ring by ring out
	 * to SearchCm. False, with the reason in OutError, on bad inputs.
	 */
	static bool Compute(TConstArrayView<FVector3f> GarmentPositions, const FMantleSkinnedMeshLOD& Surface, FMantleGarmentClearance& Out,
		float SearchCm = DefaultSearchCm, FString* OutError = nullptr);

	/**
	 * Signed clearance of garment vertex Vertex at P over another surface with the same vertex order
	 * (SurfacePositions): the point of the vertex's triangle at its barycentrics there, and that triangle's
	 * oriented unit normal. False when the vertex is not tracked, the count differs or the triangle is degenerate.
	 */
	bool Measure(int32 Vertex, const FVector3f& P, TConstArrayView<FVector3f> SurfacePositions, float& OutClearanceCm, FVector3f& OutNormal) const;

	/**
	 * P16: untracks every garment vertex whose closest point lies on an open boundary edge of Surface (the
	 * face ends in an open ring at the neck base: a shoulder vertex nearest to it is not over the face). A
	 * closest point is on edge (E, E + 1) when the opposite corner's barycentric is below Tolerance. Surface
	 * must be the one Compute ran on. Returns the number untracked.
	 */
	int32 UntrackBoundary(const FMantleSkinnedMeshLOD& Surface, float Tolerance = 0.02f);
};

/** What one clearance restore did. */
struct MANTLERUNTIME_API FMantleClearanceRestoreStats
{
	bool bRan = false;
	int32 Tracked = 0;

	/** Vertices whose clearance sat below d0 - slack (needed a push). */
	int32 Below = 0;

	/** Vertices whose push was raised to their layer twin's (P17b: the double surface moves as one). */
	int32 Coupled = 0;

	int32 SmoothIters = 0;
	int32 Points = 0;

	/** #636: points (welded within their sections) joined into one at a seam between sections, and pushed as one. */
	int32 SeamJoined = 0;

	/** Vertices moved in the end, and how: along the garment's normal, along the surface's, untracked ones the smoothing reached. */
	int32 Moved = 0;
	int32 AlongGarment = 0;
	int32 AlongSurface = 0;
	int32 UntrackedMoved = 0;

	/** Flip limiter: triangles the full push flipped, halving passes, vertices whose push was reduced / zeroed, triangles still flipped (0 expected). */
	int32 FlippedBefore = 0;
	int32 HalvingPasses = 0;
	int32 Limited = 0;
	int32 Zeroed = 0;
	int32 FlippedLeft = 0;

	/** Largest and mean push (clearance gained along the surface normal, over moved vertices) and the largest move, cm. */
	double MaxPushCm = 0.0;
	double MeanPushCm = 0.0;
	double MaxMoveCm = 0.0;
	double Ms = 0.0;

	FString ToString() const;
};

/**
 * The layers of a closed double-surface garment (P17b): for every vertex, the nearest vertex within
 * TwinReachCm whose normal opposes its own (dot below OpposeDot) is its twin on the other layer.
 */
struct MANTLERUNTIME_API FMantleLayerCrossing
{
	static constexpr float TwinReachCm = 1.0f;
	static constexpr float OpposeDot = -0.5f;

	/** Each vertex's twin on the other layer (INDEX_NONE when none within TwinReachCm). */
	static void FindTwins(const FMantleSkinnedMeshLOD& Garment, TArray<int32>& OutTwin);
};

/**
 * Triangle orientation of a garment stage against the authored garment (P14): a triangle whose face
 * normal dots negative against the authored triangle's is flipped (turned over; its inside shows); one
 * whose area fell below CollapsedAreaRatio of the authored is collapsed. Degenerate authored triangles
 * are skipped.
 */
struct MANTLERUNTIME_API FMantleFaceFlips
{
	static constexpr double CollapsedAreaRatio = 0.1;

	bool bMeasured = false;
	int32 Triangles = 0;
	int32 Degenerate = 0;
	int32 Flipped = 0;
	int32 Collapsed = 0;

	/** One bit per triangle, set where it is flipped. */
	TBitArray<> FlippedBits;

	/** Stage against Authored (same vertex count and triangle list, else false and Out empty). */
	static bool Measure(const FMantleSkinnedMeshLOD& Authored, const FMantleSkinnedMeshLOD& Stage, FMantleFaceFlips& Out);

	/** Triangles flipped in After that were not flipped in Before (both measured against the same authored garment). */
	static int32 CountAdded(const FMantleFaceFlips& Before, const FMantleFaceFlips& After);
};

/** The clearance restore. */
struct MANTLERUNTIME_API FMantleClearanceRestore
{
	/**
	 * Pushes the fitted garment back out to its target clearance. For every vertex Target tracks, the
	 * signed clearance d1 over Surface (the wearer's body or face, Target's vertex order) is measured at
	 * Target's correspondence (Measure); the required push is p = d0 - d1 where d1 < d0 - SlackCm, else 0.
	 * d0 is Target.ClearanceCm, floored at MinKeptCm for outer-layer vertices (facing away from the surface)
	 * authored between -0.3 cm and MinKeptCm (P17b: an inner layer lying on the skin keeps its clearance, or
	 * it is pushed through the outer one); MinKeptCm <= 0 floors nothing. Then, as P15 to P17e:
	 *   - a vertex and its layer twin take the larger push (FMantleLayerCrossing::FindTwins);
	 *   - vertices are welded by position within their section (split copies move together);
	 *   - the push field is smoothed SmoothIters passes over the points' edge adjacency, never below a
	 *     point's own requirement: p' = max(required, 0.5 p + 0.5 neighbour mean);
	 *   - direction: the point's own normal, scaled 1 / dot so the clearance along the surface normal grows
	 *     by p; the surface triangle's normal where the two dot below NormalMinDot; untracked points the
	 *     smoothing reached borrow their neighbours' surface normal;
	 *   - the push vectors are smoothed as many passes (P17e), each kept at least its scalar length;
	 *   - #636, when Seams is given (per vertex a seam id, INDEX_NONE for none: copies of one position in
	 *     different sections share an id): the points of one seam become one point, pushed along one
	 *     shared direction (the mean of their pushes) by the largest length any of them needs, each
	 *     point's push gain along its surface normal divided by that normal's cosine with the shared
	 *     direction, so no copy gains less clearance than its own push gave it; where a cosine falls below
	 *     NormalMinDot the shared direction is the copies' mean surface normal (cosines floored there);
	 *   - flip limiter: a triangle turned over by the push (against its pre-push face, and against
	 *     Authored's where the pre-push face still agrees with it) halves its points' push, up to
	 *     MaxHalvings passes; then the points of any triangle still flipped get no push at all.
	 * Vertices are never pulled inward; normals, tangents and everything else stay. False (Garment
	 * unchanged), with the reason in OutError, when the sizes do not match (Seams: empty or one per vertex).
	 */
	static bool Restore(FMantleSkinnedMeshLOD& Garment, const FMantleGarmentClearance& Target, TConstArrayView<FVector3f> Surface,
		const FMantleSkinnedMeshLOD* Authored, float SlackCm, float MinKeptCm, int32 SmoothIters,
		FMantleClearanceRestoreStats* OutStats = nullptr, FString* OutError = nullptr, TConstArrayView<int32> Seams = TConstArrayView<int32>());
};
