// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleSkinnedMesh.h"
#include "UObject/Object.h"
#include "MantleVehicleAsset.generated.h"

class USkeletalMesh;
class USkeleton;

/** One wheel of a creator vehicle (#435): schema/vehicle.schema.json wheels[], as the cook resolved it. */
struct FMantleVehicleWheelDesc
{
	/** The wheel-centre bone of the body. */
	FName Bone;
	bool bSteering = false;
	bool bDriven = true;
	bool bHandbrake = false;
	/** cm: measured by the cook from the geometry weighted to the bone, or the document's. */
	double Radius = 0.0;
	double Width = 0.0;
};

/** A suspension linkage rule's kind (#435 b): schema/vehicle.schema.json $defs.linkageRule type. */
UENUM(BlueprintType)
enum class EMantleVehicleLinkageType : uint8
{
	/** Turn Bone so the direction it had toward Target in the reference pose points at Target now (Up keeps its twist). */
	Aim,
	/** A damper: Bone aims at End; End moves where Target carries it (or stays where its parents put it) and aims back. */
	Stretch,
	/** Bone takes Source's movement since the reference pose: its translation, its rotation (or only the part about Axis), or both. */
	Copy,
};

/**
 * One suspension linkage rule of a creator vehicle (#435 b), as the cook resolved it: every bone named in the body's
 * own spelling, Weight set. A name the rule's type does not use is NAME_None.
 */
struct FMantleVehicleLinkageRule
{
	EMantleVehicleLinkageType Type = EMantleVehicleLinkageType::Aim;
	/** The bone the rule moves. */
	FName Bone;
	/** Aim: the bone pointed at. Stretch: the bone that carries End (optional). */
	FName Target;
	/** Aim only, optional: the bone that keeps the rule's twist about its aim. */
	FName Up;
	/** Stretch only: the damper's other end, which the rule also moves. */
	FName End;
	/** Copy only: the bone whose movement is copied. */
	FName Source;
	/** Copy only: which parts. */
	bool bRotation = true;
	bool bTranslation = true;
	/** Copy only: the body axis (0 X, 1 Y, 2 Z) the copied rotation is limited to; INDEX_NONE for all of it. */
	int32 Axis = INDEX_NONE;
	/** 0 leaves the bone where it was, 1 moves it all the way. */
	double Weight = 1.0;
};

/**
 * One attached part of a creator vehicle (#435 item 2), as the cook resolved it: an uploaded static mesh riding Bone (in the
 * body's own spelling) at Transform, in the bone's space. Drawn only: it neither collides nor weighs anything.
 */
struct FMantleVehiclePartDesc
{
	/** The uploaded static mesh (kind mesh) the part draws. */
	FString Mesh;
	/** The body's bone it rides. */
	FName Bone;
	/** Where it sits on the bone (rotation normalised). */
	FTransform Transform = FTransform::Identity;
	/** The uploaded material of each of the mesh's slots, slot 0 first; an empty entry, or a slot past the list, draws Mantle's default surface. */
	TArray<FString> Materials;
	/** The mesh's slots drawn with the vehicle's paint (its colour and livery), as the body's paint sections are; empty when none is. */
	TArray<int32> PaintSlots;
};

/**
 * A part of a creator vehicle the car clips turn on a hinge (#546): a door, the trunk lid, as the cook resolved it. Bone turns
 * (in the body's own spelling; NAME_None: no hinge) about the line through Pivot along Axis, both in the body's space as the
 * seat is, by the clips' angle times Scale. Axis is as the document gives it, never near zero length: only its direction counts.
 */
struct FMantleVehicleHingeDesc
{
	FName Bone;
	FVector Pivot = FVector::ZeroVector;
	FVector Axis = FVector::UpVector;
	double Scale = 1.0;

	bool IsSet() const { return !Bone.IsNone(); }
};

/** What a seat's hands hold (#612): schema/vehicle.schema.json $defs.hands kind. Bikes and aircraft (#559) add their grips. */
UENUM(BlueprintType)
enum class EMantleVehicleHandsKind : uint8
{
	/** No hand targets: the seat clips' own hands. */
	None,
	/** The driver's steering wheel: each hand a point on its rim with the wheel straight ahead, turned round the hub with it. */
	Wheel,
	/** Points that stay where they are in the vehicle: a grab handle, the dashboard, the lap. */
	Fixed,
};

