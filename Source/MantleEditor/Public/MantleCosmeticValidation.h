// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleGlbWriter.h"
#include "MantleSecondaryMotionRule.h"

class FMantleAvatarContract;
struct FReferenceSkeleton;
struct FMantleContractClass;
struct FMantleContractSkeleton;

enum class EMantleCosmeticSeverity : uint8
{
	/** Stops the upload: the worker or the item check would refuse it. */
	Error,
	/** Uploads, but the creator should know (a long garment). */
	Warning,
	/** What the plugin did on the creator's behalf (which LOD it took, which texture it found). */
	Info,
};

/**
 * One finding, written for the creator. Message is the worker's own wording where the worker has
 * the same rule (backend/internal/cook), so the plugin and the server never describe one problem
 * two ways; Hint says what to do about it.
 */
struct MANTLEEDITOR_API FMantleCosmeticIssue
{
	EMantleCosmeticSeverity Severity = EMantleCosmeticSeverity::Error;
	FString Message;
	FString Hint;

	FString ToString() const;
};

/** A joint as the skeleton check sees it: its name and the index of its nearest kept ancestor (INDEX_NONE for a root). */
struct MANTLEEDITOR_API FMantleSkinJointName
{
	FName Name;
	int32 Parent = INDEX_NONE;
};

/** #592: a weighted bone that is not on the skeleton, whose skin weights went to its nearest kept ancestor, Into. */
struct MANTLEEDITOR_API FMantleFoldedBone
{
	FName Bone;
	FName Into;
};

/** #592: what fitting a skin to one contract skeleton did (FMantleCosmeticChecks::FitSkinToSkeleton). */
struct MANTLEEDITOR_API FMantleSkinFit
{
	/** The joints kept, as the skeleton check sees them. */
	TArray<FMantleSkinJointName> Names;

	/** The weighted bones folded into a kept ancestor, in bone order. */
	TArray<FMantleFoldedBone> Folded;

	/** Bones left out: the unweighted helpers off the skeleton plus the folded ones. */
	int32 Dropped = 0;

	/** The most non-zero influences any vertex has after the fold. */
	int32 MostInfluences = 0;
};

/** #594: what fitting an avatar body did with its optional jaw slot (FMantleCosmeticChecks::FitAvatarBodyWithSlots). */
enum class EMantleJawSlot : uint8
{
	/** The body's skeleton has no jaw slot (a MetaHuman body: its jaw is on its face). A pick is left to the item's jawBone check. */
	NoSlot,
	/** The skeleton has the slot and no bone was picked: the body has no jaw, and a bone named like the slot folded like any extra bone. */
	NotPicked,
	/** The picked bone is the slot: named jaw, under head, its weights kept. */
	Kept,
	/** The pick was refused (not a bone of the body, not below head, or the name taken): the fit failed with the reason. */
	Refused,
};

/**
 * #595: one of the creator's bone chains mapped onto a secondary motion slot chain (MantleSecondaryMotionRule.h): the
 * chain starts at StartBone, the body's own bone, and runs down its first child that is off the skeleton each step. Its
 * bones take the slot chain's bones in order; bones past the last slot fold into the last kept one.
 */
struct MANTLEEDITOR_API FMantleSecondaryMapping
{
	/** The contract chain id ("hair_a"). */
	FString ChainId;
	/** The body's own bone the chain starts at. */
	FName StartBone;
	EMantleSecondaryPreset Preset = EMantleSecondaryPreset::Hair;
};

/** #595: one mapping as the upload kept it (FMantleCosmeticChecks::KeepSecondaryChains). */
struct MANTLEEDITOR_API FMantleSecondaryKept
{
	FString ChainId;
	EMantleSecondaryPreset Preset = EMantleSecondaryPreset::Hair;
	/** The body's own bones now named as the chain's first SourceBones.Num() slots, in order. */
	TArray<FName> SourceBones;
	/** How many bones of the creator's chain lie past its last slot: they fold into the last kept one. */
	int32 FoldedPastEnd = 0;
};

