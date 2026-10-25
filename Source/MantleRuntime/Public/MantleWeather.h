// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleEnvironmentDefaults.h"

/**
 * The weather states (#446, #930). The first eight (Clear to LightRain) are the natural weather, which the seeded
 * sequence draws; the rest (LightSnow to DustStorm) come only when a script or a place asks for them. Script-only
 * states stay last, so MantleIsNaturalWeather is a range check.
 */
enum class EMantleWeatherState : uint8
{
	Clear,
	PartlyCloudy,
	Overcast,
	Rain,
	Storm,
	Cloudy,
	Foggy,
	LightRain,
	LightSnow,
	Snow,
	Blizzard,
	DustCalm,
	DustStorm,
};

/** How many weather states there are, and how many of them (the first) the natural weather draws. */
constexpr int32 MantleWeatherStateCount = 13;
constexpr int32 MantleNaturalWeatherStateCount = 8;
constexpr EMantleWeatherState MantleLastWeatherState = EMantleWeatherState::DustStorm;

/** True for the states the seeded sequence can draw on its own (Clear to LightRain). */
constexpr bool MantleIsNaturalWeather(EMantleWeatherState State)
{
	return static_cast<int32>(State) < MantleNaturalWeatherStateCount;
}

/** "Clear", "PartlyCloudy", "Overcast", "Rain", "Storm", "Cloudy", ... "DustStorm": the names scripts see. */
MANTLERUNTIME_API const TCHAR* MantleWeatherStateName(EMantleWeatherState State);

/** The state MantleWeatherStateName names, case-exact (a place's settings.environment.weather, #532). */
MANTLERUNTIME_API bool MantleParseWeatherState(const FString& Name, EMantleWeatherState& OutState);

/**
 * What one weather state (or a blend of two) looks like. The states #930 added take Ultra Dynamic Weather's presets
 * (its 0-10 values over 10); the first five keep the values they were tuned to.
 */
struct MANTLERUNTIME_API FMantleWeatherValues
{
	/** 0-1: how much of the sky the volumetric clouds cover. */
	double CloudCover = 0.0;

	/** Multiplies the height fog's engine-default density: 1 is unchanged. */
	double FogDensityScale = 1.0;

	/** 0-1: how hard it rains. */
	double RainIntensity = 0.0;

	/** Lightning strikes per minute of environment time. */
	double LightningPerMinute = 0.0;

	/** 0-1: how hard it snows. */
	double SnowIntensity = 0.0;

	/** 0-1: how much sand and dust is blowing. */
	double DustIntensity = 0.0;

	/** 0-1: how hard the wind blows (the package's Wind Intensity over 10). */
	double WindIntensity = 0.0;

	/**
	 * 0-1: how foggy it is (the package's Fog over 10), which shapes the default fog (how high it reaches, how it is
	 * lit). FogDensityScale sets how thick it is.
	 */
	double FogAmount = 0.0;

	static FMantleWeatherValues Lerp(const FMantleWeatherValues& A, const FMantleWeatherValues& B, double Alpha);
};

/**
 * What the weather does to the default sky (#446 ticket 2): the values the environment subsystem
 * writes to the cloud material, the height fog, the sky light, the sun and the night sky, from
 * the blended FMantleWeatherValues alone. Pure maths, so every client derives the same look from
 * the replicated seed and the clock.
 *
 * Every value is clamped, and each moves one way only as the cloud cover rises: more and denser
 * cloud, dimmer light, fewer stars. The constants are in MantleEnvironmentDefaults and are tuned
 * by eye at the user's visual check.
 */
struct MANTLERUNTIME_API FMantleWeatherLook
{
	/**
	 * The cloud material's Cloud_GlobalCoverage: added to its cloud layout, so a negative value
	 * thins the clouds. Partly cloudy lands on the material's own -0.2, the sky every place had
	 * before the weather.
	 */
	float CloudCoverage = 0.0f;

	/** The cloud material's Cloud_GlobalDensity: its own 0.008 until the cover is heavy, then up. */
	float CloudDensity = 0.0f;

