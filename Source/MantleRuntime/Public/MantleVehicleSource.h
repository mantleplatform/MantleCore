// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleSource.generated.h"

class UMaterialInterface;
class USkeletalMesh;
class USoundBase;
class UStaticMesh;
class UTexture2D;

/** One wheel of a creator vehicle (#435): a wheel-centre bone of the body and how it behaves. */
USTRUCT(BlueprintType)
struct MANTLERUNTIME_API FMantleVehicleSourceWheel
{
	GENERATED_BODY()

	/** The wheel-centre bone. Convert Vehicle finds bones named like the recipe's: "wheel" and a corner, e.g. Wheel_FL, PhysWheel_BR. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheel")
	FName Bone;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheel")
	bool bSteering = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheel")
	bool bDriven = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheel")
	bool bHandbrake = false;

	/** cm. 0 lets Mantle measure it from the geometry weighted to the bone (and the tire mesh). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheel", meta = (ClampMin = "0", ClampMax = "150", Units = "cm"))
	float Radius = 0.f;

	/** cm. 0 lets Mantle measure it, as Radius. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheel", meta = (ClampMin = "0", ClampMax = "100", Units = "cm"))
	float Width = 0.f;

	/**
	 * Optional: a tire whose mesh is separate from the body (the Vehicle template's OffroadCar), merged onto the
	 * bone when the vehicle is published and spun with it. TireTransform places it relative to the bone; a negative
	 * scale mirrors it (a right-hand tire).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheel")
	TSoftObjectPtr<UStaticMesh> TireMesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheel")
	FTransform TireTransform;
};

/**
 * One attached part of a creator vehicle (#435 item 2): a static mesh riding a bone of the body, such as a body shell, glass
 * or a bumper. Published as its own mesh upload beside the vehicle; drawn only (the chassis collides as its boxes). Convert
 * Vehicle fills these from the static meshes attached to the body (in a level actor, or the mesh's preview attachments); a
 * socket is written as its bone, with the socket's offset folded into Transform.
 */
USTRUCT(BlueprintType)
struct MANTLERUNTIME_API FMantleVehicleSourcePart
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Part")
	TSoftObjectPtr<UStaticMesh> Mesh;

	/** The body's bone it rides; it moves with the bone (a wheel's spins with the wheel). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Part")
	FName Bone;

	/** Where it sits in the bone's space; a negative scale mirrors it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Part")
	FTransform Transform;

	/** Per-slot overrides of the mesh's own materials, by slot index; an empty entry keeps the mesh's own. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Part")
	TArray<TSoftObjectPtr<UMaterialInterface>> Materials;

	/**
	 * The mesh's material slots (by index, as the mesh editor lists them) drawn with the vehicle's paint, and so wearing its
	 * colour and its liveries, as the body's Paint > Sections are: a body shell that is a part (the OffroadCar's). Needs the
	 * vehicle's Paint. Convert Vehicle fills it with the slots whose material is Clear Coat or has a Paint Tint parameter, or
	 * is a painted body section's.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Part")
	TArray<int32> PaintSlots;

	/** Where Convert Vehicle found it (a component's name, or "preview attachment"), for its notes. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Part")
	FString FoundOn;
};

/** One chassis collision box, in the body's space: centre and half size, cm. */
USTRUCT(BlueprintType)
struct MANTLERUNTIME_API FMantleVehicleSourceBox
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Collision")
	FVector Center = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Collision", meta = (ClampMin = "1", ClampMax = "1000"))
	FVector Extent = FVector(100.0);
};

/**
 * A part of the body the car clips turn on a hinge (#546): a door, the trunk lid (schema/vehicle.schema.json $defs.hinge). The
 * clips carry how far the pack sedan's opens; Mantle turns Bone about the line through Pivot along Axis by that angle times Scale,
 * the right-hand way round, so the axis's sign picks the way it opens. The bone carries its children, and the parts riding them.
 */
USTRUCT(BlueprintType)
struct MANTLERUNTIME_API FMantleVehicleSourceHinge
{
	GENERATED_BODY()