/** #595: what fitting an avatar body did with its optional slots (FMantleCosmeticChecks::FitAvatarBodyWithSlots). */
struct MANTLEEDITOR_API FMantleAvatarBodyFit
{
	FMantleSkinFit Fit;
	EMantleJawSlot Jaw = EMantleJawSlot::NoSlot;
	/** The chosen skeleton carries the secondary motion slots (the UE5 and UE4 Mannequins). */
	bool bSecondarySlots = false;
	/** A mapping was refused: the fit failed with the reason. */
	bool bSecondaryRefused = false;
	/** The mappings kept, in the order given. */
	TArray<FMantleSecondaryKept> Kept;
};

/** #482: how an upload picks the LOD it sends, so the triangle-limit hint can say what to do. */
enum class EMantleLODPick : uint8
{
	/** The caller doesn't say: the general hint. */
	Unknown,
	/** The plugin takes the first LOD within the limit (garments, avatar bodies and faces): over it, none is. */
	FirstWithinLimit,
	/** The upload always takes LOD 0 as it is (hair, accessories). */
	LODZeroOnly,
};

/**
 * #480: where one section's UV0 sits (FMantleCosmeticChecks::MeasureUVTiles). A tile is an integer square of UV
 * space, (floor U, floor V); UDIM numbers it 1001 + U + 10 V.
 */
struct MANTLEEDITOR_API FMantleUVTiles
{
	/** The UV0 range of the section's vertices. */
	FBox2f Range = FBox2f(ForceInit);

	/** The tiles the triangles' UV centroids fall in, in UDIM order (V, then U). */
	TArray<FIntPoint> Tiles;

	/** Triangles with a vertex more than UVTileEpsilon outside their centroid's tile: UVs that repeat across tiles. */
	int32 CrossingTriangles = 0;

	int32 Triangles = 0;

	/** Two or more tiles and no triangle across a tile border: islands laid out on UDIM tiles, one texture per tile. */
	bool IsUdim() const { return Tiles.Num() > 1 && CrossingTriangles == 0; }

	/** "1001, 1002, 1003"; a tile outside UDIM's range (U 0 to 9, V from 0) is written "(-1, 0)". */
	FString DescribeTiles() const;
};

/**
 * The local checks of the cosmetic upload panel (A2.4 #283): the same rules as the cook worker
 * (cook/skinnedmesh.go, bodyshape.go, animclip.go, texture checks) and the item check (A2.1),
 * run in the editor before anything is exported or sent, so a creator never waits on a cook to
 * learn what the plugin could have said at once. Pure: no UObjects.
 */
struct MANTLEEDITOR_API FMantleCosmeticChecks
{
	/**
	 * The message for an avatar body with no LOD at the reference topology (plan A2.4 and section
	 * 3.3): a Low export has no such LOD, and a clothed export has body triangles removed.
	 */
	static const TCHAR* NotGarmentCompatibleMessage;

	/**
	 * #625: its hint. A MetaHuman-skeleton avatar body without the reference topology uploads as-is (a Warning: it wears
	 * no 3D clothing), only while as-is uploads are allowed (UMantleSettings::bAllowAsIsAvatars).
	 */
	static const TCHAR* NotGarmentCompatibleHint;

	/**
	 * #625 (owner, 2026-10-02): while as-is uploads are off (the default), an avatar body without Mantle's reference
	 * topology is refused: it has to go through Convert (it becomes a MetaHuman).
	 */
	static const TCHAR* ConvertFirstMessage;
	static const TCHAR* ConvertFirstHint;

	/**
	 * #774: the creator's wording when a skeleton is not one we can use (the technical reason, "bone X has parent Y...",
	 * goes to the log only). UnsupportedSkeleton logs Detail and returns the message.
	 */
	static const TCHAR* UnsupportedSkeletonMessage;
	static FString UnsupportedSkeleton(const FString& Detail);
	/** Message with one full stop at the end: adds it only when it isn't already there. */
	static FString EndWithFullStop(const FString& Message);

	/**
	 * #625 (owner, 2026-10-02): a body Convert made (a MetaHuman of Epic's build, FMantleAvatarConverter::IsConvertOutput)
	 * without Mantle's reference topology uploads, as not clothing-ready yet (a Warning): it animates and shows Epic's
	 * default underwear; clothing comes with a later clothing pipeline.
	 */
	static const TCHAR* NotClothingReadyMessage;
	static const TCHAR* NotClothingReadyHint;

