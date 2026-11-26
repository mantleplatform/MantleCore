// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * The air a flier moves through (#380, shared with #379): the International Standard Atmosphere's troposphere, so the air thins with height.
 * World Z 0 is sea level: a map built with its ground high up flies in thinner air there (a heavy helicopter cannot hover out of ground effect
 * on a 2.3 km high map, as a real one cannot). Below Z 0 the air is sea level's; above 11 km (the troposphere's top) it is 11 km's.
 * Pure and header-only, for the game thread, the physics thread and the server alike.
 */
struct FMantleAtmosphere
{
	/** Sea level's air density, kg/m^3 (ISA; Chaos::RealWorldConsts::AirDensity). */
	static constexpr double SeaLevelDensity = 1.225;
	/** The highest the formula is used, m: the troposphere's top. */
	static constexpr double MaxHeightMetres = 11000.0;

	/** The air's density at this world Z (cm) over sea level's, 0..1: (1 - 2.25577e-5 h)^4.2559, h in metres, clamped to [0, 11 km]. */
	static double DensityRatio(const double WorldZCm)
	{
		const double Metres = FMath::IsFinite(WorldZCm) ? FMath::Clamp(WorldZCm / 100.0, 0.0, MaxHeightMetres) : 0.0;
		return FMath::Pow(1.0 - 2.25577e-5 * Metres, 4.2559);
	}

	/** The air's density at this world Z (cm), kg/m^3. */
	static double Density(const double WorldZCm)
	{
		return SeaLevelDensity * DensityRatio(WorldZCm);
	}
};