/**
 * One contact point of a seat (#612), as the cook resolved it: a target transform in the vehicle's own space, never a
 * character's skeleton. Location and Rotation are in the body's space (as the seat's location) when Bone is NAME_None, else in
 * Bone's reference-pose space (in the body's own spelling), so the point moves with the bone.
 */
struct FMantleVehicleContactDesc
{
	FName Bone;
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
};

/** One standing spot beside the vehicle (#612): on the ground in the body's space, facing Yaw degrees about Z (0: the body's +X). */
struct FMantleVehicleExitDesc
{
	FVector Location = FVector::ZeroVector;
	double Yaw = 0.0;
};

/**
 * A seat's contact points (#612, the document's seat or passengerSeat pelvis, hands, feet and exits): where a seated body puts
 * its pelvis, palms and balls of its feet, and where it stands after getting out. Each group is optional; an unset one leaves
 * the seat clips' own.
 */
struct FMantleVehicleSeatContactsDesc
{
	bool bPelvis = false;
	FMantleVehicleContactDesc Pelvis;
	/** None: no hand targets. */
	EMantleVehicleHandsKind Hands = EMantleVehicleHandsKind::None;
	FMantleVehicleContactDesc LeftHand;
	FMantleVehicleContactDesc RightHand;
	bool bFeet = false;
	FMantleVehicleContactDesc LeftFoot;
	FMantleVehicleContactDesc RightFoot;
	/** In the order they are tried, the seat's own side first; empty when the document gives none. */
	TArray<FMantleVehicleExitDesc> Exits;

	bool HasAny() const { return bPelvis || Hands != EMantleVehicleHandsKind::None || bFeet || Exits.Num() > 0; }
};

/** A seat's creator-set gun aim arc (#615, #840 step 5d): yaw and pitch bounds relative to the vehicle's +X, degrees, yaw positive toward +Y.
 *  bSet false (the document has no weaponArc): the seat's arc is worked out from where it sits (MantleSeatAim::ArcFor). */
struct FMantleVehicleWeaponArcDesc
{
	bool bSet = false;
	double YawMin = -180.0;
	double YawMax = 180.0;
	double PitchMin = -90.0;
	double PitchMax = 90.0;
};

/** One chassis collision box in the body's component space: centre and half size, cm. */
struct FMantleVehicleBoxDesc
{
	FVector Center = FVector::ZeroVector;
	FVector Extent = FVector::OneVector;
};

/**
 * Which simulation builds a vehicle (#821, the document's movementType). A truck, six-wheeler and motorcycle are Wheeled; a jet is a Plane.
 * (Not FMantleVehicleMovementType, the gameplay module's registry class.)
 */
UENUM(BlueprintType)
enum class EMantleVehicleMovementKind : uint8
{
	Wheeled,
	Plane,
	Helicopter,
	Hover,
};

/** A seat's kind (#821, the document's seat.kind): the clip set its occupant plays (MantleVehicleSeatAssets::ForDriverSeatKind). */
UENUM(BlueprintType)
enum class EMantleVehicleSeatKind : uint8
{
	/** The character's own car clips. */
	Car,
	/** A motorcycle's rider set. */
	Motorcycle,
	/** A pilot's set. */
	Pilot,
};

/** An aerofoil's job (#821, schema/vehicle.schema.json $defs.aerofoil type). */
UENUM(BlueprintType)
enum class EMantleVehicleAerofoilKind : uint8
{
	Fixed,
	Wing,
	Elevator,
	Rudder,
};

/**
 * One point a hand or foot holds (#821 $defs.grip): Location in the body's space at the reference pose, moving with Bone. Bone is NAME_None only
 * while a document is parsed that leaves it out; Read (and the chassis builder) name the root bone (or the rider's fork, for a handlebar) in its place.
 */
struct FMantleVehicleGripDesc
{
	FName Bone;
	FVector Location = FVector::ZeroVector;
};

/** A left grip and a right one (left is the vehicle's -Y side). */
struct FMantleVehicleGripPairDesc
{
	FMantleVehicleGripDesc Left;
	FMantleVehicleGripDesc Right;
};

/**
 * One lifting surface of a plane (#821, FMantleVehicleAerofoil), every default the contract's. Bone, when set, is the control-surface bone that
 * deflects; AreaM2 is m^2 and the angles degrees.
 */
