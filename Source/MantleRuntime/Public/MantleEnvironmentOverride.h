// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleWeather.h"
#include "UObject/SoftObjectPath.h"

#include "MantleEnvironmentOverride.generated.h"

/** FMantleEnvironmentOverride's enum-like fields. Plain constants: UHT reads no enum here. */
namespace MantleEnvironmentOverrideValues
{
	/** WeatherMode: the seeded sequence (after any blend back to it), or one state a script forced. */
	constexpr uint8 WeatherNatural = 0;
	constexpr uint8 WeatherForced = 1;

	/** Stars: the night sky's own fade, or a script's always-shown or never-shown. */
	constexpr uint8 StarsNatural = 0;
	constexpr uint8 StarsShown = 1;
	constexpr uint8 StarsHidden = 2;

	/** The colours are 0xRRGGBB, sRGB, as a script writes "#RRGGBB". */
	constexpr uint32 White = 0xFFFFFF;

	/**
	 * ColorGrading: none (the project's own grading), or one of the fixed presets (#532), in the
	 * order FMantleEnvironmentOverride::ColorGradingName lists them. The numbers replicate, so a
	 * new preset only ever goes at the end.
	 */
	constexpr uint8 ColorGradingNone = 0;
	constexpr uint8 ColorGradingWarm = 1;
	constexpr uint8 ColorGradingCool = 2;
	constexpr uint8 ColorGradingNoir = 3;
	constexpr uint8 ColorGradingSepia = 4;
	constexpr uint8 ColorGradingVivid = 5;
	constexpr uint8 ColorGradingFaded = 6;
	constexpr uint8 ColorGradingLast = ColorGradingFaded;
}

/**
 * What server scripts have changed about the running environment (#454, script API 1.11): the
 * clock's anchor and rate, a forced weather and the blend into (or back out of) it, a cloud cover,
 * the fog, the stars, the sun and a small post-process subset, which #532 (script API 1.27) extends
 * with bloom and a fixed list of colour-grading LUTs. The default value changes nothing.
 *
 * The server's script host writes the whole struct onto AMantleScriptReplicator, which Iris
 * replicates member-wise: whenever a script writes, and to a late joiner as its initial state.
 * Every machine hands its copy to UMantleEnvironmentSubsystem::SetOverride and derives the same
 * hour, weather, sky and post-process from it, the synced server time and the weather seed; like
 * the seed, nothing is sent per frame. Times are server seconds (the script host's ServerNow), so
 * the clock and a blend run the same on every machine however late the struct arrived.
 */
USTRUCT()
struct MANTLERUNTIME_API FMantleEnvironmentOverride
{
	GENERATED_BODY()

	// ---- Clock ----------------------------------------------------------------------------

	/** False: the default clock. True: the hour runs from the anchor below at TimeScale. */
	UPROPERTY()
	bool bClockSet = false;

	/** The server time the anchor was taken at, and the unwrapped hours since day zero it held. */
	UPROPERTY()
	double ClockAnchorServerSeconds = 0.0;

	UPROPERTY()
	double ClockAnchorTotalHours = 0.0;

	/** Environment.TimeScale: 0 (frozen) to MaxScriptTimeScale times the default rate. */
	UPROPERTY()
	float TimeScale = 1.0f;

	/** Bumped by every ClockTime write, so a jump is told apart from a change of rate. */
	UPROPERTY()
	int32 ClockRevision = 0;

	// ---- Weather --------------------------------------------------------------------------

	/** MantleEnvironmentOverrideValues::WeatherNatural or WeatherForced. */
	UPROPERTY()
	uint8 WeatherMode = 0;

	/** The EMantleWeatherState a script forced (while WeatherMode is WeatherForced). */
	UPROPERTY()
	uint8 ForcedWeather = 0;

	/** The EMantleWeatherState the weather was at when the script changed it: the blend's start. */
	UPROPERTY()
	uint8 WeatherFrom = 0;

	/** Bumped by every weather write; zero while no script has touched the weather. */
	UPROPERTY()
	int32 WeatherRevision = 0;

