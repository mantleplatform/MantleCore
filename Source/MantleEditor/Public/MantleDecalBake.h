// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class IMantleMaterialFlattener;
class UMaterialInterface;
class UTexture2D;

/**
 * A1: a Decal Actor's textures, baked by the engine. A published decal is a texture whose alpha is its shape. ReadDecal takes the texture
 * feeding the material's Base Color and runs its Opacity graph into the alpha (FMantlePlaceExporter::DecalTextureWithOpacity), but that
 * reader runs only a few node kinds; a pack whose Opacity graph holds others (a Power, cell bombing: City Sample's decal master) was
 * refused. Such a decal is baked here instead: its surface copy (FMantlePlaceAssets::DecalSurfaceCopy) through the same bake the
 * converter uses, over UVs 0 to 1 (what a decal projects across its box), giving its Base Color with its Opacity in alpha, and its Normal
 * as the material draws it (City Sample's master runs its normal through its UV controls and a strength, which the raw texture misses).
 *
 * Needs a renderer: without one (or when the bake gives one flat color) it gives nothing, and ReadDecal refuses as before. Each result is
 * kept for the whole editor session under its material's content (FMantlePlaceAssetConverter::ComputeCacheKey), as strong pointers, so
 * Check, gathering and export bake each decal material once, and an edited material is baked again. Every bake's time is logged with the
 * session's running total (MANTLE_DECAL_BAKE).
 */
class MANTLEEDITOR_API FMantleDecalBake
{
public:
	/**
	 * Material's decal as transient textures (source pixels): OutBaseColor (sRGB, opacity in alpha) and OutNormal (green-up, marked Flip
	 * Green Channel; null when the material's normal is flat). False with the reason, a clause, when it can't.
	 */
	static bool Bake(const UMaterialInterface* Material, UTexture2D*& OutBaseColor, UTexture2D*& OutNormal, FString& OutWhy);

	/** Tests: the flattener the bake uses instead of GLTFExporter's (null restores it). Clears the kept results. */
	static void SetFlattenerForTests(TSharedPtr<IMantleMaterialFlattener> Flattener);

	/** Forgets every kept result. */
	static void ClearCache();
};
