// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UObject;

/**
 * #625, the default height (owner, 2026-10-02): every avatar comes out at ONE standing height, the driver's (limits.json
 * characterScale.driverStandingHeightCm, 165.6 cm), with a uniform scale that keeps its proportions; the converter never
 * refuses for size. On the Convert route, as the proven scripts do it:
 *   03 (make the posed static mesh): the posed mesh scaled by k = target / (its height + ConformAddsCm) about the
 *      origin, k stored on it as InputScaleTag (and on the work, FMantleConvertMetaHumanWork::InputScale);
 *   04 (conform): the portrait's head-joint hint scaled by the same k;
 *   05 (build): the built body measured with its face; outside target +- ToleranceCm is a warning
 *      line, never a stop. Before that warning (#751), a build outside it is redone once: the posed mesh rescaled by
 *      target / built (k with it), conformed again and rebuilt (MaxConvertHeightBuilds).
 * On the preset route (a MetaHuman Creator character as the source) the Height body constraint is not exact, so the build is
 * measured and, when further than AimCm from the target, rebuilt with a corrected constraint (RetargetCm), at most
 * MaxHeightBuilds builds in all.
 */
class MANTLEEDITOR_API FMantleAvatarHeight
{
public:
	/** The metadata tag the posed mesh carries k in (the scripts' mh.INPUT_SCALE_TAG). */
	static const FName InputScaleTag;

	/** What the conform adds to the posed mesh's height (cm): k aims the conformed body, not the posed mesh, at the target. */
	static constexpr double ConformAddsCm = 0.5;

	/** A built body within this of the target (cm) is at the default height; further is a warning, never a stop. */
	static constexpr double ToleranceCm = 0.5;

	/**
	 * A CONVERTED avatar (the Convert route) must land within this of the target (cm). It matches the backend, which refuses a
	 * converted body further than 0.5 cm from 165.6 cm at publish (publication_mode.go), so what passes here also publishes.
	 */
	static constexpr double ConvertedToleranceCm = 0.5;

	/** A build further than this from the target (cm) is rebuilt with a corrected Height constraint (tighter than ToleranceCm, so passes are centred, not just inside the edge). */
	static constexpr double AimCm = 0.15;

	/** The most builds of one preset character for its height: the first build plus at most two corrected rebuilds. */
	static constexpr int32 MaxHeightBuilds = 3;

	/**
	 * #751: the most builds of one converted body for its height: the first build plus one after a rebuild with the posed
	 * body rescaled by target / built and conformed again, when the first is outside ToleranceCm (each costs a conform).
	 */
	static constexpr int32 MaxConvertHeightBuilds = 2;

	/** A pre-build measurement further than this from the target (cm) is not trusted (it may not cover the whole body and head): the build proceeds on the plain request, as before. */
	static constexpr double PredictionPlausibleCm = 3.0;

	/** The most corrections of the Height constraint made on Creator's evaluated body before the first build. */
	static constexpr int32 MaxPredictionSteps = 4;

	/** What the build adds to the pre-build measurement (built minus predicted, cm); 0 until calibrated from the MANTLE_CONVERT_HEIGHT_PREDICTION log lines. */
	static constexpr double PredictionOffsetCm = 0.0;

	/** The default height: the plugin's contract's driver standing height, else the rule's default (FMantleCharacterScaleRule). */
	static double TargetCm();

	/** Highest Z minus lowest Z of Points (the scripts' z_height); 0 for none. Pure (tested). */
	static double ZHeight(TConstArrayView<FVector3f> Points);

	/** Lowest Z of Points (the mesh's lowest point, which ZHeight can't see); 0 for none. Pure (tested). */
	static double LowestZ(TConstArrayView<FVector3f> Points);

	/** Whether a mesh whose lowest Z is LowestCm stands on the floor: |LowestCm| within InToleranceCm (the upload's feet check). Pure (tested). */
	static bool FeetOnFloor(double LowestCm, double InToleranceCm);

	/** k = TargetCm / (PosedHeightCm + ConformAddsCm), as the script; 1 when either is not a positive number. Pure (tested). */
	static double ConformInputScale(double PosedHeightCm, double TargetCm);

	/** Whether BuiltCm is within ToleranceCm of TargetCm (an unmeasured, negative height is not). Pure (tested). */
	static bool IsWithin(double BuiltCm, double TargetCm, double InToleranceCm = ToleranceCm);

	/** Whether a build at BuiltCm is to be redone: BuiltCm is a positive number more than AimCm from TargetCm. Pure (tested). */
	static bool NeedsRetarget(double BuiltCm, double TargetCm);

	/**
	 * The Height constraint to ask for next: the constraint asked for RequestedCm and produced BuiltCm, so the proportional
	 * correction is RequestedCm * TargetCm / BuiltCm. RequestedCm unchanged when any input is not a positive number. Pure (tested).
	 */
	static double RetargetCm(double RequestedCm, double BuiltCm, double TargetCm);

	/** Whether a pre-build measurement is to be trusted: PredictedCm and TargetCm are finite, PredictedCm is positive and within PredictionPlausibleCm of TargetCm. Pure (tested). */
	static bool IsPlausiblePrediction(double PredictedCm, double TargetCm);

	/**
	 * The script's line for a built height: "BUILT HEIGHT 165.65 cm (target 165.6)" within ToleranceCm, else "HEIGHT
	 * WARNING: built 175.95 cm, target 165.6 +- 0.5" (or "HEIGHT WARNING: the built height was not measured, target ...").
	 * A line, never a stop. Pure (tested).
	 */
	static FString BuiltHeightLine(double BuiltCm, double TargetCm, double InToleranceCm = ToleranceCm);

	/** Writes K as InputScaleTag on Asset, at full precision (the script's repr(k)). */
	static void WriteInputScale(const UObject& Asset, double K);
};
