// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleClothingColor.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;

/** The shipped, fixed clothing dye graph. Source textures and geometry remain shared. */
class MANTLERUNTIME_API FMantleClothingMaterial
{
public:
	static UMaterialInterface* Parent();
	/** Copies fixed source parameters once; its per-wearer values never mutate Shared. */
	static UMaterialInstanceDynamic* MakeLocal(UMaterialInterface& Shared, UObject& Outer,
		TConstArrayView<FMantleClothingColorRegion> Regions, TConstArrayView<FMantleClothingColor> Colors);
	/** Clears all previous overrides before assigning declared RGB channel controls. */
	static void Apply(UMaterialInstanceDynamic& Instance, TConstArrayView<FMantleClothingColorRegion> Regions,
		TConstArrayView<FMantleClothingColor> Colors);
};
