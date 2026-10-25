// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleAvatarConvertFixer.h"
#include "MantleCosmeticValidation.h"
#include "Templates/Function.h"

struct FMaterialShadingModelField;
class UMaterialInstanceConstant;
class UMaterialInterface;

/** How a material slot blends, as the atlas groups it. */
enum class EMantleAtlasBlend : uint8
{
	Opaque,
	Masked,
	/** Translucent, additive, modulate: never merged with another slot. */
	Translucent,
};

/** One material slot a part's LOD 0 draws, as the atlas fixer plans it (FMantleAvatarAtlasFixer::PlanGroups). */
struct MANTLEEDITOR_API FMantleAtlasSlot
{
	/** The mesh's material slot index. */
	int32 Slot = INDEX_NONE;
	/** Its name as the upload names the section. */
	FString Name;
	EMantleAtlasBlend Blend = EMantleAtlasBlend::Opaque;
	bool bTwoSided = false;
	/**
	 * Avatars: "skin", "eyes" or "other" (the role the upload gives the section, which the game tints by); empty for
	 * garments and hair. Slots of different roles are never merged: the skin tint would colour a shirt.
	 */
	FString Role;
	/** Masked hair (hair cards, lashes, brows, beards, fur): never merged into an opaque atlas. */
	bool bHair = false;
	/** A part the game hides (eyelashes, eye shell, eye edge, saliva, cartilage): never merged, or it would show. */
	bool bHidden = false;
	/** Where its UV0 sits (FMantleCosmeticChecks::MeasureUVTiles): a UDIM layout is baked into one 0..1 tile. */
	FMantleUVTiles Tiles;
	/** Source texels its UV rectangles cover (texture size squared times UV area): what merging it costs. */
	double TexelNeed = 0.0;
};

/** Slots baked into one atlas: a merge of several, or one UDIM slot baked alone. */
struct MANTLEEDITOR_API FMantleAtlasGroup
{
	/** Material slot indices (FMantleAtlasSlot::Slot), in slot order. */
	TArray<int32> Slots;
	/** Masked when any member is (its opaque members get full opacity); Translucent only for a lone UDIM slot. */
	EMantleAtlasBlend Blend = EMantleAtlasBlend::Opaque;
	bool bTwoSided = false;
	FString Role;
	bool bHair = false;
};

/** One rectangle of an atlas: one slot's UVs, or one UV tile of a UDIM slot, and the atlas pixels they move to. */
struct MANTLEEDITOR_API FMantleAtlasCell
{
	int32 Slot = INDEX_NONE;
	/** The UV tile a UDIM slot's cell bakes; unused (bWholeSlot) for a slot baked as one rectangle. */
	FIntPoint Tile = FIntPoint::ZeroValue;
	bool bWholeSlot = true;
	/** The source UV rectangle (never empty: a slot whose UVs collapse to a point gets a tiny one around it). */
	FBox2f Source = FBox2f(ForceInit);
	/** Source texels per UV unit (the largest texture the slot's material uses): the cell's size at full resolution. */
	int32 TexelsPerUV = 1024;
	/** Atlas pixels the source rectangle maps onto, Max exclusive; the gutter lies outside it. */
	FIntRect Inner;
};

/** An atlas: its square size and its cells. */
struct MANTLEEDITOR_API FMantleAtlasLayout
{
	int32 Size = 0;
	/** Gutter pixels around every cell, baked from the source beyond the cell's rectangle (dilated content). */
	int32 Padding = 2;
	/** The share of full resolution the cells were packed at (1: no source texel lost). */
	float Scale = 1.0f;
	TArray<FMantleAtlasCell> Cells;
};

/** One face (a triangle, or a polygon of the mesh description) as the cell builder sees it. */
struct MANTLEEDITOR_API FMantleAtlasFace
{
	int32 Slot = INDEX_NONE;
	TArray<FVector2f, TInlineAllocator<4>> UVs;
};

/** Knobs for tests and for the creator's choice; the fixer's Apply uses the defaults. */
struct MANTLEEDITOR_API FMantleAtlasSettings
{
	/**
	 * Render each source material through the editor's material baking (IMaterialBakingModule) so any material bakes,
	 * not just one with a texture parameter: UDIM and virtual-textured materials too (#625: sampled before, which lost a
	 * MetaHuman skin's tint). Off, or with no renderer (-NullRHI), the source textures are sampled directly (base colour,
	 * normal and ORM found by parameter name, as the upload finds them) and a material without textures gives its flat
	 * colour.
	 */
	bool bRenderedBake = true;
	/**
	 * Merge only as many slots as the limit needs (the cheapest first) instead of every compatible slot into one. On by
	 * default (decided 2026-09-30, #610): the fewer slots share an atlas under the 2048 cap, the more texels each keeps.
	 */
	bool bMergeOnlyToLimit = true;
	/** Gutter pixels on each side of a cell (2 to 4). */
	int32 Padding = 2;
};

