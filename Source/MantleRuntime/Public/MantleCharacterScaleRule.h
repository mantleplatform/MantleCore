// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * The one formula for how big an avatar plays (schema limits.json
 * characterScale), shared by the game (FMantleCharacterScale, the server's authoritative scale and the Avatar
 * screen's preview) and the creator plugin ("Plays at X cm"). Pure numbers: no metrics lookup, no world.
 *
 *   playing height = clamp(standingHeightCm x heightFraction, MinStandingHeightCm, MaxStandingHeightCm)
 *   s              = playing height / DriverStandingHeightCm
 *
 * An avatar without a standing height (no metrics) stands as tall as the driver, so it plays at heightFraction.
 * The defaults are limits.json characterScale's; the plugin reads its own copy of limits.json into one of these
 * (FMantleAvatarContract::CharacterScale), the game mirrors them as constants.
 */
struct FMantleCharacterScaleRule
{
	/** SKM_UEFN_Mannequin's top in its reference pose, cm. */
	static constexpr double DefaultDriverStandingHeightCm = 165.6;
	/** The clamp on the height a character plays at, cm. */
	static constexpr double DefaultMinStandingHeightCm = 150.0;
	static constexpr double DefaultMaxStandingHeightCm = 210.0;

	double DriverStandingHeightCm = DefaultDriverStandingHeightCm;
	double MinStandingHeightCm = DefaultMinStandingHeightCm;
	double MaxStandingHeightCm = DefaultMaxStandingHeightCm;

	/** A usable standing height: finite and above 0 (anything else is "no metrics"). */
	static bool IsUsableHeight(const double StandingHeightCm)
	{
		return FMath::IsFinite(StandingHeightCm) && StandingHeightCm > 0.0;
	}

	/** The rule's own numbers make sense: a positive driver and a non-empty clamp. */
	bool IsValid() const
	{
		return IsUsableHeight(DriverStandingHeightCm) && IsUsableHeight(MinStandingHeightCm) && FMath::IsFinite(MaxStandingHeightCm)
			&& MaxStandingHeightCm >= MinStandingHeightCm;
	}

	/** The height an avatar StandingHeightCm tall (unusable: the driver's) plays at, at HeightFraction (1 = 100%), cm. */
	double PlayingHeightCm(const double StandingHeightCm, const double HeightFraction) const
	{
		const double Standing = IsUsableHeight(StandingHeightCm) ? StandingHeightCm : DriverStandingHeightCm;
		return FMath::Clamp(Standing * HeightFraction, MinStandingHeightCm, MaxStandingHeightCm);
	}

	/** The playing height would be outside the clamp without it (a body the character scale cannot follow all the way). */
	bool IsClamped(const double StandingHeightCm, const double HeightFraction) const
	{
		const double Standing = IsUsableHeight(StandingHeightCm) ? StandingHeightCm : DriverStandingHeightCm;
		const double Wanted = Standing * HeightFraction;
		return Wanted < MinStandingHeightCm || Wanted > MaxStandingHeightCm;
	}

	/** s: the character's scale for that avatar and height setting. */
	double Scale(const double StandingHeightCm, const double HeightFraction) const
	{
		return PlayingHeightCm(StandingHeightCm, HeightFraction) / DriverStandingHeightCm;
	}
};
