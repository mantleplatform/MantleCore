// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleVehicleDifferential.generated.h"

/**
 * An axle's differential (#376 off-road; transmission.differentials in the vehicle format). Chaos has none: its transmission gives every wheel an equal
 * share (TorqueSimModule.cpp:40), which is None here and every vehicle before it.
 */
UENUM(BlueprintType)
enum class EMantleVehicleDifferential : uint8
{
	/** Mantle's equal split: each wheel keeps its own share, nothing moves across the axle. */
	None,
	/** A real open differential: both wheels push the same, at most what the weaker one can take. */
	Open,
	/** The gripping wheel may push up to the bias ratio times the other's. */
	LimitedSlip,
	/** Always locked (a spool or welded diff): the axle's drive goes to whichever wheel can use it. */
	Locked,
	/** Open until the driver locks it (the Driveline input). */
	Selectable,
};

/**
 * Which axles a vehicle built from a type drives (#376 off-road follow-on, the builder's drive choice). Type is 0 so a source saved before it loads
 * unchanged: the type's own driven axles (chassis.layout.axles[].drive).
 */
UENUM(BlueprintType)
enum class EMantleVehicleDrive : uint8
{
	/** The type's own driven axles. */
	Type,
	/** Front-wheel drive: the frontmost axle drives, the others roll. */
	Front,
	/** Rear-wheel drive: every axle but the front one drives (a six-wheeler's tandem both drive). */
	Rear,
	/** All-wheel drive: every axle drives. */
	All,
};

/** The vehicle format's name for a differential type ("open", "limitedSlip", "locked", "selectable"); empty for None. */
MANTLERUNTIME_API FString MantleDifferentialName(EMantleVehicleDifferential Type);
/** The type a vehicle format name stands for; None for anything else (empty included). */
MANTLERUNTIME_API EMantleVehicleDifferential MantleDifferentialFromName(const FString& Name);