	/** When the last weather write happened (server seconds), and how long it blends. */
	UPROPERTY()
	double WeatherChangeServerSeconds = 0.0;

	UPROPERTY()
	float WeatherBlendSeconds = 0.0f;

	/** The blended weather values at that moment: where the blend starts from. */
	UPROPERTY()
	float FromCloudCover = 0.0f;

	UPROPERTY()
	float FromFogDensityScale = 1.0f;

	UPROPERTY()
	float FromRainIntensity = 0.0f;

	UPROPERTY()
	float FromLightningPerMinute = 0.0f;

	UPROPERTY()
	float FromSnowIntensity = 0.0f;

	UPROPERTY()
	float FromDustIntensity = 0.0f;

	UPROPERTY()
	float FromWindIntensity = 0.0f;

	UPROPERTY()
	float FromFogAmount = 0.0f;

	/** Environment:SetCloudCover: 0-1 replaces the weather's cover; below zero keeps the weather's own. */
	UPROPERTY()
	float CloudCover = -1.0f;

	// ---- Fog and sky ----------------------------------------------------------------------

	/** Environment.FogDensity: multiplies the weather's fog, 0 to MaxScriptFogDensity. */
	UPROPERTY()
	float FogDensity = 1.0f;

	/** Environment.FogColor: the height fog's inscattering colour, when set. */
	UPROPERTY()
	bool bFogColorSet = false;

	UPROPERTY()
	uint32 FogColor = 0xFFFFFF;

	/** Environment.StarsVisible: MantleEnvironmentOverrideValues::StarsNatural, StarsShown or StarsHidden. */
	UPROPERTY()
	uint8 Stars = 0;

	/** Environment.SunIntensityScale: multiplies the sun and the moon, 0 to MaxScriptSunIntensityScale. */
	UPROPERTY()
	float SunIntensityScale = 1.0f;

	// ---- Post-process (clients only) ------------------------------------------------------

	/** Environment.ExposureBias, in stops: -MaxScriptExposureBias to MaxScriptExposureBias. */
	UPROPERTY()
	float ExposureBias = 0.0f;

	/** Environment.Saturation and Contrast: 1 is unchanged, 0 to 2. */
	UPROPERTY()
	float Saturation = 1.0f;

	UPROPERTY()
	float Contrast = 1.0f;

	/** Environment.Vignette: 0-1, or below zero for the engine's own. */
	UPROPERTY()
	float Vignette = -1.0f;

	/** Environment.ColorTint: multiplies the picture's colour; white is unchanged. */
	UPROPERTY()
	uint32 ColorTint = 0xFFFFFF;

	/** Environment.Bloom (#532, 1.27): 0 to MaxScriptBloom, or below zero for the engine's own. */
	UPROPERTY()
	float Bloom = -1.0f;

	/** Environment.ColorGrading: MantleEnvironmentOverrideValues::ColorGradingNone or a preset. */
	UPROPERTY()
	uint8 ColorGrading = 0;

	/** Environment.ColorGradingIntensity: how much of the preset's LUT shows, 0 to 1. */
	UPROPERTY()
	float ColorGradingIntensity = 1.0f;

	bool operator==(const FMantleEnvironmentOverride& Other) const;
	bool operator!=(const FMantleEnvironmentOverride& Other) const { return !(*this == Other); }

	/**
	 * Clamps every value into its documented range (MantleEnvironmentDefaults); a non-finite value
	 * takes the default. Applied to every script write and to every override a client receives.
	 */
	void Sanitize();

	/** True while the post-process values change nothing, so no volume is needed. */
	bool IsPostProcessNeutral() const;

	/** True while WeatherMode is WeatherForced. */
	bool IsWeatherForced() const { return WeatherMode == MantleEnvironmentOverrideValues::WeatherForced; }

	EMantleWeatherState GetForcedWeather() const;
	EMantleWeatherState GetWeatherFrom() const;

	/** The weather values the last weather write blends from. */
	FMantleWeatherValues GetFromValues() const;
	void SetFromValues(const FMantleWeatherValues& Values);

