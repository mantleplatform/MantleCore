// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleAvatarConvertFixer.h"

class FMantleAvatarContract;
class USkeletalMesh;
class USkeleton;
struct FMantleContractSkeleton;
struct FMeshDescription;

/**
 * #610, the converter's Repose step: one reference chain of the skeleton fixture (FMantleContractSkeleton's
 * referencePose) as a body's reference pose has it. Degrees is what FMantleCosmeticChecks::CheckAvatarBodyShape
 * measures: the angle between the From-to-To offset in component space and the fixture's direction.
 */
struct MANTLEEDITOR_API FMantleReposeChain
{
	FName From;
	FName To;
	double Degrees = 0.0;
};

/** One bone the re-pose turned: the swing (degrees) that put Chain on the fixture's direction. */
struct MANTLEEDITOR_API FMantleReposeRotation
{
	FName Bone;
	FName ChainFrom;
	FName ChainTo;
	double Degrees = 0.0;
};

/** What re-posing a reference pose onto a skeleton fixture's A-pose does. */
struct MANTLEEDITOR_API FMantleReposePlan
{
	/** The new reference pose, bone space, one per bone in the input's order. */
	TArray<FTransform> LocalPose;

	/** Every fixture chain before and after, in the fixture's order. */
	TArray<FMantleReposeChain> Before;
	TArray<FMantleReposeChain> After;

	/** The bones turned, root to leaf. Empty when every chain was already within the clean target. */
	TArray<FMantleReposeRotation> Rotations;
};

/**
 * #610: re-posing an avatar body's reference pose to its skeleton's A-pose (the pose the upload contract checks, #582)
 * and re-binding its mesh in that pose. "Bake the pose as the reference pose": the result is the shape the old mesh
 * takes when animated into the new pose, with that pose as its reference.
 */
class MANTLEEDITOR_API FMantleAvatarRepose
{
public:
	/** The fix aims every chain it touches at the fixture's direction, and touches every chain further off than this. */
	static constexpr double CleanDegrees = 5.0;

	/** A single bone is never turned further: a body that needs more is probably not on this skeleton. */
	static constexpr double MaxRotationDegrees = 90.0;

	/**
	 * #625, the warn tier: a body whose worst chain is further than this off its target pose before the re-pose is still
	 * re-posed (up to MaxRotationDegrees), but with a warning naming the angle.
	 */
	static constexpr double ReposeWarnDegrees = 30.0;

	/**
	 * The warn tier's words for Chains (measured before the re-pose): empty when every chain is within ReposeWarnDegrees,
	 * else "WARNING: lowerarm_l to hand_l is 34 degrees off the UE5 Mannequin A-pose (re-posed, but past 30 degrees
	 * the result may look off): check it in the preview." Pure (tested).
	 */
	static FString ReposeWarning(TConstArrayView<FMantleReposeChain> Chains, const FString& PoseName);

	/**
	 * Each of Skeleton's reference chains on the pose Names/Parents/Local (bone space, parents before children, as a
	 * reference skeleton's raw bones are), measured as CheckAvatarBodyShape measures them. False, with why, when a
	 * chain's bone is missing or a chain's two bones sit on each other.
	 */
	static bool MeasureChains(TConstArrayView<FName> Names, TConstArrayView<int32> Parents, TConstArrayView<FTransform> Local,
		const FMantleContractSkeleton& Skeleton, TArray<FMantleReposeChain>& OutChains, FString& OutError);

	/**
	 * The re-posed reference pose: for each chain further than Clean degrees from the fixture, root to leaf, one bone
	 * turned by the smallest rotation (a swing: no twist about the chain) that points the chain along the fixture's
	 * direction. The bone turned is the chain's From bone when To hangs directly from it, otherwise the bone below From
	 * on the way to To (spine_01 for pelvis to spine_05), so the chain's first bone and everything else hanging from it
	 * (the legs from the pelvis) stay. Children follow. Bone count, names and hierarchy never change. False, with why,
	 * when a chain's bone is missing, a chain's To does not hang below its From, or a bone would turn further than
	 * MaxRotationDegrees.
	 */
	static bool Plan(TConstArrayView<FName> Names, TConstArrayView<int32> Parents, TConstArrayView<FTransform> Local,
		const FMantleContractSkeleton& Skeleton, double Clean, FMantleReposePlan& OutPlan, FString& OutError);

	/** Plan on Mesh's raw reference skeleton. */
	static bool PlanForMesh(const USkeletalMesh& Mesh, const FMantleContractSkeleton& Skeleton, double Clean, FMantleReposePlan& OutPlan,
		FString& OutError);

