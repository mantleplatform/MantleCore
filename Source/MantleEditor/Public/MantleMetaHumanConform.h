// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleAvatarConvertFixer.h"

class UObject;
class USkeletalMesh;
class UStaticMesh;
struct FMantleSilhouette;

/**
 * The guide's hand numbers on an A-pose body export (Epic's MetaHuman skeleton, whose bone names are Epic's own): the hand
 * and finger joints against the skin, and the elbows. Logged after the conform; the guide's bars (worst finger joint
 * <= 3.0 cm, elbow 36.6 +- 5 degrees) are judged after the build.
 */
struct MANTLEEDITOR_API FMantleHandMeasure
{
	/** The largest distance from a hand or finger joint (hand_l, hand_r and every bone below them) to the nearest skin vertex (cm), and which. */
	double WorstJointCm = -1.0;
	FName WorstJoint;

	/** Each hand joint's distance to the skin (cm): left, right; -1 unmeasured. */
	double HandCm[2] = { -1.0, -1.0 };

	/** Each elbow's bend, the angle between the upper arm and the forearm (degrees): left, right; -1 unmeasured. */
	double ElbowDegrees[2] = { -1.0, -1.0 };

	/** "worst hand/finger joint middle_03_l 1.61 cm; hands 0.95 / 1.02 cm; elbows 36.9 / 36.9 degrees". */
	FString ToString() const;
};

/** Where the front portrait's camera stands (step 04's framing). Component space, cm; the body faces +Y. */
struct MANTLEEDITOR_API FMantlePortraitFraming
{
	/** What the camera looks at: the middle of the head region, 1 cm below its centre. */
	FVector Target = FVector::ZeroVector;

	/** The camera: StandoffCm * Size beyond the face's front, at the target's height. */
	FVector Camera = FVector::ZeroVector;

	/** The front-most head vertex in the central band (ears and shoulders left out). */
	double FaceFrontY = 0.0;

	/** Head height above the head joint over HeadRefCm, at least 1: a bigger head moves the camera back in proportion. */
	double Size = 1.0;
};

/** What the conform made (pipeline step 04). */
struct MANTLEEDITOR_API FMantleMetaHumanConformResult
{
	/** The MetaHuman Character (a UMetaHumanCharacter), conformed, committed to the A-pose, its hand joints re-estimated. */
	TWeakObjectPtr<UObject> Character;

	FMantlePortraitFraming Framing;

	/** "INPUTCHECK ...: MATCH" for the posed mesh. */
	FString InputLine;

	/** Face landmarks found: curves and points, and which portrait gave them ("basecolor", "lit"). */
	int32 LandmarkCurves = 0;
	int32 LandmarkPoints = 0;
	FString LandmarkPortrait;

	/** The portraits as written (PortraitFile), for the record. */
	TArray<FString> PortraitFiles;

	/** The hand re-estimate's template read (EImportErrorCode) and vertex count. */
	FString JointReestimate;

	/** The guide's hand numbers on the final A-pose export (MeasureHands), for the record. */
	FMantleHandMeasure Hands;

	/** The final A-pose body and head exports (Epic's MetaHuman skeleton, its joints where the conform put them); null until made. */
	TWeakObjectPtr<USkeletalMesh> APoseBody;
	TWeakObjectPtr<USkeletalMesh> APoseHead;

	/** Every asset made (the character, its A-pose head and body exports), for the converter to record, or to discard on failure. */
	TArray<TWeakObjectPtr<UObject>> Created;
};

/**
 * Pipeline step 04, with the hand re-estimate:
 *   1. input check: Epic's conform reader against the posed mesh as it renders (MATCH, 0.000 cm);
 *   2. a front portrait in a private preview scene (base colour and lit, 1024 square, FOV 30, three
 *      directional lights and sky, every texture streamed in), captured until two captures agree; Epic's face landmark
 *      tracker on it. No landmarks on either is a clean stop: Convert needs a human-like face looking forward. Framing
 *      (FramePortrait) is aimed by the head joint the A-pose step found from the rig's Head chain, never a bone name;
 *   3. a new MetaHuman Character <CharacterFolder>/<Name>, opened for editing;
 *   4. ConformToTargetMeshes: combined parts, the "combined" pipeline, joints estimated from the mesh, auto-solve, the
 *      landmark curves and the portrait's camera;
 *   5. CommitPosedStateAsAPose; ExportGeometry of the A-pose head and body into <CharacterFolder>/APose (the character's
 *      own folder), read back by name: <character>_Body and <character>_Head (an
 *      existing asset of either name stops the step: it is never overwritten);
 *   6. The hand re-estimate: that A-pose export back through GetMeshForBodyConformingFromTemplate, ConformBodyToTarget (in the A-pose,
 *      joints estimated) and FitFaceStateFromBodyWithEyesTeethTemplate: the hand and finger joints go onto the committed
 *      skin, the skin does not move; then the A-pose exported again, with those joints.
 * The character is closed for editing at the end (the build opens it again). Nothing is saved here (the converter saves
 * what it made); the editor's own level is never used.
 */
class MANTLEEDITOR_API FMantleMetaHumanConform
{
public:
	/** A head (joint to top) up to this keeps the human standoff. */
	static constexpr double HeadRefCm = 25.0;

