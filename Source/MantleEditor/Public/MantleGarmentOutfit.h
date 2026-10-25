// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleGlbWriter.h"

class USkeletalMesh;
struct FMantleConvertContext;
struct FMantleConvertGarment;
struct FMantleConvertMetaHumanWork;
struct FMantleOutfitSection;
struct FMantlePackReview;
struct FMeshDescription;
struct FMantleSkinnedMeshLOD;
struct FReferenceSkeleton;

/** One garment FMantleGarmentOutfit made into an Outfit. */
struct MANTLEEDITOR_API FMantleGarmentOutfitPiece
{
	/** The creator's garment (never modified) and the Outfit made from it; Outfit is empty when it failed (Error says why). */
	FString Garment;
	FString Outfit;
	FString Error;

	int32 Vertices = 0;

	/** Garment vertices that had no MetaHuman skin within NearCm, and the average distance from a vertex to the skin (cm). */
	int32 FarVertices = 0;
	double MeanSkinCm = 0.0;

	/** What pushing it out of the MetaHuman's skin did (FMantleRefitPushOut::ToString). */
	FString PushOut;

	/**
	 * How far it reaches below the MetaHuman's sole (cm, 0 when it doesn't): a shoe's sole, or a high heel, made for a foot on
	 * tiptoe, once its foot is turned flat (HeelMinCm or more: no Outfit, Error says why).
	 */
	double HeelCm = 0.0;
};

/** What FMantleGarmentOutfit::Make did. */
struct MANTLEEDITOR_API FMantleGarmentOutfitResult
{
	/** Where everything went: <body folder>/Converted/<body>_Outfit. */
	FString OutputFolder;

	/** The MetaHuman Character conformed to the body, and the copy of its merged body and head each Outfit was authored on. */
	FString Character;
	FString AuthoringBody;

	/** The A-pose step's default-height scale k (the garment is scaled by it too). */
	double InputScale = 1.0;

	/** How the posed body was moved onto the MetaHuman (cm), and how far its skin then is from the MetaHuman's on average (cm). */
	FVector3f Offset = FVector3f::ZeroVector;
	double BodyGapCm = 0.0;

	/** The conformed MetaHuman's limbs turned onto MetaHuman's A-pose: "upperarm_l 7.4, ..." (degrees). */
	FString APoseTurns;

	/** The body's feet and toes turned to the MetaHuman's before its clothing is posed: "foot_l 24.0, ..." (degrees). */
	FString FeetTurns;

	TArray<FMantleGarmentOutfitPiece> Pieces;
	TArray<FString> Notes;
};

/**
 * "Make Outfit from garment": a third-party character's separate clothing mesh, plus that character's bare body, as a
 * real MetaHuman Outfit (UChaosOutfitAsset) whose one authored size is a MetaHuman built from that same body. The
 * Outfit then uploads like any MetaHuman Outfit: FMantleParametricOutfit resizes it onto Epic's standard bodies.
 *   1. The body goes through Convert's own MetaHuman steps only (Strip, MHPose, MHConform; no build): a MetaHuman Character
 *      conformed to the body in MetaHuman's A-pose at the default height. A body with no USkeleton gets one first, on a copy.
 *   2. Each garment is posed the way the body was: its vertices skinned by the A-pose clip's bones (by name; a garment bone
 *      the body lacks follows its parent), scaled by the same k, and moved by the offset that puts the posed body on the
 *      MetaHuman's skin (PoseMesh, AlignTranslation), with the body's feet turned to the MetaHuman's first (Feet). The garment
 *      keeps its shape: instead, the MetaHuman's skin is moved onto the original body's (CarryWithSkin, skin onto skin), and
 *      that shape is the body the Outfit is made on, so Epic's resize takes the garment from the body it was made for.
 *   3. Its skin weights are taken from that reshaped MetaHuman (TransferWeights), so it is on MetaHuman's bones.
 *   4. The MetaHuman's arms and legs are turned onto MetaHuman's own A-pose (TurnToTemplate), body and garment alike, and
 *      the garment is pushed out of the skin where it sits under it (FMantleAvatarRefit::PushOutOfBody, as the refit does).
 *   5. FMantleParametricOutfitAuthoring::Create makes the Outfit, its source body that reshaped, turned body (saved as a copy).
 * The creator's assets are never modified; everything is saved under the output folder.
 */
