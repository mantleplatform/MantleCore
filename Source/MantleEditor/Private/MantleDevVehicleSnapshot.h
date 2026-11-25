// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UMantleVehicleSource;

/**
 * A built vehicle written down as plain, stable JSON (Mantle.Dev.VehicleSnapshot), so two builds of the same car can be compared line by line: the
 * standing car check converts a fixed set of real cars fresh and diffs each snapshot against the last good one.
 *
 * It holds every property the vehicle source declares (its wheels with their bones, rim sections and rim radius; paint sections, layers and liveries;
 * the lights lists; linkage, so the hubs; parts; tuning, so the seats, steering wheel, contacts and hinges), each under its C++ name, plus what the
 * body says: Sections (each section's name, material and the built-in material it publishes with, empty for its own) and Bones. A property added later
 * appears by itself. The library card picture is left out (it depends on the GPU).
 *
 * Stable: object keys sorted; lists of names (section lists such as Lights.Head or Paint.Sections) sorted, as their order means nothing; liveries and
 * their names keep their order (a livery is picked by its index); numbers rounded to 3 decimals and written without trailing zeros.
 */
struct FMantleDevVehicleSnapshot
{
	static TSharedRef<FJsonObject> Make(const UMantleVehicleSource& Source);

	/** Make, written as indented JSON text. */
	static FString ToText(const UMantleVehicleSource& Source);

	/** A number as the snapshot writes it: 3 decimals at most, no trailing zeros, never "-0". */
	static FString Number(double Value);
};
