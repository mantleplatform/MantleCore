// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * The plugin's PNG writer: 8-bit RGBA, no interlace, filter 0 on every row, one zlib IDAT. Its
 * own because the same pixels must give the same bytes on every export: uploads are content-addressed, and identical bytes are what
 * lets an interrupted upload resume without sending a texture twice. Pure.
 */
struct MANTLEEDITOR_API FMantlePng
{
	/** Pixels are Width x Height FColors (BGRA in memory), row by row from the top. Empty on bad input. */
	static TArray<uint8> EncodeRGBA8(int32 Width, int32 Height, TConstArrayView<FColor> Pixels);

	/** A PNG's size and whether it can carry alpha (colour type 4 or 6, or a tRNS chunk). False when it is not a PNG. */
	static bool ReadHeader(TConstArrayView<uint8> Bytes, int32& OutWidth, int32& OutHeight, bool& OutHasAlpha);

	/** A Size x Size image of one colour, for a section with no texture (a flat colour) or a placeholder. */
	static TArray<uint8> EncodeSolid(int32 Width, int32 Height, FColor Color);
};
