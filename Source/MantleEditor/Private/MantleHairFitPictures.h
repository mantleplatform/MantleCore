// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleHairFitReport.h"

/**
 * #51/#52: the before and after pictures of a hair head fit, drawn with the catalog thumbnail's capture (FMantleCosmeticThumbnail):
 * the head the hair was fitted to, grey, with the hair brown, in one mesh on the engine's vertex colour material. The Cosmetics
 * panel shows them next to the fit's words, and Mantle.Dev.RenderHairFit writes them to files.
 */
class FMantleHairFitPictures
{
public:
	/** The camera's yaw for the views: the front, a three-quarter view, the side and the back. */
	static constexpr float FrontYaw = 270.f;
	static constexpr float AngledYaw = 225.f;
	static constexpr float SideYaw = 180.f;
	static constexpr float BackYaw = 90.f;

	/** The frame of a picture: the head and the fitted hair, or (before) the head as the authored hair sees it and the authored hair. */
	static FBox FrameOf(const FMantleHairFitMeshes& Meshes, bool bAfter);

	/** Renders the hair as authored (bAfter false) or as fitted from Yaw into Pixels (Size x Size, BGRA). False, with Error, when nothing can render here. */
	static bool Render(const FMantleHairFitMeshes& Meshes, bool bAfter, float Yaw, int32 Size, TArray<FColor>& OutPixels, FString& OutError);
};
