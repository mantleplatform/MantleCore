// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class UAnimSequence;
class USkeletalMesh;
class UStaticMesh;
class UObject;

/**
 * The avatar converter (GhzGarage/mantle#610): a creator picks a character in the editor, presses Convert, and gets
 * new, upload-ready assets in a Converted/ folder next to it. The creator's own assets are never modified.
 *
 * The converter is a fixed pipeline of fixers. Each fixer looks at the character as the previous fixers left it
 * (Analyze), says what it would do in one line for the fix list, and does it (Apply) by writing NEW assets under the
 * context's output folder and pointing the context at them. A fixer the creator switches off leaves its problem for
 * the upload checks to refuse, exactly as today.
 *
 * #625 (owner, 2026-10-02): two routes (EMantleAvatarRoute). Convert, the default, makes the avatar a MetaHuman with
 * Epic's MetaHuman tools; As-is (what you see is what you get: a supported skeleton and Mantle's limits) runs only while
 * UMantleSettings::bAllowAsIsAvatars is on, which it is not by default.
 *
 * Pipeline order (FMantleAvatarConverter runs them in this order; later fixers see earlier fixers' output):
 *   AsIsRig     (as-is) chunk 7: a model with no rig (a static mesh, or a skeletal mesh with no usable weights) keeps its look:
 *               Epic's conform places the MetaHuman skeleton inside it (MHUnrigged's posed mesh, then MHConform's conform),
 *               its vertices take the A-pose body's skin weights at their closest point (above the neck: the head), and it
 *               becomes a skeletal mesh on that skeleton for the as-is fixers after it (RigMap skips it)
 *   RigMap      (as-is) third-party rig (Mixamo, Character Creator, Daz, guided mapping) onto a supported skeleton
 *   Repose      (as-is) the reference pose re-posed to the skeleton's A-pose, the mesh re-bound in it
 *   Props       (as-is, #46) cloth simulation, hidden sections and held props (weapons) removed, on a copy (FMantleAvatarPropsFixer);
 *               before AsIsHeight, so the height is measured without them (Riktor's 7 m chain)
 *   AsIsHeight  (as-is, #774) the body scaled about its feet to the default height (FMantleAvatarHeight::TargetCm), as Convert does
 *   Strip       (convert) the body cut down to bare skin: clothing, hair, eyes and the pieces inside the head removed, on a
 *               copy, once the creator confirms the kept parts are bare skin (pipeline step 01)
 *   MHUnrigged  (convert) #776: a model with no skeleton (a static mesh, or a skeletal mesh with no usable weights) takes Strip's and
 *               MHPose's place: its arms bent into an A-pose when it is in a T-pose, scaled to the default height, frozen into
 *               the conform's input mesh (Strip and MHPose skip it)
 *   MHPose      (convert) the skin-only body posed into MetaHuman's A-pose with Epic's IK Retargeter, frozen into a
 *               static mesh scaled to the default height, the conform's input (pipeline steps 02 and 03)
 *   MHConform   (convert) a new MetaHuman Character conformed to the posed body with face landmarks from a front
 *               portrait, committed to the A-pose, hand joints re-estimated on the committed skin (pipeline step 04)
 *   MHSource    (convert) a MetaHuman Creator character picked as the source (an Epic preset or the creator's own): a copy
 *               of it set to the default height (its Height body constraint) and built as MHBuild builds, Epic's centre skin
 *               tone too, then the same hand-off. Strip, MHPose and MHConform skip it (it has no mesh to convert)
 *   MHBuild     (convert) the character built with Epic's tools: Epic's centre skin tone, no Epic underwear, cloud skin
 *               textures and face rig (signed in to Epic), the Optimized/Medium build, textures resized to 2048, the built
 *               height checked against the default (a warning) (step 05); then the hand-off (no remap yet): the avatar
 *               part becomes the built body and face, the build root the assembly folder, for the fixers after it
 *   RetargetPose (as-is) a Mannequin body's arms, hands and legs lined up, in its reference pose, with what the client's
 *               retargeter expects, so every limb moves as the animation's driver does (#625)
 *   Assembly    a MetaHuman Creator assembly: body + face (DNA), clothing, hair picked from it
 *   Refit       clothing and hair refitted onto a body with Mantle's reference topology: shape warped, re-bound in its pose
 *   Triangles   a reduced LOD 0 under the slot's triangle cap
 *   Atlas       material sections (and UDIM tiles) baked into atlases down to the slot's section limit, <= 2048
 *   Bake        every other section whose material the upload cannot read as plain textures (tints, detail maps, UDIM or
 *               virtual textures, a MetaHuman face's skin) baked through its material on its own UVs, <= 2048
 */
