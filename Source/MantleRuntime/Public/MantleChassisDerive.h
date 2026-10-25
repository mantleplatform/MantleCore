// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class FJsonObject;
struct FMantleChassisSpec;

/**
 * The handling of a chassis (#793): the numbers of a vehicle document that follow from its size, as one set. FMantleChassisDerive makes
 * one from a spec's dimensions; a preset's hand-tuned `defaults` read the same way are the archetype's (FMantleChassisSpec::Archetype).
 * Units are the document's (schema/vehicle.schema.json): cm, kg, N*m, degrees, cm/s, m^2.
 */
struct MANTLERUNTIME_API FMantleChassisHandling
{
	double Mass = 0.0;
	double MaxSpeed = 0.0;
	double DragCoefficient = 0.0;
	double FrontalArea = 0.0;
	/** Chassis frame, cm. */
	FVector CenterOfMass = FVector::ZeroVector;
	double MaxTorque = 0.0;
	double FinalDrive = 0.0;
	double MaxRaise = 0.0;
	double MaxDrop = 0.0;
	double SpringRate = 0.0;
	double SpringPreload = 0.0;
	double SpringDamping = 0.0;
	double MaxSteerAngle = 0.0;
	double BrakeTorque = 0.0;
	double HandbrakeTorque = 0.0;
	double TireFriction = 0.0;
	double CameraArmLength = 0.0;
	double CameraHeight = 0.0;
	/**
	 * A plane's airframe (chassis.aircraft): the wing's lifting area, both halves together, the tailplane's and the fin's, m^2, and each engine's thrust at
	 * full throttle, N. Not a vehicle document's numbers: the plane's definition is made from them directly. 0 for everything that is not a plane.
	 */
	double WingAreaM2 = 0.0;
	double TailplaneAreaM2 = 0.0;
	double FinAreaM2 = 0.0;
	double EngineThrustNewtons = 0.0;
	/** A helicopter's main rotor lift at full collective, N (chassis.helicopter): like a plane's thrust, the definition is made from it directly. 0 for
	 *  everything that is not a helicopter. */
	double RotorThrustNewtons = 0.0;
	/** A hovercraft's (chassis.hovercraft) fan push at rest, N, its pads' spring rate and its rudder's area, m^2: as a helicopter's thrust, the definition
	 *  is made from them directly. 0 for everything that is not a hovercraft. */
	double HoverThrustNewtons = 0.0;
	double PadSpringRate = 0.0;
	double HoverFinAreaM2 = 0.0;
	/**
	 * What a limit held back when FMantleChassisDerive made this, one line each: the document path, the number the rule gave and what it was held to
	 * ("brakes/handbrakeTorque 39321.6 held to 20000"). The rules do not stop at a limit quietly: a number that is not the rule's answer is here.
	 * Also the centre of mass when the stability target or its floor above the wheels moved it ("centerOfMass/z"). Empty for the archetype's own.
	 */
	TArray<FString> Clamped;
};

/**
 * The four feel sliders of a chassis (#793), each from -1 to 1 and 0 for the archetype's own: grip scales the tyre friction, power the
 * engine's power-to-weight, stiffness the ride frequency and topSpeed the top speed, each by up to 30 percent either way. Never
 * physics beyond that: customisation does not change how a vehicle drives (#793).
 */
struct MANTLERUNTIME_API FMantleChassisFeel
{
	double Grip = 0.0;
	double Power = 0.0;
	double Stiffness = 0.0;
	double TopSpeed = 0.0;

	/** How far a slider at its end moves the number it scales. */
	static constexpr double Reach = 0.3;
};

/**
 * The constants of a preset's `derive` block (schema/vehicle-preset.schema.json): what a ratio of dimensions cannot say. The handling of a
 * resized chassis is the preset's hand-tuned handling times how its dimensions differ from the preset's own, so a preset has no numbers of its
 * own to calibrate; these three are one block per type (validate.py holds every preset of a type to the first one's, by file name).
 */