struct MANTLEEDITOR_API FMantleGarmentOutfit
{
	/**
	 * Package metadata key on every Outfit Make writes (the garment it was made from). FMantleParametricOutfit::Prepare
	 * pushes such an Outfit's resized sizes out of their body's skin too, as Make pushes the authored size.
	 */
	static const FName MadeFromGarmentKey;

	/**
	 * Package metadata key with the version of Make that wrote the Outfit (MadeVersion). An Outfit made earlier from the same
	 * garment is reused only when it is this version, so a fit fix remakes the Outfits made before it.
	 */
	static const FName MadeVersionKey;

	static const TCHAR* const MadeVersion;

	/** A garment vertex further than this from every MetaHuman skin vertex is counted as far (FarVertices). */
	static constexpr double NearCm = 10.0;

	/**
	 * A piece reaching this far below the MetaHuman's sole (cm) or further is a high heel, and gets no Outfit: on a flat
	 * foot its heel goes into the ground, and the resize crumples it (until the game lifts the foot, GhzGarage/mantle#1051).
	 * A shoe's own sole reaches a centimetre or two below it.
	 */
	static constexpr double HeelMinCm = 4.0;

	/** The posed body must land within this of the A-pose step's own posed copy (bounds, cm), or the garment's pose is not the body's. */
	static constexpr double PoseCheckCm = 1.0;

	/** Skin vertices blended per garment vertex. */
	static constexpr int32 TransferNeighbours = 4;

	/**
	 * Pure (tested): Mesh's vertices (bind pose, Mesh.Joints' reference pose) skinned into Posed (one component-space
	 * transform per joint of Mesh.Joints), then scaled by Scale about the origin; normals and tangents turned with them.
	 * Joints and weights are left as they were. False with why when Posed does not match the joints or a weight names no joint.
	 */
	static bool PoseMesh(FMantleGlbMesh& Mesh, TConstArrayView<FTransform> Posed, double Scale, FString& OutError);

	/**
	 * Pure (tested): the translation that moves From onto To (iterated nearest points, pairs further than 20 cm left out),
	 * and OutMeanCm, the average distance from a point of From, so moved, to its nearest point of To.
	 */
	static FVector3f AlignTranslation(TConstArrayView<FVector3f> From, TConstArrayView<FVector3f> To, double& OutMeanCm);

	/**
	 * Pure (tested): Garment takes Body's joints, and each of its vertices the blend of the skin weights of its
	 * TransferNeighbours nearest Body vertices (by inverse distance), the heaviest 8 kept and renormalised. OutFar counts
	 * vertices with no Body vertex within NearCm; OutMeanCm is the average distance to the nearest one. False with why when
	 * Body has no skin.
	 */
	static bool TransferWeights(FMantleGlbMesh& Garment, const FMantleGlbMesh& Body, int32& OutFar, double& OutMeanCm, FString& OutError);

	/**
	 * Pure (tested): whether Mesh's weights all sit on its root bones (joints with no parent), or it has no skin. Such a
	 * garment (a cloth asset or static mesh made into an Outfit exports on root alone) cannot follow the body: the client
	 * takes the wearer's weights by bone name, and root is the only name it has.
	 */
	static bool FollowsRootOnly(const FMantleGlbMesh& Mesh);

	/**
	 * The MetaHuman's skin is shaped like the original body's where clothing is within ShapeNearCm of it, not at all past
	 * ShapeFarCm (in between, in proportion), and the moves smoothed ShapeSmoothIters passes over its edges: its hands,
	 * face and the rest stay MetaHuman's own.
	 */
	static constexpr double ShapeNearCm = 2.0;

