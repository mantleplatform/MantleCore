// Copyright Joshua Eger. All Rights Reserved.
#include "MantleEnvironmentOverride.h"

#include "Misc/Parse.h"

namespace
{
	/** Value if finite, else Default. Named for the override: unity builds merge this file with others. */
	float OverrideFiniteOr(float Value, float Default)
	{
		return FMath::IsFinite(Value) ? Value : Default;
	}

	float OverrideClamp(float Value, float Default, double Min, double Max)
	{
		return FMath::Clamp(OverrideFiniteOr(Value, Default), static_cast<float>(Min), static_cast<float>(Max));
	}

	constexpr uint8 OverrideLastWeatherState = static_cast<uint8>(MantleLastWeatherState);

	/** Environment.ColorGrading's presets, indexed by their number less one (ColorGradingWarm first). */
	const TCHAR* const OverrideColorGradingNames[] = {
		TEXT("Warm"), TEXT("Cool"), TEXT("Noir"), TEXT("Sepia"), TEXT("Vivid"), TEXT("Faded"),
	};
	static_assert(UE_ARRAY_COUNT(OverrideColorGradingNames) == static_cast<SIZE_T>(MantleEnvironmentOverrideValues::ColorGradingLast),
		"one name per colour-grading preset");
}

bool FMantleEnvironmentOverride::operator==(const FMantleEnvironmentOverride& Other) const
{
	return bClockSet == Other.bClockSet
		&& ClockAnchorServerSeconds == Other.ClockAnchorServerSeconds
		&& ClockAnchorTotalHours == Other.ClockAnchorTotalHours
		&& TimeScale == Other.TimeScale
		&& ClockRevision == Other.ClockRevision
		&& WeatherMode == Other.WeatherMode
		&& ForcedWeather == Other.ForcedWeather
		&& WeatherFrom == Other.WeatherFrom
		&& WeatherRevision == Other.WeatherRevision
		&& WeatherChangeServerSeconds == Other.WeatherChangeServerSeconds
		&& WeatherBlendSeconds == Other.WeatherBlendSeconds
		&& FromCloudCover == Other.FromCloudCover
		&& FromFogDensityScale == Other.FromFogDensityScale
		&& FromRainIntensity == Other.FromRainIntensity
		&& FromLightningPerMinute == Other.FromLightningPerMinute
		&& FromSnowIntensity == Other.FromSnowIntensity
		&& FromDustIntensity == Other.FromDustIntensity
		&& FromWindIntensity == Other.FromWindIntensity
		&& FromFogAmount == Other.FromFogAmount
		&& CloudCover == Other.CloudCover
		&& FogDensity == Other.FogDensity
		&& bFogColorSet == Other.bFogColorSet
		&& FogColor == Other.FogColor
		&& Stars == Other.Stars
		&& SunIntensityScale == Other.SunIntensityScale
		&& ExposureBias == Other.ExposureBias
		&& Saturation == Other.Saturation
		&& Contrast == Other.Contrast
		&& Vignette == Other.Vignette
		&& ColorTint == Other.ColorTint
		&& Bloom == Other.Bloom
		&& ColorGrading == Other.ColorGrading
		&& ColorGradingIntensity == Other.ColorGradingIntensity;
}