struct MANTLERUNTIME_API FMantleChassisDeriveConstants
{
	/** Least track / (2 x centre of mass height) the centre of mass is lowered to meet; 0 for none (a two-wheeler balances itself). */
	double Stability = 0.0;
	/** How much longer the chase camera's arm is per cm the body is longer than the preset's, and its height per cm of body height above the ground. */
	double CameraArmPerLength = 0.0;
	double CameraHeightPerHeight = 0.0;
};

/** A plane in steady level flight (FMantleChassisDerive::LevelFlight): the pitch it flies at, the elevator input that balances it and the throttle that holds its speed. */
struct MANTLERUNTIME_API FMantleChassisTrim
{
	double PitchDegrees = 0.0;
	/** The elevator input, -1 to 1, at which the pitch moment is zero: 0 means the tailplane's camber alone trims the plane. */
	double Elevator = 0.0;
	/** 0 to 1 of the engines' thrust. */
	double Throttle = 0.0;
};

/**
 * The handling of a chassis from its dimensions (#793). Every number of a vehicle document is the archetype's own, hand-tuned, times a ratio of
 * how the chassis's dimensions differ from the archetype's (FMantleChassisSpec::ArchetypeSize):
 *   H(spec) = Archetype x D(spec dimensions) / D(archetype dimensions)
 * so at the archetype's own dimensions every ratio is 1 and the result is exactly the preset's `defaults`, and a chassis resized by 1.001 is
 * within 0.4% of it at most (the mass moves with S^3, the brakes with S^4): scaled and unscaled are continuous. Pure: the same inputs give the same handling on every machine, with no state and
 * no file reads. No UObjects: any thread.
 *
 * The rules, with L W H the body box, c its clearance, r the wheel radius, n the wheels, m the mass, a 0 for the archetype's own:
 *   mass            m0 x (L W (H + c)) / (L0 W0 (H0 + c0))
 *   centre of mass  X the archetype's x its wheelbase ratio; Z r + the archetype's share of the body above the wheel centres x (H + c - r), lowered to
 *                   meet derive.stability (track / (2 x Z)), never below r + 2 cm
 *   spring rate     rate0 x (m / m0) x (n0 / n) x (f / f0)^2, with f / f0 = 1 / sqrt(r / r0) (the sag stays the same share of the travel); preload
 *                   follows the rate
 *   travel          travel0 x r / r0 each way (3 to 40 cm)
 *   steering lock   the archetype's own, an angle that does not scale. The turning circle (from the steered axles to the middle of the others, over
 *                   tan(lock)) then follows the wheelbase and the axle layout, as on a real car: a longer wheelbase alone turns wider.
 *   max torque      torque0 x m / m0 (constant power-to-weight)
 *   top speed       speed0, less in proportion to r / r0 below 1: a smaller wheel moves less of the road a physics step
 *   final drive     drive0 x (r / r0) / (top speed / speed0): the top gear turns the same share of the redline at the top speed
 *   brakes          brake0 x (m / m0) x (n0 / n) x (r / r0); the handbrake follows the brakes
 *   frontal area    area0 x (W (H + c)) / (W0 (H0 + c0))
 *   camera          the archetype's arm + derive.cameraArmPerLength x (L - L0) (250 to 3000 cm); the height + derive.cameraHeightPerHeight x ((H + c) - (H0 + c0)) (80 to 1000 cm)
 *   wing, tailplane and fin (a plane only)   each area0 x (span x chord) / (span0 x chord0), a fin's span being its height
 *   engine thrust   thrust0 x (m / m0) x (engines0 / engines): the same thrust-to-weight, shared by the engines it has
 *   rotor thrust (a helicopter only)   thrust0 x (m / m0): the same thrust-to-weight, so the collective that holds the weight is the preset's
 *   a hovercraft's fan, pads and rudder   the fan's push thrust0 x (m / m0); each pad's spring rate0 x (m / m0) x (n0 / n) x (d0 / d), d a pad's travel
 *                   less the hover height (how far it comes down at rest, at least 1 cm), so the hull rests at its own hover height; the rudder's
 *                   area area0 x (height x chord) / (height0 x chord0)
 * What no rule moves is the archetype's own: the drag coefficient, the spring damping and the tyre friction (the friction by the grip slider), the
 * torque curve, gear ratios and rpm limits. A ratio is held to 0.001 to 1000 so a dimension that is zero or not a number gives a held number and not
 * a NaN. Every handling number is held inside the document's absolute ranges (limits.json), and each hold is listed in the result's
 * Clamped; the geometry is not held: a wheel radius or a body outside the document's ranges makes a document the reader refuses. Results are rounded to
 * six significant digits, so machines write the same document text unless a number lands within about 1e-15 (relative) of a rounding tie: the
 * last-bit differences of /fp:fast, libm or a fused multiply-add are absorbed.
 */
