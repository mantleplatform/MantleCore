// Copyright Joshua Eger. All Rights Reserved.
#include "MantleChassisSpec.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "MantleVehicleChassisDocument.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleChassisSpecPrivate
{
	/** The schema's defaults (schema/vehicle.schema.json): what a document without the number has, which the cook fills and the reader requires. */
	constexpr double DefaultMass = 1500.0;
	constexpr double DefaultMaxSpeed = 4000.0;
	constexpr double DefaultDragCoefficient = 0.3;
	constexpr double DefaultFrontalArea = 2.2;
	constexpr double DefaultIdleRpm = 900.0;
	constexpr double DefaultReverseRatio = 3.0;
	constexpr double DefaultMaxRaise = 10.0;
	constexpr double DefaultMaxDrop = 10.0;
	constexpr double DefaultSpringRate = 250.0;
	constexpr double DefaultSpringPreload = 50.0;
	constexpr double DefaultSpringDamping = 0.5;
	constexpr double DefaultMaxSteerAngle = 35.0;
	constexpr double DefaultBrakeTorque = 1500.0;
	constexpr double DefaultHandbrakeTorque = 3000.0;
	constexpr double DefaultTireFriction = 2.0;
	constexpr double DefaultCameraArmLength = 650.0;
	constexpr double DefaultCameraHeight = 150.0;

	/** The bone names of a two-axle chassis, in wheel order: front-left, front-right, rear-left, rear-right. */
	const TCHAR* const TwoAxleBoneNames[4] = { TEXT("wheel_fl"), TEXT("wheel_fr"), TEXT("wheel_rl"), TEXT("wheel_rr") };

	/**
	 * The bone of the wheel of axle Axle (counted from the front) on Side (0 the left, -Y, 1 the right). Two axles keep the names
	 * wheel_fl, wheel_fr, wheel_rl and wheel_rr; three or four are wheel_a1_l, wheel_a1_r, wheel_a2_l and so on, front axle first.
	 */
	FName WheelBoneName(const int32 AxleCount, const int32 Axle, const int32 Side)
	{
		if (AxleCount == 2)
		{
			return FName(TwoAxleBoneNames[Axle * 2 + Side]);
		}
		return FName(*FString::Printf(TEXT("wheel_a%d_%s"), Axle + 1, Side == 0 ? TEXT("l") : TEXT("r")));
	}

	/** The bone of the one wheel on axle Axle of a centreline chassis: wheel_f and wheel_r for two axles, wheel_a1, wheel_a2 and so on for three or four. */
	FName CentrelineWheelBoneName(const int32 AxleCount, const int32 Axle)
	{
		if (AxleCount == 2)
		{
			return FName(Axle == 0 ? TEXT("wheel_f") : TEXT("wheel_r"));
		}
		return FName(*FString::Printf(TEXT("wheel_a%d"), Axle + 1));
	}

	/** The fork bone a steered centreline wheel hangs under: fork_f and fork_r for two axles, fork_a1, fork_a2 and so on. */
	FName CentrelineForkBoneName(const int32 AxleCount, const int32 Axle)
	{
		if (AxleCount == 2)
		{
			return FName(Axle == 0 ? TEXT("fork_f") : TEXT("fork_r"));
		}
		return FName(*FString::Printf(TEXT("fork_a%d"), Axle + 1));
	}

	bool Refuse(FString& OutError, const FString& Why)
	{
		OutError = Why;
		return false;
	}

	bool GetObject(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Key, const FString& Label, TSharedPtr<FJsonObject>& Out, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		if (Parent.IsValid() && Parent->TryGetObjectField(Key, Found) && Found && Found->IsValid())
		{
			Out = *Found;
			return true;
		}
		return Refuse(OutError, FString::Printf(TEXT("the preset's %s is missing or not an object"), *Label));
	}

	bool GetNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, const FString& Label, const bool bPositive, double& Out, FString& OutError)
	{
		double Value = 0.0;
		if (!Object->TryGetNumberField(Key, Value) || !FMath::IsFinite(Value) || (bPositive && Value <= 0.0))
		{
			return Refuse(OutError, FString::Printf(TEXT("the preset's %s is missing, not a number%s"), *Label, bPositive ? TEXT(" or not above 0") : TEXT("")));
		}
		Out = Value;
		return true;
	}

	bool GetVector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, const FString& Label, FVector& Out, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		double Parts[3] = { 0.0, 0.0, 0.0 };
		bool bOk = Object->TryGetArrayField(Key, Array) && Array && Array->Num() == 3;
		for (int32 Index = 0; bOk && Index < 3; ++Index)
		{
			bOk = (*Array)[Index].IsValid() && (*Array)[Index]->TryGetNumber(Parts[Index]) && FMath::IsFinite(Parts[Index]);
		}
		if (!bOk)
		{
			return Refuse(OutError, FString::Printf(TEXT("the preset's %s is missing or not three numbers"), *Label));
		}
		Out = FVector(Parts[0], Parts[1], Parts[2]);
		return true;
	}

	/** The rider block of a motorcycle preset's chassis: the grips, the pegs, how the rider is fitted and the rotation control. */
	bool ParseRider(const TSharedPtr<FJsonObject>& Chassis, FMantleVehicleChassisRider& Out, FString& OutError)
	{
		TSharedPtr<FJsonObject> Rider;
		TSharedPtr<FJsonObject> Bar;
		TSharedPtr<FJsonObject> Pegs;
		FMantleVehicleChassisRider Read;
		if (!GetObject(Chassis, TEXT("rider"), TEXT("chassis.rider"), Rider, OutError)
			|| !GetObject(Rider, TEXT("handlebar"), TEXT("chassis.rider.handlebar"), Bar, OutError)
			|| !GetObject(Rider, TEXT("footPegs"), TEXT("chassis.rider.footPegs"), Pegs, OutError)
			|| !GetVector(Bar, TEXT("left"), TEXT("chassis.rider.handlebar.left"), Read.LeftGrip, OutError)
			|| !GetVector(Bar, TEXT("right"), TEXT("chassis.rider.handlebar.right"), Read.RightGrip, OutError)
			|| !GetVector(Pegs, TEXT("left"), TEXT("chassis.rider.footPegs.left"), Read.LeftPeg, OutError)
			|| !GetVector(Pegs, TEXT("right"), TEXT("chassis.rider.footPegs.right"), Read.RightPeg, OutError)
			|| !GetVector(Rider, TEXT("pelvisOffset"), TEXT("chassis.rider.pelvisOffset"), Read.PelvisOffset, OutError)
			|| !GetVector(Rider, TEXT("stopPelvisShift"), TEXT("chassis.rider.stopPelvisShift"), Read.StopPelvisShift, OutError))
		{
			return false;
		}
		Rider->TryGetNumberField(TEXT("kneeSplayDegrees"), Read.KneeSplayDegrees);
		Rider->TryGetNumberField(TEXT("rearTireFriction"), Read.RearTireFriction);
		Rider->TryGetNumberField(TEXT("rearBrakeTorque"), Read.RearBrakeTorque);
		const TSharedPtr<FJsonObject>* Rotation = nullptr;
		if (Rider->TryGetObjectField(TEXT("rotationControl"), Rotation) && Rotation && Rotation->IsValid())
		{
			const TSharedPtr<FJsonObject>& Json = *Rotation;
			FMantleVehicleChassisRotationControl& Control = Read.RotationControl;
			Json->TryGetBoolField(TEXT("torqueControl"), Control.bTorqueControl);
			Json->TryGetBoolField(TEXT("targetRotation"), Control.bTargetRotation);
			Json->TryGetBoolField(TEXT("rollVsSpeed"), Control.bRollVsSpeed);
			Json->TryGetNumberField(TEXT("torqueRotationDamping"), Control.TorqueRotationDamping);
			Json->TryGetNumberField(TEXT("rollControlScaling"), Control.RollControlScaling);
			Json->TryGetNumberField(TEXT("rollMaxAngle"), Control.RollMaxAngle);
			Json->TryGetNumberField(TEXT("pitchControlScaling"), Control.PitchControlScaling);
			Json->TryGetNumberField(TEXT("pitchMaxAngle"), Control.PitchMaxAngle);
			Json->TryGetNumberField(TEXT("rotationStiffness"), Control.RotationStiffness);
			Json->TryGetNumberField(TEXT("rotationDamping"), Control.RotationDamping);
			Json->TryGetNumberField(TEXT("maxAccel"), Control.MaxAccel);
			Json->TryGetNumberField(TEXT("autoCentreRollStrength"), Control.AutoCentreRollStrength);
			Json->TryGetNumberField(TEXT("autoCentrePitchStrength"), Control.AutoCentrePitchStrength);
			Json->TryGetNumberField(TEXT("autoCentreYawStrength"), Control.AutoCentreYawStrength);
		}
		Read.bSet = true;
		Out = Read;
		return true;
	}

	/** One lifting surface of a plane preset's chassis.aircraft: its span (a fin's height), chord, x, z (not a fin's), area, the largest control angle, and the optional camber and stall angle. */
	bool ParseSurface(const TSharedPtr<FJsonObject>& Aircraft, const TCHAR* Key, const bool bFin, FMantleChassisSurface& Out, FString& OutError)
	{
		const FString Label = FString::Printf(TEXT("chassis.aircraft.%s"), Key);
		TSharedPtr<FJsonObject> Json;
		FMantleChassisSurface Read;
		if (!GetObject(Aircraft, Key, Label, Json, OutError)
			|| !GetNumber(Json, bFin ? TEXT("height") : TEXT("span"), Label + (bFin ? TEXT(".height") : TEXT(".span")), true, Read.Span, OutError)
			|| !GetNumber(Json, TEXT("chord"), Label + TEXT(".chord"), true, Read.Chord, OutError)
			|| !GetNumber(Json, TEXT("x"), Label + TEXT(".x"), false, Read.X, OutError)
			|| (!bFin && !GetNumber(Json, TEXT("z"), Label + TEXT(".z"), false, Read.Z, OutError))
			|| !GetNumber(Json, TEXT("area"), Label + TEXT(".area"), true, Read.AreaM2, OutError)
			|| !GetNumber(Json, TEXT("maxControlAngle"), Label + TEXT(".maxControlAngle"), true, Read.MaxControlAngle, OutError))
		{
			return false;
		}
		Json->TryGetNumberField(TEXT("camber"), Read.Camber);
		Json->TryGetNumberField(TEXT("stallAngle"), Read.StallAngle);
		if (!FMath::IsFinite(Read.Camber) || !FMath::IsFinite(Read.StallAngle) || Read.StallAngle < 1.0)
		{
			return Refuse(OutError, FString::Printf(TEXT("the preset's %s camber is not a number, or its stallAngle is below 1"), *Label));
		}
		Out = Read;
		return true;
	}

	/**
	 * The aircraft block of a plane preset's chassis (chassis.aircraft): the wing, tailplane and fin, the engines and their propeller, the air-speed limit
	 * and where the pilot's hands hold. The propeller block is optional: without one the engines are jets.
	 */
	bool ParseAircraft(const TSharedPtr<FJsonObject>& Chassis, FMantleChassisAircraft& Out, FString& OutError)
	{
		TSharedPtr<FJsonObject> Json;
		TSharedPtr<FJsonObject> Engines;
		TSharedPtr<FJsonObject> Propeller;
		TSharedPtr<FJsonObject> Controls;
		FMantleChassisAircraft Read;
		double Count = 0.0;
		if (!GetObject(Chassis, TEXT("aircraft"), TEXT("chassis.aircraft"), Json, OutError)
			|| !ParseSurface(Json, TEXT("wing"), false, Read.Wing, OutError)
			|| !ParseSurface(Json, TEXT("tailplane"), false, Read.Tailplane, OutError)
			|| !ParseSurface(Json, TEXT("fin"), true, Read.Fin, OutError)
			|| !GetObject(Json, TEXT("engines"), TEXT("chassis.aircraft.engines"), Engines, OutError)
			|| !GetNumber(Engines, TEXT("count"), TEXT("chassis.aircraft.engines.count"), true, Count, OutError)
			|| !GetNumber(Engines, TEXT("thrust"), TEXT("chassis.aircraft.engines.thrust"), true, Read.EngineThrustNewtons, OutError)
			|| !GetNumber(Engines, TEXT("x"), TEXT("chassis.aircraft.engines.x"), false, Read.EngineX, OutError))
		{
			return false;
		}
		// A propeller block, when there is one, is read whole; a key that is there but is not an object is a mistake, not a jet.
		Read.bPropeller = Json->HasField(TEXT("propeller"));
		if (Read.bPropeller
			&& (!GetObject(Json, TEXT("propeller"), TEXT("chassis.aircraft.propeller"), Propeller, OutError)
				|| !GetNumber(Propeller, TEXT("radius"), TEXT("chassis.aircraft.propeller.radius"), true, Read.PropellerRadius, OutError)
				|| !GetNumber(Propeller, TEXT("rpm"), TEXT("chassis.aircraft.propeller.rpm"), true, Read.PropellerRpm, OutError)))
		{
			return false;
		}
		if (!GetNumber(Json, TEXT("airSpeedLimit"), TEXT("chassis.aircraft.airSpeedLimit"), true, Read.AirSpeedLimitCmPerSec, OutError)
			|| !GetNumber(Json, TEXT("cruiseThrottle"), TEXT("chassis.aircraft.cruiseThrottle"), true, Read.CruiseThrottle, OutError)
			|| !GetObject(Json, TEXT("controls"), TEXT("chassis.aircraft.controls"), Controls, OutError)
			|| !GetVector(Controls, TEXT("throttle"), TEXT("chassis.aircraft.controls.throttle"), Read.ThrottleGrip, OutError)
			|| !GetVector(Controls, TEXT("stick"), TEXT("chassis.aircraft.controls.stick"), Read.StickGrip, OutError))
		{
			return false;
		}
		if (Count != 1.0 && Count != 2.0)
		{
			return Refuse(OutError, TEXT("the preset's chassis.aircraft.engines.count is not 1 or 2"));
		}
		if (Read.CruiseThrottle > 1.0)
		{
			return Refuse(OutError, TEXT("the preset's chassis.aircraft.cruiseThrottle is above 1"));
		}
		// The cruise speed is optional here: a spec without one is never re-trimmed (the schema requires it of a preset).
		Json->TryGetNumberField(TEXT("cruiseSpeed"), Read.CruiseSpeedCmPerSec);
		if (!FMath::IsFinite(Read.CruiseSpeedCmPerSec) || Read.CruiseSpeedCmPerSec < 0.0)
		{
			return Refuse(OutError, TEXT("the preset's chassis.aircraft.cruiseSpeed is not a number above 0"));
		}
		Read.EngineCount = static_cast<int32>(Count);
		Read.bSet = true;
		Out = Read;
		return true;
	}

	/**
	 * The helicopter block of a helicopter preset's chassis (chassis.helicopter): the main rotor, the tail boom, the tail rotor, the fin, the collective that
	 * holds the weight, the air-speed limit and where the pilot's hands hold. Every part is required.
	 */
	bool ParseHelicopter(const TSharedPtr<FJsonObject>& Chassis, FMantleChassisHelicopter& Out, FString& OutError)
	{
		TSharedPtr<FJsonObject> Json;
		TSharedPtr<FJsonObject> Rotor;
		TSharedPtr<FJsonObject> Boom;
		TSharedPtr<FJsonObject> TailRotor;
		TSharedPtr<FJsonObject> Fin;
		TSharedPtr<FJsonObject> Controls;
		FMantleChassisHelicopter Read;
		if (!GetObject(Chassis, TEXT("helicopter"), TEXT("chassis.helicopter"), Json, OutError)
			|| !GetObject(Json, TEXT("rotor"), TEXT("chassis.helicopter.rotor"), Rotor, OutError)
			|| !GetNumber(Rotor, TEXT("diameter"), TEXT("chassis.helicopter.rotor.diameter"), true, Read.RotorDiameter, OutError)
			|| !GetNumber(Rotor, TEXT("z"), TEXT("chassis.helicopter.rotor.z"), true, Read.RotorZ, OutError)
			|| !GetNumber(Rotor, TEXT("thrust"), TEXT("chassis.helicopter.rotor.thrust"), true, Read.RotorThrustNewtons, OutError)
			|| !GetNumber(Rotor, TEXT("rpm"), TEXT("chassis.helicopter.rotor.rpm"), true, Read.RotorRpm, OutError)
			|| !GetObject(Json, TEXT("tailBoom"), TEXT("chassis.helicopter.tailBoom"), Boom, OutError)
			|| !GetNumber(Boom, TEXT("length"), TEXT("chassis.helicopter.tailBoom.length"), true, Read.BoomLength, OutError)
			|| !GetNumber(Boom, TEXT("width"), TEXT("chassis.helicopter.tailBoom.width"), true, Read.BoomWidth, OutError)
			|| !GetNumber(Boom, TEXT("height"), TEXT("chassis.helicopter.tailBoom.height"), true, Read.BoomHeight, OutError)
			|| !GetNumber(Boom, TEXT("z"), TEXT("chassis.helicopter.tailBoom.z"), true, Read.BoomZ, OutError)
			|| !GetObject(Json, TEXT("tailRotor"), TEXT("chassis.helicopter.tailRotor"), TailRotor, OutError)
			|| !GetNumber(TailRotor, TEXT("diameter"), TEXT("chassis.helicopter.tailRotor.diameter"), true, Read.TailRotorDiameter, OutError)
			|| !GetNumber(TailRotor, TEXT("rpm"), TEXT("chassis.helicopter.tailRotor.rpm"), true, Read.TailRotorRpm, OutError)
			|| !GetObject(Json, TEXT("fin"), TEXT("chassis.helicopter.fin"), Fin, OutError)
			|| !GetNumber(Fin, TEXT("height"), TEXT("chassis.helicopter.fin.height"), true, Read.FinHeight, OutError)
			|| !GetNumber(Fin, TEXT("chord"), TEXT("chassis.helicopter.fin.chord"), true, Read.FinChord, OutError)
			|| !GetNumber(Json, TEXT("hoverCollective"), TEXT("chassis.helicopter.hoverCollective"), true, Read.HoverCollective, OutError)
			|| !GetNumber(Json, TEXT("airSpeedLimit"), TEXT("chassis.helicopter.airSpeedLimit"), true, Read.AirSpeedLimitCmPerSec, OutError)
			|| !GetObject(Json, TEXT("controls"), TEXT("chassis.helicopter.controls"), Controls, OutError)
			|| !GetVector(Controls, TEXT("collective"), TEXT("chassis.helicopter.controls.collective"), Read.CollectiveGrip, OutError)
			|| !GetVector(Controls, TEXT("cyclic"), TEXT("chassis.helicopter.controls.cyclic"), Read.CyclicGrip, OutError))
		{
			return false;
		}
		if (Read.HoverCollective > 1.0)
		{
			return Refuse(OutError, TEXT("the preset's chassis.helicopter.hoverCollective is above 1: its rotor cannot hold its weight"));
		}
		Read.bSet = true;
		Out = Read;
		return true;
	}

	/**
	 * The hovercraft block of a hovercraft preset's chassis (chassis.hovercraft): the pads, the fan and its pod, the fin and its rudder, the drawn skirt and
	 * the damping that stands in for a tyre's grip. Every part is required; the fin's stall angle has the schema's default.
	 */
	bool ParseHovercraft(const TSharedPtr<FJsonObject>& Chassis, FMantleChassisHovercraft& Out, FString& OutError)
	{
		TSharedPtr<FJsonObject> Json;
		TSharedPtr<FJsonObject> Pads;
		TSharedPtr<FJsonObject> Fan;
		TSharedPtr<FJsonObject> Fin;
		TSharedPtr<FJsonObject> Skirt;
		TSharedPtr<FJsonObject> Damping;
		FMantleChassisHovercraft Read;
		if (!GetObject(Chassis, TEXT("hovercraft"), TEXT("chassis.hovercraft"), Json, OutError)
			|| !GetObject(Json, TEXT("pads"), TEXT("chassis.hovercraft.pads"), Pads, OutError)
			|| !GetNumber(Pads, TEXT("travel"), TEXT("chassis.hovercraft.pads.travel"), true, Read.PadTravel, OutError)
			|| !GetNumber(Pads, TEXT("raise"), TEXT("chassis.hovercraft.pads.raise"), true, Read.PadRaise, OutError)
			|| !GetNumber(Pads, TEXT("springRate"), TEXT("chassis.hovercraft.pads.springRate"), true, Read.PadSpringRate, OutError)
			|| !GetNumber(Pads, TEXT("damping"), TEXT("chassis.hovercraft.pads.damping"), true, Read.PadDamping, OutError)
			|| !GetObject(Json, TEXT("fan"), TEXT("chassis.hovercraft.fan"), Fan, OutError)
			|| !GetNumber(Fan, TEXT("thrust"), TEXT("chassis.hovercraft.fan.thrust"), true, Read.FanThrustNewtons, OutError)
			|| !GetNumber(Fan, TEXT("reverseShare"), TEXT("chassis.hovercraft.fan.reverseShare"), true, Read.ReverseShare, OutError)
			|| !GetNumber(Fan, TEXT("vectorAngle"), TEXT("chassis.hovercraft.fan.vectorAngle"), false, Read.VectorAngle, OutError)
			|| !GetNumber(Fan, TEXT("x"), TEXT("chassis.hovercraft.fan.x"), false, Read.FanX, OutError)
			|| !GetNumber(Fan, TEXT("z"), TEXT("chassis.hovercraft.fan.z"), true, Read.FanZ, OutError)
			|| !GetNumber(Fan, TEXT("length"), TEXT("chassis.hovercraft.fan.length"), true, Read.FanLength, OutError)
			|| !GetNumber(Fan, TEXT("width"), TEXT("chassis.hovercraft.fan.width"), true, Read.FanWidth, OutError)
			|| !GetNumber(Fan, TEXT("height"), TEXT("chassis.hovercraft.fan.height"), true, Read.FanHeight, OutError)
			|| !GetObject(Json, TEXT("fin"), TEXT("chassis.hovercraft.fin"), Fin, OutError)
			|| !GetNumber(Fin, TEXT("x"), TEXT("chassis.hovercraft.fin.x"), false, Read.FinX, OutError)
			|| !GetNumber(Fin, TEXT("height"), TEXT("chassis.hovercraft.fin.height"), true, Read.FinHeight, OutError)
			|| !GetNumber(Fin, TEXT("chord"), TEXT("chassis.hovercraft.fin.chord"), true, Read.FinChord, OutError)
			|| !GetNumber(Fin, TEXT("area"), TEXT("chassis.hovercraft.fin.area"), true, Read.FinAreaM2, OutError)
			|| !GetNumber(Fin, TEXT("maxControlAngle"), TEXT("chassis.hovercraft.fin.maxControlAngle"), true, Read.FinMaxControlAngle, OutError)
			|| !GetObject(Json, TEXT("skirt"), TEXT("chassis.hovercraft.skirt"), Skirt, OutError)
			|| !GetNumber(Skirt, TEXT("overhang"), TEXT("chassis.hovercraft.skirt.overhang"), false, Read.SkirtOverhang, OutError)
			|| !GetNumber(Skirt, TEXT("gap"), TEXT("chassis.hovercraft.skirt.gap"), true, Read.SkirtGap, OutError)
			|| !GetObject(Json, TEXT("damping"), TEXT("chassis.hovercraft.damping"), Damping, OutError)
			|| !GetNumber(Damping, TEXT("lateral"), TEXT("chassis.hovercraft.damping.lateral"), true, Read.LateralPerSecond, OutError)
			|| !GetNumber(Damping, TEXT("maxLateralAccel"), TEXT("chassis.hovercraft.damping.maxLateralAccel"), true, Read.MaxLateralAccel, OutError)
			|| !GetNumber(Damping, TEXT("yaw"), TEXT("chassis.hovercraft.damping.yaw"), true, Read.YawPerSecond, OutError)
			|| !GetNumber(Damping, TEXT("maxYawAccel"), TEXT("chassis.hovercraft.damping.maxYawAccel"), true, Read.MaxYawAccelDegrees, OutError)
			|| !GetNumber(Damping, TEXT("skirtBrake"), TEXT("chassis.hovercraft.damping.skirtBrake"), true, Read.SkirtBrakePerSecond, OutError)
			|| !GetNumber(Damping, TEXT("maxSkirtBrakeAccel"), TEXT("chassis.hovercraft.damping.maxSkirtBrakeAccel"), true, Read.MaxSkirtBrakeAccel, OutError))
		{
			return false;
		}
		Fin->TryGetNumberField(TEXT("stallAngle"), Read.FinStallAngle);
		if (!FMath::IsFinite(Read.FinStallAngle) || Read.FinStallAngle < 1.0)
		{
			return Refuse(OutError, TEXT("the preset's chassis.hovercraft.fin stallAngle is not a number, or is below 1"));
		}
		if (Read.ReverseShare > 1.0 || Read.VectorAngle < 0.0 || Read.VectorAngle > 45.0 || Read.SkirtOverhang < 0.0)
		{
			return Refuse(OutError, TEXT("the preset's chassis.hovercraft.fan reverseShare is above 1, its vectorAngle is outside 0 to 45, or its skirt's overhang is below 0"));
		}
		Read.bSet = true;
		Out = Read;
		return true;
	}

	/** The number at Path ("engine/maxTorque", objects separated by '/') under Root. False when any step is missing or the last is not a number. */
	bool FindNumber(const TSharedPtr<FJsonObject>& Root, const FString& Path, double& Out)
	{
		TArray<FString> Parts;
		Path.ParseIntoArray(Parts, TEXT("/"));
		const FJsonObject* Node = Root.Get();
		for (int32 Index = 0; Node && Index < Parts.Num(); ++Index)
		{
			if (Index + 1 == Parts.Num())
			{
				double Value = 0.0;
				if (!Node->TryGetNumberField(Parts[Index], Value) || !FMath::IsFinite(Value))
				{
					return false;
				}
				Out = Value;
				return true;
			}
			const TSharedPtr<FJsonObject>* Child = nullptr;
			if (!Node->TryGetObjectField(Parts[Index], Child) || !Child || !Child->IsValid())
			{
				return false;
			}
			Node = Child->Get();
		}
		return false;
	}

	/** One number of the handling a preset's defaults hold: where, which member, the schema default when it is absent, and whether there is none (so it is required). */
	struct FArchetypeField
	{
		const TCHAR* Path;
		double FMantleChassisHandling::* Member;
		double Default;
		bool bRequired;
	};

	/** The preset's defaults, read as handling; the centre of mass is the chassis's. False with the reason when a number with no schema default is missing. */
	bool ReadArchetype(const TSharedPtr<FJsonObject>& Defaults, FMantleChassisSpec& Spec, FString& OutError)
	{
		const FArchetypeField Fields[] = {
			{ TEXT("mass"), &FMantleChassisHandling::Mass, DefaultMass, false },
			{ TEXT("maxSpeed"), &FMantleChassisHandling::MaxSpeed, DefaultMaxSpeed, false },
			{ TEXT("dragCoefficient"), &FMantleChassisHandling::DragCoefficient, DefaultDragCoefficient, false },
			{ TEXT("frontalArea"), &FMantleChassisHandling::FrontalArea, DefaultFrontalArea, false },
			{ TEXT("engine/maxTorque"), &FMantleChassisHandling::MaxTorque, 0.0, true },
			{ TEXT("transmission/finalDrive"), &FMantleChassisHandling::FinalDrive, 0.0, true },
			{ TEXT("suspension/maxRaise"), &FMantleChassisHandling::MaxRaise, DefaultMaxRaise, false },
			{ TEXT("suspension/maxDrop"), &FMantleChassisHandling::MaxDrop, DefaultMaxDrop, false },
			{ TEXT("suspension/springRate"), &FMantleChassisHandling::SpringRate, DefaultSpringRate, false },
			{ TEXT("suspension/springPreload"), &FMantleChassisHandling::SpringPreload, DefaultSpringPreload, false },
			{ TEXT("suspension/damping"), &FMantleChassisHandling::SpringDamping, DefaultSpringDamping, false },
			{ TEXT("steering/maxAngle"), &FMantleChassisHandling::MaxSteerAngle, DefaultMaxSteerAngle, false },
			{ TEXT("brakes/brakeTorque"), &FMantleChassisHandling::BrakeTorque, DefaultBrakeTorque, false },
			{ TEXT("brakes/handbrakeTorque"), &FMantleChassisHandling::HandbrakeTorque, DefaultHandbrakeTorque, false },
			{ TEXT("tires/friction"), &FMantleChassisHandling::TireFriction, DefaultTireFriction, false },
			{ TEXT("camera/armLength"), &FMantleChassisHandling::CameraArmLength, DefaultCameraArmLength, false },
			{ TEXT("camera/height"), &FMantleChassisHandling::CameraHeight, DefaultCameraHeight, false },
		};
		FMantleChassisHandling Read;
		for (const FArchetypeField& Field : Fields)
		{
			double Value = 0.0;
			if (FindNumber(Defaults, Field.Path, Value))
			{
				Read.*Field.Member = Value;
			}
			else if (!Field.bRequired)
			{
				Read.*Field.Member = Field.Default;
			}
			else
			{
				return Refuse(OutError, FString::Printf(TEXT("the preset's defaults.%s is missing or not a number"), Field.Path));
			}
		}
		Read.CenterOfMass = Spec.CenterOfMass;
		Spec.Archetype = Read;
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> NumberArray(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Add(MakeShared<FJsonValueNumber>(V.X));
		Out.Add(MakeShared<FJsonValueNumber>(V.Y));
		Out.Add(MakeShared<FJsonValueNumber>(V.Z));
		return Out;
	}

	/** The nested object Key of Parent, made when it is not there. */
	TSharedPtr<FJsonObject> ChildObject(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Key)
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		if (Parent->TryGetObjectField(Key, Found) && Found && Found->IsValid())
		{
			return *Found;
		}
		const TSharedPtr<FJsonObject> Made = MakeShared<FJsonObject>();
		Parent->SetObjectField(Key, Made);
		return Made;
	}

	/** Sets Key to Value unless the object has it: the schema's default, which the cook fills and the reader requires. */
	void SetDefault(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, const double Value)
	{
		if (!Object->HasField(Key))
		{
			Object->SetNumberField(Key, Value);
		}
	}

	/** A Scaled chassis's handling over its document: every number FMantleChassisDerive makes (the centre of mass is the spec's own). */
	void ApplyHandling(const TSharedPtr<FJsonObject>& Doc, const FMantleChassisHandling& Handling)
	{
		Doc->SetNumberField(TEXT("mass"), Handling.Mass);
		Doc->SetNumberField(TEXT("maxSpeed"), Handling.MaxSpeed);
		Doc->SetNumberField(TEXT("dragCoefficient"), Handling.DragCoefficient);
		Doc->SetNumberField(TEXT("frontalArea"), Handling.FrontalArea);
		ChildObject(Doc, TEXT("engine"))->SetNumberField(TEXT("maxTorque"), Handling.MaxTorque);
		ChildObject(Doc, TEXT("transmission"))->SetNumberField(TEXT("finalDrive"), Handling.FinalDrive);
		const TSharedPtr<FJsonObject> Suspension = ChildObject(Doc, TEXT("suspension"));
		Suspension->SetNumberField(TEXT("maxRaise"), Handling.MaxRaise);
		Suspension->SetNumberField(TEXT("maxDrop"), Handling.MaxDrop);
		Suspension->SetNumberField(TEXT("springRate"), Handling.SpringRate);
		Suspension->SetNumberField(TEXT("springPreload"), Handling.SpringPreload);
		Suspension->SetNumberField(TEXT("damping"), Handling.SpringDamping);
		ChildObject(Doc, TEXT("steering"))->SetNumberField(TEXT("maxAngle"), Handling.MaxSteerAngle);
		const TSharedPtr<FJsonObject> Brakes = ChildObject(Doc, TEXT("brakes"));
		Brakes->SetNumberField(TEXT("brakeTorque"), Handling.BrakeTorque);
		Brakes->SetNumberField(TEXT("handbrakeTorque"), Handling.HandbrakeTorque);
		ChildObject(Doc, TEXT("tires"))->SetNumberField(TEXT("friction"), Handling.TireFriction);
		const TSharedPtr<FJsonObject> Camera = ChildObject(Doc, TEXT("camera"));
		Camera->SetNumberField(TEXT("armLength"), Handling.CameraArmLength);
		Camera->SetNumberField(TEXT("height"), Handling.CameraHeight);
	}

	/** How far from the origin, cm, a vehicle document's collision box may reach on an axis (limits.json collision/<i>/center and extent: +-1000). */
	constexpr double DocumentReach = 1000.0;

	/**
	 * A block as the document's collision box (its centre and half-extent), held inside DocumentReach of the origin: only a plane's physics boxes
	 * are, since a large wing or tail is past what the document can say. False when nothing of the block is left (the document's extent is at least 1 cm).
	 */
	bool HoldBlockInDocument(const FMantleChassisBlock& Block, FVector& OutCentre, FVector& OutExtent)
	{
		FVector Low = FVector::ZeroVector;
		FVector High = FVector::ZeroVector;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			Low[Axis] = FMath::Max(Block.Min[Axis], -DocumentReach);
			High[Axis] = FMath::Min(Block.Max[Axis], DocumentReach);
			if (!(High[Axis] - Low[Axis] >= 2.0))
			{
				return false;
			}
		}
		OutCentre = 0.5 * (Low + High);
		OutExtent = 0.5 * (High - Low);
		return true;
	}

	/** A preset id: a lowercase letter, then lowercase letters, digits and dashes. */
	bool IsId(const FString& Text)
	{
		bool bValid = Text.Len() > 0 && Text[0] >= 'a' && Text[0] <= 'z';
		for (int32 Index = 0; bValid && Index < Text.Len(); ++Index)
		{
			const TCHAR Char = Text[Index];
			bValid = (Char >= 'a' && Char <= 'z') || (Char >= '0' && Char <= '9') || Char == '-';
		}
		return bValid;
	}
}