	/**
	 * #625: an avatar body with Mantle's reference topology and no face is headless (the reference body ends at the
	 * neck): refused, never uploaded. The hint names the face pick.
	 */
	static const TCHAR* HeadlessAvatarMessage;
	static const TCHAR* HeadlessAvatarHint;

	/** The note for an avatar body on the UE5 Mannequin skeleton (A2.5): no reference topology, no face. */
	static const TCHAR* UE5MannequinAvatarMessage;

	/** The note for an avatar body on the UE4 Mannequin skeleton (#593): no reference topology, no face. */
	static const TCHAR* UE4MannequinAvatarMessage;

	/** The note for an avatar body on the UEFN Mannequin skeleton (#609): the driver's own, no reference topology, no face. */
	static const TCHAR* UEFNMannequinAvatarMessage;

	/** The panel's "Publish as" option for a new item; hints that point to it name it. */
	static const TCHAR* PublishAsNewItemLabel;

	/**
	 * cook.matchSkeleton: every joint is on Skeleton, every joint's nearest joint ancestor is its
	 * fixture parent (or it has none when that parent is not in the skin), one root, and the
	 * fixture's root present when bRequireRoot. False with the worker's message.
	 */
	static bool MatchSkeleton(TConstArrayView<FMantleSkinJointName> Joints, const FMantleContractSkeleton& Skeleton,
		bool bRequireRoot, FString& OutError);

	/**
	 * Fits Glb's skin (its joints parent-first, ComponentPose and Weighted per joint) to Skeleton. Bones on Skeleton are
	 * kept, each reparented to its nearest kept ancestor (its local transform composed through the bones left out) and
	 * given the skeleton's spelling: FNames match without regard to case, so a MetaHuman outfit's "Root" is the
	 * fixture's "root", as the worker's cooked bone table says (#286). A bone off Skeleton that no vertex weights is left
	 * out silently (cloth and corrective helpers). #592: a weighted bone off Skeleton is folded: its weights move to its
	 * nearest kept ancestor, each vertex's influences on one joint are merged and renormalised, heaviest first, within
	 * Glb.NumInfluences; then it is left out too. A weighted bone with no kept ancestor stays (MatchSkeleton refuses it):
	 * folding never fixes a hierarchy, and a fixture bone under the wrong parent is still refused. #594: the jaw slot
	 * (JawSlotBone) is kept only when bKeepJawSlot (the creator picked it, KeepJawBone); otherwise a bone of that name is
	 * an extra bone like any other, so a body without a picked jaw fits as it did before the slot. #593 (decided): a
	 * weighted bone in NoFoldBones (SupportedFixtureBones: a bone of some supported avatar skeleton) is never folded but
	 * kept, so MatchSkeleton refuses the fit; FitSkinToClass always passes it. Null keeps #592's fold of every weighted
	 * extra (direct calls, tests). #595: a secondary motion slot bone (dyn_*) is kept only when KeptSlotBones names it
	 * (the creator mapped a chain onto it, KeepSecondaryChains); otherwise it is an extra like the unpicked jaw. Pure (tested).
	 */
	static void FitSkinToSkeleton(FMantleGlbMesh& Glb, TConstArrayView<FTransform> ComponentPose, TConstArrayView<bool> Weighted,
		const FMantleContractSkeleton& Skeleton, FMantleSkinFit& OutFit, bool bKeepJawSlot = false, const TSet<FName>* NoFoldBones = nullptr,
		const TSet<FName>* KeptSlotBones = nullptr);

	/**
	 * #593: the bones folding may never remove: every bone of the contract's avatar_body skeletons (metahuman,
	 * ue5mannequin, ue4mannequin, #609's uefnmannequin) but the optional jaw slot. Only a weighted bone on none of them is an extra to fold, so
	 * a MetaHuman or UE5 Mannequin body is never refitted as a UE4 Mannequin one by folding its spine_04, spine_05,
	 * neck_02 or metacarpals away. #595: nor the secondary motion slots, optional like the jaw.
	 */
	static TSet<FName> SupportedFixtureBones(const FMantleAvatarContract& Contract);

