// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleVehicleAsset.h"

struct FMantleChassisAircraft;
struct FMantleChassisSpec;

/**
 * ChaosVehicles' arcade rotation control as a motorcycle preset's chassis.rider.rotationControl writes it (#553): the keys of
 * FMantleVehicleRotationControl that matter, by the same names. An absent key is 0 or false. MantleRuntime cannot name the
 * gameplay struct, so MantleGameplay copies these across.
 */
struct FMantleVehicleChassisRotationControl
{
	bool bTorqueControl = false;
	bool bTargetRotation = false;
	bool bRollVsSpeed = false;
	double TorqueRotationDamping = 0.0;
	double RollControlScaling = 0.0;
	double RollMaxAngle = 0.0;
	double PitchControlScaling = 0.0;
	double PitchMaxAngle = 0.0;
	double RotationStiffness = 0.0;
	double RotationDamping = 0.0;
	double MaxAccel = 0.0;
	double AutoCentreRollStrength = 0.0;
	double AutoCentrePitchStrength = 0.0;
	double AutoCentreYawStrength = 0.0;
};

/**
 * What a rider-seat preset (seats.driver "motorcycle") adds to the chassis (#553). Chassis frame, cm. The grips ride the front fork
 * bone and the pegs the root bone; the reference names the bones, so a preset never does.
 */
struct FMantleVehicleChassisRider
{
	/** The preset has a rider block. */
	bool bSet = false;
	FVector LeftGrip = FVector::ZeroVector;
	FVector RightGrip = FVector::ZeroVector;
	FVector LeftPeg = FVector::ZeroVector;
	FVector RightPeg = FVector::ZeroVector;
	/** The seat's RiderPelvisOffset, RiderStopPelvisShift and RiderKneeSplayDegrees. */
	FVector PelvisOffset = FVector::ZeroVector;
	FVector StopPelvisShift = FVector::ZeroVector;
	double KneeSplayDegrees = 8.0;
	/** The rear wheel's own tyre friction and brake torque (the document holds one of each, which the front wheel gets); 0 keeps the document's. */
	double RearTireFriction = 0.0;
	double RearBrakeTorque = 0.0;
	FMantleVehicleChassisRotationControl RotationControl;
};

/**
 * What a preset's chassis block says, read for sizing a creator's model onto it (#728 step 2). Model space, cm, the origin on the
 * ground under the middle of the wheelbase, +X forward, the driver on -Y.
 */
struct FMantleVehicleChassisReference
{
	/** Front axle to the rearmost axle. */
	double Wheelbase = 0.0;
	/** Each axle front to rear, as a share of the wheelbase behind the front axle (0 is the front axle, 1 the rearmost): two to four. */
	TArray<double> AxlePositions;
	double Track = 0.0;
	double WheelRadius = 0.0;
	double WheelWidth = 0.0;
	/** One wheel per axle, on the centreline (a motorcycle): Track is then only the fit's nominal width, and the chassis has no left or right wheel. */
	bool bCentreline = false;
	/** The body box: its length (X), width (Y), height (Z) and how far its underside is above the ground. */
	double BodyLength = 0.0;
	double BodyWidth = 0.0;
	double BodyHeight = 0.0;
	double GroundClearance = 0.0;
	FVector CenterOfMass = FVector::ZeroVector;
	FVector Seat = FVector::ZeroVector;
	FVector SteeringWheelHub = FVector::ZeroVector;
	double SteeringWheelRadius = 0.0;
	/** How the steering wheel is pitched, degrees. */
	double SteeringWheelPitch = 0.0;
	/** The car has a passenger seat: the driver's, mirrored across Y. */
	bool bPassenger = false;
	/** seats.driver: the seat kind, "car" or "motorcycle". */
	FString DriverSeat = TEXT("car");
	/** A steering wheel is drawn and held; a rider seat has handlebars instead and no steering wheel. */
	bool bSteeringWheel = true;
	/** The bone a steered centreline wheel hangs under (yawed by SteerBone); None when no wheel has one. */
	FName ForkBone;
	FMantleVehicleChassisRider Rider;

	/** The X of axle Index (0 is the front): the origin is the middle of the wheelbase, so the front axle is at +Wheelbase / 2. */
	double AxleX(const int32 Index) const { return Wheelbase * (0.5 - AxlePositions[Index]); }

	FVector PassengerSeat() const { return FVector(Seat.X, -Seat.Y, Seat.Z); }
};

