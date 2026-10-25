// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleClothingColorDetect.h"
#include "MantleCosmeticPackage.h"
#include "MantleGlbWriter.h"
#include "MantleOverlayBake.h"
#include "MantleOutfitSections.h"
#include "MantleThumbnailChoice.h"
#include "UObject/WeakObjectPtrTemplates.h"

class FMantleAvatarContract;
class UMaterialInterface;
class USkeletalMesh;
class USkeleton;
class UStaticMesh;
class UMetaHumanCharacter;
struct FMantleContractSkeleton;
class UTexture;
class UTexture2D;
struct FMantleSkinnedMesh;
struct FMantleFitBodySet;

/** Which sex's standard MetaHuman bodies an Outfit's garment variants are fitted to. Auto lets the plugin decide from the Outfit's own sizes. */
enum class EMantleFitSex : uint8
{
	Auto,
	Female,
	Male,
};

/** One representative body shape for a logical native clothing piece: a converted Creator character, or a standard body type made in memory. */
struct MANTLEEDITOR_API FMantleOutfitAuthoringTarget
{
	FString Id;
	TWeakObjectPtr<UMetaHumanCharacter> Character;
	TWeakObjectPtr<USkeletalMesh> Body;
	TWeakObjectPtr<USkeletalMesh> Face;
	/** A standard body type made in memory (FMantleFitBodies): Body is its merged head+body, and the converted-body checks do not apply. */
	bool bInMemory = false;
};

/** Explicit UV0 mask and region controls for one named exported clothing material. */
struct MANTLEEDITOR_API FMantleClothingColorAuthoring
{
	FString MaterialName;
	TWeakObjectPtr<UTexture2D> Mask;
	/** Explicit replacement for a black source; creator-authored texture detail is required. */
	TWeakObjectPtr<UTexture2D> NeutralAlbedo;
	/** Reference luminance is measured from the exported albedo and UV0 mask, never trusted from the UI. */
	TArray<FMantleClothingColorRegion> Regions;
};

/** What the creator chose in the panel: the source asset and the item's settings. */
struct MANTLEEDITOR_API FMantleCosmeticDraft
{
	/** Explicit creator product route; empty means unknown, never inferred from topology. */
	FString AvatarPublicationMode;

	/**
	 * Who may use the item (#614): public (preselected, listed for everyone) or private (only the
	 * creator). A public item is published only with the creator's rights declaration, which
	 * bRightsDeclared records; it is never assumed.
	 */
	bool bPublic = true;
	bool bRightsDeclared = false;
	EMantleCosmeticClass Class = EMantleCosmeticClass::Garment;
	FString Slot;
	FString Name;
	FString Description;

	/**
	 * Hair: a groom (its cards are used), a cards static mesh, or a skeletal mesh (one section is
	 * lifted, e.g. a MetaHuman face's eyelashes). Garment: a skeletal mesh or an evaluated MetaHuman
	 * Outfit Asset with native source-size selection. Avatar: a skeletal mesh. Accessory: a
	 * static mesh. Emote: an animation sequence.
	 */
	TWeakObjectPtr<UObject> Source;

	/** Optional explicit source-size override for diagnostics; empty uses native selection with a Creator character. */
	FString OutfitSourceSize;

	/** Native Outfit: resize onto the Creator character behind the converted authoring body/face. */
	TWeakObjectPtr<UMetaHumanCharacter> OutfitAuthoringCharacter;

	/** OutfitAuthoringCharacter is a standard body type made in memory (FMantleFitBodies), so Prepare skips the converted-body checks. */
	bool bOutfitAuthoringInMemory = false;

	/**
	 * Clothing from an Outfit: which sex's standard bodies the garment variants are fitted to. Auto (the default) picks from the
	 * Outfit's own authored sizes; Female or Male overrides it. Read by FMantleEditorActions::ApplyCosmeticFitTargetDefaults.
	 */
	EMantleFitSex FitSex = EMantleFitSex::Auto;
	/** The standard bodies behind OutfitTargets that were made in memory; held here so they live as long as any copy of the draft (the whole export), and are released with the last. */
	TSharedPtr<FMantleFitBodySet> FitBodies;
	/** What ApplyCosmeticFitTargetDefaults chose and why, or why it made no fit bodies; shown in the check's notes. */
	FString FitBodiesStatus;
	/** One line per fit body: how its body was put in Mantle's reference vertex order (a note on the check). */
	TArray<FString> FitBodyNotes;

	/** Native Outfit: representative converted source shapes packaged together in one catalog item. */
	TArray<FMantleOutfitAuthoringTarget> OutfitTargets;
	/** Hand-made colour areas (the panel's advanced section). Any declaration here turns detection off for the item. */
	TArray<FMantleClothingColorAuthoring> ClothingColors;

	/**
	 * #756, clothing (top, bottom, footwear): find the areas players can recolour from a mask texture the item's materials
	 * already carry (FMantleClothingColorDetect) and upload them, at export. On by default, so a creator who changes
	 * nothing, or a draft built in code, gets every area found. ClothingColorChoices untick or rename single areas.
	 */
	bool bDetectClothingColors = true;
	TArray<FMantleClothingColorChoice> ClothingColorChoices;

	/** Garments: source body shape. Hair: the actual authoring body or head-only rig locating its cards; not uploaded as a body shape. */
	TWeakObjectPtr<USkeletalMesh> AuthoringBody;

