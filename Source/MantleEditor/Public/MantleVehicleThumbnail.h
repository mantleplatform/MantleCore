// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleThumbnailChoice.h"

class UMantleVehicleSource;
class USkeletalMesh;
struct FMantleSkinnedMesh;

/**
 * A built vehicle's library card picture (#821): vehicles are library items (#614), and a library card shows a thumbnail, a 512 x 512 PNG texture the item names as
 * thumbnailAssetId. Nothing made one for a vehicle before. The builder takes it when it builds the vehicle, stores it on the vehicle asset
 * (UMantleVehicleSource::ThumbnailPng) and publishing uploads it with the vehicle and names it on the item.
 *
 * It is the vehicle as a player first sees it, from the front and a little above: the editor's own thumbnail scene through a scene capture when this process can
 * render (the model's own materials and paint), else drawn on the CPU from the body's triangles (flat shaded, one colour), so a headless build, the tests and a
 * build on a machine without a renderer still get a picture and never a blank card.
 */
struct MANTLEEDITOR_API FMantleVehicleThumbnail
{
	/** The picture's width and height: the library thumbnail's class (limits.json avatar content thumbnail, 512 x 512 PNG). */
	static constexpr int32 Size = 512;

	/** The direction the camera looks along: the vehicle's front and left side seen from a little above, its nose along +X in the vehicle's space. */
	static FRotator View();

	/**
	 * Draws Body's LOD 0 on the CPU: opaque Size x Size pixels, top row first, the triangles lit from above and the side, on a dark card, framed with some room round it.
	 * Orthographic and 2x2 supersampled. False when the body has no triangles.
	 */
	static bool Draw(const FMantleSkinnedMesh& Body, TArray<FColor>& OutPixels);

	/**
	 * The picture of a built vehicle as PNG bytes: Mesh (its skeletal mesh, built) rendered in the thumbnail scene when this process can, else Body drawn by Draw.
	 * OutHow says which, in a few words ("rendered", "drawn without a renderer: ..."). False, with the reason in OutError, when neither made a picture.
	 */
	static bool Make(USkeletalMesh* Mesh, const FMantleSkinnedMesh& Body, TArray<uint8>& OutPng, FString& OutHow, FString& OutError);

	/** Make, stored in Source.ThumbnailPng (one undo step with the build). False, with the reason in OutHow, when it could not be made. */
	static bool Take(UMantleVehicleSource& Source, USkeletalMesh* Mesh, const FMantleSkinnedMesh& Body, FString& OutHow);

	/** The words shown in place of the picture on Automatic until the vehicle has been built. */
	static FString TakenWhenBuiltNote();

	/**
	 * The Thumbnail section's render for the vehicle builder. On Automatic it is the picture the upload sends: the built vehicle's saved picture
	 * (Source.ThumbnailPng, as publishing uploads it), or, before a build, TakenWhenBuiltNote. Choose the angle renders the picked model (GetModel).
	 * GetBuilt returns the built vehicle, or null.
	 */
	static FMantleThumbnailResolver::FRender PreviewFor(TFunction<const UMantleVehicleSource*()> GetBuilt, TFunction<UObject*()> GetModel);
};