FName FMantleChassisSpec::WheelBone(const int32 Index) const
{
	using namespace MantleChassisSpecPrivate;
	// Each axle is named by its own kind: a plane's centreline nose wheel is wheel_f, beside its pair of mains wheel_rl and wheel_rr.
	const int32 Axle = AxleOf(Index);
	return AxleIsCentreline(Axle) ? CentrelineWheelBoneName(Axles.Num(), Axle) : WheelBoneName(Axles.Num(), Axle, Index - FirstWheelOf(Axle));
}

FName FMantleChassisSpec::ForkBoneOfAxle(const int32 Axle) const
{
	return MantleChassisSpecPrivate::CentrelineForkBoneName(Axles.Num(), Axle);
}

const TCHAR* FMantleChassisSpec::TypeName(const EMantleChassisType Kind)
{
	switch (Kind)
	{
	case EMantleChassisType::Truck:
		return TEXT("truck");
	case EMantleChassisType::Motorcycle:
		return TEXT("motorcycle");
	case EMantleChassisType::Plane:
		return TEXT("plane");
	case EMantleChassisType::Helicopter:
		return TEXT("helicopter");
	case EMantleChassisType::Hovercraft:
		return TEXT("hovercraft");
	default:
		return TEXT("car");
	}
}

bool FMantleChassisSpec::ParseType(const FString& Name, EMantleChassisType& OutType)
{
	for (const EMantleChassisType Kind : { EMantleChassisType::Car, EMantleChassisType::Truck, EMantleChassisType::Motorcycle, EMantleChassisType::Plane,
			 EMantleChassisType::Helicopter, EMantleChassisType::Hovercraft })
	{
		if (Name.Equals(TypeName(Kind), ESearchCase::CaseSensitive))
		{
			OutType = Kind;
			return true;
		}
	}
	return false;
}

