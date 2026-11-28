// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * engine.sound (#258): the engine sound a creator picks for a vehicle, by engine type, and which vehicles may have which. Each id is one of Mantle's
 * built-in engine sound sets (client/Content/Audio/Vehicles, UMantleEngineSoundSet::Id). A car's, a motorcycle's and an aircraft's are apart: a flier
 * (a plane, a helicopter, a hovercraft) takes an aircraft's and nothing else does; a two-wheeler (wheeled, two wheels) takes a motorcycle's, a vehicle
 * with handlebars on more wheels (an ATV, a jet ski) either a car's or a motorcycle's, and every other wheeled or tracked vehicle or boat with a
 * combustion engine a car's. A pedalled vehicle, an electric motor and a trailer have none to pick. The one list and rule for the game: the plugin's
 * dropdown and checks, the reader, and the vehicle audio (UMantleVehicleAudioComponent::EngineSetFits). The cook's
 * (backend/internal/cook/vehicle_engine_sound.go) and validate.py's (ENGINE_SOUNDS_CAR / _BIKE / _AIRCRAFT, check_engine_sound_rules) are the same,
 * and their tests hold them to the schema's enum.
 */
struct MANTLERUNTIME_API FMantleEngineSounds
{
	/** A car's engine sounds: standard, rotary, inline3 ... w16, diesel, racing. */
	static const TArray<FName>& Car();
	/** A motorcycle's engine sounds: bike_vtwin, bike_inline3, bike_inline4, bike_highrev. */
	static const TArray<FName>& Bike();
	/** An aircraft's engine sounds (car sounds step 4): aircraft_airliner, aircraft_fighter, aircraft_transport, helicopter, hovercraft. */
	static const TArray<FName>& Aircraft();

	/** Whether a vehicle may pick an engine sound at all: wheeled, tracked or a boat with a combustion engine, not pedalled; or a flier (a plane, a
	 *  helicopter, a hovercraft). */
	static bool CanPick(bool bWheeledTrackedOrBoat, bool bFlier, bool bPedal, bool bElectric)
	{
		return bFlier || (bWheeledTrackedOrBoat && !bPedal && !bElectric);
	}
	/** A two-wheeler, which sounds like a motorcycle only: a wheeled vehicle on two wheels. */
	static bool IsTwoWheeler(bool bWheeled, int32 WheelCount) { return bWheeled && WheelCount == 2; }
	/** Whether a vehicle that CanPick takes a car's engine sounds: anything but a flier or a two-wheeler. */
	static bool TakesCar(bool bFlier, bool bTwoWheeler) { return !bFlier && !bTwoWheeler; }
	/** Whether a vehicle that CanPick takes a motorcycle's engine sounds: a two-wheeler, or handlebars on more wheels (an ATV, a jet ski); never a flier. */
	static bool TakesBike(bool bFlier, bool bTwoWheeler, bool bHandlebars) { return !bFlier && (bTwoWheeler || bHandlebars); }
	/** Whether a vehicle that CanPick takes an aircraft's engine sounds: a flier, and only a flier. */
	static bool TakesAircraft(bool bFlier) { return bFlier; }
	/** The engine sounds a vehicle that CanPick may have: a car's (TakesCar), then a motorcycle's (TakesBike), then an aircraft's (TakesAircraft). */
	static void Options(bool bFlier, bool bTwoWheeler, bool bHandlebars, TArray<FName>& OutIds);
	/** Whether Id is one of the engine sounds Options gives such a vehicle. */
	static bool Fits(FName Id, bool bFlier, bool bTwoWheeler, bool bHandlebars);
};
