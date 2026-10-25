// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/** The gun holds the fit knows a typical size for (#729). */
enum class EMantleWeaponFitHold : uint8
{
	Pistol,
	Revolver,
	Rifle,
	Smg,
	Dmr,
	PumpShotgun,
	BoltSniper,
	Lmg,
	Melee1h, Melee2h, Spear, Staff, Bow, Unarmed, Shield, Throwable, Consumable,
	Count
};

/** How the right-hand grip is placed along the gun (#729). */
enum class EMantleWeaponGripRule : uint8
{
	/** The rearmost narrow protrusion hanging below the body in the rear 60%. */
	Protrusion,

	/** A typical fraction of the length from the rear, then down to the underside of the mesh there. */
	Proportion
};

struct FMantleWeaponFitInput
{
	/** The mesh's vertices, in mesh space. */
	TArray<FVector3f> Positions;

	/** Optional triangle indices into Positions. When given, the surface between sparse vertices is sampled too, so a low-poly mesh measures the same as a dense one. */
	TArray<int32> Triangles;

	EMantleWeaponFitHold Hold = EMantleWeaponFitHold::Rifle;

	EMantleWeaponGripRule GripRule = EMantleWeaponGripRule::Protrusion;

	/** How far below the middle of the handguard a two-handed left hand's palm sits, in cm (the builder uses
	 *  FMantleWeaponFit::LeftHandBelowHandguardCm; 0 is the middle). */
	double LeftHandBelowCentreCm = 0.0;

	/** A blade (melee_1h, melee_2h): the creator's Flip, the handle at the other end from the one the fit finds. */
	bool bReverseLength = false;
};

struct FMantleWeaponFitResult
{
	/** Mesh space to the weapon frame: bore along +X (muzzle at +X), up +Z, origin at the right-hand grip guess. A proper rotation plus the uniform Scale. */
	FTransform MeshToWeapon = FTransform::Identity;

	/** Uniform, centimetres per mesh unit; already inside MeshToWeapon. */
	float Scale = 1.f;

	/** Why the scale is what it is, in plain words. */
	FString ScaleReason;

	/** The mesh length is outside the hold's range (after any unit fix). Never a refusal: the fit still returns its guesses. */
	bool bOutsideTypicalRange = false;

	/** What to try instead when bOutsideTypicalRange, e.g. "Try the SMG or rifle preset." */
	FString Suggestion;

	/** Weapon-frame guesses: Grip_R (the origin), Grip_L (two-handed holds), Muzzle, Sight, Magazine (only when one is found). Rotations are identity; the hold decides the hands' orientation. */
	TMap<FName, FTransform> Points;

	/** Measurement for tests and tuning: the gun's length along the bore in centimetres (after Scale). */
	float LengthCm = 0.f;

	/** Measurement for tests and tuning: how far the Grip_R guess (the weapon-frame origin) is ahead of the rear of the mesh, in centimetres. */
	float GripFromRearCm = 0.f;

	/** Plain-language notes for the panel: the guesses the creator should check. */
	TArray<FString> Notes;
};

/**
 * Pure fitting math for a gun mesh (#729): from plain vertex positions, a confident guess at which way the mesh points,
 * how big it should be, and where the grips, muzzle, sight and magazine are. It makes no assets and touches no UObject, so
 * the creator's panel can show the result and let the gizmo override it.
 *
 * Orientation: the principal axis of the vertices is the bore line, refined by a small sweep so the box around the gun is
 * as tight as possible (a grip below the rear tilts the raw principal axis). Of the other two axes the taller one is up,
 * signed so the side with more mass away from the bore line (grip and magazine) is down. The end whose cross-section is
 * smaller is the muzzle.
 */
struct MANTLEEDITOR_API FMantleWeaponFit
{
	static FMantleWeaponFitResult Fit(const FMantleWeaponFitInput& In);

