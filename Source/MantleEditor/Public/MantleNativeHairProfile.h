// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleNativeHair.h"
#include "Engine/EngineTypes.h"

class UMaterialInterface;

/** Versioned native cards opacity profile; custom source curves require artist-authored alpha. */
struct MANTLEEDITOR_API FMantleNativeHairProfile
{
	static constexpr const TCHAR* Version = TEXT("metahuman_cards_v1");
	static bool ValidateParameters(bool bFacial, const FString& BaseMaterialName, EBlendMode BlendMode, float MaskClip,
		TFunctionRef<bool(FName, float&)> Scalar, FString& Description, FString& Error);
	/** Repaired fixed recipe: full parameter identities, source values and supported single-colour modes. */
	static bool FromMaterialRecipe(const UMaterialInterface* Material, FString& Profile, FMantleNativeHairSettings& Settings, FString& Description, FString& Error);

	/** Audited effective material family, independent of cosmetic slot or pose leader. */
	static bool FromMaterialFamily(const UMaterialInterface* Material, FString& Profile, FString& Description, FString& Error);
	static bool ValidateFamilySwitches(bool bTranslucent, TFunctionRef<bool(FName, bool&)> Switch, FString& Error);
	static bool ClassifyFamily(const FString& BasePath, EBlendMode BlendMode, FString& Profile, FString& Error);
	static bool ValidateShading(const FMaterialShadingModelField& Effective, const FMaterialShadingModelField& Base,
		ETranslucencyLightingMode SourceLighting, ETranslucencyLightingMode AuthoredLighting, FString& Error,
		const FMaterialShadingModelField* AuthoredModels = nullptr);
};