	/** The body's bone that turns: not the root, a wheel's bone or a bone a wheel hangs from, and no other hinge's. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hinge")
	FName Bone;

	/** A point on the hinge, in the body's space (as Seat Location), cm. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hinge")
	FVector Pivot = FVector::ZeroVector;

	/** The way the hinge runs, in the body's space, each part -1 to 1; only its direction counts. A door's is upright: 0, 0, 1 or 0, 0, -1. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hinge")
	FVector Axis = FVector::UpVector;

	/** How far it turns for the clips' angle: 1 as far as the pack sedan's door or lid; negative turns it the other way. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Hinge", meta = (ClampMin = "-2", ClampMax = "2"))
	float Scale = 1.f;
};

/**
 * One contact point of a seat (#612, schema/vehicle.schema.json $defs.contactPoint): where a seated body puts its pelvis, a palm
 * or the ball of a foot, in the vehicle's own space. Every body Mantle supports reaches the same point; nothing here names a
 * character's skeleton.
 */
USTRUCT(BlueprintType)
struct MANTLERUNTIME_API FMantleVehicleSourceContact
{
	GENERATED_BODY()

	/** The body's bone the point is on (a pedal, a seat that slides), so it moves with it; never a wheel's bone. None: the body's space. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contact")
	FName Bone;

	/** Where the point is: in the body's space (as Seat Location), or in Bone's space, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contact")
	FVector Location = FVector::ZeroVector;

	/** How the target is turned: its X the way the pelvis faces or the fingers point. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contact")
	FRotator Rotation = FRotator::ZeroRotator;
};

/** A standing spot beside the vehicle (#612, $defs.exit): where a character stands after getting out. */
USTRUCT(BlueprintType)
struct MANTLERUNTIME_API FMantleVehicleSourceExit
{
	GENERATED_BODY()

	/** On the ground beside the vehicle, in the body's space, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Exit")
	FVector Location = FVector::ZeroVector;

	/** The way the character faces there, about Z: 0 faces the vehicle's +X. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Exit", meta = (Units = "deg"))
	float Yaw = 0.f;
};

/**
 * A seat's contact points (#612): the pelvis, hands and feet targets a seated body is posed onto, and where it stands after
 * getting out. Each group is optional. Convert Vehicle guesses them for a placed seat that has none (GuessSeatContacts): the
 * pelvis under the seat, the hands on the steering wheel's rim (or the lap), the feet ahead on the floor, an exit on each side.
 */
USTRUCT(BlueprintType)
struct MANTLERUNTIME_API FMantleVehicleSourceSeatContacts
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contacts", meta = (ScriptName = "has_pelvis"))
	bool bPelvis = false;

	/** Where the hips sit, facing its X. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contacts", meta = (EditCondition = "bPelvis"))
	FMantleVehicleSourceContact Pelvis;

	/**
	 * What the hands hold. Wheel: the driver's Steering Wheel, each hand a point on its rim (within 5 cm) with the wheel straight
	 * ahead, in the body's space; Mantle turns them round the hub with the wheel. Fixed: points that stay put (a grab handle, the
	 * lap). None: the seat clips' own hands.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contacts")
	EMantleVehicleHandsKind Hands = EMantleVehicleHandsKind::None;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contacts", meta = (EditCondition = "Hands != EMantleVehicleHandsKind::None"))
	FMantleVehicleSourceContact LeftHand;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contacts", meta = (EditCondition = "Hands != EMantleVehicleHandsKind::None"))
	FMantleVehicleSourceContact RightHand;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contacts")
	bool bFeet = false;

	/** The balls of the feet: the pedals or the floor. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contacts", meta = (EditCondition = "bFeet"))
	FMantleVehicleSourceContact LeftFoot;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contacts", meta = (EditCondition = "bFeet"))
	FMantleVehicleSourceContact RightFoot;

	/** Where to stand after getting out, in the order they are tried (the seat's own side first), at most 4. Empty: beside the car. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Contacts")
	TArray<FMantleVehicleSourceExit> Exits;

	bool HasAny() const { return bPelvis || Hands != EMantleVehicleHandsKind::None || bFeet || Exits.Num() > 0; }
};

/** Where a gun held in this seat may aim and fire (#615, #840 step 5d), relative to the vehicle's front. Off: Mantle works the arc out from
 *  where the seat sits (its side of the car and its row). */
USTRUCT(BlueprintType)
struct MANTLERUNTIME_API FMantleVehicleSourceWeaponArc
{
	GENERATED_BODY()

