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

	/** A flight or helicopter block's aerofoils (a plane's surfaces, a helicopter's fin, #380). */
	TArray<TSharedPtr<FJsonValue>> AerofoilsJson(const TArray<FMantleVehicleAerofoilDesc>& In)
	{
		TArray<TSharedPtr<FJsonValue>> Aerofoils;
		for (const FMantleVehicleAerofoilDesc& A : In)
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
			if (A.MaxLift > 0.0)
			{
				Entry->SetField(TEXT("maxLift"), Number(A.MaxLift));
			}
			if (A.AspectRatio > 0.0)
			{
				Entry->SetField(TEXT("aspectRatio"), Number(A.AspectRatio));
			}
			Aerofoils.Add(ObjectValue(Entry));
		}
		return Aerofoils;
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
		// #379's keys are written only when they say something: absent is the type's peak lift and aspect ratio, a jet's 4 s spool, assists on.
		if (F.SpoolTime >= 0.0)
		{
			Out->SetField(TEXT("spoolTime"), Number(F.SpoolTime));
		}
		if (!F.bStallProtection || !F.bCoordinatedTurns)
		{
			const TSharedRef<FJsonObject> Assists = MakeShared<FJsonObject>();
			Assists->SetBoolField(TEXT("stallProtection"), F.bStallProtection);
			Assists->SetBoolField(TEXT("coordinatedTurns"), F.bCoordinatedTurns);
			Out->SetObjectField(TEXT("assists"), Assists);
		}
		Out->SetArrayField(TEXT("aerofoils"), AerofoilsJson(F.Aerofoils));
		TArray<TSharedPtr<FJsonValue>> Thrusters;
		for (const FMantleVehicleThrusterDesc& T : F.Thrusters)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			SetBone(*Entry, T.Bone);
			Entry->SetField(TEXT("location"), Triple(T.Location));
			Entry->SetField(TEXT("direction"), Triple(T.Direction));
			Entry->SetField(TEXT("maxThrust"), Number(T.MaxThrustNewtons));
			if (T.PowerKw > 0.0)
			{
				Entry->SetField(TEXT("power"), Number(T.PowerKw));
			}
			if (T.AfterburnerNewtons > 0.0)
			{
				Entry->SetField(TEXT("afterburnerThrust"), Number(T.AfterburnerNewtons));
			}
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
		Out->SetBoolField(TEXT("heightHold"), H.bHeightHold);
		Out->SetBoolField(TEXT("yawHold"), H.bYawHold);
		if (H.Aerofoils.Num() > 0)
		{
			Out->SetArrayField(TEXT("aerofoils"), AerofoilsJson(H.Aerofoils));
		}
		TArray<TSharedPtr<FJsonValue>> Rotors;
		for (const FMantleVehicleRotorDesc& R : H.Rotors)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			SetBone(*Entry, R.Bone);
			Entry->SetField(TEXT("location"), Triple(R.Location));
			Entry->SetField(TEXT("axis"), Triple(R.Axis));
			Entry->SetField(TEXT("maxThrust"), Number(R.MaxThrustNewtons));
			if (R.Diameter > 0.0)
			{
				Entry->SetField(TEXT("diameter"), Number(R.Diameter));
			}
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

	/** A tracked vehicle's block, every number written; the rollers only when it has some (a chassis has none: its road wheels are all it draws). */
	TSharedRef<FJsonObject> TrackedJson(const FMantleVehicleTrackedDesc& T)
	{
		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetField(TEXT("pivotTrackSpeed"), Number(T.PivotTrackSpeed));
		Out->SetField(TEXT("lateralShare"), Number(T.LateralShare));
		Out->SetField(TEXT("trackWidth"), Number(T.TrackWidth));
		Out->SetField(TEXT("padPitch"), Number(T.PadPitch));
		if (T.Rollers.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> Rollers;
			for (const FMantleVehicleTrackRollerDesc& Roller : T.Rollers)
			{
				const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("bone"), Roller.Bone.ToString());
				Entry->SetField(TEXT("radius"), Number(Roller.Radius));
				Rollers.Add(ObjectValue(Entry));
			}
			Out->SetArrayField(TEXT("rollers"), Rollers);
		}
		return Out;
	}

	TSharedRef<FJsonObject> BoatJson(const FMantleVehicleBoatDesc& B)
	{
		const TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		const TSharedRef<FJsonObject> Hull = MakeShared<FJsonObject>();
		const FMantleVehicleBoatHullDesc& H = B.Hull;
		Hull->SetField(TEXT("location"), Triple(H.Location));
		Hull->SetField(TEXT("length"), Number(H.Length));
		Hull->SetField(TEXT("beam"), Number(H.Beam));
		Hull->SetField(TEXT("depth"), Number(H.Depth));
		Hull->SetField(TEXT("deadrise"), Number(H.DeadriseDegrees));
		Hull->SetNumberField(TEXT("points"), H.Points);
		Hull->SetField(TEXT("dragAlong"), Number(H.DragAlong));
		Hull->SetField(TEXT("dragAcross"), Number(H.DragAcross));
		Hull->SetField(TEXT("heaveDamping"), Number(H.HeaveDamping));
		Out->SetObjectField(TEXT("hull"), Hull);
		Out->SetField(TEXT("planingSpeed"), Number(B.PlaningSpeedCmPerSec));
		const TSharedRef<FJsonObject> Drive = MakeShared<FJsonObject>();
		const FMantleVehicleBoatPropulsionDesc& T = B.Propulsion;
		Drive->SetStringField(TEXT("kind"), T.Kind == 1 ? TEXT("jet") : TEXT("propeller"));
		Drive->SetField(TEXT("location"), Triple(T.Location));
		Drive->SetField(TEXT("direction"), Triple(T.Direction));
		Drive->SetField(TEXT("maxThrust"), Number(T.MaxThrustNewtons));
		Drive->SetField(TEXT("reverseShare"), Number(T.ReverseShare));
		Drive->SetField(TEXT("vectorAngle"), Number(T.VectorAngleDegrees));
		Drive->SetField(TEXT("topSpeed"), Number(T.TopSpeedCmPerSec));
		Drive->SetField(TEXT("brakeFade"), Number(T.BrakeFadeCmPerSec));
		if (T.Kind != 1)
		{
			Drive->SetField(TEXT("propWalk"), Number(T.PropWalk));
		}
		Out->SetObjectField(TEXT("propulsion"), Drive);
		if (B.bRudder)
		{
			const TSharedRef<FJsonObject> Fin = MakeShared<FJsonObject>();
			Fin->SetField(TEXT("location"), Triple(B.Rudder.Location));
			Fin->SetField(TEXT("area"), Number(B.Rudder.AreaM2));
			Fin->SetField(TEXT("maxControlAngle"), Number(B.Rudder.MaxControlAngle));
			Fin->SetField(TEXT("stallAngle"), Number(B.Rudder.StallAngle));
			Out->SetObjectField(TEXT("rudder"), Fin);
		}
		Out->SetField(TEXT("angularDamping"), Number(B.AngularDamping));
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
		Control->SetField(TEXT("autoCenterRollStrength"), Number(C.AutoCenterRollStrength));
		Control->SetField(TEXT("autoCenterPitchStrength"), Number(C.AutoCenterPitchStrength));
		Control->SetField(TEXT("autoCenterYawStrength"), Number(C.AutoCenterYawStrength));
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
		C.AutoCenterRollStrength = Control.AutoCenterRollStrength;
		C.AutoCenterPitchStrength = Control.AutoCenterPitchStrength;
		C.AutoCenterYawStrength = Control.AutoCenterYawStrength;
		D.bRider = true;
		D.bSeatExitSideDistance = true;
		// A quad (#376 step 9d) is as wide as a small car, not a bike's 40 cm: its rider steps off clear of its wheels and fenders.
		const double QuadHalfWidth = Spec.bCenterline ? 0.0 : 0.5 * FMath::Max(Spec.Width, Spec.Track + Spec.WheelWidth);
		D.SeatExitSideDistance = FMath::Max(FMantleVehicleChassisDocument::BikeExitSideDistance, QuadHalfWidth + FMantleVehicleChassisDocument::QuadExitClearance);
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
		// #379: a jet's spool and the player's assists, as the preset gives them.
		F.SpoolTime = Plane.SpoolTime;
		F.bStallProtection = Plane.bStallProtection;
		F.bCoordinatedTurns = Plane.bCoordinatedTurns;
		// Each surface's peak lift (the preset's, or the type's) and aspect ratio (its own span over chord: a stretched wing's grows).
		const auto Shape = [](const FMantleChassisSurface& Surface, FMantleVehicleAerofoilDesc& Out)
		{
			Out.MaxLift = Surface.MaxLift;
			Out.AspectRatio = Surface.AspectRatio();
		};

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
			Shape(Plane.Wing, Wing);
		}
		FMantleVehicleAerofoilDesc& Elevator = F.Aerofoils.AddDefaulted_GetRef();
		Elevator.Bone = FMantleVehicleChassisBuilder::ElevatorBoneName();
		Elevator.Location = Spec.ElevatorLocation();
		Elevator.Type = EMantleVehicleAerofoilKind::Elevator;
		Elevator.AreaM2 = Handling.TailplaneAreaM2;
		Elevator.Camber = Plane.Tailplane.Camber;
		Elevator.MaxControlAngle = Plane.Tailplane.MaxControlAngle;
		Elevator.StallAngle = Plane.Tailplane.StallAngle;
		Shape(Plane.Tailplane, Elevator);
		FMantleVehicleAerofoilDesc& Rudder = F.Aerofoils.AddDefaulted_GetRef();
		Rudder.Bone = FMantleVehicleChassisBuilder::RudderBoneName();
		Rudder.Location = Spec.RudderLocation();
		Rudder.Type = EMantleVehicleAerofoilKind::Rudder;
		Rudder.AreaM2 = Handling.FinAreaM2;
		Rudder.Camber = Plane.Fin.Camber;
		Rudder.MaxControlAngle = Plane.Fin.MaxControlAngle;
		Rudder.StallAngle = Plane.Fin.StallAngle;
		Shape(Plane.Fin, Rudder);

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
			// A resized plane's thrust follows its mass (its handling); its power and afterburner follow its thrust.
			const double ThrustRatio = Plane.EngineThrustNewtons > 0.0 ? Handling.EngineThrustNewtons / Plane.EngineThrustNewtons : 1.0;
			Push.PowerKw = Plane.bPropeller ? Plane.EnginePowerKw * ThrustRatio : 0.0;
			Push.AfterburnerNewtons = Plane.bPropeller ? 0.0 : Plane.AfterburnerNewtons * ThrustRatio;
			if (Plane.bPropeller)
			{
				F.SpinBones.Add(MakeSpin(EngineBone, FVector::ForwardVector, Engine == 1 ? -Plane.PropellerRpm : Plane.PropellerRpm));
			}
		}

		// The pilot's hands: the left on the throttle, the right on the stick, both on the root bone (neither lever is modeled to move).
		F.bPilotGrips = true;
		F.PilotGrips.Left = MakeGrip(Root, Plane.ThrottleGrip);
		F.PilotGrips.Right = MakeGrip(Root, Plane.StickGrip);

		// The chase camera orbits the cockpit's height over the plane's center of mass; a pilot steps out beside the fuselage.
		D.bCameraBoom = true;
		D.CameraBoomLocation = FVector(Spec.CenterOfMass.X, 0.0, Spec.Seat.Z + 7.0 * Spec.Scale);
		D.bSeatExitSideDistance = true;
		D.SeatExitSideDistance = 0.5 * Spec.Width + 145.0 * Spec.Scale;
	}

	/** A helicopter's rotors (DeriveHelicopter): the main rotor at its hub with its disc's size, the fin, the spinning rotors, the pilot's hands. */
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
		// No angular damping: the rotor's assists, or its own damping with them off, damp its turning (#380).
		H.AngularDamping = 0.0;
		// The fin on the boom's end (#380): a fixed vertical surface that weathervanes the nose into the airflow at speed and does nothing in a hover. A
		// rudder-type surface (its lift is sideways) whose control surface never moves; symmetric, so no camber.
		if (Heli.FinHeight > 0.0 && Heli.FinChord > 0.0)
		{
			const FMantleChassisBlock Fin = Spec.TailFinBlock();
			FMantleVehicleAerofoilDesc& FinFoil = H.Aerofoils.AddDefaulted_GetRef();
			FinFoil.Location = 0.5 * (Fin.Min + Fin.Max);
			FinFoil.Type = EMantleVehicleAerofoilKind::Rudder;
			FinFoil.AreaM2 = Heli.FinHeight * Heli.FinChord / 10000.0;
			FinFoil.Camber = 0.0;
			FinFoil.MaxControlAngle = 0.0;
			FinFoil.StallAngle = 20.0;
		}

		// The main rotor, at its hub over the center of mass; the rotor's other numbers are its defaults, which act through the body's inertia, so at any size.
		FMantleVehicleRotorDesc& Rotor = H.Rotors.AddDefaulted_GetRef();
		Rotor.Bone = FMantleVehicleChassisBuilder::MainRotorBoneName();
		Rotor.Location = Spec.MainRotorHub();
		Rotor.Axis = FVector::UpVector;
		Rotor.MaxThrustNewtons = Handling.RotorThrustNewtons;
		Rotor.Diameter = Heli.RotorDiameter;

		// The main rotor spins about Z and the tail rotor about Y, both while someone is aboard.
		H.SpinBones.Add(MakeSpin(FMantleVehicleChassisBuilder::MainRotorBoneName(), FVector::UpVector, Heli.RotorRpm));
		H.SpinBones.Add(MakeSpin(FMantleVehicleChassisBuilder::TailRotorBoneName(), FVector::RightVector, Heli.TailRotorRpm));

		// The pilot's hands: the left on the collective, the right on the cyclic, both on the root bone.
		H.bPilotGrips = true;
		H.PilotGrips.Left = MakeGrip(Root, Heli.CollectiveGrip);
		H.PilotGrips.Right = MakeGrip(Root, Heli.CyclicGrip);

		// The chase camera orbits the cockpit's height over the center of mass, as a plane's; the pilot steps out beside the cabin, past the skids.
		D.bCameraBoom = true;
		D.CameraBoomLocation = FVector(Spec.CenterOfMass.X, 0.0, Spec.Seat.Z + 7.0 * Spec.Scale);
		D.bSeatExitSideDistance = true;
		D.SeatExitSideDistance = FMath::Max(0.5 * Spec.Width, 0.5 * Spec.Track) + 145.0 * Spec.Scale;
	}

	/** A hovercraft's pads, fan, fin and damping (DeriveHovercraft): a pad where each of the layout's wheels is, on the hull's underside. */
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

		// What stands in for a tire's grip, as the builder's Grip slider sets it (#381), and the skirt brake.
		using Doc = FMantleVehicleChassisDocument;
		const double Grip = Handling.HoverGrip;
		H.Damping.LateralPerSecond = Doc::HovercraftSlide(Grip, Craft.LateralPerSecond, Doc::HovercraftGrippyLateralPerSecond);
		H.Damping.MaxLateralAccel = Doc::HovercraftSlide(Grip, Craft.MaxLateralAccel, Doc::HovercraftGrippyMaxLateralAccel);
		H.Damping.YawPerSecond = Doc::HovercraftSlide(Grip, Craft.YawPerSecond, Doc::HovercraftGrippyYawPerSecond);
		H.Damping.MaxYawAccelDegrees = Doc::HovercraftSlide(Grip, Craft.MaxYawAccelDegrees, Doc::HovercraftGrippyMaxYawAccelDegrees);
		H.Damping.SkirtBrakePerSecond = Craft.SkirtBrakePerSecond;
		H.Damping.MaxSkirtBrakeAccel = Craft.MaxSkirtBrakeAccel;
	}

	/**
	 * A tracked vehicle's tracks: the preset's (a resized chassis's scaled) width, pad length, pivot speed and lateral share. Its road wheels are the
	 * document's wheels, the layout's; it has no rollers, since the chassis draws only its road wheels.
	 */
	void DeriveTracked(const FMantleChassisSpec& Spec, FMantleVehicleDesc& D)
	{
		const FMantleChassisTracked& Tracks = Spec.Tracked;
		D.MovementType = EMantleVehicleMovementKind::Tracked;
		FMantleVehicleTrackedDesc& T = D.Tracked;
		T.PivotTrackSpeed = Tracks.PivotTrackSpeed;
		T.LateralShare = Tracks.LateralShare;
		T.TrackWidth = Tracks.TrackWidth;
		T.PadPitch = Tracks.PadPitch;
	}

	/** A boat's or jet ski's hull, drive and rudder (#15): the hull is the body box, its keel the box's bottom along its middle. */
	void DeriveBoat(const FMantleChassisSpec& Spec, const FMantleChassisHandling& Handling, FMantleVehicleDesc& D)
	{
		const FMantleChassisBoat& Craft = Spec.Boat;
		D.MovementType = EMantleVehicleMovementKind::Boat;
		FMantleVehicleBoatDesc& B = D.Boat;
		B.Hull.Location = Spec.BoatHullLocation();
		B.Hull.Length = Spec.Length;
		B.Hull.Beam = Spec.Width;
		B.Hull.Depth = Craft.HullDepth;
		B.Hull.DeadriseDegrees = Craft.DeadriseDegrees;
		B.Hull.Points = Craft.Points;
		B.Hull.DragAlong = Craft.DragAlong;
		B.Hull.DragAcross = Craft.DragAcross;
		B.Hull.HeaveDamping = Craft.HeaveDamping;
		B.PlaningSpeedCmPerSec = Craft.PlaningSpeedCmPerSec;
		B.AngularDamping = Craft.AngularDamping;

		FMantleVehicleBoatPropulsionDesc& Drive = B.Propulsion;
		Drive.Kind = Craft.bJet ? 1 : 0;
		Drive.Location = Spec.BoatDriveLocation();
		Drive.Direction = FVector::ForwardVector;
		Drive.MaxThrustNewtons = Handling.BoatThrustNewtons;
		Drive.ReverseShare = Craft.ReverseShare;
		Drive.VectorAngleDegrees = Craft.VectorAngle;
		Drive.TopSpeedCmPerSec = Craft.TopSpeedCmPerSec;
		Drive.PropWalk = Craft.bJet ? 0.0 : Craft.PropWalk;

		B.bRudder = Craft.bRudder;
		if (Craft.bRudder)
		{
			B.Rudder.Location = Spec.BoatRudderLocation();
			B.Rudder.AreaM2 = Handling.BoatRudderAreaM2;
			B.Rudder.MaxControlAngle = Craft.RudderMaxControlAngle;
		}
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
			Hold(TEXT("seat/rider/rotationControl/autoCenterRollStrength"), C.AutoCenterRollStrength);
			Hold(TEXT("seat/rider/rotationControl/autoCenterPitchStrength"), C.AutoCenterPitchStrength);
			Hold(TEXT("seat/rider/rotationControl/autoCenterYawStrength"), C.AutoCenterYawStrength);
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
				// 0 is "the type's" (not written), so only a given value is held.
				if (A.MaxLift > 0.0)
				{
					Hold(TEXT("flight/aerofoils/*/maxLift"), A.MaxLift);
				}
				if (A.AspectRatio > 0.0)
				{
					Hold(TEXT("flight/aerofoils/*/aspectRatio"), A.AspectRatio);
				}
			}
			if (F.SpoolTime >= 0.0)
			{
				Hold(TEXT("flight/spoolTime"), F.SpoolTime);
			}
			for (FMantleVehicleThrusterDesc& T : F.Thrusters)
			{
				HoldVector(TEXT("flight/thrusters/*/location/*"), T.Location);
				Hold(TEXT("flight/thrusters/*/maxThrust"), T.MaxThrustNewtons);
				if (T.PowerKw > 0.0)
				{
					Hold(TEXT("flight/thrusters/*/power"), T.PowerKw);
				}
				if (T.AfterburnerNewtons > 0.0)
				{
					Hold(TEXT("flight/thrusters/*/afterburnerThrust"), T.AfterburnerNewtons);
				}
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
			for (FMantleVehicleAerofoilDesc& A : H.Aerofoils)
			{
				HoldVector(TEXT("helicopter/aerofoils/*/location/*"), A.Location);
				Hold(TEXT("helicopter/aerofoils/*/area"), A.AreaM2);
				Hold(TEXT("helicopter/aerofoils/*/camber"), A.Camber);
				Hold(TEXT("helicopter/aerofoils/*/maxControlAngle"), A.MaxControlAngle);
				Hold(TEXT("helicopter/aerofoils/*/stallAngle"), A.StallAngle);
			}
			for (FMantleVehicleRotorDesc& R : H.Rotors)
			{
				HoldVector(TEXT("helicopter/rotors/*/location/*"), R.Location);
				Hold(TEXT("helicopter/rotors/*/maxThrust"), R.MaxThrustNewtons);
				if (R.Diameter > 0.0)
				{
					Hold(TEXT("helicopter/rotors/*/diameter"), R.Diameter);
				}
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
		if (D.MovementType == EMantleVehicleMovementKind::Tracked)
		{
			FMantleVehicleTrackedDesc& T = D.Tracked;
			Hold(TEXT("tracked/pivotTrackSpeed"), T.PivotTrackSpeed);
			Hold(TEXT("tracked/lateralShare"), T.LateralShare);
			Hold(TEXT("tracked/trackWidth"), T.TrackWidth);
			Hold(TEXT("tracked/padPitch"), T.PadPitch);
			for (FMantleVehicleTrackRollerDesc& Roller : T.Rollers)
			{
				Hold(TEXT("tracked/rollers/*/radius"), Roller.Radius);
			}
		}
		if (D.MovementType == EMantleVehicleMovementKind::Boat)
		{
			FMantleVehicleBoatDesc& B = D.Boat;
			HoldVector(TEXT("boat/hull/location/*"), B.Hull.Location);
			Hold(TEXT("boat/hull/length"), B.Hull.Length);
			Hold(TEXT("boat/hull/beam"), B.Hull.Beam);
			Hold(TEXT("boat/hull/depth"), B.Hull.Depth);
			Hold(TEXT("boat/hull/deadrise"), B.Hull.DeadriseDegrees);
			Hold(TEXT("boat/hull/dragAlong"), B.Hull.DragAlong);
			Hold(TEXT("boat/hull/dragAcross"), B.Hull.DragAcross);
			Hold(TEXT("boat/hull/heaveDamping"), B.Hull.HeaveDamping);
			Hold(TEXT("boat/planingSpeed"), B.PlaningSpeedCmPerSec);
			HoldVector(TEXT("boat/propulsion/location/*"), B.Propulsion.Location);
			Hold(TEXT("boat/propulsion/maxThrust"), B.Propulsion.MaxThrustNewtons);
			Hold(TEXT("boat/propulsion/reverseShare"), B.Propulsion.ReverseShare);
			Hold(TEXT("boat/propulsion/vectorAngle"), B.Propulsion.VectorAngleDegrees);
			Hold(TEXT("boat/propulsion/topSpeed"), B.Propulsion.TopSpeedCmPerSec);
			Hold(TEXT("boat/propulsion/brakeFade"), B.Propulsion.BrakeFadeCmPerSec);
			Hold(TEXT("boat/propulsion/propWalk"), B.Propulsion.PropWalk);
			if (B.bRudder)
			{
				HoldVector(TEXT("boat/rudder/location/*"), B.Rudder.Location);
				Hold(TEXT("boat/rudder/area"), B.Rudder.AreaM2);
				Hold(TEXT("boat/rudder/maxControlAngle"), B.Rudder.MaxControlAngle);
				Hold(TEXT("boat/rudder/stallAngle"), B.Rudder.StallAngle);
			}
			Hold(TEXT("boat/angularDamping"), B.AngularDamping);
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
		{ TEXT("tires/friction"), TEXT("The front tires' grip") },
		{ TEXT("tires/rearFriction"), TEXT("The rear tires' grip") },
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
		{ TEXT("tracked/pivotTrackSpeed"), TEXT("How fast the tracks run in a turn on the spot") },
		{ TEXT("tracked/trackWidth"), TEXT("The tracks' width") },
		{ TEXT("tracked/padPitch"), TEXT("A track pad's length") },
		{ TEXT("tracked"), TEXT("A tracked-vehicle setting") },
		{ TEXT("boat/hull"), TEXT("The boat's hull") },
		{ TEXT("boat/propulsion"), TEXT("The boat's drive") },
		{ TEXT("boat/rudder"), TEXT("The boat's rudder") },
		{ TEXT("boat"), TEXT("A boat setting") },
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
	case EMantleVehicleMovementKind::Tracked:
		return TEXT("tracked");
	case EMantleVehicleMovementKind::Boat:
		return TEXT("boat");
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
	InOutDesc.Tracked = FMantleVehicleTrackedDesc();
	InOutDesc.Boat = FMantleVehicleBoatDesc();
	InOutDesc.SeatKind = SeatKindOf(Spec.DriverSeat);
	InOutDesc.PassengerSeatKind = SeatKindOf(Spec.PassengerSeat);
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
	if (Spec.Tracked.bSet)
	{
		DeriveTracked(Spec, InOutDesc);
	}
	if (Spec.Boat.bSet)
	{
		DeriveBoat(Spec, Handling, InOutDesc);
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
	case EMantleVehicleMovementKind::Tracked:
		Document.SetObjectField(TEXT("tracked"), TrackedJson(Desc.Tracked));
		break;
	case EMantleVehicleMovementKind::Boat:
		Document.SetObjectField(TEXT("boat"), BoatJson(Desc.Boat));
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
	// #376 step 5, from the drive probe's steering sweeps: at each speed, 1.3x the road-wheel angle that gives a car its peak lateral g, as a share of
	// its lock, taking the most any ground preset needs (the limousine and six-wheeler: long, with less lock). DA_SportsCar's line is kept to 25 mph,
	// so parking and town turns are unchanged (at 20 km/h every preset still wants more than its full lock).
	return { FVector2D(0.0, 1.0), FVector2D(20.0, 0.8), FVector2D(25.0, 0.75), FVector2D(37.5, 0.53), FVector2D(56.0, 0.35), FVector2D(75.0, 0.27),
		FVector2D(120.0, 0.24) };
}

TArray<FVector2D> FMantleVehicleChassisDocument::TwoWheelerSteeringSpeedCurve()
{
	// #378, from the drive probe's steering sweeps on the default 600 at grip 2.0 (probe round 5): at each speed the road-wheel angle that gives
	// its peak sideways g, as a share of its 40 deg lock (this tyre model has no camber thrust, so the front tyre's slip angle turns the bike, as on
	// a car). Not the cars' 1.3x: past the peak the front tyre lets go, the yaw rate drops, the lean target follows it and the bike weaves and hops
	// (round 5's full-lock turn at 52 km/h, 1.27x its peak: roll 11-38 deg, yaw rate 2-46 deg/s). Peaks: 40 km/h 10.9 deg, 60 km/h past the
	// 8 deg the old curve gave (0.93 g at 7.6), 90 km/h past 4 deg (0.88 g there), 120 km/h 2.2 deg. Walking pace and 20 km/h keep the old lock
	// for parking and town turns, where the lean is small. From 75 mph it holds at the document's floor, 0.1 (steering/speedCurve/*/1 is 0.1-1).
	return { FVector2D(0.0, 1.0), FVector2D(5.0, 1.0), FVector2D(12.4, 0.68), FVector2D(24.9, 0.3), FVector2D(37.3, 0.22), FVector2D(55.9, 0.14),
		FVector2D(74.6, 0.1) };
}

void FMantleVehicleChassisDocument::ComposeGroundHandling(const FMantleChassisSpec& Spec, FJsonObject& Document, TArray<FString>* OutHeld)
{
	using namespace MantleVehicleChassisDocumentPrivate;

	// A tracked vehicle steers no wheel and grips alike on every road wheel (its tracks turn it), so neither the curve nor a front and rear grip is its.
	if (Spec.Aircraft.bSet || Spec.Helicopter.bSet || Spec.Hovercraft.bSet || Spec.Tracked.bSet || Spec.Boat.bSet)
	{
		return;
	}
	// A rider on two wheels (#378) gets its own steering curve and keeps its tyres' own grip (a bike's front and rear are the preset's, not a car's split).
	const bool bTwoWheeler = Spec.WheelCount() == 2 && Spec.Rider.bSet;
	if (Spec.WheelCount() <= 2 && !bTwoWheeler)
	{
		return;
	}
	TArray<TSharedPtr<FJsonValue>> Curve;
	for (const FVector2D& Point : bTwoWheeler ? TwoWheelerSteeringSpeedCurve() : GroundSteeringSpeedCurve())
	{
		const TArray<TSharedPtr<FJsonValue>> Pair = { Number(Point.X), Number(Point.Y) };
		Curve.Add(MakeShared<FJsonValueArray>(Pair));
	}
	ChildObject(Document, TEXT("steering"))->SetArrayField(TEXT("speedCurve"), Curve);
	if (bTwoWheeler)
	{
		return;
	}

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
		// A preset that sets its own rear grip (the drift car's, under its front) keeps it, scaled with the friction so a resized or re-gripped car
		// keeps the preset's balance; every other preset's rear is RearTireGripShare of the friction.
		double Rear = Friction * RearTireGripShare;
		const TSharedPtr<FJsonObject>* PresetTires = nullptr;
		double PresetRear = 0.0;
		if (Spec.Defaults.IsValid() && Spec.Defaults->TryGetObjectField(TEXT("tires"), PresetTires) && PresetTires && (*PresetTires)->TryGetNumberField(TEXT("rearFriction"), PresetRear)
			&& Spec.Archetype.TireFriction > 0.0)
		{
			Rear = PresetRear * Friction / Spec.Archetype.TireFriction;
		}
		Tires->SetNumberField(TEXT("friction"), Held(TEXT("tires/friction"), Friction * FrontTireGripShare));
		Tires->SetNumberField(TEXT("rearFriction"), Held(TEXT("tires/rearFriction"), Rear));
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
	for (const TCHAR* Block : { TEXT("flight"), TEXT("helicopter"), TEXT("hover"), TEXT("tracked"), TEXT("boat") })
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
		if (Key.Equals(TEXT("helicopter"), ESearchCase::CaseSensitive))
		{
			// The creator's assists (#380), part of the helicopter like a car's ABS: the level assist on every rotor, height hold and the yaw assist on the block.
			Value->SetBoolField(TEXT("heightHold"), Source.Tuning.bHelicopterHeightHold);
			Value->SetBoolField(TEXT("yawHold"), Source.Tuning.bHelicopterYawHold);
			const TArray<TSharedPtr<FJsonValue>>* Rotors = nullptr;
			if (Value->TryGetArrayField(TEXT("rotors"), Rotors) && Rotors)
			{
				for (const TSharedPtr<FJsonValue>& Rotor : *Rotors)
				{
					if (Rotor.IsValid() && Rotor->Type == EJson::Object)
					{
						Rotor->AsObject()->SetBoolField(TEXT("stabilityAssist"), Source.Tuning.bHelicopterLevelAssist);
					}
				}
			}
		}
		if (Key.Equals(TEXT("flight"), ESearchCase::CaseSensitive) || Key.Equals(TEXT("helicopter"), ESearchCase::CaseSensitive)
			|| Key.Equals(TEXT("hover"), ESearchCase::CaseSensitive) || Key.Equals(TEXT("tracked"), ESearchCase::CaseSensitive)
			|| Key.Equals(TEXT("boat"), ESearchCase::CaseSensitive))
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
			return Refuse(FString::Printf(TEXT("the vehicle's movement blocks hold %s; a source sets flight, helicopter, hover, tracked, boat, seat or camera there (the type and the kinds are its own fields)"), *Key));
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

double FMantleVehicleChassisDocument::HovercraftSlide(const double Grip, const double Own, const double Grippy)
{
	const double G = FMath::IsFinite(Grip) ? FMath::Clamp(Grip, -1.0, 1.0) : 0.0;
	return G < 0.0 ? Own * (1.0 + G) : Own + (Grippy - Own) * G;
}
