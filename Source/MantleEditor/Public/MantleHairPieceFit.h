// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * The target head's skin as a cloud of oriented surface samples (#766 stage 1). Ported from the prototype's head_samples
 * (hair-trial pack_prep.py): random points on every triangle, each with its triangle's normal, then all normals flipped
 * once if the top quarter of the head faces inward. The whole-piece fit scores thousands of candidates against it, so
 * the query is a nearest sample (a k-d tree), not the exact distance the stage-2 head surface gives; a sample is never
 * farther than a fraction of a millimetre from the true surface, which is far below the fit's 1 cm grid.
 *
 * Space is the caller's, in centimetres. Pure: no UObjects.
 */
class MANTLEEDITOR_API FMantleHairHeadSamples
{
public:
	/** Samples per triangle at most; fewer when the head has more than about 20,000 triangles. */
	static constexpr int32 MaxSamplesPerTriangle = 12;

	bool Build(TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Indices, FString& OutError);
	bool IsValid() const { return Points.Num() > 0; }
	int32 Num() const { return Points.Num(); }

	/** The highest vertex of the head, cm. The whole-piece fit's height offsets are measured from it. */
	double TopCm() const { return Top; }

	/**
	 * #51: the scalp, where hair roots belong: samples within ScalpTopCm of the top, or facing back (normal Y below
	 * -0.3) within ScalpBackCm of the top (down to the nape), but never the face (facing forward, normal Y above 0.3,
	 * more than FaceStartCm below the top). +Y is forward, as everywhere in the fit. Up to MaxScalpPoints of the scalp
	 * samples (an even stride) are the points the fit measures the roots' coverage on: a scalp point is covered when a
	 * seated root is within CoverReachCm of it.
	 */
	static constexpr double ScalpTopCm = 10.0;
	static constexpr double ScalpBackCm = 15.0;
	static constexpr double FaceStartCm = 5.0;
	static constexpr int32 MaxScalpPoints = 800;
	static constexpr double CoverReachCm = 2.0;
	/** The scalp point nearest the sample, or INDEX_NONE when the sample is not on the scalp. */
	int32 NearestScalpPoint(int32 Id) const { return ScalpOf[Id]; }
	int32 NumScalpPoints() const { return ScalpPoints.Num(); }
	const FVector& ScalpPoint(int32 Index) const { return ScalpPoints[Index]; }
	/** The scalp points within twice CoverReachCm of a scalp point: every point a root seated near it can cover. */
	TConstArrayView<int32> ScalpNeighbours(int32 Index) const
	{
		return MakeArrayView(ScalpNeighbourIds.GetData() + ScalpNeighbourStart[Index], ScalpNeighbourStart[Index + 1] - ScalpNeighbourStart[Index]);
	}

	/** The nearest sample to P: its id and the squared distance. */
	void Nearest(const FVector& P, int32& OutId, double& OutDistSquared) const;
	const FVector& Point(int32 Id) const { return Points[Id]; }
	/** Unit, pointing out of the skin. */
	const FVector& Normal(int32 Id) const { return Normals[Id]; }

private:
	void BuildRange(int32 Lo, int32 Hi, int32 Axis);
	void NearestRange(int32 Lo, int32 Hi, int32 Axis, const FVector& P, int32& BestId, double& BestD2) const;

	TArray<FVector> Points;
	TArray<FVector> Normals;
	TArray<int32> ScalpOf;
	TArray<FVector> ScalpPoints;
	TArray<int32> ScalpNeighbourStart;
	TArray<int32> ScalpNeighbourIds;
	/** Implicit tree: the median of each [Lo, Hi) range sits at its middle. */
	TArray<int32> Order;
	double Top = 0.0;
};

