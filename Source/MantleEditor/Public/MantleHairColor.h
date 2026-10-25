// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class UMaterialInterface;

/**
 * The colour a groom's cards are drawn in (#325, #326): a groom cards upload sends its attribute atlas
 * (coverage, not colour) as a hair_cards section, with hairColor, linear RGB, which the runtime hair
 * material multiplies in. MetaHuman grooms keep their colour in the material (Melanin, Redness,
 * DyeColor), not a texture, so the plugin reads it from there; the creator may override it in the
 * Cosmetics panel (FMantleCosmeticDraft::HairColor).
 */
struct MANTLEEDITOR_API FMantleHairColor
{
	/** Where a colour came from, for the package notes. */
	enum class ESource : uint8
	{
		/** The creator's override. */
		Override,
		/** A material's Melanin (with Redness and DyeColor when present), through the engine's GetHairColorFromMelanin. */
		Melanin,
		/** A material's colour vector parameter (HairColor, BaseColor, Color, Tint). */
		Vector,
		/** Nothing to read: DefaultColor. */
		Default,
	};

	/**
	 * The engine's GetHairColorFromMelanin (HairShadingCommon.ush, UE 5.8), ported: melanin and redness in
	 * [0, 1] plus a dye colour (white: none) to linear RGB reflectance. Pure.
	 */
	static FLinearColor FromMelanin(float Melanin, float Redness, const FLinearColor& DyeColor);

	/**
	 * A colour from a material's parameters, as looked up by name (FName, so without regard to case). Melanin
	 * wins over a colour vector: MetaHuman hair keeps its colour there, and a vector named Color may be a
	 * tint over it. Unset when neither is present. Pure: the lookups are callbacks.
	 */
	static TOptional<FLinearColor> FromParameters(TFunctionRef<bool(FName Name, float& OutValue)> Scalar,
		TFunctionRef<bool(FName Name, FLinearColor& OutValue)> Vector, ESource* OutSource = nullptr);

	/** FromParameters on a material's parameter values. */
	static TOptional<FLinearColor> FromMaterial(const UMaterialInterface* Material, ESource* OutSource = nullptr);

	/**
	 * The colour a groom's cards upload with: the creator's override when set, else the first of Materials that
	 * names one (FromMaterial), else DefaultColor. OutMaterial gets the material read, when one was.
	 */
	static FLinearColor Resolve(const TOptional<FLinearColor>& Override, TConstArrayView<const UMaterialInterface*> Materials,
		ESource& OutSource, const UMaterialInterface** OutMaterial = nullptr);

	/** "the creator's colour", "Melanin", ... for the package notes. */
	static const TCHAR* Describe(ESource Source);

	/** A mid brown (melanin 0.5, redness 0.3): what a groom whose material says nothing is drawn in. */
	static FLinearColor DefaultColor();

	/** "#RRGGBB" or "RRGGBB" (sRGB, as colour pickers show it) to linear RGB; false on anything else. */
	static bool ParseHex(const FString& Text, FLinearColor& OutLinear);

	/** Linear RGB as "#RRGGBB" (sRGB), for the panel. */
	static FString ToHex(const FLinearColor& Linear);

	/** The scalar and vector parameter names read, in order (tests and the panel's help). */
	static const TArray<FName>& MelaninNames();
	static const TArray<FName>& RednessNames();
	static const TArray<FName>& DyeNames();
	static const TArray<FName>& ColorNames();
};