	/** Set this seat's arc yourself. Off: worked out from where the seat sits. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Firearm Aim Arc")
	bool bEnabled = false;

	/** The left edge: degrees from straight ahead, negative to the left (-90 is straight out of the left side). */

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Firearm Aim Arc", meta = (EditCondition = "bEnabled", ClampMin = "-180", ClampMax = "180", Units = "deg"))
	float YawMin = -180.f;

	/** The right edge: degrees from straight ahead, positive to the right (90 is straight out of the right side). */

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Firearm Aim Arc", meta = (EditCondition = "bEnabled", ClampMin = "-180", ClampMax = "180", Units = "deg"))
	float YawMax = 180.f;

	/** How far down the gun may aim, degrees below the horizon as a negative number. */

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Firearm Aim Arc", meta = (EditCondition = "bEnabled", ClampMin = "-90", ClampMax = "90", Units = "deg"))
	float PitchMin = -90.f;

	/** How far up the gun may aim, degrees above the horizon. */

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Firearm Aim Arc", meta = (EditCondition = "bEnabled", ClampMin = "-90", ClampMax = "90", Units = "deg"))
	float PitchMax = 90.f;
};

/**
 * How a creator vehicle drives (#435): schema/vehicle.schema.json, every default the schema's. Mantle holds
 * each number to limits.json placeAssets.vehicle.ranges when the place is checked and again when it is cooked.
 */
USTRUCT(BlueprintType)
struct MANTLERUNTIME_API FMantleVehicleSourceTuning
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Chassis", meta = (ClampMin = "50", ClampMax = "20000", Units = "kg"))
	float Mass = 1500.f;

