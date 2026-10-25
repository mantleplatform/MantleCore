// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleChassisDocument.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MantleChassisSpec.h"
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehicleSource.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleVehicleChassisDocumentPrivate
{
	TSharedRef<FJsonValue> Number(const double Value)
	{
		return MakeShared<FJsonValueNumber>(Value);
	}

	TSharedRef<FJsonValue> Triple(const FVector& Value)
	{
		const TArray<TSharedPtr<FJsonValue>> Parts = { Number(Value.X), Number(Value.Y), Number(Value.Z) };
		return MakeShared<FJsonValueArray>(Parts);
	}

	/** Object's child object Key, made when it has none. */
	TSharedPtr<FJsonObject> ChildObject(FJsonObject& Object, const TCHAR* Key)
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		if (Object.TryGetObjectField(Key, Found) && Found && Found->IsValid())
		{
			return *Found;
		}
		const TSharedPtr<FJsonObject> Made = MakeShared<FJsonObject>();
		Object.SetObjectField(Key, Made);
		return Made;
	}

	TSharedRef<FJsonValue> ObjectValue(const TSharedRef<FJsonObject>& Object)
	{
		return MakeShared<FJsonValueObject>(Object);
	}

	void SetBone(FJsonObject& Object, const FName Bone)
	{
		if (!Bone.IsNone())
		{
			Object.SetStringField(TEXT("bone"), Bone.ToString());
		}
	}

	TSharedRef<FJsonObject> GripJson(const FMantleVehicleGripDesc& Grip)
	{
		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		SetBone(*Out, Grip.Bone);
		Out->SetField(TEXT("location"), Triple(Grip.Location));
		return Out;
	}

	TSharedRef<FJsonObject> GripPairJson(const FMantleVehicleGripPairDesc& Pair)
	{
		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetObjectField(TEXT("left"), GripJson(Pair.Left));
		Out->SetObjectField(TEXT("right"), GripJson(Pair.Right));
		return Out;
	}

	TArray<TSharedPtr<FJsonValue>> SpinBonesJson(const TArray<FMantleVehicleSpinBoneDesc>& Spins)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FMantleVehicleSpinBoneDesc& Spin : Spins)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("bone"), Spin.Bone.ToString());
			Entry->SetField(TEXT("axis"), Triple(Spin.Axis));
			Entry->SetField(TEXT("rpm"), Number(Spin.Rpm));
			Out.Add(ObjectValue(Entry));
		}
		return Out;
	}

	TSharedRef<FJsonObject> FlightJson(const FMantleVehicleFlightDesc& F)
	{
		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetField(TEXT("airSpeedLimit"), Number(F.AirSpeedLimit));
		Out->SetField(TEXT("cruiseThrottle"), Number(F.CruiseThrottle));
		Out->SetField(TEXT("pitchAuthority"), Number(F.PitchAuthority));
		Out->SetField(TEXT("throttleLeverRate"), Number(F.ThrottleLeverRate));
		Out->SetField(TEXT("linearDamping"), Number(F.LinearDamping));
		Out->SetField(TEXT("angularDamping"), Number(F.AngularDamping));
		TArray<TSharedPtr<FJsonValue>> Aerofoils;
		for (const FMantleVehicleAerofoilDesc& A : F.Aerofoils)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			SetBone(*Entry, A.Bone);
			Entry->SetField(TEXT("location"), Triple(A.Location));
			Entry->SetStringField(TEXT("type"), FMantleVehicleChassisDocument::AerofoilKindName(A.Type));
			Entry->SetBoolField(TEXT("mirror"), A.bMirror);
			Entry->SetField(TEXT("area"), Number(A.AreaM2));
			Entry->SetField(TEXT("camber"), Number(A.Camber));
			Entry->SetField(TEXT("maxControlAngle"), Number(A.MaxControlAngle));
			Entry->SetField(TEXT("stallAngle"), Number(A.StallAngle));
			Aerofoils.Add(ObjectValue(Entry));
		}
		Out->SetArrayField(TEXT("aerofoils"), Aerofoils);
		TArray<TSharedPtr<FJsonValue>> Thrusters;
		for (const FMantleVehicleThrusterDesc& T : F.Thrusters)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			SetBone(*Entry, T.Bone);
			Entry->SetField(TEXT("location"), Triple(T.Location));
			Entry->SetField(TEXT("direction"), Triple(T.Direction));
			Entry->SetField(TEXT("maxThrust"), Number(T.MaxThrustNewtons));
			Thrusters.Add(ObjectValue(Entry));
		}
		Out->SetArrayField(TEXT("thrusters"), Thrusters);
		if (F.SpinBones.Num() > 0)
		{
			Out->SetArrayField(TEXT("spinBones"), SpinBonesJson(F.SpinBones));
		}
		if (F.bPilotGrips)
		{
			Out->SetObjectField(TEXT("pilotGrips"), GripPairJson(F.PilotGrips));
		}
		return Out;
	}

	TSharedRef<FJsonObject> HelicopterJson(const FMantleVehicleHelicopterDesc& H)
	{
		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetField(TEXT("airSpeedLimit"), Number(H.AirSpeedLimit));
		Out->SetField(TEXT("hoverCollective"), Number(H.HoverCollective));
		Out->SetField(TEXT("throttleLeverRate"), Number(H.ThrottleLeverRate));
		Out->SetField(TEXT("linearDamping"), Number(H.LinearDamping));
		Out->SetField(TEXT("angularDamping"), Number(H.AngularDamping));
		TArray<TSharedPtr<FJsonValue>> Rotors;
		for (const FMantleVehicleRotorDesc& R : H.Rotors)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			SetBone(*Entry, R.Bone);
			Entry->SetField(TEXT("location"), Triple(R.Location));
			Entry->SetField(TEXT("axis"), Triple(R.Axis));
			Entry->SetField(TEXT("maxThrust"), Number(R.MaxThrustNewtons));
			Entry->SetField(TEXT("pitchAcceleration"), Number(R.PitchAcceleration));
			Entry->SetField(TEXT("rollAcceleration"), Number(R.RollAcceleration));
			Entry->SetField(TEXT("yawAcceleration"), Number(R.YawAcceleration));
			Entry->SetBoolField(TEXT("stabilityAssist"), R.bStabilityAssist);
			Entry->SetField(TEXT("stabilityStiffness"), Number(R.StabilityStiffness));
			Entry->SetField(TEXT("stabilityDamping"), Number(R.StabilityDamping));
			Entry->SetField(TEXT("stabilityMaxAcceleration"), Number(R.StabilityMaxAcceleration));
			Rotors.Add(ObjectValue(Entry));
		}
		Out->SetArrayField(TEXT("rotors"), Rotors);
		if (H.SpinBones.Num() > 0)
		{
			Out->SetArrayField(TEXT("spinBones"), SpinBonesJson(H.SpinBones));
		}
		if (H.bPilotGrips)
		{
			Out->SetObjectField(TEXT("pilotGrips"), GripPairJson(H.PilotGrips));
		}
		return Out;
	}

	TSharedRef<FJsonObject> HoverJson(const FMantleVehicleHoverDesc& H)
	{
		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Pads;
		for (const FMantleVehicleHoverPadDesc& Pad : H.Pads)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetField(TEXT("location"), Triple(Pad.Location));
			Entry->SetField(TEXT("maxRaise"), Number(Pad.MaxRaise));
			Entry->SetField(TEXT("maxDrop"), Number(Pad.MaxDrop));
			Entry->SetField(TEXT("springRate"), Number(Pad.SpringRate));
			Entry->SetField(TEXT("springDamping"), Number(Pad.SpringDamping));
			Pads.Add(ObjectValue(Entry));
		}
		Out->SetArrayField(TEXT("pads"), Pads);
		const TSharedRef<FJsonObject> Fan = MakeShared<FJsonObject>();
		const FMantleVehicleHoverThrusterDesc& T = H.Thruster;
		Fan->SetField(TEXT("location"), Triple(T.Location));
		Fan->SetField(TEXT("direction"), Triple(T.Direction));
		Fan->SetField(TEXT("maxThrust"), Number(T.MaxThrustNewtons));
		Fan->SetField(TEXT("reverseShare"), Number(T.ReverseShare));
		Fan->SetField(TEXT("vectorAngle"), Number(T.VectorAngleDegrees));
		Fan->SetField(TEXT("topSpeed"), Number(T.TopSpeedCmPerSec));
		Fan->SetField(TEXT("brakeFade"), Number(T.BrakeFadeCmPerSec));
		Out->SetObjectField(TEXT("thruster"), Fan);
		if (H.bRudder)
		{
			const TSharedRef<FJsonObject> Fin = MakeShared<FJsonObject>();
			Fin->SetField(TEXT("location"), Triple(H.Rudder.Location));
			Fin->SetField(TEXT("area"), Number(H.Rudder.AreaM2));
			Fin->SetField(TEXT("camber"), Number(H.Rudder.Camber));
			Fin->SetField(TEXT("maxControlAngle"), Number(H.Rudder.MaxControlAngle));
			Fin->SetField(TEXT("stallAngle"), Number(H.Rudder.StallAngle));
			Out->SetObjectField(TEXT("rudder"), Fin);
		}
		const TSharedRef<FJsonObject> Damping = MakeShared<FJsonObject>();
		Damping->SetField(TEXT("lateralPerSecond"), Number(H.Damping.LateralPerSecond));
		Damping->SetField(TEXT("maxLateralAccel"), Number(H.Damping.MaxLateralAccel));
		Damping->SetField(TEXT("yawPerSecond"), Number(H.Damping.YawPerSecond));
		Damping->SetField(TEXT("maxYawAccel"), Number(H.Damping.MaxYawAccelDegrees));
		Damping->SetField(TEXT("skirtBrakePerSecond"), Number(H.Damping.SkirtBrakePerSecond));
		Damping->SetField(TEXT("maxSkirtBrakeAccel"), Number(H.Damping.MaxSkirtBrakeAccel));
		Out->SetObjectField(TEXT("damping"), Damping);
		Out->SetField(TEXT("angularDamping"), Number(H.AngularDamping));
		return Out;
	}

	TSharedRef<FJsonObject> RiderJson(const FMantleVehicleRiderDesc& R)
	{
		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("forkBone"), R.ForkBone.ToString());
		Out->SetObjectField(TEXT("handlebar"), GripPairJson(R.Handlebar));
		Out->SetObjectField(TEXT("footPegs"), GripPairJson(R.FootPegs));
		Out->SetField(TEXT("pelvisOffset"), Triple(R.PelvisOffset));
		Out->SetField(TEXT("stopPelvisShift"), Triple(R.StopPelvisShift));
		Out->SetField(TEXT("kneeSplayDegrees"), Number(R.KneeSplayDegrees));
		if (R.bRearTireFriction)
		{
			Out->SetField(TEXT("rearTireFriction"), Number(R.RearTireFriction));
		}
		if (R.bRearBrakeTorque)
		{
			Out->SetField(TEXT("rearBrakeTorque"), Number(R.RearBrakeTorque));
		}
		const FMantleVehicleRotationControlDesc& C = R.RotationControl;
		const TSharedRef<FJsonObject> Control = MakeShared<FJsonObject>();
		Control->SetBoolField(TEXT("torqueControl"), C.bTorqueControl);
		Control->SetField(TEXT("torqueRotationDamping"), Number(C.TorqueRotationDamping));
		Control->SetBoolField(TEXT("targetRotation"), C.bTargetRotation);
		Control->SetBoolField(TEXT("rollVsSpeed"), C.bRollVsSpeed);
		Control->SetField(TEXT("rollControlScaling"), Number(C.RollControlScaling));
		Control->SetField(TEXT("rollMaxAngle"), Number(C.RollMaxAngle));
		Control->SetField(TEXT("pitchControlScaling"), Number(C.PitchControlScaling));
		Control->SetField(TEXT("pitchMaxAngle"), Number(C.PitchMaxAngle));
		Control->SetField(TEXT("rotationStiffness"), Number(C.RotationStiffness));
		Control->SetField(TEXT("rotationDamping"), Number(C.RotationDamping));
		Control->SetField(TEXT("maxAccel"), Number(C.MaxAccel));
		Control->SetField(TEXT("autoCentreRollStrength"), Number(C.AutoCentreRollStrength));
		Control->SetField(TEXT("autoCentrePitchStrength"), Number(C.AutoCentrePitchStrength));
		Control->SetField(TEXT("autoCentreYawStrength"), Number(C.AutoCentreYawStrength));
		Out->SetObjectField(TEXT("rotationControl"), Control);
		return Out;
	}

	FMantleVehicleGripDesc MakeGrip(const FName Bone, const FVector& Location)
	{
		FMantleVehicleGripDesc Grip;
		Grip.Bone = Bone;
		Grip.Location = Location;
		return Grip;
	}

	/** A spin bone about Axis. */
	FMantleVehicleSpinBoneDesc MakeSpin(const FName Bone, const FVector& Axis, const double Rpm)
	{
		FMantleVehicleSpinBoneDesc Spin;
		Spin.Bone = Bone;
		Spin.Axis = Axis;
		Spin.Rpm = Rpm;
		return Spin;
	}

	/** The seat kind a preset's seats.driver names: car (anything else), motorcycle or pilot. */
	EMantleVehicleSeatKind SeatKindOf(const FString& DriverSeat)
	{
		if (DriverSeat.Equals(TEXT("motorcycle"), ESearchCase::CaseSensitive))
		{
			return EMantleVehicleSeatKind::Motorcycle;
		}
		return DriverSeat.Equals(TEXT("pilot"), ESearchCase::CaseSensitive) ? EMantleVehicleSeatKind::Pilot : EMantleVehicleSeatKind::Car;
	}

	/** A motorcycle's rider fit: the handlebar on the fork bone, the pegs on the root, the offsets, the rear wheel's own grip and the balance (ApplySeatKind). */
	void DeriveRider(const FMantleChassisSpec& Spec, FMantleVehicleDesc& D)
	{
		const FMantleVehicleChassisRider& Rider = Spec.Rider;
		if (!Rider.bSet)
		{
			return;
		}
		const FName Root = FMantleVehicleChassisBuilder::RootBoneName();
		FMantleVehicleRiderDesc& Out = D.Rider;
		Out.ForkBone = Spec.ForkBone;
		Out.Handlebar.Left = MakeGrip(Spec.ForkBone, Rider.LeftGrip);
		Out.Handlebar.Right = MakeGrip(Spec.ForkBone, Rider.RightGrip);
		Out.FootPegs.Left = MakeGrip(Root, Rider.LeftPeg);
		Out.FootPegs.Right = MakeGrip(Root, Rider.RightPeg);
		Out.PelvisOffset = Rider.PelvisOffset;
		Out.StopPelvisShift = Rider.StopPelvisShift;
		Out.KneeSplayDegrees = Rider.KneeSplayDegrees;
		// The document's one friction and brake torque are the front wheel's; the rear has its own, when the preset gives one.
		Out.bRearTireFriction = Rider.RearTireFriction > 0.0;
		Out.RearTireFriction = Out.bRearTireFriction ? Rider.RearTireFriction : 0.0;
		Out.bRearBrakeTorque = Rider.RearBrakeTorque > 0.0;
		Out.RearBrakeTorque = Out.bRearBrakeTorque ? Rider.RearBrakeTorque : 0.0;
		const FMantleVehicleChassisRotationControl& Control = Rider.RotationControl;
		FMantleVehicleRotationControlDesc& C = Out.RotationControl;
		C.bTorqueControl = Control.bTorqueControl;
		C.TorqueRotationDamping = Control.TorqueRotationDamping;
		C.bTargetRotation = Control.bTargetRotation;
		C.bRollVsSpeed = Control.bRollVsSpeed;
		C.RollControlScaling = Control.RollControlScaling;
		C.RollMaxAngle = Control.RollMaxAngle;
		C.PitchControlScaling = Control.PitchControlScaling;
		C.PitchMaxAngle = Control.PitchMaxAngle;
		C.RotationStiffness = Control.RotationStiffness;
		C.RotationDamping = Control.RotationDamping;
		C.MaxAccel = Control.MaxAccel;
		C.AutoCentreRollStrength = Control.AutoCentreRollStrength;
		C.AutoCentrePitchStrength = Control.AutoCentrePitchStrength;
		C.AutoCentreYawStrength = Control.AutoCentreYawStrength;
		D.bRider = true;
		D.bSeatExitSideDistance = true;
		D.SeatExitSideDistance = FMantleVehicleChassisDocument::BikeExitSideDistance;
	}

	/** A plane's airframe (ApplyAircraft): the wing's two halves, the elevator and the rudder, the engines and their propellers, the pilot's hands. */
	void DeriveAircraft(const FMantleChassisSpec& Spec, const FMantleChassisHandling& Handling, FMantleVehicleDesc& D)
	{
		const FMantleChassisAircraft& Plane = Spec.Aircraft;
		const FName Root = FMantleVehicleChassisBuilder::RootBoneName();
		D.MovementType = EMantleVehicleMovementKind::Plane;
		FMantleVehicleFlightDesc& F = D.Flight;
		F.AirSpeedLimit = Plane.AirSpeedLimitCmPerSec;
		F.CruiseThrottle = Plane.CruiseThrottle;
		F.PitchAuthority = FMantleVehicleChassisDocument::PlanePitchAuthority;
		// The surfaces are the plane's drag: the rigid body's damping would take mass x velocity x damping off it on top.
		F.LinearDamping = 0.0;
		F.AngularDamping = 0.0;

		// The wing's two halves (the right one mirrored, so its aileron goes the other way), the elevator and the rudder; each on its bone. A half-wing is half
		// the wing's area; the tailplane's camber is the plane's trim.
		for (const bool bRight : { false, true })
		{
			FMantleVehicleAerofoilDesc& Wing = F.Aerofoils.AddDefaulted_GetRef();
			Wing.Bone = FMantleVehicleChassisBuilder::AileronBoneName(bRight);
			Wing.Location = Spec.WingAerofoilLocation(bRight);
			Wing.Type = EMantleVehicleAerofoilKind::Wing;
			Wing.bMirror = bRight;
			Wing.AreaM2 = 0.5 * Handling.WingAreaM2;
			Wing.Camber = Plane.Wing.Camber;
			Wing.MaxControlAngle = Plane.Wing.MaxControlAngle;
			Wing.StallAngle = Plane.Wing.StallAngle;
		}
		FMantleVehicleAerofoilDesc& Elevator = F.Aerofoils.AddDefaulted_GetRef();
		Elevator.Bone = FMantleVehicleChassisBuilder::ElevatorBoneName();
		Elevator.Location = Spec.ElevatorLocation();
		Elevator.Type = EMantleVehicleAerofoilKind::Elevator;
		Elevator.AreaM2 = Handling.TailplaneAreaM2;
		Elevator.Camber = Plane.Tailplane.Camber;
		Elevator.MaxControlAngle = Plane.Tailplane.MaxControlAngle;
		Elevator.StallAngle = Plane.Tailplane.StallAngle;
		FMantleVehicleAerofoilDesc& Rudder = F.Aerofoils.AddDefaulted_GetRef();
		Rudder.Bone = FMantleVehicleChassisBuilder::RudderBoneName();
		Rudder.Location = Spec.RudderLocation();
		Rudder.Type = EMantleVehicleAerofoilKind::Rudder;
		Rudder.AreaM2 = Handling.FinAreaM2;
		Rudder.Camber = Plane.Fin.Camber;
		Rudder.MaxControlAngle = Plane.Fin.MaxControlAngle;
		Rudder.StallAngle = Plane.Fin.StallAngle;

		// An engine pushes forward from its propeller's hub, and the propeller spins about X (the right one of two the other way). A jet (an airframe with no
		// propeller) pushes from its nozzle behind the tail, and nothing of it spins.
		for (int32 Engine = 0; Engine < Plane.EngineCount; ++Engine)
		{
			const FName EngineBone = FMantleVehicleChassisBuilder::EngineBoneName(Plane, Engine);
			FMantleVehicleThrusterDesc& Push = F.Thrusters.AddDefaulted_GetRef();
			Push.Bone = EngineBone;
			Push.Location = Spec.EngineLocation(Engine);
			Push.Direction = FVector::ForwardVector;
			Push.MaxThrustNewtons = Handling.EngineThrustNewtons;
			if (Plane.bPropeller)
			{
				F.SpinBones.Add(MakeSpin(EngineBone, FVector::ForwardVector, Engine == 1 ? -Plane.PropellerRpm : Plane.PropellerRpm));
			}
		}

		// The pilot's hands: the left on the throttle, the right on the stick, both on the root bone (neither lever is modelled to move).
		F.bPilotGrips = true;
		F.PilotGrips.Left = MakeGrip(Root, Plane.ThrottleGrip);
		F.PilotGrips.Right = MakeGrip(Root, Plane.StickGrip);

		// The chase camera orbits the cockpit's height over the plane's centre of mass; a pilot steps out beside the fuselage.
		D.bCameraBoom = true;
		D.CameraBoomLocation = FVector(Spec.CenterOfMass.X, 0.0, Spec.Seat.Z + 7.0 * Spec.Scale);
		D.bSeatExitSideDistance = true;
		D.SeatExitSideDistance = 0.5 * Spec.Width + 145.0 * Spec.Scale;
	}

	/** A helicopter's rotors (ApplyHelicopter): the main rotor at its hub, the spinning rotors, the pilot's hands. */
	void DeriveHelicopter(const FMantleChassisSpec& Spec, const FMantleChassisHandling& Handling, FMantleVehicleDesc& D)
	{
		const FMantleChassisHelicopter& Heli = Spec.Helicopter;
		const FName Root = FMantleVehicleChassisBuilder::RootBoneName();
		D.MovementType = EMantleVehicleMovementKind::Helicopter;
		FMantleVehicleHelicopterDesc& H = D.Helicopter;
		H.AirSpeedLimit = Heli.AirSpeedLimitCmPerSec;
		// The collective that holds the weight: a ratio, so a resized helicopter, whose thrust follows its mass, keeps it.
		H.HoverCollective = Heli.HoverCollective;
		H.ThrottleLeverRate = FMantleVehicleChassisDocument::HelicopterCollectiveRate;
		H.LinearDamping = FMantleVehicleChassisDocument::HelicopterLinearDamping;
		// No angular damping: the rotor's stability assist damps its turning.
		H.AngularDamping = 0.0;

		// The main rotor, at its hub over the centre of mass; the rotor's other numbers are its defaults, which act through the body's inertia, so at any size.
		FMantleVehicleRotorDesc& Rotor = H.Rotors.AddDefaulted_GetRef();
		Rotor.Bone = FMantleVehicleChassisBuilder::MainRotorBoneName();
		Rotor.Location = Spec.MainRotorHub();
		Rotor.Axis = FVector::UpVector;
		Rotor.MaxThrustNewtons = Handling.RotorThrustNewtons;

		// The main rotor spins about Z and the tail rotor about Y, both while someone is aboard.
		H.SpinBones.Add(MakeSpin(FMantleVehicleChassisBuilder::MainRotorBoneName(), FVector::UpVector, Heli.RotorRpm));
		H.SpinBones.Add(MakeSpin(FMantleVehicleChassisBuilder::TailRotorBoneName(), FVector::RightVector, Heli.TailRotorRpm));

		// The pilot's hands: the left on the collective, the right on the cyclic, both on the root bone.
		H.bPilotGrips = true;
		H.PilotGrips.Left = MakeGrip(Root, Heli.CollectiveGrip);
		H.PilotGrips.Right = MakeGrip(Root, Heli.CyclicGrip);

		// The chase camera orbits the cockpit's height over the centre of mass, as a plane's; the pilot steps out beside the cabin, past the skids.
		D.bCameraBoom = true;
		D.CameraBoomLocation = FVector(Spec.CenterOfMass.X, 0.0, Spec.Seat.Z + 7.0 * Spec.Scale);
		D.bSeatExitSideDistance = true;
		D.SeatExitSideDistance = FMath::Max(0.5 * Spec.Width, 0.5 * Spec.Track) + 145.0 * Spec.Scale;
	}

	/** A hovercraft's pads, fan, fin and damping (ApplyHovercraft): a pad where each of the layout's wheels is, on the hull's underside. */
	void DeriveHovercraft(const FMantleChassisSpec& Spec, const FMantleChassisHandling& Handling, FMantleVehicleDesc& D)
	{
		const FMantleChassisHovercraft& Craft = Spec.Hovercraft;
		D.MovementType = EMantleVehicleMovementKind::Hover;
		FMantleVehicleHoverDesc& H = D.Hover;
		for (int32 Index = 0; Index < Spec.WheelCount(); ++Index)
		{
			FMantleVehicleHoverPadDesc& Pad = H.Pads.AddDefaulted_GetRef();
			Pad.Location = Spec.PadLocation(Index);
			Pad.MaxRaise = Craft.PadRaise;
			Pad.MaxDrop = Craft.PadTravel;
			Pad.SpringRate = Handling.PadSpringRate;
			Pad.SpringDamping = Craft.PadDamping;
		}
		// The rigid body's angular damping, the same on the ground and off it: the pads hold its pitch and roll, the hover chassis its yaw.
		H.AngularDamping = FMantleVehicleChassisDocument::HovercraftAngularDamping;

		// The fan pushes forward from its pod, falling off to nothing at the document's top speed; its reverse vanes and its steering as the preset says.
		FMantleVehicleHoverThrusterDesc& Fan = H.Thruster;
		Fan.Location = Spec.FanLocation();
		Fan.Direction = FVector::ForwardVector;
		Fan.MaxThrustNewtons = Handling.HoverThrustNewtons;
		Fan.ReverseShare = Craft.ReverseShare;
		Fan.VectorAngleDegrees = Craft.VectorAngle;
		Fan.TopSpeedCmPerSec = Handling.MaxSpeed;

		// The fin's rudder, at the fin's middle, on the steering.
		H.bRudder = true;
		H.Rudder.Location = Spec.HoverRudderLocation();
		H.Rudder.AreaM2 = Handling.HoverFinAreaM2;
		H.Rudder.Camber = 0.0;
		H.Rudder.MaxControlAngle = Craft.FinMaxControlAngle;
		H.Rudder.StallAngle = Craft.FinStallAngle;

		// What stands in for a tyre's grip, and the skirt brake.
		H.Damping.LateralPerSecond = Craft.LateralPerSecond;
		H.Damping.MaxLateralAccel = Craft.MaxLateralAccel;
		H.Damping.YawPerSecond = Craft.YawPerSecond;
		H.Damping.MaxYawAccelDegrees = Craft.MaxYawAccelDegrees;
		H.Damping.SkirtBrakePerSecond = Craft.SkirtBrakePerSecond;
		H.Damping.MaxSkirtBrakeAccel = Craft.MaxSkirtBrakeAccel;
	}

	/** The seat extras (kind, exit distance, rider) of Desc, and the boom, put on Document's seat and camera. */
	void WriteSeatAndCamera(const FMantleVehicleDesc& Desc, FJsonObject& Document)
	{
		if (Desc.SeatKind != EMantleVehicleSeatKind::Car || Desc.bSeatExitSideDistance || Desc.bRider)
		{
			const TSharedPtr<FJsonObject> Seat = ChildObject(Document, TEXT("seat"));
			if (Desc.SeatKind != EMantleVehicleSeatKind::Car)
			{
				Seat->SetStringField(TEXT("kind"), FMantleVehicleChassisDocument::SeatKindName(Desc.SeatKind));
			}
			if (Desc.bSeatExitSideDistance)
			{
				Seat->SetField(TEXT("exitSideDistance"), Number(Desc.SeatExitSideDistance));
			}
			if (Desc.bRider)
			{
				Seat->SetObjectField(TEXT("rider"), RiderJson(Desc.Rider));
			}
		}
		if (Desc.PassengerSeatKind != EMantleVehicleSeatKind::Car)
		{
			const TSharedPtr<FJsonObject>* Passenger = nullptr;
			if (Document.TryGetObjectField(TEXT("passengerSeat"), Passenger) && Passenger && Passenger->IsValid())
			{
				(*Passenger)->SetStringField(TEXT("kind"), FMantleVehicleChassisDocument::SeatKindName(Desc.PassengerSeatKind));
			}
		}
		if (Desc.bCameraBoom)
		{
			ChildObject(Document, TEXT("camera"))->SetField(TEXT("boomLocation"), Triple(Desc.CameraBoomLocation));
		}
	}

	/**
	 * Holds every number of the movement part to its contract range (limits.json placeAssets.vehicle.ranges), as FMantleChassisSpec::ComposeDocument holds the
	 * collision boxes to the document's +-1000 cm: a document cannot say more, and a chassis resized past it (a plane 1.3 times the preset's size has a tail 11 m
	 * back) would otherwise not read at all. Each number held is one line in OutHeld.
	 */
	void HoldToRanges(FMantleVehicleDesc& D, TArray<FString>* OutHeld)
	{
		auto Hold = [OutHeld](const TCHAR* Path, double& Value)
		{
			double Min = 0.0;
			double Max = 0.0;
			if (!FMantleVehicleLimits::GetRange(Path, Min, Max))
			{
				return;
			}
			const double Held = FMath::Clamp(Value, Min, Max);
			if (Held != Value)
			{
				if (OutHeld)
				{
					OutHeld->Add(FString::Printf(TEXT("%s %g held to %g"), Path, Value, Held));
				}
				Value = Held;
			}
		};
		auto HoldVector = [&Hold](const TCHAR* Path, FVector& Value)
		{
			Hold(Path, Value.X);
			Hold(Path, Value.Y);
			Hold(Path, Value.Z);
		};
		auto HoldGrips = [&HoldVector](const FString& Block, FMantleVehicleGripPairDesc& Pair)
		{
			HoldVector(*(Block + TEXT("/left/location/*")), Pair.Left.Location);
			HoldVector(*(Block + TEXT("/right/location/*")), Pair.Right.Location);
		};

		if (D.bCameraBoom)
		{
			HoldVector(TEXT("camera/boomLocation/*"), D.CameraBoomLocation);
		}
		if (D.bSeatExitSideDistance)
		{
			Hold(TEXT("seat/exitSideDistance"), D.SeatExitSideDistance);
		}
		if (D.bRider)
		{
			FMantleVehicleRiderDesc& R = D.Rider;
			HoldGrips(TEXT("seat/rider/handlebar"), R.Handlebar);
			HoldGrips(TEXT("seat/rider/footPegs"), R.FootPegs);
			HoldVector(TEXT("seat/rider/pelvisOffset/*"), R.PelvisOffset);
			HoldVector(TEXT("seat/rider/stopPelvisShift/*"), R.StopPelvisShift);
			Hold(TEXT("seat/rider/kneeSplayDegrees"), R.KneeSplayDegrees);
			if (R.bRearTireFriction)
			{
				Hold(TEXT("seat/rider/rearTireFriction"), R.RearTireFriction);
			}
			if (R.bRearBrakeTorque)
			{
				Hold(TEXT("seat/rider/rearBrakeTorque"), R.RearBrakeTorque);
			}
			FMantleVehicleRotationControlDesc& C = R.RotationControl;
			Hold(TEXT("seat/rider/rotationControl/torqueRotationDamping"), C.TorqueRotationDamping);
			Hold(TEXT("seat/rider/rotationControl/rollControlScaling"), C.RollControlScaling);
			Hold(TEXT("seat/rider/rotationControl/rollMaxAngle"), C.RollMaxAngle);
			Hold(TEXT("seat/rider/rotationControl/pitchControlScaling"), C.PitchControlScaling);
			Hold(TEXT("seat/rider/rotationControl/pitchMaxAngle"), C.PitchMaxAngle);
			Hold(TEXT("seat/rider/rotationControl/rotationStiffness"), C.RotationStiffness);
			Hold(TEXT("seat/rider/rotationControl/rotationDamping"), C.RotationDamping);
			Hold(TEXT("seat/rider/rotationControl/maxAccel"), C.MaxAccel);
			Hold(TEXT("seat/rider/rotationControl/autoCentreRollStrength"), C.AutoCentreRollStrength);
			Hold(TEXT("seat/rider/rotationControl/autoCentrePitchStrength"), C.AutoCentrePitchStrength);
			Hold(TEXT("seat/rider/rotationControl/autoCentreYawStrength"), C.AutoCentreYawStrength);
		}

		if (D.MovementType == EMantleVehicleMovementKind::Plane)
		{
			FMantleVehicleFlightDesc& F = D.Flight;
			Hold(TEXT("flight/airSpeedLimit"), F.AirSpeedLimit);
			Hold(TEXT("flight/cruiseThrottle"), F.CruiseThrottle);
			Hold(TEXT("flight/pitchAuthority"), F.PitchAuthority);
			Hold(TEXT("flight/throttleLeverRate"), F.ThrottleLeverRate);
			Hold(TEXT("flight/linearDamping"), F.LinearDamping);
			Hold(TEXT("flight/angularDamping"), F.AngularDamping);
			for (FMantleVehicleAerofoilDesc& A : F.Aerofoils)
			{
				HoldVector(TEXT("flight/aerofoils/*/location/*"), A.Location);
				Hold(TEXT("flight/aerofoils/*/area"), A.AreaM2);
				Hold(TEXT("flight/aerofoils/*/camber"), A.Camber);
				Hold(TEXT("flight/aerofoils/*/maxControlAngle"), A.MaxControlAngle);
				Hold(TEXT("flight/aerofoils/*/stallAngle"), A.StallAngle);
			}
			for (FMantleVehicleThrusterDesc& T : F.Thrusters)
			{
				HoldVector(TEXT("flight/thrusters/*/location/*"), T.Location);
				Hold(TEXT("flight/thrusters/*/maxThrust"), T.MaxThrustNewtons);
			}
			for (FMantleVehicleSpinBoneDesc& Spin : F.SpinBones)
			{
				Hold(TEXT("flight/spinBones/*/rpm"), Spin.Rpm);
			}
			if (F.bPilotGrips)
			{
				HoldGrips(TEXT("flight/pilotGrips"), F.PilotGrips);
			}
		}
		if (D.MovementType == EMantleVehicleMovementKind::Helicopter)
		{
			FMantleVehicleHelicopterDesc& H = D.Helicopter;
			Hold(TEXT("helicopter/airSpeedLimit"), H.AirSpeedLimit);
			Hold(TEXT("helicopter/hoverCollective"), H.HoverCollective);
			Hold(TEXT("helicopter/throttleLeverRate"), H.ThrottleLeverRate);
			Hold(TEXT("helicopter/linearDamping"), H.LinearDamping);
			Hold(TEXT("helicopter/angularDamping"), H.AngularDamping);
			for (FMantleVehicleRotorDesc& R : H.Rotors)
			{
				HoldVector(TEXT("helicopter/rotors/*/location/*"), R.Location);
				Hold(TEXT("helicopter/rotors/*/maxThrust"), R.MaxThrustNewtons);
				Hold(TEXT("helicopter/rotors/*/pitchAcceleration"), R.PitchAcceleration);
				Hold(TEXT("helicopter/rotors/*/rollAcceleration"), R.RollAcceleration);
				Hold(TEXT("helicopter/rotors/*/yawAcceleration"), R.YawAcceleration);
				Hold(TEXT("helicopter/rotors/*/stabilityStiffness"), R.StabilityStiffness);
				Hold(TEXT("helicopter/rotors/*/stabilityDamping"), R.StabilityDamping);
				Hold(TEXT("helicopter/rotors/*/stabilityMaxAcceleration"), R.StabilityMaxAcceleration);
			}
			for (FMantleVehicleSpinBoneDesc& Spin : H.SpinBones)
			{
				Hold(TEXT("helicopter/spinBones/*/rpm"), Spin.Rpm);
			}
			if (H.bPilotGrips)
			{
				HoldGrips(TEXT("helicopter/pilotGrips"), H.PilotGrips);
			}
		}
		if (D.MovementType == EMantleVehicleMovementKind::Hover)
		{
			FMantleVehicleHoverDesc& H = D.Hover;
			for (FMantleVehicleHoverPadDesc& Pad : H.Pads)
			{
				HoldVector(TEXT("hover/pads/*/location/*"), Pad.Location);
				Hold(TEXT("hover/pads/*/maxRaise"), Pad.MaxRaise);
				Hold(TEXT("hover/pads/*/maxDrop"), Pad.MaxDrop);
				Hold(TEXT("hover/pads/*/springRate"), Pad.SpringRate);
				Hold(TEXT("hover/pads/*/springDamping"), Pad.SpringDamping);
			}
			FMantleVehicleHoverThrusterDesc& T = H.Thruster;
			HoldVector(TEXT("hover/thruster/location/*"), T.Location);
			Hold(TEXT("hover/thruster/maxThrust"), T.MaxThrustNewtons);
			Hold(TEXT("hover/thruster/reverseShare"), T.ReverseShare);
			Hold(TEXT("hover/thruster/vectorAngle"), T.VectorAngleDegrees);
			Hold(TEXT("hover/thruster/topSpeed"), T.TopSpeedCmPerSec);
			Hold(TEXT("hover/thruster/brakeFade"), T.BrakeFadeCmPerSec);
			if (H.bRudder)
			{
				HoldVector(TEXT("hover/rudder/location/*"), H.Rudder.Location);
				Hold(TEXT("hover/rudder/area"), H.Rudder.AreaM2);
				Hold(TEXT("hover/rudder/camber"), H.Rudder.Camber);
				Hold(TEXT("hover/rudder/maxControlAngle"), H.Rudder.MaxControlAngle);
				Hold(TEXT("hover/rudder/stallAngle"), H.Rudder.StallAngle);
			}
			Hold(TEXT("hover/damping/lateralPerSecond"), H.Damping.LateralPerSecond);
			Hold(TEXT("hover/damping/maxLateralAccel"), H.Damping.MaxLateralAccel);
			Hold(TEXT("hover/damping/yawPerSecond"), H.Damping.YawPerSecond);
			Hold(TEXT("hover/damping/maxYawAccel"), H.Damping.MaxYawAccelDegrees);
			Hold(TEXT("hover/damping/skirtBrakePerSecond"), H.Damping.SkirtBrakePerSecond);
			Hold(TEXT("hover/damping/maxSkirtBrakeAccel"), H.Damping.MaxSkirtBrakeAccel);
			Hold(TEXT("hover/angularDamping"), H.AngularDamping);
		}
	}

	/** What a document path is, for a creator: the first row whose prefix the path starts with. */
	struct FPlainPath
	{
		const TCHAR* Prefix;
		const TCHAR* Phrase;
	};

	const FPlainPath PlainPaths[] = {
		{ TEXT("camera/boomLocation"), TEXT("The chase camera's position") },
		{ TEXT("seat/exitSideDistance"), TEXT("How far to the side the driver steps out") },
		{ TEXT("seat/rider"), TEXT("The rider's pose") },
		{ TEXT("tires/friction"), TEXT("The front tyres' grip") },
		{ TEXT("tires/rearFriction"), TEXT("The rear tyres' grip") },
		{ TEXT("flight/airSpeedLimit"), TEXT("The air-speed limit") },
		{ TEXT("flight/cruiseThrottle"), TEXT("The cruise throttle") },
		{ TEXT("flight/aerofoils/*/location"), TEXT("The position of a wing, tail or fin") },
		{ TEXT("flight/aerofoils/*/area"), TEXT("The area of a wing, tail or fin") },
		{ TEXT("flight/aerofoils/*/camber"), TEXT("The camber of a wing, tail or fin") },
		{ TEXT("flight/aerofoils/*/maxControlAngle"), TEXT("How far an aileron, elevator or rudder turns") },
		{ TEXT("flight/aerofoils/*/stallAngle"), TEXT("The stall angle of a wing, tail or fin") },
		{ TEXT("flight/thrusters/*/location"), TEXT("The position of an engine") },
		{ TEXT("flight/thrusters/*/maxThrust"), TEXT("An engine's thrust") },
		{ TEXT("flight/spinBones"), TEXT("A propeller's spin") },
		{ TEXT("flight/pilotGrips"), TEXT("Where the pilot's hands hold") },
		{ TEXT("flight"), TEXT("A flight setting") },
		{ TEXT("helicopter/rotors/*/location"), TEXT("The position of the rotor") },
		{ TEXT("helicopter/rotors/*/maxThrust"), TEXT("The rotor's lift") },
		{ TEXT("helicopter/rotors"), TEXT("A rotor's handling") },
		{ TEXT("helicopter/spinBones"), TEXT("A rotor's spin") },
		{ TEXT("helicopter/pilotGrips"), TEXT("Where the pilot's hands hold") },
		{ TEXT("helicopter"), TEXT("A helicopter setting") },
		{ TEXT("hover/pads/*/location"), TEXT("The position of a hover pad") },
		{ TEXT("hover/pads"), TEXT("A hover pad's spring") },
		{ TEXT("hover/thruster"), TEXT("The hover fan") },
		{ TEXT("hover/rudder"), TEXT("The hovercraft's rudder") },
		{ TEXT("hover"), TEXT("A hovercraft setting") },
	};

	/**
	 * One OutHeld line ("path value held to limit") in plain words. The line is empty when it is not in that form. Which way it was held says the most or the
	 * least Mantle allows.
	 */
	FString PlainHeld(const FString& Line, FString& OutKey)
	{
		TArray<FString> Parts;
		Line.ParseIntoArray(Parts, TEXT(" "));
		if (Parts.Num() != 5 || Parts[2] != TEXT("held") || Parts[3] != TEXT("to"))
		{
			return FString();
		}
		const FString& Path = Parts[0];
		FString Phrase = FString::Printf(TEXT("The %s setting"), *Path);
		for (const FPlainPath& Row : PlainPaths)
		{
			if (Path.StartsWith(Row.Prefix, ESearchCase::CaseSensitive))
			{
				Phrase = Row.Phrase;
				break;
			}
		}
		const double Value = FCString::Atod(*Parts[1]);
		const double Held = FCString::Atod(*Parts[4]);
		OutKey = Phrase + (Value > Held ? TEXT("+") : TEXT("-"));
		return FString::Printf(TEXT("%s would be %s, past what Mantle allows: it is held to %s (the %s)."), *Phrase, *Parts[1], *Parts[4], Value > Held ? TEXT("most") : TEXT("least"));
	}

	FString Condensed(const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object, Writer);
		return Text;
	}
}

