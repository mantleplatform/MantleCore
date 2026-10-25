// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleAvatarConvertFixer.h"

class FMantleAvatarContract;
class UAnimSequence;
class UClass;
class UFactory;
class UObject;
class USkeletalMesh;
class UStaticMesh;

/**
 * What Convert's MetaHuman steps (A-pose, conform, build) share: asset names, making and discarding assets, and the
 * check that a step changed nothing of Epic's, MetaHuman Creator's or the source's.
 */
struct MANTLEEDITOR_API FMantleMetaHumanSteps
{
	/** A name an asset or a file may carry: letters, digits and underscores ("Avatar" for none). */
	static FString AssetTag(const FString& Name);

	/** A new asset Folder/Name (made unique) through Factory, as the scripts' create_asset; null when it could not be made. */
	static UObject* CreateAsset(const FString& Folder, const FString& Name, UClass* Class, UFactory* Factory);

	/** A free package name Folder/Name (made unique), for makers that take a path (Geometry Script's new static mesh). */
	static FString UniquePackageName(const FString& Folder, const FString& Name);

	/** Force-deletes what a failed step made (a failed Apply leaves nothing behind) and empties Made. */
	static void Discard(TArray<TWeakObjectPtr<UObject>>& Made);

	/**
	 * The folders a step must not change for Context.Parts[PartIndex]: /Engine/, /MetaHumanCharacter/, the folders of the
	 * part's original mesh, its skeleton and physics asset (the batch retarget writes onto the skeleton the skin-only body
	 * shares), the part's current mesh's skeleton and physics asset, and the context's source (each with a trailing slash).
	 */
	static TArray<FString> ProtectedRoots(const FMantleConvertContext& Context, int32 PartIndex);

	/**
	 * The dirty packages under Roots, except under Except (the step's own write folder, which may sit inside the source's)
	 * and /Engine/Transient with every package inside it (never saved). Roots { "/" } means every package.
	 */
	static TSet<FString> DirtyUnder(TConstArrayView<FString> Roots, const FString& Except);

	/** False with why (Step names the step) when a package under Roots became dirty since DirtyBefore. */
	static bool CheckUnchanged(TConstArrayView<FString> Roots, const FString& Except, const TSet<FString>& DirtyBefore, const TCHAR* Step, FString& OutError);
};

/** One chain direction the step-02 pose check compared with MetaHuman's A-pose. */
struct MANTLEEDITOR_API FMantlePoseAngle
{
	/** "Spine", "LeftArm upper arm", "LeftArm elbow bend", "LeftLeg calf", "Shoulders (right to left)", ... */
	FString Label;

	double Degrees = 0.0;

	/** Whether it counts toward the stop (false for the hands: reported only). */
	bool bJudged = true;
};

/** What the step-02 pose check found (FMantleMetaHumanPose::ComparePose). */
struct MANTLEEDITOR_API FMantlePoseCheck
{
	/** Every direction compared, judged or reported. */
	TArray<FMantlePoseAngle> Angles;

	/** The largest judged difference's index in Angles; INDEX_NONE when nothing judged was compared. */
	int32 Worst = INDEX_NONE;

	/** Required chains (FMantleMetaHumanPose::RequiredChains) the body's rig, or MetaHuman's, does not have. */
	TArray<FName> MissingOnBody;
	TArray<FName> MissingOnTemplate;

	/** The largest judged difference (degrees); 0 when nothing judged was compared. */
	double WorstDegrees() const { return Angles.IsValidIndex(Worst) ? Angles[Worst].Degrees : 0.0; }

	/** Every required chain on both, something judged, and every judged direction within Limit degrees. */
	bool Passes(const double Limit) const
	{
		return MissingOnBody.Num() == 0 && MissingOnTemplate.Num() == 0 && Angles.IsValidIndex(Worst) && WorstDegrees() <= Limit;
	}
};

/** One retarget chain for FMantleMetaHumanPose::SecantAlign: the target's bones and the source chain mapped to it. */
struct MANTLEEDITOR_API FMantleSecantChain
{
	/** The target chain's name ("Spine", "Neck", "Head", ...): a one-bone Spine aims at Neck's start, a one-bone Neck at Head's. */
	FName Name;

	/** The target chain's bone indices, start to end along the hierarchy. */
	TArray<int32> Bones;

	/** The mapped source chain's joints, component space, start to end; empty when no source chain is mapped. */
	TArray<FVector> Source;
};

/** One bone SecantAlign turned: the world (component-space) rotation about the bone's joint, applied root to leaf. */
struct MANTLEEDITOR_API FMantleSecantTurn
{
	int32 Bone = INDEX_NONE;
	FQuat Rotation = FQuat::Identity;
};

