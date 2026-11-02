// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleSkinnedMesh.h"
#include "MantleVehicleChassisMesh.h"

/** What reducing a vehicle body did. */
struct MANTLEEDITOR_API FMantleVehicleBodyReduceResult
{
	/** Triangles in LOD 0 before and after. Equal when nothing was reduced. */
	int32 Before = 0;
	int32 After = 0;

	/** The body was reduced (After < Before). */
	bool bReduced = false;

	/** The creator pressed Cancel on the progress dialog. */
	bool bCanceled = false;

	/** Why the body could not be brought within the limit, as a clause for a sentence; empty when it was reduced or did not need it. */
	FString Problem;
};

/**
 * A model with more triangles than a published vehicle may have (FMantleVehicleLimits::MaxTriangles) is reduced for the creator, as a place mesh over its limit is
 * (FMantleMeshReduce): nothing is refused, and the creator is told in plain words. The model they made is never touched; the reduced body is the vehicle's own.
 *
 * The body on a chassis is rigid (each vertex rides one bone), so each bone's triangles are reduced as a piece of their own with the engine's mesh reducer, their
 * materials kept as the piece's sections, and the wheels, doors and the rest keep their places. Pure arithmetic over FMantleSkinnedMesh plus the reducer.
 */
struct MANTLEEDITOR_API FMantleVehicleBodyReduce
{
	/** Triangles in LOD 0. */
	static int32 CountTriangles(const FMantleSkinnedMesh& Body);

	/**
	 * Reduces Body's LOD 0 in place to at most MaxTriangles, showing a progress dialog (with a Cancel button) when it takes a while. The vertices change, so Extras (a
	 * vertex-for-vertex list of more UVs and colors) is emptied; tangents are left to be worked out again. True when Body was within the limit or now is; false with
	 * Body untouched when the reducer is missing, could not get under the limit or was canceled (see Out).
	 */
	static bool ReduceToFit(FMantleSkinnedMesh& Body, FMantleVehicleBodyExtras& Extras, int32 MaxTriangles, FMantleVehicleBodyReduceResult& Out);

	/** The share of the limit a reduction aims for: the reducer stops near its target, not on it. */
	static constexpr float TargetShare = 0.92f;

	/** A reduction that comes out over the limit is tried again this many times in all, each aiming lower. */
	static constexpr int32 MaxAttempts = 3;
};
