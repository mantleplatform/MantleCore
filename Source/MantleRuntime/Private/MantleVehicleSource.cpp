// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleSource.h"

#include "MantleEngineSounds.h"

bool UMantleVehicleSource::CanPickEngineSound() const
{
	const bool bWheeledOrTracked = MovementType == EMantleVehicleMovementKind::Wheeled || MovementType == EMantleVehicleMovementKind::Tracked;
	return FMantleEngineSounds::CanPick(bWheeledOrTracked, Tuning.bPedal, Tuning.bElectric);
}

TArray<FName> UMantleVehicleSource::GetEngineSoundOptions() const
{
	TArray<FName> Options;
	if (CanPickEngineSound())
	{
		FMantleEngineSounds::Options(Wheels.Num(), DriverSeatKind == EMantleVehicleSeatKind::Motorcycle, Options);
	}
	Options.Insert(NAME_None, 0);
	return Options;
}