const TCHAR* FMantleVehicleChassisDocument::MovementName(const EMantleVehicleMovementKind Movement)
{
	switch (Movement)
	{
	case EMantleVehicleMovementKind::Plane:
		return TEXT("plane");
	case EMantleVehicleMovementKind::Helicopter:
		return TEXT("helicopter");
	case EMantleVehicleMovementKind::Hover:
		return TEXT("hover");
	default:
		return TEXT("wheeled");
	}
}

const TCHAR* FMantleVehicleChassisDocument::SeatKindName(const EMantleVehicleSeatKind Kind)
{
	switch (Kind)
	{
	case EMantleVehicleSeatKind::Motorcycle:
		return TEXT("motorcycle");
	case EMantleVehicleSeatKind::Pilot:
		return TEXT("pilot");
	default:
		return TEXT("car");
	}
}

const TCHAR* FMantleVehicleChassisDocument::AerofoilKindName(const EMantleVehicleAerofoilKind Kind)
{
	switch (Kind)
	{
	case EMantleVehicleAerofoilKind::Wing:
		return TEXT("wing");
	case EMantleVehicleAerofoilKind::Elevator:
		return TEXT("elevator");
	case EMantleVehicleAerofoilKind::Rudder:
		return TEXT("rudder");
	default:
		return TEXT("fixed");
	}
}

