// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleBuilderKind.h"
#include "MantleWeaponFit.h"

/** The pure parts of the weapon builder's contact checks (the hands, the sight, the aiming eye), all in the weapon frame, so they run without a mesh. */
namespace MantleWeaponBuilderMath
{
	/** The game puts the palm's centre on Grip_R / Grip_L; a palm is about this big (cm). */
	constexpr double PalmLengthCm = 9.0;
	constexpr double PalmWidthCm = 8.0;
	constexpr double PalmThicknessCm = 3.0;

	/** The wire hand drawn at a hand point (weapon frame, cm): palm capsule radius, finger and thumb radii and segment lengths. */
	constexpr double HandPalmRadiusCm = 1.2;
	constexpr double HandFingerRadiusCm = 0.9;
	constexpr double HandThumbRadiusCm = 1.1;
	constexpr double HandSegment1Cm = 4.5;
	constexpr double HandSegment2Cm = 3.5;

	/** The box is shrunk by this on every side before counting, so a hand resting on the surface is not "inside". */
	constexpr double PalmShrinkCm = 1.0;

	/** More surface samples than this inside the shrunk palm box means the hand is inside the gun. The right hand's limit is higher because a grip legitimately passes through the palm. */
	constexpr int32 LeftPalmInsideLimit = 12;
	constexpr int32 RightPalmInsideLimit = 30;

	/** The sight point is "inside the gun" when it sits lower than the top of the gun within this radius (cm) by more than this (cm). */
	constexpr double SightColumnRadiusCm = 3.0;
	constexpr double SightSinkToleranceCm = 1.0;

	/** An axis-aligned-in-the-weapon-frame palm box (Rotation is kept so a caller may turn it). */
	struct FPalmBox
	{
		FVector Center = FVector::ZeroVector;
		FQuat Rotation = FQuat::Identity;
		FVector HalfExtent = FVector(1.0);
	};

	/**
	 * The palm centred on a grip point. bWrapGrip: the fingers wrap round a grip, so the thin side faces the gun's side (thin along Y,
	 * long along the grip's height, wide along the bore). Otherwise the palm is under the gun, thin side vertical (long along the bore, wide across it).
	 */
	MANTLEEDITOR_API FPalmBox MakePalmBox(const FVector& GripWeapon, bool bWrapGrip);

	/** Which hand MakeHandCapsules draws. */
	enum class EHandStyle : uint8
	{
		/** Right hand on a grip: palm on the grip's side facing -Y, fingers wrapping the front to the far (left) side, index along the bore, thumb round the back. */
		GripRight,
		/** Two-handed left hand: palm under the gun facing up, fingers up the far (right) side, thumb up the near (left) side. */
		SupportUnder,
		/** A pistol's support hand: the under hand rolled to face up and to the right, cupping the shooting hand. */
		SupportCup
	};

	/**
	 * A simple wire hand in the weapon frame: three palm capsules (about 8 x 8 cm, centred on HandPoint) plus four fingers and a
	 * thumb of two capsules each. FarReachCm is how far the gun's far surface is from the hand point across the gun (its half-width
	 * when the hand is on the gun's centre line); each digit curls so its tip ends 0.8 cm beyond it, or as far as it can reach on a wide gun.
	 * bWarning marks every capsule (red). Part names: Palm_n, Finger_n_A/B (n 0 is the index), Thumb_A/B.
	 */
	MANTLEEDITOR_API TArray<FMantleGhostCapsule> MakeHandCapsules(const FVector& HandPoint, EHandStyle Style, double FarReachCm, bool bWarning);

	/** How many samples (mesh space, mapped through SamplesToWeapon) lie inside the box shrunk by ShrinkCm on every side. */
	MANTLEEDITOR_API int32 CountSamplesInsidePalm(const TArray<FVector3f>& Samples, const FTransform& SamplesToWeapon, const FPalmBox& Box, double ShrinkCm = PalmShrinkCm);

	/** A two-handed left palm is inside the gun: the gun cuts through its box. (The right hand
	 *  wraps a grip and a pistol's support hand wraps the other, so neither is checked: on the pack's own grips both read as inside.) */
	MANTLEEDITOR_API bool IsLeftPalmInsideGun(const TArray<FVector3f>& Samples, const FTransform& SamplesToWeapon, const FVector& GripL);

	/** True when the sight sits lower than the highest sample within SightColumnRadiusCm of it (plan view) by more than SightSinkToleranceCm. OutTopZ is that highest sample's height (the sight's own when nothing is near). */
	MANTLEEDITOR_API bool IsSightInsideGun(const TArray<FVector3f>& Samples, const FTransform& SamplesToWeapon, const FVector& SightWeapon, double& OutTopZ);

	/** Where the eye is when aiming: along the bore from the rear of the gun, at the sight's height and side. */
	MANTLEEDITOR_API FVector AimEyeWeapon(double RearX, const FVector& SightWeapon, bool bTwoHanded);