struct FMantleVehicleAerofoilDesc
{
	FName Bone;
	FVector Location = FVector::ZeroVector;
	EMantleVehicleAerofoilKind Type = EMantleVehicleAerofoilKind::Fixed;
	bool bMirror = false;
	double AreaM2 = 0.0;
	double Camber = 5.0;
	double MaxControlAngle = 12.0;
	double StallAngle = 20.0;
};

/** One engine's push (#821, FMantleVehicleThruster). Bone, when set, is where it pushes from (a reference, never posed). Direction is as given: only its direction counts. */
struct FMantleVehicleThrusterDesc
{
	FName Bone;
	FVector Location = FVector::ZeroVector;
	FVector Direction = FVector::ForwardVector;
	double MaxThrustNewtons = 0.0;
};

/** A propeller or rotor bone that turns while someone is aboard (#821, FMantleVehicleSpinBone); cosmetic. Axis is in the body's space. */
struct FMantleVehicleSpinBoneDesc
{
	FName Bone;
	FVector Axis = FVector::ForwardVector;
	double Rpm = 0.0;
};

/** A plane's flight block (#821, MovementType Plane): the numbers ApplyAircraft sets. Speeds cm/s, dampings per second. */
struct FMantleVehicleFlightDesc
{
	double AirSpeedLimit = 0.0;
	double CruiseThrottle = 0.4;
	double PitchAuthority = 0.5;
	double ThrottleLeverRate = 0.5;
	double LinearDamping = 0.0;
	double AngularDamping = 0.0;
	TArray<FMantleVehicleAerofoilDesc> Aerofoils;
	TArray<FMantleVehicleThrusterDesc> Thrusters;
	TArray<FMantleVehicleSpinBoneDesc> SpinBones;
	/** The pilot's hands: left on the throttle, right on the stick. */
	bool bPilotGrips = false;
	FMantleVehicleGripPairDesc PilotGrips;
};

/** One rotor (#821, FMantleVehicleRotor), every default the contract's. The accelerations are rad/s^2. */
struct FMantleVehicleRotorDesc
{
	FName Bone;
	FVector Location = FVector::ZeroVector;
	FVector Axis = FVector::UpVector;
	double MaxThrustNewtons = 0.0;
	double PitchAcceleration = 1.5;
	double RollAcceleration = 1.5;
	double YawAcceleration = 1.0;
	bool bStabilityAssist = true;
	double StabilityStiffness = 3.0;
	double StabilityDamping = 2.0;
	double StabilityMaxAcceleration = 4.0;
};

/** A helicopter's block (#821, MovementType Helicopter): the numbers ApplyHelicopter sets. */
struct FMantleVehicleHelicopterDesc
{
	double AirSpeedLimit = 0.0;
	/** The collective that holds the weight: weight over rotor thrust. */
	double HoverCollective = 0.0;
	double ThrottleLeverRate = 1.0;
	double LinearDamping = 0.1;
	double AngularDamping = 0.0;
	TArray<FMantleVehicleRotorDesc> Rotors;
	TArray<FMantleVehicleSpinBoneDesc> SpinBones;
	/** The pilot's hands: left on the collective, right on the cyclic. */
	bool bPilotGrips = false;
	FMantleVehicleGripPairDesc PilotGrips;
};

/** One hover pad (#821, FMantleVehicleHoverPad), every default the struct's. Lengths cm. */
struct FMantleVehicleHoverPadDesc
{
	FVector Location = FVector::ZeroVector;
	double MaxRaise = 10.0;
	double MaxDrop = 50.0;
	double SpringRate = 37.0;
	double SpringDamping = 0.6;
};

/** A hovercraft's fan (#821): its place and push, and FMantleVehicleHoverThrust. TopSpeedCmPerSec 0 is no falloff. */
struct FMantleVehicleHoverThrusterDesc
{
	FVector Location = FVector::ZeroVector;
	FVector Direction = FVector::ForwardVector;
	double MaxThrustNewtons = 0.0;
	double ReverseShare = 0.5;
	double VectorAngleDegrees = 25.0;
	double TopSpeedCmPerSec = 2500.0;
	double BrakeFadeCmPerSec = 100.0;
};