struct MANTLERUNTIME_API FMantleChassisDerive
{
	/** The handling of Spec's dimensions, as Spec's own type, Archetype and ArchetypeSize rule it, and Feel. */
	static FMantleChassisHandling Derive(const FMantleChassisSpec& Spec, const FMantleChassisFeel& Feel = FMantleChassisFeel());

	/** The engine's own air density, kg/m^3 (Chaos::RealWorldConsts::AirDensity), and the world's gravity as the chassis weighs itself, m/s^2. */
	static constexpr double AirDensity = 1.225;
	static constexpr double Gravity = 9.8;

	/**
	 * Steady level flight of Spec's plane (Spec.Aircraft.bSet) at SpeedMs m/s, with Handling's mass, drag and areas (a spec's own, or a resized one's): the
	 * port of schema/validate.py's _plane_level_flight, the engine's own forces in the body frame (X forward, Z up) about the centre of mass. Each aerofoil
	 * gives its lift and drag at its quarter-chord line (a wing's lift acts (centre of lift - 50) cm aft of it, FAerofoilSimModule::GetCenterOfLiftOffset),
	 * the fin its drag, the fuselage its drag at the chassis module's own position (the root body's origin, not the centre of mass: FChassisSimModule adds
	 * it with AddLocalForce), and each engine its thrust along X at its own height. Level flight is the lift holding the weight with the pitch moment zero:
	 * the pitch is where the lift matches the weight, the elevator the one that zeroes the moment, the throttle the thrust that holds the speed. False when
	 * the plane cannot hold level flight there with the elevator in reach (-1 to 1), or Spec is not a plane. The tailplane's own camber is the elevator's
	 * offset, so OutTrim.Elevator is how far the stick would be held. Pure, no state.
	 */
	static bool LevelFlight(const FMantleChassisSpec& Spec, const FMantleChassisHandling& Handling, double SpeedMs, FMantleChassisTrim& OutTrim);

	/**
	 * The trim of Spec's plane at its own cruise speed (Aircraft.CruiseSpeedCmPerSec): OutCamber, the tailplane camber that centres the elevator there (the
	 * camber now, plus the elevator the plane needs times the tailplane's control angle: only their sum acts on the air), and OutCruiseThrottle, the throttle
	 * that holds the speed. Both are rounded (0.001 degrees, 0.0001) so every machine writes the same document text. On false, neither is changed.
	 */
	static bool SolveTrim(const FMantleChassisSpec& Spec, const FMantleChassisHandling& Handling, double& OutCamber, double& OutCruiseThrottle);

	/** The constants of a preset's `derive` object, read. False with the reason. */
	static bool ReadConstants(const TSharedPtr<FJsonObject>& Block, FMantleChassisDeriveConstants& OutConstants, FString& OutError);

	/** Every document path Derive holds a result to (FMantleVehicleLimits::GetRange has each), for the contract test. */
	static TArray<FString> LimitedPaths();
};