	/** Points covering the mesh surface about SpacingMeshUnits apart (vertices alone when there are no triangles), thinned so there are never more than MaxSamples. */
	MANTLEEDITOR_API void BuildSurfaceSamples(const TArray<FVector3f>& Positions, const TArray<int32>& Triangles, double SpacingMeshUnits, int32 MaxSamples, TArray<FVector3f>& OutSamples);
}

/**
 * The weapon side of the shared builder (#729): the grips, muzzle, sight, magazine and shell ejection a gun needs, a guess
 * for each from FMantleWeaponFit, the Mantle character posed holding the gun, and plain-words checks.
 *
 * Points live in the creator's mesh space, unscaled. The fit works in the weapon frame (scaled, bore along +X, up +Z, origin
 * at the right-hand grip), so every point and ghost capsule is mapped back through the inverse of the fit's MeshToWeapon.
 */
class MANTLEEDITOR_API FMantleWeaponBuilderKind : public IMantleBuilderKind
{
public:
	explicit FMantleWeaponBuilderKind(EMantleWeaponFitHold InHold);

	void SetHold(EMantleWeaponFitHold InHold) { Hold = InHold; }

	/** How Grip_R is placed along the gun; Proportion unless a caller picks another. */
	void SetGripRule(EMantleWeaponGripRule InRule) { GripRule = InRule; }
	EMantleWeaponFitHold GetHold() const { return Hold; }

	/** The creator's Flip and Turn corrections: a turn about the grip in the weapon frame, applied after the fit. Points stay in the mesh's own space. */
	void SetFlipRotation(const FQuat& InFlip) { Flip = InFlip; }

	/** The creator's own size (centimetres per mesh unit) in place of the fit's guess; 0 or less goes back to the guess. */
	void SetScaleOverride(float InScale) { ScaleOverride = InScale; }

	/** The fit from the last GuessPoints, for the panel to show the scale, why, the suggestion and the notes. */
	const FMantleWeaponFitResult& GetLastFit() const { return LastFit; }

	/** Step 4's "Show aiming": poses the ghost aiming and draws the eye and the line of sight. Off by default. */
	void SetShowAiming(bool bInShow) { bShowAiming = bInShow; }
	bool GetShowAiming() const { return bShowAiming; }

	/** The guess from plain vertex positions (mesh space), with optional triangle indices; GuessPoints reads them from the mesh. */
	TArray<FMantleBuilderPoint> GuessPointsFromPositions(const TArray<FVector3f>& Positions, const TArray<int32>& Triangles = TArray<int32>()) const;

	// IMantleBuilderKind
	virtual TArray<FMantleBuilderPointRole> GetRoles() const override;
	virtual TArray<FMantleBuilderPoint> GuessPoints(const UObject* Mesh) const override;
	virtual TArray<FMantleGhostCapsule> GhostPose(const TArray<FMantleBuilderPoint>& Points) const override;
	virtual FQuat ViewTurn() const override;
	virtual TArray<FText> Check(const TArray<FMantleBuilderPoint>& Points) const override;
	virtual FMantleBuilderOverlay Overlay(const TArray<FMantleBuilderPoint>& Points) const override;

private:
	/** What the model's geometry says about the hands and the sight; cached until a point, the fit or the hold changes. */
	struct FContactCache
	{
		bool bValid = false;
		FTransform MeshToWeapon;
		EMantleWeaponFitHold Hold = EMantleWeaponFitHold::Rifle;
		TOptional<FVector> HandR, HandL, Sight;
		bool bRightInside = false;
		bool bLeftInside = false;
		bool bSightInside = false;
		/** Distance from each hand point across the gun to its far surface (cm, weapon frame), for the wire hands. */
		double FarReachR = 2.0;
		double FarReachL = 3.0;
	};

	const FContactCache& Contacts(const TArray<FMantleBuilderPoint>& Points) const;

	/** The eye's place in the weapon frame when aiming, or unset without a Sight point. */
	TOptional<FVector> AimEye(const TArray<FMantleBuilderPoint>& Points) const;
	/** The middle of the gun's height at its back end (weapon frame), from the cached surface samples; unset before any. */
	TOptional<double> ButtCentreZ(double RearXCm) const;

	EMantleWeaponFitHold Hold;
	// Proportion since #729: on the seven held-out pack guns its median grip error is 9.3 cm against 17.6 cm for Protrusion
	// (Mantle.Weapon.AutoFitAccuracy.AllPackGuns); the fit's own default stays Protrusion for its geometry tests.
	EMantleWeaponGripRule GripRule = EMantleWeaponGripRule::Proportion;
	FQuat Flip = FQuat::Identity;
	float ScaleOverride = 0.f;
	mutable FMantleWeaponFitResult LastFit;
	bool bShowAiming = false;
	/** The model's surface, sampled about 1 cm apart, from the last GuessPoints (cached per model). */
	mutable TArray<FVector3f> Samples;
	mutable FContactCache Cache;
};