/** A hovercraft's fin (#821): a rudder aerofoil with no bone that follows the steering. */
struct FMantleVehicleHoverRudderDesc
{
	FVector Location = FVector::ZeroVector;
	double AreaM2 = 0.0;
	double Camber = 0.0;
	double MaxControlAngle = 0.0;
	double StallAngle = 0.0;
};

/** What stands in for a tyre's grip on a hover (#821, FMantleVehicleHoverDamping). */
struct FMantleVehicleHoverDampingDesc
{
	double LateralPerSecond = 1.5;
	double MaxLateralAccel = 400.0;
	double YawPerSecond = 2.0;
	double MaxYawAccelDegrees = 180.0;
	double SkirtBrakePerSecond = 2.0;
	double MaxSkirtBrakeAccel = 500.0;
};

/** A hovercraft's block (#821, MovementType Hover): the numbers ApplyHovercraft sets. */
struct FMantleVehicleHoverDesc
{
	TArray<FMantleVehicleHoverPadDesc> Pads;
	FMantleVehicleHoverThrusterDesc Thruster;
	bool bRudder = false;
	FMantleVehicleHoverRudderDesc Rudder;
	FMantleVehicleHoverDampingDesc Damping;
	/** The chassis's angular damping on the ground and in the air alike. */
	double AngularDamping = 0.5;
};

/**
 * ChaosVehicles' arcade rotation control as a rider writes it (#553, #821): an absent key is 0 or false here, not the definition's default, so a cooked
 * document writes all fourteen.
 */
struct FMantleVehicleRotationControlDesc
{
	bool bTorqueControl = false;
	double TorqueRotationDamping = 0.0;
	bool bTargetRotation = false;
	bool bRollVsSpeed = false;
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
 * A motorcycle rider's fit (#553, #821; the driver seat's rider block): the handlebar on the fork, the foot pegs, the rider's offsets and the rotation
 * control. RearTireFriction and RearBrakeTorque, when set, are every non-steering wheel's own.
 */
struct FMantleVehicleRiderDesc
{
	/** The steered wheel's SteerBone and the handlebar grips' bone. */
	FName ForkBone;
	FMantleVehicleGripPairDesc Handlebar;
	FMantleVehicleGripPairDesc FootPegs;
	FVector PelvisOffset = FVector::ZeroVector;
	FVector StopPelvisShift = FVector::ZeroVector;
	double KneeSplayDegrees = 8.0;
	bool bRearTireFriction = false;
	double RearTireFriction = 0.0;
	bool bRearBrakeTorque = false;
	double RearBrakeTorque = 0.0;
	FMantleVehicleRotationControlDesc RotationControl;
};

/**
 * A cooked vehicle's resolved document (MVH1's JSON, schema/vehicle.schema.json with every default
 * filled by the cook). Unreal units: cm, kg, Nm, degrees, cm/s. Nothing in it names an asset path.
 */
struct MANTLERUNTIME_API FMantleVehicleDesc
{
	double Mass = 1500.0;
	/** cm/s: each wheel's spin is capped at MaxSpeed over its radius. */
	double MaxSpeed = 4000.0;
	bool bCenterOfMass = false;
	FVector CenterOfMass = FVector::ZeroVector;
	double DragCoefficient = 0.3;
	/** m^2. */
	double FrontalArea = 2.2;

	double MaxTorque = 0.0;
	int32 MaxRPM = 0;
	int32 IdleRPM = 900;
	/** (rpm, fraction of MaxTorque), rpm strictly rising. */
	TArray<FVector2D> TorqueCurve;

	/** Forward gears, first gear first. */
	TArray<float> GearRatios;
	double ReverseRatio = 3.0;
	double FinalDrive = 0.0;
	int32 ChangeUpRPM = 0;
	int32 ChangeDownRPM = 0;

	double SuspensionMaxRaise = 10.0;
	double SuspensionMaxDrop = 10.0;
	double SpringRate = 250.0;
	double SpringPreload = 50.0;
	double SpringDamping = 0.5;

	double MaxSteeringAngle = 35.0;
	bool bAckermann = true;
	/**
	 * How much of the lock the steering gives at a speed (#821, steering.speedCurve): (mph, share) points, mph strictly rising; empty (absent) is the full lock at
	 * every speed. A ground car's document carries the chassis cars' curve, so a published car steers as the developer build's does.
	 */
	TArray<FVector2D> SteeringSpeedCurve;