	/** The camera's distance beyond the face's front for a human-sized head (cm). */
	static constexpr double StandoffCm = 55.0;

	/** Without a head joint, the head is taken to start this far below the top (cm). */
	static constexpr double NoHeadDropCm = 15.0;

	/** The portrait's square size (pixels) and field of view (degrees). */
	static constexpr int32 PortraitSize = 1024;
	static constexpr float PortraitFov = 30.0f;

	/** How long the portrait's settle waits for textures to stream in, per capture (seconds). */
	static constexpr float StreamSeconds = 10.0f;

	/** How long each settle keeps the portrait mesh's textures forced fully resident (PrestreamTextures, seconds). */
	static constexpr float PrestreamSeconds = 60.0f;

	/**
	 * The script's face_target, pure (tested): the head region is every vertex above HeadJoint.Z - 6 cm; the target its
	 * box's middle (X), the joint's Y, 1 cm below the box's middle (Z); the face's front the largest Y of the region's
	 * vertices within 35% of the box's width of its middle; the camera StandoffCm * Size beyond it. Facing +Y. Without a
	 * head joint (no Head chain): the joint's height NoHeadDropCm below the top, its Y the region's middle.
	 */
	static FMantlePortraitFraming FramePortrait(TConstArrayView<FVector3f> Vertices, const TOptional<FVector>& HeadJoint);

	/** Where the portrait Label ("basecolor", "lit") of Name is written: Saved/MantleConvert/<Name>/landmarks/<Name>_portrait_<Label>.png. */
	static FString PortraitFile(const FString& Name, const TCHAR* Label);

	/**
	 * The guide's hand numbers on Body, an A-pose body export on Epic's MetaHuman skeleton (bind pose = the A-pose): every
	 * hand and finger joint's distance to the nearest LOD 0 vertex, and the elbow bends (upperarm, lowerarm, hand). False
	 * when the skin can't be read or the skeleton has no hand_l / hand_r.
	 */
	static bool MeasureHands(const USkeletalMesh& Body, FMantleHandMeasure& Out);

	/**
	 * Step 04 for Posed (the A-pose step's mesh), the character <CharacterFolder>/<Name> (Name made an asset name here).
	 * HeadJoint is in the posed mesh's space: scaled by its default-height k (FMantleConvertMetaHumanWork::ConformHeadHint).
	 * False with why; OutResult.Created lists what was made either way.
	 */
	static bool Conform(UStaticMesh& Posed, const TOptional<FVector>& HeadJoint, const FString& CharacterFolder, const FString& Name,
		FMantleMetaHumanConformResult& OutResult, FString& OutError);
	/**
	 * #770, "keep the face, choose a body": the clean head (FMantleMetaHumanHeadOnly::MakeCleanHead, sized for HeightCm) conformed on
	 * its own. The same input check and portrait as Conform, then a new character <CharacterFolder>/<Name> with its Height
	 * constraint at HeightCm and ConformToTargetMeshes with the head only and MetaHuman's head_only pipeline: the face is the
	 * character's, the body MetaHuman's own (no A-pose export or hand re-estimate: nothing about the body was fitted). No
	 * landmarks is a plain stop ("we couldn't see a face"). SourceBuild (the source's torso as shares of its height,
	 * FMantleMetaHumanHeadOnly::MeasureBuild) seeds the Weight, Fat and Muscularity sliders (FMantleBodySilhouette::Seed): below the
	 * default is allowed, each clamped to its range. False with why; OutResult.Created lists what was made either way.
	 */
	static bool ConformHeadOnly(UStaticMesh& Head, const TOptional<FVector>& HeadJoint, double HeightCm, bool bFeminine, const FString& CharacterFolder,
		const FString& Name, FMantleMetaHumanConformResult& OutResult, FString& OutError, const FMantleSilhouette* SourceBuild = nullptr);

	/**
	 * The body the creator picked for a kept face (#770, #875): Character (a UMetaHumanCharacter open for editing) keeps MetaHuman's
	 * own parametric body, set on its Masculine/Feminine constraint, and the body state is committed (which refits the face onto
	 * its neck). The height is not touched. False with why when the constraint is missing or the subsystem is not running.
	 */
	static bool ChooseParametricBody(UObject& Character, bool bFeminine, FString& OutError);
};

/**
 * MantleAvatarFixerIds::MHConform (the Convert route): one line for a body Convert makes a MetaHuman of; Apply needs the
 * A-pose step's work (FMantleConvertContext::MetaHuman), runs FMantleMetaHumanConform::Conform with the character in
 * <output>/<Name> and the head joint scaled as the posed mesh was (FMantleConvertMetaHumanWork::ConformHeadHint), and
 * records the character. A failed Apply deletes what it made and leaves the context as it was; so
 * does one that dirtied a protected package outside <output>/<Name> (FMantleMetaHumanSteps::ProtectedRoots).
 */
class MANTLEEDITOR_API FMantleMetaHumanConformFixer final : public IMantleAvatarFixer
{
public:
	virtual FName GetId() const override { return MantleAvatarFixerIds::MHConform; }
	virtual void Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const override;
	virtual bool Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const override;
};

/** Registers FMantleMetaHumanConformFixer with FMantleAvatarFixers (MantleEditor's StartupModule). */
MANTLEEDITOR_API void RegisterMantleMetaHumanConformFixer();
