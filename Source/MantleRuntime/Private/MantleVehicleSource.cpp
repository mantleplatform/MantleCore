// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleSource.h"

#include "MantleEngineSounds.h"

bool UMantleVehicleSource::CanPickEngineSound() const
{
	const bool bEngineMovement = MovementType == EMantleVehicleMovementKind::Wheeled || MovementType == EMantleVehicleMovementKind::Tracked
		|| MovementType == EMantleVehicleMovementKind::Boat;
	return FMantleEngineSounds::CanPick(bEngineMovement, IsFlier(), Tuning.bPedal, Tuning.bElectric);
}

bool UMantleVehicleSource::IsFlier() const
{
	return MovementType == EMantleVehicleMovementKind::Plane || MovementType == EMantleVehicleMovementKind::Helicopter
		|| MovementType == EMantleVehicleMovementKind::Hover;
}

TArray<FName> UMantleVehicleSource::GetEngineSoundOptions() const
{
	TArray<FName> Options;
	if (CanPickEngineSound())
	{
		FMantleEngineSounds::Options(IsFlier(), FMantleEngineSounds::IsTwoWheeler(MovementType == EMantleVehicleMovementKind::Wheeled, Wheels.Num()),
			DriverSeatKind == EMantleVehicleSeatKind::Motorcycle, Options);
	}
	Options.Insert(NAME_None, 0);
	return Options;
}
