// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleEnvironmentDefaults.h"

/**
 * Where the one environment light points and how bright it is, for one hour (#445). By day it
 * is the sun, by night the moon, and near the horizon it fades to nothing before it swaps, so
 * there is only ever one light.
 */
struct FMantleSkyLightState
{
	/** The light's forward vector: the direction its light travels, always pointing downward. */
	FVector Direction = FVector(0.0, 0.0, -1.0);

	/** True when the light is moonlight (the sun is below the horizon). */
	bool bMoon = false;

	/** 0-1: zero at the horizon, one from MantleEnvironmentDefaults::HorizonFadeDegrees up. */
	double Fade = 1.0;
};

/**
 * The shared time of day (#445): pure maths, no world and no state beyond three numbers.
 *
 *     hour = wrap24(StartHour + serverSeconds * TimeScale * 24 / DayLengthSeconds)
 *
 * serverSeconds is the synced server time every client already has (the script host's
 * ServerNow(), the same value as Time.Now), so nothing replicates for the clock and a late
 * joiner computes the same hour as everyone else.
 *
 * The sun runs on a tilted great circle: it rises at 06:00 on the right of the heading, peaks
 * at PeakElevationDegrees at noon behind the heading (so the noon light shines along it), and
 * sets at 18:00 on the left. The heading is the place sun's yaw, or +X. The moon is always
 * exactly opposite the sun.
 */
struct MANTLERUNTIME_API FMantleSkyClock
{
	double StartHour = MantleEnvironmentDefaults::StartHour;
	double TimeScale = 1.0;
	double DayLengthSeconds = MantleEnvironmentDefaults::DayLengthSeconds;

	/** Game hours per real second: 24 / DayLengthSeconds, times TimeScale. */
	double HoursPerSecond() const;

	/** Hour wrapped into [0, 24), negative hours included. */
	static double Wrap24(double Hour);

	/** The sun's elevation above the horizon in degrees: 0 at 06:00 and 18:00, the peak at noon. */
	static double SunElevationDegrees(double Hour);

	/** True while the sun is below the horizon. */
	static bool IsNight(double Hour);

	/** Unit vector from the world toward the sun. */
	static FVector SunDirection(double Hour, double HeadingYawDegrees);

	/** Unit vector from the world toward the moon: always -SunDirection. */
	static FVector MoonDirection(double Hour, double HeadingYawDegrees);

	/** The one environment light for Hour: see FMantleSkyLightState. */
	static FMantleSkyLightState EvaluateLight(double Hour, double HeadingYawDegrees);

	// ---- Night sky (#445 ticket 4) ----------------------------------------------------------

	/**
	 * 0-1, how strongly the stars show for a sun elevation in degrees: 0 while the sun is at or
	 * above the horizon, 1 from StarFadeFullBelowDegrees below it, smooth in between.
	 */
	static double StarFade(double SunElevation);

	/**
	 * 0-1, how strongly the moon's disc shows for a sun elevation in degrees. The moon is always
	 * opposite the sun, so it is up exactly when the sun is down; it fades over the same
	 * HorizonFadeDegrees as the moonlight, so the disc and its light come and go together.
	 */
	static double MoonFade(double SunElevation);

	/**
	 * The axis the sky turns around (the sun's circle's normal, a unit vector). Turning the
	 * sunrise sun direction around it by the sun's progress gives the sun at any hour.
	 */
	static FVector SkyPole(double HeadingYawDegrees);

	/**
	 * The star dome's rotation for Hour: none at sunrise, then turning around SkyPole with the
	 * sun, a full turn a day, so the stars rise and set with it. Every client derives it from
	 * the same hour, so everyone sees the same sky.
	 */
	static FQuat StarDomeRotation(double Hour, double HeadingYawDegrees);

	/** Hours since the start of day zero, not wrapped: StartHour + ServerSeconds * HoursPerSecond. */
	double TotalHoursAt(double ServerSeconds) const;

	/**
	 * The same, from hours since day zero (TotalHoursAt, or a script's re-anchored clock, #454),
	 * so a clock a script jumped or sped up keeps its moon phase in step with its days.
	 */
	static double MoonLunationAtTotalHours(double TotalHours);

	/**
	 * The world-space direction toward the light that shades the moon's phase (the moon
	 * material's PhaseLightDirection). The moon always sits opposite the sun, where the real
	 * sun would only ever light it full, so the phase comes from the sun as it will be
	 * Lunation * 24 hours later: at 0 that is the sun itself, lighting the face toward the world
	 * (full); at 0.5 it is behind the moon (new). It stays on the sun's circle, so the
	 * terminator is always square to the moon's path.
	 */
	static FVector MoonPhaseLightDirection(double Hour, double HeadingYawDegrees, double Lunation);
};