namespace MantleAvatarFixerIds
{
	inline const FName AsIsRig(TEXT("AsIsRig"));
	inline const FName RigMap(TEXT("RigMap"));
	inline const FName Repose(TEXT("Repose"));
	inline const FName AsIsHeight(TEXT("AsIsHeight"));
	inline const FName Props(TEXT("Props"));
	inline const FName Strip(TEXT("Strip"));
	inline const FName MHUnrigged(TEXT("MHUnrigged"));
	inline const FName MHPose(TEXT("MHPose"));
	inline const FName MHConform(TEXT("MHConform"));
	inline const FName MHSource(TEXT("MHSource"));
	inline const FName MHBuild(TEXT("MHBuild"));
	inline const FName RetargetPose(TEXT("RetargetPose"));
	inline const FName Assembly(TEXT("Assembly"));
	inline const FName Refit(TEXT("Refit"));
	inline const FName Triangles(TEXT("Triangles"));
	inline const FName Atlas(TEXT("Atlas"));
	inline const FName Bake(TEXT("Bake"));
}

/** One upload the converter prepares: an avatar body (with an optional face), a garment, or hair. */
struct MANTLEEDITOR_API FMantleConvertPart
{
	/** "avatar", "top", "bottom", "footwear", "hair", ... as the cosmetic upload names slots. */
	FString Slot;

	/** The mesh as it stands now: the creator's original until a fixer writes a converted one. */
	TWeakObjectPtr<USkeletalMesh> Mesh;

	/** A MetaHuman avatar's separate face (with DNA); null otherwise. */
	TWeakObjectPtr<USkeletalMesh> Face;

	/** Appearance skeleton id once known ("metahuman", "ue5mannequin", "ue4mannequin", "uefnmannequin"); empty before. */
	FString SkeletonId;

	/** The section a hair or accessory lift uses (the Cosmetics panel's Section picker); INDEX_NONE for all. */
	int32 Section = INDEX_NONE;

	/** Display name for the upload. */
	FString Name;

	/**
	 * Garments and hair: the body (and its face) the item was made on, which the upload sends as its body shape
	 * (FMantleCosmeticDraft::AuthoringBody, AuthoringFace). Null lets the upload find one, as the Cosmetics panel does.
	 * May be Mantle's reference body and face from the catalog (FMantleReferenceBodies): transient meshes the provider
	 * keeps for the editor session, read at upload time and never saved; reports name them by editor-only provenance id.
	 */
	TWeakObjectPtr<USkeletalMesh> AuthoringBody;
	TWeakObjectPtr<USkeletalMesh> AuthoringFace;

	/** The creator's mesh the part started from (Mesh before any fixer ran). Never modified. */
	TWeakObjectPtr<USkeletalMesh> Original;

	/**
	 * #776: a model with no skeleton, which Convert fits to a MetaHuman body (FMantleUnriggedBody; the MHUnrigged step in place of
	 * Strip and MHPose). Its geometry is StaticSource (a static mesh) or, for a skeletal mesh with no usable weights, Original. Mesh stays
	 * what it is (null for a static mesh) until the build hands the part on.
	 */
	bool bUnrigged = false;
	TWeakObjectPtr<UStaticMesh> StaticSource;

	/**
	 * Chunk 7: an unrigged part on the As-is route, once the AsIsRig step made Mesh of it (its own geometry skinned to Epic's MetaHuman
	 * body skeleton; FMantleUnriggedAsIs). Until then As-is leaves Mesh null for it, so no other As-is fixer reads the source's rig.
	 */
	bool bAutoRigged = false;