bool FMantleChassisSpec::IsPresetId(const FString& Text)
{
	return MantleChassisSpecPrivate::IsId(Text);
}

bool FMantleChassisSpec::Parse(const FString& PresetJsonText, FMantleChassisSpec& OutSpec, FString& OutError)
{
	using namespace MantleChassisSpecPrivate;
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(PresetJsonText), Root) || !Root.IsValid())
	{
		return Refuse(OutError, TEXT("the preset is not a JSON object"));
	}
	FMantleChassisSpec Spec;
	TSharedPtr<FJsonObject> DefaultsJson;
	TSharedPtr<FJsonObject> DeriveJson;
	TSharedPtr<FJsonObject> Chassis;
	TSharedPtr<FJsonObject> Layout;
	TSharedPtr<FJsonObject> Body;
	TSharedPtr<FJsonObject> SeatJson;
	TSharedPtr<FJsonObject> WheelJson;
	Root->TryGetStringField(TEXT("id"), Spec.Id);
	if (!GetObject(Root, TEXT("defaults"), TEXT("defaults"), DefaultsJson, OutError)
		|| !GetObject(Root, TEXT("chassis"), TEXT("chassis"), Chassis, OutError)
		|| !GetObject(Chassis, TEXT("layout"), TEXT("chassis.layout"), Layout, OutError)
		|| !GetObject(Chassis, TEXT("body"), TEXT("chassis.body"), Body, OutError)
		|| !GetObject(Chassis, TEXT("seat"), TEXT("chassis.seat"), SeatJson, OutError)
		|| !GetNumber(Layout, TEXT("wheelbase"), TEXT("chassis.layout.wheelbase"), true, Spec.Wheelbase, OutError)
		|| !GetNumber(Layout, TEXT("track"), TEXT("chassis.layout.track"), true, Spec.Track, OutError)
		|| !GetNumber(Layout, TEXT("wheelRadius"), TEXT("chassis.layout.wheelRadius"), true, Spec.WheelRadius, OutError)
		|| !GetNumber(Layout, TEXT("wheelWidth"), TEXT("chassis.layout.wheelWidth"), true, Spec.WheelWidth, OutError)
		|| !GetNumber(Body, TEXT("length"), TEXT("chassis.body.length"), true, Spec.Length, OutError)
		|| !GetNumber(Body, TEXT("width"), TEXT("chassis.body.width"), true, Spec.Width, OutError)
		|| !GetNumber(Body, TEXT("height"), TEXT("chassis.body.height"), true, Spec.Height, OutError)
		|| !GetNumber(Body, TEXT("groundClearance"), TEXT("chassis.body.groundClearance"), false, Spec.Clearance, OutError)
		|| !GetVector(Chassis, TEXT("centerOfMass"), TEXT("chassis.centerOfMass"), Spec.CenterOfMass, OutError)
		|| !GetVector(SeatJson, TEXT("location"), TEXT("chassis.seat.location"), Spec.Seat, OutError))
	{
		return false;
	}
	Body->TryGetNumberField(TEXT("x"), Spec.BodyX);
	// Which seat it is: a car's driver has a steering wheel, a rider's (a centreline chassis) handlebars and a pilot's a stick and a throttle.
	Layout->TryGetBoolField(TEXT("centreline"), Spec.bCentreline);
	const TSharedPtr<FJsonObject>* Seats = nullptr;
	Spec.bPassenger = Root->TryGetObjectField(TEXT("seats"), Seats) && Seats && (*Seats)->HasField(TEXT("passenger"));
	FString DriverKind;
	if (Root->TryGetObjectField(TEXT("seats"), Seats) && Seats && (*Seats)->TryGetStringField(TEXT("driver"), DriverKind))
	{
		Spec.DriverSeat = DriverKind;
	}
	Spec.bSteeringWheel = !Spec.bCentreline && !Spec.DriverSeat.Equals(TEXT("pilot"), ESearchCase::CaseSensitive);
	if (Spec.bSteeringWheel
		&& (!GetObject(SeatJson, TEXT("steeringWheel"), TEXT("chassis.seat.steeringWheel"), WheelJson, OutError)
			|| !GetVector(WheelJson, TEXT("location"), TEXT("chassis.seat.steeringWheel.location"), Spec.Hub, OutError)
			|| !GetNumber(WheelJson, TEXT("radius"), TEXT("chassis.seat.steeringWheel.radius"), true, Spec.RimRadius, OutError)))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* AxlesJson = nullptr;
	if (!Layout->TryGetArrayField(TEXT("axles"), AxlesJson) || !AxlesJson || AxlesJson->Num() < MinAxles || AxlesJson->Num() > MaxAxles)
	{
		return Refuse(OutError, FString::Printf(TEXT("the preset's chassis.layout.axles is missing or not %d to %d axles"), MinAxles, MaxAxles));
	}
	const int32 AxleCount = AxlesJson->Num();
	Spec.Axles.SetNum(AxleCount);
	for (int32 Axle = 0; Axle < AxleCount; ++Axle)
	{
		FMantleChassisAxle& Out = Spec.Axles[Axle];
		const TSharedPtr<FJsonObject> Object = (*AxlesJson)[Axle].IsValid() ? (*AxlesJson)[Axle]->AsObject() : nullptr;
		if (!Object.IsValid() || !Object->TryGetBoolField(TEXT("steer"), Out.bSteer) || !Object->TryGetBoolField(TEXT("drive"), Out.bDrive))
		{
			return Refuse(OutError, FString::Printf(TEXT("the preset's chassis.layout.axles[%d] needs steer and drive"), Axle));
		}
		Object->TryGetBoolField(TEXT("handbrake"), Out.bHandbrake);
		Object->TryGetBoolField(TEXT("centreline"), Out.bCentreline);
		// Evenly spaced unless the axle says where it sits.
		Out.Position = static_cast<double>(Axle) / static_cast<double>(AxleCount - 1);
		if (Object->HasField(TEXT("position")) && !Object->TryGetNumberField(TEXT("position"), Out.Position))
		{
			return Refuse(OutError, FString::Printf(TEXT("the preset's chassis.layout.axles[%d].position is not a number"), Axle));
		}
	}
	// Front to rear: the front axle at 0, the rearmost at 1 (the wheelbase), each behind the one before.
	for (int32 Axle = 0; Axle < AxleCount; ++Axle)
	{
		const double Position = Spec.Axles[Axle].Position;
		const bool bEnd = Axle == 0 || Axle == AxleCount - 1;
		const bool bEndWrong = bEnd && !FMath::IsNearlyEqual(Position, Axle == 0 ? 0.0 : 1.0);
		if (!FMath::IsFinite(Position) || bEndWrong || (Axle > 0 && Position <= Spec.Axles[Axle - 1].Position))
		{
			return Refuse(OutError, TEXT("the preset's chassis.layout.axles positions must run from 0 at the front axle to 1 at the rearmost, each behind the one before"));
		}
	}
	if (Spec.bCentreline && Spec.Axles[0].bSteer)
	{
		Spec.ForkBone = CentrelineForkBoneName(AxleCount, 0);
	}
	if (Spec.DriverSeat.Equals(TEXT("motorcycle"), ESearchCase::CaseSensitive) && !ParseRider(Chassis, Spec.Rider, OutError))
	{
		return false;
	}
	if (Chassis->HasField(TEXT("aircraft")) && !ParseAircraft(Chassis, Spec.Aircraft, OutError))
	{
		return false;
	}
	if (Chassis->HasField(TEXT("helicopter")) && !ParseHelicopter(Chassis, Spec.Helicopter, OutError))
	{
		return false;
	}
	if (Chassis->HasField(TEXT("hovercraft")) && !ParseHovercraft(Chassis, Spec.Hovercraft, OutError))
	{
		return false;
	}

	// What the size derives from: the type, the derive block, the hand-tuned handling every derived number is a ratio of, and the dimensions it is a ratio against.
	FString TypeText;
	if (!Root->TryGetStringField(TEXT("type"), TypeText) || !ParseType(TypeText, Spec.Type))
	{
		return Refuse(OutError, TEXT("the preset's type is missing or not car, truck, motorcycle, plane, helicopter or hovercraft"));
	}
	if ((Spec.Type == EMantleChassisType::Motorcycle) != Spec.bCentreline)
	{
		return Refuse(OutError, TEXT("the preset's type motorcycle and its chassis.layout.centreline go together"));
	}
	if ((Spec.Type == EMantleChassisType::Plane) != Spec.Aircraft.bSet)
	{
		return Refuse(OutError, TEXT("the preset's type plane and its chassis.aircraft go together"));
	}
	if (Spec.Type == EMantleChassisType::Plane && !Spec.DriverSeat.Equals(TEXT("pilot"), ESearchCase::CaseSensitive))
	{
		return Refuse(OutError, TEXT("the preset's type plane takes the pilot seat"));
	}
	if ((Spec.Type == EMantleChassisType::Helicopter) != Spec.Helicopter.bSet)
	{
		return Refuse(OutError, TEXT("the preset's type helicopter and its chassis.helicopter go together"));
	}
	if (Spec.Type == EMantleChassisType::Helicopter && !Spec.DriverSeat.Equals(TEXT("pilot"), ESearchCase::CaseSensitive))
	{
		return Refuse(OutError, TEXT("the preset's type helicopter takes the pilot seat"));
	}
	if ((Spec.Type == EMantleChassisType::Hovercraft) != Spec.Hovercraft.bSet)
	{
		return Refuse(OutError, TEXT("the preset's type hovercraft and its chassis.hovercraft go together"));
	}
	// A hovercraft's pads are the four corners of two axles: its definition turns each of the layout's wheels into one, from a car's seat.
	if (Spec.Type == EMantleChassisType::Hovercraft && (Spec.WheelCount() != 4 || !Spec.bSteeringWheel))
	{
		return Refuse(OutError, TEXT("the preset's type hovercraft stands on four pads (two axles, a pair each) and is driven from a car's seat"));
	}
	if (!GetObject(Root, TEXT("derive"), TEXT("derive"), DeriveJson, OutError) || !FMantleChassisDerive::ReadConstants(DeriveJson, Spec.Constants, OutError)
		|| !ReadArchetype(DefaultsJson, Spec, OutError))
	{
		return false;
	}
	Spec.Defaults = DefaultsJson;
	// A plane's areas and thrust are the archetype's own too: what its handling holds them to is a ratio of how its airframe differs from this one.
	Spec.Archetype.WingAreaM2 = Spec.Aircraft.Wing.AreaM2;
	Spec.Archetype.TailplaneAreaM2 = Spec.Aircraft.Tailplane.AreaM2;
	Spec.Archetype.FinAreaM2 = Spec.Aircraft.Fin.AreaM2;
	Spec.Archetype.EngineThrustNewtons = Spec.Aircraft.EngineThrustNewtons;
	// So is a helicopter's rotor thrust: a resized one's follows its mass, so the collective that holds its weight stays the preset's.
	Spec.Archetype.RotorThrustNewtons = Spec.Helicopter.RotorThrustNewtons;
	// And a hovercraft's fan, pads and rudder: a resized one's follow its mass, how far its pads come down and its fin.
	Spec.Archetype.HoverThrustNewtons = Spec.Hovercraft.FanThrustNewtons;
	Spec.Archetype.PadSpringRate = Spec.Hovercraft.PadSpringRate;
	Spec.Archetype.HoverFinAreaM2 = Spec.Hovercraft.FinAreaM2;
	FMantleChassisSize& Size = Spec.ArchetypeSize;
	Size.Wheelbase = Spec.Wheelbase;
	Size.Track = Spec.Track;
	Size.WheelRadius = Spec.WheelRadius;
	Size.WheelWidth = Spec.WheelWidth;
	Size.Length = Spec.Length;
	Size.Width = Spec.Width;
	Size.Height = Spec.Height;
	Size.Clearance = Spec.Clearance;
	Size.Axles = Spec.Axles;
	Size.bCentreline = Spec.bCentreline;
	Size.Aircraft = Spec.Aircraft;
	Size.Helicopter = Spec.Helicopter;
	Size.Hovercraft = Spec.Hovercraft;
	OutSpec = MoveTemp(Spec);
	return true;
}

