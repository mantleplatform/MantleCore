// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

struct FMantleGlbMesh;

/**
 * The vehicle builder's rims (vehicle-customization-plan.md section 3): which sections of a car's body are the rims a wheel swap hides.
 *
 * A section is a rim when its name reads as one (rim, wheel, alloy, hub, spoke; never tire, tyre, rubber, brake, disc or caliper) and its
 * geometry agrees: nearly all of its vertices (RimShare) are weighted mostly to a wheel's bone and sit within that wheel's radius around the
 * bone. A GTA car's `[WHEEL]` material is one section for all four wheels, so a section belongs to every wheel that owns some of it. A section
 * that mixes the wheel's triangles with the rest of the body (brake discs inside the body's own material) is left alone: hiding it would hide
 * the car. A car with no such section (the wheels are welded into the body) gets none, and its wheels simply cannot be swapped.
 */
struct MANTLEEDITOR_API FMantleVehicleRims
{
	/** The share of a section's vertices that must be a wheel's own and inside its radius. */
	static constexpr float RimShare = 0.9f;

	/** How far past the wheel's radius (as a multiple) a rim vertex may sit from the bone: the rim is inside the tire, the margin covers a flared lip. */
	static constexpr double RadiusMargin = 1.15;

	/** Whether a section's name reads as a rim (and not as a tire or a brake). */
	static bool NameIsRim(const FString& Section);

	/**
	 * Per wheel (by Bones), the names of the body sections that are its rim. SectionNames is the body's unique section names by
	 * material index (FMantleVehicleConverter::UniqueSectionNames); Radii the wheels' radii, cm (0: not known, the section's geometry
	 * is then only checked to be the wheel's own). OutNotes says what was found, and why a section named like a rim was left. OutRimRadii, when given, gets
	 * per wheel how far its rim reaches from the bone (the farthest vertex of its rim sections, cm; 0 for a wheel with none).
	 */
	static TArray<TArray<FString>> Find(const FMantleGlbMesh& Body, TConstArrayView<FName> Bones, TConstArrayView<double> Radii,
		const TArray<FString>& SectionNames, TArray<FString>& OutNotes, TArray<double>* OutRimRadii = nullptr);
};