	/**
	 * #592: whether Candidate is a better fit than Best when Class's skeletons are tried in order (the earlier wins a
	 * tie). The fit that folds fewer weighted bones wins, so a fit that keeps every weighted bone always beats one that
	 * folds any (before #592 a fit that folded was refused); then the fit that leaves out fewer bones, folded ones
	 * counted, which is what tells a UE5 Mannequin body (its ik_*, interaction, center_of_mass and weapon bones kept)
	 * from a MetaHuman one. #593: FitSkinToClass never folds a weighted bone of a supported skeleton, so a MetaHuman or
	 * UE5 Mannequin body that weights its spine_04, spine_05, neck_02 or metacarpals does not fit the UE4 Mannequin at
	 * all, and a UE4 Mannequin body fits neither of them (its clavicles, neck and fingers hang elsewhere). #609: the UEFN
	 * Mannequin comes last in the class order and is the UE5 Mannequin's hierarchy without its correctives, interaction
	 * and center_of_mass, plus attach, props_root, prop_01 and poi (on no other skeleton). A body that carries any of
	 * those four leaves out fewer bones on it (or, weighting one, fits nothing else), so it wins on the second rule; a
	 * body that carries none leaves out as many or more on it, so the earlier skeleton keeps it on the tie. A UE5
	 * Mannequin body that weights a corrective does not fit it at all (the corrective is a supported skeleton's bone).
	 */
	static bool IsBetterSkeletonFit(const FMantleSkinFit& Candidate, const FMantleSkinFit& Best);

	/**
	 * Tries Class's skeletons (FitSkinToSkeleton on a copy of Glb for each) and keeps the one IsBetterSkeletonFit
	 * ranks first among those MatchSkeleton accepts: Glb fitted to it and OutFit, or null with the worker's message
	 * (UnsupportedSkeletonMessage; the first skeleton's reason goes to the log). Pure (tested).
	 */
	static const FMantleContractSkeleton* FitSkinToClass(FMantleGlbMesh& Glb, TConstArrayView<FTransform> ComponentPose, TConstArrayView<bool> Weighted,
		const FMantleContractClass& Class, const FMantleAvatarContract& Contract, bool bRequireRoot, FMantleSkinFit& OutFit, FString& OutError,
		bool bKeepJawSlot = false);

	/**
	 * #594: the optional jaw slot of an avatar body skeleton (the UE5 and, #593, the UE4 and, #609, the UEFN Mannequin's): a bone named JawSlotBone under
	 * JawSlotParent. The creator picks which of the body's bones is its jaw; kept as the slot, it is what the voice
	 * mouth opens (the item's jawBone). Not a required bone: a body without it is complete.
	 */
	static const FName JawSlotBone;
	static const FName JawSlotParent;

	/** The joint table of a reference skeleton's whole raw bone list (parents by index, parent-first). */
	static TArray<FMantleSkinJointName> JointsOf(const FReferenceSkeleton& Reference);

	/** #594: whether Skeleton has the jaw slot: JawSlotBone, parented to JawSlotParent. */
	static bool HasJawSlot(const FMantleContractSkeleton& Skeleton);

	/**
	 * #594: the bones below JawSlotParent (its whole subtree, not the head itself) of a joint table (parents by index,
	 * parent-first), in table order: what the panel's jaw picker lists. Empty when there is no JawSlotParent. Pure (tested).
	 */
	static TArray<FName> JawCandidates(TConstArrayView<FMantleSkinJointName> Joints);

	/** #594: the one candidate whose name contains "jaw" in any case, else None (none, or more than one): the picker's preselection. */
	static FName PreselectJawBone(TConstArrayView<FName> Candidates);

