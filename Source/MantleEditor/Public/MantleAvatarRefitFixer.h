// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleAvatarConvertFixer.h"
#include "MantleCosmeticValidation.h"
#include "MantleGarmentFitter.h"
#include "MantleNailSeating.h"

class FMantleAvatarContract;
class USkeletalMesh;
struct FMeshDescription;

/** #610 Refit: how a garment's shape is carried from the body it was made on to the body it is refitted onto. */
enum class EMantleRefitCorrespondence : uint8
{
	/** The garment's source body is the authoring body: no warp, only the re-bind. */
	Identity,
	/** The two bodies share a vertex topology at some LOD pair (same vertex count, near-identical UVs): vertex i of one is vertex i of the other. */
	Dense,
	/** Otherwise: the shared MetaHuman body skeleton's joints, and points on its bone segments, as landmarks. */
	Joints,
};

MANTLEEDITOR_API const TCHAR* LexToString(EMantleRefitCorrespondence Path);

/** What pushing one refitted LOD out of the authoring body did (FMantleAvatarRefit::PushOutOfBody). */
struct MANTLEEDITOR_API FMantleRefitPushOut
{
	/** Garment vertices with the body within the search, and of those the ones under its skin before and after. */
	int32 Tracked = 0;
	int32 InsideBefore = 0;
	int32 InsideAfter = 0;

	/** The deepest a vertex sat under the skin before, cm (0 when none did). */
	float DeepestCm = 0.0f;

	/** Vertices deeper than FMantleAvatarRefit::MaxPushDepthCm, left where they are (the client's coverage hides the skin there). */
	int32 TooDeep = 0;

	/**
	 * #153: vertices under the skin whose layer twin (FMantleLayerCrossing) is outside it, left where they are: the inner layer
	 * of a padded cup or a lined piece, under its own shell. Pushed, the twin coupling would push the shell with it and fold the piece;
	 * left, the client's coverage hides the skin under the shell.
	 */
	int32 UnderShell = 0;

	/**
	 * #153: vertices under the skin further than FMantleAvatarRefit::OpeningReachCm along the surface from any opening of the
	 * garment, left where they are: skin is only visible at the openings, and the client's coverage hides it under the rest (a
	 * corset cinches tighter than the body; pushing its waist out stretched the front and folded the cups).
	 */
	int32 UnderCover = 0;

	/** The under-skin vertices by depth and by distance to an opening ("under the skin by <0.5 cm 12, ..."); empty when none. */
	FString Detail;

	/** Vertices moved, and the largest move, cm. */
	int32 Moved = 0;
	double MaxMoveCm = 0.0;

	/** With bCoverSkin: skin points that came through the garment between its vertices, before and after CoverSkin (-1: not run). */
	int32 SkinThroughBefore = -1;
	int32 SkinThroughAfter = -1;

	/** "312 of 5120 under the skin (deepest 2.41 cm), 340 moved (largest 2.70 cm), 3 left under". */
	FString ToString() const;
};

/** What refitting one garment or hair mesh onto an authoring body does (FMantleAvatarRefit::Plan). */
struct MANTLEEDITOR_API FMantleRefitPlan
{
	/** The garment's raw reference bones, parents before children. */
	TArray<FName> Names;
	TArray<int32> Parents;

	/**
	 * Component space, one per garment bone: the garment's own reference pose; the pose on its source body (shared
	 * bones at the source body's reference pose, the rest keeping their local transforms), which the vertices are
	 * first re-bound to; and the same on the authoring body, the refitted mesh's reference pose.
	 */
	TArray<FTransform> GarmentComponent;
	TArray<FTransform> SourceComponent;
	TArray<FTransform> TargetComponent;

	/** TargetComponent in bone space: what the refitted mesh's reference skeleton gets. */
	TArray<FTransform> TargetLocal;

	/** Garment bones the authoring body has, which take its pose. */
	int32 NumShared = 0;

	/**
	 * Weighted garment bones the authoring body lacks (a breast or cloth helper), each with the nearest ancestor the body
	 * has (FMantleAvatarRefit::FoldBones). Such a bone keeps its local transform under that ancestor, so its vertices
	 * move exactly as the ancestor's would: its weights are the ancestor's for the refit, as the upload folds them.
	 */
	TArray<FMantleFoldedBone> Folded;