/** One retarget chain a known bone-naming scheme gives: Epic's chain label and the skeleton's start and end bones. */
struct MANTLEEDITOR_API FMantleNamedChain
{
	FName Chain;
	FName Start;
	FName End;
};

/**
 * #751: whether the kept bare-skin parts are a whole body, judged on the posed copy before it is scaled to the default
 * height (FMantleMetaHumanPose::CheckWholeBody). Heights in cm, component space.
 */
struct MANTLEEDITOR_API FMantleWholeBodyCheck
{
	/** Whether the check ran (the body has a head and both foot joints to measure against). */
	bool bChecked = false;

	/** The head joint's height above the lower foot joint, and the kept parts' height (lowest to highest point). */
	double JointSpanCm = 0.0;
	double KeptHeightCm = 0.0;

	/** How much of the joint span the kept parts cover (between the foot joints and the head joint). */
	double CoveredCm = 0.0;

	/** The kept parts come within WholeBodyReach of the joint span of the head joint, and of each foot joint. */
	bool bHead = false;
	bool bLeftFoot = false;
	bool bRightFoot = false;

	/** They cover all but WholeBodyShortfall of the joint span, and are no taller than WholeBodyMaxHeight times it. */
	bool bSpan = false;
	bool bNotTooTall = false;

	bool Passes() const { return !bChecked || (bHead && bLeftFoot && bRightFoot && bSpan && bNotTooTall); }

	/** The parts that look missing, in plain words ("the head", "the legs and feet"); empty when none. */
	TArray<FString> Missing() const;
};

/** What posing a skin-only body into MetaHuman's A-pose made (pipeline steps 02 and 03). */
struct MANTLEEDITOR_API FMantleMetaHumanPoseResult
{
	/** The IK rig made for the body (a UIKRigDefinition) and MetaHuman's A-pose retargeted onto it (one frame). */
	TWeakObjectPtr<UObject> TargetRig;
	TWeakObjectPtr<UAnimSequence> APoseClip;

	/** The body frozen in that pose: the conform's input. */
	TWeakObjectPtr<UStaticMesh> PosedMesh;

	/** Bones on the target rig's retarget chains, start to end along the hierarchy, by chain (Epic's auto definition). */
	TMap<FName, TArray<FName>> Chains;

	/** The ends of the Head and LeftArm chains (the LeftArm's is the gate's hand). HeadBone is None without a Head chain. */
	FName HeadBone;
	FName HandBone;

	/** The head bone in the A-pose clip on the body, component space, cm; unset without a Head chain. */
	TOptional<FVector> HeadJoint;

	/** The ends of the LeftLeg and RightLeg chains (the whole-body check's feet). */
	FName LeftFootBone;
	FName RightFootBone;

	/** #751: the kept parts against the joints, on the posed copy before its scale. A failed check stops the step. */
	FMantleWholeBodyCheck WholeBody;

	/** The step-02 pose check: the body's chains in the clip against SKM_Body's chains of the same names. */
	FMantlePoseCheck PoseCheck;

	/** The gate: how far the hand moved from the bind pose, and how far the posed hand is from the clip's own. */
	double HandMovedCm = 0.0;
	double HandToClipCm = 0.0;

	/** The body was already in MetaHuman's A-pose: the clip's hand is within GateMovedCm of the bind pose's. */
	bool bAlreadyInAPose = false;

	/**
	 * The default height (03): the posed copy's height before the scale (cm), the scale k it was given about the origin
	 * (FMantleAvatarHeight::ConformInputScale; also the posed mesh's FMantleAvatarHeight::InputScaleTag), and its height after.
	 */
	double PosedHeightCm = 0.0;
	double InputScale = 1.0;
	double ScaledHeightCm = 0.0;

	/** Every asset made (in order), for the converter to record, or to discard on failure. */
	TArray<TWeakObjectPtr<UObject>> Created;
};

