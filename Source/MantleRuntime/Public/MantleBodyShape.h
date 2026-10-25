// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleNeckSamples.h"
#include "MantleRBFInterpolation.h"
#include "MantleSkinnedMesh.h"

class USkeletalMesh;
struct FReferenceSkeleton;

/**
 * A body's bone table (names, parents) and its reference pose (one local transform per bone): what
 * a fitted garment is rebound to (AVATAR_CATALOG_PLAN.md section 0.1 item 3) and what MBS1 carries.
 * Plain arrays, so it can be read off the game thread.
 */
struct MANTLERUNTIME_API FMantleReferencePose
{
	/** Parent-first, one root (the FMantleSkinnedMesh bone table rules). */
	TArray<FMantleSkinnedMeshBone> Bones;

	/** One local (parent-relative) transform per bone. */
	TArray<FTransform> LocalTransforms;

	int32 Num() const { return Bones.Num(); }

	/** The raw bones and raw reference pose of Skeleton (virtual bones are the USkeleton's, not the mesh's). */
	static FMantleReferencePose FromReferenceSkeleton(const FReferenceSkeleton& Skeleton);

	/** Mesh's bone table and its own reference pose (empty transforms when it has none). */
	static FMantleReferencePose FromSkinnedMesh(const FMantleSkinnedMesh& Mesh);

	/** At least one bone, a parent-first table with one root and unique names, one finite, unit-rotation transform per bone. */
	bool Validate(FString& OutError) const;
};

/**
 * The neck a body shape may carry (#430, plan section 0.1 item 12; MBS1's NECK block): the source half of
 * FMantleNeckSamples (BuildSource) measured by the plugin on the face the body was authored with, and the RBF
 * weights over the shape's positions followed by these samples, which the plugin computes at upload because a
 * client cannot in time. The client builds the wearer's half (FMantleNeckSamples::BuildWearer) from the wearer's
 * face and fits through both, so the collar follows the wearer's neck. Empty on a shape uploaded without a face.
 */
struct MANTLERUNTIME_API FMantleBodyShapeNeck
{
	EMantleNeckMethod Method = EMantleNeckMethod::None;

	/** Per sample, ascending: the source face's triangle (Correspondence) or the ray (Rays) it came from. */
	TArray<int32> Ids;

	/** The samples on the source face, cm, reference pose. */
	TArray<FVector3f> Positions;

	/** Over the shape's Positions then these: SampleIndices may point past the body, into the neck. */
	FMantleRBFInterpolationData Weights;

	bool IsSet() const { return Method != EMantleNeckMethod::None && Positions.Num() > 0 && Weights.SampleIndices.Num() > 0; }

	/** Bytes it takes (ids, positions, weights). */
	int64 GetBytes() const;
};

/**
 * A body shape: the in-memory form of MBS1 (AVATAR_CATALOG_PLAN.md section 3.4). The positions of a
 * body at Mantle's reference topology in canonical (vertex) order, its reference pose, and the RBF
 * weights FMantleGarmentFitter::ComputeSourceWeights generates from the positions. A garment is
 * uploaded with the shape of the body it was authored on; the client warps it from that shape onto
 * each wearer's avatar.
 *
 * Holds no UObjects, so it can be read and used off the game thread.
 */
struct MANTLERUNTIME_API FMantleBodyShape
{
	/** Reference-topology positions, reference pose component space, cm, in the topology's vertex order. */
	TArray<FVector3f> Positions;

	FMantleReferencePose ReferencePose;

	/**
	 * The engine's RBF weights over Positions (empty until ComputeSourceWeights): SampleIndices into
	 * Positions, SampleRestPositions (Positions at those indices), and InterpolationWeights, the
	 * (S + 4) x (S + 4) inverse of the multiquadric system for S samples.
	 */
	FMantleRBFInterpolationData Weights;

	/** UMantleBodyTopologyData::ComputePositionsHash of Positions (set by whoever fills Positions); the fitter's cache key. */
	uint32 PositionsHash = 0;

	/** #430: the neck measured on the face the body was authored with; unset without one. */
	FMantleBodyShapeNeck Neck;

	bool HasWeights() const { return Weights.SampleIndices.Num() > 0; }

	/**
	 * The neck is consistent with this shape: a method, one id per sample (ascending; rays inside the family),
	 * finite positions, and weights over Positions followed by Neck.Positions (FMantleBodyShape::ValidateWeights'
	 * rules). False with the first problem. A shape without a neck passes.
	 */
	bool ValidateNeck(FString& OutError) const;

	/**
	 * The shape the neck's weights are over: Positions followed by Neck.Positions, Neck.Weights, the same
	 * reference pose, its own positions hash. What FMantleGarmentFitter::ComputeField takes with the neck's sample
	 * count. Empty positions without a neck.
	 */
	FMantleBodyShape MakeNeckShape() const;

	/** Bytes the weights take (indices, rest positions, interpolation weights): what MBS1 would carry for them. */
	int64 GetWeightsBytes() const;

	/**
	 * The weights are consistent with Positions: S >= 1 samples, every index in range and its rest
	 * position equal to Positions there, and (S + 4)^2 interpolation weights, all finite. False with
	 * the first problem in OutError.
	 */
	bool ValidateWeights(FString& OutError) const;

	/**
	 * Builds a body shape from a skeletal mesh: fills Out from the first LOD
	 * of Source with ReferenceVertexCount vertices and ReferenceUVHash (the reference topology, see
	 * UMantleBodyTopologyData), plus Source's raw reference skeleton. No weights. Reads CPU copies of
	 * render data (a dev and editor tool, like FMantleSkinnedMesh::ExtractFromSkeletalMesh). False,
	 * logged, with the reason in OutError, when no LOD has the reference topology.
	 */
	static bool FromSkeletalMesh(const USkeletalMesh* Source, int32 ReferenceVertexCount, uint32 ReferenceUVHash,
		FMantleBodyShape& Out, int32* OutLODIndex = nullptr, FString* OutError = nullptr);
};
