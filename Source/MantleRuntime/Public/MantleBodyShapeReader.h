// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleBodyShape.h"

/** The MBS1 header fields FMantleBodyShape has no place for. */
struct FMantleBodyShapeHeader
{
	/** The skeleton of the shape's reference pose ("metahuman"). */
	FString Skeleton;

	/** The quantised UV hash of the topology the positions are in (body-topology-v1.json's, checked at cook). */
	uint32 UVHash = 0;

	/** The positions' bounds as the cook worker wrote them, cm. */
	FBox3f Bounds = FBox3f(ForceInit);

	/** The file carried the plugin's RBF warp weights (FMantleBodyShape::HasWeights). */
	bool bWarpWeights = false;

	/** Tags of the extension blocks this build does not know and skipped (it reads NECK, #430). */
	TArray<FString> SkippedExtensions;

	/** The NECK block's samples (FMantleBodyShape::Neck); 0 without one. */
	int32 NeckSamples = 0;
};

struct FMantleBodyShapeReadResult
{
	bool bSuccess = false;

	FMantleBodyShape Shape;

	FMantleBodyShapeHeader Header;

	/** What went wrong, phrased for whoever has to act on it. */
	FString Error;
};

/** Validated selection geometry. No reference pose, neck data or RBF matrices remain resident. */
struct FMantleBodyShapeCandidateReadResult
{
	bool bSuccess = false;
	TArray<FVector3f> Positions;
	FMantleBodyShapeHeader Header;
	FString Error;
};

/**
 * Reads MBS1, the cooked body shape a garment was authored on (AVATAR_CATALOG_PLAN.md section 3.4),
 * into FMantleBodyShape: positions in the reference topology's order, the bone table and reference
 * pose, the RBF warp weights when present (SampleRestPositions rebuilt from the positions at
 * SampleIndices, which MBS1 does not store), the neck from the NECK extension block when present
 * (#430: FMantleBodyShape::Neck, checked by ValidateNeck), and PositionsHash, which the reader recomputes from the
 * positions and requires to equal the file's (it is the fitter's cache key, so a wrong one would
 * reuse another shape's field). Written by backend/internal/cook/bodyshape.go.
 *
 * Strict in the same way as FMantleSkinnedMeshReader: counts checked against the bytes left and a
 * sanity cap before allocating, floats finite, sample indices in range and distinct, unknown flags
 * and trailing bytes refused, unknown extensions skipped, and the result must pass
 * FMantleReferencePose::Validate and, with weights, FMantleBodyShape::ValidateWeights. The reader
 * does not know the reference topology's vertex count; the fitter checks it. Touches no UObjects.
 */
class MANTLERUNTIME_API FMantleBodyShapeReader
{
public:
	/** The version this build understands. Must match BodyShapeVersion in the worker. */
	static constexpr uint32 SupportedVersion = 1;

	/** Sanity caps above limits.json's (the reference body has 8,568 vertices; maxWarpPointCount is 1,500). */
	static constexpr int32 MaxBones = 2048;
	static constexpr int32 MaxVertices = 1'000'000;
	static constexpr int32 MaxWarpSamples = 2048;

	/** The NECK block's cap (backend cook.MaxNeckSamples). */
	static constexpr int32 MaxNeckSamples = 4096;

	static FMantleBodyShapeReadResult Read(TConstArrayView<uint8> Data);

	/** Validates the same complete MBS1 bytes as Read, scanning warp floats without allocating matrices. */
	static FMantleBodyShapeCandidateReadResult ReadCandidate(TConstArrayView<uint8> Data, int32 ExpectedVertices = 0);

private:
	static FMantleBodyShapeReadResult ReadInternal(TConstArrayView<uint8> Data, bool bCandidate, int32 ExpectedVertices = 0);
};
