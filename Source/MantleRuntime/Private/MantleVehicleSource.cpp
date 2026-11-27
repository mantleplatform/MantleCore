// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleSource.h"

#include "MantleEngineSounds.h"

bool UMantleVehicleSource::CanPickEngineSound() const
{
	const bool bEngineMovement = MovementType == EMantleVehicleMovementKind::Wheeled || MovementType == EMantleVehicleMovementKind::Tracked
		|| MovementType == EMantleVehicleMovementKind::Boat;
	return FMantleEngineSounds::CanPick(bEngineMovement, Tuning.bPedal, Tuning.bElectric);
}

TArray<FName> UMantleVehicleSource::GetEngineSoundOptions() const
{
	TArray<FName> Options;
	if (CanPickEngineSound())
	{
		FMantleEngineSounds::Options(FMantleEngineSounds::IsTwoWheeler(MovementType == EMantleVehicleMovementKind::Wheeled, Wheels.Num()),
			DriverSeatKind == EMantleVehicleSeatKind::Motorcycle, Options);
	}
	Options.Insert(NAME_None, 0);
	return Options;
}