	/**
	 * Clothing cut from a character's own mesh (#55) is refused when shaping the MetaHuman like the skin under it turns over
	 * more than this share of the MetaHuman's triangles: that skin isn't a whole body (Vampire's merged mesh: 3.6%; her
	 * separate bare body: 1.1%, at the toes in her heels), and the resize would carry its folds into the clothing. Its
	 * clothing then isn't offered as items (owner's call); the character itself still converts.
	 */
	static constexpr double MaxFoldShare = 0.025;

	/** Pure (tested): whether a shaping that turned over Turned of Triangles triangles is clean enough to make Outfits on (MaxFoldShare). */
	static bool SplitsCleanly(int32 Turned, int32 Triangles);

	/** Pure (tested): Mesh's triangles whose normal is more than 90 degrees from what it was at Before (its positions before a step). */
	static int32 TurnedTriangles(TConstArrayView<FVector3f> Before, const FMantleGlbMesh& Mesh);
	static constexpr double ShapeFarCm = 8.0;
	static constexpr int32 ShapeSmoothIters = 10;

	/** Skin moves are smoothed this many passes over the original body's edges, so a fold of the closest-point map is not printed on the garment. */
	static constexpr int32 CarrySmoothIters = 3;

	/**
	 * Pure (tested): Garment carried the way its skin moves from the original body From to the MetaHuman To, so it keeps
	 * the clearance it had: each point of From moves onto its closest point of To (FMantleGarmentClearance; points with
	 * none within its search take their neighbours' moves), smoothed CarrySmoothIters passes; a garment point takes the
	 * moves of the corners of its closest From triangle by its barycentrics, or the inverse-distance blend of its nearest
	 * From points' when none is within the search. Where the MetaHuman is smaller than the original body (a fuller bust,
	 * a rounder seat) the garment comes in with it instead of floating; where it is bigger, the garment goes out with it.
	 * OutMeanMoveCm: the average garment move. False with why on bad surfaces.
	 */
	static bool CarryWithSkin(TArray<FVector3f>& Garment, const FMantleSkinnedMeshLOD& From, const FMantleSkinnedMeshLOD& To, double& OutMeanMoveCm, FString& OutError);

	/** Pure (tested): Mesh as one point per position (render copies welded), their normals summed, its triangles; OutPointOf maps each vertex to its point. */
	static void WeldedSurface(const FMantleGlbMesh& Mesh, FMantleSkinnedMeshLOD& Out, TArray<int32>& OutPointOf);

	/**
	 * Pure (tested): how Points sit over Surface (FMantleGarmentClearance within its default search), for the log:
	 * "tracked 1650/1660, under 12, mean 0.84, p10 0.10 p50 0.61 p90 1.92 cm, within 1 cm 71%".
	 */
	static FString DescribeClearance(TConstArrayView<FVector3f> Points, const FMantleSkinnedMeshLOD& Surface);

	/**
	 * Pure (tested): how a step changed Mesh's shape, from Before (its positions before the step) to its positions now, for the
	 * log: each triangle edge's length change (p50, p90, largest, as a share of its length), the triangles turned over (normal
	 * more than 90 degrees from before), and the bones (Mesh's heaviest weight per vertex) whose edges changed most.
	 */
	static FString DescribeShapeChange(TConstArrayView<FVector3f> Before, const FMantleGlbMesh& Mesh);