	double BrakeTorque = 1500.0;
	double HandbrakeTorque = 3000.0;
	double TireFriction = 2.0;
	/** The grip of every wheel that does not steer (#821, tires.rearFriction), where TireFriction is the steered wheels'; unset, TireFriction is every wheel's. */
	bool bTireRearFriction = false;
	double TireRearFriction = 0.0;

	TArray<FMantleVehicleWheelDesc> Wheels;

	/** Where the seated driver's capsule centre goes, in the body's space, cm. */
	FVector SeatLocation = FVector::ZeroVector;
	FMantleVehicleWeaponArcDesc SeatWeaponArc;

	/**
	 * The seat's steering wheel (#546), the rim the driver's hands hold. bSteeringWheel: the seat has one. Its hub in the
	 * body's space (as SeatLocation), turned so its X runs along the column away from the driver and its Z towards the top
	 * of the rim; the rim's radius where the hands grip it, cm.
	 */
	bool bSteeringWheel = false;
	FVector SteeringWheelLocation = FVector::ZeroVector;
	FRotator SteeringWheelRotation = FRotator::ZeroRotator;
	double SteeringWheelRadius = 0.0;
	/** The body's bone the steering wheel is on, which turns with it about the hub's X (#546); NAME_None: nothing turns. */
	FName SteeringWheelBone;
	/** The door beside the driver (#546); unset on a car without one. */
	FMantleVehicleHingeDesc SeatDoor;
	/** The driver's contact points (#612); none set on a seat without them. */
	FMantleVehicleSeatContactsDesc SeatContacts;
	/** The document's seat.hidden: a model with no inside, so the driver is not drawn while seated. */
	bool bSeatHidden = false;

	/**
	 * A front passenger seat (#546). bPassengerSeat: the car has one. Where the seated passenger's capsule centre goes, in the
	 * body's space (as SeatLocation), and the door beside it (unset without one).
	 */
	bool bPassengerSeat = false;
	FVector PassengerSeatLocation = FVector::ZeroVector;
	FMantleVehicleWeaponArcDesc PassengerSeatWeaponArc;
	FMantleVehicleHingeDesc PassengerSeatDoor;
	/** The passenger's contact points (#612); never Wheel hands. */
	FMantleVehicleSeatContactsDesc PassengerSeatContacts;
	/** The document's passengerSeat.hidden: the passenger is not drawn while seated. */
	bool bPassengerSeatHidden = false;

	/**
	 * The trunk (#546). bTrunk: the car has one. Its lid, where a character stands to open it (on the ground behind the car, in
	 * the body's space) and the way it faces there, degrees about Z (0: the body's +X, towards the car).
	 */
	bool bTrunk = false;
	FMantleVehicleHingeDesc TrunkLid;
	FVector TrunkStand = FVector::ZeroVector;
	double TrunkYaw = 0.0;

	/**
	 * The horn (#546): the uploaded sound (kind sound, as a SoundEmitter's) the driver's horn plays; empty on a car without one,
	 * which plays Mantle's built-in horn instead.
	 */
	FString HornSound;

	double CameraArmLength = 650.0;
	double CameraHeight = 150.0;

	/** The chase camera's orbit point (#821, camera.boomLocation), in the body's space; unset, the definition's own. A flier's sets it. */
	bool bCameraBoom = false;
	FVector CameraBoomLocation = FVector::ZeroVector;

	/**
	 * What simulation builds it (movementType). A plane has Flight, a helicopter Helicopter and a hovercraft Hover, and only that one is meaningful.
	 * The wheels stay required for every type (the reader needs a steered and a driven one); a type's rules say what they do.
	 */
	EMantleVehicleMovementKind MovementType = EMantleVehicleMovementKind::Wheeled;
	FMantleVehicleFlightDesc Flight;
	FMantleVehicleHelicopterDesc Helicopter;
	FMantleVehicleHoverDesc Hover;

	/** The seats' kinds (#821, seat.kind and passengerSeat.kind). */
	EMantleVehicleSeatKind SeatKind = EMantleVehicleSeatKind::Car;
	EMantleVehicleSeatKind PassengerSeatKind = EMantleVehicleSeatKind::Car;
	/** How far to the side an exiting driver is put, cm (#821, seat.exitSideDistance); unset, the client's car rule. */
	bool bSeatExitSideDistance = false;
	double SeatExitSideDistance = 0.0;
	/** The driver's motorcycle rider fit (#821, seat.rider); needs SeatKind Motorcycle. */
	bool bRider = false;
	FMantleVehicleRiderDesc Rider;

