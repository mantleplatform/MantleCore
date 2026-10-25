// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleAvatarConvertFixer.h"

class USkeletalMesh;
class UStaticMesh;
struct FReferenceSkeleton;

/**
 * Chunk 7: a skin to take weights from (FMantleUnriggedAsIs::TransferWeights): the A-pose body (and head) Epic's conform placed
 * inside the model. Its vertices, its triangles, and per vertex NumInfluences (bone, weight) slots; bones index the reference
 * skeleton the new mesh is built on, unused slots weigh 0.
 */
struct MANTLEEDITOR_API FMantleAutoRigSkin
{
	TArray<FVector3f> Positions;

	/** Triangle list of indices into Positions. */
	TArray<int32> Indices;

	/** Slots per vertex; set before the first AddVertex. */
	int32 NumInfluences = 0;
	TArray<int32> Bones;
	TArray<float> Weights;

	/** Adds a vertex with Influences (bone, weight); the heaviest NumInfluences kept, the rest of the slots empty. Its index. */
	int32 AddVertex(const FVector3f& Position, TConstArrayView<TPair<int32, float>> Influences);
};

/** What FMantleUnriggedAsIs::TransferWeights gave each point: NumInfluences (bone, weight) slots per point, weights summing to 1. */
struct MANTLEEDITOR_API FMantleAutoRigWeights
{
	int32 NumInfluences = 0;
	TArray<int32> Bones;
	TArray<float> Weights;

	/** Points above the head cut, all on the head bone; points weighted from the skin's closest point. */
	int32 HeadPoints = 0;
	int32 SkinPoints = 0;

	/** Of the skin points: their mean and largest distance to the skin (cm), and how many were over FMantleUnriggedAsIs::FarCm away. */
	double MeanDistanceCm = 0.0;
	double MaxDistanceCm = 0.0;
	int32 FarPoints = 0;

	/** Point's weight on Bone (0 when it has none). */
	float WeightOf(int32 Point, int32 Bone) const;

	/** Point's heaviest bone; INDEX_NONE without. */
	int32 Dominant(int32 Point) const;

	/** "12000 points: 1800 above the neck on head, 10200 from the skin (mean 1.2 cm, largest 14.0 cm, 90 over 5 cm)". */
	FString ToString() const;
};

/** What FMantleUnriggedAsIs::KeepOneFinger did. */
struct MANTLEEDITOR_API FMantleFingerFix
{
	/** Points weighted to any finger. */
	int32 FingerPoints = 0;

	/** Of those, points whose weights reached two or more fingers (they stretch when the fingers curl apart). */
	int32 MixedPoints = 0;

	/** Points whose weights changed: kept to their nearest finger. */
	int32 Changed = 0;

	/** By finger (its root bone, e.g. index_metacarpal_l): how many points had their weights on it dropped for another finger. */
	TMap<FName, int32> DroppedFrom;

	/** By finger (its root bone): its points' summed distance to their nearest bone of that finger (cm) and how many; its chain's length, hand to last joint (cm). */
	TMap<FName, FVector2D> DistanceTo;
	TMap<FName, double> ChainLength;

	/** Hands whose finger skin lies farther than FMantleUnriggedAsIs::FingerReachCm from their finger bones, on average: their fingers follow the hand rigidly. */
	TArray<FName> RigidHands;

	/** "1200 finger points: 310 on two or more fingers, 340 kept to their nearest finger". */
	FString ToString() const;
};

/** What FMantleUnriggedAsIs::MakeSkinnedMesh made. */
struct MANTLEEDITOR_API FMantleAutoRigResult
{
	/** The model's geometry and textures, skinned to the conform's A-pose body skeleton. */
	TWeakObjectPtr<USkeletalMesh> Mesh;

	/** The finger weights kept to one finger each (KeepOneFinger). */
	FMantleFingerFix Fingers;

	/** How far the model and its skeleton were moved up to put its lowest point on the floor (cm; negative is down). */
	double FloorShiftCm = 0.0;

	/** The head cut: the head bone's height in the A-pose (cm). */
	double HeadCutZ = 0.0;

	FMantleAutoRigWeights Weights;

	/** Every asset made, for the converter to record, or to discard on failure. */
	TArray<TWeakObjectPtr<UObject>> Created;
};