	EMantleRefitCorrespondence Path = EMantleRefitCorrespondence::Identity;

	/** Dense: the LOD of the source body and of the authoring body that share the topology. */
	int32 SourceLOD = INDEX_NONE;
	int32 TargetLOD = INDEX_NONE;

	/** RBF landmarks the field was made from (0 on the identity). */
	int32 NumLandmarks = 0;

	/** Source body space to authoring body space (FMantleRBFInterpolation, the runtime fitter's RBF); bIdentity on the identity. */
	FMantleWarpField Field;

	/** The largest distance a garment vertex moves (every LOD, re-bind and warp together), cm, and on which LOD. */
	double MaxMoveCm = 0.0;
	int32 MaxMoveLOD = INDEX_NONE;

	/**
	 * Overlay slots (nails): every shared bone's source pose is the authoring body's, so each piece is carried rigidly by
	 * its own bones (a fingertip nail by its finger) onto the body, with no warp; however far the source arm and hand were
	 * posed, a nail keeps its place on its finger. Seating (FMantleNailSeating) then sets it on the skin and is the
	 * sanity check: the whole-piece MaxMoveCm cap does not apply.
	 */
	bool bOnBones = false;

	/** P through the field (P itself on the identity). */
	FVector3f Warp(const FVector3f& P) const;

	/** A vertex would move more than FMantleAvatarRefit::MaxMoveCm, and the cap applies (not bOnBones). */
	bool ExceedsMoveCap() const;
};

/**
 * #610, the converter's Refit step: a garment (or hair) made on a body without Mantle's reference topology, or bound
 * to another body's reference pose, carried onto an authoring body that has the topology (a reference MetaHuman body
 * in the project), so the upload accepts it and every client can warp it from that body onto its wearer.
 *
 * Two steps on every LOD's mesh description, one new mesh:
 *   1. the re-bind (FMantleAvatarRepose::RebindMeshDescription): from the garment's own reference pose to the source
 *      body's (a no-op when the garment was bound to its body's pose, as a MetaHuman Creator assembly's are);
 *   2. the warp: every vertex through an RBF field (FMantleRBFInterpolation, the one the client fits garments with)
 *      that takes the source body's surface onto the authoring body's, the dense vertex correspondence when the two
 *      share a topology, else the shared skeleton's joints. Normals, tangents and morph deltas are carried the way the
 *      runtime fitter carries them (the field applied to a 1 cm offset along them, tangents re-orthogonalised, the
 *      binormal sign kept); UVs, skin weights, sections and triangles are untouched.
 * The refitted mesh's reference pose is the authoring body's for every bone the body has; the garment's own extra
 * bones keep their local transforms. Posed in the authoring body's reference pose, the garment sits where the warp put it.
 */
class MANTLEEDITOR_API FMantleAvatarRefit
{
public:
	/** No vertex moves further: a garment that would is probably not made for a body like the authoring body. */
	static constexpr double MaxMoveCm = 25.0;

	/** Dense landmarks picked (farthest-point sampling) from the source body's vertices. */
	static constexpr int32 DensePoints = 500;

	/** The joint landmarks are capped at this many (farthest-point sampling). */
	static constexpr int32 MaxJointLandmarks = 400;

	/** How far two bodies' UVs may be apart, per vertex, to count as the same topology (four texels of a 2048 map). */
	static constexpr float DenseUVTolerance = 1.0f / 512.0f;

	/** Bones every refit needs in the garment and the authoring body: the MetaHuman body skeleton's spine to head. */
	static const TArray<FName>& RequiredBones();

	/**
	 * #740: slots whose pieces sit on one part of the body (shaped nails on the hands) and may carry a skeleton that stops
	 * there. They skip the RequiredBones rule; every weighted bone must still exist on the MetaHuman skeleton, so they are
	 * bound to it all the same. Case-sensitive: API slot ids are lower case.
	 */
	static bool IsOverlaySlot(const FString& Slot);