	/** Top speed, cm/s. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Chassis", meta = (ClampMin = "100", ClampMax = "12000"))
	float MaxSpeed = 4000.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Chassis")
	bool bOverrideCenterOfMass = false;

	/** In the body's space, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Chassis", meta = (EditCondition = "bOverrideCenterOfMass"))
	FVector CenterOfMass = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Chassis", meta = (ClampMin = "0", ClampMax = "2"))
	float DragCoefficient = 0.3f;

	/** Square metres the drag acts on. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Chassis", meta = (ClampMin = "0", ClampMax = "20"))
	float FrontalArea = 2.2f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Engine", meta = (ClampMin = "10", ClampMax = "5000"))
	float MaxTorque = 400.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Engine", meta = (ClampMin = "1000", ClampMax = "20000"))
	int32 MaxRPM = 6000;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Engine", meta = (ClampMin = "300", ClampMax = "5000"))
	int32 IdleRPM = 900;

	/** (rpm, fraction of Max Torque) points, rpm rising, 2 to 16. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Engine")
	TArray<FVector2D> TorqueCurve = { FVector2D(0.0, 0.6), FVector2D(3000.0, 1.0), FVector2D(6000.0, 0.8) };

	/** Forward gears, first gear first, 1 to 8. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Transmission")
	TArray<float> GearRatios = { 3.2f, 2.1f, 1.4f, 1.0f };

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Transmission", meta = (ClampMin = "0.1", ClampMax = "10"))
	float ReverseRatio = 3.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Transmission", meta = (ClampMin = "0.1", ClampMax = "10"))
	float FinalDrive = 3.5f;

	/** 0: 85% of Max RPM. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Transmission", meta = (ClampMin = "0", ClampMax = "20000"))
	int32 ChangeUpRPM = 0;

	/** 0: 40% of Max RPM. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Transmission", meta = (ClampMin = "0", ClampMax = "20000"))
	int32 ChangeDownRPM = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Suspension", meta = (ClampMin = "0", ClampMax = "50", Units = "cm"))
	float SuspensionMaxRaise = 10.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Suspension", meta = (ClampMin = "0", ClampMax = "50", Units = "cm"))
	float SuspensionMaxDrop = 10.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Suspension", meta = (ClampMin = "1", ClampMax = "5000"))
	float SpringRate = 250.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Suspension", meta = (ClampMin = "0", ClampMax = "5000"))
	float SpringPreload = 50.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Suspension", meta = (ClampMin = "0", ClampMax = "1"))
	float SpringDamping = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Steering", meta = (ClampMin = "5", ClampMax = "70", Units = "deg"))
	float MaxSteeringAngle = 35.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Steering")
	bool bAckermann = true;

	/**
	 * How much of the lock the steering gives at a speed (#821, steering.speedCurve): (mph, share of the lock) points, mph strictly rising, at most 8; empty is the
	 * full lock at every speed. A ground car built from a preset carries the chassis cars' curve.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Steering")
	TArray<FVector2D> SteeringSpeedCurve;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Brakes", meta = (ClampMin = "0", ClampMax = "20000"))
	float BrakeTorque = 1500.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Brakes", meta = (ClampMin = "0", ClampMax = "20000"))
	float HandbrakeTorque = 3000.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tires", meta = (ClampMin = "0.1", ClampMax = "5"))
	float TireFriction = 2.f;

	/** The grip of every wheel that does not steer (#821, tires.rearFriction), where Tire Friction is the steered wheels'; off, Tire Friction is every wheel's. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tires")
	bool bTireRearFriction = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tires", meta = (EditCondition = "bTireRearFriction", ClampMin = "0.1", ClampMax = "10"))
	float TireRearFriction = 2.f;

	/** For a model with no inside: the driver isn't drawn while driving, so only the vehicle shows. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Seat", meta = (DisplayName = "Hide the driver"))
	bool bHideDriver = false;

	/** For a model with no inside: the passenger isn't drawn while riding, so only the vehicle shows. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Seat", meta = (DisplayName = "Hide the passenger"))
	bool bHidePassenger = false;

	/** Off: Mantle puts the driver behind the middle, on the left. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Driver", meta = (ScriptName = "has_seat_location"))
	bool bSeatLocation = false;

	/** Where the seated driver's capsule centre goes, in the body's space, cm; the driver faces +X. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Driver", meta = (EditCondition = "bSeatLocation"))
	FVector SeatLocation = FVector::ZeroVector;

	/** Where a gun held in this seat may aim and fire. Off: worked out from where the seat sits. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Driver", meta = (DisplayName = "Firearm Aim Arc", EditCondition = "bSeatLocation"))
	FMantleVehicleSourceWeaponArc SeatWeaponArc;

	/**
	 * The driver's steering wheel (#546), so the seated driver's hands hold this car's own rim. Part of the seat: it needs
	 * Seat Location. Off: no steering wheel, and the driver's hands stay where the car clips hold theirs.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Driver")
	bool bSteeringWheel = false;

	/** The wheel's hub, in the body's space (as Seat Location), cm. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Driver", meta = (EditCondition = "bSteeringWheel"))
	FVector SteeringWheelLocation = FVector::ZeroVector;

	/** How the hub is turned: its X along the column, away from the driver; its Z towards the top of the rim. A typical wheel is pitched about -20. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Driver", meta = (EditCondition = "bSteeringWheel"))
	FRotator SteeringWheelRotation = FRotator::ZeroRotator;

	/** The rim's radius where the hands grip it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Driver", meta = (EditCondition = "bSteeringWheel", ClampMin = "10", ClampMax = "30", Units = "cm"))
	float SteeringWheelRadius = 19.f;

	/** The body's bone the wheel itself is on, which turns with the driver's hands about the hub's X (its column). None: nothing turns. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Driver", meta = (EditCondition = "bSteeringWheel"))
	FName SteeringWheelBone;

	/**
	 * The door beside the driver (#546), which the car clips open and shut as the driver gets in and out. Part of the seat: it
	 * needs Seat Location. Off: no door moves.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Driver")
	bool bHasDriverDoor = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Driver", meta = (EditCondition = "bHasDriverDoor"))
	FMantleVehicleSourceHinge DriverDoor;

	/** The driver's contact points (#612): pelvis, hands, feet and exits. Part of the seat: they need Seat Location. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Driver", meta = (EditCondition = "bSeatLocation"))
	FMantleVehicleSourceSeatContacts DriverContacts;

	/**
	 * A front passenger seat (#546). The passenger clips get in and out through the car's right-hand side (+Y), as the driver's
	 * do through the left. Off: the car has no passenger seat.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Passenger")
	bool bPassengerSeat = false;

	/** Where the seated passenger's capsule centre goes, in the body's space (as Seat Location), cm; the passenger faces +X. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Passenger", meta = (EditCondition = "bPassengerSeat"))
	FVector PassengerSeatLocation = FVector::ZeroVector;

	/** Where a gun held in this seat may aim and fire. Off: worked out from where the seat sits. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Passenger", meta = (DisplayName = "Firearm Aim Arc", EditCondition = "bPassengerSeat"))
	FMantleVehicleSourceWeaponArc PassengerSeatWeaponArc;

	/** The door beside the passenger, which the passenger clips open and shut. Off: no door moves. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Passenger", meta = (EditCondition = "bPassengerSeat"))
	bool bHasPassengerDoor = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Passenger", meta = (EditCondition = "bPassengerSeat && bHasPassengerDoor"))
	FMantleVehicleSourceHinge PassengerDoor;

	/** The passenger's contact points (#612), as the driver's; its hands are never on the steering wheel. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Passenger", meta = (EditCondition = "bPassengerSeat"))
	FMantleVehicleSourceSeatContacts PassengerContacts;

	/** A trunk (#546): a lid that someone standing behind the car opens and shuts. It holds nothing. Off: the car has no trunk. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Trunk")
	bool bTrunk = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Trunk", meta = (EditCondition = "bTrunk"))
	FMantleVehicleSourceHinge TrunkLid;

	/** Where the character stands to open it, in the body's space, cm: on the ground behind the car. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Trunk", meta = (EditCondition = "bTrunk"))
	FVector TrunkStand = FVector::ZeroVector;

	/** The way the character faces there, about Z: 0 faces the car's +X, towards the car from behind it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Trunk", meta = (EditCondition = "bTrunk", Units = "deg"))
	float TrunkYaw = 0.f;

	/**
	 * The horn (#546): a sound wave of your own project (imported from a WAV file), which publishes with the vehicle as its own
	 * sound upload and plays once through each time the driver sounds the horn, for everyone near the car. None: a car
	 * plays Mantle's built-in horn (a bike, a boat or an aircraft stays silent). Mantle's own sounds can't be named here.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Horn")
	TSoftObjectPtr<USoundBase> HornSound;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Camera", meta = (ClampMin = "100", ClampMax = "3000", Units = "cm"))
	float CameraArmLength = 650.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Camera", meta = (ClampMin = "0", ClampMax = "1000", Units = "cm"))
	float CameraHeight = 150.f;

	/** The chassis as up to 8 boxes. Empty: one box around the body's geometry that no wheel bone carries. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Collision")
	TArray<FMantleVehicleSourceBox> Collision;
};

/**
 * A creator vehicle's paint (#435 f) and liveries (#488): schema/vehicle.schema.json paint, liveries, liveryUV and
 * defaultLivery, every default the schema's. Mantle holds each number to limits.json placeAssets.vehicle.ranges and the
 * liveries to placeAssets.livery when the place is checked and again when it is cooked.
 */
USTRUCT(BlueprintType)
struct MANTLERUNTIME_API FMantleVehicleSourcePaint
{
	GENERATED_BODY()