	/**
	 * Re-binds one LOD's mesh description from the reference pose OldComponent to NewComponent (component space, one per
	 * reference bone, RefNames naming them): every vertex moves by linear blend skinning with its own weights,
	 * v' = sum_i w_i * v * OldComponent_i^-1 * NewComponent_i; normals and tangents turn with the same blend (and are
	 * normalised); morph target position and normal deltas are carried by the blend's linear part. The skin weights,
	 * UVs, colours and triangles are untouched. The description's own bone poses, when it has them, become NewLocal.
	 * The description's skin-weight bone indices are matched to RefNames by the description's bone names when it has
	 * them, otherwise taken as reference skeleton indices. False, with why, when it has no skin weights.
	 */
	static bool RebindMeshDescription(FMeshDescription& Mesh, TConstArrayView<FName> RefNames, TConstArrayView<FTransform> OldComponent,
		TConstArrayView<FTransform> NewComponent, TConstArrayView<FTransform> NewLocal, FString& OutError);

	/**
	 * A NEW skeletal mesh, PackagePath/AssetName, duplicated from Source with NewLocalPose (bone space, one per raw
	 * reference bone) as its reference pose and every LOD re-bound in it (RebindMeshDescription), built. Source is
	 * never modified; the new mesh keeps Source's USkeleton (same bones, so its animations still play on it), its
	 * sockets on their bones, its materials and its physics asset. The new package is dirty, not saved. Null, with why,
	 * when NewLocalPose does not match Source's bones or a LOD cannot be re-bound.
	 */
	static USkeletalMesh* WriteReposed(USkeletalMesh* Source, TConstArrayView<FTransform> NewLocalPose, const FString& PackagePath,
		const FString& AssetName, FString& OutError);

	/**
	 * WriteReposed's asset side with the LOD edit left to the caller (the Refit fixer re-binds and warps): a NEW
	 * skeletal mesh, PackagePath/AssetName, duplicated from Source, each LOD with source geometry handed to EditLOD in
	 * its mesh description (a LOD with only built data converted first; a LOD generated by reduction is regenerated by
	 * the build), then NewLocalPose (bone space, one per raw reference bone) as its reference pose, built. Source is
	 * never modified; the new package is dirty, not saved. Null, with why, when NewLocalPose does not match Source's
	 * bones or EditLOD refuses a LOD (nothing is left behind). The new mesh is an asset even when Source is transient (a catalog reference body,
	 * FMantleReferenceBodies): the transient flag a copy inherits is cleared on it and its inner objects.
	 */
	static USkeletalMesh* WriteWithPose(USkeletalMesh* Source, TConstArrayView<FTransform> NewLocalPose, const FString& PackagePath,
		const FString& AssetName, TFunctionRef<bool(int32 LOD, FMeshDescription& Description, FString& OutError)> EditLOD, FString& OutError);

	/** Mesh's raw reference pose, bone by bone: names, parent indices (INDEX_NONE for roots) and local transforms. */
	static void ReadMesh(const USkeletalMesh& Mesh, TArray<FName>& OutNames, TArray<int32>& OutParents, TArray<FTransform>& OutLocal);

	/** A mesh a fixer wrote and then gave up on: never left behind as a half-made asset. */
	static void DiscardMesh(USkeletalMesh* Mesh);

	/** Component-space transforms of a bone-space pose (parents before children). */
	static void ComposeComponent(TConstArrayView<int32> Parents, TConstArrayView<FTransform> Local, TArray<FTransform>& OutComponent);

	/** "lowerarm_l 28 deg, upperarm_r 23 deg" (a degree sign, not "deg"): the chains further than Clean off, by their From bone. */
	static FString DescribeChains(TConstArrayView<FMantleReposeChain> Chains, double Clean);
};

/**
 * MantleAvatarFixerIds::Repose. Analyze proposes a fix for an avatar body on a skeleton with a referencePose when at
 * least one chain is further off than the contract's ChainToleranceDegrees (the upload would refuse it); Apply then
 * re-poses every chain further off than FMantleAvatarRepose::CleanDegrees into a new mesh under the output folder.
 * #625: a part whose skeleton id is not set yet is read the upload's way (FMantleCosmeticExport::AvatarSkeletonId). Only
 * a bone that would turn further than MaxRotationDegrees refuses it; past ReposeWarnDegrees it is re-posed with a
 * warning.
 */
class MANTLEEDITOR_API FMantleAvatarReposeFixer : public IMantleAvatarFixer
{
public:
	/**
	 * The skeleton id of Context.Parts[PartIndex] (its own, else the upload's reading), and the pose it is re-posed to: its
	 * own skeleton's fixture. False when the part is not an avatar body or its skeleton has no reference pose.
	 */
	static bool TargetPose(const FMantleConvertContext& Context, int32 PartIndex, const FMantleAvatarContract& Contract, FString& OutSkeletonId,
		FMantleContractSkeleton& OutPose);

	virtual FName GetId() const override { return MantleAvatarFixerIds::Repose; }
	virtual void Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const override;
	virtual bool Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const override;
};

/** Registers FMantleAvatarReposeFixer with FMantleAvatarFixers (MantleEditor's StartupModule). */
MANTLEEDITOR_API void RegisterMantleAvatarReposeFixer();