	/**
	 * #430, garments and hair: the MetaHuman face that goes with AuthoringBody; found from exact hair provenance or next to the body when unset
	 * (FMantleCosmeticExport::FindAuthoringFace). The body shape carries the neck measured on it, and a garment
	 * uploads it as its faceMeshAssetId, so players' clients fit the collar around each wearer's neck. Required for head hair's source-head correspondence.
	 */
	TWeakObjectPtr<USkeletalMesh> AuthoringFace;

	/** Avatars: the optional MetaHuman face. */
	TWeakObjectPtr<USkeletalMesh> Face;

	/**
	 * #584, emotes on the MetaHuman, UE5 or UE4 Mannequin skeleton: the body the clip was animated on, which it plays on
	 * while it is retargeted onto the UEFN mannequin (FMantleEmoteRetarget::ChooseSourceMesh). The sequence's own preview
	 * mesh wins over it; unset (or on another skeleton) falls back to a mesh the project has on the clip's skeleton.
	 */
	TWeakObjectPtr<USkeletalMesh> EmoteBody;

	/**
	 * Emotes: whether the emote loops on players' screens until they move, jump or are hit (on), or plays once and ends (off).
	 * On by default. On, a clip with a loop_start notify loops from it, and one without loops whole; off, any loop_start
	 * notify is left out (FMantleAnimClipWriter::ApplyLoopChoice).
	 */
	bool bEmoteLoop = true;

	/**
	 * #736, overlays (scars, tattoos): Source is the mask, a UTexture2D; or leave Source empty and name a PNG file
	 * (OverlayMaskFile). The optional normal map (scar slots only) is likewise OverlayNormal or OverlayNormalFile.
	 * Each is checked by FMantleOverlayChecks; no mesh is uploaded.
	 */
	FString OverlayMaskFile;
	TWeakObjectPtr<UTexture2D> OverlayNormal;
	FString OverlayNormalFile;

	/**
	 * #54, overlays: instead of a painted mask, a flat picture (OverlayPicture, a texture, else the PNG at OverlayPictureFile) placed on
	 * OverlayPlaceMesh (the converted body or face, else Mantle's reference one) with OverlayPlacement. Set bOverlayPlaced and the export
	 * bakes the mask (and a scar's normal map) from it (FMantleOverlayBake); the painted mask fields are then ignored.
	 */
	bool bOverlayPlaced = false;
	TWeakObjectPtr<UTexture2D> OverlayPicture;
	FString OverlayPictureFile;
	TWeakObjectPtr<USkeletalMesh> OverlayPlaceMesh;
	FMantleOverlayPlacement OverlayPlacement;
	/** The picture's height over its width (the panel's outline); the bake reads the picture itself. */
	float OverlayPlaceAspect = 1.f;

	/** Hair from a groom: which cards entry (FMantleCosmeticExport::ReadGroomCards); INDEX_NONE picks the default. */
	int32 CardsEntry = INDEX_NONE;

	/** Hair from a skeletal mesh: the render section lifted; INDEX_NONE uploads the whole cards mesh. */
	int32 SourceSection = INDEX_NONE;

	/**
	 * Hair from a groom's cards (their atlases upload as hair_cards sections, #325, #326): the creator's colour,
	 * linear RGB, over the one read from the groom's material (FMantleHairColor). Unset reads the material.
	 */
	TOptional<FLinearColor> HairColor;

	/** #51: the creator pressed Adjust on the hair head-fit preview, so bought hair uploads where they made it. */
	bool bPlaceHairByHand = false;

	/**
	 * #483, garments: the material slots (indices into the mesh's Materials, the same on every LOD) whose sections
	 * upload, so a one-piece outfit splits into one item per slot and the source character's skin stays behind. Unset
	 * uploads the whole mesh; set and empty keeps nothing, which Build refuses. The panel sets it from its section
	 * checkboxes (FMantleOutfitSections::InitialKeep), and leaves it unset when every section is ticked.
	 */
	TOptional<TArray<int32>> KeepSlots;

	/** Avatars: skin, eyes or other per body section (empty entries are filled from the material slot names). */
	TArray<FString> SectionRoles;

	/**
	 * Avatars: the jaw bone lip-sync drives; None picks the skeleton's default (a MetaHuman face's FACIAL_C_Jaw). #594: on
	 * a body whose skeleton has the jaw slot (the UE5, UE4 or UEFN Mannequin), the body's own bone the creator picked (below head); the
	 * upload keeps it as the slot, named jaw, and the item's jawBone is jaw. None there uploads a body without a jaw.
	 */
	FName JawBone;

	/**
	 * #595, avatars: the body's bone chains mapped onto the secondary motion slots, each with its preset. On a body whose
	 * skeleton has the slots (the UE5, UE4 or UEFN Mannequin) the upload keeps each as its slots (renamed, reparented to the
	 * slot's parent, weights kept) and the item carries the presets; unmapped extra bones still fold. Ignored on a
	 * MetaHuman body. The panel starts it from FMantleCosmeticChecks::SuggestSecondaryMappings.
	 */
	TArray<FMantleSecondaryMapping> SecondaryMotion;

	/**
	 * Any class: publish a new version into this explicitly selected existing item instead of creating
	 * one; empty creates a new item. The API checks the caller's ownership.
	 */
	FString PublishIntoItemId;

	/**
	 * Avatars: per accessory socket and tool socket (hand_rSocket, #585), the parts that replace the
	 * socket's own (sent as the item's "sockets", only for entries with a part on; a tool socket's
	 * bone is checked here, not sent). The panel keeps one entry per limits.json socket,
	 * filled by FMantleCosmeticExport::DefaultSocketOverrides.
	 */
	TArray<FMantleCosmeticSocketOverride> SocketOverrides;