void FMantleEnvironmentOverride::Sanitize()
{
	using namespace MantleEnvironmentDefaults;
	using namespace MantleEnvironmentOverrideValues;

	if (!FMath::IsFinite(ClockAnchorServerSeconds) || !FMath::IsFinite(ClockAnchorTotalHours))
	{
		bClockSet = false;
		ClockAnchorServerSeconds = 0.0;
		ClockAnchorTotalHours = 0.0;
	}
	TimeScale = OverrideClamp(TimeScale, 1.0f, 0.0, MaxScriptTimeScale);

	WeatherMode = WeatherMode == WeatherForced ? WeatherForced : WeatherNatural;
	ForcedWeather = FMath::Min(ForcedWeather, OverrideLastWeatherState);
	WeatherFrom = FMath::Min(WeatherFrom, OverrideLastWeatherState);
	if (!FMath::IsFinite(WeatherChangeServerSeconds))
	{
		WeatherChangeServerSeconds = 0.0;
	}
	WeatherBlendSeconds = OverrideClamp(WeatherBlendSeconds, 0.0f, 0.0, MaxScriptWeatherBlendSeconds);
	FromCloudCover = OverrideClamp(FromCloudCover, 0.0f, 0.0, 1.0);
	FromFogDensityScale = OverrideClamp(FromFogDensityScale, 1.0f, 0.0, MaxFogDensityScale);
	FromRainIntensity = OverrideClamp(FromRainIntensity, 0.0f, 0.0, 1.0);
	FromLightningPerMinute = OverrideClamp(FromLightningPerMinute, 0.0f, 0.0, MaxLightningPerMinute);
	FromSnowIntensity = OverrideClamp(FromSnowIntensity, 0.0f, 0.0, 1.0);
	FromDustIntensity = OverrideClamp(FromDustIntensity, 0.0f, 0.0, 1.0);
	FromWindIntensity = OverrideClamp(FromWindIntensity, 0.0f, 0.0, 1.0);
	FromFogAmount = OverrideClamp(FromFogAmount, 0.0f, 0.0, 1.0);

	// Below zero (or not a number) is "the weather's own": one value for it, so equality holds.
	CloudCover = OverrideFiniteOr(CloudCover, -1.0f) < 0.0f ? -1.0f : FMath::Min(CloudCover, 1.0f);

	FogDensity = OverrideClamp(FogDensity, 1.0f, 0.0, MaxScriptFogDensity);
	FogColor = bFogColorSet ? (FogColor & White) : White;
	Stars = Stars <= StarsHidden ? Stars : StarsNatural;
	SunIntensityScale = OverrideClamp(SunIntensityScale, 1.0f, 0.0, MaxScriptSunIntensityScale);

	ExposureBias = OverrideClamp(ExposureBias, 0.0f, -MaxScriptExposureBias, MaxScriptExposureBias);
	Saturation = OverrideClamp(Saturation, 1.0f, 0.0, MaxScriptSaturation);
	Contrast = OverrideClamp(Contrast, 1.0f, 0.0, MaxScriptContrast);
	Vignette = OverrideFiniteOr(Vignette, -1.0f) < 0.0f ? -1.0f : FMath::Min(Vignette, 1.0f);
	ColorTint &= White;

	// Below zero (or not a number) is "the engine's own bloom", as for the vignette.
	Bloom = OverrideFiniteOr(Bloom, -1.0f) < 0.0f ? -1.0f : FMath::Min(Bloom, static_cast<float>(MaxScriptBloom));
	ColorGrading = ColorGrading <= ColorGradingLast ? ColorGrading : ColorGradingNone;
	ColorGradingIntensity = OverrideClamp(ColorGradingIntensity, 1.0f, 0.0, 1.0);
}

bool FMantleEnvironmentOverride::IsPostProcessNeutral() const
{
	// ColorGradingIntensity on its own grades nothing: it only says how much of a preset shows.
	return ExposureBias == 0.0f
		&& Saturation == 1.0f
		&& Contrast == 1.0f
		&& Vignette < 0.0f
		&& ColorTint == MantleEnvironmentOverrideValues::White
		&& Bloom < 0.0f
		&& ColorGrading == MantleEnvironmentOverrideValues::ColorGradingNone;
}

EMantleWeatherState FMantleEnvironmentOverride::GetForcedWeather() const
{
	return static_cast<EMantleWeatherState>(FMath::Min(ForcedWeather, OverrideLastWeatherState));
}

EMantleWeatherState FMantleEnvironmentOverride::GetWeatherFrom() const
{
	return static_cast<EMantleWeatherState>(FMath::Min(WeatherFrom, OverrideLastWeatherState));
}

FMantleWeatherValues FMantleEnvironmentOverride::GetFromValues() const
{
	FMantleWeatherValues Values;
	Values.CloudCover = FromCloudCover;
	Values.FogDensityScale = FromFogDensityScale;
	Values.RainIntensity = FromRainIntensity;
	Values.LightningPerMinute = FromLightningPerMinute;
	Values.SnowIntensity = FromSnowIntensity;
	Values.DustIntensity = FromDustIntensity;
	Values.WindIntensity = FromWindIntensity;
	Values.FogAmount = FromFogAmount;
	return Values;
}

void FMantleEnvironmentOverride::SetFromValues(const FMantleWeatherValues& Values)
{
	FromCloudCover = static_cast<float>(Values.CloudCover);
	FromFogDensityScale = static_cast<float>(Values.FogDensityScale);
	FromRainIntensity = static_cast<float>(Values.RainIntensity);
	FromLightningPerMinute = static_cast<float>(Values.LightningPerMinute);
	FromSnowIntensity = static_cast<float>(Values.SnowIntensity);
	FromDustIntensity = static_cast<float>(Values.DustIntensity);
	FromWindIntensity = static_cast<float>(Values.WindIntensity);
	FromFogAmount = static_cast<float>(Values.FogAmount);
}

