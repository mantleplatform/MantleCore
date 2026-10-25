// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleCosmeticPackage.h"
#include "Engine/SkinnedAssetCommon.h"

/** Preview the same exported pixel data and fixed dye graph the published item uses. */
bool MakeClothingColorPreviewMaterials(const FMantleCosmeticPackage& Package,
	TConstArrayView<FString> ExportMaterialNames, TConstArrayView<FMantleClothingColor> Colors,
	TArray<FSkeletalMaterial>& Materials, FString& OutError);
