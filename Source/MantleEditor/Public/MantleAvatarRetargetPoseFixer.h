// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleAvatarConvertFixer.h"

class FMantleAvatarContract;
class USkeletalMesh;

/**
 * #625, the retarget pose of a converted body. The client animates every MetaHuman-skeleton body from a hidden UEFN
 * mannequin through the plugin's RTG_UEFN_to_Mantle_MetaHuman. Its FK chains copy each driver bone's rotation as a
 * delta from the driver's retarget pose onto the body bone's retarget pose (FChainDecoderFK::RetargetRotation), so a
 * limb whose retarget-pose direction differs from the driver's keeps that difference in every frame. The body's
 * retarget pose is its reference pose with the retargeter's target offsets applied in each bone's own axes (Ref times
 * Offset, the processor's GenerateRetargetPose), and those offsets (upperarm 21, lowerarm 10, hand 28 degrees) were
 * authored for the stock MetaHuman's A-pose and bone axes. A body that keeps another pose or other bone axes than the
 * stock body's (the UE5 Mannequin's hands are rolled 20 degrees from MetaHuman's) would therefore move off, so:
 *   - its hands and arms sit at a constant angle to the driver's (the UEFN Mannequin's hands 21 degrees, the Elf's 15),
 *   - and a near-straight knee bent another way than the driver's makes the Align Pole Vector op pick another axis of
 *     the thigh as its pole (FPoleVectorMatcher::CalculateBestPoleAxisForChain), 98 degrees from the driver's on
 *     Mixamo's Ch02: the op then swings the whole leg about hip to ankle, knees in, 65 to 75 degrees off in a crouch.
 *
 * The fix is computed here, at convert time, and stored in what the client already reads, the body's reference pose:
 *   1. The axes: each limb bone (upperarm, lowerarm, hand, thigh, calf, foot) takes the axes of the retargeter's own
 *      MetaHuman skeleton, swung onto the body's segment. No joint moves and the mesh does not move: only the frames
 *      its vertices are bound to change, so the retargeter's offsets (and the MetaHuman twist rules) act as authored.
 *   2. The pose: each arm and leg bone is turned (a swing, its twist kept) so that, with the retargeter's offsets
 *      applied, the segment to its next joint points where the driver's does in the driver's retarget pose. The FK
 *      delta then carries no constant offset and the knees bend in the driver's plane, so the pole op picks the
 *      driver's axis. The mesh is re-bound in the new pose.
 * The feet keep their angle to the floor: a foot's ankle-to-ball line is the character's anatomy (an Elf's high ankle,
 * Mixamo's), and turning it onto the driver's would tilt the sole into the floor.
 *
 * The fingers take step 1's axes too (#774), and only those: the retargeter's finger chains are FK as well, and a finger
 * bone whose local axes differ from the target skeleton's (a Mixamo finger rolled 90 degrees, a thumb with another
 * "up") turns the animation's finger rotations about the wrong axes, which twists and mangles the finger. Each finger
 * bone (metacarpal where both skeletons have one, then 01, 02, 03) takes the target skeleton's frame as it sits in the
 * (already framed) bone above it, swung onto the segment to the next finger bone, the way the limbs' are. The tip has no
 * segment of its own and keeps the frame its parent hands it. Fingers never turn in step 2: no joint moves, no vertex
 * moves (a finger's skin matrix is the identity, but for the swing of the hand it hangs from), so the mesh stays exactly
 * as the creator made it.
 *
 * The driver's pose, the MetaHuman axes and the retargeter's offsets are read from the plugin's own assets (the
 * retargeter and its two IK rigs' stored skeletons), by reflection: MantleEditor does not link IKRig.
 *
 * A body uploaded as it is on a Mannequin skeleton (UE5, UE4, UEFN) is lined up the same way through that skeleton's
 * retargeter (RTG_UEFN_to_Mantle_UE5Mannequin and so on): the axes are its target IK rig's Mannequin's, the offsets its
 * own (the UE5 one's include the legs and feet, the UE4 one's the clavicles and spine). A Mixamo rig RigMap put on the
 * UE4 Mannequin skeleton keeps Mixamo's bone axes until step 1 gives it the Mannequin's.
 */

/** One limb segment: Bone takes axes (step 1) or turns (step 2) by the segment Bone to Child. */
struct MANTLEEDITOR_API FMantleRetargetPoseStep
{
	FName Bone;
	FName Child;
};

/** A segment's angle to the driver's in the retarget pose, before and after. */
struct MANTLEEDITOR_API FMantleRetargetPoseSegment
{
	FName From;
	FName To;
	double BeforeDegrees = 0.0;
	double AfterDegrees = 0.0;
};

