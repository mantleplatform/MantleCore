// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/** One part of the stand-in body the builders draw over a creator's mesh: a capsule from Start to End, in character space. */
struct FMantleGhostCapsule
{
	FName Part;
	FVector Start = FVector::ZeroVector;
	FVector End = FVector::ZeroVector;
	float RadiusCm = 4.f;

	/** Drawn in the warning colour, e.g. a hand that cannot reach its point. */
	bool bWarning = false;
};

/**
 * The Mantle character's body sizes in centimetres, from standard anthropometric ratios of its height, so a builder can show a
 * creator the real character against their mesh (no mesh, no licensed content: just numbers and capsules). Used by both the
 * weapon and the vehicle builder.
 *
 * Character space: the root at the feet, +X forward, +Z up, +Y the character's right. Ratios of height H:
 *   eye 0.936, shoulder 0.818, shoulder width 0.259, upper arm 0.186, forearm 0.146, hand length 0.108, hand width 0.045,
 *   hip joint 0.530, hip width 0.191, thigh 0.245, shin 0.246, foot length 0.152;
 *   seated, above the seat surface: eye 0.445, hip joint 0.05.
 */
struct MANTLERUNTIME_API FMantleBodyMeasurements
{
	/** The Mantle character's height (cm); the editor test pins it to FMantleAvatarHeight::TargetCm(). */
	static constexpr float HeightCm = 165.6f;

	static float EyeHeightCm();
	static float ShoulderHeightCm();
	static float ShoulderWidthCm();
	static float UpperArmCm();
	static float ForearmCm();
	static float HandLengthCm();
	static float HandWidthCm();
	static float HipHeightCm();
	static float HipWidthCm();
	static float ThighCm();
	static float ShinCm();
	static float FootLengthCm();
	static float SeatedEyeHeightCm();

	/** The hip joint's height above the seat surface when seated. */
	static float SeatedHipHeightCm();

	/**
	 * Two-bone limb solve (law of cosines). Mid is the elbow or knee, on the plane of Root, Target and PoleDir, bent toward PoleDir.
	 * Beyond reach (or too close for the bones to fold), the limb points straight at the target, End is clamped and bOutReached
	 * is false.
	 */
	static void SolveLimb(const FVector& Root, const FVector& Target, float UpperLen, float LowerLen, const FVector& PoleDir, FVector& OutMid, FVector& OutEnd, bool& bOutReached);

	/** Standing, arms down: head, torso, pelvis, upper and lower arms, hands, thighs, shins and feet. */
	static TArray<FMantleGhostCapsule> StandingGhost();

	/** Same space with the origin at the seat point: thighs forward, shins down, forearms forward in the lap. */
	static TArray<FMantleGhostCapsule> SeatedGhost();
};