FString FMantleChassisSpec::ComposeDocument(const FMantleChassisSpec& Spec, TArray<FString>* OutHeld)
{
	using namespace MantleChassisSpecPrivate;
	// A copy: the spec's defaults are never changed, so a spec composes the same document however often it is asked.
	const TSharedPtr<FJsonObject> Doc = MakeShared<FJsonObject>();
	FJsonObject::Duplicate(Spec.Defaults, Doc);
	Doc->SetNumberField(TEXT("schemaVersion"), 1);
	if (Spec.Derived.IsSet())
	{
		ApplyHandling(Doc, Spec.Derived.GetValue());
	}
	SetDefault(Doc, TEXT("mass"), DefaultMass);
	SetDefault(Doc, TEXT("maxSpeed"), DefaultMaxSpeed);
	SetDefault(Doc, TEXT("dragCoefficient"), DefaultDragCoefficient);
	SetDefault(Doc, TEXT("frontalArea"), DefaultFrontalArea);

	const TSharedPtr<FJsonObject> Engine = ChildObject(Doc, TEXT("engine"));
	SetDefault(Engine, TEXT("idleRPM"), DefaultIdleRpm);
	const TSharedPtr<FJsonObject> Transmission = ChildObject(Doc, TEXT("transmission"));
	SetDefault(Transmission, TEXT("reverseRatio"), DefaultReverseRatio);
	double RedlineRpm = 0.0;
	if (Engine->TryGetNumberField(TEXT("maxRPM"), RedlineRpm))
	{
		SetDefault(Transmission, TEXT("changeUpRPM"), FMath::FloorToDouble(RedlineRpm * 0.85));
		SetDefault(Transmission, TEXT("changeDownRPM"), FMath::FloorToDouble(RedlineRpm * 0.4));
	}
	const TSharedPtr<FJsonObject> Suspension = ChildObject(Doc, TEXT("suspension"));
	SetDefault(Suspension, TEXT("maxRaise"), DefaultMaxRaise);
	SetDefault(Suspension, TEXT("maxDrop"), DefaultMaxDrop);
	SetDefault(Suspension, TEXT("springRate"), DefaultSpringRate);
	SetDefault(Suspension, TEXT("springPreload"), DefaultSpringPreload);
	SetDefault(Suspension, TEXT("damping"), DefaultSpringDamping);
	const TSharedPtr<FJsonObject> Steering = ChildObject(Doc, TEXT("steering"));
	SetDefault(Steering, TEXT("maxAngle"), DefaultMaxSteerAngle);
	if (!Steering->HasField(TEXT("ackermann")))
	{
		Steering->SetBoolField(TEXT("ackermann"), true);
	}
	const TSharedPtr<FJsonObject> Brakes = ChildObject(Doc, TEXT("brakes"));
	SetDefault(Brakes, TEXT("brakeTorque"), DefaultBrakeTorque);
	SetDefault(Brakes, TEXT("handbrakeTorque"), DefaultHandbrakeTorque);
	SetDefault(ChildObject(Doc, TEXT("tires")), TEXT("friction"), DefaultTireFriction);
	const TSharedPtr<FJsonObject> Camera = ChildObject(Doc, TEXT("camera"));
	SetDefault(Camera, TEXT("armLength"), DefaultCameraArmLength);
	SetDefault(Camera, TEXT("height"), DefaultCameraHeight);

	// The wheels, on the layout: every key the reader requires.
	TArray<TSharedPtr<FJsonValue>> Wheels;
	for (int32 Index = 0; Index < Spec.WheelCount(); ++Index)
	{
		const int32 Axle = Spec.AxleOf(Index);
		const TSharedPtr<FJsonObject> Wheel = MakeShared<FJsonObject>();
		Wheel->SetStringField(TEXT("bone"), Spec.WheelBone(Index).ToString());
		Wheel->SetBoolField(TEXT("steering"), Spec.Axles[Axle].bSteer);
		Wheel->SetBoolField(TEXT("driven"), Spec.Axles[Axle].bDrive);
		Wheel->SetBoolField(TEXT("handbrake"), Spec.Axles[Axle].bHandbrake);
		Wheel->SetNumberField(TEXT("radius"), Spec.WheelRadius);
		Wheel->SetNumberField(TEXT("width"), Spec.WheelWidth);
		Wheels.Add(MakeShared<FJsonValueObject>(Wheel));
	}
	Doc->SetArrayField(TEXT("wheels"), Wheels);

	// The collision boxes, and the centre of mass. A car has the one, the body's. A plane's physics body is its fuselage, wing, tailplane and fin (the
	// body's box first), each held inside what the document can say; a helicopter's is its cabin, tail boom and fin the same way (its rotors and skids
	// collide with nothing: the skids' hidden wheels stand it on the ground).
	TArray<TSharedPtr<FJsonValue>> Boxes;
	if (Spec.Aircraft.bSet || Spec.Helicopter.bSet)
	{
		TArray<FMantleChassisBlock> Collided;
		if (Spec.Aircraft.bSet)
		{
			Collided = { Spec.BodyBlock(), Spec.WingBlock(), Spec.TailplaneBlock(), Spec.FinBlock() };
		}
		else
		{
			Collided = { Spec.BodyBlock(), Spec.TailBoomBlock(), Spec.TailFinBlock() };
		}
		for (const FMantleChassisBlock& Block : Collided)
		{
			FVector Centre = FVector::ZeroVector;
			FVector Extent = FVector::ZeroVector;
			if (HoldBlockInDocument(Block, Centre, Extent))
			{
				const TSharedPtr<FJsonObject> Box = MakeShared<FJsonObject>();
				Box->SetArrayField(TEXT("center"), NumberArray(Centre));
				Box->SetArrayField(TEXT("extent"), NumberArray(Extent));
				Boxes.Add(MakeShared<FJsonValueObject>(Box));
			}
		}
	}
	else
	{
		const TSharedPtr<FJsonObject> Box = MakeShared<FJsonObject>();
		Box->SetArrayField(TEXT("center"), NumberArray(FVector(Spec.BodyX, 0.0, Spec.Clearance + 0.5 * Spec.Height)));
		Box->SetArrayField(TEXT("extent"), NumberArray(FVector(0.5 * Spec.Length, 0.5 * Spec.Width, 0.5 * Spec.Height)));
		Boxes.Add(MakeShared<FJsonValueObject>(Box));
	}
	Doc->SetArrayField(TEXT("collision"), Boxes);
	Doc->SetArrayField(TEXT("centerOfMass"), NumberArray(Spec.CenterOfMass));

	// The driver's seat and steering wheel (a rider's seat has none), and the passenger's seat across the car.
	const TSharedPtr<FJsonObject> SeatJson = MakeShared<FJsonObject>();
	SeatJson->SetArrayField(TEXT("location"), NumberArray(Spec.Seat));
	if (Spec.bSteeringWheel)
	{
		const TSharedPtr<FJsonObject> Rim = MakeShared<FJsonObject>();
		Rim->SetArrayField(TEXT("location"), NumberArray(Spec.Hub));
		Rim->SetArrayField(TEXT("rotation"), NumberArray(FVector(SteeringWheelPitch, 0.0, 0.0)));
		Rim->SetNumberField(TEXT("radius"), Spec.RimRadius);
		SeatJson->SetObjectField(TEXT("steeringWheel"), Rim);
	}
	Doc->SetObjectField(TEXT("seat"), SeatJson);
	if (Spec.bPassenger)
	{
		const TSharedPtr<FJsonObject> Passenger = MakeShared<FJsonObject>();
		Passenger->SetArrayField(TEXT("location"), NumberArray(FVector(Spec.Seat.X, -Spec.Seat.Y, Spec.Seat.Z)));
		Doc->SetObjectField(TEXT("passengerSeat"), Passenger);
	}

	// The movement type's block and the seats' kinds: flat numbers, never the preset's id. After the seats and the camera they go on.
	FMantleVehicleChassisDocument::Compose(Spec, *Doc, OutHeld);
	// A ground car's steering curve and front and rear grip: in the document, so a published car and the chassis car are made alike.
	FMantleVehicleChassisDocument::ComposeGroundHandling(Spec, *Doc, OutHeld);

	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Doc.ToSharedRef(), Writer);
	return Text;
}