	TArray<FMantleVehicleBoxDesc> Collision;

	/** The uploaded material each body section is drawn with, by the section's material name. */
	TMap<FString, FString> Materials;

	/**
	 * The paint (#435 f, the document's paint): the body sections, and the parts' PaintSlots (#435 item 2), drawn with
	 * Mantle's car paint instead of their material, and its finish. bPaint: the document has paint (it paints a body
	 * section, a part's slot or both); without it the vehicle draws every section as authored.
	 */
	bool bPaint = false;
	TArray<FString> PaintSections;
	/** Linear RGB. */
	FLinearColor PaintColor = FLinearColor(0.02f, 0.09f, 0.3f);
	double PaintMetallic = 0.5;
	double PaintRoughness = 0.4;
	double FlakeDensity = 0.5;
	/** cm. */
	double FlakeSize = 0.06;
	FLinearColor FlakeTint = FLinearColor::White;
	double ClearCoat = 1.0;
	double ClearCoatRoughness = 0.05;

	/** The liveries (#488): uploaded texture ids, picked by index; empty when it has none. Needs paint. */
	TArray<FString> Liveries;
	/** The body's UV set the liveries are laid out on: always 0, the one UV set a vehicle body carries. */
	int32 LiveryUV = 0;
	/** The livery a spawned car wears, an index into Liveries; INDEX_NONE for none. */
	int32 DefaultLivery = INDEX_NONE;

	/** The suspension linkage (#435 b), applied in this order after the wheel bones are posed; empty when it has none. */
	TArray<FMantleVehicleLinkageRule> Linkage;

	/** The attached parts (#435 item 2): static meshes riding the body's bones; empty when it has none. */
	TArray<FMantleVehiclePartDesc> Parts;

	bool HasPaint() const { return bPaint || PaintSections.Num() > 0; }

	/** Whether any part has a painted slot (#435 item 2). */
	bool HasPartPaint() const
	{
		return Parts.ContainsByPredicate([](const FMantleVehiclePartDesc& Part) { return Part.PaintSlots.Num() > 0; });
	}

	/** Every uploaded material the vehicle draws with, its body's sections' and then its parts', each once. */
	TArray<FString> MaterialIds() const;

	/** Every uploaded mesh its parts draw (#435 item 2), each once, in the parts' order. */
	TArray<FString> PartMeshIds() const;

	bool HasHorn() const { return !HornSound.IsEmpty(); }

	/** Every uploaded sound it plays (#546: its horn's), each once. */
	TArray<FString> SoundIds() const;
};

/** MVH1 read: the resolved document and the body (MSK1 role 4). Holds no UObjects: any thread. */
struct FMantleVehicleAsset
{
	FMantleVehicleDesc Desc;
	FMantleSkinnedMesh Body;

	/** Each wheel's bone position in the body's reference pose, component space, cm, in Desc.Wheels order. */
	TArray<FVector> WheelPositions;

	/** The body's root bone in its reference pose, component space. */
	FTransform RootBoneTransform = FTransform::Identity;
};

/**
 * limits.json placeAssets.vehicle as this build reads it (#435). The cook enforces the same numbers on
 * every upload; the client checks them again because MVH1 arrives over the network.
 */