	/** Accessories: an offset from the socket, clamped to what the API accepts. */
	bool bAttach = false;
	FVector3f AttachLocation = FVector3f::ZeroVector;
	FVector3f AttachRotation = FVector3f::ZeroVector;
	FVector3f AttachScale = FVector3f::OneVector;

	/**
	 * Accessories made from a skinned source (a MetaHuman Outfit hat): the rotation of the bone it hangs from in the
	 * source's reference pose. The thumbnail stands it upright as worn; identity draws it in its own axes.
	 */
	FQuat AccessoryWorn = FQuat::Identity;

	/**
	 * Accessories made from cloth (a MetaHuman Outfit): an open surface whose inside shows, as the game draws it (a
	 * two-sided material). The thumbnail draws its back faces too; a seller's cloth material may not, even when marked two-sided.
	 */
	bool bAccessoryBackFaces = false;

	/**
	 * Render the 512 x 512 thumbnail: a scene capture of the avatar's headshot (#625), the garment on its
	 * authoring body or the accessory alone (#364), else the editor's thumbnail renderer. Off, or
	 * in a process that cannot render (-NullRHI), a plain one is used.
	 */
	bool bRenderThumbnail = true;

	/**
	 * #124: how the thumbnail is made, the choice every Mantle uploader offers: Automatic (the render above), Choose the angle (the same
	 * render from the angle the creator took) or a Custom image (a PNG or a texture, cut square and resized to 512 x 512).
	 */
	FMantleThumbnailChoice Thumbnail;

	/**
	 * #849: a converted avatar reads its weight, muscle, chest and hips shapes off its MetaHuman Creator character (about
	 * half a minute). Off where only the checks are wanted (the converter's check pass).
	 */
	bool bBodyOffsets = true;

	/**
	 * A3.8 (#290): avatars (body and face) and garments upload with LOD1 and LOD2 generated here by the editor's
	 * skeletal mesh reduction (limits.json lods.targetFractions of LOD0's triangles); the asset's own LODs are not
	 * uploaded. Hair uses complete-card reductions; accessories preserve authored static LODs through MRM2.
	 * Off uploads one LOD (tests, and A5.1's LODs-off comparison).
	 */
	bool bGenerateLODs = true;
};

/** One cards entry of a groom asset, read by reflection (HairGroupsCards: mesh, LOD, atlases). */
struct MANTLEEDITOR_API FMantleGroomCardsEntry
{
	int32 Index = INDEX_NONE;
	int32 GroupIndex = 0;
	int32 LODIndex = INDEX_NONE;
	TWeakObjectPtr<UStaticMesh> Mesh;
	TArray<TWeakObjectPtr<UTexture2D>> Textures;
	/** Epic EHairTextureLayout (0 default cards, 1 default mesh, 2 compact cards, 3 compact mesh). */
	int32 TextureLayout = 0;
	bool bInvertUV = false;
	/**
	 * The groom's materials (HairGroupsMaterials), the one named by the entry's MaterialSlotName first: where the
	 * hair colour is read from (FMantleHairColor::FromMaterial).
	 */
	TArray<TWeakObjectPtr<UMaterialInterface>> Materials;
	/** Exact cards slot, or the group rendering slot when no cards override exists. */
	TWeakObjectPtr<UMaterialInterface> EffectiveCardsMaterial;

	/** "Group 0, LOD 3: CardsMesh_Group0_LOD3 (2 textures)". */
	FString Describe() const;
};

/** How a texture is tied to a hair cards static mesh, strongest first (FMantleCosmeticExport::PickCardAtlases). */
enum class EMantleCardAtlasLink : uint8
{
	/** A material in one of the cards mesh's own slots references it. */
	Material,
	/** The groom the cards came from (the asset named the groom stem, near the mesh) references it. */
	Groom,
	/** Only its name ties it to the mesh: it must carry the groom stem (CardAtlasBelongsTo). */
	Name,
};

/** One texture the hair cards atlas lookup may choose. */
struct MANTLEEDITOR_API FMantleCardAtlasCandidate
{
	/** Asset name, "Eyebrows_M_FlatThick_CardsAtlas_Attribute". */
	FString Name;
	/** Its folder, "/Game/MetaHumans/Common/Optional/Grooms/GroomAssets/Eyebrows/Eyebrows_M_FlatThick". */
	FString PackagePath;
	EMantleCardAtlasLink Link = EMantleCardAtlasLink::Name;
};

/** #481: one texture parameter of a section's material, as the opacity pick sees it. */
struct MANTLEEDITOR_API FMantleTextureParameter
{
	/** The material's parameter name, "Alpha", "OpacityMask". */
	FString Parameter;
	/** The texture's asset name, "T_Hair_alpha". */
	FString Texture;
	bool bNormalMap = false;
	/** Already the section's base colour, normal or ORM. */
	bool bTaken = false;
};

/** #628: one render section of a static mesh LOD that shares vertices with another: its triangles in the LOD's index buffer, and the glTF material it draws with. */
struct MANTLEEDITOR_API FMantleSharedVertexSection
{
	int32 FirstIndex = 0;
	int32 NumTriangles = 0;
	int32 GlbMaterial = 0;
};

/** #483: one render section of a skeletal mesh LOD, as the section filter sees it. */
struct MANTLEEDITOR_API FMantleSectionSpan
{
	/** The material slot it draws with (an index into the mesh's Materials, the same on every LOD). */
	int32 Slot = 0;
	int32 FirstVertex = 0;
	int32 NumVertices = 0;
	int32 NumTriangles = 0;
};