	/**
	 * False for a part the creator opts into (an assembly's cloth-simulated piece). Fixers still analyse it, but the
	 * converter applies no fix to it except the one that includes it, and hands it to nobody while it stays out.
	 */
	bool bIncluded = true;

	/** The cloth simulation the source carries (a Chaos cloth asset's or clothing asset's name); empty for none. Uploads drop it. */
	FString ClothAsset;
};

/**
 * #625 (owner, 2026-10-02): how an avatar uploads. Convert: it becomes a MetaHuman (Epic's MetaHuman tools), wears
 * clothing and takes the avatar options. AsIs: what you see is what you get (a supported skeleton and Mantle's limits;
 * no customisation, no clothing); only while UMantleSettings::bAllowAsIsAvatars is on.
 */
enum class EMantleAvatarRoute : uint8
{
	Convert,
	AsIs,
};

/** "convert", "asis": the toolset's and the converter JSON's spelling. */
MANTLEEDITOR_API const TCHAR* LexToString(EMantleAvatarRoute Route);

/**
 * #625: what Convert's MetaHuman steps made for one avatar part, handed from step to step (the pipeline's files between
 * its scripts). All of it lives under the output's Work folder.
 */
struct MANTLEEDITOR_API FMantleConvertMetaHumanWork
{
	/** Step 01's skin-only body (the part's mesh after Strip). */
	TWeakObjectPtr<USkeletalMesh> SkinOnly;

	/** Step 02: the IK rig made for the skin-only body (a UIKRigDefinition), whose retarget chains name its bones. */
	TWeakObjectPtr<UObject> TargetRig;

	/** Step 02: MetaHuman's A-pose retargeted onto the skin-only body (one frame). */
	TWeakObjectPtr<UAnimSequence> APoseClip;

	/** Step 03: the skin-only body frozen in that pose: the conform's input. */
	TWeakObjectPtr<UStaticMesh> PosedMesh;

	/** The end bones of the target rig's Head and LeftArm chains (the portrait's aim, the pose gate's hand); None without. */
	FName HeadBone;
	FName HandBone;

	/** The head bone in the A-pose clip on the skin-only body, component space, cm (the portrait's aim); unset without a Head chain. */
	TOptional<FVector> HeadJoint;

	/**
	 * Step 03's default-height scale k: the posed mesh was scaled by it about the origin (FMantleAvatarHeight; also on the
	 * posed mesh as FMantleAvatarHeight::InputScaleTag).
	 */
	double InputScale = 1.0;

	/** HeadJoint scaled by InputScale, as the posed mesh was: the conform's aim. */
	TOptional<FVector> ConformHeadHint() const
	{
		return HeadJoint.IsSet() ? TOptional<FVector>(HeadJoint.GetValue() * InputScale) : TOptional<FVector>();
	}

	/** Step 04: the MetaHuman Character (a UMetaHumanCharacter, editable in MetaHuman Creator), conformed and committed. */
	TWeakObjectPtr<UObject> Character;

	/** Its folder, <output>/<Name>: Epic's build goes to <CharacterFolder>/Unpacked/<Name>. */
	FString CharacterFolder;

	/** Step 05: where Epic's build put the built MetaHuman (found, not assumed), and its body and face meshes. */
	FString BuildRoot;
	TWeakObjectPtr<USkeletalMesh> BuiltBody;
	TWeakObjectPtr<USkeletalMesh> BuiltFace;

	/**
	 * #770, "keep the face, choose a body" (Strip's FaceOnly line): the part is converted from its clean head alone
	 * (FMantleMetaHumanHeadOnly), conformed head-only onto MetaHuman's own body at the default height. Strip and the A-pose
	 * step leave it alone, the conform takes the head, and the build sets the height on the Height constraint.
	 */
	bool bFaceOnly = false;
	/** Why the body was read as covered ("no material on it reads as bare skin"), empty when the creator chose it. */
	FString FaceOnlyWhy;
	/** The body the creator picked for the face (Strip's FeminineBody line); false is the MasculineBody line. */
	bool bFeminineBody = false;
};

