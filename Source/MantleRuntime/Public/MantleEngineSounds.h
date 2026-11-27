// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * engine.sound (#258): the engine sound a creator picks for a vehicle, by engine type, and which vehicles may have which. Each id is one of Mantle's
 * built-in engine sound sets (client/Content/Audio/Vehicles, UMantleEngineSoundSet::Id). A car's and a motorcycle's are apart: a two-wheeler (wheeled,
 * two wheels) takes a motorcycle's, a vehicle with handlebars on more wheels (an ATV, a jet ski) either kind, and every other wheeled or tracked
 * vehicle or boat with a combustion engine a car's. A pedalled vehicle, an electric motor, a trailer, a plane, a helicopter and a hovercraft have none
 * to pick (yet: aircraft ids join these lists later). The one list and rule for the game: the plugin's dropdown and checks, the reader, and the
 * vehicle audio (UMantleVehicleAudioComponent::EngineSetFits). The cook's (backend/internal/cook/vehicle_engine_sound.go) and validate.py's
 * (ENGINE_SOUNDS_CAR / ENGINE_SOUNDS_BIKE, check_engine_sound_rules) are the same, and their tests hold them to the schema's enum.
 */
struct MANTLERUNTIME_API FMantleEngineSounds
{
	/** A car's engine sounds: standard, rotary, inline3 ... w16, diesel. */
	static const TArray<FName>& Car();
	/** A motorcycle's engine sounds: bike_vtwin, bike_inline3, bike_inline4, bike_highrev. */
	static const TArray<FName>& Bike();

	/** Whether a vehicle may pick an engine sound at all: wheeled, tracked or a boat, with a combustion engine, not pedalled. */
	static bool CanPick(bool bWheeledTrackedOrBoat, bool bPedal, bool bElectric) { return bWheeledTrackedOrBoat && !bPedal && !bElectric; }
	/** A two-wheeler, which sounds like a motorcycle only: a wheeled vehicle on two wheels. */
	static bool IsTwoWheeler(bool bWheeled, int32 WheelCount) { return bWheeled && WheelCount == 2; }
	/** Whether a vehicle that CanPick takes a car's engine sounds: anything but a two-wheeler. */
	static bool TakesCar(bool bTwoWheeler) { return !bTwoWheeler; }
	/** Whether a vehicle that CanPick takes a motorcycle's engine sounds: a two-wheeler, or handlebars on more wheels (an ATV, a jet ski). */
	static bool TakesBike(bool bTwoWheeler, bool bHandlebars) { return bTwoWheeler || bHandlebars; }
	/** The engine sounds a vehicle that CanPick may have: a car's (TakesCar), then a motorcycle's (TakesBike). */
	static void Options(bool bTwoWheeler, bool bHandlebars, TArray<FName>& OutIds);
	/** Whether Id is one of the engine sounds Options gives such a vehicle. */
	static bool Fits(FName Id, bool bTwoWheeler, bool bHandlebars);
};