	/**
	 * Steps 1 to 4 for Body and each of Garments. TurnOff and Confirm are Convert's fix keys (comma-separated), as the
	 * toolset's ConvertCosmetic takes them; "keep the face, choose a body" never applies here (the garment needs its own body).
	 * OutConvertJson is the converter's fix list and result (FMantleAvatarConverter::ToJson). False with OutRefusal ("code"
	 * and a sentence) when the body could not be made a MetaHuman; a garment that fails is reported in its piece.
	 */
	static bool Make(USkeletalMesh& Body, TConstArrayView<USkeletalMesh*> Garments, const FString& TurnOff, const FString& Confirm,
		FMantleGarmentOutfitResult& OutResult, FString& OutConvertJson, FString& OutRefusalCode, FString& OutRefusal,
		TConstArrayView<TArray<USkeletalMesh*>> Trims = {});

	/**
	 * A saved copy of Source as Folder/<Name> (made unique), for meshes MetaHuman Creator makes in memory: a transient
	 * skeleton is replaced by a saved one of the same bones, and a transient material by the first saved material it is an
	 * instance of. Null with why.
	 */
	static USkeletalMesh* SaveCopy(USkeletalMesh& Source, const FString& Folder, const FString& Name, FString& OutError,
		const TFunction<bool(FMeshDescription&, FReferenceSkeleton&, FString&)>& Edit = nullptr);

	/**
	 * Pure (tested): Component (one component-space transform per raw bone of Reference) turned so each limb bone of Limbs()
	 * points as it does in Template (bones by name), root to leaf: a bone turns about its joint by the smallest rotation,
	 * and everything below it follows. The conform keeps the arm angle of the body it was given; Epic's standard bodies,
	 * and the resize that fits an Outfit onto them, stand in MetaHuman's own A-pose. OutLines: "upperarm_l 7.4", one per
	 * limb bone, degrees.
	 */
	static TArray<FTransform> TurnToTemplate(const FReferenceSkeleton& Reference, TConstArrayView<FTransform> Component,
		const FReferenceSkeleton& Template, TConstArrayView<FTransform> TemplateComponent, TArray<FString>& OutLines,
		const TArray<TPair<FName, FName>>& Pairs = Limbs());

	/** The limb bones TurnToTemplate lines up, each with the child it points at, root to leaf (MetaHuman's names). */
	static const TArray<TPair<FName, FName>>& Limbs();

	/**
	 * The foot bones, each with the child it points at: a character in high heels stands on tiptoe, and the MetaHuman made
	 * of it on flat feet. Its feet are turned to the MetaHuman's (TurnToTemplate with these) before its clothing is posed.
	 */
	static const TArray<TPair<FName, FName>>& Feet();

	/** Component-space transforms of Reference's raw bones in their reference pose. */
	static TArray<FTransform> ComponentPose(const FReferenceSkeleton& Reference);

	/**
	 * Steps 2 to 5 on a MetaHuman Convert already made of the body (Work: its skin-only body, A-pose clip, posed mesh and
	 * conformed Character), so the Cosmetics panel reuses the avatar's own Convert rather than conforming again. BodyName
	 * names the body in messages; everything is saved under Folder. Fills OutResult's Character, InputScale and the rest,
	 * leaving its Notes; false with OutRefusal ("code" and a sentence) when the MetaHuman can't be read or the body can't
	 * be posed as it was.
	 * bCutFromOwnMesh: the garments were cut from the character's own mesh (#55); refused ("not_splittable") when its skin
	 * doesn't shape cleanly (SplitsCleanly): the clothing isn't offered as items.
	 * Trims: per garment (by index, when given), the thin pieces worn on it (FMantleConvertGarment::Trims), posed with it and
	 * made into its Outfit.
	 */
	static bool MakeFromWork(const FMantleConvertMetaHumanWork& Work, const FString& BodyName, const FString& Folder, TConstArrayView<USkeletalMesh*> Garments,
		FMantleGarmentOutfitResult& OutResult, FString& OutRefusalCode, FString& OutRefusal, bool bCutFromOwnMesh = false,
		TConstArrayView<TArray<USkeletalMesh*>> Trims = {});

