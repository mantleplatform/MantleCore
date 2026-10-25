// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehicleFit.h"

class UMantleVehicleSource;

/** Where a creator's model has its wheels, in the model's own units (#728 step 2). The only thing the builder reads from the model's body. */
struct FMantleChassisLayout
{
	bool bValid = false;
	/** One wheel at the front and one at the rear, in a line down the middle (a motorcycle): there is no track, and Track stays 0. */
	bool bCentreline = false;
	/** The front and rear axles' X; the front is the larger. */
	double FrontX = 0.0;
	double RearX = 0.0;
	/** The middle of the wheels across the car. */
	double CentreY = 0.0;
	/** Wheel centres' height. */
	double CentreZ = 0.0;
	/** Wheel centre to wheel centre across an axle. */
	double Track = 0.0;
	/** The wheels' radius; 0 when it could not be read, and the ground is then taken to be Z = 0. */
	double Radius = 0.0;

	double Wheelbase() const { return FrontX - RearX; }
	/** The wheel radius the fit uses: the measured one, or the wheel centres' height when it is not known. */
	double FitRadius() const { return Radius > 0.0 ? Radius : CentreZ; }
};

/** How a preset's chassis is stretched to a model's wheels, and where it sits on the model (#728 step 2). */
struct FMantleChassisStretch
{
	/** Longer, wider, taller, each held to the band below. */
	double Kx = 1.0;
	double Ky = 1.0;
	double Kz = 1.0;
	bool bClampedX = false;
	bool bClampedY = false;
	bool bClampedZ = false;
	/** The vehicle's Size: every length written to the model is divided by it, because the export multiplies by it. */
	double Size = 1.0;
	/** The chassis origin on the model, in model units: the middle of the axles, on the ground, under the middle of the track. */
	FVector Origin = FVector::ZeroVector;

	/** A point in chassis space (cm, origin mid-wheelbase on the ground) as the model's. */
	FVector PointToModel(const FVector& ChassisPoint) const;
	/** A half-size or other offset in chassis space as the model's. */
	FVector ExtentToModel(const FVector& ChassisExtent) const;
	/** True when any stretch differs from 1 by more than a fiftieth. */
	bool IsStretched() const;
};

/** One collision box, in model units: its centre and half size. */
struct FMantleChassisBox
{
	FVector Center = FVector::ZeroVector;
	FVector Extent = FVector::ZeroVector;
};

/** Every physical value of the chassis as written onto the model's vehicle, in model units (what the export multiplies by Size). */
struct FMantleChassisPlacement
{
	FVector CollisionCenter = FVector::ZeroVector;
	FVector CollisionExtent = FVector::ZeroVector;
	/** Boxes above the tub that follow the model's visible body (FitBodyBoxes); empty until the builder fits them. Written after the tub. */
	TArray<FMantleChassisBox> BodyBoxes;
	FVector CenterOfMass = FVector::ZeroVector;
	FVector Seat = FVector::ZeroVector;
	/** The seat has a steering wheel; a rider's (a motorcycle's) has handlebars instead, which a movement block carries, and a pilot's the stick and throttle. */
	bool bSteeringWheel = true;
	FVector SteeringWheelHub = FVector::ZeroVector;
	double SteeringWheelRadius = 0.0;
	double SteeringWheelPitch = 0.0;
	bool bPassenger = false;
	FVector PassengerSeat = FVector::ZeroVector;
};

/**
 * A creator's model as a skin on a preset's chassis (#728 step 2). Everything here is pure arithmetic on numbers, so it is tested
 * without a mesh. The model gives only its wheel layout; every physical value comes from the chassis, stretched to that layout.
 */
struct MANTLEEDITOR_API FMantleVehicleChassisFit
{
	/** The smallest and largest stretch of any one direction. */
	static constexpr double MinStretch = 0.6;
	static constexpr double MaxStretch = 2.5;

	/**
	 * The wheel layout of these wheels (model units). Invalid with fewer than two wheels or no length between the axles. Wheels that all sit on the model's
	 * centre line (a motorcycle's two) are a centreline layout: no track, so a bike is not refused for having none (#821).
	 */
	static FMantleChassisLayout LayoutFromWheels(TConstArrayView<FMantleVehicleFitWheel> Wheels);

