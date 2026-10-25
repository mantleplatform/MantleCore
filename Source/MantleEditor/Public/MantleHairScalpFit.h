// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * Median-split k-d tree over a point set (double precision). Built once, queried many times. Pure: no UObjects.
 * The scalp fit uses one over the target head's vertices (nearest-vertex lookups) and one over the head vertices
 * that have hair nearby (the Gaussian radius query).
 */
class MANTLEEDITOR_API FMantleScalpKdTree
{
public:
	void Build(const TArray<FVector>& InPoints);
	int32 Num() const { return Points.Num(); }
	const FVector& Point(int32 Id) const { return Points[Id]; }

	/** Up to K nearest points, nearest first: their ids and distances. */
	void Nearest(const FVector& P, int32 K, TArray<int32>& OutIds, TArray<double>& OutDist) const;

	/** Every point within Radius (inclusive), unordered. OutIds is reset. */
	void InRadius(const FVector& P, double Radius, TArray<int32>& OutIds) const;

private:
	void BuildRange(int32 Lo, int32 Hi, int32 Axis);
	void NearestRange(int32 Lo, int32 Hi, int32 Axis, const FVector& P, int32 K, TArray<TPair<double, int32>>& Best) const;
	void RadiusRange(int32 Lo, int32 Hi, int32 Axis, const FVector& P, double R2, TArray<int32>& OutIds) const;

	TArray<FVector> Points;
	/** Implicit tree: the median of each [Lo, Hi) range sits at its middle. */
	TArray<int32> Order;
};

/**
 * The target head's skin as a query surface (#766 stage 2): the exact signed distance from any point, with the closest
 * point and the interpolated vertex normal there. Triangles come from a bounding-volume tree, so the answer is exact
 * (the prototype picked candidate triangles from surface samples with a k-d tree and then measured exactly).
 *
 * Space is the caller's, in centimetres. The winding is checked once: if the triangles in the top quarter of the mesh
 * face inward, every triangle is flipped, so "outside" is always the side the skin faces. Vertex normals are the sum
 * of the unit face normals around each vertex, as in the prototype; vertices split at a UV seam keep their own.
 */
class MANTLEEDITOR_API FMantleScalpHeadSurface
{
public:
	struct FHit
	{
		/** Positive outside the skin, negative inside, centimetres. */
		double SignedDistance = 0.0;
		FVector Point = FVector::ZeroVector;
		FVector Normal = FVector::UpVector;
	};

	bool Build(TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Indices, FString& OutError);
	bool IsValid() const { return Triangles.Num() > 0; }

	FHit Query(const FVector& P) const;

	int32 NumVertices() const { return Vertices.Num(); }
	const FVector& Vertex(int32 Id) const { return Vertices[Id]; }
	const FVector& VertexNormal(int32 Id) const { return VertexNormals[Id]; }
	const FMantleScalpKdTree& VertexTree() const { return VertexKd; }

private:
	struct FNode
	{
		FBox Box = FBox(ForceInit);
		int32 Left = INDEX_NONE;
		int32 Right = INDEX_NONE;
		int32 First = 0;
		int32 Count = 0;
	};

	int32 BuildNode(int32 First, int32 Count);
	void QueryNode(int32 NodeIndex, const FVector& P, double& BestD2, int32& BestTri, double& BestV, double& BestW, FVector& BestPoint) const;

	TArray<FVector> Vertices;
	TArray<FVector> VertexNormals;
	TArray<FIntVector> Triangles;
	TArray<FVector> TriangleCentroids;
	TArray<int32> TriOrder;
	TArray<FNode> Nodes;
	FMantleScalpKdTree VertexKd;
};