	/**
	 * The skeleton rule: every RequiredBones in GarmentBones and in BodyBones, and every bone in Weighted (the
	 * garment's bones that carry weight) in BodyBones, so the refitted garment is bound to the authoring body's
	 * skeleton. False with a sentence naming the first missing bone. Pure (tested). bOverlay (IsOverlaySlot) skips
	 * the RequiredBones in the garment (the body still needs them) and keeps the Weighted rule.
	 */
	static bool CheckBones(TConstArrayView<FName> GarmentBones, TConstArrayView<FName> Weighted, TConstArrayView<FName> BodyBones, FString& OutError,
		bool bOverlay = false);

	/**
	 * The weighted bones (Weighted, a subset of Names) BodyBones lacks that have an ancestor (Parents, one per Names
	 * entry) BodyBones has, each with the nearest such ancestor: the bones the upload folds into their parents
	 * (FMantleCosmeticChecks::FitSkinToSkeleton). A weighted bone with no such ancestor is not in the list. Pure (tested).
	 */
	static TArray<FMantleFoldedBone> FoldBones(TConstArrayView<FName> Names, TConstArrayView<int32> Parents, TConstArrayView<FName> Weighted,
		TConstArrayView<FName> BodyBones);

	/** The bones LOD 0 of Mesh gives weight to (its imported model), in reference skeleton order; every bone when it has none. */
	static TArray<FName> WeightedBones(const USkeletalMesh& Mesh);

	/**
	 * The upload's bind check (FMantleCosmeticExport::Build, cook.checkGarmentBindPose): the first bone of Garment's LOD 0
	 * that carries weight whose reference pose (component space) is outside the contract's bind tolerance of Body's.
	 * True, with the bone, distance and angle, when one is; false when every weighted bone Body has is within it.
	 */
	static bool FindBindOffset(const USkeletalMesh& Garment, const USkeletalMesh& Body, const FMantleAvatarContract& Contract,
		FName& OutBone, double& OutCm, double& OutDegrees);

	/**
	 * The first LOD pair of Source and Target with the same vertex count and UVs within DenseUVTolerance (render
	 * vertices, as the fitter reads them), and their positions. False when none shares the topology. #620: a LOD whose
	 * reference topology is its mesh description's (FMantleCosmeticExport::HasReferenceTopology: a catalog reference body,
	 * a body written in its order) is read from its description, in the reference order, never in the build's render order.
	 */
	static bool FindDenseCorrespondence(const USkeletalMesh& Source, const USkeletalMesh& Target, int32& OutSourceLOD, int32& OutTargetLOD,
		TArray<FVector3f>* OutSourcePositions = nullptr, TArray<FVector3f>* OutTargetPositions = nullptr);

	/**
	 * The RBF field taking every From onto its To (FMantleRBFInterpolation::GenerateWeights over all of From, then
	 * GenerateMeshSamples). False with why when there are fewer than four landmarks or the counts differ.
	 */
	static bool MakeField(TConstArrayView<FVector3f> From, TConstArrayView<FVector3f> To, FMantleWarpField& OutField, FString& OutError);

	/**
	 * The refit of Garment onto Target: the poses, the field and the largest vertex move (read from the garment's
	 * imported LOD models). SourceBody is the body the garment was made on (the character's own); null when unknown,
	 * and then the garment's own reference pose stands for it (joint landmarks only). bAllowDense false forces the
	 * joint landmarks (tests). False with why when the skeleton rule fails or the field cannot be made; a move over
	 * MaxMoveCm is not a failure here (Out.MaxMoveCm says it). A garment's weighted bones the body lacks are folded
	 * (FoldBones, Out.Folded) before the skeleton rule. bOverlay: the piece is an IsOverlaySlot one (CheckBones, nothing
	 * folded), and it is carried on its bones (FMantleRefitPlan::bOnBones).
	 */
	static bool Plan(const USkeletalMesh& Garment, const USkeletalMesh* SourceBody, const USkeletalMesh& Target, bool bAllowDense,
		FMantleRefitPlan& Out, FString& OutError, bool bOverlay = false);

	/**
	 * One LOD's mesh description refitted by InPlan (the class comment's two steps). OutMaxMoveCm gets its largest vertex
	 * move. False with why when it has no skin weights.
	 */
	static bool RefitMeshDescription(FMeshDescription& Mesh, const FMantleRefitPlan& InPlan, double& OutMaxMoveCm, FString& OutError);