	/** The chassis stretched to Layout at the vehicle's Size (Size 0 or less counts as 1). */
	static FMantleChassisStretch Stretch(const FMantleVehicleChassisReference& Chassis, const FMantleChassisLayout& Layout, double Size);

	/** The chassis's parts as the model's. */
	static FMantleChassisPlacement Place(const FMantleVehicleChassisReference& Chassis, const FMantleChassisStretch& Stretch);

	/**
	 * Up to MaxBodyBoxes boxes covering the model's visible body above the chassis tub (#728), so the roof and cabin are solid too.
	 * BodyPoints are the model's body vertices (those not weighted to a wheel), in model units; the tub is the chassis box already
	 * placed. Only points above the tub's top count, and a box starts exactly at the tub's top, so the boxes never reach the wheels.
	 * The points above the tub are cut at the middle of their X range into a front half and a rear half, each a box to its own roof
	 * (a cab-over truck is a tall cab and a low bed, a pickup a cab and a low bed rail); halves of nearly the same height are one box.
	 * A stray point (an aerial, a mirror) is trimmed by a percentile. Empty when there is too little above the tub to matter.
	 * Every box is also kept inside the body's X and Y extents (BodyExtentXY), so none sticks out past the visible body.
	 */
	static TArray<FMantleChassisBox> FitBodyBoxes(TConstArrayView<FVector> BodyPoints, const FVector& TubCenter, const FVector& TubExtent);

	/**
	 * The trimmed X and Y extents of BodyPoints (the model's body vertices, model units), the range the collision boxes must stay inside so
	 * none sticks out past what the player sees (#728). The same trim as FitBodyBoxes, so a mirror or an aerial does not stretch it. False,
	 * with the outputs untouched, when there are too few usable points.
	 */
	static bool BodyExtentXY(TConstArrayView<FVector> BodyPoints, FVector2D& OutMin, FVector2D& OutMax);

	/**
	 * Shrinks Box's X and Y extents to lie inside [BodyMin, BodyMax]; it never grows and never moves Z. A box with no overlap with the
	 * range, or that would be left thinner than 2 units, is left as it is. True when it changed.
	 */
	static bool ClampBoxToBodyXY(FMantleChassisBox& Box, const FVector2D& BodyMin, const FVector2D& BodyMax);

	/**
	 * The chassis tub (Placement's collision box) kept inside the model's body in X and Y (#728): the preset's length is scaled by the
	 * wheelbase, but a model's overhangs differ, and a tub longer than the body is an invisible block. True when the tub changed.
	 * Leaves it alone when the body's extents cannot be read.
	 */
	static bool ClampTubToBody(FMantleChassisPlacement& Placement, TConstArrayView<FVector> BodyPoints);

	/** The most body boxes added to the tub. */
	static constexpr int32 MaxBodyBoxes = 2;

	/**
	 * Writes Placement into Source: the tub's collision box, then its body boxes (none unless the builder fitted some), the centre of mass (overridden, so the cook uses it), the driver's
	 * seat with its steering wheel, and the passenger seat (or none), and a first guess of each seat's contact points. Marks Source
	 * for undo first. Does not touch the wheels, body, parts, doors or the other tuning.
	 *
	 * MaxBoxes is how many boxes in all the document gets: the tub and MaxBodyBoxes more for a car fitted to its wheels. A creator's model built on a chassis (#821)
	 * passes FMantleVehicleLimits::MaxCollisionBoxes, its collision boxes in Placement (the one the driver sits in as the tub, the rest as body boxes); the exits are then
	 * guessed beside that first box only, since a wing or a tail box is not where a pilot gets out.
	 */
	static void Apply(const FMantleChassisPlacement& Placement, UMantleVehicleSource& Source, int32 MaxBoxes = MaxBodyBoxes + 1);

	/** Plain-words lines for the creator: what stretched and by how much, and what was held to its limit. Empty when nothing stretched. */
	static TArray<FString> StretchNotes(const FMantleChassisStretch& Stretch);
};
