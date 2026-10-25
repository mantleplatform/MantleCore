// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleVehicleAsset.h"

class FJsonObject;
class UMantleVehicleSource;
struct FMantleChassisSpec;

/**
 * The movement part of a vehicle document, written and derived: movementType, the flight, helicopter or hover block, the seats' kinds, the
 * driver's exit distance and rider fit, and the chase camera's boom. Every number is flat: a published vehicle's handling is fixed when it is published, so
 * the document never carries a preset id and the client never re-derives from one. The Go cook
 * (backend/internal/cook/vehicle_movement.go) writes the same form into MVH1 and FMantleVehicleAssetReader reads it.
 *
 * Derive is the pure number derivation the chassis demo path (FMantleChassisDefinitions, in the game module, which this plugin cannot depend on) computed
 * from a chassis spec: the airframe's aerofoils, the engines, the propellers, the rotors, the hover pads, the rider's grips. Write turns what Derive made (or
 * what a reader read) into the document's JSON, with every default filled, so Write then ParseDocument gives back exactly the Desc that was written.
 *
 * No UObjects: any thread.
 */
struct MANTLERUNTIME_API FMantleVehicleChassisDocument
{
	/** A helicopter's rigid-body linear damping, per second: the rotor's drag, which settles a hover and holds its top speed near 50 m/s (helicopter.linearDamping). */
	static constexpr double HelicopterLinearDamping = 0.1;
	/** How fast a helicopter's collective moves, full travel per second (a plane's throttle takes two): helicopter.throttleLeverRate. */
	static constexpr double HelicopterCollectiveRate = 1.0;
	/** A hovercraft's rigid-body angular damping, per second, on the ground and in the air alike (default 0.5): hover.angularDamping. */
	static constexpr double HovercraftAngularDamping = 0.5;
	/**
	 * How much of the elevator's travel the player's keys and stick reach (flight.pitchAuthority): half of its 20 degrees, a visible flap and a real climb that is
	 * still hard to stall from one held key.
	 */
	static constexpr double PlanePitchAuthority = 0.5;
	/** How far to the side a bike's rider steps off, cm: a body-width, not a car's door (seat.exitSideDistance). */
	static constexpr double BikeExitSideDistance = 100.0;
	/**
	 * A car's (a ground chassis on more than two wheels) tyre grip per axle, as shares of its preset's one friction: the steered front's and the rest's, 3.0 and 4.0
	 * of a 3.5, the built-in sports car's balance. The front's is written as tires.friction and the rear's as tires.rearFriction. A bike's tyres keep their own.
	 */
	static constexpr double FrontTireGripShare = 6.0 / 7.0;
	static constexpr double RearTireGripShare = 8.0 / 7.0;

	/**
	 * The movement part of Spec's chassis, in InOutDesc: MovementType, the block of its type, the seat kinds, the exit distance, the rider, the camera boom. The
	 * areas, thrust and spring rate are the spec's handling (the preset's own, or a resized chassis's derived ones). Everything else in InOutDesc is left alone;
	 * the movement fields are first reset, so a Desc is derived once however often it is asked. Bones are named as the chassis builder's skeleton names them
	 * (FMantleVehicleChassisBuilder), a grip on the root bone by name.
	 *
	 * Every number is held to its contract range, as ComposeDocument holds the collision boxes to +-1000 cm: a chassis resized past what a document can say (a
	 * plane 1.3 times the preset's size has its tail 11 m back) would not read at all. OutHeld, when given, gets a line for each number held ("path value held to
	 * limit"); nothing is held at the presets' own sizes or at 0.7.
	 */
	static void Derive(const FMantleChassisSpec& Spec, FMantleVehicleDesc& InOutDesc, TArray<FString>* OutHeld = nullptr);

	/**
	 * Writes the movement part of Desc into Document: movementType when it is not wheeled, the block of its type with every default filled, seat.kind and
	 * passengerSeat.kind when not car, seat.exitSideDistance, seat.rider and camera.boomLocation. A seat or camera object Document does not have is made;
	 * passengerSeat.kind is written only to a passengerSeat Document has (it has no location of its own to make one from). A car writes nothing.
	 */
	static void Write(const FMantleVehicleDesc& Desc, FJsonObject& Document);

	/** Derive then Write, for ComposeDocument: Spec's movement part into Document. */
	static void Compose(const FMantleChassisSpec& Spec, FJsonObject& Document, TArray<FString>* OutHeld = nullptr);

	/**
	 * A ground car's handling, baked into Document (#821; a published vehicle's handling is fixed when it is published, so Make adds none of its own): for a
	 * chassis on more than two wheels that is not a plane, a helicopter or a hovercraft, the steering eases off with speed (steering.speedCurve, the built-in
	 * sports car's curve: mph, share of the lock) and the tyres grip front and rear as the built-in sports car's do (tires.friction times FrontTireGripShare, and
	 * tires.rearFriction times RearTireGripShare): at full lock at 100 km/h a tap of the key slid the front tyres past their grip and spun the car, and equal
	 * tyres let the driven rear go first. Document's tires and steering objects must exist. Nothing for any other chassis.
	 */
	static void ComposeGroundHandling(const FMantleChassisSpec& Spec, FJsonObject& Document, TArray<FString>* OutHeld = nullptr);

	/**
	 * Every number a document made from Spec has to hold to the contract's ranges (Derive's, and a ground car's tyre grip), in plain words for a creator: "The
	 * area of a wing, tail or fin would be 120, past what Mantle allows: it is held to 100 (the most)." One line for each thing held, however many of its axes
	 * or surfaces were. Empty when the document says everything Spec asks for, as it does inside the presets' fit ranges. The silent clamps a published
	 * vehicle would otherwise carry.
	 */
	static TArray<FString> HeldNumbers(const FMantleChassisSpec& Spec);

	/** The steering speed curve ComposeGroundHandling writes: (mph, share of the lock), DA_SportsCar's. */
	static TArray<FVector2D> GroundSteeringSpeedCurve();

	/**
	 * The movement keys of Spec's chassis alone, as the JSON object text UMantleVehicleSource::MovementBlocks holds: the flight, helicopter or hover block, and
	 * the seat's exitSideDistance and rider and the camera's boomLocation, each under the key they have in a document. "{}" for a car.
	 */
	static FString MovementBlocksJson(const FMantleChassisSpec& Spec);

	/**
	 * Merges a source's movement and optional seat firearm aim arcs into a document made from it (#821, #615): its movement type and seat kinds when they are
	 * not the baseline, enabled arcs on placed seats, and the blocks MovementBlocks holds (top-level flight, helicopter and hover replace the document's;
	 * seat, passengerSeat and camera objects add their keys to the document's own, which must already exist). A disabled arc is not written: the seat's arc is
	 * then worked out from where it sits. False, with the reason in OutError, for invalid arcs or blocks that are not a JSON object or name a key a source may not
	 * set (movementType, kind, or anything else).
	 */
	static bool MergeSource(const UMantleVehicleSource& Source, FJsonObject& Document, FString* OutError = nullptr);

	/** The strings of the document's enums, as the schema spells them. */
	static const TCHAR* MovementName(EMantleVehicleMovementKind Movement);
	static const TCHAR* SeatKindName(EMantleVehicleSeatKind Kind);
	static const TCHAR* AerofoilKindName(EMantleVehicleAerofoilKind Kind);
};
