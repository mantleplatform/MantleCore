// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * The platform's one default environment (#445, #446): every place gets the same day length,
 * start hour, sun path and weather table. These are constants, not settings, and nothing here is
 * part of the place schema. Server scripts change the running environment through script API
 * 1.11 (#454, FMantleEnvironmentOverride), within the ranges at the end of this file; a place's
 * settings.environment (#532) starts it elsewhere, within the same ranges, through the same override.
 */
namespace MantleEnvironmentDefaults
{
	// ---- Clock (#445) ----------------------------------------------------------------------

	/** One full day, sunrise to sunrise, in real seconds: 30 minutes, about 15 of them daylight. */
	constexpr double DayLengthSeconds = 1800.0;

	/** The hour every server starts at, at server time zero. */
	constexpr double StartHour = 9.0;

	/** The sun crosses the horizon at these hours; the moon crosses it twelve hours apart. */
	constexpr double SunriseHour = 6.0;
	constexpr double SunsetHour = 18.0;

	/** The sun's elevation at noon, and the moon's at midnight. */
	constexpr double PeakElevationDegrees = 60.0;

	// ---- Sun and moon light (#445) ---------------------------------------------------------

	/**
	 * The light is re-aimed only once the clock has moved it this far. Lumen and virtual shadow
	 * maps run at engine defaults, so every re-aim invalidates shadow pages and lighting; at
	 * 0.2 degrees a second this is one step about every 1.25 s. Measured in #445 ticket 3.
	 */
	constexpr double SunStepDegrees = 0.25;

	/**
	 * Near the horizon the light fades to zero over this many degrees of elevation, flips to the
	 * other body, and fades back in, so the switch between sun and moon happens at zero intensity
	 * and never pops.
	 */
	constexpr double HorizonFadeDegrees = 6.0;

	/**
	 * Moonlight, as a fraction of the place sun's own intensity. Tuned against fixed exposure
	 * (DefaultEngine.ini turns AutoExposure off).
	 */
	constexpr float MoonIntensityScale = 0.12f;

	/** Moonlight's cool colour, in linear space. */
	inline const FLinearColor MoonColor(0.55f, 0.66f, 1.0f);

	/**
	 * Intensity changes below this fraction of the light's day intensity are not applied, so a
	 * fade that is not moving does not dirty the light's render state every frame.
	 */
	constexpr float IntensityEpsilonScale = 0.002f;

	// ---- Night sky: star dome and moon (#445 ticket 4) -------------------------------------

	/**
	 * The stars start to fade in as the sun reaches the horizon and are fully out once it is this
	 * many degrees below it (and the reverse at dawn): about a minute of a 30-minute day.
	 */
	constexpr double StarFadeFullBelowDegrees = 10.0;

	/**
	 * The star dome's radius in cm: 20 km, past the default volumetric cloud layer's top (5 km
	 * up, 10 km thick), so the clouds always pass in front of the stars.
	 */
	constexpr double StarDomeRadius = 2000000.0;

	/** How far from the world's centre the moon sits, in cm: inside the dome, above the clouds. */
	constexpr double MoonDistance = 1800000.0;

	/** The moon's apparent diameter. The real moon's half a degree reads as a speck in a game. */
	constexpr double MoonAngularDiameterDegrees = 2.0;

	/**
	 * The dome is turned, and the moon moved, only once the clock has carried them this far:
	 * well under a pixel at 1080p, so the stars never visibly jump, and at 0.2 degrees a second
	 * one transform update every 50 ms rather than every frame. Neither casts shadows, so a move
	 * invalidates no shadow pages or lighting.
	 */
	constexpr double NightSkyStepDegrees = 0.01;

	/** A star or moon fade change below this is not applied (the ends of a fade always are). */
	constexpr double NightSkyFadeEpsilon = 0.002;

	/**
	 * One cycle of the moon's phases, in game days (4 real hours), so players see the phase
	 * change from night to night. The moon always sits opposite the sun, so its phase cannot
	 * come from where the sun really is (that would be a full moon forever): see
	 * FMantleSkyClock::MoonPhaseLightDirection.
	 */
	constexpr double MoonCycleDays = 8.0;

	/**
	 * Where in the cycle (0 full, 0.5 new) the moon is at server time zero. A server's first
	 * midnight (day 1 at a 9:00 start) then shows a moon about three-quarters lit.
	 */
	constexpr double MoonLunationAtStart = 0.05;

	// ---- Weather (#446) --------------------------------------------------------------------
	// Weather time is environment time: server seconds times the clock's time scale.

	/** Each weather slot lasts a whole number of seconds in this range, both ends included. */
	constexpr int64 WeatherSlotMinSeconds = 12 * 60;
	constexpr int64 WeatherSlotMaxSeconds = 20 * 60;

	/** A new slot blends in from the previous slot's state over this many seconds. */
	constexpr double WeatherBlendSeconds = 90.0;

	/**
	 * Lightning candidates in a storm are this many milliseconds apart, both ends included: the
	 * game client's storm (Ultra Dynamic Weather's, client content) flashes every 60/14 s, half
	 * that to one and a half times it, so a mean of 4.29 s and 14 strikes a minute at full storm.
	 */
	constexpr int64 LightningIntervalMinMs = 2143;
	constexpr int64 LightningIntervalMaxMs = 6429;

	/** The strike rate at full storm; candidates are thinned by the blended rate over this. */
	constexpr double MaxLightningPerMinute = 14.0;

	/** A flash's length in real seconds (not scaled by the test time scale): bright, then gone. */
	constexpr double LightningFlashSeconds = 0.35;

	/** A full-brightness flash adds this multiple of the sun's day intensity to the light. */
	constexpr float LightningFlashIntensityScale = 1.5f;

	/**
	 * After a hitch or a jump in the clock, strikes further back than this many environment
	 * seconds are skipped instead of all firing at once.
	 */
	constexpr double LightningCatchUpSeconds = 10.0;

	// ---- Weather on the sky (#446 ticket 2), see FMantleWeatherLook -----------------------
	// Tuned by eye. The cloud values are the default cloud material's
	// own parameters (m_SimpleVolumetricCloud, read from the asset; the test
	// Mantle.Environment.Weather.CloudMaterialParameters pins its defaults).

	/**
	 * Cloud_GlobalCoverage at no cloud cover and at full cover, linear between. The material's
	 * own -0.2 falls at a cover of 0.45, partly cloudy: the sky every place had before weather.
	 */
	constexpr double CloudCoverageAtNoCover = -0.75;
	constexpr double CloudCoverageAtFullCover = 0.5;

	/** Above this cover the clouds thicken and the light dims, reaching the full-cover values at 1. */
	constexpr double HeavyCloudCoverStart = 0.5;

	/** Cloud_GlobalDensity: the material's own value up to heavy cover, then up to this at full cover. */
	constexpr double CloudDensityBelowHeavyCover = 0.008;
	constexpr double CloudDensityAtFullCover = 0.02;

	/**
	 * The sun (by night the moon) by cloud cover, as a fraction of its own intensity, linear between the
	 * points. Mantle never turns on the light's cloud shadows (bCastCloudShadows), so the clouds do not
	 * darken the ground on their own: the light itself dims under an overcast sky. The curve is Ultra
	 * Dynamic Sky's, read off it live (its sun at its cover 3.8 to 7 of 10): untouched while the sky is
	 * broken, then down to 0.35 by overcast, faster and faster as the gaps close.
	 */
	inline constexpr double SunDimCover[] = { 0.38, 0.45, 0.5, 0.55, 0.6, 0.65, 0.7 };
	inline constexpr double SunDimScale[] = { 1.0, 0.982, 0.9278, 0.8376, 0.7112, 0.5486, 0.35 };

	/**
	 * The sky light at full cover, as a fraction of its own intensity, from HeavyCloudCoverStart. Ultra Dynamic Sky keeps its
	 * sky light whole in every weather and brightens its sky instead, which lights the
	 * overcast clouds pale grey; dimming it here left them dark blue.
	 */
	constexpr double SkyLightIntensityAtFullCover = 1.0;

	/** The stars and the moon show fully at this cover (Clear's) and below, and are gone at full cover. */
	constexpr double StarsFullyShownBelowCover = 0.15;

	/**
	 * The default fog for a place with none of its own (FMantleEnvironment::DefaultFogSettings): Ultra Dynamic Sky's base
	 * fog, read off it live in clear weather. The engine's own (0.02, falling off at 0.2, from the camera) hugs the ground
	 * and greys out everything near; this is thinner and reaches higher, and starts 73 m out.
	 */
	constexpr double DefaultFogDensity = 0.00551;
	constexpr double DefaultFogHeightFalloff = 0.06;
	constexpr double DefaultFogStartDistance = 7348.0;

	/**
	 * FMantleWeatherValues::FogDensityScale is clamped to this, whatever the table says. The package's dust storms are
	 * about 92 times its base fog (Dust 10: 0.0055 + 0.5).
	 */
	constexpr double MaxFogDensityScale = 100.0;

	/**
	 * A cloud, fog or sky-light change smaller than this fraction of the value's own scale is not
	 * written, so a 90 s blend is a few hundred steps of half a percent, too small to see, rather
	 * than a write every frame. Once the weather has settled, the last difference is always
	 * written, so the values land exactly.
	 */
	constexpr double WeatherLookEpsilon = 0.005;

	// ---- Script overrides (#454, script API 1.11), see FMantleEnvironmentOverride ----------
	// The documented ranges: every script write is clamped to these, and so is every override a
	// client receives.

	/** Environment.TimeScale: 0 freezes the clock; 60 runs a 30-minute day in 30 seconds. */
	constexpr double MaxScriptTimeScale = 60.0;

	/** Environment:SetWeather / ResumeNaturalWeather: the blend when a script names none, and the longest. */
	constexpr double DefaultScriptWeatherBlendSeconds = WeatherBlendSeconds;
	constexpr double MaxScriptWeatherBlendSeconds = 600.0;

	/** Environment.FogDensity multiplies the weather's fog: 0 clears it, 4 is four times as thick. */
	constexpr double MaxScriptFogDensity = 4.0;

	/** Environment.SunIntensityScale multiplies the sun and the moon. */
	constexpr double MaxScriptSunIntensityScale = 4.0;

	/** The post-process subset: exposure in stops, saturation and contrast around 1 (unchanged). */
	constexpr double MaxScriptExposureBias = 4.0;
	constexpr double MaxScriptSaturation = 2.0;
	constexpr double MaxScriptContrast = 2.0;

	/** Environment.Bloom (#532, script API 1.27): the bloom's intensity, 0 (none) to this. */
	constexpr double MaxScriptBloom = 4.0;

	// ---- Exposure and height fog of a place's look (#628, settings.environment) -------------------
	// Place keys only: no script writes these. The ranges are the schema's, validate.py's and the backend's.

	/** settings.environment.exposure.bias: stops brighter or darker than the game's own exposure compensation. */
	constexpr double MaxExposureBias = 4.0;

	/** exposure.minBrightness and maxBrightness: the auto exposure's range, in the engine's luminance units. */
	constexpr double MinExposureBrightness = 0.03;
	constexpr double MaxExposureBrightness = 20.0;

	/** What an auto exposure's range is when the place names no end of it: the engine's own default range. */
	constexpr double DefaultAutoExposureMin = 0.03;
	constexpr double DefaultAutoExposureMax = 8.0;

	/** settings.environment.fog: the height fog's density, falloff, colours (a channel's luminance) and start distance (cm). */
	constexpr double MaxFogDensity = 0.05;
	constexpr double MinFogHeightFalloff = 0.001;
	constexpr double MaxFogHeightFalloff = 2.0;
	constexpr double MaxFogColor = 10.0;
	constexpr double MaxFogStartDistance = 1000000.0;

	/**
	 * Environment.ColorGrading's LUTs (#532): <folder>/<prefix><Preset>, a 256x16 colour lookup
	 * table each, made by tools/materials/Create-MantleLUTs.py. /Game/Mantle/Materials is in
	 * DefaultGame.ini's DirectoriesToAlwaysCook, so every client has them without a reference.
	 */
	constexpr const TCHAR* ColorGradingLutFolder = TEXT("/Game/Mantle/Materials/Textures");
	constexpr const TCHAR* ColorGradingLutPrefix = TEXT("T_MantleLUT_");

	/**
	 * A script-forced storm's lightning is laid out in chunks of this many environment seconds
	 * from the moment the script forced it, each with its own hashed strike times, so a strike
	 * query never walks back further than one chunk however long the storm has been forced.
	 */
	constexpr int64 ScriptLightningChunkSeconds = 600;

	/** The post-process volume the environment owns outranks any other (places have none today). */
	constexpr float ScriptPostProcessPriority = 1000.0f;
}