	/**
	 * #594: keeps Picked, a bone of Glb's skin (joints parent-first, ComponentPose per joint), as the jaw slot instead of
	 * folding it (#592): renamed JawSlotBone and reparented straight to JawSlotParent, its local transform its reference
	 * pose relative to the head's, so its component transform is unchanged (and so are its children's), and its weights
	 * untouched. Bones between the head and it stay where they are and fold or drop as extras. False, with the creator's
	 * message, when Picked is not a bone of the skin, is not below JawSlotParent, or another bone already has the slot's
	 * name. Pure (tested).
	 */
	static bool KeepJawBone(FMantleGlbMesh& Glb, TConstArrayView<FTransform> ComponentPose, FName Picked, FString& OutError);

	/**
	 * #594: the note on an avatar body whose skeleton has the jaw slot: "the mouth moves with voice chat using mech_jaw."
	 * for a kept jaw (KeptFrom, the creator's own name for it), "no jaw bone: this avatar's mouth won't move with voice
	 * chat." without one. A warning, shown as a note: it never stops the upload. Pure (tested).
	 */
	static FMantleCosmeticIssue JawSlotIssue(FName KeptFrom);

	/**
	 * #594: the avatar item's jawBone after FitAvatarBodyWithSlots: JawSlotBone for a kept jaw, none on a skeleton with the
	 * slot and no pick, else Picked as the creator named it (a MetaHuman avatar: none takes its face's default, and the
	 * export checks a named one against the body's and the face's bones). Pure (tested).
	 */
	static FName AvatarJawBone(EMantleJawSlot Jaw, FName Picked);

	/** #595: whether Skeleton carries the secondary motion slots: every contract chain's first bone. */
	static bool HasSecondarySlots(const FMantleContractSkeleton& Skeleton);

	/**
	 * #595: whether a bone of a creator's body counts as a bone of Skeleton for mapping: on it and not one of the optional
	 * secondary motion slots (a creator bone named like a slot is an extra until it is mapped).
	 */
	static bool IsSkeletonBoneForMapping(const FMantleContractSkeleton& Skeleton, FName Bone);

	/**
	 * #595: the bones a chain may start at: every bone of Joints (parent-first) that is not a bone of Skeleton
	 * (IsSkeletonBoneForMapping), in table order. What the panel's Secondary motion pickers list. Pure (tested).
	 */
	static TArray<FName> SecondaryCandidates(TConstArrayView<FMantleSkinJointName> Joints, const FMantleContractSkeleton& Skeleton);

	/**
	 * #595: the creator's chain from Start: Start, then at each step its first child (table order) that is not a bone of
	 * Skeleton, for as long as there is one. Indices into Joints. Pure (tested).
	 */
	static TArray<int32> SecondaryChainFrom(TConstArrayView<FMantleSkinJointName> Joints, int32 Start, const FMantleContractSkeleton& Skeleton);

	/**
	 * #595: the kind of slot a bone's name suggests, in this order: "chest" (breast, pector, chest, boob), "hair" (hair,
	 * ponytail, braid, pigtail), "tail", "back" (cape, cloak, back), "cloth" (skirt, sleeve, coat, cloth, dress, robe,
	 * scarf, tassel); empty for anything else. Case-insensitive. Pure (tested).
	 */
	static FString SecondaryKindOf(FName Bone);

	/**
	 * #595: the panel's first guess. Each bone that starts a chain of extra bones (not a bone of Skeleton, its parent a
	 * bone of Skeleton or a helper with several extra children, and not itself a helper with several) whose name has a
	 * kind (SecondaryKindOf) takes the first free slot chain of that kind below whose slot parent it hangs, in table
	 * order: breast_l and breast_r the chest chains by their side, five hair chains hair_a to hair_e, sleeves tied at
	 * the waist cloth_a and cloth_b. Each with its chain's default preset. Pure (tested).
	 */
	static TArray<FMantleSecondaryMapping> SuggestSecondaryMappings(TConstArrayView<FMantleSkinJointName> Joints, const FMantleContractSkeleton& Skeleton);