/** #485: one skeletal mesh FMantleCosmeticExport::FindAuthoringFace may choose, as the asset registry lists it. */
struct MANTLEEDITOR_API FMantleFaceCandidate
{
	/** "/Game/CharacterAssemblies/Ada/Face/SKM_Ada_Face". */
	FString PackageName;
	/** "SKM_Ada_Face". */
	FString AssetName;
	/** The mesh's Skeleton tag (its skeleton's object path). */
	FString Skeleton;
};

/**
 * #620: a material's textures as the upload and the converter read them (FMantleCosmeticExport::ResolveMaterialTextures).
 *
 * A material is plain when the upload can take its base colour texture as it is: the material is Mantle's own
 * (M_MantlePlace, what the converter writes), or its Base Color input is exactly one texture sample (through reroutes
 * and the static switches its instance selects) on UV channel 0, untiled, of a one-block texture, with its RGB output.
 * Anything else (a tint, blend or mask over its textures, a detail map tiled over the UVs, a UDIM texture, material
 * attributes or layers, a flat colour) needs a bake: its colour exists only when the material is evaluated.
 */
struct MANTLEEDITOR_API FMantleMaterialTextures
{
	bool bPlain = false;
	/** Why it is not plain, for the converter's fix list ("its base colour is computed in the material"); empty when plain. */
	FString Why;
	/** The base colour texture: the plain sample's, else by parameter name, else the largest colour texture it samples. */
	UTexture* BaseColor = nullptr;
	/** Taken from the largest colour texture it samples (no parameter named for it): the upload says so. */
	bool bBaseColorFromLargest = false;
	UTexture* Normal = nullptr;
	/** An occlusion / roughness / metallic texture, and the parameter it was found by ("ORM", "RMA", "SRMF Baked VT"). */
	UTexture* Orm = nullptr;
	FString OrmParameter;
	/** #481: a separate opacity texture (FMantleCosmeticExport::PickOpacityTexture), found only when asked for. */
	UTexture* Opacity = nullptr;
	/** Texture parameters the material samples only with tiled UVs (detail maps): never its base colour, normal or ORM. */
	TArray<FName> TiledParameters;
};

/** #481: which channel of an opacity texture holds the opacity. */
enum class EMantleOpacityChannel : uint8
{
	/** A grey (single-channel) texture, or a packed mask with opacity in R. */
	Red,
	/** A texture whose alpha varies and whose colour is flat or an image. */
	Alpha,
};

/**
 * Turns a draft into an upload package (A2.4 #283): exports every file the item needs in the
 * formats the cook worker reads (skinned GLB in Epic's axes with the render vertices and
 * triangles in their own order, rigid GLB, PNG, MAN1, the coverage bitset), checks each against
 * the contract with the worker's own messages, and names them in the item.
 *
 * Garments and hair upload with the body they were authored on as a body shape (its reference
 * LOD, with the RBF warp weights the client needs, FMantleGarmentFitter::ComputeSourceWeights),
 * and garments with their coverage bitset over that body. Hair converted from a groom's cards
 * carries the hair card stream (FMantleHairCards). #430: when the body's MetaHuman face is found,
 * the body shape also carries the neck measured on it (FMantleCatalogGarmentFit::ComputeNeck:
 * samples and a second set of warp weights), and a garment uploads that face as the one it was
 * authored with (faceMeshAssetId), exported exactly as an avatar's face, so a creator who also
 * uploads the avatar sends it once.
 *
 * A3.8: avatar bodies, faces and garments upload with generated LODs (FMantleCosmeticDraft::bGenerateLODs),
 * the uploaded LOD first and the reduced ones after it in one GLB (FMantleGlbWriter::WriteLODs). Hair
 * (cards collapse under quadric reduction, and a groom's cards are already a LOD of it) and body shapes (positions,
 * nothing drawn) keep one. Accessories preserve up to three authored static LODs through WriteStatic/MRM2.
 *
 * Hooks, marked in the code, for work not done here:
 * - #273 (A0.4, the user's materials handoff): which textures the runtime avatar, garment and
 *   hair materials consume. Until then each section uploads its albedo (colour), normal and, for
 *   avatars, ORM texture found by parameter name, or baked through GLTFExporter, or a flat colour.
 * - Hair root UVs from the groom's strands (zeros for now), an avatar's DNA, automatic emote
 *   retargeting. (The fit preview is the panel's:
 *   FMantleCosmeticPreviewSession.)
 */
class MANTLEEDITOR_API FMantleCosmeticExport
{
public:
	/**
	 * The package for Draft: BuildCore, plus the item's provenance (#614 stage 7), the draft's source asset path and import
	 * file name, for a public draft only.
	 */
	static FMantleCosmeticPackage Build(const FMantleCosmeticDraft& Draft, const FMantleAvatarContract& Contract,
		FMantleSkinnedMesh* OutHairPreview = nullptr, FMantleSkinnedMesh* OutHairSourceFace = nullptr);

	/**
	 * Build without the provenance: the files and the item. A skeletal-mesh or cloth-asset garment is first wrapped as an
	 * Outfit for the standard fit sizes (FMantleParametricOutfit::PrepareWrapped), or uploads in its one size when it cannot be.
	 */
	static FMantleCosmeticPackage BuildCore(const FMantleCosmeticDraft& Draft, const FMantleAvatarContract& Contract,
		FMantleSkinnedMesh* OutHairPreview = nullptr, FMantleSkinnedMesh* OutHairSourceFace = nullptr);

