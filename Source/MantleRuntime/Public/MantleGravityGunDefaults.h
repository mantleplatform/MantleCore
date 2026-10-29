// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * Round 3 step 16 (Gravity Gun, GitHub issue 139 part 1): the document's gravity block defaults, and the limits of its fields. The one place
 * they are written: the document (FMantleWeaponDocGravity), the weapon definition (FMantleGravitySettings) and the gun read them from here, so
 * changing a default is a one-line edit. The schema's defaults and limits (weapon.schema.json gravity) must match; Mantle.Weapon.Round3.Gravity
 * reads both.
 */
namespace MantleGravityGunDefaults
{
	/** How far the crosshair reaches for a prop, cm. */
	inline constexpr double RangeCm = 2000.0;
	inline constexpr double RangeMinCm = 100.0;
	inline constexpr double RangeMaxCm = 10000.0;
	/** The heaviest prop it grabs, kg. */
	inline constexpr double MaxMassKg = 300.0;
	inline constexpr double MaxMassMinKg = 1.0;
	inline constexpr double MaxMassMaxKg = 5000.0;
	/** A grabbed prop drops by itself after this long, s; 0 holds it until the player lets go. */
	inline constexpr double HoldSeconds = 0.0;
	inline constexpr double HoldSecondsMax = 300.0;
	/** The held prop's centre is this far in front of the muzzle, along the aim, cm. */
	inline constexpr double HoldDistanceCm = 150.0;
	inline constexpr double HoldDistanceMinCm = 50.0;
	inline constexpr double HoldDistanceMaxCm = 600.0;
	/** A launch leaves at this speed along the crosshair, cm/s. */
	inline constexpr double LaunchSpeedCmS = 2500.0;
	inline constexpr double LaunchSpeedMinCmS = 100.0;
	inline constexpr double LaunchSpeedMaxCmS = 10000.0;
	/** What a launched prop's hit does to a player or NPC it strikes (0: nothing). */
	inline constexpr double LaunchDamage = 25.0;
	inline constexpr double LaunchDamageMax = 1000.0;
	/** Seconds after the prop is let go (launched, dropped or lost) before the gun grabs again. */
	inline constexpr double CooldownSeconds = 0.5;
	inline constexpr double CooldownSecondsMax = 60.0;
}
