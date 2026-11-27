// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * engine.sound (#258): the engine sound a creator picks for a vehicle, by engine type. Each id is one of Mantle's built-in engine sound sets
 * (client/Content/Audio/Vehicles, UMantleEngineSoundSet::Id). A car's and a motorcycle's are apart: a two-wheeler takes a motorcycle's, a quad with
 * handlebars (an ATV) either kind, and every other wheeled or tracked vehicle with a combustion engine a car's. A pedalled vehicle, an electric motor, a
 * trailer, a boat, a plane, a helicopter and a hovercraft have none to pick (yet: aircraft ids join these lists later). The same lists and rule are the
 * cook's (backend/internal/cook/vehicle_engine_sound.go) and validate.py's (ENGINE_SOUNDS_CAR / ENGINE_SOUNDS_BIKE, check_engine_sound_rules).
 */
struct MANTLERUNTIME_API FMantleEngineSounds
{
	/** A car's engine sounds: standard, rotary, inline3 ... w16, diesel. */
	static const TArray<FName>& Car();
	/** A motorcycle's engine sounds: bike_vtwin, bike_inline3, bike_inline4, bike_highrev. */
	static const TArray<FName>& Bike();

	/** Whether a vehicle may pick an engine sound at all: a wheeled or tracked vehicle with a combustion engine, not pedalled. */
	static bool CanPick(bool bWheeledOrTracked, bool bPedal, bool bElectric) { return bWheeledOrTracked && !bPedal && !bElectric; }
	/** The engine sounds a vehicle that CanPick may have: a motorcycle's on two wheels, both kinds on a quad with handlebars, else a car's. */
	static void Options(int32 WheelCount, bool bHandlebars, TArray<FName>& OutIds);
	/** Whether Id is one of the engine sounds Options gives such a vehicle. */
	static bool Fits(FName Id, int32 WheelCount, bool bHandlebars);
};