	/** BuildCore for a draft that is already as it uploads (an Outfit's prepared size, a garment in its one size): no wrapping. */
	static FMantleCosmeticPackage BuildPrepared(const FMantleCosmeticDraft& Draft, const FMantleAvatarContract& Contract,
		FMantleSkinnedMesh* OutHairPreview = nullptr, FMantleSkinnedMesh* OutHairSourceFace = nullptr);

	/**
	 * #756: for each exported clothing material not already declared by hand, detects its colour areas from its mask
	 * texture (FMantleClothingColorDetect), applies Draft.ClothingColorChoices, measures each kept area against the
	 * exported albedo (Package.Materials[i].AlbedoKey) and adds the clean area map and regions to Package.Materials[i].
	 * Every material's result, found or not, goes to Package.DetectedClothingColors. Off for anything but a top, bottom
	 * or footwear garment or a rigid accessory, when Draft.bDetectClothingColors is false, or when Draft.ClothingColors declares any by hand.
	 * Build calls it; public for tests.
	 */
	static void AddDetectedClothingColors(FMantleCosmeticPackage& Package, const FMantleGlbMesh& Mesh, TConstArrayView<UMaterialInterface*> Materials,
		const FMantleCosmeticDraft& Draft, int32 MaxTextureSize, const FMantleAvatarContract& Contract);

	/** True for a UGroomAsset (by class name: the plugin does not link HairStrands). */
	static bool IsGroomAsset(const UObject* Object);

	/** The groom's cards entries; empty when it has none or is not a groom. */
	static TArray<FMantleGroomCardsEntry> ReadGroomCards(const UObject* Groom);

	/** The entry MetaHuman draws at Medium (cards LOD 3, A2.0), else the lowest LOD with a mesh. */
	static int32 DefaultCardsEntry(const TArray<FMantleGroomCardsEntry>& Entries);

	/**
	 * The groom a cards static mesh was generated from, by the name the groom editor gives it:
	 * "Eyebrows_M_FlatThick_CardsMesh_Group0_LOD0" is "Eyebrows_M_FlatThick". Without "_CardsMesh",
	 * a trailing "_LOD<n>" and "_Group<n>" (and a leading "SM_") are dropped instead.
	 */
	static FString CardsGroomStem(const FString& CardsMeshName);

	/**
	 * True when TextureName is a cards atlas named for the groom Stem: Stem (after an optional
	 * "T_"), then "_" and straight away an atlas word (Cards, Atlas, Attribute, Coverage, Tangent).
	 * "Hair_S_Casual_CardsAtlas_Attribute" belongs to Hair_S_Casual; it does not belong to
	 * Eyebrows_M_FlatThick, and "Hair_S_Casual_Long_CardsAtlas_Attribute" does not belong to Hair_S_Casual.
	 */
	static bool CardAtlasBelongsTo(const FString& TextureName, const FString& Stem);

	/**
	 * The attribute (coverage, depth, seed) and tangent atlases of the cards mesh CardsMeshName in
	 * MeshFolder among Candidates, as indices (INDEX_NONE where none belongs to it). A candidate is
	 * an atlas by name (Attribute or Coverage, or Tangent, and Atlas or Cards). The strongest link
	 * wins (material, then groom, then name); a name-only candidate must carry the mesh's groom stem,
	 * so another groom's atlas in the same folder is never taken. Ties go to the stem's own name,
	 * then the mesh's own folder, then the longest shared prefix with the mesh name.
	 */
	static void PickCardAtlases(const FString& CardsMeshName, const FString& MeshFolder, const TArray<FMantleCardAtlasCandidate>& Candidates,
		int32& OutAttribute, int32& OutTangent);

	/** The first LOD of Mesh with the reference body topology (HasReferenceTopology), INDEX_NONE when none. */
	static int32 FindReferenceLOD(const USkeletalMesh* Mesh, const FMantleAvatarContract& Contract);
	/** Eligible native head LOD, preferring the reference face count; INDEX_NONE requires explicit same-source normalization. */
	static int32 ChooseHeadSourceLOD(TConstArrayView<int32> TriangleCounts, int32 MaxTriangles);
	/**
	 * Native groom hair: the finest cards LOD whose groups, summed, fit MaxTriangles. PerLODGroupTriangles is ordered
	 * finest first, one array of per-group triangle counts per LOD. When none fits, the coarsest LOD that draws anything
	 * (the caller trims it); INDEX_NONE when no LOD draws. MaxTriangles <= 0 means no budget (the finest LOD).
	 */
	static int32 ChooseGroomCardsLOD(const TArray<TArray<int32>>& PerLODGroupTriangles, int32 MaxTriangles);

	/**
	 * The shipped MetaHuman body family is also matched against body-layout-v1.json: a geometry-free UV/connectivity
	 * layout. Complete correspondence is verified even when render order differs from MeshDescription order. In that
	 * case *bOutDescriptionOrder (retained API name) means any required reorder, and OutRenderOfReference is a bijection.
	 * No source positions, weights or UVs are changed; the whole-body export uses it for every vertex attribute.
	 *
	 * Whether LOD LODIndex of Mesh has Mantle's reference topology (the contract's vertex count and UV hash), in one of two
	 * orders:
	 *   - render order: its render data's vertices in their own order (an imported MetaHuman body);
	 *   - description order (#620): its mesh description's vertices by id (ReadDescriptionVertices), when its render data
	 *     is in the editor build's own order. A catalog reference body is built from cooked data (FMantleReferenceBodies),
	 *     and a body written in its order carries its description, so their identity is the description's: the build renumbers
	 *     the vertices, and nothing reads the reference order back from render data without OutRenderOfReference.
	 * For description order, *bOutDescriptionOrder is set and OutRenderOfReference gets each reference vertex's render
	 * vertex: from the LOD model's MeshToImportVertexMap (render vertex to description vertex id), else by matching
	 * positions and UVs. The render UVs read through it must give the contract's hash too, or the LOD does not count, so
	 * a render-order consumer that reads through it reads the reference order exactly. Empty for render order. Editor
	 * data; game thread.
	 */
	static bool HasReferenceTopology(const USkeletalMesh* Mesh, int32 LODIndex, const FMantleAvatarContract& Contract,
		bool* bOutDescriptionOrder = nullptr, TArray<int32>* OutRenderOfReference = nullptr);