	/**
	 * The body's sections (its material slots, by the names Convert Vehicle lists) drawn with Mantle's car paint: a clear
	 * coat over metallic flakes, in a colour every player sees and scripts can change. Empty, with no part's Paint Slots either:
	 * no paint (and so no liveries); every section keeps its own material. A part's Paint Slots (#435 item 2) wear this paint
	 * too. Convert Vehicle fills it with the sections whose material is Clear Coat or has a Paint Tint parameter.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Paint")
	TArray<FString> Sections;

	/** The colour a car is painted when it spawns (linear). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Paint", meta = (HideAlphaChannel))
	FLinearColor Color = FLinearColor(0.02f, 0.09f, 0.3f);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Paint", meta = (ClampMin = "0", ClampMax = "1"))
	float Metallic = 0.5f;

	/** The paint's own roughness; the clear coat above it has Clear Coat Roughness. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Paint", meta = (ClampMin = "0", ClampMax = "1"))
	float Roughness = 0.4f;

	/** 0 is a solid paint, 1 a flake everywhere. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Paint", meta = (ClampMin = "0", ClampMax = "1"))
	float FlakeDensity = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Paint", meta = (ClampMin = "0.01", ClampMax = "1", Units = "cm"))
	float FlakeSize = 0.06f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Paint", meta = (HideAlphaChannel))
	FLinearColor FlakeTint = FLinearColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Paint", meta = (ClampMin = "0", ClampMax = "1"))
	float ClearCoat = 1.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Paint", meta = (ClampMin = "0", ClampMax = "1"))
	float ClearCoatRoughness = 0.05f;

	/**
	 * Liveries: images laid over the paint and under its clear coat, on the body's UV 0 (a vehicle body carries one UV
	 * set). Their alpha is where the paint shows through. At most 8, published with the vehicle as textures; a Mantle
	 * Vehicle Spawn's Livery or a script (Vehicles:SetLivery) picks one by its index here, the first being 0. Needs paint.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Liveries")
	TArray<TSoftObjectPtr<UTexture2D>> Liveries;

	/** The livery a car wears when it spawns, by its index in Liveries; -1 for none. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Liveries", meta = (ClampMin = "-1", ClampMax = "7"))
	int32 DefaultLivery = -1;
};

/** Which part of a copied rotation a linkage rule takes (#435 b): all of it, or its twist about one axis of the body. */
UENUM(BlueprintType)
enum class EMantleVehicleLinkageAxis : uint8
{
	All,
	X,
	Y,
	/** Up: a hub takes its wheel's steering but not its spin. */
	Z,
};

/**
 * One suspension linkage rule (#435 b): schema/vehicle.schema.json $defs.linkageRule. The published car runs no Control
 * Rig, so these move its visible suspension bones after the simulation has posed the wheels, every frame, in order:
 *   Aim: Bone turns so the direction it had toward Target (in the mesh's reference pose) points at Target; Up, if set,
 *        keeps its twist, as a Control Rig aim's secondary target does.
 *   Stretch: a damper. Bone aims at End, and End moves to where Target carries it (or stays where its parents put it)
 *        and aims back at Bone, so the shaft slides in its body.
 *   Copy: Bone takes Source's movement since the reference pose: its translation, its rotation (Axis Z: only the
 *        steering), or both.
 * A rule never moves the root, a wheel bone or a bone a wheel hangs from. Convert Vehicle fills these from the bone names
 * and where the bones sit; they can be edited here.
 */
USTRUCT(BlueprintType)
struct MANTLERUNTIME_API FMantleVehicleSourceLinkage
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Linkage")
	EMantleVehicleLinkageType Type = EMantleVehicleLinkageType::Aim;