/** #620: one material baked over one UV rectangle (FMantleAvatarAtlasFixer::BakeMaterial), Size x Size, row-major. */
struct MANTLEEDITOR_API FMantleBakedTextures
{
	int32 Size = 0;
	/** sRGB colour; opacity in alpha for a masked or translucent material, else 255. */
	TArray<FColor> BaseColor;
	/** Tangent-space normal. */
	TArray<FColor> Normal;
	/** Occlusion, roughness, metallic. */
	TArray<FColor> Orm;
};

/**
 * The avatar converter's Atlas fixer (#610): a part whose LOD 0 draws more material slots than its slot's limit
 * (limits.json maxSections: avatar 8, garments and hair 4), or has a section laid out on UDIM tiles, gets a new mesh
 * whose chosen slots share baked atlases.
 *
 * Planning (PlanGroups): translucent slots and slots the game hides never merge; avatars never mix roles (skin, eyes,
 * other); masked hair never merges with anything but masked hair. Compatible slots merge into one atlas each (or, with
 * bMergeOnlyToLimit, only as many as the limit needs); an opaque atlas that takes a masked non-hair slot turns masked.
 * A UDIM slot left out of every merge is baked alone. When the limit cannot be met that way the fix says why and is
 * not offered.
 *
 * Layout (BuildCells, PackLayout): every (slot, UV tile) becomes a rectangle of the atlas sized by the texels it needs
 * (its UV rectangle times its material's largest texture), shelf-packed with a gutter into the smallest power-of-two
 * square that holds them at full resolution, else into Context.MaxTextureSize at the largest scale that fits. UV
 * channel 0 of every vertex moves into its cell by a translation and a positive scale, so tangents keep their direction
 * and the baked tangent-space normals stay correct; other UV channels are kept.
 *
 * Apply writes new assets only: a duplicate of the part's mesh (its skin weights, normals, tangents, morph targets,
 * sockets and physics kept) whose every LOD with source geometry is remapped the same way from LOD 0's layout (LODs the
 * mesh generates by reduction are regenerated from the new LOD 0 by the rebuild), the atlas textures (base colour sRGB
 * with opacity in alpha for a masked atlas, tangent-space normal, ORM: occlusion, roughness, metallic) and one material
 * instance per atlas on M_MantlePlace's Opaque, Masked or Translucent permutation, whose BaseColor, Normal and ORM
 * texture parameters are what the upload reads. The new mesh is checked with the upload's own section and UV tile
 * checks before the part is pointed at it.
 */
class MANTLEEDITOR_API FMantleAvatarAtlasFixer : public IMantleAvatarFixer
{
public:
	virtual FName GetId() const override { return MantleAvatarFixerIds::Atlas; }
	virtual void Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const override;
	virtual bool Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const override;

	/** Analyze for one part. */
	static void AnalyzePart(const FMantleConvertContext& Context, int32 PartIndex, TArray<FMantleConvertFix>& OutFixes, const FMantleAtlasSettings& Settings = FMantleAtlasSettings());

	/** Apply for one part, with Settings (tests turn the rendered bake off to stay deterministic without a GPU). */
	static bool ApplyToPart(FMantleConvertContext& Context, int32 PartIndex, const FMantleAtlasSettings& Settings, FString& OutError);

	/** The section limit of a part's slot (limits.json maxSections, else maxMaterials); 0 when it has none or is unknown. */
	static int32 SectionLimitOf(const FString& Slot);

	/**
	 * Which slots to bake together so at most Limit slots remain (Limit 0: no limit) and no slot is left on UDIM tiles.
	 * Empty and true: nothing to do. False with the reason when the limit cannot be met without merging slots that must
	 * stay apart. Pure (tested).
	 */
	static bool PlanGroups(TConstArrayView<FMantleAtlasSlot> Slots, int32 Limit, bool bMergeOnlyToLimit, TArray<FMantleAtlasGroup>& OutGroups, FString& OutWhyNot);

	/**
	 * The cells of one group from its faces: a slot on UDIM tiles (bUdimSlot) gets a cell per tile its faces' UV centroids
	 * fall in, any other slot one cell over all its UVs. OutFaceCell[i] is face i's cell (INDEX_NONE for a face of a slot
	 * outside the group). TexelsPerUV gives each slot's source density. Pure (tested).
	 */
	static void BuildCells(TConstArrayView<FMantleAtlasFace> Faces, const FMantleAtlasGroup& Group, TFunctionRef<bool(int32 Slot)> bUdimSlot,
		TFunctionRef<int32(int32 Slot)> TexelsPerUV, FMantleAtlasLayout& OutLayout, TArray<int32>& OutFaceCell);