/**
 * #625, pipeline steps 02 and 03 (retarget to the A-pose, make the posed static mesh), the calls the proven
 * scripts made, in C++:
 *   02. a one-frame clip of SKM_Body's reference pose (MetaHuman's A-pose) on SKM_Body's skeleton; IK rigs made by Epic's
 *       auto definition and auto full-body IK for SKM_Body and the body; an IK Retargeter with Epic's default ops, all on,
 *       fuzzy chain auto-map, then every bone on the body rig's retarget chains auto-aligned chain to chain on the target
 *       (bones read from the rig's chains, never by name) and realigned by SecantAlign (#751); the clip batch-retargeted onto the body (never over an existing
 *       asset); then the guide's pose check (ComparePose): the body's spine, neck, arms, forearms, elbows, thighs, calves,
 *       shoulder line and hip line within PoseCheckDegrees of SKM_Body's chains of the same names (the hands reported,
 *       not judged: they follow the hand's anatomy, and the conform's hand re-estimate redoes their joints). It is judged
 *       against SKM_Body itself, not the chain the retargeter mapped, so a wrong auto-map, a body facing another way or
 *       swapped sides fail it.
 *   03. the body posed by that clip on a component in a private preview scene (OverrideAnimationData ticks the bones at
 *       once); gate: the posed hand sits within GateClipCm of the clip's evaluated hand, and moved more than GateMovedCm
 *       from the bind pose unless the body was already in the A-pose; then the CPU-skinned render vertices (LOD 0) copied
 *       out (Geometry Script, what Persona's Make Static Mesh does); the whole-body check (CheckWholeBody, #751: a part of a
 *       body is never scaled up to full height); then scaled about the origin to the default height (k =
 *       DefaultHeightCm / (its height + 0.5 cm), the conform adds about 0.5 cm), into a
 *       new static mesh: normals and tangents kept, Nanite and collision off, each slot the body's material, k stored on it
 *       as FMantleAvatarHeight::InputScaleTag.
 * Everything goes under Folder; nothing of the body's, SKM_Body's or Epic's is changed. The editor's own level is never used.
 */
class MANTLEEDITOR_API FMantleMetaHumanPose
{
public:
	/** MetaHuman Creator's template body: its reference pose is MetaHuman's A-pose. */
	static const TCHAR* const TemplateBodyPath;

	/** The chains the pose check needs on both the body's rig and MetaHuman's (Epic's standard labels). */
	static const TArray<FName>& RequiredChains();

	/** The pose check (02): no compared direction further than this from SKM_Body's. */
	static constexpr double PoseCheckDegrees = 5.0;

	/** The gate (03): the hand must move further than this from the bind pose (else the body was already in the A-pose)... */
	static constexpr double GateMovedCm = 1.0;

	/** ...and sit this close to the clip's evaluated hand. */
	static constexpr double GateClipCm = 0.01;

	/** A posed copy whose bounds moved less than this from the bind pose's came out unposed (cm). */
	static constexpr double BindCopyCm = 0.01;

	/** The whole-body check (#751), as fractions of the head-to-foot joint span: how near each joint the kept parts must come... */
	static constexpr double WholeBodyReach = 0.10;

	/** ...how much of the span they may leave uncovered... */
	static constexpr double WholeBodyShortfall = 0.10;

	/** ...and how tall they may be (a head above its joint and feet below theirs add about a fifth; a bow or a staff adds more). */
	static constexpr double WholeBodyMaxHeight = 1.35;

	/**
	 * #751, pure (tested): whether Kept, the bounds of the kept bare-skin parts, is a whole body standing on the joints:
	 * Head the head joint (or the spine's top without a Head chain), LeftFoot and RightFoot the leg chains' ends, all in
	 * the same space as Kept. Each joint must lie within WholeBodyReach of the span (the head joint's height above the lower
	 * foot joint) of Kept; Kept must cover all but WholeBodyShortfall of the span, and be no taller than WholeBodyMaxHeight
	 * times it. Not checked (passes) when the span is not positive.
	 */
	static FMantleWholeBodyCheck CheckWholeBody(const FBox3d& Kept, const FVector& Head, const FVector& LeftFoot, const FVector& RightFoot);

	/**
	 * The creator's message for a failed check, in plain words: which parts look missing (or that something too tall is
	 * kept), how tall the kept parts are against the skeleton, and to untick under Strip the slots that hold the missing
	 * bare skin and tick clothing and props. Suggested lists removed slots near the missing parts (may be empty). Pure (tested).
	 */
	static FString WholeBodyMessage(const FMantleWholeBodyCheck& Check, TConstArrayView<FString> Suggested);

	/** SKM_Body, or null with why (the MetaHuman Creator plugin's content is missing). */
	static USkeletalMesh* LoadTemplateBody(FString& OutError);

	/**
	 * The bones of each retarget chain of Rig (a UIKRigDefinition) on Mesh's reference skeleton, start to end along the
	 * hierarchy; a chain whose start is not above its end on Mesh is left out.
	 */
	static TMap<FName, TArray<FName>> RigChainBones(const UObject& Rig, const USkeletalMesh& Mesh);

	/**
	 * The retarget chains of a VRoid/VRM skeleton (J_Bip_C_Hips, J_Bip_L_UpperArm, ...), a layout Epic's auto
	 * characterisation doesn't know, under Epic's labels (Spine, Neck, Head, Left/Right Clavicle, Arm, Leg, Foot and the
	 * fingers) so the retargeter maps them onto MetaHuman's; OutPelvis is the hips. Bones match by name ending, any case.
	 * Empty when Names lacks the hips, spine, head, or either side's upper arm, hand, upper leg or foot. Pure (tested).
	 */
	static TArray<FMantleNamedChain> VRoidChains(TConstArrayView<FName> Names, FName& OutPelvis);

