// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleThumbnailChoice.h"

struct FMantleThumbnailSubject;

/**
 * The automatic picture of a 3D model on its own (#124): a vehicle's, a weapon's or a consumable's. The model (a static or skeletal mesh) in the engine's
 * thumbnail scene, from the front and a little above, framed on its bounds, through the same capture the catalog's thumbnails use. Choose the angle
 * turns, tilts and zooms the same picture.
 */
struct MANTLEEDITOR_API FMantleModelThumbnail
{
	/** The default camera direction: the model's front and left side from a little above (the vehicle card's view). */
	static FRotator DefaultView();

	/** Model alone, framed on its bounds, looking along DefaultView; empty for a null or unsupported model. */
	static FMantleThumbnailSubject Subject(UObject* Model);

	/**
	 * Applies the creator's angle to Subject: with Choose the angle, the camera looks along the choice's pitch and yaw and the framed box closes in
	 * by the zoom; any other mode leaves Subject as it is. Pure.
	 */
	static void ApplyChoice(FMantleThumbnailSubject& Subject, const FMantleThumbnailChoice& Choice);

	/** Renders Model for Choice into Size x Size pixels. False, with the reason, when there is no model or this process cannot render. */
	static bool Render(UObject* Model, const FMantleThumbnailChoice& Choice, int32 Size, TArray<FColor>& OutPixels, FString& OutError);

	/** The shared resolver's render for whichever model GetModel returns when it is called. */
	static FMantleThumbnailResolver::FRender RenderFor(TFunction<UObject*()> GetModel);
};
