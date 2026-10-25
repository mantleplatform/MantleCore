// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleAvatarAtlasFixer.h"
#include "MantleAvatarConvertFixer.h"

class UMaterialInterface;
class USkeletalMesh;

/** One texture set the Bake fixer makes (FMantleAvatarBakeFixer::PlanMesh): a material baked for one or more slots. */
struct MANTLEEDITOR_API FMantleBakeTarget
{
	/** The mesh's material slots that take the baked material (a MetaHuman face's skin slots, one per LOD, share one). */
	TArray<int32> Slots;
	/** The first slot's section name, for the fix list. */
	FString Name;
	/** The material whose shading is baked: the slot's own, or for a face's skin the skin material its LOD 0 draws. */
	TWeakObjectPtr<UMaterialInterface> Material;
	/** Why the upload cannot read it as it is (FMantleMaterialTextures::Why). */
	FString Why;
	/** The UV tile the slot's triangles lie on: the bake covers [Tile, Tile + 1], so the section keeps its own UVs. */
	FIntPoint Tile = FIntPoint::ZeroValue;
	/** Baked texture size: the material's largest texture, to a power of two within [64, the texture cap]. */
	int32 Size = 0;
	/** Rendered through the material, UDIM and virtual-textured ones included (else its textures are sampled: no renderer). */
	bool bRendered = false;
};

/**
 * #620, the converter's Bake step (after Atlas): every section the upload would read wrongly gets its material baked,
 * on its own UVs, into a base colour, normal and ORM texture set on Mantle's own material, which the upload reads as it
 * is. A section needs the bake when its material is not plain (FMantleMaterialTextures): MetaHuman Creator clothing's
 * tints over tiling detail maps, a UDIM or virtual-textured skin, a MetaHuman face whose texture slots also hold
 * animated wrinkle maps, any blend, mask or layered material.
 *
 * Which sections: every section of a garment, and of an avatar body except its eyes and the parts the game hides; of an
 * avatar's face, its skin. A face's skin is baked once, from the skin material its LOD 0 draws (the full MetaHuman head
 * material: lower LODs' head slots can carry cheaper variants, and every MetaHuman head LOD shares one UV layout), and
 * every skin slot of every LOD takes it, so whichever LOD the upload picks by the triangle budget shows the real skin.
 * A section laid out on UDIM tiles is the Atlas fixer's (which runs first); one left on them is not baked.
 *
 * The bake is the atlas's (FMantleAvatarAtlasFixer::BakeMaterial) over the UV tile the section lies on, at up to
 * Context.MaxTextureSize (2048): the geometry, UVs, sections and LODs are untouched; only the slots' materials change,
 * on a copy of the mesh (FMantleAvatarConverter::MakeWritable), never the creator's. Its fixes are per mesh: Kind Mesh
 * (the part's mesh) and Face (an avatar's face).
 */
class MANTLEEDITOR_API FMantleAvatarBakeFixer : public IMantleAvatarFixer
{
public:
	static const FName KindMesh;
	static const FName KindFace;

	virtual FName GetId() const override { return MantleAvatarFixerIds::Bake; }
	virtual void Analyze(const FMantleConvertContext& Context, TArray<FMantleConvertFix>& OutFixes) const override;
	virtual bool Apply(const FMantleConvertFix& Fix, FMantleConvertContext& Context, FString& OutError) const override;

	/** Analyze for one part. */
	static void AnalyzePart(const FMantleConvertContext& Context, int32 PartIndex, TArray<FMantleConvertFix>& OutFixes,
		const FMantleAtlasSettings& Settings = FMantleAtlasSettings());

	/** Apply for one part's mesh (bFace: its face), with Settings (tests turn the rendered bake off to stay deterministic). */
	static bool ApplyToPart(FMantleConvertContext& Context, int32 PartIndex, bool bFace, const FMantleAtlasSettings& Settings, FString& OutError);

	/**
	 * What Mesh needs baked: bAvatar leaves out the eyes and hidden parts, bFace keeps only the skin, OnlySlot (a lifted
	 * section's slot, else INDEX_NONE) keeps one. OutNotes says what was left alone and why. MaxSize caps the bake.
	 */
	static void PlanMesh(const USkeletalMesh* Mesh, bool bAvatar, bool bFace, int32 OnlySlot, int32 MaxSize, const FMantleAtlasSettings& Settings,
		TArray<FMantleBakeTarget>& OutTargets, TArray<FString>& OutNotes);
};

/** Registers the Bake fixer with FMantleAvatarFixers (MantleEditor module startup). */
MANTLEEDITOR_API void RegisterMantleAvatarBakeFixer();