/**
 * #766 stage 2: the upload-time hair fit that runs right after the whole-piece fit (scale + offset onto the MetaHuman
 * head). Ported from the proven prototype (hair-trial pack_field.py, pack_snap.py, pack_combo.py). Bought hair is made
 * for a different head, so the whole-piece fit leaves some of it sunk into the skin and some of it floating. Per-card
 * root pinning was tried and rejected (it collapses volume-style packs), so:
 *
 *  - Cards get a smoothed scalp offset field. On the head surface, push where hair is closer to the skin than the
 *    minimum clearance (1 mm; 2 mm when the pack has a cap, as cards ride on the cap) and pull in only floating roots
 *    (no-cap packs: card roots; cap packs: the cap's open edge) more than 5 mm off, to 3 mm. The field is smoothed with
 *    a normalised Gaussian (sigma 1.5 cm) over head vertices with hair nearby, and each card vertex moves along the head
 *    normal by the field interpolated over its 4 nearest head vertices. Volume-style packs therefore keep their layering.
 *  - A solid cap gets only a lower-edge wrap: its open-boundary vertices snap to the closest scalp point + 1 mm (capped
 *    at 3 cm per axis), the displacement spreads inward with a cosine falloff over 3 cm of surface distance, and any
 *    cap vertex within 0.5 mm of the skin lifts to 1 mm.
 *  - Last resort: any card vertex still more than 1 mm inside the skin moves out along the normal to the clearance.
 *  - Neither the push nor the last resort touches hair more than HeadDepthCm below the head's top (#51): long hair over
 *    the shoulders would be shoved out of the head mesh's collar, which is not the wearer's body.
 *
 * "Cap" versus "cards" is the pack's own structure, as in the prototype: the caller passes the vendor's solid cap piece
 * as Cap, and everything else as Cards. Positions are the whole-piece-fitted ones, in the head's space (cm). Pure.
 */
struct MANTLEEDITOR_API FMantleScalpFit
{
	/** Gaussian sigma of the offset field, cm. */
	static constexpr double SigmaCm = 1.5;
	/** Roots (or the cap's open edge) farther off the skin than this are pulled in, cm. */
	static constexpr double FloatToleranceCm = 0.5;
	/** ... to this distance, cm. */
	static constexpr double PullToCm = 0.3;
	/** Minimum skin clearance for hair, cm; cards over a cap use CardOverCapClearanceCm. */
	static constexpr double ClearanceCm = 0.10;
	static constexpr double CardOverCapClearanceCm = 0.20;
	/** A vertex whose signed distance is below this counts as inside the head (and is lifted), cm. */
	static constexpr double SinkCm = -0.10;
	/** The cap's open edge sits this far above the skin, cm. */
	static constexpr double CapEdgeOffsetCm = 0.10;
	/** Per axis cap on the edge snap, and the falloff length of the wrap, cm. */
	static constexpr double CapWrapCm = 3.0;
	/** Cap vertices closer to the skin than this are lifted to CapEdgeOffsetCm, cm. */
	static constexpr double CapLiftBelowCm = 0.05;
	/** Card vertices lower than this below the head's top (over the neck and shoulders) are neither pushed nor lifted, cm. */
	static constexpr double HeadDepthCm = 18.0;
	/** A card's root end is the 12% of its length nearer the head. */
	static constexpr double CardEndFraction = 0.12;

	/** The creator is asked to check the fit when the last-resort lift touched more than this share of the card vertices on the head ... */
	static constexpr double CheckFitLiftShare = 0.05;
	/** ... or moved a vertex farther than this, cm. */
	static constexpr double CheckFitLiftCm = 3.0;

	struct FMesh
	{
		TConstArrayView<FVector3f> Positions;
		/** A triangle list. */
		TConstArrayView<uint32> Indices;
	};