/**
 * A third-party character's separate piece of clothing found beside the body being converted
 * (FMantleGarmentOutfit::FindGarments). After Convert, it can become a MetaHuman Outfit on the MetaHuman Convert made of
 * that body (FMantleGarmentOutfit::MakeFromWork). Fixers never touch it.
 */
struct MANTLEEDITOR_API FMantleConvertGarment
{
	/** The creator's mesh (never modified). For a piece cut from the character's own mesh (Sections set), the copy that holds only that piece, made when the Outfits are (FMantleGarmentOutfit::MakeSectionMeshes). */
	TWeakObjectPtr<USkeletalMesh> Mesh;

	/** #55: a piece of the character's own mesh: that mesh, and the material slots of the sections that make the piece. Empty for a separate mesh. */
	TWeakObjectPtr<USkeletalMesh> SourceMesh;
	TArray<int32> Sections;

	/** "top", "bottom" or "footwear"; empty for merged clothing (Why says so). */
	FString Slot;

	/**
	 * Thin pieces worn on this one, in the same slot (FMantleGarmentOutfit::IsTrimOn: a corset's lace edging): they go into
	 * its Outfit with it, as one item, and are not listed on their own.
	 */
	TArray<TWeakObjectPtr<USkeletalMesh>> Trims;

	/** Display name for the upload. */
	FString Name;

	/** Why it can't become an Outfit, in plain words; empty when it can. */
	FString Why;

	/** The Outfit made from it, once made; and why making it failed, when it did. */
	FString Outfit;
	FString Error;
};

/** What the converter is working on, handed from fixer to fixer. */
struct MANTLEEDITOR_API FMantleConvertContext
{
	/** The route the creator chose (FMantleConvertOptions::Route). Fixers of the other route propose nothing. */
	EMantleAvatarRoute Route = EMantleAvatarRoute::Convert;

	/** Long package path of the folder the converted assets go in, e.g. /Game/Vampire/Converted. Never the source's. */
	FString OutputFolder;

	/** The parts to upload. Usually one; a MetaHuman assembly yields avatar + garments + hair. */
	TArray<FMantleConvertPart> Parts;

	/** Texture cap for anything baked (owner, 2026-09-30: the platform maximum, 2048). */
	int32 MaxTextureSize = 2048;

	/** Every asset a fixer created, for the summary and for undo (delete the Converted/ folder). */
	TArray<TWeakObjectPtr<UObject>> CreatedAssets;

	/** Human-readable lines for the summary after Convert. */
	TArray<FString> Notes;

	/** What the creator picked: a skeletal mesh or an assembly's Blueprint; null when a folder was given. */
	TWeakObjectPtr<UObject> Source;

	/** Long package path of the MetaHuman Creator assembly the source belongs to, e.g. /Game/CharacterAssemblies/Vampire; empty for none. */
	FString AssemblyFolder;

	/**
	 * #875: an assembled MetaHuman (a Blueprint with meshes, no MetaHuman Creator character) on the Convert route: its body and
	 * face meshes, which MHSource makes into a character (FMantleMetaHumanBuild::MakeCharacterFromAssembly) before it builds it.
	 * AssemblyFolder stays empty until the hand-off, as for a character source.
	 */
	TWeakObjectPtr<USkeletalMesh> AssemblyBodyToFit;
	TWeakObjectPtr<USkeletalMesh> AssemblyFaceToFit;

	/**
	 * #625, the Strip step's choices as Convert met its lines (FMantleAvatarStripFixer): the creator's per-slot and
	 * per-piece lines are applied before its bare-skin line, each recording here what it removes; the bare-skin line then
	 * strips. By part index: material slot indices to remove, and head pieces to remove (HeadSlot * 65536 + the piece's
	 * index, the largest piece first).
	 */
	TMap<int32, TSet<int32>> StripSlots;
	TMap<int32, TSet<int32>> StripPieces;