	/**
	 * Pure (tested): the garment slot a mesh beside a body is ("top", "bottom", "footwear"), from its name's words, else
	 * from its sections when its name says no piece. Empty when it is not one piece of clothing; then OutWhy says why in
	 * plain words when it is clothing that can't be an Outfit (several pieces in one mesh, or clothing merged with the
	 * body's skin), and is empty when it isn't clothing at all (a body, hair, a hat, a prop). A one-piece name with a skin
	 * section is merged clothing too.
	 */
	static FString GarmentSlot(const FString& MeshName, TConstArrayView<FMantleOutfitSection> Sections, FString& OutWhy);

	/**
	 * A third-party avatar's separate clothing beside Body: the skeletal meshes under Body's folder (its subfolders too,
	 * Converted folders left out) that GarmentSlot calls a top, bottom or footwear, or merged clothing (Why says so), top
	 * first. A piece that shares too few of Body's bones gets a Why too, and every piece does when Body itself wears
	 * clothing (WornClothing), naming a bare body beside it to convert instead. OutNotOffered: the display names of the
	 * others that aren't a body (hair, gloves, a belt ...).
	 */
	static void FindGarments(USkeletalMesh& Body, TArray<FMantleConvertGarment>& OutGarments, TArray<FString>& OutNotOffered,
		TArray<FString>* OutPaired = nullptr);

	/**
	 * #55: the clothing Convert makes for Body from the pack review's rows as the creator left them (FMantlePackReview::Items): one
	 * garment per item, its other pieces (edging, sleeves) as its Trims, named "Corset with Lace Corset and Lace Arms". The same
	 * reasons as FindGarments say why a piece can't be an Outfit (too few of Body's bones; Body wears clothing). Top first.
	 */
	static void GarmentsFromReview(USkeletalMesh& Body, const FMantlePackReview& Review, TArray<FMantleConvertGarment>& OutGarments);

	/**
	 * A piece is trim on another (IsTrimOn) when it has at most TrimMaxTriangleShare of the other's triangles and at least
	 * TrimNearShare of its points are within TrimNearCm of the other's surface, both as made (bind pose). An edging stands
	 * up from the garment's rim, so not all of it lies on it: Vampire's corset lace (152 of the corset's 2650 triangles)
	 * measured 66% within 1 cm and 100% within 2 cm, which is why 80% within 1 cm missed it. Two real garments (a shirt and
	 * the jacket over it) are each too big to pass; the lace against the pack's other top measured 85% within 2 cm, and the
	 * closest garment wins.
	 */
	static constexpr double TrimMaxTriangleShare = 0.2;
	static constexpr double TrimNearShare = 0.6;
	static constexpr float TrimNearCm = 2.0f;

	/** Pure (tested): whether Trim is a thin piece lying on Garment (a corset's lace edging), by TrimMaxTriangleShare, TrimNearShare and TrimNearCm. */
	static bool IsTrimOn(const FMantleGlbMesh& Trim, const FMantleGlbMesh& Garment);

	/**
	 * Pure (tested): the share of Trim's points within WithinCm (TrimNearCm) of Garment's surface, or 0 when Trim is too big to be its
	 * trim (TrimMaxTriangleShare). OutTrimTriangles and OutGarmentTriangles, when given, get the two triangle counts, so a 0 can be
	 * told apart: too big, or not lying on it.
	 */
	static double TrimNear(const FMantleGlbMesh& Trim, const FMantleGlbMesh& Garment, float WithinCm = TrimNearCm, int32* OutTrimTriangles = nullptr,
		int32* OutGarmentTriangles = nullptr);

	/** Pure (tested through TrimNear): the share of Points within WithinCm of Surface (FMantleGarmentClearance); 0 for no points. */
	static double ShareOn(TConstArrayView<FVector3f> Points, const FMantleSkinnedMeshLOD& Surface, float WithinCm);