	/** Garment vertices left under the authoring body's skin by the warp are pushed out to this clearance, cm. */
	static constexpr float PushOutCm = 0.3f;

	/** ... unless they sit deeper than this, cm: a part made to pass under the skin (the client's coverage hides it). */
	static constexpr float MaxPushDepthCm = 6.0f;

	/** ... or further than this from any opening of the garment, along its surface, cm (#153: skin is only visible at the openings). */
	static constexpr float OpeningReachCm = 4.0f;

	/**
	 * Each vertex's distance along the surface (Dijkstra over the edges) to the nearest opening of Garment: an edge with one
	 * triangle. Max for a vertex no opening reaches (a closed surface, or one with no triangles). Pure (tested).
	 */
	static TArray<float> DistanceToOpenings(const FMantleSkinnedMeshLOD& Garment);

	/**
	 * After the warp: every vertex of Mesh under Body's skin (FMantleGarmentClearance, the open neck ring left out) is
	 * pushed out to PushOutCm, and every vertex at the skin up to it, by the client's clearance restore
	 * (FMantleClearanceRestore: a smoothed push field, layer twins moved together, triangles never turned over). Vertices
	 * deeper than MaxPushDepthCm stay, and so do an inner layer under its own shell (#153, FMantleRefitPushOut::UnderShell:
	 * a vertex under the skin whose layer twin is outside it) and a vertex further than OpeningReachCm from any opening
	 * (FMantleRefitPushOut::UnderCover). Positions only. False with why on bad inputs.
	 */
	static bool PushOutOfBody(FMeshDescription& Mesh, const FMantleSkinnedMeshLOD& Body, FMantleRefitPushOut& Out, FString& OutError, bool bCoverSkin = false);

	/** The same on a surface (one point per welded vertex, its normals and triangles): Garment's positions are moved. bCoverSkin: CoverSkin after. */
	static bool PushOutOfBody(FMantleSkinnedMeshLOD& Garment, const FMantleSkinnedMeshLOD& Body, FMantleRefitPushOut& Out, FString& OutError, bool bCoverSkin = false);

	/** Skin under a garment triangle is kept at least this far under it by CoverSkin, cm (PushOutCm, as at the vertices). */
	static constexpr float SkinCoverCm = PushOutCm;

	/** CoverSkin looks at skin within this of the garment, cm: further out it is not under this garment (an arm beside a top). */
	static constexpr float SkinCoverSearchCm = 2.0f;

	/** CoverSkin's passes. */
	static constexpr int32 SkinCoverPasses = 3;

	/**
	 * CoverSkin only answers skin at most this far through a garment triangle, cm: a kneecap between vertices comes through
	 * a little; skin further out is beside the garment's edge (a neckline, a waistband, a cuff), not under it.
	 */
	static constexpr float SkinCoverMaxThroughCm = 0.5f;

	/** ... whose normal agrees with the triangle's at least this much (a hem's fold turns away from the skin), ... */
	static constexpr float SkinCoverMinDot = 0.7f;

	/** ... and moves no garment vertex more than this in all, cm. */
	static constexpr float SkinCoverMaxMoveCm = 1.0f;

	/**
	 * Pure (tested): skin that comes through a garment between its vertices (a kneecap under a coarse trouser leg, where every
	 * vertex is outside the skin) is covered. Each Body point within SkinCoverSearchCm whose closest garment point lies inside
	 * a garment triangle (not on its open edge) facing the way the skin does (SkinCoverMinDot), less than SkinCoverCm under it
	 * and at most SkinCoverMaxThroughCm through it, moves that triangle's corners out along the triangle's normal by what it
	 * lacks (the most any such point asks of a corner), SkinCoverPasses passes, no vertex more than SkinCoverMaxMoveCm in all. A garment layer facing the skin (an inner lining) is left alone. Returns the skin points through
	 * the garment (above it) before; OutAfter, after.
	 */
	static int32 CoverSkin(FMantleSkinnedMeshLOD& Garment, const FMantleSkinnedMeshLOD& Body, int32& OutAfter);

