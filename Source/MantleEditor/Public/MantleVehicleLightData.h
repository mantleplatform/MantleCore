// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"
#include "MantleVehicleLightData.generated.h"

/** Marks a vehicle light's glowing material copy (FMantleVehicleLights::MakeLightMaterial) with the light it glows as. Editor only: the game never loads it. */
UCLASS()
class MANTLEEDITOR_API UMantleVehicleLightData : public UAssetUserData
{
	GENERATED_BODY()

public:
	/** The document's key of the light: head, rear, reverse, left or right. */
	UPROPERTY(VisibleAnywhere, Category = "Mantle")
	FString Kind;

	/** A light section that keeps its own material (Lights > Own Material): it publishes with the model's own glow color, not the kind's one color. */
	UPROPERTY(VisibleAnywhere, Category = "Mantle")
	bool bOwnLook = false;
};