	/**
	 * 0-1, the cloud material's StormClouds: the blend to darker storm clouds, taken from the rain.
	 * It also gates the clouds' lightning glow, so the clouds flash only in rain and storms.
	 */
	float StormClouds = 0.0f;

	/** Multiplies the height fog's density as it was bound. */
	float FogDensityScale = 1.0f;

	/** Multiply the sun's (by night the moon's) intensity and the sky light's, as they were bound. */
	float SunIntensityScale = 1.0f;
	float SkyLightIntensityScale = 1.0f;

	/**
	 * 0-1: multiplies the star dome's and the moon's fades. The night sky is additive and draws
	 * over the clouds, so it fades with the cover instead of being hidden by it: 1 at Clear's
	 * cover and below, 0 at full cover.
	 */
	float StarVisibility = 1.0f;

	/**
	 * SunElevationDegrees: the sun's height (below the horizon by night), for heavy fog's dimming, which Ultra Dynamic
	 * Sky makes stronger with the sun low (its Directional Inscattering Multiplier).
	 */
	static FMantleWeatherLook FromValues(const FMantleWeatherValues& Values, double SunElevationDegrees = 90.0);

	/**
	 * 0-1: what heavy fog leaves of the sun's (and the moon's) light: all of it up to the package's Fog 5, then down to
	 * half of 0.25-0.35 (the sun low to high) at Fog 10. 0.175 with the sun 48.6 degrees up, read live off its Foggy.
	 */
	static double FogSunScale(double FogAmount, double SunElevationDegrees);

	/**
	 * 0-1: how much of the cloud material's own lightning brightness a flash of this strength
	 * (UMantleEnvironmentSubsystem::GetLightningFlash, which overlapping strikes can push past 1)
	 * shows.
	 */
	static float CloudLightningScale(double Flash);
};

/**
 * One slot of the weather sequence: StartSeconds to StartSeconds + LengthSeconds of
 * environment time, blending from PreviousState into State over its first
 * MantleEnvironmentDefaults::WeatherBlendSeconds. Whole seconds, so every machine's slot
 * boundaries are exact integer sums and never drift apart.
 */
struct FMantleWeatherSlot
{
	int64 Index = 0;
	int64 StartSeconds = 0;
	int64 LengthSeconds = 0;
	EMantleWeatherState State = EMantleWeatherState::Clear;
	EMantleWeatherState PreviousState = EMantleWeatherState::Clear;

	int64 EndSeconds() const { return StartSeconds + LengthSeconds; }
};

/** The weather at one moment. */
struct FMantleWeatherSample
{
	int64 SlotIndex = 0;

	/** The state being blended toward: what scripts read as Environment.Weather. */
	EMantleWeatherState State = EMantleWeatherState::Clear;
	EMantleWeatherState PreviousState = EMantleWeatherState::Clear;

	/** 0-1 through the slot's opening blend; 1 once the blend is done. */
	double Blend = 1.0;

	/** The blended values. */
	FMantleWeatherValues Values;
};

/** One lightning strike, identical on every machine: (SlotIndex, StrikeIndex) names it. */
struct FMantleLightningStrike
{
	int64 SlotIndex = 0;
	int32 StrikeIndex = 0;

	/** Environment seconds; an exact number of milliseconds. */
	double Seconds = 0.0;

	/** 0.6-1: how bright this flash is. */
	double Brightness = 1.0;

	/**
	 * 0-1: which frame of the cloud material's lightning animation this flash shows
	 * (Storm_LightningAnim's ManualLightningAnim), so each strike lights the clouds differently
	 * and every client shows the same one.
	 */
	double CloudFrame = 0.0;
};

