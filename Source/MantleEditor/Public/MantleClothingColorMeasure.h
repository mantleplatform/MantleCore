// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleClothingColor.h"
#include "MantleGlbWriter.h"

/** Measures the fixed dye reference from actual exported color/data pixels used by a UV0 material. */
class MANTLEEDITOR_API FMantleClothingColorMeasure
{
public:
	static constexpr int64 MaxRasterTexelVisits = 64ll * 1024 * 1024;
	/**
	 * One byte per texel of a Size image: 1 where a triangle of MaterialIndex covers the texel centre on UV0 (a sub-texel
	 * triangle marks its centroid texel). The area Measure averages over and #756 detection scores over. UVs outside the
	 * unit tile wrap, as the runtime's textures do.
	 */
	static bool MarkUsedTexels(const FMantleGlbMesh& Mesh, int32 MaterialIndex, FIntPoint Size, TArray<uint8>& OutUsed, FString& OutError);
	/** Pixels share Size; albedo is sRGB and mask is linear RGB. Writes measured references only on success. */
	static bool Measure(const FMantleGlbMesh& Mesh, int32 MaterialIndex, FIntPoint Size,
		TConstArrayView<FColor> Albedo, TConstArrayView<FColor> Mask,
		TArray<FMantleClothingColorRegion>& Regions, FString& OutError);
};