struct MANTLERUNTIME_API FMantleVehicleLimits
{
	static constexpr int32 MinWheels = 2;
	static constexpr int32 MaxWheels = 8;
	static constexpr int32 MaxTorqueCurvePoints = 16;
	static constexpr int32 MaxGears = 8;
	static constexpr int32 MaxCollisionBoxes = 8;
	/** placeAssets.vehicle.body.maxSections: one material per section at most. */
	static constexpr int32 MaxSections = 8;
	static constexpr int32 MaxBones = 128;
	/** placeAssets.vehicle.body.maxTriangles and maxInfluencesPerVertex. */
	static constexpr int32 MaxTriangles = 200000;
	static constexpr int32 MaxInfluences = 4;
	/** placeAssets.vehicle.wheelMinVertices: vertices weighted to a wheel bone alone that a measurement needs. */
	static constexpr int32 WheelMinVertices = 8;
	/** placeAssets.livery.maxLiveries (#488): textures a vehicle's liveries list may hold. */
	static constexpr int32 MaxLiveries = 8;
	/** placeAssets.vehicle.maxLinkageRules (#435 b): rules a vehicle's linkage may hold. */
	static constexpr int32 MaxLinkageRules = 64;
	/** placeAssets.vehicle.maxParts (#435 item 2): attached parts a vehicle may carry. */
	static constexpr int32 MaxParts = 16;
	/** placeAssets.staticMesh.maxSections: the materials a part may list, one per slot of its mesh. */
	static constexpr int32 MaxPartMaterials = 8;
	/** How far from 1 a part's rotation quaternion's length may be, and how near 0 its scale on an axis (the cook's partRules). */
	static constexpr double PartRotationTolerance = 0.01;
	static constexpr double PartMinScale = 0.001;
	/** How short a hinge's axis may be (#546, the cook's carRules): shorter, it has no direction to turn about. */
	static constexpr double HingeMinAxis = 0.01;
	/** placeAssets.vehicle.maxSeatExits (#612): the standing spots a seat's exits may list. */
	static constexpr int32 MaxSeatExits = 4;
	/** placeAssets.vehicle.contactReach (#612): how far, cm, a pelvis, hand or foot target in the body's space may be from its seat's location. */
	static constexpr double ContactReach = 150.0;
	/** placeAssets.vehicle.wheelHandTolerance (#612): how far, cm, a hand holding the steering wheel may be from its rim. */
	static constexpr double WheelHandTolerance = 5.0;
	/** placeAssets.vehicle maxAerofoils, maxThrusters, maxSpinBones (a flight or helicopter block), maxRotors and minHoverPads to maxHoverPads (#821). */
	static constexpr int32 MaxAerofoils = 8;
	static constexpr int32 MaxThrusters = 4;
	static constexpr int32 MaxSpinBones = 8;
	static constexpr int32 MaxRotors = 4;
	static constexpr int32 MinHoverPads = 3;
	static constexpr int32 MaxHoverPads = 8;
	/** placeAssets.vehicle.maxSteeringCurvePoints (#821): the points of steering.speedCurve. */
	static constexpr int32 MaxSteeringCurvePoints = 8;
	/** How short a thrust direction, a spin axis or a rotor axis may be (#821, the cook's movementRules): shorter, it points nowhere. */
	static constexpr double DirectionMinLength = 0.01;

	/**
	 * placeAssets.vehicle.ranges[Path]: Path is limits.json's key, such as "engine/maxRPM" (a star stands
	 * for any array item). False when this build has no such range.
	 */
	static bool GetRange(const FString& Path, double& OutMin, double& OutMax);
};

/**
 * Reads MVH1, a creator vehicle the cook wrote (backend/internal/cook/vehicle.go): "MVH1", version 1,
 * the resolved document (length-prefixed UTF-8 JSON), then the body as MSK1 role 4 on skeleton
 * "vehicle" (length-prefixed). Nothing may follow.
 *
 * Strict, like every cooked-format reader: these bytes came from a stranger. The document must hold
 * exactly the schema's keys with every value the cook resolves (each wheel's radius and width, the seat,
 * the collision boxes, the shift points; with paint, its every finish value; with liveries, liveryUV; with
 * linkage, each rule's weight and a copy's parts; with parts, each one's location, rotation and scale; with a
 * steering wheel, its rotation; with a door or a trunk lid, its scale; with a trunk, its yaw; with a contact point, its
 * rotation; with an exit, its yaw), numbers finite and within FMantleVehicleLimits' ranges, a seat's contact points held
 * to its reach and a hand on the steering wheel to its rim (the cook's contactRules); the body must read
 * (FMantleSkinnedMeshReader) as a vehicle body with every wheel bone in it, none of them the root, every bone a linkage
 * rule names, none it moves the root, a wheel bone or a bone a wheel hangs from, every bone a part rides, every bone the
 * car clips turn (the steering wheel's, the doors', the trunk lid's), none of them the root, a wheel bone or a bone a
 * wheel hangs from, every bone a contact point is on, and every bone a movement block or the rider names
 * (ResolveMovementBones). A document with a movement type has its block and no other; a block's optional
 * numbers read as the contract's defaults when left out, and a key the reader does not know is refused. A refusal is false
 * with the reason. No UObjects: any thread.
 */