	/**
	 * A NEW skeletal mesh, PackagePath/AssetName, Garment refitted by InPlan on every LOD (FMantleAvatarRepose::
	 * WriteWithPose), with InPlan.TargetLocal as its reference pose. Garment is never modified. Null with why when a LOD
	 * cannot be refitted or a vertex would move more than MaxMoveCm (not bOnBones). OutMaxMoveCm: the largest move written.
	 * SeatBody (#740, nails): when given, each LOD is then seated on it (FMantleNailSeating), after the move check;
	 * OutSeating gets the last LOD's result. A bOnBones plan needs it, and fails when a nail is not seated or its seat
	 * moves it more than FMantleNailSeating::MaxSeatMoveCm (the nail is not where its finger bone puts a fingertip).
	 * PushOutBody (not bOnBones): when given, each LOD is then pushed out of it (PushOutOfBody); OutPushOut gets LOD 0's result.
	 */
	static USkeletalMesh* WriteRefit(USkeletalMesh* Garment, const FMantleRefitPlan& InPlan, const FString& PackagePath, const FString& AssetName,
		double& OutMaxMoveCm, FString& OutError, const FMantleSkinnedMeshLOD* SeatBody = nullptr, FMantleNailSeatingResult* OutSeating = nullptr,
		const FMantleSkinnedMeshLOD* PushOutBody = nullptr, FMantleRefitPushOut* OutPushOut = nullptr);

	/**
	 * The fix list's words when Mantle's reference body cannot be measured or loaded and no more specific
	 * provider error is available.
	 */
	static FString NoReferenceBodyText();
};

/**
 * MantleAvatarFixerIds::Refit, after Assembly and before Triangles. Analyze proposes, for each garment and hair part,
 * a refit onto its authoring body (the part's AuthoringBody when it has the reference topology, else the character's
 * own body when it has, else Mantle's neutral reference body, built once into the project by
 * FMantleReferenceBodies::Build and found by FMantleReferenceBodies::GetClosest) when the part's weighted bones
 * are bound outside the upload's bind tolerance of that body's reference pose, or when the character's own body has
 * no reference topology (the piece was shaped for a body no client can fit from). Apply writes the refitted mesh under
 * the output folder and points the part at it and at the authoring body and its face. When the reference body is needed
 * and the project hasn't built it yet, the line offers to build it, and Apply does so (Epic's MetaHuman cloud) first.
 */
class MANTLEEDITOR_API FMantleAvatarRefitFixer final : public IMantleAvatarFixer
{
public:
	virtual FName GetId() const override { return MantleAvatarFixerIds::Refit; }
	virtual void Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const override;
	virtual bool Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const override;

	/** The bodies a part's refit uses: where it was made, and where it goes. */
	struct FBodies
	{
		/** The character's own body (the assembly's), null outside an assembly. */
		USkeletalMesh* Source = nullptr;

		/**
		 * The authoring body (reference topology) and its face; null when there is none. Mantle's reference body is the
		 * project's own build of it (FMantleReferenceBodies).
		 */
		USkeletalMesh* Target = nullptr;
		USkeletalMesh* TargetFace = nullptr;

		/** The part carries no authoring body and its own character's body has no reference topology. */
		bool bSourceLacksTopology = false;

		/** No Target: why Mantle's reference body is not available (a phrase that follows "Can't: "). */
		FString NoTargetWhy;

		/** No Target only because this project hasn't built Mantle's reference body yet: Apply builds it first. */
		bool bReferenceNotBuilt = false;
	};

	/** The bodies for Part, given the character's own body (null outside an assembly): the class comment's order. */
	static FBodies ResolveBodies(const FMantleConvertPart& Part, USkeletalMesh* CharacterBody, const FMantleAvatarContract& Contract);

	/**
	 * The fix for Context.Parts[PartIndex] with Bodies already resolved (Analyze's body, for the tests): none when the
	 * part is bound to Bodies.Target within tolerance and its own body is fine; a greyed line when there is no target,
	 * the skeleton rule fails, or a vertex would move more than FMantleAvatarRefit::MaxMoveCm.
	 */
	static void AnalyzePart(const FMantleConvertContext& Context, int32 PartIndex, const FBodies& Bodies, const FMantleAvatarContract& Contract,
		TArray<FMantleConvertFix>& OutFixes);
};

/** Registers FMantleAvatarRefitFixer with FMantleAvatarFixers (MantleEditor's StartupModule). */
MANTLEEDITOR_API void RegisterMantleAvatarRefitFixer();