	/** Places Layout's cells (Padding kept around each) in a square of at most MaxSize; false when they cannot fit. Pure (tested). */
	static bool PackLayout(FMantleAtlasLayout& Layout, int32 MaxSize);

	/** UV in a cell's source rectangle to its place in the atlas (0..1). Pure (tested). */
	static FVector2f RemapUV(const FMantleAtlasLayout& Layout, int32 Cell, const FVector2f& UV);

	/** The cell of Slot a face with this UV centroid goes to: its tile's, else the nearest; INDEX_NONE when Slot has none. */
	static int32 FindCell(const FMantleAtlasLayout& Layout, int32 Slot, const FVector2f& Centroid);

	/**
	 * The fix list's action for a plan: "merges 10 sections into 1 atlas (2048 squared; base colour, normal, ORM)", "bakes UDIM
	 * tiles 1001-1002 of "Body" into one 2048 squared texture set", with the resolution kept when the atlas had to shrink.
	 */
	static FString DescribeAction(TConstArrayView<FMantleAtlasSlot> Slots, TConstArrayView<FMantleAtlasGroup> Groups, TConstArrayView<FMantleAtlasLayout> Layouts);

	// #620: the atlas's bake of one cell, for a whole section (the Bake fixer, FMantleAvatarBakeFixer).

	/**
	 * Material baked over UV Rectangle into a Size x Size texture set, exactly as the atlas bakes a cell: rendered through
	 * the material (tints, detail maps, masks and blends all bake; UDIM and virtual-textured materials too, #625) unless
	 * Settings turns that off or there is no renderer; then its textures (FMantleCosmeticExport::ResolveMaterialTextures)
	 * are sampled directly, a UDIM texture by the tile each texel's UV falls in. Notes say what fell back.
	 */
	static void BakeMaterial(UMaterialInterface* Material, const FBox2f& Rectangle, int32 Size, const FMantleAtlasSettings& Settings,
		FMantleBakedTextures& Out, TArray<FString>& OutNotes);

	/**
	 * Whether a rendered bake takes the material's emissive as its base colour: the material is unlit (VRoid's MToon and
	 * other toon shaders put all their colour there), or its base colour is black at every texel; and the emissive has
	 * colour. A lit material with a real base colour keeps it. Pure (tested).
	 */
	static bool BakesEmissiveAsColour(bool bUnlit, TConstArrayView<FColor> BaseColor, TConstArrayView<FColor> Emissive);

	/**
	 * #167: whether a rendered bake takes a material's Metallic pin as metal, and its Normal pin as a normal map. The Hair shading
	 * model reuses both (Metallic is Scatter, Normal the strand Tangent), the Eye model Metallic (Curvature): baked as metal they
	 * turn the colour near-black, so those bake metal 0 and a flat normal. Pure (tested).
	 */
	static bool BakesMetallic(const FMaterialShadingModelField& Models);
	static bool BakesNormal(const FMaterialShadingModelField& Models);

	/** Whether BakeMaterial renders Material (else it samples its textures). */
	static bool WouldRender(const UMaterialInterface* Material, const FMantleAtlasSettings& Settings);

	/**
	 * Textures as new assets under Folder (T_<Stem>_BaseColor, _Normal, _ORM, compressed as the atlas's are) and an instance
	 * of M_MantlePlace's permutation for Blend and bTwoSided (MI_<Stem>) whose BaseColor, Normal and ORM parameters are
	 * them: what the upload reads as a plain material. Everything made is added to InOutMade. Null with the reason.
	 */
	static UMaterialInstanceConstant* CreateBakedMaterial(const FString& Folder, const FString& Stem, const FMantleBakedTextures& Textures,
		EMantleAtlasBlend Blend, bool bTwoSided, TArray<UObject*>& InOutMade, FString& OutError);

	/** Deletes assets a failed fix made (CreateBakedMaterial's), as a failed atlas deletes its own. */
	static void DiscardMade(TArray<UObject*>& Made);

	/** How a material blends, as the atlas groups it. */
	static EMantleAtlasBlend BlendOf(const UMaterialInterface* Material);

	/**
	 * The largest source texture Material samples, within [32, MaxSize]: the resolution its bake keeps. The textures are
	 * GetUsedTextures's; without a renderer (which gives none there, #625), those its parameters and graph name.
	 */
	static int32 TexelsPerUVOf(const UMaterialInterface* Material, int32 MaxSize);
};

/** Registers the Atlas fixer with FMantleAvatarFixers (MantleEditor module startup). */
MANTLEEDITOR_API void RegisterMantleAvatarAtlasFixer();