	struct FMetrics
	{
		int32 CardVertices = 0;
		int32 CardCount = 0;
		int32 CapVertices = 0;
		/** Largest push and pull of the smoothed field, cm (pull is negative). */
		double MaxPushCm = 0.0;
		double MaxPullCm = 0.0;
		double FieldShiftMedianCm = 0.0;
		double FieldShiftMaxCm = 0.0;
		/** Last-resort lift: card vertices still more than 1 mm inside after the field, and the largest lift, cm. */
		int32 CardLifted = 0;
		double CardLiftMaxCm = 0.0;
		int32 CardInsideBefore = 0;
		int32 CardInsideAfter = 0;
		/** Roots within 3 mm and 5 mm of the skin, percent, before and after (roots: card roots, or the cap's open edge). */
		double RootsWithin3mmBefore = 0.0;
		double RootsWithin5mmBefore = 0.0;
		double RootsWithin3mmAfter = 0.0;
		double RootsWithin5mmAfter = 0.0;
		double RootMedianMmAfter = 0.0;
		/** Cap: open-edge vertices, how many hit the 3 cm cap, vertices lifted off the skin by the final pass. */
		int32 CapEdgeVertices = 0;
		int32 CapEdgeClamped = 0;
		int32 CapLifted = 0;
		int32 CapInsideAfter = 0;
	};

	struct FResult
	{
		TArray<FVector3f> CardPositions;
		/** Empty without a cap. */
		TArray<FVector3f> CapPositions;
		FMetrics Metrics;
		/** The preview should ask the creator to check the fit. */
		bool bCheckFit = false;
		/** Plain words for the preview when bCheckFit; empty otherwise. */
		FString CheckFitNote;
	};

	/** Cap may be null (a cards-only pack). Fails, with a sentence, on bad meshes or an empty head. */
	static bool Run(const FMantleScalpHeadSurface& Head, const FMesh& Cards, const FMesh* Cap, FResult& Out, FString& Error);

	/**
	 * One vertex per card (the pivot, as the prototype's snap_cards finds it): cards are the connected pieces of the mesh
	 * (vertices at one position are one); the root end is the end of the card's long axis nearer the head, and the pivot
	 * is that end's vertex nearest the skin. Distances are signed distances per vertex. Cards under four vertices are skipped.
	 */
	static TArray<int32> FindCardRoots(const TArray<FVector>& Positions, TConstArrayView<uint32> Indices, const TArray<double>& Distances);

	/**
	 * Both ends of every card (cards as FindCardRoots finds them), two positions per card: the vertices at the two ends of its
	 * long axis. Which end is the root depends on where the card sits, so the whole-piece fit (FMantleHairPieceFit) takes the
	 * end nearer the head for each candidate. Empty when a triangle names a missing vertex.
	 */
	static TArray<FVector3f> FindCardEnds(TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Indices);

	/**
	 * Normalised Gaussian over the head vertices flagged in Mask: Out[i] = sum(w * Value * mask) / sum(w * mask) with
	 * w = exp(-0.5 (d/Sigma)^2) within 3 sigma, and the vertex's own weight doubled (the prototype added an identity
	 * matrix to the distance matrix, which already counted the vertex itself). 0 where no masked vertex is near. A
	 * constant field stays constant. Only vertices flagged in Wanted are computed (empty: all); the rest are 0.
	 */
	static void SmoothField(const FMantleScalpHeadSurface& Head, const TArray<double>& Value, const TArray<uint8>& Mask,
		double Sigma, const TArray<uint8>& Wanted, TArray<double>& Out);

	/** Moves every flagged vertex that is more than 1 mm inside the skin out along the normal to Clearance. Returns how many moved. */
	static int32 LiftSunk(const FMantleScalpHeadSurface& Head, TArray<FVector>& Positions, const TArray<uint8>& Used,
		double Clearance, double& OutMaxLift);

	/** The cap's lower-edge wrap (see the type comment). Positions per input vertex; OutPositions is reset. */
	static bool WrapCapEdge(const FMantleScalpHeadSurface& Head, const TArray<FVector>& Positions, TConstArrayView<uint32> Indices,
		TArray<FVector>& OutPositions, FMetrics& Metrics, TArray<int32>* OutEdgeVertices = nullptr);
};