	/**
	 * Appends, in a fixed order, the script-facing name of every property that differs between
	 * Before and After: ClockTime (a jump), TimeScale, Weather, CloudCover, FogDensity, FogColor,
	 * StarsVisible, SunIntensityScale, ExposureBias, Saturation, Contrast, Vignette, ColorTint,
	 * Bloom, ColorGrading, ColorGradingIntensity. The names Environment.PropertyChanged fires with.
	 */
	static void DiffPropertyNames(const FMantleEnvironmentOverride& Before, const FMantleEnvironmentOverride& After, TArray<FString>& Out);

	/** A preset's script name ("Warm", "Noir"...), or null for ColorGradingNone and anything unknown. */
	static const TCHAR* ColorGradingName(uint8 Preset);

	/** A script's preset name, case-exact, to its number. False for anything not on the list. */
	static bool ParseColorGradingName(const FString& Name, uint8& OutPreset);

	/** "Warm, Cool, Noir, Sepia, Vivid or Faded": the list, for an error that names it. */
	static FString ColorGradingNameList();

	/**
	 * The preset's LUT texture as an object path: <ColorGradingLutFolder>/<ColorGradingLutPrefix><Name>.
	 * Empty for ColorGradingNone and anything unknown.
	 */
	static FSoftObjectPath ColorGradingLutPath(uint8 Preset);

	/** 0xRRGGBB to linear colour, as the fog and the post-process use it. */
	static FLinearColor LinearFromRgb(uint32 Rgb);

	/** "#RRGGBB" for a script, from 0xRRGGBB. */
	static FString RgbToHex(uint32 Rgb);

	/** "#RRGGBB" (the hash optional, six hex digits) to 0xRRGGBB. False when it is not that. */
	static bool ParseHexRgb(const FString& Text, uint32& OutRgb);
};

/**
 * One player's own look (#532 ticket 4, script API 1.29): what a client script's
 * Environment:SetLocalLook asked for. It never replicates: each field that is set replaces the
 * replicated override's field of the same name on this machine only (ApplyTo), inside
 * UMantleEnvironmentSubsystem::UpdatePostProcess, so there is still one volume; a field left unset
 * keeps whatever the server's override says. Only the post-process subset: time and weather stay
 * shared. The ranges are the server members' (Sanitize).
 */
struct MANTLERUNTIME_API FMantleEnvironmentLocalLook
{
	/** Stops, -MaxScriptExposureBias to MaxScriptExposureBias. */
	TOptional<float> ExposureBias;

	/** 1 is unchanged, 0 to MaxScriptSaturation and 0 to MaxScriptContrast. */
	TOptional<float> Saturation;
	TOptional<float> Contrast;

	/** 0 to 1. Never the engine's own: unset is how a local look hands that back. */
	TOptional<float> Vignette;

	/** 0xRRGGBB, sRGB; white is unchanged. */
	TOptional<uint32> ColorTint;

	/** 0 to MaxScriptBloom. */
	TOptional<float> Bloom;

	/**
	 * A MantleEnvironmentOverrideValues::ColorGrading* preset, shown at ColorGradingIntensity when
	 * that is set and at the override's otherwise.
	 */
	TOptional<uint8> ColorGrading;

	/** 0 to 1: how much of the preset shows, this look's or the server's. */
	TOptional<float> ColorGradingIntensity;

	bool operator==(const FMantleEnvironmentLocalLook& Other) const;
	bool operator!=(const FMantleEnvironmentLocalLook& Other) const { return !(*this == Other); }

	/** True when no field is set: the replicated look shows as it is. */
	bool IsEmpty() const;

	/**
	 * Clamps every set field into the server member's range (MantleEnvironmentDefaults); a
	 * non-finite number or an unknown preset is unset instead.
	 */
	void Sanitize();

	/** Writes every set field over InOut's field of the same name, leaving the rest alone. */
	void ApplyTo(FMantleEnvironmentOverride& InOut) const;
};