	/**
	 * #595: keeps each mapped chain as its slots instead of folding it (#592), in the spirit of KeepJawBone: the chain's
	 * bones (SecondaryChainFrom its StartBone) take the slot chain's names in order, as many as there are slots; the first
	 * is reparented straight to the slot's parent with its local transform its reference pose relative to that parent,
	 * so no bone's component transform changes and no weights move. The chain's bones past the last slot, and any side
	 * branches, stay extras and fold into their nearest kept ancestor (the last slot kept). False, with the creator's
	 * message, for a chain id the contract lacks or mapped twice, a start that is not a bone of the body or is a bone of
	 * Skeleton, a start not below the slot's parent, a bone in two mapped chains, or a slot name another bone already
	 * has. Pure (tested).
	 */
	static bool KeepSecondaryChains(FMantleGlbMesh& Glb, TConstArrayView<FTransform> ComponentPose, TConstArrayView<FMantleSecondaryMapping> Mappings,
		const FMantleContractSkeleton& Skeleton, TArray<FMantleSecondaryKept>& OutKept, FString& OutError);

	/**
	 * FitSkinToClass for an avatar body (root required) with the optional jaw and secondary motion slots. The skeleton is
	 * chosen on the body as it is (no slot kept), so neither the jaw pick nor the mappings ever change it, and the panel
	 * (FMantleCosmeticExport::AvatarSkeletonId) asks the same question to decide whether to offer the picker. On a skeleton
	 * with the slots, PickedJaw (None: no pick) is kept as the jaw (KeepJawBone), the mappings are kept
	 * (KeepSecondaryChains, after the jaw) and the body fitted again with them. Out.Jaw says which happened. Unmapped extras fold as before (#592).
	 * Mappings on a skeleton without slots (a MetaHuman body) are ignored. A refused jaw or mapping returns null with the
	 * reason and says which in Out. Pure (tested).
	 */
	static const FMantleContractSkeleton* FitAvatarBodyWithSlots(FMantleGlbMesh& Glb, TConstArrayView<FTransform> ComponentPose,
		TConstArrayView<bool> Weighted, FName PickedJaw, TConstArrayView<FMantleSecondaryMapping> Mappings, const FMantleContractClass& Class,
		const FMantleAvatarContract& Contract, FMantleAvatarBodyFit& Out, FString& OutError);

	/**
	 * #595: the note on an avatar body whose skeleton has the secondary motion slots and kept some: "hair and cloth swing
	 * as the avatar moves: hair_a (mech_hair_short_01_l, 4 bones, Hair), ...". A warning, shown as a note. Pure (tested).
	 */
	static FMantleCosmeticIssue SecondaryMotionIssue(TConstArrayView<FMantleSecondaryKept> Kept);

	/** #592: how many folded bones the note names before "and N more". */
	static constexpr int32 FoldedBonesListed = 8;

	/**
	 * #592: the note for bones folded into their parents: "2 bones were merged into their parent bones and won't move
	 * on their own: mech_jaw -> head, breast_l -> spine_05." (the list capped at FoldedBonesListed). A warning, which
	 * the panel shows as a note: it never stops the upload. What, when not empty, leads it ("the body"). Pure (tested).
	 */
	static FMantleCosmeticIssue FoldedBonesIssue(const FString& What, TConstArrayView<FMantleFoldedBone> Folded);

	/**
	 * Triangle, vertex (3 x triangles), section and influence limits of Class. #482: NumLODs (the source mesh's LOD
	 * count, 0 when unknown) and Pick choose the triangle limit's hint (TriangleLimitHint).
	 */
	static void CheckMeshLimits(int32 Triangles, int32 Vertices, int32 Sections, int32 MostInfluences,
		const FMantleContractClass& Class, TArray<FMantleCosmeticIssue>& OutIssues, int32 NumLODs = 0, EMantleLODPick Pick = EMantleLODPick::Unknown);

	/**
	 * #482: what to do about Triangles over Limit. A mesh with one LOD is told it has no lower LOD and how to add one
	 * in the Skeletal Mesh editor (Max Triangles at the limit, or the Percent of Triangles ratio); a mesh whose LODs
	 * are all over is told to reduce one; an upload that always takes LOD 0 is told to use a coarser mesh or reduce
	 * LOD 0 to about that percentage. Unknown keeps the general hint. Pure (tested).
	 */
	static FString TriangleLimitHint(int32 Triangles, int32 Limit, int32 NumLODs, EMantleLODPick Pick);

