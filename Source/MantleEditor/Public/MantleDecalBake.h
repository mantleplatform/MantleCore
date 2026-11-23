// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class IMantleMaterialFlattener;
class UMaterialInterface;
class UTexture2D;

/**
 * A1: a Decal Actor's shape, baked by the engine. A published decal is a texture whose alpha is its shape. ReadDecal takes the texture
 * feeding the material's Base Color and runs its Opacity graph into the alpha (FMantlePlaceExporter::DecalTextureWithOpacity), but that
 * reader runs only a few node kinds; a pack whose Opacity graph holds others (a Power, cell bombing: City Sample's decal master) was
 * refused. Such a decal is baked here instead: its surface copy (FMantlePlaceAssets::DecalSurfaceCopy) through the same bake the
 * converter uses, over UVs 0 to 1 (what a decal projects across its box), giving its Base Color with its Opacity in alpha.
 *
 * Needs a renderer: without one (or when the bake gives one flat color) it gives nothing, and ReadDecal refuses as before. Each result is
 * kept for the editor session under its material's content (FMantlePlaceAssetConverter::ComputeCacheKey), so Check, gathering and
 * export bake each decal material once, and an edited material is baked again.
 */
class MANTLEEDITOR_API FMantleDecalBake
{
public:
	/** Material's decal shape as a transient texture (BGRA8 source pixels, sRGB); null with the reason, a clause, when it can't. */
	static UTexture2D* BakeShape(const UMaterialInterface* Material, FString& OutWhy);

	/** Tests: the flattener the bake uses instead of GLTFExporter's (null restores it). Clears the kept results. */
	static void SetFlattenerForTests(TSharedPtr<IMantleMaterialFlattener> Flattener);

	/** Forgets every kept result. */
	static void ClearCache();
};