	/** #625: Convert's MetaHuman steps' work, by part index. */
	TMap<int32, FMantleConvertMetaHumanWork> MetaHuman;

	/** A third-party avatar's separate clothing (one body converted on its own): tops, bottoms and footwear, for Outfits. */
	TArray<FMantleConvertGarment> Garments;

	/**
	 * Chunk 6: a MetaHuman Blueprint's clothing and hair on the Convert route. The rebuild makes the MetaHuman from its face
	 * and body alone; these are already on Epic's MetaHuman skeleton, so they stay as they are: no fixer touches them, and
	 * the panel lists them to publish after Convert (a cloth-simulated piece unticked, its simulation dropped).
	 */
	TArray<FMantleConvertPart> Kept;

	/**
	 * #625: whether Parts[PartIndex] is a MetaHuman Convert built and handed on (FMantleMetaHumanBuild::HandOff). Its body
	 * and face are Epic's build, tied to its DNA (face rig, blend shapes): never reduced, re-LODed or atlased.
	 */
	bool IsBuiltMetaHuman(const int32 PartIndex) const
	{
		const FMantleConvertMetaHumanWork* Work = MetaHuman.Find(PartIndex);
		return Work && Work->BuiltBody.IsValid();
	}

	/** #625: whether Folder is the build root of a MetaHuman this conversion built and handed on (read despite its Converted folder). */
	bool IsHandedOffBuild(const FString& Folder) const
	{
		for (const TPair<int32, FMantleConvertMetaHumanWork>& Work : MetaHuman)
		{
			if (Work.Value.BuiltBody.IsValid() && !Folder.IsEmpty() && Work.Value.BuildRoot.Equals(Folder, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}
};

/** One line in the fix list the creator sees before converting. */
struct MANTLEEDITOR_API FMantleConvertFix
{
	FName FixerId;

	/** Which part it applies to (index into FMantleConvertContext::Parts). */
	int32 PartIndex = INDEX_NONE;

	/** What is wrong, in the upload checks' wording ("24,810 triangles, the limit is 20,000"). */
	FString Problem;

	/** What the fixer will do and what it costs ("reduces LOD 0 to 18,400 triangles"). */
	FString Action;

	/** False when the fixer found the problem but cannot fix it here (the line says why); the upload will refuse. */
	bool bCanFix = true;

	/** The creator's choice in the fix list. */
	bool bEnabled = true;

	/**
	 * #625: a line the creator must tick themselves (it starts unticked): e.g. "the kept parts are bare skin". While any
	 * such line is unticked, Convert runs nothing (FMantleAvatarConverter::Convert) and the panel greys Convert.
	 */
	bool bNeedsConfirm = false;

	/**
	 * Which of its fixes a fixer proposed, for a fixer with several kinds per part (its own names, e.g. the Assembly
	 * fixer's Face and Section); None for a fixer with one. Apply dispatches on it, and the converter keys the creator's
	 * choices by fixer, part and kind (FMantleAvatarConverter::FixKeys).
	 */
	FName Kind;
};

/** A converter step. Implementations are stateless; register them with FMantleAvatarConverter. */
class MANTLEEDITOR_API IMantleAvatarFixer
{
public:
	virtual ~IMantleAvatarFixer() = default;

	virtual FName GetId() const = 0;

	/** Adds zero or more fixes for the context as it stands. Must not modify any asset. */
	virtual void Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const = 0;

	/**
	 * Applies one enabled fix this fixer proposed: writes new assets under Context.OutputFolder, records them in
	 * CreatedAssets, and repoints the part at them. False with a reason leaves the context as it was.
	 */
	virtual bool Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const = 0;
};

/** The fixer registry: FMantleAvatarConverter owns the pipeline; fixers register at module startup. */
struct MANTLEEDITOR_API FMantleAvatarFixers
{
	static void Register(const TSharedRef<IMantleAvatarFixer>& Fixer);

	/** Registered fixers in pipeline order (see MantleAvatarFixerIds above; unknown ids last). */
	static TArray<TSharedRef<IMantleAvatarFixer>> Get();
};