	/** #482: floor(100 x Limit / Triangles), at least 1 and at most 100: the share of triangles a reduction keeps. */
	static int32 ReductionPercent(int32 Triangles, int32 Limit);

	/**
	 * #484: keeps the MaxInfluences heaviest of one vertex's influences (bone, weight > 0; any order in, heaviest first
	 * out) and, when any were dropped, renormalises the rest in the fixed point MSK1 stores: 8-bit weights summing to
	 * exactly 255 by largest remainder (as cook.quantizeSkin quantises), written back as q / 255. A vertex within the
	 * limit keeps its weights. Returns the largest weight dropped as a share of the vertex's total, 0 when none was.
	 * Pure (tested).
	 */
	static float CapInfluences(TArray<TPair<int32, float>>& Influences, int32 MaxInfluences);

	/** #484: above this largest dropped weight the capping note is a warning: the deformation may change visibly. */
	static constexpr float CappedInfluenceWarning = 0.1f;

	/**
	 * #484: the note for Vertices vertices capped at MaxInfluences (CapInfluences), LargestDropped the largest weight
	 * dropped: Info, or a Warning above CappedInfluenceWarning. What, when not empty, leads it ("the face"). Pure (tested).
	 */
	static FMantleCosmeticIssue CappedInfluencesIssue(const FString& What, int32 Vertices, float LargestDropped, int32 MaxInfluences);

	/**
	 * #582, cook.checkAvatarBodyShape: an avatar body's skeleton contract beyond the hierarchy, on its exported joint
	 * table (Joints: the GLB's joints after the fit to Skeleton, parents by index, local reference-pose transforms in
	 * Unreal's axes and cm, which is the component space the cook composes after converting the glTF back). In the
	 * cook's order: every bone limits.json avatar_body.requiredBones names for Skeleton (all the missing ones in one
	 * message); every bone's reference-pose scale within 1 +- BoneScaleTolerance, bones taken in fixture order; then,
	 * when the fixture has a referencePose, each chain within ChainToleranceDegrees of its direction. False with the
	 * cook's message (the text after its ErrBadMesh prefix) for the first rule broken. Avatar bodies only. Pure (tested).
	 */
	static bool CheckAvatarBodyShape(TConstArrayView<FMantleGlbJoint> Joints, const FMantleContractSkeleton& Skeleton,
		const FMantleAvatarContract& Contract, FString& OutError);

	/** cook.checkAvatarBodyBounds on the reference-pose bounds (cm, component space). */
	static void CheckAvatarBodyBounds(const FBox3f& Bounds, const FString& SkeletonId, const FMantleAvatarContract& Contract,
		TArray<FMantleCosmeticIssue>& OutIssues);

	/** cook.CheckAvatarHeight over the body and its face (their tops, cm). */
	static void CheckAvatarHeight(float BodyMaxZ, TOptional<float> FaceMaxZ, const FMantleAvatarContract& Contract,
		TArray<FMantleCosmeticIssue>& OutIssues);

	/**
	 * An avatar's standing height as the cook measures it (#582, cook.AvatarMetrics.StandingHeightCm): the body's top,
	 * raised to its face's top when it has one (item creation does that on a MetaHuman avatar), to 0.1 cm
	 * (cook.RoundMetric).
	 */
	static double AvatarStandingHeightCm(float BodyMaxZ, TOptional<float> FaceMaxZ);

	/**
	 * #581: the height the game plays an avatar StandingHeightCm tall at, at the default height setting (100%), cm:
	 * limits.json characterScale's rule (FMantleCharacterScaleRule, the game's own formula), so within its clamp.
	 */
	static double AvatarPlayingHeightCm(double StandingHeightCm, const FMantleAvatarContract& Contract);

	/**
	 * The panel's line for it: "Plays at 189.7 cm." and, for a body outside characterScale's clamp, that its
	 * character is clamped to the limit (the avatar still shows at its own height). Pure (tested).
	 */
	static FString DescribeAvatarPlayingHeight(double StandingHeightCm, const FMantleAvatarContract& Contract);