/** One bone the plan turned or gave new axes, and by how much. */
struct MANTLEEDITOR_API FMantleRetargetPoseTurn
{
	FName Bone;
	double Degrees = 0.0;
};

/** What lining a reference pose up with the driver does. */
struct MANTLEEDITOR_API FMantleRetargetPosePlan
{
	/** The new reference pose, bone space, one per input bone, in its order. No joint of a bone outside the steps moves. */
	TArray<FTransform> LocalPose;

	/**
	 * The input pose with only step 1's new axes: the same joints and the same mesh, the pose the vertices are re-bound
	 * from (FMantleAvatarRepose::RebindMeshDescription's old pose), so a change of axes never moves a vertex.
	 */
	TArray<FTransform> FramedPose;

	/** Every step's segment, and each foot's (foot to ball, reported, never turned), in the retarget pose. */
	TArray<FMantleRetargetPoseSegment> Segments;

	/** Step 1: the bones given the MetaHuman axes (at least MinTurnDegrees of change), in order. */
	TArray<FMantleRetargetPoseTurn> Reframes;

	/** Step 1 for the fingers (#774): the finger bones given the target skeleton's axes (at least MinTurnDegrees of change), in order. */
	TArray<FMantleRetargetPoseTurn> FingerReframes;

	/** Step 2: the bones turned (at least MinTurnDegrees), in step order. */
	TArray<FMantleRetargetPoseTurn> Turns;

	/** Nothing to do: no new axes and no turn. */
	bool IsEmpty() const { return Reframes.Num() == 0 && FingerReframes.Num() == 0 && Turns.Num() == 0; }

	/** The largest change of axes among the fingers, degrees (0 when none). */
	double MaxFingerReframeDegrees() const;
};

/** The retargeter's side of the plan: the driver, the MetaHuman axes and the body's offsets. */
struct MANTLEEDITOR_API FMantleRetargetPoseDriver
{
	/** The retargeter's target retarget pose: local rotation offsets by bone (Ref times Offset), the current pose's. */
	TMap<FName, FQuat> TargetOffsets;

	/** The driver's joints in its retarget pose (its reference pose with the source offsets), component space, by bone. */
	TMap<FName, FVector> DriverJoints;

	/**
	 * The target IK rig's stored skeleton (the MetaHuman the retargeter was authored on), component space, by bone:
	 * step 1's axes. Empty: step 1 is skipped.
	 */
	TMap<FName, FTransform> TargetReference;

	/** What they were read from ("RTG_UEFN_to_Mantle_MetaHuman (36 target offsets), driver from IK_UEFN_Mannequin ..."). */
	FString Describe;
};

class MANTLEEDITOR_API FMantleAvatarRetargetPose
{
public:
	/** A bone that would turn, or change axes, less than this is left as it is. */
	static constexpr double MinTurnDegrees = 0.25;

	/** A bone that would turn further refuses the plan: a converted body that far off is not a MetaHuman-skeleton humanoid. */
	static constexpr double MaxTurnDegrees = 45.0;

	/**
	 * Step 2, parents first: per side upperarm to lowerarm, lowerarm to hand, hand to middle_01 (through its
	 * metacarpal), thigh to calf, calf to foot. Each foot then keeps its component rotation.
	 */
	static TConstArrayView<FMantleRetargetPoseStep> Steps();

	/** Step 1: Steps() and each foot to its ball. */
	static TConstArrayView<FMantleRetargetPoseStep> FrameSteps();

	/**
	 * Step 1 as well, where the body and the target skeleton both have the two bones: the clavicles and the spine (each
	 * bone its first entry that applies). Never required.
	 */
	static TConstArrayView<FMantleRetargetPoseStep> OptionalFrameSteps();

	/**
	 * #774, the finger chains step 1 frames, one per finger and side (thumb, index, middle, ring, pinky; left then right),
	 * parents first: "<finger>_metacarpal_<side>" (not on every skeleton), then "<finger>_01_<side>", 02 and 03. A plan
	 * uses the bones of a chain that the body and the target skeleton both have, in this order.
	 */
	static TArray<TArray<FName>> FingerChains();

	/** The bones a plan needs on the body, the driver and the MetaHuman skeleton (the steps' bones and children). */
	static TArray<FName> RequiredBones();

	/**
	 * Pose (bone space, parents before children) with Offsets applied as the retargeter applies them (each bone's local
	 * rotation times its offset), in component space.
	 */
	static void RetargetPoseComponent(TConstArrayView<FName> Names, TConstArrayView<int32> Parents, TConstArrayView<FTransform> Local,
		const TMap<FName, FQuat>& Offsets, TArray<FTransform>& OutComponent);