	/**
	 * The step-02 pose check, pure (tested). Body and Template hold, by chain name, the joints (component space, start to
	 * end) of each chain of the body's rig in the clip on the body, and of each chain of SKM_Body's rig in its reference
	 * pose; a chain is compared with the one of the same name. Compared: Spine and Neck (start to end; a one-joint chain runs
	 * on to the start of the next chain, Neck's or Head's); each arm's upper arm and forearm (its direction a quarter and
	 * three quarters along its length, so twist joints in the chain don't matter), its elbow bend, and its hand (the arm's
	 * end to the first joint of the side's Middle finger chain, the knuckle; reported with bJudged false, never part of
	 * Worst or Passes: it follows where the knuckles sit, not the pose); each leg's thigh and calf; the shoulder line
	 * (RightArm's start to LeftArm's) and the hip line (RightLeg's start to LeftLeg's), which catch a body facing another
	 * way or with its sides swapped. Head and clavicles are left out: their joints sit differently rig to rig, and the
	 * conform never sees the skeleton. Every chain of RequiredChains missing on either side is listed.
	 */
	static FMantlePoseCheck ComparePose(const TMap<FName, TArray<FVector>>& Body, const TMap<FName, TArray<FVector>>& Template);

	/**
	 * #751, pure (tested): the "secant" realignment run after Epic's chain-to-chain auto-align. Epic aims each target bone
	 * along the source chain's tangent over [p, p + 0.1] at the bone's chain parameter p, so a 3-bone spine samples thin
	 * slices of MetaHuman's S-curved 5-bone spine and leans 5-8 degrees off, and a one-bone chain (p = 1) is never aimed.
	 * Here each chain bone with a child in its chain is aimed at the source chain between its own parameter and its child's
	 * (both by the target chain's lengths); a one-bone Spine or Neck is aimed from its joint to the next chain's start (Neck's,
	 * Head's), matched to the source from its chain's start to the next source chain's start. Bones go root to leaf (by index;
	 * Parents lists every bone's parent, parents before children), each in the first chain of Chains holding it; chains with
	 * no Source and SkipBone (the retarget root, which Epic aligns on its own) are left. InOutJoints, every bone's
	 * component-space joint, is updated as each turn swings the bone's descendants about its joint.
	 */
	static TArray<FMantleSecantTurn> SecantAlign(const TArray<int32>& Parents, TArray<FVector>& InOutJoints, const TArray<FMantleSecantChain>& Chains, int32 SkipBone);

	/**
	 * Steps 02 and 03 for Body into Folder, Tag naming the assets, the posed mesh scaled to DefaultHeightCm
	 * (FMantleAvatarHeight::TargetCm for Convert). False with why (OutResult.Created lists what was made).
	 */
	static bool PoseIntoAPose(USkeletalMesh& Body, const FString& Folder, const FString& Tag, double DefaultHeightCm, FMantleMetaHumanPoseResult& OutResult,
		FString& OutError);
};

/**
 * MantleAvatarFixerIds::MHPose (the Convert route): one line for a body Strip takes (no assembly, no Mantle reference
 * topology); Apply needs the skin-only body Strip made, poses it (FMantleMetaHumanPose::PoseIntoAPose into <output>/Work, at
 * the default height FMantleAvatarHeight::TargetCm) and records the work for the conform (FMantleConvertContext::MetaHuman,
 * with the scale k). The part keeps pointing at the skin-only body.
 * A failed Apply deletes what it made and leaves the context as it was; so does one that dirtied a protected package
 * outside <output>/Work (FMantleMetaHumanSteps::ProtectedRoots).
 */
class MANTLEEDITOR_API FMantleMetaHumanPoseFixer final : public IMantleAvatarFixer
{
public:
	/** Whether Context.Parts[PartIndex] is a body Convert makes a MetaHuman of (Convert route, no assembly, its original no MetaHuman). */
	static bool IsBodyToConvert(const FMantleConvertContext& Context, int32 PartIndex, const FMantleAvatarContract& Contract);

	virtual FName GetId() const override { return MantleAvatarFixerIds::MHPose; }
	virtual void Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const override;
	virtual bool Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const override;
};

/** Registers FMantleMetaHumanPoseFixer with FMantleAvatarFixers (MantleEditor's StartupModule). */
MANTLEEDITOR_API void RegisterMantleMetaHumanPoseFixer();