	/**
	 * Pure (tested): Trim's geometry added to Garment as more primitives (its materials after Garment's: OutMaterials gets
	 * TrimMaterials appended). Joints and skin weights are Garment's; the trim's vertices get none, so this is for a garment
	 * whose weights are transferred afterwards (TransferWeights), both posed alike first.
	 */
	static void AppendTrim(FMantleGlbMesh& Garment, const FMantleGlbMesh& Trim);

	/**
	 * #55: the pieces of clothing in Body's own mesh (its material sections), when the character splits into a bare body and
	 * clothing (FMantleCharacterSplit::Propose): one entry per garment piece, appended to OutGarments with SourceMesh and Sections
	 * set and no Mesh yet (MakeSectionMeshes cuts them). Nothing is appended when the character stays a fixed avatar, or, with
	 * OutNote, when the pack has its bare body (FindBareBody) and OutGarments already holds separate clothing: those come first,
	 * and OutNote says which source was chosen and why. Without OutNote the sections are always read (a maintainer's comparison).
	 */
	static void FindSectionGarments(USkeletalMesh& Body, TArray<FMantleConvertGarment>& OutGarments, FString* OutNote = nullptr);

	/**
	 * #55: cuts each section piece of Context.Garments that has no Mesh yet from its SourceMesh (FMantleAvatarStrip::WriteSlots)
	 * into <output>/Work, so MakeFromWork can take it like a separate mesh. A piece that can't be cut gets its Error. Returns
	 * how many were cut.
	 */
	static int32 MakeSectionMeshes(FMantleConvertContext& Context);

	/**
	 * Why Context's avatar part (PartIndex) gives its clothing no Outfits, in plain words; empty when MakeFromWork can run:
	 * Convert made no MetaHuman of it, made it as a face on MetaHuman's own body, or cut clothing off it (so it isn't the
	 * bare body the clothing was made for). bSectionPieces: for pieces cut from the character's own mesh (#55), whose bare body
	 * is exactly the one Strip left, wearing clothing is not a reason.
	 */
	static FString WhyNoOutfits(const FMantleConvertContext& Context, int32 PartIndex, bool bSectionPieces = false);

	/** The material slots of Body that are clothing (a top, bottom or footwear), comma-separated; empty for a bare body. */
	static FString WornClothing(const USkeletalMesh& Body);

	/**
	 * #55: the character's bare body beside Body (its folder and below; Convert's output never), when the pack has one: a whole
	 * body (at least BareHeightShare of Body's height, on Body's bones) that is all skin by Strip's check, or whose name says
	 * it is nude or in underwear (Convert's Strip takes that off). The best by BareBodyRank; null when none. OutWhy says which and why, plainly.
	 * When there is one, its clothing in separate meshes is used, and Body's own sections are not cut (the owner's rule: a
	 * bare body and separate garments first, the merged mesh only when there is nothing separate).
	 */
	static USkeletalMesh* FindBareBody(const USkeletalMesh& Body, FString& OutWhy);

	/** A bare body is at least this share of the character's height (a torso, hands or feet alone are parts, not a body). */
	static constexpr double BareHeightShare = 0.9;

	/**
	 * Pure (tested): how good a bare body MeshName is (higher is better), or -1 when it is not one: CoveredSlots of its
	 * material slots are neither skin nor a face detail (Strip's own check, FMantleAvatarStrip::LooksLikeSkin and
	 * NamesFaceDetail), HeightShare its height over the character's. All skin is best, whatever the names say; a body with
	 * something on it is a bare body only when its name says nude, naked, bare or underwear (Convert takes it off).
	 */
	static int32 BareBodyRank(const FString& MeshName, int32 CoveredSlots, double HeightShare);

	/** Pure (tested): whether a mesh, slot or material name says underwear (underwear, lingerie, bra, panties ...). */
	static bool IsUnderwearName(const FString& Name);

	/** The toolset's MakeOutfitFromGarment: Make for asset paths (Garments comma-separated), as one JSON object. */
	static FString RunForAgent(const FString& BodyPath, const FString& GarmentPaths, const FString& TurnOff, const FString& Confirm);
};
