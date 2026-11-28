// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * One lifting surface's numbers for FMantleFlightModel::AerofoilForce (#379): a wing half, a tailplane, a fin or a rudder. Angles in degrees,
 * the area in m^2, the chord in cm.
 */
struct FMantleAerofoilSurface
{
	/** m^2 (a wing half's own half). */
	double AreaM2 = 0.0;
	/** Degrees of built-in angle, added to the airflow's angle: a tailplane's trim, a wing's incidence. */
	double CamberDegrees = 0.0;
	/** Degrees: the lift peaks here, then drops sharply (5 to 30). */
	double StallDegrees = 16.0;
	/** The peak lift coefficient, at the stall angle. */
	double MaxLift = 1.5;
	/** The whole surface's span^2 / area (a wing's two halves together): its drag from lift. */
	double AspectRatio = 7.0;
	/** cm: how far the lift slides aft past the stall (a quarter of it). */
	double ChordCm = 100.0;
	/** The creator's multipliers on the lift and drag coefficients (FMantleVehicleAerofoil). */
	double LiftMultiplier = 1.0;
	double DragMultiplier = 1.0;
};

/** What FMantleFlightModel::AerofoilForce gives: the force in newtons and where it acts, both in the surface's frame. */
struct FMantleAerofoilForce
{
	/** Newtons, surface frame (X the chord forward, Z the lift axis). */
	FVector Force = FVector::ZeroVector;
	/** cm along X from the surface's location where the force acts (negative: aft). */
	double PointX = 0.0;
	/** The airflow's angle on the chord plus the camber, degrees, -180..180 (what the stall test reads). */
	double AngleDegrees = 0.0;
	double LiftCoefficient = 0.0;
	double DragCoefficient = 0.0;
};

/** One engine's numbers for FMantleFlightModel::Thrust (#379). */
struct FMantlePlaneEngine
{
	/** Newtons at full throttle, static (a propeller) or dry (a jet). */
	double MaxThrustNewtons = 0.0;
	/** Shaft power, watts. Above 0: a propeller, whose push falls off as power / speed. 0: a jet, flat with speed. */
	double PowerWatts = 0.0;
	/** Newtons with the afterburner lit (the lever's last tenth). 0 or not above MaxThrustNewtons: none. */
	double AfterburnerNewtons = 0.0;
};

/**
 * Mantle's flight maths (#379), one copy for the physics (UMantleVehicleSimAerofoilComponent's module, the plane thruster), the trim
 * (FMantleChassisDerive) and schema/validate.py's twin, so what flies and what is trimmed can never differ. Pure: no world, no state.
 *
 * The lift curve: straight to 80% of the stall angle (slope 1.111 x MaxLift / stall), rounded into the peak MaxLift at the stall angle, a
 * drop over 2 degrees to 0.7 x MaxLift, then a straight fade into the flat plate's 1.1 sin(2 angle) by 30 degrees (or the stall + 4, if
 * later), the flat plate past it and for air from behind. A control surface's angle adds a lift step (slope x angle) while the flow is
 * attached, fading out with it, so it never moves the stall point; |lift| is capped at 1.25 x MaxLift. Drag: the larger of 0.01 +
 * lift^2 / (pi 0.8 AR) and the flat plate's 1.2 sin^2(angle). Lift acts at right angles to the airflow (in the chord-lift plane, the span's
 * flow dropped first), drag along it. The lift point slides aft by up to a quarter chord past the stall.
 */
struct MANTLERUNTIME_API FMantleFlightModel
{
	/** The profile drag every surface has, and the span efficiency in its drag from lift. */
	static constexpr double ProfileDrag = 0.01;
	static constexpr double SpanEfficiency = 0.8;
	/** A propeller's efficiency in its power-limited push (0.8 P / V), and the least airspeed that formula sees, m/s. */
	static constexpr double PropellerEfficiency = 0.8;
	static constexpr double PropellerLeastSpeed = 1.0;
	/** Where the afterburner starts on the lever. */
	static constexpr double AfterburnerLever = 0.9;

	/** The lift coefficient at an angle (degrees, the airflow's plus the camber, any value) with a control angle's lift step, before the multiplier. */
	static double LiftCoefficient(double AngleDegrees, double ControlDegrees, const FMantleAerofoilSurface& Surface);
	/** The drag coefficient for that angle and lift coefficient, before the multiplier. */
	static double DragCoefficient(double AngleDegrees, double Lift, const FMantleAerofoilSurface& Surface);
	/** The lift slope in the straight part, per degree. */
	static double LiftSlopePerDegree(const FMantleAerofoilSurface& Surface);
	/** How far the lift point has slid aft at an angle, cm (0 below the stall, a quarter chord past the fade). */
	static double LiftPointShiftCm(double AngleDegrees, const FMantleAerofoilSurface& Surface);
	/** The airflow's angle on the chord, degrees, -180..180: Velocity is the surface's own through the air (surface frame, any unit). */
	static double AirflowAngleDegrees(const FVector& Velocity);

	/**
	 * The force of one surface moving at Velocity (m/s, the surface's frame: X its chord forward, Z its lift axis, Y the span) through air
	 * of Density (kg/m^3), its control surface at ControlDegrees. The span's flow is dropped before the angle and the dynamic pressure.
	 * Nothing for a surface standing still or for a bad number.
	 */
	static FMantleAerofoilForce AerofoilForce(const FVector& Velocity, double ControlDegrees, double Density, const FMantleAerofoilSurface& Surface);

	/**
	 * One engine's push, newtons, at Throttle (0..1) and the airspeed along its axis (m/s), in air DensityRatio of sea level's. A propeller:
	 * Throttle x min(MaxThrust, 0.8 Power / max(speed, 1)). A jet: 0..0.9 of the lever gives 0..MaxThrust in a straight line, 0.9..1 dry to
	 * afterburner (none: Throttle x MaxThrust). Both times DensityRatio.
	 */
	static double Thrust(const FMantlePlaneEngine& Engine, double Throttle, double AirspeedMs, double DensityRatio);

	/**
	 * A jet's spool (the share of its thrust it makes) after Dt seconds chasing the Lever: up at (0.25 + 0.75 spool) x 1.848 / SpoolSeconds
	 * per second (slow at idle, quick near the top: idle to full in SpoolSeconds), down at 2 / SpoolSeconds. SpoolSeconds 0: the lever at once.
	 */
	static double AdvanceSpool(double Spool, double Lever, double SpoolSeconds, double Dt);

	/** The fade the stall protection puts on the player's nose-up input at this share of the stall angle: 1 up to 0.85, 0 from 0.95. */
	static double StallProtectionShare(double StallShare);
	/** The coordinated-turn assist's rudder for a sideslip (degrees, positive: the air comes from the right, the nose left of the path): +1/10 per degree, within +-0.5. */
	static double CoordinatedYaw(double SideslipDegrees);
};