class MANTLERUNTIME_API FMantleVehicleAssetReader
{
public:
	/** The MVH1 version this build reads. Must match VehicleVersion in the worker. */
	static constexpr uint32 SupportedVersion = 1;

	/** The document's size cap (MaxVehicleDocumentBytes in the worker). */
	static constexpr int32 MaxDocumentBytes = 64 * 1024;

	/**
	 * Parses a resolved vehicle document. A key with a default (a block's optional numbers) may be left out and reads as the contract's default, so the
	 * cook's resolved form and the editor's own composed one both read; a grip that names no bone is left NAME_None for ResolveMovementBones.
	 */
	static bool ParseDocument(TConstArrayView<uint8> Json, FMantleVehicleDesc& Out, FString& OutError);

	/**
	 * Checks the bones the movement blocks and the rider name against the body's (#821, the cook's resolveMovement) and names them as the body
	 * does: a bone the body lacks is refused; a posed bone (an aerofoil's, a spin bone) is not the root, a wheel's bone or one a wheel hangs from; a
	 * fork is neither the root nor a wheel's bone; a grip that names no bone is on the root (a handlebar: the fork). Read does this; so does the
	 * chassis builder for the body it makes.
	 */
	static bool ResolveMovementBones(FMantleVehicleDesc& Desc, TConstArrayView<FMantleSkinnedMeshBone> Bones, FString& OutError);

	/** Reads a whole MVH1 file. */
	static bool Read(TConstArrayView<uint8> Data, FMantleVehicleAsset& Out, FString& OutError);
};

/**
 * #728: the surface of a vehicle's body as the exact shape characters collide with ("what you see is what you stand on"), in the
 * body's reference pose and component space, cm. The wheels are left out: a wheel moves, spins and steers, and the car's physics
 * owns where it is. Plain arrays, no engine collision types; the game module turns them into a query-only collision component.
 */
struct MANTLERUNTIME_API FMantleVehicleBodyCollision
{
	/** Only the vertices a kept triangle uses. */
	TArray<FVector3f> Positions;

	/** Triangle list of indices into Positions. */
	TArray<int32> Indices;

	int32 NumTriangles() const { return Indices.Num() / 3; }

	/**
	 * Body's LOD0 triangles that are not the wheels': a triangle is dropped when any of its vertices has a wheel bone (one named in
	 * WheelBones) as its heaviest influence (the cook's rule for what is the chassis), and a triangle with no area is dropped too.
	 * Null when the body has no LOD or skin data to tell by, or nothing is left.
	 */
	static TSharedPtr<const FMantleVehicleBodyCollision> FromBody(const FMantleSkinnedMesh& Body, TConstArrayView<FName> WheelBones);
};

/**
 * A creator vehicle built for this process (#435): its body as a transient USkeletalMesh on a transient
 * USkeleton made from the body's own bone table, with the document. FMantlePlaceAssets builds one per
 * uploaded vehicle id a place uses (on the game thread, after the MVH1 read); MantleGameplay turns it into
 * the vehicle definition AMantleVehicle builds from. No gameplay types here: MantleCore stays free of them.
 */
UCLASS(Transient)
class MANTLERUNTIME_API UMantleBuiltVehicle : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	TObjectPtr<USkeletalMesh> Mesh;

	UPROPERTY()
	TObjectPtr<USkeleton> Skeleton;

	FMantleVehicleDesc Desc;

	/** Each wheel's reference-pose bone position, component space, in Desc.Wheels order. */
	TArray<FVector> WheelPositions;

	/** The root bone's reference pose in component space (the chassis boxes are authored in component space). */
	FTransform RootBoneTransform = FTransform::Identity;

	/** #728: the body's triangles off the wheels, for the exact character collision; null when there are none. */
	TSharedPtr<const FMantleVehicleBodyCollision> BodyCollision;

	/**
	 * Builds from a read MVH1: the skeleton from the body's bone table and reference pose, the mesh on it
	 * (one LOD, material slots named after the body's sections). Game thread. Null, with the reason in
	 * OutError, when the body will not build.
	 */
	static UMantleBuiltVehicle* Build(const FMantleVehicleAsset& Asset, UObject* Outer, FString* OutError = nullptr);
};