	/** Where the builder guesses a two-handed left palm: this far below the middle of the handguard, in cm. The pack TR15's
	 *  (#729, Mantle.Weapon.AutoFitAccuracy.LeftHandBelowCentre); a fraction of the handguard's height failed on other guns. */
	static constexpr double LeftHandBelowHandguardCm = 6.9;

	/** A blade's Grip_R guess (the middle of the palm) sits this far from the handle's centre line, toward the weapon frame's
	 *  -Z: where the palm rests when the melee holds' clips close the fist on the handle (#840, the slices of the in-game fist). */
	static constexpr double BladePalmFromHandleCm = 2.0;

	/** A throwable is "handled" (a knife, a hatchet: held by the handle end, like a blade) when its long axis is at least this many times
	 *  the larger of the other two; a grenade or a ball is compact and is held at its middle (#840). */
	static constexpr double ElongatedThrowableRatio = 2.5;

	/** A shield's Grip guess (the middle of the left palm) sits this far behind the back of the shield, at its middle: the
	 *  SwordShield pack's palm is 0.5 to 2.4 cm behind its shield's back in the idle and the guard (#840 step 2). */
	static constexpr double ShieldPalmBehindCm = 1.5;

	/** A bow's Grip_L guess (the middle of the left palm, #840 step 4c), from the BowAndArrow pack's bow (135 cm limbs, 27.3 cm deep, riser
	 *  3.6 cm thick) and the pack's left palm on its clips (palm_l in the bow bone's frame, stable on every clip): the palm sits 3.8 cm
	 *  behind the riser's front face, 2.6 cm past the riser's hand-side face (the fingers wrap the grip, 4.4 cm off the riser's middle)
	 *  and 7.5 cm below the middle of the limbs (5.6 % of the length). The riser is the middle RiserHalfShare of the length each way. */
	static constexpr double BowPalmBehindFrontCm = 3.8;
	static constexpr double BowPalmBeyondRiserCm = 2.6;
	static constexpr double BowGripBelowMiddleShare = 0.056;
	static constexpr double BowRiserHalfShare = 0.075;

	/** Where a bow's string and nock sit, as shares of its depth from the limb tips (the pack's tips curve 2.4 cm past the string plane,
	 *  the nock bone 2.9 cm: of 27.3 cm); the nock a little above the middle (2.0 cm of 135) and on the riser's face the arrow lies along
	 *  (the pack's nock is 1.5 cm to that side of the riser's middle, its face 1.8 cm). */
	static constexpr double BowStringPlaneShare = 0.088;
	static constexpr double BowNockPlaneShare = 0.106;
	static constexpr double BowNockAboveMiddleShare = 0.015;
	static constexpr double BowNockInsideRiserCm = 0.3;

	/** A staff's head (#840 step 4d): the end of its length with the larger cross-section, compared over the outer share of the length at each end
	 *  (an orb, a crystal or a curl is bulkier than the butt). The head is the front (+X). */
	static constexpr double StaffEndShare = 0.25;

	/** A staff's Grip_R guess (the middle of the right palm) sits this far from the butt, as a share of its length, on the shaft: the MagicStaff
	 *  pack's palm is 2.7 cm below the shaft's middle point, 0.35 of the length from the head (staff-plan 2: the shaft runs -119 to 60.3 cm,
	 *  the palm at -2.7). The left hand never grips a staff, so Grip_L is only kept where the document needs it: this share of the length
	 *  back toward the butt (the TestStaff fixture's 30 cm of 179). */
	static constexpr double StaffGripFromButtShare = 0.65;
	static constexpr double StaffLeftHandBehindShare = 0.167;

	/** The staff's Focus (where the beam and casts leave) is the head's top: the very end of the length, in the middle of the outer share of it. */
	static constexpr double StaffFocusRegionShare = 0.10;
	/** The Grip_R's sideways position is the shaft's middle at the palm: the middle of the mesh within this share of the length each way. */
	static constexpr double StaffShaftSampleShare = 0.05;
};