void FMantleVehicleChassisDocument::Derive(const FMantleChassisSpec& Spec, FMantleVehicleDesc& InOutDesc, TArray<FString>* OutHeld)
{
	using namespace MantleVehicleChassisDocumentPrivate;

	// Reset the movement part: a Desc is derived once however often it is asked.
	InOutDesc.MovementType = EMantleVehicleMovementKind::Wheeled;
	InOutDesc.Flight = FMantleVehicleFlightDesc();
	InOutDesc.Helicopter = FMantleVehicleHelicopterDesc();
	InOutDesc.Hover = FMantleVehicleHoverDesc();
	InOutDesc.SeatKind = SeatKindOf(Spec.DriverSeat);
	InOutDesc.PassengerSeatKind = EMantleVehicleSeatKind::Car;
	InOutDesc.bSeatExitSideDistance = false;
	InOutDesc.SeatExitSideDistance = 0.0;
	InOutDesc.bRider = false;
	InOutDesc.Rider = FMantleVehicleRiderDesc();
	InOutDesc.bCameraBoom = false;
	InOutDesc.CameraBoomLocation = FVector::ZeroVector;

	const FMantleChassisHandling& Handling = Spec.Derived.IsSet() ? Spec.Derived.GetValue() : Spec.Archetype;
	DeriveRider(Spec, InOutDesc);
	if (Spec.Aircraft.bSet)
	{
		DeriveAircraft(Spec, Handling, InOutDesc);
	}
	if (Spec.Helicopter.bSet)
	{
		DeriveHelicopter(Spec, Handling, InOutDesc);
	}
	if (Spec.Hovercraft.bSet)
	{
		DeriveHovercraft(Spec, Handling, InOutDesc);
	}
	HoldToRanges(InOutDesc, OutHeld);
}

