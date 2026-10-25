// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleSkinnedMesh.h"

struct FReferenceSkeleton;

/**
 * The identity of a place skeleton (#469, the arbitrary-skeleton contract): which clips a place skeletal
 * mesh can play. A place skeletal mesh (MSK1 role 3) names its own bone table by this hash, and a place clip
 * (MAN1 flag bit 0) names the table it was authored on; a clip plays only on a mesh with the same hash.
 *
 * "skel_" and the first 16 lower-case hex digits of the SHA-256 over every bone in table order: its name with
 * ASCII A-Z folded to lower case (as Unreal's FNames compare, and as the worker's cook.BoneKey folds), UTF-8,
 * one zero byte, then its parent index as a little-endian int32 (-1 for the root). Names and hierarchy only:
 * the reference pose is not part of it, since a clip's tracks are local transforms. The worker's
 * cook.SkeletonHash is the same function; Mantle.Place.SkeletalMesh.SkeletonHash and the Go test share test
 * vectors.
 *
 * Pure; any thread.
 */
struct MANTLERUNTIME_API FMantleSkeletonHash
{
	/** Every hash starts with this. */
	static constexpr const TCHAR* Prefix = TEXT("skel_");

	/** Hex digits after the prefix. */
	static constexpr int32 Digits = 16;

	/** The hash of a bone table given as parallel arrays (Names.Num() == Parents.Num()); empty when they differ. */
	static FString Compute(TConstArrayView<FName> Names, TConstArrayView<int32> Parents);

	/** The hash of a cooked mesh's bone table. */
	static FString Compute(TConstArrayView<FMantleSkinnedMeshBone> Bones);

	/** The hash of a reference skeleton's raw bones (what the plugin uploads a mesh's skeleton as). */
	static FString Compute(const FReferenceSkeleton& Skeleton);

	/** True when Value is shaped like a hash: the prefix and Digits lower-case hex digits. */
	static bool IsHash(const FString& Value);
};