/**
 * The default weather sequence (#446): pure maths from one seed, so every machine that knows
 * the seed and the time computes the same weather, a late joiner included, with nothing
 * replicated but the seed.
 *
 * Time is cut into slots of 12-20 minutes. Each slot's length comes from
 * Hash(Seed, slotIndex), and its state from the same hash against a weighted table of the natural
 * states (Clear 32%, PartlyCloudy 25%, Cloudy 15%, Overcast 10%, Foggy 5%, LightRain 6%, Rain 5%,
 * Storm 2%; the owner's shares, #930). A storm is always followed by Rain, LightRain or Overcast;
 * the draw weights are compensated for that rule so the long-run shares still match the table.
 * The script-only states (snow and dust) are never drawn.
 *
 * Lightning: in any slot that is, or blends out of, a Storm, strike candidates are spaced by
 * Hash(Seed, slotIndex, strikeIndex) and thinned by the blended lightning rate at their time,
 * so every client flashes at the same environment time with no message per strike.
 *
 * Every number here must come out bit-identical on the Windows client and the Linux server:
 * integer hashing and integer slot and strike times only, and the tests pin golden values.
 */
class MANTLERUNTIME_API FMantleWeatherSequence
{
public:
	explicit FMantleWeatherSequence(int32 InSeed = 1);

	int32 GetSeed() const { return Seed; }

	/** The documented share of each state, in percent (see the class comment); 0 for the script-only states. */
	static int32 SharePercent(EMantleWeatherState State);

	/** The look of each state on its own. */
	static const FMantleWeatherValues& ValuesFor(EMantleWeatherState State);

	/** SplitMix64 over (Seed, A, B, Salt): the one source of randomness for the weather. */
	static uint64 Hash(int32 Seed, int64 A, int64 B, uint32 Salt);

	/** Hash mapped to [0, 1) with 53 bits, exactly. */
	static double HashUnit(int32 Seed, int64 A, int64 B, uint32 Salt);

	/** The slot covering WeatherSeconds (negative times fall in slot 0). */
	const FMantleWeatherSlot& SlotAt(double WeatherSeconds);

	/** Slot number Index (0 and up). */
	const FMantleWeatherSlot& SlotByIndex(int64 Index);

	/** The weather at WeatherSeconds. */
	FMantleWeatherSample Sample(double WeatherSeconds);

	/** Every strike of slot Index, in time order. Empty unless the slot is or follows a Storm. */
	TArray<FMantleLightningStrike> StrikesInSlot(int64 Index);

	/** Appends every strike with FromSeconds < Seconds <= ToSeconds to Out, in time order. */
	void CollectStrikes(double FromSeconds, double ToSeconds, TArray<FMantleLightningStrike>& Out);

	/**
	 * #454: the lightning of a weather a script forced (Environment:SetWeather), which no slot
	 * describes. Environment time from StartSeconds is cut into ScriptLightningChunkSeconds
	 * chunks; each chunk's candidates are spaced like a slot's, from the same hash under a key no
	 * slot uses (it carries Revision, so every weather write gets fresh strikes), and thinned by
	 * RateAt(seconds), the strikes per minute the blended weather has then. Every machine with the
	 * seed, the revision and the start therefore flashes at the same moments, with no message
	 * per strike. Appends every strike with FromSeconds < Seconds <= ToSeconds, in time order.
	 */
	static void CollectScriptStrikes(int32 Seed, int32 Revision, double StartSeconds, double FromSeconds, double ToSeconds,
		TFunctionRef<double(double)> RateAt, TArray<FMantleLightningStrike>& Out);

#if !UE_BUILD_SHIPPING
	/**
	 * The test-only -MantleEnvWeatherSeed=<n> switch (non-Shipping, the same value on the
	 * server and every client). Zero is not a seed (it means "not received yet" on a client),
	 * so it is refused. Returns true and sets OutSeed when the switch is present and valid.
	 */
	static bool ParseTestSeed(const TCHAR* CommandLine, int32& OutSeed);
#endif

private:
	/** Computes slots until the last one reaches past EnvironmentSeconds, or until Index exists. */
	void ExtendPastSeconds(int64 EnvironmentSeconds);
	void ExtendToIndex(int64 Index);
	void AppendNextSlot();

	int32 Seed = 1;

	/** Every slot computed so far, from slot 0. Each depends on the one before (the storm rule). */
	TArray<FMantleWeatherSlot> Slots;
};