/**
 * #766 stage 1: the upload-time whole-piece fit of bought hair. Bought hair is made for a different head, at a different
 * scale, so before the stage-2 scalp fit (FMantleScalpFit) the whole piece gets one similarity transform onto the
 * MetaHuman head: a uniform scale plus an offset.
 *
 *  1. Units. The piece is first scaled about the origin so its X width is ReferenceWidthCm (18 cm, about a head's width).
 *     That is only a starting size: long hair is far wider at the shoulders than at the scalp, so the search reaches 3x.
 *  2. Search. A coarse grid over a further uniform scale, a Y offset and a top offset, then a finer grid around the
 *     best coarse candidate (FSearch). The Z offset is not free: it puts the piece's highest point at the head's top plus
 *     the top offset. X is not offset (hair is centred on the head's plane of symmetry).
 *  3. Score (#51, roots). Hair belongs where its roots meet the scalp, not where its top lines up with the head's top
 *     (that floated ponytails, braids and long hair above the crown). Each card's root is the end of the card nearer the
 *     head; a root is seated when it is within SeatCm of a scalp sample. A candidate scores the share of cards seated
 *     times the share of the scalp points with a seated root within CoverReachCm, so the hair can neither shrink onto a
 *     patch of the head nor grow until only a few roots touch. Candidates that push more than MaxDeepInsideFraction of the hair
 *     more than DeepInsideCm inside the head (above HeadDepthCm below its top: hair over the shoulders does not count)
 *     are ruled out; scores equal to 3 decimals go to the smallest median root distance. If every candidate is ruled
 *     out, the one with the least hair inside wins and bKeepsOutOfHead is false.
 *
 * Up to MaxFitPoints points and MaxFitCards cards score each candidate. The transform is
 * P' = P * (UnitScale * Scale) + (0, OffsetY, OffsetZ).
 */
class MANTLEEDITOR_API FMantleHairPieceFit
{
public:
	/** The piece's width along X is scaled to this before the search, cm. */
	static constexpr double ReferenceWidthCm = 18.0;
	/** A root this close to the scalp is seated, cm. */
	static constexpr double SeatCm = 1.0;
	/** Hair this far inside the skin is inside the head, cm ... */
	static constexpr double DeepInsideCm = 1.0;
	/** ... counted only down to this far below the head's top (the skull, not the shoulders), cm ... */
	static constexpr double HeadDepthCm = 18.0;
	/** ... and a candidate may put at most this share of the scored points there. */
	static constexpr double MaxDeepInsideFraction = 0.03;
	/** The most points and cards that score a candidate (an even stride over the piece). */
	static constexpr int32 MaxFitPoints = 2000;
	static constexpr int32 MaxFitCards = 1500;

	/** The candidate grid: every combination of the coarse lists, then (when the steps are positive) a finer grid of +-2 steps around the best. */
	struct FSearch
	{
		TArray<double> Scales;
		TArray<double> OffsetsY;
		TArray<double> TopOffsets;
		double FineScaleStep = 0.0;
		double FineOffsetStep = 0.0;

		static FSearch Default();
	};

	struct FResult
	{
		/** ReferenceWidthCm over the piece's X width. */
		double UnitScale = 1.0;
		/** The searched scale, to 4 decimals. */
		double Scale = 1.0;
		double OffsetY = 0.0;
		/** To 2 decimals. */
		double OffsetZ = 0.0;
		double TopOffset = 0.0;

		/** At the chosen candidate: share of the scored points outside the skin, and share inside the head as the bar counts it. */
		double OutsideFraction = 0.0;
		double DeepInsideFraction = 0.0;
		/** Share of cards whose root is seated on the scalp, and share of the scalp points a seated root covers. */
		double SeatedFraction = 0.0;
		double ScalpCoverage = 0.0;
		/** Median distance of the card roots from the skin, cm. */
		double RootMedianCm = 0.0;
		bool bKeepsOutOfHead = false;
		int32 CandidatesScored = 0;

		double TotalScale() const { return UnitScale * Scale; }
		FVector Apply(const FVector& P) const { return P * TotalScale() + FVector(0.0, OffsetY, OffsetZ); }
		FVector3f Apply(const FVector3f& P) const
		{
			const FVector Moved = Apply(FVector(P.X, P.Y, P.Z));
			return FVector3f(static_cast<float>(Moved.X), static_cast<float>(Moved.Y), static_cast<float>(Moved.Z));
		}
		/** One sentence for the preview's notes. */
		FString Describe() const;
	};

	/** CardEnds holds two positions per card, its two ends (FMantleScalpFit::FindCardEnds), in the same space as Points. */
	static bool Fit(const FMantleHairHeadSamples& Head, TConstArrayView<FVector3f> Points, TConstArrayView<FVector3f> CardEnds,
		const FSearch& Search, FResult& Out, FString& Error);
	static bool Fit(const FMantleHairHeadSamples& Head, TConstArrayView<FVector3f> Points, TConstArrayView<FVector3f> CardEnds, FResult& Out, FString& Error);

	/** Moves every point by the result's transform. */
	static void ApplyTo(const FResult& Fit, TArray<FVector3f>& Points);
};