	/**
	 * #620: LOD LODIndex's vertices in its mesh description's order: per vertex instance, by id (one instance per cooked
	 * vertex, made in the cooked order, ids from 0 with no holes, as FMantleReferenceBodies builds them), its vertex's
	 * position and its UV0. *bOutVertexPerInstance: each instance i is on vertex i (nothing welded), so vertex ids are the
	 * same order. False without a description or with holes in the instance ids. Editor data; game thread.
	 */
	static bool ReadDescriptionVertices(const USkeletalMesh* Mesh, int32 LODIndex, TArray<FVector3f>& OutPositions, TArray<FVector2f>& OutUVs,
		bool* bOutVertexPerInstance = nullptr);

	/** Garments prefer Near's body folder and require canonical topology. Hair resolves exact Groom/head provenance or one unambiguous character body, never a global fallback. */
	static USkeletalMesh* FindAuthoringBody(const UObject* Near, const FMantleAvatarContract& Contract, bool bRequireCanonicalTopology = true);

	/** #430: Draft.AuthoringFace, else the MetaHuman face next to Body (its MetaHuman folder); null when there is none. */
	static USkeletalMesh* FindAuthoringFace(const FMantleCosmeticDraft& Draft, const USkeletalMesh* Body);

	/**
	 * #485: the MetaHuman folder a package sits in, its first three path segments: "/Game/MetaHumans/<Name>" for a
	 * MetaHuman Creator export, "/Game/CharacterAssemblies/<Name>" for a Cinematic MetaHuman package from Fab (body
	 * under Body/, face under Face/, outfit under Clothing/). Empty when the package is not inside a folder that deep.
	 * Pure (tested).
	 */
	static FString MetaHumanFolder(const FString& PackageName);

	/**
	 * #485: FindAuthoringBody's filter without the asset registry: a mesh on the MetaHuman body skeleton that is not a
	 * garment or a face by its name, and not under a Clothing folder (an assembly's outfit shares the body skeleton).
	 * Pure (tested).
	 */
	static bool CouldBeAuthoringBody(const FString& AssetName, const FString& PackageName, const FString& Skeleton);

	/**
	 * #485: FindAuthoringFace's pick without the asset registry, among the skeletal meshes of the body's MetaHuman folder
	 * (any order): a face ("Face" in its name and in its skeleton's) whose name shares the longest prefix with BodyName,
	 * then the first by package path; when none is named for a face, a mesh on the face skeleton under a Face/ folder.
	 * INDEX_NONE when none is a face. Pure (tested).
	 */
	static int32 PickAuthoringFace(const FString& BodyName, TConstArrayView<FMantleFaceCandidate> Candidates);

	/**
	 * #535: the project's UEFN mannequin, the mesh emotes are made on and the avatar preview's hidden driver: a skeletal
	 * mesh whose skeleton (by the asset registry's Skeleton tag, CouldBeUEFNMannequin) is a UEFN mannequin skeleton and
	 * has every bone of the contract's uefn fixture (virtual bones aside). SKM_UEFN_Mannequin first, then by package
	 * path. Null when the project has none; callers then use the one the plugin builds in memory
	 * (FMantleBuiltInMannequin::UEFNMannequin).
	 */
	static USkeletalMesh* FindUEFNMannequin(const FMantleAvatarContract& Contract);

	/**
	 * #535: FindUEFNMannequin's filter without the asset registry: a mesh whose Skeleton tag names a UEFN mannequin
	 * skeleton ("UEFN_Mannequin" in it, any case). Pure (tested).
	 */
	static bool CouldBeUEFNMannequin(const FString& Skeleton);

	/** #535: whether Skeleton has every bone of the contract's Fixture skeleton, virtual ("VB ") bones aside. */
	static bool HasFixtureBones(const USkeleton* Skeleton, const FMantleContractSkeleton& Fixture, FString* OutMissing = nullptr);

	/** skin / eyes / other per section of Mesh's LOD, from MetaHuman's material slot names. */
	static TArray<FString> DefaultSectionRoles(const USkeletalMesh* Mesh, int32 LODIndex);

	/** Builds metadata-only section bindings for converted MetaHuman geometry; no source material is inspected or exported. */
	static bool BuildClientAppearanceMaterials(const FMantleGlbMesh& Mesh, const TArray<FString>& Roles, bool bFace,
		TArray<FMantleCosmeticMaterialRef>& OutMaterials, FString& OutError);

	/** Restores a reducer LOD's section-index map from source render-section ordinals, rejecting conflicting existing maps. */
	static bool BuildReducedLODMaterialMap(TConstArrayView<int32> SourceSectionSlots, TConstArrayView<int32> GeneratedMaterialIndices,
		TConstArrayView<int32> ExistingLODMaterialMap, int32 MaterialSlotCount, TArray<int32>& OutLODMaterialMap, FString& OutError);

	/** Avatar face only: bound auxiliary eye/teeth textures while keeping skin and stricter class limits. */
	static int32 FaceSectionTextureLimit(int32 ClassLimit, const FString& Role, const FString& Section);

