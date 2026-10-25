// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"

/** Internal coverage computation shared by preparation and its focused tests. */
namespace MantlePlaceAssetsPrivate
{
	TSet<FString> UploadedIdsPlaceSetTakes(
		const FMantlePlace& Place,
		TFunctionRef<void(const FString&, TSet<FString>&)> AddWeaponDependencies);
}