/**
 * Chunk 7, auto-rig for As-is: a model with no rig (a static mesh, or a skeletal mesh with no usable weights) keeps its own geometry
 * and textures, skinned to the MetaHuman body skeleton. The unrigged Convert route's first two steps place Epic's skeleton inside it
 * (FMantleUnriggedBody::MakePosedMesh, then FMantleMetaHumanConform::Conform, whose A-pose body export carries the skeleton); then
 * each vertex of the posed model takes the A-pose body's (and head's) skin weights at its closest point, everything above the head
 * joint goes to the head bone, and a skeletal mesh is built of the posed model on the body's skeleton. No MetaHuman build, texture
 * synthesis or Epic cloud; never a Convert output (FMantleAvatarConverter::IsConvertOutput).
 *
 * Limits: the conform needs a renderer (no -nullrhi) and a human-like face looking forward (+Y); the body skeleton has no jaw, so the
 * face does not animate; hair, capes and skirts follow the nearest body bone rigidly; a model standing on tiptoe (made for heels) has
 * its feet turned flat by the clips, so its heels smear and its ankles pinch.
 */
class MANTLEEDITOR_API FMantleUnriggedAsIs
{
public:
	/** Influences per vertex on the made mesh (the upload keeps eight). */
	static constexpr int32 MaxInfluences = 8;

	/** A point farther than this from the skin is counted (hair, a cape, a skirt's hem): it follows its nearest bone rigidly. */
	static constexpr double FarCm = 5.0;

	/** A model whose lowest point is within this of the floor (cm) is left at its height. */
	static constexpr double FloorToleranceCm = 0.05;

	/**
	 * A hand whose finger skin lies farther than this from its finger bones on average (cm) has fingers the fitted MetaHuman hand
	 * doesn't line up with: bent about joints that aren't inside them they stretch into claws, so they follow the hand rigidly.
	 * A finger's skin lies within about 1 cm of its bones, a glove's within about 2, once the hand is laid on (HandFit*).
	 */
	static constexpr double FingerReachCm = 3.5;

	/**
	 * The hand fit (MakeSkinnedMesh): the conform's body holds its hands where MetaHuman's A-pose puts them, not inside the model's,
	 * so each is laid onto the model's hand (FitRigid). Its targets are the model's points within HandFitRadiusCm of the points that
	 * first took the hand's weights; its sources at most about HandFitSamples of the body hand's skin; HandFitRounds rounds at most.
	 * A fit that moves the hand joint farther than HandFitMaxCm or turns it more than HandFitMaxDegrees, or lays it no closer, is
	 * not taken.
	 */
	static constexpr double HandFitRadiusCm = 25.0;
	static constexpr int32 HandFitSamples = 1500;
	static constexpr int32 HandFitRounds = 60;
	static constexpr double HandFitMaxCm = 25.0;
	static constexpr double HandFitMaxDegrees = 60.0;

	/** FitRigid keeps the closest this share of its point pairs each round (the rest are taken for another part of the model). */
	static constexpr double FitKeepShare = 0.9;

	/** The bone everything above the neck follows. */
	static const FName HeadBone;

	/**
	 * For the panel (chunk 2's As-is choice): whether Selection is a model with no rig that As-is can auto-rig: a static mesh, or a
	 * skeletal mesh with one bone or no usable weights (FMantleUnriggedBody::HasNoUsableWeights). OutNote says why, in plain words:
	 * PlainMessage when it can; when it can't for this editor (no renderer), why. False and empty for anything with a rig of its own.
	 */
	static bool CanAutoRig(const UObject* Selection, FString& OutNote);

	/** Whether Selection has no rig to keep (a static mesh, a skeletal mesh with one bone or no usable weights). */
	static bool HasNoRig(const UObject* Selection);

	/** "This model has no rig, so we'll fit Epic's MetaHuman skeleton inside it and skin it to that. It keeps its own look." */
	static FString PlainMessage();

	/** Whether Context.Parts[PartIndex] is a model with no rig the As-is route auto-rigs (not yet done). */
	static bool IsUnriggedAsIs(const FMantleConvertContext& Context, int32 PartIndex);

	/**
	 * Pure (tested): each of Points takes Skin's weights at its closest point on Skin's surface (the three corners' weights blended by
	 * the point's barycentrics, the heaviest MaxInfluences kept, renormalised); a point above HeadCutZ takes HeadBoneIndex alone.
	 * False with why when Skin has no triangles or no weights.
	 */
	static bool TransferWeights(TConstArrayView<FVector3f> Points, const FMantleAutoRigSkin& Skin, int32 HeadBoneIndex, double HeadCutZ,
		FMantleAutoRigWeights& Out, FString& OutError);