	/** The bone the rule moves. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Linkage")
	FName Bone;

	/** Aim: the bone to point at. Stretch: the bone that carries End (optional). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Linkage", meta = (EditCondition = "Type != EMantleVehicleLinkageType::Copy", EditConditionHides))
	FName Target;

	/** Aim, optional: the bone that keeps the rule's twist about its aim. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Linkage", meta = (EditCondition = "Type == EMantleVehicleLinkageType::Aim", EditConditionHides))
	FName Up;

	/** Stretch: the damper's other end, which the rule moves too. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Linkage", meta = (EditCondition = "Type == EMantleVehicleLinkageType::Stretch", EditConditionHides))
	FName End;

	/** Copy: the bone whose movement Bone takes. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Linkage", meta = (EditCondition = "Type == EMantleVehicleLinkageType::Copy", EditConditionHides))
	FName Source;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Linkage", meta = (EditCondition = "Type == EMantleVehicleLinkageType::Copy", EditConditionHides))
	bool bRotation = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Linkage", meta = (EditCondition = "Type == EMantleVehicleLinkageType::Copy", EditConditionHides))
	bool bTranslation = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Linkage", meta = (EditCondition = "Type == EMantleVehicleLinkageType::Copy", EditConditionHides))
	EMantleVehicleLinkageAxis Axis = EMantleVehicleLinkageAxis::All;

	/** 0 leaves the bone where it was, 1 moves it all the way. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Linkage", meta = (ClampMin = "0", ClampMax = "1"))
	float Weight = 1.f;
};

/**
 * A creator vehicle as authored (#435): the body skeletal mesh (a bone at each wheel centre), its wheels and its
 * tuning. The plugin's Convert Vehicle command makes one from a skeletal mesh, the vehicle builder puts it in the level, and
 * publishing uploads it as one Vehicle asset (the body as a GLB with the vehicle document in it) and its materials, which a library item and the place's Storage then name.
 * Nothing here is a class, a blueprint or code: a published vehicle runs on Mantle's own vehicle.
 */
UCLASS(BlueprintType)
class MANTLERUNTIME_API UMantleVehicleSource : public UDataAsset
{
	GENERATED_BODY()

public:
	/** The body: its materials are published with it, one per section. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Vehicle")
	TSoftObjectPtr<USkeletalMesh> Body;

	/**
	 * The preset this vehicle was built from (#728), such as "sedan". Empty for a vehicle converted the old way. When set, the
	 * preset's narrower ranges and wheel counts apply when the vehicle is checked and again on upload.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Vehicle")
	FName Preset;

	/**
	 * The uniform size applied to the whole vehicle on export so the Mantle character fits it (#728). The vehicle builder proposes it
	 * from the preset; the creator may change it. 1 is as modelled.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Vehicle", meta = (ClampMin = 0.25, ClampMax = 4.0, DisplayName = "Size"))
	float FitScale = 1.f;

	/** 2 to 8 wheels, each on a bone of the body. At least one steers and one is driven. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Vehicle")
	TArray<FMantleVehicleSourceWheel> Wheels;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Vehicle")
	FMantleVehicleSourceTuning Tuning;

	/** Its paint and liveries (#435 f, #488). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Vehicle")
	FMantleVehicleSourcePaint Paint;

	/**
	 * Its suspension linkage (#435 b): what moves the hubs, control arms and dampers after the wheels, at most 64 rules.
	 * Convert Vehicle detects them when this is empty; empty it and convert again to detect them afresh.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Vehicle")
	TArray<FMantleVehicleSourceLinkage> Linkage;

	/**
	 * Its attached parts (#435 item 2): static meshes riding the body's bones, at most 16. Convert Vehicle finds them when this is
	 * empty (a static mesh attached to a wheel's bone becomes that wheel's Tire Mesh instead); empty it and convert again to
	 * find them afresh.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Vehicle")
	TArray<FMantleVehicleSourcePart> Parts;

	/**
	 * Which simulation builds it (#821): wheeled for a car, truck, six-wheeler or motorcycle, plane for a plane or jet, helicopter, or hover. Anything but
	 * wheeled also needs its block in MovementBlocks. Written into the document as movementType (FMantleVehicleChassisDocument::MergeSource).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement")
	EMantleVehicleMovementKind MovementType = EMantleVehicleMovementKind::Wheeled;

	/** The driver's and the passenger's seat kind (#821): the clip set a seated character plays. A rider's fit (a motorcycle's) is in MovementBlocks. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement")
	EMantleVehicleSeatKind DriverSeatKind = EMantleVehicleSeatKind::Car;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement")
	EMantleVehicleSeatKind PassengerSeatKind = EMantleVehicleSeatKind::Car;

	/**
	 * The movement numbers a document carries, as the JSON object text they have in it: a top-level flight, helicopter or hover block,
	 * and seat (exitSideDistance, rider) and camera (boomLocation) with only those keys. Flat numbers in the body's space at the reference pose, never a
	 * preset id. FMantleVehicleChassisDocument::MovementBlocksJson makes it from a chassis spec; empty for a wheeled vehicle with no rider fit.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement", meta = (MultiLine = true))
	FString MovementBlocks;

	/**
	 * Its library card's picture (#821): a 512 x 512 PNG of the built vehicle, taken when the vehicle builder built it (FMantleVehicleThumbnail). Publishing uploads it
	 * with the vehicle and names it as the library item's thumbnail. Empty for a vehicle built before this, or converted by hand: its card has no picture.
	 */
	UPROPERTY()
	TArray<uint8> ThumbnailPng;
};