FMantleChassisBlock FMantleChassisSpec::BodyBlock() const
{
	return { FVector(BodyX - 0.5 * Length, -0.5 * Width, Clearance), FVector(BodyX + 0.5 * Length, 0.5 * Width, Clearance + Height) };
}

FMantleChassisBlock FMantleChassisSpec::WingBlock() const
{
	const FMantleChassisSurface& S = Aircraft.Wing;
	const double HalfThickness = 0.5 * SurfaceThickness * S.Chord;
	return { FVector(S.X - 0.75 * S.Chord, -0.5 * S.Span, S.Z - HalfThickness), FVector(S.X + 0.25 * S.Chord, 0.5 * S.Span, S.Z + HalfThickness) };
}

FMantleChassisBlock FMantleChassisSpec::HalfWingBlock(const bool bRight) const
{
	FMantleChassisBlock Block = WingBlock();
	// The wing is centred on Y, so a half is the wing cut at Y 0.
	if (bRight)
	{
		Block.Min.Y = 0.0;
	}
	else
	{
		Block.Max.Y = 0.0;
	}
	return Block;
}

FMantleChassisBlock FMantleChassisSpec::TailplaneBlock() const
{
	const FMantleChassisSurface& S = Aircraft.Tailplane;
	const double HalfThickness = 0.5 * SurfaceThickness * S.Chord;
	return { FVector(S.X - 0.75 * S.Chord, -0.5 * S.Span, S.Z - HalfThickness), FVector(S.X + 0.25 * S.Chord, 0.5 * S.Span, S.Z + HalfThickness) };
}