	/** Each render section's material slot name, by section index (what Draft.SourceSection indexes). */
	static TArray<FString> RenderSectionNames(const USkeletalMesh* Mesh, int32 LODIndex);

	/** Distinct material slot names of Mesh's LOD in the order sections first use them: the uploaded mesh's sections. */
	static TArray<FString> SectionMaterialNames(const USkeletalMesh* Mesh, int32 LODIndex);

	/** Triangles in LOD LODIndex of Mesh; #483: only in the sections of KeepSlots' material slots when it is given. */
	static int32 CountTriangles(const USkeletalMesh* Mesh, int32 LODIndex, const TArray<int32>* KeepSlots = nullptr);

	/** #483: LOD LODIndex's render sections of Mesh, by section index (empty when it has no such LOD). */
	static TArray<FMantleSectionSpan> ReadSectionSpans(const USkeletalMesh* Mesh, int32 LODIndex);

	/**
	 * #483: the render sections an upload reads, by section index in vertex order (then section order): every section
	 * when KeepSlots is null, else those drawn with one of its material slots. Pure (tested).
	 */
	static TArray<int32> KeptSections(TConstArrayView<FMantleSectionSpan> Sections, const TArray<int32>* KeepSlots);

	/** #483: triangles in the sections KeptSections reads. Pure (tested). */
	static int32 CountKeptTriangles(TConstArrayView<FMantleSectionSpan> Sections, const TArray<int32>* KeepSlots);

	/**
	 * #483: the material slots LOD LODIndex of Mesh draws, in the order sections first use them, with the names the
	 * outfit classifier reads (FMantleOutfitSections); OutSlots gets each one's material slot index.
	 */
	static TArray<FMantleOutfitSection> ReadOutfitSections(const USkeletalMesh* Mesh, int32 LODIndex, TArray<int32>& OutSlots);

	/** The LOD Build uploads for an avatar body: the reference-topology LOD, else the first within the triangle limit. */
	static int32 AvatarBodyLOD(const USkeletalMesh* Body, const FMantleAvatarContract& Contract);

	/**
	 * #283: the appearance skeleton id (metahuman, ue5mannequin, ue4mannequin, uefnmannequin) Build identifies for an avatar body, the way the
	 * upload does (its AvatarBodyLOD fitted to each avatar skeleton, the fewest bones left out winning), or false with
	 * the worker's message. The avatar preview picks the body's retargeter by it.
	 */
	static bool AvatarSkeletonId(const USkeletalMesh* Body, const FMantleAvatarContract& Contract, FString& OutSkeletonId, FString& OutError);

	/**
	 * One override per accessory socket of the contract, then one per tool socket (limits.json
	 * toolSockets, #585: hand_rSocket on hand_r), every part off, its values the socket's
	 * own on Body's skeleton when it has that socket (location, rotation and scale relative to the
	 * bone), else zero offset, no rotation, scale 1. Body may be null.
	 */
	static TArray<FMantleCosmeticSocketOverride> DefaultSocketOverrides(const USkeletalMesh* Body, const FMantleAvatarContract& Contract);

	/**
	 * The texture's source pixels as an RGBA PNG, scaled down to MaxSize on its longer side when
	 * larger. bLinear keeps data (normal maps, masks) linear; colour comes out sRGB.
	 */
	static bool TextureToPng(const UTexture* Texture, int32 MaxSize, bool bLinear, TArray<uint8>& OutPng, FIntPoint& OutSize, FString& OutError);

	/**
	 * Writes "Mantle-Role" = Role as a tEXt chunk right after the PNG's IHDR, so a texture uploaded for one role never shares
	 * its exact bytes with one uploaded for another (the API keeps one role per file). Pixels are unchanged; false leaves
	 * Png alone when it isn't a PNG with an IHDR first.
	 */
	static bool TagPngRole(TArray<uint8>& Png, const FString& Role);

	/** Whether every pixel of the PNG has R = G = B, the API's test for a normal map it refuses; false when it can't be read. */
	static bool IsGreyPng(const TArray<uint8>& Png);

	/**
	 * #575: the check-list line for a texture its export scales down (FMantleCosmeticChecks::CheckTextureSize on its
	 * source size, against MaxSize capped by the contract). Adds nothing for a texture within the limit or without source
	 * pixels; its export reports those.
	 */
	static void CheckSourceTextureSize(const UTexture* Texture, int32 MaxSize, const FString& What, const FMantleAvatarContract& Contract,
		TArray<FMantleCosmeticIssue>& OutIssues);

	/**
	 * #469: one LOD of a skeletal mesh as an upload GLB's mesh, the reader avatars and garments use (shared with the place
	 * converter): render vertices and triangles in their own order, every section, every bone of the raw reference skeleton
	 * as a joint with its reference pose, and at most eight influences a vertex (#484: the heaviest kept and renormalised).
	 * OutSlots[i] is the mesh's material slot glTF material i came from (UploadMaterialSlots' order). OutCappedVertices is how
	 * many vertices had influences dropped and OutLargestDropped the largest weight one lost. False with a sentence for the
	 * creator when the LOD has no CPU copy or its sections share vertices. #620: a LOD whose reference topology is in its
	 * mesh description's order (HasReferenceTopology) is read in that order, so the GLB has the reference topology.
	 */
	static bool ReadSkeletalMesh(const USkeletalMesh* Mesh, int32 LODIndex, FMantleGlbMesh& OutMesh, TArray<int32>& OutSlots, int32& OutTriangles,
		int32& OutCappedVertices, float& OutLargestDropped, FString& OutError);