void FMantleVehicleChassisDocument::Write(const FMantleVehicleDesc& Desc, FJsonObject& Document)
{
	using namespace MantleVehicleChassisDocumentPrivate;

	if (Desc.MovementType != EMantleVehicleMovementKind::Wheeled)
	{
		Document.SetStringField(TEXT("movementType"), MovementName(Desc.MovementType));
	}
	switch (Desc.MovementType)
	{
	case EMantleVehicleMovementKind::Plane:
		Document.SetObjectField(TEXT("flight"), FlightJson(Desc.Flight));
		break;
	case EMantleVehicleMovementKind::Helicopter:
		Document.SetObjectField(TEXT("helicopter"), HelicopterJson(Desc.Helicopter));
		break;
	case EMantleVehicleMovementKind::Hover:
		Document.SetObjectField(TEXT("hover"), HoverJson(Desc.Hover));
		break;
	default:
		break;
	}
	WriteSeatAndCamera(Desc, Document);
}

void FMantleVehicleChassisDocument::Compose(const FMantleChassisSpec& Spec, FJsonObject& Document, TArray<FString>* OutHeld)
{
	FMantleVehicleDesc Movement;
	Derive(Spec, Movement, OutHeld);
	Write(Movement, Document);
}

TArray<FVector2D> FMantleVehicleChassisDocument::GroundSteeringSpeedCurve()
{
	return { FVector2D(0.0, 1.0), FVector2D(20.0, 0.8), FVector2D(60.0, 0.4), FVector2D(120.0, 0.3) };
}