FMantleChassisBlock FMantleChassisSpec::FinBlock() const
{
	const FMantleChassisSurface& S = Aircraft.Fin;
	const double HalfThickness = 0.5 * SurfaceThickness * S.Chord;
	return { FVector(S.X - 0.75 * S.Chord, -HalfThickness, BodyTop()), FVector(S.X + 0.25 * S.Chord, HalfThickness, BodyTop() + S.Span) };
}

FVector FMantleChassisSpec::WingAerofoilLocation(const bool bRight) const
{
	const double Y = WingAerofoilSpanShare * 0.5 * Aircraft.Wing.Span;
	return FVector(Aircraft.Wing.X, bRight ? Y : -Y, Aircraft.Wing.Z);
}

FVector FMantleChassisSpec::ElevatorLocation() const
{
	return FVector(Aircraft.Tailplane.X, 0.0, Aircraft.Tailplane.Z);
}

FVector FMantleChassisSpec::RudderLocation() const
{
	return FVector(Aircraft.Fin.X, 0.0, BodyTop() + 0.5 * Aircraft.Fin.Span);
}

FVector FMantleChassisSpec::AileronBonePosition(const bool bRight) const
{
	const double Y = 0.25 * Aircraft.Wing.Span;
	return FVector(Aircraft.Wing.X, bRight ? Y : -Y, Aircraft.Wing.Z);
}