void FMantleEnvironmentOverride::DiffPropertyNames(const FMantleEnvironmentOverride& Before, const FMantleEnvironmentOverride& After, TArray<FString>& Out)
{
	auto Note = [&Out](bool bChanged, const TCHAR* Name)
	{
		if (bChanged)
		{
			Out.Add(FString(Name));
		}
	};
	Note(Before.ClockRevision != After.ClockRevision, TEXT("ClockTime"));
	Note(Before.TimeScale != After.TimeScale, TEXT("TimeScale"));
	Note(Before.WeatherRevision != After.WeatherRevision, TEXT("Weather"));
	Note(Before.CloudCover != After.CloudCover, TEXT("CloudCover"));
	Note(Before.FogDensity != After.FogDensity, TEXT("FogDensity"));
	Note(Before.bFogColorSet != After.bFogColorSet || Before.FogColor != After.FogColor, TEXT("FogColor"));
	Note(Before.Stars != After.Stars, TEXT("StarsVisible"));
	Note(Before.SunIntensityScale != After.SunIntensityScale, TEXT("SunIntensityScale"));
	Note(Before.ExposureBias != After.ExposureBias, TEXT("ExposureBias"));
	Note(Before.Saturation != After.Saturation, TEXT("Saturation"));
	Note(Before.Contrast != After.Contrast, TEXT("Contrast"));
	Note(Before.Vignette != After.Vignette, TEXT("Vignette"));
	Note(Before.ColorTint != After.ColorTint, TEXT("ColorTint"));
	Note(Before.Bloom != After.Bloom, TEXT("Bloom"));
	Note(Before.ColorGrading != After.ColorGrading, TEXT("ColorGrading"));
	Note(Before.ColorGradingIntensity != After.ColorGradingIntensity, TEXT("ColorGradingIntensity"));
}

const TCHAR* FMantleEnvironmentOverride::ColorGradingName(uint8 Preset)
{
	if (Preset == MantleEnvironmentOverrideValues::ColorGradingNone || Preset > MantleEnvironmentOverrideValues::ColorGradingLast)
	{
		return nullptr;
	}
	return OverrideColorGradingNames[Preset - 1];
}

bool FMantleEnvironmentOverride::ParseColorGradingName(const FString& Name, uint8& OutPreset)
{
	// Case-exact, as Environment.ColorGrading reads it back (FString == would take "noir").
	for (uint8 Preset = 1; Preset <= MantleEnvironmentOverrideValues::ColorGradingLast; ++Preset)
	{
		if (Name.Equals(OverrideColorGradingNames[Preset - 1], ESearchCase::CaseSensitive))
		{
			OutPreset = Preset;
			return true;
		}
	}
	return false;
}

FString FMantleEnvironmentOverride::ColorGradingNameList()
{
	FString List;
	const int32 Count = static_cast<int32>(UE_ARRAY_COUNT(OverrideColorGradingNames));
	for (int32 Index = 0; Index < Count; ++Index)
	{
		if (Index > 0)
		{
			List += (Index == Count - 1) ? TEXT(" or ") : TEXT(", ");
		}
		List += OverrideColorGradingNames[Index];
	}
	return List;
}

FSoftObjectPath FMantleEnvironmentOverride::ColorGradingLutPath(uint8 Preset)
{
	const TCHAR* Name = ColorGradingName(Preset);
	if (!Name)
	{
		return FSoftObjectPath();
	}
	const FString AssetName = FString(MantleEnvironmentDefaults::ColorGradingLutPrefix) + Name;
	return FSoftObjectPath(FString::Printf(TEXT("%s/%s.%s"), MantleEnvironmentDefaults::ColorGradingLutFolder, *AssetName, *AssetName));
}

FLinearColor FMantleEnvironmentOverride::LinearFromRgb(uint32 Rgb)
{
	return FLinearColor::FromSRGBColor(FColor(
		static_cast<uint8>((Rgb >> 16) & 0xFF), static_cast<uint8>((Rgb >> 8) & 0xFF), static_cast<uint8>(Rgb & 0xFF), 255));
}

FString FMantleEnvironmentOverride::RgbToHex(uint32 Rgb)
{
	return FString::Printf(TEXT("#%06X"), Rgb & MantleEnvironmentOverrideValues::White);
}

