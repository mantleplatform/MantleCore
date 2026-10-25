// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleCosmeticValidation.h"

/**
 * #736: the rules a scar or tattoo overlay's textures must meet before they upload, checked on pixels the creator
 * picked (a PNG file or a texture asset), in plain words. Pure: no editor state, so it is tested headless.
 *
 * An overlay is a mask (and, for the scar slots, a normal map) painted to the converted MetaHuman UV layout of the
 * body or the face. The mask is one channel: a greyscale image, or an image painted only in its red channel.
 */
struct MANTLEEDITOR_API FMantleOverlayChecks
{
	/** The overlay slots, in the order the panel lists them. */
	static TArray<FString> Slots();

	static bool IsOverlaySlot(const FString& Slot);

	/** scar_body and scar_face take a normal map; the tattoo slots do not. */
	static bool IsScarSlot(const FString& Slot);

	/** True for scar_face / tattoo_face: the overlay is drawn on the face, otherwise on the body. */
	static bool IsFaceSlot(const FString& Slot);

	/** The largest side of an overlay texture. */
	static constexpr int32 MaxSize = 1024;

	/** Textures are square, a power of two. */
	static bool IsPowerOfTwoSquare(int32 Width, int32 Height);

	/** Pixels in a PNG file as BGRA, raw bytes, no colour conversion. False with a plain reason. */
	static bool ReadPngFile(const FString& File, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError);

	/**
	 * The mask rules, on Width x Height pixels read as stored: square, power of two, at most MaxSize; one channel
	 * (greyscale, or red only); some paint, and not all white. Errors are added to OutIssues; What names the image
	 * ("The mask"). Returns true when none was added.
	 */
	static bool CheckMask(const FString& What, int32 Width, int32 Height, TConstArrayView<FColor> Pixels, TArray<FMantleCosmeticIssue>& OutIssues);

	/** The normal-map rules: the same size rules, then the pixels look like a tangent-space normal map. */
	static bool CheckNormal(const FString& What, int32 Width, int32 Height, TConstArrayView<FColor> Pixels, TArray<FMantleCosmeticIssue>& OutIssues);

	/** A normal map on a tattoo slot is refused. Returns true when it is fine. */
	static bool CheckNormalAllowed(const FString& Slot, bool bHasNormal, TArray<FMantleCosmeticIssue>& OutIssues);

	/** The mask as one canonical RGBA image: the mask value (red channel) in red, green and blue, opaque. */
	static TArray<FColor> CanonicalMask(TConstArrayView<FColor> Pixels);
};