/**
 * A vehicle preset's own bare chassis (#728): schema/vehicle-presets/<id>.json's `chassis` block made into a complete vehicle that
 * needs no creator model, so each vehicle type is Mantle's to tune and the creator's model is only a skin on it.
 *
 * MakeAsset builds what a published vehicle's MVH1 read yields (FMantleVehicleAsset): a skeleton of a root bone and one bone per
 * wheel at the layout's positions (two to four axles), a grey body (the body box, plus a cylinder per wheel skinned to its bone), and a Desc made by
 * composing a vehicle document, the preset's `defaults` plus the chassis's wheels, collision box, centre of mass and seats, and
 * reading it with FMantleVehicleAssetReader::ParseDocument, so it passes the validation a published document does. UMantleBuiltVehicle::Build
 * and FMantlePlaceVehicleDefinitions::Make then turn it into a drivable definition exactly as they do a published vehicle.
 *
 * The preset is read into an FMantleChassisSpec (MantleChassisSpec.h, #793), which can also be sized by a uniform scale with its handling
 * re-derived (FMantleChassisDerive); every function here that takes a preset's text has a twin that takes the spec.
 *
 * Model space, cm: +X forward, +Y right, +Z up, the origin on the ground under the middle of the wheelbase, the driver on -Y.
 * Wheels are in the order front axle left, front axle right, then each axle behind it (left is -Y). The bones of a two-axle chassis are
 * wheel_fl, wheel_fr, wheel_rl and wheel_rr; three or four axles are wheel_a1_l, wheel_a1_r, wheel_a2_l and so on, front axle first.
 * A centreline chassis (a motorcycle) has one wheel on each axle, at Y 0: wheel_f and wheel_r (wheel_a1, wheel_a2 and so on for three or
 * four axles), and a steered one hangs under a fork bone (fork_f, fork_a1 and so on) that a wheel's SteerBone yaws. A plane has one axle
 * so and one not: its nose wheel is wheel_f and its mains wheel_rl and wheel_rr, then the bones of its control surfaces and engines (below),
 * and its body is the fuselage, a block per half-wing, the tailplane, the fin and a cross for each propeller (a jet: a nozzle box behind the
 * tail for each engine and an intake each side of the fuselage), the document's collision the fuselage, wing, tailplane and fin. A helicopter has two
 * axles of a pair each (its skids), whose wheels are not drawn: its body is the cabin, a mast, the main rotor's blades on main_rotor, the tail boom, the
 * fin, the tail rotor's blades on tail_rotor and the two skids on their legs and cross-tubes, the document's collision the cabin, boom and fin. A
 * hovercraft has two axles of a pair each too (where its pads lift), whose wheels are not drawn: its body is the hull, the skirt under it, the fan's pod
 * behind it and the fin on its top, all on the root, the document's collision the hull alone.
 * No UObjects: any thread.
 */
struct MANTLERUNTIME_API FMantleVehicleChassisBuilder
{
	/** The chassis body's root bone. */
	static FName RootBoneName();

	/** The fork bone of the steered centreline axle of a two-axle chassis (fork_f), the parent of wheel_f. */
	static FName FrontForkBoneName();

	/**
	 * A plane's bones (chassis.aircraft), each a child of the root posed at its surface's hinge, which is where the surface's block turns about: an aileron
	 * (aileron_l, aileron_r) turns a half-wing, the elevator the tailplane and the rudder the fin; a propeller (propeller with one engine, propeller_l and
	 * propeller_r with two) spins about X; a jet's nozzle (nozzle with one engine, nozzle_l and nozzle_r with two) is where its engine pushes from and does
	 * not move. None has a body in the physics asset, which is the root's alone.
	 */
	static FName AileronBoneName(bool bRight);
	static FName ElevatorBoneName();
	static FName RudderBoneName();
	static FName PropellerBoneName(int32 Engine, int32 EngineCount);
	static FName NozzleBoneName(int32 Engine, int32 EngineCount);

	/** The bone engine Engine of Aircraft pushes from: its propeller's, or its jet nozzle's when the airframe has no propeller. */
	static FName EngineBoneName(const FMantleChassisAircraft& Aircraft, int32 Engine);

	/**
	 * A helicopter's bones (chassis.helicopter), each a child of the root: main_rotor at the main rotor's hub, which spins about Z and carries its blades,
	 * and tail_rotor at the tail rotor's hub, which spins about Y and carries its. Neither has a body in the physics asset.
	 */
	static FName MainRotorBoneName();
	static FName TailRotorBoneName();

	/** The plugin's Resources/VehiclePresets directory (a developer build's presets; empty when the plugin is not found). */
	static FString PresetDirectory();

	/** The ids of the preset files in PresetDirectory, sorted. */
	static TArray<FString> PresetIds();

	/** The text of preset PresetId (a lowercase id, as the file's name). False with the reason. */
	static bool LoadPresetText(const FString& PresetId, FString& OutText, FString& OutError);

	/** The chassis block of a preset's text, read. False with the reason. */
	static bool ReadReference(const FString& PresetJsonText, FMantleVehicleChassisReference& OutReference, FString& OutError);

	/** The chassis block of Spec, read: its dimensions, so a scaled chassis's are its scaled ones. */
	static FMantleVehicleChassisReference ReferenceOf(const FMantleChassisSpec& Spec);

	/**
	 * The spec of SelectorText (FMantleChassisSpec::ParseSelector: a preset id, or an id and a scale such as "sedan@0.7") from the plugin's
	 * presets: the preset as it is at scale 1, a Scaled one with derived handling otherwise. False with the reason.
	 */
	static bool LoadSpec(const FString& SelectorText, FMantleChassisSpec& OutSpec, FString& OutError);

	/** The resolved vehicle document (schema/vehicle.schema.json, every default filled) for a preset's text. False with the reason. */
	static bool MakeDocument(const FString& PresetJsonText, FString& OutDocument, FString& OutError);

	/** The chassis as a read vehicle: Desc from MakeDocument through ParseDocument, the body and the wheel and root poses. False with the reason. */
	static bool MakeAsset(const FString& PresetJsonText, FMantleVehicleAsset& OutAsset, FString& OutError);

	/** The same for a spec (a preset as it is, or a Scaled one): Desc from FMantleChassisSpec::ComposeDocument through ParseDocument. */
	static bool MakeAsset(const FMantleChassisSpec& Spec, FMantleVehicleAsset& OutAsset, FString& OutError);
};