bool FMantleEnvironmentOverride::ParseHexRgb(const FString& Text, uint32& OutRgb)
{
	// The same rule as Vehicles:SetPaintColor and Backpack:SetWeaponColor, so a creator writes
	// one colour format everywhere.
	FString Value = Text.TrimStartAndEnd();
	Value.RemoveFromStart(TEXT("#"));
	if (Value.Len() != 6)
	{
		return false;
	}
	uint32 Rgb = 0;
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsHexDigit(Character))
		{
			return false;
		}
		Rgb = (Rgb << 4) | static_cast<uint32>(FParse::HexDigit(Character));
	}
	OutRgb = Rgb;
	return true;
}

namespace
{
	/** Both unset, or both set to the same value. Named for the local look: unity builds merge files. */
	template <typename T>
	bool LocalLookFieldSame(const TOptional<T>& A, const TOptional<T>& B)
	{
		return A.IsSet() == B.IsSet() && (!A.IsSet() || A.GetValue() == B.GetValue());
	}

	/** A set number clamped into Min..Max; a non-finite one is unset. */
	void LocalLookClamp(TOptional<float>& Field, double Min, double Max)
	{
		if (!Field.IsSet())
		{
			return;
		}
		if (!FMath::IsFinite(Field.GetValue()))
		{
			Field.Reset();
			return;
		}
		Field.Emplace(FMath::Clamp(Field.GetValue(), static_cast<float>(Min), static_cast<float>(Max)));
	}
}

bool FMantleEnvironmentLocalLook::operator==(const FMantleEnvironmentLocalLook& Other) const
{
	return LocalLookFieldSame(ExposureBias, Other.ExposureBias)
		&& LocalLookFieldSame(Saturation, Other.Saturation)
		&& LocalLookFieldSame(Contrast, Other.Contrast)
		&& LocalLookFieldSame(Vignette, Other.Vignette)
		&& LocalLookFieldSame(ColorTint, Other.ColorTint)
		&& LocalLookFieldSame(Bloom, Other.Bloom)
		&& LocalLookFieldSame(ColorGrading, Other.ColorGrading)
		&& LocalLookFieldSame(ColorGradingIntensity, Other.ColorGradingIntensity);
}

bool FMantleEnvironmentLocalLook::IsEmpty() const
{
	return !ExposureBias.IsSet() && !Saturation.IsSet() && !Contrast.IsSet() && !Vignette.IsSet()
		&& !ColorTint.IsSet() && !Bloom.IsSet() && !ColorGrading.IsSet() && !ColorGradingIntensity.IsSet();
}

void FMantleEnvironmentLocalLook::Sanitize()
{
	using namespace MantleEnvironmentDefaults;
	// The same ranges FMantleEnvironmentOverride::Sanitize and the server members clamp to.
	LocalLookClamp(ExposureBias, -MaxScriptExposureBias, MaxScriptExposureBias);
	LocalLookClamp(Saturation, 0.0, MaxScriptSaturation);
	LocalLookClamp(Contrast, 0.0, MaxScriptContrast);
	LocalLookClamp(Vignette, 0.0, 1.0);
	LocalLookClamp(Bloom, 0.0, MaxScriptBloom);
	LocalLookClamp(ColorGradingIntensity, 0.0, 1.0);
	if (ColorTint.IsSet())
	{
		ColorTint.Emplace(ColorTint.GetValue() & MantleEnvironmentOverrideValues::White);
	}
	if (ColorGrading.IsSet() && ColorGrading.GetValue() > MantleEnvironmentOverrideValues::ColorGradingLast)
	{
		ColorGrading.Reset();
	}
}

void FMantleEnvironmentLocalLook::ApplyTo(FMantleEnvironmentOverride& InOut) const
{
	if (ExposureBias.IsSet()) { InOut.ExposureBias = ExposureBias.GetValue(); }
	if (Saturation.IsSet()) { InOut.Saturation = Saturation.GetValue(); }
	if (Contrast.IsSet()) { InOut.Contrast = Contrast.GetValue(); }
	if (Vignette.IsSet()) { InOut.Vignette = Vignette.GetValue(); }
	if (ColorTint.IsSet()) { InOut.ColorTint = ColorTint.GetValue(); }
	if (Bloom.IsSet()) { InOut.Bloom = Bloom.GetValue(); }
	if (ColorGrading.IsSet()) { InOut.ColorGrading = ColorGrading.GetValue(); }
	if (ColorGradingIntensity.IsSet()) { InOut.ColorGradingIntensity = ColorGradingIntensity.GetValue(); }
}
