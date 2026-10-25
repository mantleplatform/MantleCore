// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/** Fixed version-one source colour inputs. Native per-pixel direction is a separate linear RGB data texture. */
struct MANTLERUNTIME_API FMantleNativeHairSettings
{
    int32 Version = 1;
    float Melanin = 0.8f;
    float Redness = 0.25f;
    FLinearColor DyeColor = FLinearColor::White;

    bool IsValid() const
    {
        const auto Unit = [](float Value) { return FMath::IsFinite(Value) && Value >= 0.f && Value <= 1.f; };
        return Version == 1 && Unit(Melanin) && Unit(Redness) && Unit(DyeColor.R) && Unit(DyeColor.G) && Unit(DyeColor.B);
    }
    bool operator==(const FMantleNativeHairSettings& Other) const
    { return Version == Other.Version && Melanin == Other.Melanin && Redness == Other.Redness && DyeColor == Other.DyeColor; }
    bool operator!=(const FMantleNativeHairSettings& Other) const { return !(*this == Other); }
    uint32 GetIdentityHash() const
    {
        uint32 Hash = HashCombine(GetTypeHash(Version), GetTypeHash(Melanin));
        Hash = HashCombine(Hash, GetTypeHash(Redness));
        for (float Channel : {DyeColor.R, DyeColor.G, DyeColor.B}) Hash = HashCombine(Hash, GetTypeHash(Channel));
        return Hash;
    }
};