void FMantleVehicleChassisDocument::ComposeGroundHandling(const FMantleChassisSpec& Spec, FJsonObject& Document, TArray<FString>* OutHeld)
{
	using namespace MantleVehicleChassisDocumentPrivate;

	if (Spec.Aircraft.bSet || Spec.Helicopter.bSet || Spec.Hovercraft.bSet || Spec.WheelCount() <= 2)
	{
		return;
	}
	TArray<TSharedPtr<FJsonValue>> Curve;
	for (const FVector2D& Point : GroundSteeringSpeedCurve())
	{
		const TArray<TSharedPtr<FJsonValue>> Pair = { Number(Point.X), Number(Point.Y) };
		Curve.Add(MakeShared<FJsonValueArray>(Pair));
	}
	ChildObject(Document, TEXT("steering"))->SetArrayField(TEXT("speedCurve"), Curve);

	// The preset's one friction is the middle, so a two-axle car's average grip is unchanged. Held to the document's ranges, as the movement numbers are.
	const TSharedPtr<FJsonObject> Tires = ChildObject(Document, TEXT("tires"));
	double Friction = 0.0;
	if (Tires->TryGetNumberField(TEXT("friction"), Friction))
	{
		auto Held = [OutHeld](const TCHAR* Path, const double Value)
		{
			double Min = 0.0;
			double Max = 0.0;
			if (!FMantleVehicleLimits::GetRange(Path, Min, Max))
			{
				return Value;
			}
			const double Clamped = FMath::Clamp(Value, Min, Max);
			if (Clamped != Value && OutHeld)
			{
				OutHeld->Add(FString::Printf(TEXT("%s %g held to %g"), Path, Value, Clamped));
			}
			return Clamped;
		};
		Tires->SetNumberField(TEXT("friction"), Held(TEXT("tires/friction"), Friction * FrontTireGripShare));
		Tires->SetNumberField(TEXT("rearFriction"), Held(TEXT("tires/rearFriction"), Friction * RearTireGripShare));
	}
}