FVector FMantleChassisSpec::PropellerHub(const int32 Engine) const
{
	// One engine is on the centreline; two are a third of the half-span out, the first (0) on the left.
	const double Y = Aircraft.EngineCount < 2 ? 0.0 : (Engine == 0 ? -1.0 : 1.0) * (Aircraft.Wing.Span / 6.0);
	return FVector(Aircraft.EngineX, Y, Clearance + 0.5 * Height);
}

FVector FMantleChassisSpec::NozzleLocation(const int32 Engine) const
{
	// One jet is on the centreline; two sit side by side behind the tail, the first (0) on the left, close enough to share the fuselage's end.
	const double Y = Aircraft.EngineCount < 2 ? 0.0 : (Engine == 0 ? -1.0 : 1.0) * (TwinNozzleSpread * Width);
	return FVector(Aircraft.EngineX, Y, Clearance + 0.5 * Height);
}

FVector FMantleChassisSpec::EngineLocation(const int32 Engine) const
{
	return Aircraft.bPropeller ? PropellerHub(Engine) : NozzleLocation(Engine);
}

FMantleChassisBlock FMantleChassisSpec::NozzleBlock(const int32 Engine) const
{
	const FVector Exit = NozzleLocation(Engine);
	const double Half = 0.5 * (Aircraft.EngineCount < 2 ? SingleNozzleSize : TwinNozzleSize) * FMath::Min(Width, Height);
	// From the exit forward to the fuselage's rear end, so it reads as the engine's back end; never less than a centimetre long, whatever the preset says.
	const double Front = FMath::Max(BodyBlock().Min.X, Exit.X + 1.0);
	return { FVector(Exit.X, Exit.Y - Half, Exit.Z - Half), FVector(Front, Exit.Y + Half, Exit.Z + Half) };
}

FMantleChassisBlock FMantleChassisSpec::IntakeBlock(const bool bRight) const
{
	// Against the fuselage's side, half its height tall about its middle, from the wing's leading edge forward a fifth of the fuselage's length.
	const double Back = Aircraft.Wing.X + 0.25 * Aircraft.Wing.Chord;
	const double Inner = 0.5 * Width;
	const double Outer = Inner + IntakeDepthShare * Width;
	const double Middle = Clearance + 0.5 * Height;
	const double HalfHeight = 0.5 * IntakeHeightShare * Height;
	const FVector Min(Back, bRight ? Inner : -Outer, Middle - HalfHeight);
	const FVector Max(Back + IntakeLengthShare * Length, bRight ? Outer : -Inner, Middle + HalfHeight);
	return { Min, Max };
}

FVector FMantleChassisSpec::MainRotorHub() const
{
	return FVector(CenterOfMass.X, 0.0, Helicopter.RotorZ);
}

FVector FMantleChassisSpec::TailRotorHub() const
{
	const FMantleChassisBlock Boom = TailBoomBlock();
	return FVector(Boom.Min.X + 0.5 * Helicopter.FinChord, -(0.5 * Helicopter.BoomWidth + TailRotorGap * Scale), Helicopter.BoomZ);
}

FMantleChassisBlock FMantleChassisSpec::TailBoomBlock() const
{
	// From the cabin's rear end back: a boom on a cabin that is not centred on the skids still meets it.
	const double Front = BodyBlock().Min.X;
	const double HalfWidth = 0.5 * Helicopter.BoomWidth;
	const double HalfHeight = 0.5 * Helicopter.BoomHeight;
	return { FVector(Front - Helicopter.BoomLength, -HalfWidth, Helicopter.BoomZ - HalfHeight), FVector(Front, HalfWidth, Helicopter.BoomZ + HalfHeight) };
}

FMantleChassisBlock FMantleChassisSpec::TailFinBlock() const
{
	const FMantleChassisBlock Boom = TailBoomBlock();
	const double HalfThickness = 0.5 * SurfaceThickness * Helicopter.FinChord;
	return { FVector(Boom.Min.X, -HalfThickness, Boom.Max.Z), FVector(Boom.Min.X + Helicopter.FinChord, HalfThickness, Boom.Max.Z + Helicopter.FinHeight) };
}

FMantleChassisBlock FMantleChassisSpec::MastBlock() const
{
	// From the cabin's top up to the blades' underside, so the mast and the blades meet without overlapping.
	const FVector RotorHub = MainRotorHub();
	const double Half = MastHalfWidth * Scale;
	const double Top = FMath::Max(RotorHub.Z - RotorBladeHalfThickness * Scale, BodyTop() + 1.0);
	return { FVector(RotorHub.X - Half, -Half, BodyTop()), FVector(RotorHub.X + Half, Half, Top) };
}

FMantleChassisBlock FMantleChassisSpec::MainRotorBladeBlock(const int32 Blade) const
{
	const FVector RotorHub = MainRotorHub();
	const double Radius = 0.5 * Helicopter.RotorDiameter;
	const double Chord = RotorBladeHalfChord * Scale;
	const double Thickness = RotorBladeHalfThickness * Scale;
	const FVector Half = Blade == 0 ? FVector(Radius, Chord, Thickness) : FVector(Chord, Radius, Thickness);
	return { RotorHub - Half, RotorHub + Half };
}

FMantleChassisBlock FMantleChassisSpec::TailRotorBladeBlock(const int32 Blade) const
{
	const FVector RotorHub = TailRotorHub();
	const double Radius = 0.5 * Helicopter.TailRotorDiameter;
	const double Chord = TailBladeHalfChord * Scale;
	const double Thickness = TailBladeHalfThickness * Scale;
	const FVector Half = Blade == 0 ? FVector(Radius, Thickness, Chord) : FVector(Chord, Thickness, Radius);
	return { RotorHub - Half, RotorHub + Half };
}

FMantleChassisBlock FMantleChassisSpec::SkidBlock(const bool bRight) const
{
	// Along one side's wheels, from past the rear axle to further past the front one (where a skid curls up), its underside a little off the ground.
	const double Front = Wheelbase * (0.5 - Axles[0].Position) + SkidFrontReachShare * WheelRadius;
	const double Rear = Wheelbase * (0.5 - Axles.Last().Position) - SkidRearReachShare * WheelRadius;
	const double Y = (bRight ? 0.5 : -0.5) * Track;
	const double Half = 0.5 * SkidTubeShare * WheelRadius;
	const double Bottom = SkidTubeLiftShare * WheelRadius;
	return { FVector(Rear, Y - Half, Bottom), FVector(Front, Y + Half, Bottom + 2.0 * Half) };
}

FMantleChassisBlock FMantleChassisSpec::SkidLegBlock(const int32 Axle, const bool bRight) const
{
	// From the skid's top up to the cabin's underside, over the axle.
	const FMantleChassisBlock Skid = SkidBlock(bRight);
	const double X = Wheelbase * (0.5 - Axles[Axle].Position);
	const double Half = 0.5 * SkidTubeShare * WheelRadius;
	const double Y = 0.5 * (Skid.Min.Y + Skid.Max.Y);
	return { FVector(X - Half, Y - Half, Skid.Max.Z), FVector(X + Half, Y + Half, FMath::Max(Clearance, Skid.Max.Z + 1.0)) };
}

FMantleChassisBlock FMantleChassisSpec::SkidCrossTubeBlock(const int32 Axle) const
{
	// Across the track just under the cabin, joining the axle's two legs.
	const double X = Wheelbase * (0.5 - Axles[Axle].Position);
	const double Half = 0.5 * SkidTubeShare * WheelRadius;
	const double Top = FMath::Max(Clearance, SkidBlock(false).Max.Z + 2.0 * Half);
	return { FVector(X - Half, -0.5 * Track - Half, Top - 2.0 * Half), FVector(X + Half, 0.5 * Track + Half, Top) };
}

FVector FMantleChassisSpec::PadLocation(const int32 Index) const
{
	const FVector Wheel = WheelCentre(Index);
	return FVector(Wheel.X, Wheel.Y, Clearance);
}

FVector FMantleChassisSpec::FanLocation() const
{
	return FVector(Hovercraft.FanX, 0.0, Hovercraft.FanZ);
}

FVector FMantleChassisSpec::HoverRudderLocation() const
{
	return FVector(Hovercraft.FinX, 0.0, BodyTop() + 0.5 * Hovercraft.FinHeight);
}

FMantleChassisBlock FMantleChassisSpec::SkirtBlock() const
{
	// Under the hull, a little wider than it, its underside off the ground; never less than a centimetre tall, whatever the preset says.
	const FMantleChassisBlock Hull = BodyBlock();
	const double Out = Hovercraft.SkirtOverhang;
	const double Bottom = FMath::Min(Hovercraft.SkirtGap, Clearance - 1.0);
	return { FVector(Hull.Min.X - Out, Hull.Min.Y - Out, Bottom), FVector(Hull.Max.X + Out, Hull.Max.Y + Out, Clearance) };
}

