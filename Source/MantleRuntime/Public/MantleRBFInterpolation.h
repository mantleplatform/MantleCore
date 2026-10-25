// Copyright Joshua Eger. All Rights Reserved.
// Derived from Epic Games code: UE 5.8 MeshResizing's RBFInterpolation.h. Epic's line stays below.
// Copyright Epic Games, Inc. All Rights Reserved.
// Vendored from UE 5.8: MeshResizing's RBFInterpolation (MeshResizingCore) and the HairStrands pieces it
// calls (GroomBinding_RBFWeighting's sampler and weights builder, FGroomRBFDeformer::GetEntryCount), cut
// to what Mantle uses, so the Experimental MeshResizing plugin and its dependencies (Dataflow,
// GeometryCollection, HairStrands) stay out of every game build and creator project. The arithmetic is
// the engine's, unchanged, so the output is too.

#pragma once

#include "CoreMinimal.h"

/**
 * The engine's FMeshResizingRBFInterpolationData, same fields in the same order: the RBF weights over a
 * body shape's positions. Never UE-serialized; it travels in Mantle's own formats (the plugin's GLB
 * extension, then MBS1), which carry exactly these three arrays.
 */
struct FMantleRBFInterpolationData
{
	/** Per sample, the source position's index. */
	TArray<int32> SampleIndices;

	/** Per sample, the source position at that index. */
	TArray<FVector3f> SampleRestPositions;

	/** The (S + 4) x (S + 4) inverse of the multiquadric system for S samples, row-major. */
	TArray<float> InterpolationWeights;
};

/** UE::MeshResizing::FRBFInterpolation, the entry points Mantle calls. */
struct MANTLERUNTIME_API FMantleRBFInterpolation
{
	/**
	 * FRBFInterpolation::GenerateWeights: NumInterpolationPoints samples chosen by farthest-point sampling
	 * from position 0, and the pseudo-inverse of their multiquadric system. Cubic in the sample count, so
	 * the plugin runs it at upload, never a client at load.
	 */
	static void GenerateWeights(TConstArrayView<FVector3f> SourcePositions, int32 NumInterpolationPoints, FMantleRBFInterpolationData& OutData);

	/**
	 * FRBFInterpolation::GenerateMeshSamples: TargetPositions holds the target's positions at the
	 * SampleIndices, in sample order (checked). SampleDeformations gets S + 4 entries: one per sample,
	 * then the constant, X, Y and Z terms.
	 */
	static void GenerateMeshSamples(const TArray<FVector3f>& TargetPositions, const FMantleRBFInterpolationData& InterpolationData,
		TArray<FVector3f>& SampleDeformations);
};