TArray<FString> FMantleVehicleChassisDocument::HeldNumbers(const FMantleChassisSpec& Spec)
{
	using namespace MantleVehicleChassisDocumentPrivate;

	TArray<FString> Lines;
	FMantleChassisSpec::ComposeDocument(Spec, &Lines);
	// One sentence for each thing held, however many axes, surfaces or pads it has.
	TArray<FString> Plain;
	TSet<FString> Seen;
	for (const FString& Line : Lines)
	{
		FString Key;
		const FString Said = PlainHeld(Line, Key);
		if (!Said.IsEmpty() && !Seen.Contains(Key))
		{
			Seen.Add(Key);
			Plain.Add(Said);
		}
	}
	return Plain;
}

FString FMantleVehicleChassisDocument::MovementBlocksJson(const FMantleChassisSpec& Spec)
{
	using namespace MantleVehicleChassisDocumentPrivate;

	const TSharedRef<FJsonObject> Scratch = MakeShared<FJsonObject>();
	Compose(Spec, *Scratch);
	const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	for (const TCHAR* Block : { TEXT("flight"), TEXT("helicopter"), TEXT("hover") })
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		if (Scratch->TryGetObjectField(Block, Found) && Found && Found->IsValid())
		{
			Out->SetObjectField(Block, *Found);
		}
	}
	// The seat's and the camera's extras: not the kind, which a source holds as its own field.
	const TPair<const TCHAR*, const TCHAR*> Extras[] = { { TEXT("seat"), TEXT("exitSideDistance") }, { TEXT("seat"), TEXT("rider") }, { TEXT("camera"), TEXT("boomLocation") } };
	for (const TPair<const TCHAR*, const TCHAR*>& Extra : Extras)
	{
		const TSharedPtr<FJsonObject>* Parent = nullptr;
		if (Scratch->TryGetObjectField(Extra.Key, Parent) && Parent && Parent->IsValid() && (*Parent)->HasField(Extra.Value))
		{
			ChildObject(*Out, Extra.Key)->SetField(Extra.Value, (*Parent)->TryGetField(Extra.Value));
		}
	}
	return Condensed(Out);
}