	/**
	 * #469: the material slots LOD LODIndex of Mesh's upload GLB lists as its glTF materials, in the order its sections (by
	 * vertex, then section order) first use them: what ReadSkeletalMesh's OutSlots holds, without reading the vertices.
	 */
	static TArray<int32> UploadMaterialSlots(const USkeletalMesh* Mesh, int32 LODIndex);

	/**
	 * One LOD of a static mesh as an upload GLB's mesh, the reader accessories use (shared with the
	 * place converter, F5 #339): render vertices and triangles in their own order, one primitive
	 * per section, one glTF material per distinct material slot in the order sections first use
	 * them (the cook's slot order). OutSlots[i] is the mesh's material slot glTF material i came
	 * from. False with a sentence for the creator when the LOD has no CPU copy. Sections that share
	 * vertices across materials are split (#628, SplitSharedVertexSections), so the sections tile the vertices.
	 */
	static bool ReadStaticMesh(const UStaticMesh* Mesh, int32 LODIndex, FMantleGlbMesh& OutMesh, TArray<int32>& OutSlots, int32& OutTriangles, FString& OutError);

	/**
	 * #628: Glb's per-vertex arrays (positions, normals, tangents, UVs, UV1s, colours; any that are empty stay empty)
	 * rebuilt so each of Sections has its own vertices, and one primitive per section, in Sections' order, appended to
	 * Glb.Primitives: a vertex two sections use is written once for each, and each section's indices are renumbered into
	 * its own range. The primitives tile the new vertex arrays in order, which is the layout the runtime reader needs;
	 * every triangle keeps its corners' positions and attributes. Vertices no section uses are dropped.
	 */
	static void SplitSharedVertexSections(FMantleGlbMesh& Glb, TConstArrayView<uint32> Indices, TConstArrayView<FMantleSharedVertexSection> Sections);

	/** #628: how many vertices SplitSharedVertexSections writes for Sections: each section's distinct vertices, summed. */
	static int32 CountSplitVertices(TConstArrayView<uint32> Indices, TConstArrayView<FMantleSharedVertexSection> Sections);

	/**
	 * One LOD of a skeletal mesh as an upload GLB's skinned mesh (a creator vehicle's body, #435): render vertices
	 * and triangles in their own order, one primitive per render section, one glTF material per slot in the order
	 * sections first use them, the full raw reference skeleton as joints, weights as floats (heaviest first, at
	 * most eight). OutMaterials[i] is glTF material i's material. False with a sentence for the creator when the
	 * LOD has no CPU copy or its sections share vertices.
	 */
	static bool ReadSkeletalMesh(const USkeletalMesh* Mesh, int32 LODIndex, FMantleGlbMesh& OutMesh, TArray<UMaterialInterface*>& OutMaterials,
		int32& OutTriangles, FString& OutError);

	/** One bit per reference vertex, least significant first, padding clear: the upload's coverage bytes. */
	static TArray<uint8> PackCoverageBits(const TBitArray<>& Covered, int32 NumBits);

	/**
	 * #481: the opacity texture among a material's texture parameters, INDEX_NONE when there is none. Normal maps and
	 * textures already taken (base colour, normal, ORM) are never picked. A parameter named for opacity (Opacity,
	 * Alpha, Transparency, Cutout) wins, in parameter order; else a texture whose name ends in one of those words
	 * ("T_Hair_alpha"). Pure (tested).
	 */
	static int32 PickOpacityTexture(TConstArrayView<FMantleTextureParameter> Parameters);

	/**
	 * #481: A when some texel's alpha is below 255 and the colour carries nothing else (one flat colour, or a colour
	 * image); else R (a grey single-channel mask, or a packed mask with opacity in R). Pure (tested).
	 */
	static EMantleOpacityChannel ChooseOpacityChannel(TConstArrayView<FColor> OpacityPixels);

	/**
	 * #481: writes Channel of each opacity texel into the albedo texel's alpha, keeping its colour. The two are the
	 * same size (the caller resizes); a length mismatch packs the texels both have. Pure (tested).
	 */
	static void PackOpacityIntoAlpha(TArrayView<FColor> AlbedoPixels, TConstArrayView<FColor> OpacityPixels, EMantleOpacityChannel Channel);

	/**
	 * #620: a material's base colour, normal and ORM textures, and whether it is plain (FMantleMaterialTextures). The
	 * upload and the converter's bakes both read textures this way. By parameter name (BaseColor, Albedo, Diffuse, Color,
	 * ...; Normal; ORM, RMA, ARM, SRMF, Roughness) as before, except that a MetaHuman animated wrinkle or colour-mask map
	 * (IsAnimatedMaskTexture) and a texture parameter sampled only with tiled UVs (a detail map) are never taken; a plain
	 * material's base colour (and normal, when that is a plain sample too) is the sample's own texture whatever its
	 * parameter is called. bPickOpacity also finds a separate opacity texture (hair), which the base colour fallback then
	 * skips.
	 */
	static FMantleMaterialTextures ResolveMaterialTextures(UMaterialInterface* Material, bool bPickOpacity = false);

	/**
	 * #620: a MetaHuman animated wrinkle / colour-mask map by its parameter or texture name ("Basecolor Animated Delta
	 * cm1", "T_Face_Basecolor_Animated_CM1", "T_Face_Normal_Animated_WM2", "T_head_wm1_msk_01"): a delta blended over
	 * the real map while the face animates, never a base colour or normal of its own. Pure (tested).
	 */
	static bool IsAnimatedMaskTexture(const FString& Name);
};
