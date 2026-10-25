// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UMantleVehicleSource;
class USkeletalMesh;
struct FMantleVehiclePreset;

/** One wheel as the fit sees it (#728): where its centre is in the vehicle's space, and how big it is. Centimetres, +X forward, +Y right. */
struct FMantleVehicleFitWheel
{
	FVector Centre = FVector::ZeroVector;
	/** The wheel's radius; 0 when it is not known (the wheel then counts towards the track only). */
	double Radius = 0.0;
};

/** What sizing a creator's vehicle for the Mantle character found (#728). */
struct FMantleVehicleFit
{
	/** The uniform size to apply to the whole vehicle: 1 is as modelled. Rounded to hundredths; 1 when it is already close. */
	double Scale = 1.0;
	/** The size the wheel radius alone asks for, and the one the track width alone asks for; 0 when that signal is not available. */
	double RadiusScale = 0.0;
	double TrackScale = 0.0;
	/** The averages the two came from (0 when there were no wheels or no axle to measure). */
	double MeanRadius = 0.0;
	double MeanTrack = 0.0;
	/** True when the preset's smallest or largest size held the result back. */
	bool bClamped = false;
	/** True when the wheel radius and the track width disagree by more than a fifth. */
	bool bSignalsDisagree = false;
	/** One or two plain sentences for the creator saying what was done and why. */
	FString Reason;
};

/**
 * Sizes a creator's mesh for the 165.6 cm Mantle character (#728). One uniform scale per vehicle, from the parts that are sized for
 * roads and people: the wheel radius and the track width (wheel centre to wheel centre across an axle), compared with the
 * preset's typical values. Never from the wheelbase, so a stretched limousine keeps its length.
 */
struct MANTLEEDITOR_API FMantleVehicleFitter
{
	/** The fit of these wheels for this preset. Pure: nothing is read or changed outside the arguments. */
	static FMantleVehicleFit Compute(const FMantleVehiclePreset& Preset, TConstArrayView<FMantleVehicleFitWheel> Wheels);

	/**
	 * The Size of a model that has no wheels to measure (#821: a spaceship, a plane, a helicopter): how much ground its footprint covers (its length by its
	 * width, in the units it was modelled in, nose along +X) against the chassis's at its own size. The result is the uniform Size the chassis is scaled by
	 * (FMantleChassisSpec::Scaled), the square root of the footprints' ratio, held to the preset's fit range and rounded to hundredths. Pure.
	 */
	static FMantleVehicleFit ComputeFromFootprint(const FMantleVehiclePreset& Preset, double ModelLength, double ModelWidth, double ChassisLength, double ChassisWidth);

	/**
	 * The wheels of Source as Mesh holds them in its reference pose, one per entry of Source.Wheels whose bone is found. The radius is
	 * the entry's own Radius when it is set; otherwise it is measured from the body's geometry weighted to the wheel's bone, the
	 * way Convert Vehicle measures it (FMantleVehicleConverter::ReadBody and MeasureWheel), and left 0 when that cannot be read.
	 * False, with Out empty, when none of the wheels' bones is in the mesh.
	 */
	static bool WheelsFromMesh(const USkeletalMesh& Mesh, const UMantleVehicleSource& Source, TArray<FMantleVehicleFitWheel>& Out);
};
