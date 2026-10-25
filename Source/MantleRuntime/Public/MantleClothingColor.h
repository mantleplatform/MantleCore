// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "MantleClothingColor.generated.h"

/** A player's quantized colour for a region declared by the clothing creator. */
USTRUCT(BlueprintType)
struct MANTLERUNTIME_API FMantleClothingColor
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mantle|Avatar")
	FString RegionId;

	/** Opaque sRGB; FColor keeps the reflected replication payload quantized. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mantle|Avatar")
	FColor Color = FColor::White;

	bool operator==(const FMantleClothingColor& Other) const { return RegionId == Other.RegionId && Color == Other.Color; }
};

/** One RGB channel of an approved UV0 mask; metadata belongs to the item, not the player. */
struct MANTLERUNTIME_API FMantleClothingColorRegion
{
	FString Id;
	FString Label;
	uint8 Channel = 0;
	FColor DefaultColor = FColor::White;
	/** Mean linear luminance of the authored albedo in the masked region, measured during export. */
	float ReferenceLuminance = 0.0f;
	bool operator==(const FMantleClothingColorRegion& Other) const
	{ return Id == Other.Id && Label == Other.Label && Channel == Other.Channel && DefaultColor == Other.DefaultColor && ReferenceLuminance == Other.ReferenceLuminance; }
};

namespace MantleClothingColor
{
	inline constexpr int32 MaxRegionsPerSection = 3;
	inline constexpr int32 MaxColorsPerItem = 8;
	inline constexpr float MinReferenceLuminance = 1.0e-6f;
	inline constexpr const TCHAR* Mode = TEXT("masked_luminance_v1");

	MANTLERUNTIME_API bool IsRegionId(const FString& Id);
	MANTLERUNTIME_API bool ValidateRegions(TConstArrayView<FMantleClothingColorRegion> Regions, FString& OutError);
	/** Declared is the item's logical region IDs, combined across sections. */
	MANTLERUNTIME_API bool ValidateColors(TConstArrayView<FMantleClothingColor> Colors,
		TConstArrayView<FString> Declared, FString& OutError);

	/**
	 * CPU reference for the fixed dye shader. Unchanged without an override; unmasked pixels retain
	 * their authored colour. Only colour is remapped: normal and surface textures are independent.
	 * Overlapping active mask channels are normalized to keep the result independent of region order.
	 */
	MANTLERUNTIME_API FLinearColor Apply(const FLinearColor& Albedo, const FVector3f& Mask,
		TConstArrayView<FMantleClothingColorRegion> Regions, TConstArrayView<FMantleClothingColor> Colors);
}