bool FMantleVehicleChassisDocument::MergeSource(const UMantleVehicleSource& Source, FJsonObject& Document, FString* OutError)
{
	using namespace MantleVehicleChassisDocumentPrivate;

	// The type and the kinds first, so a source whose blocks do not read still fails the document's own rules instead of passing as a car.
	if (Source.MovementType != EMantleVehicleMovementKind::Wheeled)
	{
		Document.SetStringField(TEXT("movementType"), MovementName(Source.MovementType));
	}
	const TSharedPtr<FJsonObject>* Seat = nullptr;
	const bool bSeat = Document.TryGetObjectField(TEXT("seat"), Seat) && Seat && Seat->IsValid();
	if (bSeat && Source.DriverSeatKind != EMantleVehicleSeatKind::Car)
	{
		(*Seat)->SetStringField(TEXT("kind"), SeatKindName(Source.DriverSeatKind));
	}
	const TSharedPtr<FJsonObject>* Passenger = nullptr;
	if (Source.PassengerSeatKind != EMantleVehicleSeatKind::Car && Document.TryGetObjectField(TEXT("passengerSeat"), Passenger) && Passenger && Passenger->IsValid())
	{
		(*Passenger)->SetStringField(TEXT("kind"), SeatKindName(Source.PassengerSeatKind));
	}

	auto Refuse = [OutError](const FString& Why)
	{
		if (OutError)
		{
			*OutError = Why;
		}
		return false;
	};
	auto MergeWeaponArc = [&Refuse, &Document](const TCHAR* SeatKey, const TCHAR* SeatName, const bool bSeatPlaced,
		const FMantleVehicleSourceWeaponArc& Arc)
	{
		if (!Arc.bEnabled)
		{
			return true;
		}
		const FString Label = FString::Printf(TEXT("%s firearm aim arc"), SeatName);
		if (!bSeatPlaced)
		{
			return Refuse(FString::Printf(TEXT("the %s requires its seat to be placed"), *Label));
		}
		if (!FMath::IsFinite(Arc.YawMin) || !FMath::IsFinite(Arc.YawMax) || !FMath::IsFinite(Arc.PitchMin) || !FMath::IsFinite(Arc.PitchMax))
		{
			return Refuse(FString::Printf(TEXT("the %s must use finite angle bounds"), *Label));
		}
		if (Arc.YawMin >= Arc.YawMax || Arc.PitchMin >= Arc.PitchMax)
		{
			return Refuse(FString::Printf(TEXT("the %s minimums must be below their maximums"), *Label));
		}
		if (Arc.YawMin < -180.f || Arc.YawMax > 180.f || Arc.PitchMin < -90.f || Arc.PitchMax > 90.f)
		{
			return Refuse(FString::Printf(TEXT("the %s must stay within yaw -180 to 180 and pitch -90 to 90 degrees"), *Label));
		}
		const TSharedPtr<FJsonObject>* Seat = nullptr;
		if (!Document.TryGetObjectField(SeatKey, Seat) || !Seat || !Seat->IsValid())
		{
			return Refuse(FString::Printf(TEXT("the document has no %s seat for its firearm aim arc"), SeatName));
		}
		const TSharedRef<FJsonObject> WeaponArc = MakeShared<FJsonObject>();
		WeaponArc->SetField(TEXT("yawMin"), Number(Arc.YawMin));
		WeaponArc->SetField(TEXT("yawMax"), Number(Arc.YawMax));
		WeaponArc->SetField(TEXT("pitchMin"), Number(Arc.PitchMin));
		WeaponArc->SetField(TEXT("pitchMax"), Number(Arc.PitchMax));
		(*Seat)->SetObjectField(TEXT("weaponArc"), WeaponArc);
		return true;
	};
	if (!MergeWeaponArc(TEXT("seat"), TEXT("driver"), Source.Tuning.bSeatLocation, Source.Tuning.SeatWeaponArc)
		|| !MergeWeaponArc(TEXT("passengerSeat"), TEXT("passenger"), Source.Tuning.bPassengerSeat, Source.Tuning.PassengerSeatWeaponArc))
	{
		return false;
	}
	if (Source.MovementBlocks.IsEmpty())
	{
		return true;
	}
	TSharedPtr<FJsonObject> Blocks;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Source.MovementBlocks);
	if (!FJsonSerializer::Deserialize(Reader, Blocks) || !Blocks.IsValid())
	{
		return Refuse(TEXT("the vehicle's movement blocks are not a JSON object"));
	}
	for (const auto& Pair : Blocks->Values)
	{
		const FString Key(Pair.Key.ToView());
		const TSharedPtr<FJsonObject> Value = Pair.Value.IsValid() && Pair.Value->Type == EJson::Object ? Pair.Value->AsObject() : nullptr;
		if (!Value.IsValid())
		{
			return Refuse(FString::Printf(TEXT("the vehicle's movement blocks hold %s, which is not an object"), *Key));
		}
		if (Key.Equals(TEXT("flight"), ESearchCase::CaseSensitive) || Key.Equals(TEXT("helicopter"), ESearchCase::CaseSensitive)
			|| Key.Equals(TEXT("hover"), ESearchCase::CaseSensitive))
		{
			Document.SetObjectField(Key, Value);
			continue;
		}
		// The seat's and camera's extras join the keys the document already has.
		TArray<FString> Allowed;
		if (Key.Equals(TEXT("seat"), ESearchCase::CaseSensitive))
		{
			Allowed = { TEXT("exitSideDistance"), TEXT("rider") };
		}
		else if (Key.Equals(TEXT("camera"), ESearchCase::CaseSensitive))
		{
			Allowed = { TEXT("boomLocation") };
		}
		else
		{
			return Refuse(FString::Printf(TEXT("the vehicle's movement blocks hold %s; a source sets flight, helicopter, hover, seat or camera there (the type and the kinds are its own fields)"), *Key));
		}
		const TSharedPtr<FJsonObject>* Target = nullptr;
		if (!Document.TryGetObjectField(Key, Target) || !Target || !Target->IsValid())
		{
			return Refuse(FString::Printf(TEXT("the vehicle's movement blocks hold %s keys, but the document has no %s to put them on"), *Key, *Key));
		}
		for (const auto& Extra : Value->Values)
		{
			const FString ExtraKey(Extra.Key.ToView());
			if (!Allowed.ContainsByPredicate([&ExtraKey](const FString& Name) { return Name.Equals(ExtraKey, ESearchCase::CaseSensitive); }))
			{
				return Refuse(FString::Printf(TEXT("the vehicle's movement blocks hold %s/%s, which a source may not set"), *Key, *ExtraKey));
			}
			(*Target)->SetField(ExtraKey, Extra.Value);
		}
	}
	return true;
}