	/**
	 * The plan (the class comment's two steps). Step 1 runs when Driver.TargetReference is set: each frame step's bone
	 * gets the reference's rotation swung by the smallest rotation from the reference's segment to the body's; every
	 * joint keeps its component transform. Step 2: each step, in order, turns its bone by the smallest rotation that
	 * points its segment, in the retarget pose (the pose with Driver.TargetOffsets), along the driver's
	 * (Driver.DriverJoints), kept in the reference pose by taking the bone's offset off again; each foot is then given
	 * back its component rotation from after step 1. The fingers (FingerChains) are framed after the limbs in step 1,
	 * when Driver.TargetReference is set, each from its parent's framed rotation; a finger chain missing on either side
	 * is skipped, and so is a finger segment with no length; a target skeleton without the finger bone a limb step ends at
	 * (middle_01, the hand's) leaves that bone's axes as they are. False, with why, when a bone is missing on any side, a segment has
	 * no length, a parent comes after its child, or a bone would turn further than MaxTurnDegrees. Pure (tested).
	 */
	static bool Plan(TConstArrayView<FName> Names, TConstArrayView<int32> Parents, TConstArrayView<FTransform> Local, const FMantleRetargetPoseDriver& Driver,
		FMantleRetargetPosePlan& OutPlan, FString& OutError);

	/** Plan on Mesh's raw reference skeleton. */
	static bool PlanForMesh(const USkeletalMesh& Mesh, const FMantleRetargetPoseDriver& Driver, FMantleRetargetPosePlan& OutPlan, FString& OutError);

	/**
	 * Reads Driver from a UIKRetargeter (as a UObject): its current target retarget pose's offsets, its source IK rig's
	 * stored skeleton (the driver's reference pose) with the current source retarget pose's offsets applied, and its
	 * target IK rig's stored skeleton. False, with why, when Retargeter is not one or a property the read relies on is
	 * missing (an engine rename).
	 */
	static bool ReadRetargeter(const UObject* Retargeter, FMantleRetargetPoseDriver& OutDriver, FString& OutError);

	/**
	 * ReadRetargeter on the plugin's retargeter for SkeletonId (metahuman, ue5mannequin, ue4mannequin, uefnmannequin: the
	 * one the client loads for a body on that skeleton, FMantleAvatarPreviewPlan::ForSkeleton).
	 */
	static bool ReadRetargeterForSkeleton(const FString& SkeletonId, FMantleRetargetPoseDriver& OutDriver, FString& OutError);

	/** "hand_l 20.3°, lowerarm_l 2.9°": the turns, largest first. */
	static FString DescribeTurns(TConstArrayView<FMantleRetargetPoseTurn> Turns);
};

/**
 * MantleAvatarFixerIds::RetargetPose, before Assembly (so the body's own garments refit onto the body as it uploads).
 * Analyze proposes it for an avatar body without DNA on a Mannequin skeleton (UE5, UE4, UEFN), uploaded as it is (the
 * as-is route), planned through the retargeter the client uses for its skeleton: any avatar body, the creator's own
 * mesh or one an earlier step wrote (RigMap's Mixamo rig, Repose's re-posed body). The creator's mesh is never modified:
 * the result is a new mesh, as every step's is. #625: a MetaHuman-skeleton body is never lined up: a converted one is
 * Epic's own build, with the bone axes and A-pose the client's MetaHuman retargeter was authored for (the plan maths
 * still serves the MetaHuman retargeter, and its tests read it).
 * Apply re-plans on the mesh as it stands and writes <Mesh>_RetargetPose under the output folder, re-bound from the
 * plan's FramedPose to its LocalPose. It refuses a result the upload would refuse (a chain further than
 * ChainToleranceDegrees from the skeleton's A-pose).
 */
class MANTLEEDITOR_API FMantleAvatarRetargetPoseFixer final : public IMantleAvatarFixer
{
public:
	virtual FName GetId() const override { return MantleAvatarFixerIds::RetargetPose; }
	virtual void Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const override;
	virtual bool Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const override;

	/**
	 * Whether Context.Parts[PartIndex] is a body to line up now (the class comment's rule): included, the avatar slot, no
	 * DNA, on a Mannequin skeleton the client poses bodies on (never the MetaHuman one). OutSkeletonId, when given, gets the
	 * skeleton id (also set for a MetaHuman body, which returns false).
	 */
	static bool IsBodyToLineUp(const FMantleConvertContext& Context, int32 PartIndex, const FMantleAvatarContract& Contract, FString* OutSkeletonId = nullptr);
};

/** Registers FMantleAvatarRetargetPoseFixer with FMantleAvatarFixers (MantleEditor's StartupModule). */
MANTLEEDITOR_API void RegisterMantleAvatarRetargetPoseFixer();
