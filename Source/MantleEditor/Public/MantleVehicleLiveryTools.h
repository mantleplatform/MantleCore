// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
class UTexture2D;
struct FMantleSkinnedMesh;

/**
 * The Liveries step's helpers (SMantleVehicleBuilder): the stand-in material that shows a livery on the model in the window's picture,
 * and the UV guide an artist paints a livery on.
 */
struct MANTLEEDITOR_API FMantleVehicleLiveryTools
{
	/**
	 * A material for the picture that draws a painted section the way the game does: Paint where the livery's alpha is clear, the
	 * livery's own color where it is not, on UV 0. A null Livery draws the plain paint. Transient; the caller keeps it alive.
	 */
	static UMaterialInterface* MakePreviewMaterial(UTexture2D* Livery, const FLinearColor& Paint);

	/**
	 * The UV guide: a square PNG of the body's UV 0 layout. Every triangle's edges are drawn dim; the triangles of the sections in
	 * PaintedMaterials (the body's material slots that wear the paint, which is where a livery shows) are filled and outlined in a
	 * color each, so an artist sees where the paint is. Size is the picture's side in pixels.
	 */
	static bool WriteUvGuide(const FMantleSkinnedMesh& Body, const TSet<int32>& PaintedMaterials, int32 Size, const FString& File, FString& OutError);

	/** The guide's pixels (BGRA, Size by Size) without writing them; what WriteUvGuide saves. Empty when the body has no UVs. */
	static TArray<FColor> DrawUvGuide(const FMantleSkinnedMesh& Body, const TSet<int32>& PaintedMaterials, int32 Size);

	/** A livery's name for a menu: its own, or "Livery <number>" (counting from 1) when it has none. */
	static FString DisplayName(const FString& Name, int32 Index);

	/** A name to start a livery with from its image's file or asset name: underscores and dashes to spaces, cut to the longest a name may be. */
	static FString NameFromImage(const FString& ImageName);
};