	/** A texture the item uses: both sides at most MaxSize (and the upload-wide texture cap). */
	static void CheckTextureSize(const FString& What, int32 Width, int32 Height, int32 MaxSize, const FMantleAvatarContract& Contract,
		TArray<FMantleCosmeticIssue>& OutIssues);

	/** cook.CheckAccessoryBounds: the box's longest edge and its farthest corner from the pivot (the socket). */
	static void CheckAccessoryBounds(const FString& Slot, const FBox3f& Bounds, const FMantleAvatarContract& Contract,
		TArray<FMantleCosmeticIssue>& OutIssues);

	/**
	 * cook.poseDifference: translation distance (cm), the largest angle between corresponding axes
	 * (degrees) and the largest relative axis-length difference of two component-space transforms.
	 */
	static void PoseDifference(const FTransform& A, const FTransform& B, double& OutDistanceCm, double& OutDegrees, double& OutScale);

	/** Within the garment bind-pose tolerance (0.5 cm, 1 degree, 1% scale). */
	static bool WithinBindTolerance(const FTransform& A, const FTransform& B, const FMantleAvatarContract& Contract,
		double* OutDistanceCm = nullptr, double* OutDegrees = nullptr);

	/** How far (UV units) a triangle's vertex may sit outside its centroid's tile before the triangle counts as crossing. */
	static constexpr float UVTileEpsilon = 0.001f;

	/**
	 * #480: the UV0 range and tiles of one section, from its vertices' UVs and its triangles (Indices into UVs, three
	 * per triangle; an index past UVs skips its triangle). Pure (tested).
	 */
	static FMantleUVTiles MeasureUVTiles(TConstArrayView<FVector2f> UVs, TConstArrayView<uint32> Indices);

	/** 1001 + U + 10 V for a tile inside UDIM's range (U 0 to 9, V 0 or more), else INDEX_NONE. */
	static int32 UdimNumber(const FIntPoint& Tile);

	/**
	 * #480: an error when the section What ("section \"Shirt\"") is laid out on UDIM tiles: the upload carries one
	 * texture per section, so every tile would wrap onto it. One tile anywhere is fine (MetaHuman bodies sit in 1002
	 * and are sampled with frac), and so are UVs that repeat a tiling texture across tiles.
	 */
	static void CheckUVTiles(const FString& What, const FMantleUVTiles& Tiles, TArray<FMantleCosmeticIssue>& OutIssues);

	/** Material names: non-empty and unique (sections are identified by name). */
	static void CheckSectionNames(TConstArrayView<FString> Names, TArray<FMantleCosmeticIssue>& OutIssues);

	/** The item's name (1 to 64 characters) and description (at most 1,000). */
	static void CheckNameAndDescription(const FString& Name, const FString& Description, TArray<FMantleCosmeticIssue>& OutIssues);

	/**
	 * The item check's transform bounds (A2.1, backend avatar.checkTransform): each axis of a
	 * location within 50 cm, of a rotation within 180 degrees, of a scale within 0.5 to 2, and no
	 * NaN. Refused, not clamped (DECISIONS.md 2026-09-25), so the creator sees the number. A null
	 * part is absent and not checked. What names the transform ("the acc_hat socket").
	 */
	static void CheckTransform(const FString& What, const FVector3f* Location, const FVector3f* Rotation, const FVector3f* Scale,
		TArray<FMantleCosmeticIssue>& OutIssues);

	static constexpr float MaxTransformOffsetCm = 50.0f;
	static constexpr float MaxTransformDegrees = 180.0f;
	static constexpr float MinTransformScale = 0.5f;
	static constexpr float MaxTransformScale = 2.0f;

	/** Any upload's size cap. */
	static void CheckUploadSize(const FString& What, int64 Bytes, const FMantleAvatarContract& Contract, TArray<FMantleCosmeticIssue>& OutIssues);

	static bool HasErrors(TConstArrayView<FMantleCosmeticIssue> Issues);

	static FMantleCosmeticIssue Error(const FString& Message, const FString& Hint = FString());
	static FMantleCosmeticIssue Warning(const FString& Message, const FString& Hint = FString());
	static FMantleCosmeticIssue Info(const FString& Message);
};
