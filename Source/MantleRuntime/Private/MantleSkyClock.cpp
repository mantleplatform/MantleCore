// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSkyClock.h"

namespace
{
	/** The angle along the sun's circle: 0 at sunrise, 90 degrees at noon, 180 at sunset. */
	double SunArcRadians(double Hour)
	{
		const double SinceSunrise = FMantleSkyClock::Wrap24(Hour - MantleEnvironmentDefaults::SunriseHour);
		return SinceSunrise / 24.0 * UE_DOUBLE_TWO_PI;
	}

	/** The heading's horizontal forward vector, from a yaw in degrees. */
	FVector HeadingForward(double HeadingYawDegrees)
	{
		const double Yaw = FMath::DegreesToRadians(HeadingYawDegrees);
		return FVector(FMath::Cos(Yaw), FMath::Sin(Yaw), 0.0);
	}
}

double FMantleSkyClock::HoursPerSecond() const
{
	return DayLengthSeconds > 0.0 ? 24.0 / DayLengthSeconds * TimeScale : 0.0;
}

double FMantleSkyClock::Wrap24(double Hour)
{
	double Wrapped = FMath::Fmod(Hour, 24.0);
	if (Wrapped < 0.0)
	{
		Wrapped += 24.0;
	}
	// Fmod of a tiny negative number plus 24 can round to exactly 24.
	return Wrapped >= 24.0 ? 0.0 : Wrapped;
}

double FMantleSkyClock::SunElevationDegrees(double Hour)
{
	const double Peak = FMath::DegreesToRadians(MantleEnvironmentDefaults::PeakElevationDegrees);
	const double SinElevation = FMath::Sin(SunArcRadians(Hour)) * FMath::Sin(Peak);
	return FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(SinElevation, -1.0, 1.0)));
}

bool FMantleSkyClock::IsNight(double Hour)
{
	return SunElevationDegrees(Hour) < 0.0;
}

FVector FMantleSkyClock::SunDirection(double Hour, double HeadingYawDegrees)
{
	// Two orthonormal vectors span the sun's circle: East, where it rises (the heading's right),
	// and Up, where it peaks (tilted from the zenith back against the heading by 90 - peak
	// degrees, so the noon light travels along the heading).
	const FVector Forward = HeadingForward(HeadingYawDegrees);
	const FVector East(-Forward.Y, Forward.X, 0.0);
	const double Peak = FMath::DegreesToRadians(MantleEnvironmentDefaults::PeakElevationDegrees);
	const FVector Up = FVector::UpVector * FMath::Sin(Peak) - Forward * FMath::Cos(Peak);

	const double Arc = SunArcRadians(Hour);
	return (East * FMath::Cos(Arc) + Up * FMath::Sin(Arc)).GetSafeNormal();
}

FVector FMantleSkyClock::MoonDirection(double Hour, double HeadingYawDegrees)
{
	return -SunDirection(Hour, HeadingYawDegrees);
}

FMantleSkyLightState FMantleSkyClock::EvaluateLight(double Hour, double HeadingYawDegrees)
{
	FMantleSkyLightState State;
	const FVector Sun = SunDirection(Hour, HeadingYawDegrees);
	const double Elevation = SunElevationDegrees(Hour);

	// The light travels away from whichever body is up: -Sun by day, -Moon (= +Sun) by night.
	State.bMoon = Elevation < 0.0;
	State.Direction = State.bMoon ? Sun : -Sun;
	State.Fade = FMath::SmoothStep(0.0, MantleEnvironmentDefaults::HorizonFadeDegrees, FMath::Abs(Elevation));
	return State;
}

double FMantleSkyClock::StarFade(double SunElevation)
{
	return FMath::SmoothStep(0.0, MantleEnvironmentDefaults::StarFadeFullBelowDegrees, -SunElevation);
}

double FMantleSkyClock::MoonFade(double SunElevation)
{
	return FMath::SmoothStep(0.0, MantleEnvironmentDefaults::HorizonFadeDegrees, -SunElevation);
}

FVector FMantleSkyClock::SkyPole(double HeadingYawDegrees)
{
	// The sunrise direction crossed with the sun's peak direction (see SunDirection): turning
	// East around this by the arc angle carries it to East * cos + Up * sin.
	const FVector East = SunDirection(MantleEnvironmentDefaults::SunriseHour, HeadingYawDegrees);
	const FVector Up = SunDirection(MantleEnvironmentDefaults::SunriseHour + 6.0, HeadingYawDegrees);
	return (East ^ Up).GetSafeNormal();
}

FQuat FMantleSkyClock::StarDomeRotation(double Hour, double HeadingYawDegrees)
{
	return FQuat(SkyPole(HeadingYawDegrees), SunArcRadians(Hour));
}

double FMantleSkyClock::TotalHoursAt(double ServerSeconds) const
{
	return StartHour + ServerSeconds * HoursPerSecond();
}

double FMantleSkyClock::MoonLunationAtTotalHours(double TotalHours)
{
	const double Days = TotalHours / 24.0;
	return FMath::Frac(Days / MantleEnvironmentDefaults::MoonCycleDays + MantleEnvironmentDefaults::MoonLunationAtStart);
}

FVector FMantleSkyClock::MoonPhaseLightDirection(double Hour, double HeadingYawDegrees, double Lunation)
{
	return SunDirection(Hour + Lunation * 24.0, HeadingYawDegrees);
}