	/**
	 * Pure (tested): a finger's skin follows one finger. Closest-point weights between two fingers blend both (or take the
	 * neighbour's), and those points stretch when the fingers curl apart. For each point weighted to a finger, the finger is the
	 * one whose bones (each a segment from its parent's joint to its own, a fingertip carried on past its last joint) pass
	 * nearest the point; its weights on other fingers are dropped and the rest renormalised (all on that nearest bone when
	 * nothing is left). Fingers are the chains under a "hand_*" bone whose root is named thumb, index, middle, ring or pinky
	 * (Epic's MetaHuman skeleton); only their main bones (<finger>_metacarpal, _01, _02, _03 and the side) are measured, not the
	 * helper bones Epic's rig logic drives (bulge, half, palm, side, pip, dip, slide). Then a hand whose finger points lie
	 * farther than FingerReachCm from those bones on average has its fingers' weights moved onto the hand bone. Joints are
	 * component-space, as Points are.
	 */
	static FMantleFingerFix KeepOneFinger(TConstArrayView<FVector3f> Points, TConstArrayView<FName> Names, TConstArrayView<int32> Parents,
		TConstArrayView<FVector3f> Joints, FMantleAutoRigWeights& InOut);

	/**
	 * Pure (tested): linear blend skinning. Each of Points moves by its weights' blend of each bone's move From -> To (component
	 * transforms by bone, as Bones index them; NumInfluences (bone, weight) slots per point); bInverse moves it back To -> From by the
	 * inverse of that same blend. A point with no weight stays.
	 */
	static void MoveByWeights(TArrayView<FVector3f> Points, int32 NumInfluences, TConstArrayView<int32> Bones, TConstArrayView<float> Weights,
		TConstArrayView<FTransform> From, TConstArrayView<FTransform> To, bool bInverse);

	/**
	 * Pure (tested): the rigid move that best lays Source onto Target, by iterative closest point from Start: each round every
	 * source point (moved) pairs with its nearest target, the closest FitKeepShare of the pairs give the best turn and shift
	 * (Horn's closed form), Rounds rounds; the move whose kept pairs came closest is returned. OutStartCm and OutFitCm: the kept
	 * pairs' mean distance at Start and at the move returned (cm). Start when either is empty.
	 */
	static FTransform FitRigid(TConstArrayView<FVector3f> Source, TConstArrayView<FVector3f> Target, const FTransform& Start, int32 Rounds,
		double& OutStartCm, double& OutFitCm);

	/**
	 * Appends Mesh's LOD 0 (its imported model: vertices, triangles, weights) to InOut, its bones mapped by name onto Target's raw
	 * bones; a bone Target lacks (the face's FACIAL_ joints) maps to its nearest ancestor Target has. False with why.
	 */
	static bool ReadSkin(const USkeletalMesh& Mesh, const FReferenceSkeleton& Target, FMantleAutoRigSkin& InOut, FString& OutError);

	/**
	 * The skinned mesh: Posed's geometry (its LOD 0 source, each slot its material) where it stands (the conform fitted Body to it
	 * there), weighted from Body and Head (TransferWeights; the head cut at Body's head joint) with each of Body's hands first laid
	 * onto the model's (FitRigid; the skeleton's hand joint, its fingers and their skin with it), its lowest point then put on the floor
	 * with the skeleton (the root's children moved by as much), as a new skeletal mesh Folder/SK_<Name>_AsIs on that reference
	 * skeleton and Body's skeleton. False with why (OutResult.Created lists what
	 * was made).
	 */
	static bool MakeSkinnedMesh(UStaticMesh& Posed, USkeletalMesh& Body, USkeletalMesh* Head, const FString& Folder, const FString& Name,
		FMantleAutoRigResult& OutResult, FString& OutError);
};

/**
 * MantleAvatarFixerIds::AsIsRig (the As-is route): one line for a model with no rig. Apply makes the posed mesh into <output>/Work
 * (FMantleUnriggedBody::MakePosedMesh), conforms a MetaHuman Character to it in <output>/<Name> (FMantleMetaHumanConform::Conform),
 * and builds the skinned mesh (FMantleUnriggedAsIs::MakeSkinnedMesh) into <output>; the part becomes that mesh on the metahuman
 * skeleton (FMantleConvertPart::bAutoRigged), for AsIsHeight, Atlas and Bake after it (RigMap skips it). A failed Apply deletes what
 * it made and leaves the context as it was; so does one that dirtied a protected package outside <output>.
 */
class MANTLEEDITOR_API FMantleUnriggedAsIsFixer final : public IMantleAvatarFixer
{
public:
	virtual FName GetId() const override { return MantleAvatarFixerIds::AsIsRig; }
	virtual void Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const override;
	virtual bool Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const override;
};

/** Registers FMantleUnriggedAsIsFixer with FMantleAvatarFixers (MantleEditor's StartupModule). */
MANTLEEDITOR_API void RegisterMantleUnriggedAsIsFixer();