FMantleChassisBlock FMantleChassisSpec::FanPodBlock() const
{
	const FVector Half(0.5 * Hovercraft.FanLength, 0.5 * Hovercraft.FanWidth, 0.5 * Hovercraft.FanHeight);
	return { FanLocation() - Half, FanLocation() + Half };
}

FMantleChassisBlock FMantleChassisSpec::HoverFinBlock() const
{
	const double HalfChord = 0.5 * Hovercraft.FinChord;
	const double HalfThickness = 0.5 * SurfaceThickness * Hovercraft.FinChord;
	return { FVector(Hovercraft.FinX - HalfChord, -HalfThickness, BodyTop()), FVector(Hovercraft.FinX + HalfChord, HalfThickness, BodyTop() + Hovercraft.FinHeight) };
}

FMantleChassisSpec FMantleChassisSpec::Scaled(const FMantleChassisSpec& Spec, const double Factor)
{
	if (!FMath::IsFinite(Factor) || Factor <= 0.0 || Factor == 1.0)
	{
		return Spec;
	}
	FMantleChassisSpec Out = Spec;
	Out.Scale = Spec.Scale * Factor;
	// The layout: the axles' positions are shares of the wheelbase, so they stay.
	Out.Wheelbase *= Factor;
	Out.Track *= Factor;
	Out.WheelRadius *= Factor;
	Out.WheelWidth *= Factor;
	Out.Length *= Factor;
	Out.Width *= Factor;
	Out.Height *= Factor;
	Out.Clearance *= Factor;
	Out.Seat *= Factor;
	if (Spec.bSteeringWheel)
	{
		Out.Hub *= Factor;
		// The document holds a rim to 10 to 30 cm (limits.json seat/steeringWheel/radius): a hand's span, not the car's.
		Out.RimRadius = FMath::Clamp(Spec.RimRadius * Factor, 10.0, 30.0);
	}
	FMantleVehicleChassisRider& RiderOut = Out.Rider;
	RiderOut.LeftGrip *= Factor;
	RiderOut.RightGrip *= Factor;
	RiderOut.LeftPeg *= Factor;
	RiderOut.RightPeg *= Factor;
	RiderOut.PelvisOffset *= Factor;
	RiderOut.StopPelvisShift *= Factor;
	// A plane's body box, airframe and cockpit grow with it. Its angles do not, and its areas and thrust follow from the handling below.
	Out.BodyX *= Factor;
	if (Out.Aircraft.bSet)
	{
		FMantleChassisAircraft& Plane = Out.Aircraft;
		for (FMantleChassisSurface* Surface : { &Plane.Wing, &Plane.Tailplane, &Plane.Fin })
		{
			Surface->Span *= Factor;
			Surface->Chord *= Factor;
			Surface->X *= Factor;
			Surface->Z *= Factor;
		}
		// A jet's nozzle exit too; a jet has no propeller, whose radius stays 0.
		Plane.EngineX *= Factor;
		Plane.PropellerRadius *= Factor;
		Plane.ThrottleGrip *= Factor;
		Plane.StickGrip *= Factor;
	}
	// A helicopter's rotors, tail and cockpit grow with it too; its thrust follows the handling below, so the collective that holds its weight stays.
	if (Out.Helicopter.bSet)
	{
		FMantleChassisHelicopter& Heli = Out.Helicopter;
		for (double* Length : { &Heli.RotorDiameter, &Heli.RotorZ, &Heli.BoomLength, &Heli.BoomWidth, &Heli.BoomHeight, &Heli.BoomZ, &Heli.TailRotorDiameter,
				 &Heli.FinHeight, &Heli.FinChord })
		{
			*Length *= Factor;
		}
		Heli.CollectiveGrip *= Factor;
		Heli.CyclicGrip *= Factor;
	}
	// A hovercraft's pads' reach, its fan's pod, its fin and its skirt grow with it; its push, spring rate and rudder area follow the handling below, and its
	// damping is accelerations, which do not scale.
	if (Out.Hovercraft.bSet)
	{
		FMantleChassisHovercraft& Hover = Out.Hovercraft;
		for (double* Length : { &Hover.PadTravel, &Hover.PadRaise, &Hover.FanX, &Hover.FanZ, &Hover.FanLength, &Hover.FanWidth, &Hover.FanHeight, &Hover.FinX,
				 &Hover.FinHeight, &Hover.FinChord, &Hover.SkirtOverhang, &Hover.SkirtGap })
		{
			*Length *= Factor;
		}
	}

	// The handling follows the new size. The handling before is what a rider's rear brake and tyre are relative to.
	const FMantleChassisHandling Before = Spec.Derived.IsSet() ? Spec.Derived.GetValue() : Spec.Archetype;
	const FMantleChassisHandling After = FMantleChassisDerive::Derive(Out, FMantleChassisFeel());
	if (Before.BrakeTorque > 0.0)
	{
		RiderOut.RearBrakeTorque *= After.BrakeTorque / Before.BrakeTorque;
	}
	if (Before.TireFriction > 0.0)
	{
		RiderOut.RearTireFriction *= After.TireFriction / Before.TireFriction;
	}
	Out.CenterOfMass = After.CenterOfMass;
	Out.Derived = After;
	// The preset's trim is for the preset's weight, wings and thrust: a plane of another size needs its own camber and cruise throttle to fly level hands-off.
	Trim(Out);
	return Out;
}

bool FMantleChassisSpec::Trim(FMantleChassisSpec& InOutSpec)
{
	FMantleChassisAircraft& Plane = InOutSpec.Aircraft;
	if (!Plane.bSet || Plane.CruiseSpeedCmPerSec <= 0.0)
	{
		return false;
	}
	const FMantleChassisHandling& Handling = InOutSpec.Derived.IsSet() ? InOutSpec.Derived.GetValue() : InOutSpec.Archetype;
	double Camber = Plane.Tailplane.Camber;
	double Throttle = Plane.CruiseThrottle;
	if (!FMantleChassisDerive::SolveTrim(InOutSpec, Handling, Camber, Throttle))
	{
		if (InOutSpec.Derived.IsSet())
		{
			InOutSpec.Derived.GetValue().Clamped.Add(TEXT("flight/aerofoils/*/camber: no tailplane camber holds level flight at the cruise speed, the preset's kept"));
		}
		return false;
	}
	Plane.Tailplane.Camber = Camber;
	Plane.CruiseThrottle = Throttle;
	return true;
}

bool FMantleChassisSpec::ParseSelector(const FString& Selector, FString& OutPresetId, double& OutScale, FString& OutError)
{
	using namespace MantleChassisSpecPrivate;
	if (Selector.IsEmpty() || Selector.Len() > MaxSelectorLength)
	{
		return Refuse(OutError, FString::Printf(TEXT("a vehicle preset selector is 1 to %d characters"), MaxSelectorLength));
	}
	FString PresetText = Selector;
	int32 At = INDEX_NONE;
	const bool bScaled = Selector.FindChar(TEXT('@'), At);
	int32 Milli = 1000;
	if (bScaled)
	{
		PresetText = Selector.Left(At);
		// Digits, at most one point and at most three digits after it: the scale in thousandths, read as whole numbers so it is the same
		// number whatever a machine's locale or floating point.
		int32 Whole = 0;
		int32 Frac = 0;
		int32 WholeDigits = 0;
		int32 FracDigits = 0;
		bool bPoint = false;
		bool bValid = true;
		for (int32 Index = At + 1; bValid && Index < Selector.Len(); ++Index)
		{
			const TCHAR Char = Selector[Index];
			if (Char >= '0' && Char <= '9')
			{
				if (bPoint)
				{
					bValid = ++FracDigits <= 3;
					Frac = Frac * 10 + (Char - '0');
				}
				else
				{
					bValid = ++WholeDigits <= 2;
					Whole = Whole * 10 + (Char - '0');
				}
			}
			else
			{
				bValid = Char == '.' && !bPoint && WholeDigits > 0;
				bPoint = true;
			}
		}
		bValid = bValid && WholeDigits > 0 && (!bPoint || FracDigits > 0);
		for (int32 Pad = FracDigits; bValid && Pad < 3; ++Pad)
		{
			Frac *= 10;
		}
		Milli = bValid ? Whole * 1000 + Frac : 0;
		if (Milli < FMath::RoundToInt(MinScale * 1000.0) || Milli > FMath::RoundToInt(MaxScale * 1000.0))
		{
			return Refuse(OutError, FString::Printf(TEXT("the scale in '%s' is not a number from %g to %g with at most three decimals"), *Selector, MinScale, MaxScale));
		}
	}
	if (!IsId(PresetText))
	{
		return Refuse(OutError, FString::Printf(TEXT("'%s' is not a vehicle preset id"), *PresetText));
	}
	OutPresetId = PresetText;
	OutScale = static_cast<double>(Milli) / 1000.0;
	return true;
}

FString FMantleChassisSpec::MakeSelector(const FString& PresetId, const double Factor)
{
	const int32 Milli = FMath::RoundToInt(FMath::Clamp(Factor, MinScale, MaxScale) * 1000.0);
	if (Milli == 1000)
	{
		return PresetId;
	}
	FString Text = FString::Printf(TEXT("%d"), Milli / 1000);
	if (Milli % 1000 != 0)
	{
		FString Fraction = FString::Printf(TEXT("%03d"), Milli % 1000);
		while (Fraction.EndsWith(TEXT("0")))
		{
			Fraction.LeftChopInline(1);
		}
		Text += TEXT(".") + Fraction;
	}
	return PresetId + TEXT("@") + Text;
}
