// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleGlbWriter.h"

class USkeletalMesh;
struct FMantleFitBodyPool;

/**
 * Which of Epic's 18 standard MetaHuman bodies (f|m x srt|med|tal x unw|nrw|ovw) a skeletal-mesh garment was made on, from
 * its bind-pose joints. A garment made on a standard body is skinned to that body's skeleton, so its joints sit on that
 * body's joints. The skin distance can't pick the body: the closest vertices of a shirt touch any torso (the 2026-11-25
 * proof: Montana's shirt passed the 1.5 cm skin check on 16 of the 18 bodies), so it only confirms the pick.
 */
struct MANTLEEDITOR_API FMantleStandardBodyMatch
{
	/**
	 * How close (cm, mean over the bones the garment is skinned to) a garment's joints must be to a standard body's for it to
	 * count as made on that body (Joshua, 2026-11-25). Montana's shirt, made on m_tal_nrw, measured 1.07 cm over every
	 * shared bone (its worst gap a foot bone) against 1.73 for m_tal_nrw's nearest neighbor m_tal_ovw and 4.90 or more for
	 * every medium body. Not a refusal limit: a garment that misses it uploads in one size, as before.
	 */
	static constexpr float JointMatchCm = 1.5f;

	/** One body's score: mean and largest joint distance (cm) over Bones of the garment's skinned bones. MeanCm < 0: not scored. */
	struct FScore
	{
		FString Id;
		float MeanCm = -1.0f;
		float LargestCm = 0.0f;
		FName LargestBone;
		int32 Bones = 0;
	};

	struct FCandidate
	{
		FString Id;
		TMap<FName, FVector3f> Joints;
	};

	struct FResult
	{
		/** Best is within JointMatchCm. */
		bool bMatch = false;
		/** The lowest mean; unscored (MeanCm < 0) when no body could be scored. */
		FScore Best;
		/** The next lowest; unscored when there is only one body. */
		FScore RunnerUp;
		/** Every candidate, in the order given. */
		TArray<FScore> All;
		/** Skinned bones the bodies don't have (the garment is then not scored). */
		TArray<FName> Missing;
	};

	/** The 18 standard body ids, f_ then m_, srt|med|tal then unw|nrw|ovw. */
	static TArray<FString> AllBodyIds();

	/** Pure: each joint's component-space position (cm) by name, from parent-first joints with local transforms. */
	static TMap<FName, FVector3f> JointPositions(TConstArrayView<FMantleGlbJoint> Joints);

	/** Pure: the joints with any weight on the vertices of Mesh's primitives (only those drawn with KeepMaterials' glTF materials, when given). */
	static TSet<FName> SkinnedBones(const FMantleGlbMesh& Mesh, const TSet<int32>* KeepMaterials = nullptr);

	/**
	 * Pure: scores each candidate by the distance of the garment's joints to its own, over Skinned (bones with no weight, such as
	 * a shirt's feet, don't count). A skinned bone some candidate lacks leaves the garment unscored (Missing). bMatch when the
	 * best mean is within CutoffCm.
	 */
	static FResult Match(const TMap<FName, FVector3f>& Garment, const TSet<FName>& Skinned, TConstArrayView<FCandidate> Bodies,
		float CutoffCm = JointMatchCm);

	/** Pure: "men's tall normal-weight" for m_tal_nrw; the id itself when it is not one. */
	static FString BodyWords(const FString& Id);

	/** Pure: the sentence for the creator (panel, check notes, log): which body it was made on, or the nearest one and how far. */
	static FString Describe(const FResult& Result);

	/**
	 * Editor: Id's joint positions, from the body made in memory (FMantleFitBodies::Make) on the first ask in an editor session,
	 * then from a cache. A body made here goes into Pool when Keep says so (for the fit to reuse), else is released. False with why.
	 */
	static bool BodyJoints(const FString& Id, FMantleFitBodyPool& Pool, TFunctionRef<bool(const FString&)> Keep, TMap<FName, FVector3f>& OutJoints,
		FString& OutError);

	/** Editor: the skeleton bone names every standard body has (Epic's metahuman_base_skel); empty when it can't be loaded. */
	static TSet<FName> StandardBoneNames();
};
