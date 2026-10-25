// Copyright Joshua Eger. All Rights Reserved.
#include "MantleChassisDerive.h"

#include "Dom/JsonObject.h"
#include "MantleChassisSpec.h"
#include "MantleVehicleAsset.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleChassisDerivePrivate
{
	/** The least a wheel-centre-to-centre-of-mass height may be above the wheel centres, cm (never below the wheel radius + 2 cm). */
	constexpr double CentreOfMassFloor = 2.0;

	/** Suspension travel each way, cm. */
	constexpr double MinTravel = 3.0;
	constexpr double MaxTravel = 40.0;

	/** The chase camera, cm. */
	constexpr double MinCameraArm = 250.0;
	constexpr double MaxCameraArm = 3000.0;
	constexpr double MinCameraHeight = 80.0;
	constexpr double MaxCameraHeight = 1000.0;

	/** How far a dimension may differ from the archetype's before the rules stop following it: a ratio outside this is a unit slip or a zero, not a vehicle. */
	constexpr double MinRatio = 1.0e-3;
	constexpr double MaxRatio = 1.0e3;

	/** The digits a result keeps. */
	constexpr int32 SignificantDigits = 6;

	/** The document paths Derive holds a result to (limits.json placeAssets.vehicle.ranges), named once so the list the contract test checks is the list Derive uses. */
	namespace DocPath
	{
		constexpr const TCHAR* Mass = TEXT("mass");
		constexpr const TCHAR* MaxSpeed = TEXT("maxSpeed");
		constexpr const TCHAR* FrontalArea = TEXT("frontalArea");
		constexpr const TCHAR* MaxTorque = TEXT("engine/maxTorque");
		constexpr const TCHAR* FinalDrive = TEXT("transmission/finalDrive");
		constexpr const TCHAR* MaxRaise = TEXT("suspension/maxRaise");
		constexpr const TCHAR* MaxDrop = TEXT("suspension/maxDrop");
		constexpr const TCHAR* SpringRate = TEXT("suspension/springRate");
		constexpr const TCHAR* SpringPreload = TEXT("suspension/springPreload");
		constexpr const TCHAR* MaxSteerAngle = TEXT("steering/maxAngle");
		constexpr const TCHAR* BrakeTorque = TEXT("brakes/brakeTorque");
		constexpr const TCHAR* HandbrakeTorque = TEXT("brakes/handbrakeTorque");
		constexpr const TCHAR* TireFriction = TEXT("tires/friction");
		constexpr const TCHAR* CameraArmLength = TEXT("camera/armLength");
		constexpr const TCHAR* CameraHeight = TEXT("camera/height");
	}

	/** One constant of the derive block: its JSON name and its member. */
	struct FConstantEntry
	{
		const TCHAR* Name;
		double FMantleChassisDeriveConstants::* Member;
	};

	/** The derive block's constants in the order a preset lists them; each is a number that is not below 0. */
	const FConstantEntry ConstantTable[] = {
		{ TEXT("stability"), &FMantleChassisDeriveConstants::Stability },
		{ TEXT("cameraArmPerLength"), &FMantleChassisDeriveConstants::CameraArmPerLength },
		{ TEXT("cameraHeightPerHeight"), &FMantleChassisDeriveConstants::CameraHeightPerHeight },
	};

	/** The path as the contract test names it, for every path the rules hold a result to. */
	const TCHAR* const LimitedPathTable[] = {
		DocPath::Mass, DocPath::MaxSpeed, DocPath::FrontalArea, DocPath::MaxTorque, DocPath::FinalDrive, DocPath::MaxRaise, DocPath::MaxDrop,
		DocPath::SpringRate, DocPath::SpringPreload, DocPath::MaxSteerAngle, DocPath::BrakeTorque, DocPath::HandbrakeTorque, DocPath::TireFriction,
		DocPath::CameraArmLength, DocPath::CameraHeight,
	};

	/** Whether a rule's answer differs from what it was held to by more than the floating-point noise of working it out (a result at a bound must not look held). */
	bool IsHeld(const double Value, const double Held)
	{
		return !(FMath::Abs(Value - Held) <= 1.0e-9 * FMath::Max(1.0, FMath::Abs(Value)));
	}

	/** A number the rules moved off their answer, in Clamped: where, what the rule gave, what it was held to. */
	void NoteHeld(TArray<FString>& Clamped, const TCHAR* Path, const double Value, const double Held)
	{
		Clamped.Add(FString::Printf(TEXT("%s %.6g held to %.6g"), Path, Value, Held));
	}

	/**
	 * Value held inside the document's absolute range for Path (limits.json placeAssets.vehicle.ranges, as the reader enforces it) and inside Min and
	 * Max, the bounds of the rule itself. A hold goes to Clamped, with the raw value; a Path the document has no range for is a mistake in Derive and
	 * is listed too, so the contract test sees it.
	 */
	double Limited(TArray<FString>& Clamped, const TCHAR* Path, const double Value, const double Min = -TNumericLimits<double>::Max(), const double Max = TNumericLimits<double>::Max())
	{
		double Low = Min;
		double High = Max;
		double RangeLow = 0.0;
		double RangeHigh = 0.0;
		if (FMantleVehicleLimits::GetRange(FString(Path), RangeLow, RangeHigh))
		{
			Low = FMath::Max(Low, RangeLow);
			High = FMath::Min(High, RangeHigh);
		}
		else
		{
			Clamped.Add(FString::Printf(TEXT("%s has no document range"), Path));
		}
		const double Held = FMath::Min(FMath::Max(Value, Low), High);
		if (IsHeld(Value, Held))
		{
			NoteHeld(Clamped, Path, Value, Held);
		}
		return Held;
	}

	/**
	 * How many times Value is Reference. 1 when the archetype has no such size (a spec not read from a preset), and held to MinRatio to MaxRatio so a
	 * size that is zero, negative or not a number gives a number the rest of the rules can multiply.
	 */
	double SizeRatio(const double Value, const double Reference)
	{
		if (!(Reference > 0.0))
		{
			return 1.0;
		}
		const double Result = Value / Reference;
		return Result > MinRatio ? FMath::Min(Result, MaxRatio) : MinRatio;
	}

	/** 10 to the power Exponent, 0 to 22: every one of them is exact in a double. */
	double PowerOfTen(const int32 Exponent)
	{
		static const double Powers[] = { 1.0e0, 1.0e1, 1.0e2, 1.0e3, 1.0e4, 1.0e5, 1.0e6, 1.0e7, 1.0e8, 1.0e9, 1.0e10, 1.0e11, 1.0e12, 1.0e13, 1.0e14, 1.0e15,
			1.0e16, 1.0e17, 1.0e18, 1.0e19, 1.0e20, 1.0e21, 1.0e22 };
		return Powers[Exponent];
	}

	/**
	 * Value to SignificantDigits significant digits. Windows Game and Client targets compile with /fp:fast while the Editor and the Linux server are
	 * precise, so the last bit of a rule's answer can differ between them; rounding to six digits absorbs that except within about 1e-15 (relative) of a
	 * rounding tie. The rounding itself is only comparisons, one multiply or divide by an exact power of ten and a floor, with no libm call (log10 is not
	 * exact at a power of ten). The result is the double nearest the decimal, the one a JSON reader gives for the same text.
	 */
	double RoundSignificant(const double Value)
	{
		if (Value == 0.0 || !FMath::IsFinite(Value))
		{
			return Value;
		}
		const double Magnitude = FMath::Abs(Value);
		// Where the first digit is: 10^Exponent <= Magnitude < 10^(Exponent + 1). The search stops at 10^22 and 10^-17, the scales PowerOfTen has exactly; nothing derived is outside them.
		int32 Exponent = 0;
		if (Magnitude >= 1.0)
		{
			while (Exponent < 22 && Magnitude >= PowerOfTen(Exponent + 1))
			{
				++Exponent;
			}
		}
		else
		{
			while (Exponent > -17 && Magnitude * PowerOfTen(-Exponent) < 1.0)
			{
				--Exponent;
			}
		}
		const int32 Step = SignificantDigits - 1 - Exponent;
		const double Rounded = Step >= 0 ? FMath::FloorToDouble(Magnitude * PowerOfTen(Step) + 0.5) / PowerOfTen(Step)
										  : FMath::FloorToDouble(Magnitude / PowerOfTen(-Step) + 0.5) * PowerOfTen(-Step);
		return Value < 0.0 ? -Rounded : Rounded;
	}

	void RoundHandling(FMantleChassisHandling& H)
	{
		for (double* Number : { &H.Mass, &H.MaxSpeed, &H.DragCoefficient, &H.FrontalArea, &H.MaxTorque, &H.FinalDrive, &H.MaxRaise, &H.MaxDrop, &H.SpringRate,
				 &H.SpringPreload, &H.SpringDamping, &H.MaxSteerAngle, &H.BrakeTorque, &H.HandbrakeTorque, &H.TireFriction, &H.CameraArmLength, &H.CameraHeight,
				 &H.WingAreaM2, &H.TailplaneAreaM2, &H.FinAreaM2, &H.EngineThrustNewtons, &H.RotorThrustNewtons, &H.HoverThrustNewtons, &H.PadSpringRate,
				 &H.HoverFinAreaM2 })
		{
			*Number = RoundSignificant(*Number);
		}
		H.CenterOfMass = FVector(RoundSignificant(H.CenterOfMass.X), RoundSignificant(H.CenterOfMass.Y), RoundSignificant(H.CenterOfMass.Z));
	}
}

bool FMantleChassisDerive::ReadConstants(const TSharedPtr<FJsonObject>& Block, FMantleChassisDeriveConstants& OutConstants, FString& OutError)
{
	using namespace MantleChassisDerivePrivate;
	FMantleChassisDeriveConstants Read;
	for (const FConstantEntry& Entry : ConstantTable)
	{
		double Value = 0.0;
		if (!Block.IsValid() || !Block->TryGetNumberField(Entry.Name, Value) || !FMath::IsFinite(Value) || Value < 0.0)
		{
			OutError = FString::Printf(TEXT("the preset's derive.%s is missing, not a number or below 0"), Entry.Name);
			return false;
		}
		Read.*Entry.Member = Value;
	}
	OutConstants = Read;
	return true;
}

TArray<FString> FMantleChassisDerive::LimitedPaths()
{
	TArray<FString> Paths;
	for (const TCHAR* Path : MantleChassisDerivePrivate::LimitedPathTable)
	{
		Paths.Add(Path);
	}
	return Paths;
}

FMantleChassisHandling FMantleChassisDerive::Derive(const FMantleChassisSpec& Spec, const FMantleChassisFeel& Feel)
{
	using namespace MantleChassisDerivePrivate;
	const FMantleChassisHandling& Arch = Spec.Archetype;
	const FMantleChassisSize& Base = Spec.ArchetypeSize;
	const FMantleChassisDeriveConstants& K = Spec.Constants;
	const double Reach = FMantleChassisFeel::Reach;
	const double Grip = FMath::Clamp(Feel.Grip, -1.0, 1.0);
	const double Power = FMath::Clamp(Feel.Power, -1.0, 1.0);
	const double Stiffness = FMath::Clamp(Feel.Stiffness, -1.0, 1.0);
	const double TopSpeed = FMath::Clamp(Feel.TopSpeed, -1.0, 1.0);
	const double Wheels = static_cast<double>(FMath::Max(1, Spec.WheelCount()));
	const double BaseWheels = static_cast<double>(FMath::Max(1, Base.WheelCount()));
	// How large the wheel is against the archetype's: the one number every size-dependent rule below that is not a volume reads.
	const double RadiusRatio = SizeRatio(Spec.WheelRadius, Base.WheelRadius);

	FMantleChassisHandling H;

	// Mass: the archetype's, by the body's volume (length x width x (height + ground clearance)) against its own.
	const double VolumeRatio = SizeRatio(Spec.Length * Spec.Width * (Spec.Height + Spec.Clearance), Base.Length * Base.Width * (Base.Height + Base.Clearance));
	H.Mass = Limited(H.Clamped, DocPath::Mass, Arch.Mass * VolumeRatio);
	const double MassRatio = SizeRatio(H.Mass, Arch.Mass);

	// Centre of mass: forward by the archetype's share of the wheelbase, up by its share of the body above the wheel centres, lowered if the track needs it
	// to stay upright, and never below the floor above the wheel centres.
	const double BaseBodyAbove = Base.Clearance + Base.Height - Base.WheelRadius;
	const double ComShare = BaseBodyAbove > 0.0 ? (Arch.CenterOfMass.Z - Base.WheelRadius) / BaseBodyAbove : 0.0;
	const double WantedComZ = Spec.WheelRadius + ComShare * (Spec.Clearance + Spec.Height - Spec.WheelRadius);
	double ComZ = WantedComZ;
	if (K.Stability > 0.0 && !Spec.bCentreline)
	{
		ComZ = FMath::Min(ComZ, Spec.Track / (2.0 * K.Stability));
	}
	ComZ = FMath::Max(ComZ, Spec.WheelRadius + CentreOfMassFloor);
	if (IsHeld(WantedComZ, ComZ))
	{
		NoteHeld(H.Clamped, TEXT("centerOfMass/z"), WantedComZ, ComZ);
	}
	H.CenterOfMass = FVector(Arch.CenterOfMass.X * SizeRatio(Spec.Wheelbase, Base.Wheelbase), Arch.CenterOfMass.Y, ComZ);

	// Springs: each wheel carries m / n and a ride frequency f makes its rate (m / n) x (2 pi f)^2, so against the archetype the rate follows the mass, the
	// wheels and the frequency squared. f falls as the wheel grows (1 / sqrt of the radius ratio), so the sag is the same share of the travel at every size.
	const double FrequencyRatio = (1.0 / FMath::Sqrt(RadiusRatio)) * (1.0 + Reach * Stiffness);
	H.SpringRate = Limited(H.Clamped, DocPath::SpringRate, Arch.SpringRate * MassRatio * (BaseWheels / Wheels) * FrequencyRatio * FrequencyRatio);
	H.SpringPreload = Limited(H.Clamped, DocPath::SpringPreload, Arch.SpringPreload * SizeRatio(H.SpringRate, Arch.SpringRate));
	H.MaxRaise = Limited(H.Clamped, DocPath::MaxRaise, Arch.MaxRaise * RadiusRatio, MinTravel, MaxTravel);
	H.MaxDrop = Limited(H.Clamped, DocPath::MaxDrop, Arch.MaxDrop * RadiusRatio, MinTravel, MaxTravel);
	// Damping stays the archetype's absolute value: the Chaos solver reads it as an absolute (PBDSuspensionConstraints.cpp:579), not a share of the spring,
	// so it is the first suspect if a scaled car pogoes.
	H.SpringDamping = Arch.SpringDamping;

	// Steering: the lock is an angle, the archetype's own whatever the size. The turning circle (the distance from the steered axles to the middle of the
	// others over tan(lock)) then follows the wheelbase and the axle layout, as it does on a real car: a longer wheelbase turns wider.
	H.MaxSteerAngle = Limited(H.Clamped, DocPath::MaxSteerAngle, Arch.MaxSteerAngle);

	// Engine: the archetype's power-to-weight, so the torque at the curve's peak follows the mass.
	H.MaxTorque = Limited(H.Clamped, DocPath::MaxTorque, Arch.MaxTorque * MassRatio * (1.0 + Reach * Power));

	// Top speed and gearing: the archetype's top speed, less in proportion to a smaller wheel; the final drive makes the top gear turn the same share of
	// the redline at that speed: the wheel's size over the speed.
	H.MaxSpeed = Limited(H.Clamped, DocPath::MaxSpeed, Arch.MaxSpeed * (1.0 + Reach * TopSpeed) * FMath::Min(1.0, RadiusRatio));
	H.FinalDrive = Limited(H.Clamped, DocPath::FinalDrive, Arch.FinalDrive * RadiusRatio / SizeRatio(H.MaxSpeed, Arch.MaxSpeed));

	// Brakes: the same deceleration, so each wheel's torque follows the mass it carries and its radius; the handbrake follows the brakes.
	H.BrakeTorque = Limited(H.Clamped, DocPath::BrakeTorque, Arch.BrakeTorque * MassRatio * (BaseWheels / Wheels) * RadiusRatio);
	H.HandbrakeTorque = Limited(H.Clamped, DocPath::HandbrakeTorque, Arch.HandbrakeTorque * SizeRatio(H.BrakeTorque, Arch.BrakeTorque));

	// Tyres and drag: the archetype's tyre and drag coefficient; the frontal area is the body's face against its own.
	H.TireFriction = Limited(H.Clamped, DocPath::TireFriction, Arch.TireFriction * (1.0 + Reach * Grip));
	H.DragCoefficient = Arch.DragCoefficient;
	H.FrontalArea = Limited(H.Clamped, DocPath::FrontalArea,
		Arch.FrontalArea * SizeRatio(Spec.Width * (Spec.Height + Spec.Clearance), Base.Width * (Base.Height + Base.Clearance)));

	// Camera: further back for a longer body, higher for a taller one, by the type's share of the difference: the driver does not scale.
	H.CameraArmLength = Limited(H.Clamped, DocPath::CameraArmLength, Arch.CameraArmLength + K.CameraArmPerLength * (Spec.Length - Base.Length), MinCameraArm, MaxCameraArm);
	H.CameraHeight = Limited(H.Clamped, DocPath::CameraHeight,
		Arch.CameraHeight + K.CameraHeightPerHeight * ((Spec.Clearance + Spec.Height) - (Base.Clearance + Base.Height)), MinCameraHeight, MaxCameraHeight);

	// A plane's airframe: each lifting area follows its planform against the archetype's, and the thrust follows the mass at the same thrust-to-weight,
	// shared by the engines it has. Nothing here is a vehicle document's number, so none is held to a document range. (A preset with no airframe has none.)
	if (Spec.Aircraft.bSet && Base.Aircraft.bSet)
	{
		const FMantleChassisAircraft& Plane = Spec.Aircraft;
		const FMantleChassisAircraft& Own = Base.Aircraft;
		H.WingAreaM2 = Arch.WingAreaM2 * SizeRatio(Plane.Wing.Planform(), Own.Wing.Planform());
		H.TailplaneAreaM2 = Arch.TailplaneAreaM2 * SizeRatio(Plane.Tailplane.Planform(), Own.Tailplane.Planform());
		H.FinAreaM2 = Arch.FinAreaM2 * SizeRatio(Plane.Fin.Planform(), Own.Fin.Planform());
		H.EngineThrustNewtons = Arch.EngineThrustNewtons * MassRatio * (static_cast<double>(Own.EngineCount) / static_cast<double>(FMath::Max(1, Plane.EngineCount)));
	}
	// A helicopter's rotor lifts the same share of its weight at every size (the mass as held above, so a mass at its limit lifts what it weighs): the
	// collective that holds the weight is the preset's own, and so is how hard it climbs.
	if (Spec.Helicopter.bSet && Base.Helicopter.bSet)
	{
		H.RotorThrustNewtons = Arch.RotorThrustNewtons * MassRatio;
	}
	// A hovercraft's fan pushes the same share of its weight at every size. Each pad carries its share of the mass over how far it comes down at rest, so
	// its rate follows the mass, the pads and that distance (the hull then rests at its own hover height: a pad's push is the rate times how far it has come
	// down). The distance is held to a centimetre at least, so a hover height at or past the pads' reach gives a stiff pad and
	// not a division by nothing. The rudder follows its fin's planform, as a plane's surfaces do.
	if (Spec.Hovercraft.bSet && Base.Hovercraft.bSet)
	{
		const FMantleChassisHovercraft& Own = Base.Hovercraft;
		const double Compression = FMath::Max(Spec.Hovercraft.Compression(Spec.Clearance), 1.0);
		const double BaseCompression = FMath::Max(Own.Compression(Base.Clearance), 1.0);
		H.HoverThrustNewtons = Arch.HoverThrustNewtons * MassRatio;
		H.PadSpringRate = Arch.PadSpringRate * MassRatio * (BaseWheels / Wheels) * SizeRatio(BaseCompression, Compression);
		H.HoverFinAreaM2 = Arch.HoverFinAreaM2 * SizeRatio(Spec.Hovercraft.FinHeight * Spec.Hovercraft.FinChord, Own.FinHeight * Own.FinChord);
	}

	RoundHandling(H);
	return H;
}

namespace MantleChassisDerivePrivate
{
	/**
	 * The force of one aerofoil in newtons, FAerofoilSimModule::GetForce: the lift along the surface's force axis is 2 sin((alpha + control + camber) 90 / stall)
	 * times area times q (none past twice the stall angle), the drag along the airflow 0.05 + ((alpha + control + camber) / (stall + |control + camber|))^2
	 * times the same.
	 */
	void AerofoilForce(const double Area, const double Camber, const double Control, const double Stall, const double Alpha, const double Q, double& OutLift, double& OutDrag)
	{
		const double Total = Control + Camber;
		const double Angle = Alpha + Total;
		const double Lift = FMath::Abs(Angle) > 2.0 * Stall ? 0.0 : 2.0 * FMath::Sin(FMath::DegreesToRadians(Angle * 90.0 / Stall));
		double A = Alpha;
		if (A > 90.0)
		{
			A = 180.0 - A;
		}
		if (A < -90.0)
		{
			A = -180.0 - A;
		}
		const double Ratio = (A + Total) / (Stall + FMath::Abs(Total));
		OutLift = Lift * Area * Q;
		OutDrag = (0.05 + Ratio * Ratio) * Area * Q;
	}

	/** Where the fuselage's drag pushes, chassis frame, cm (schema/validate.py PLANE_BODY_DRAG_POINT): the root body's origin, on the ground. */
	const FVector BodyDragPoint = FVector::ZeroVector;

	/** Rounds Value to a multiple of Step. */
	double RoundTo(const double Value, const double Step)
	{
		return FMath::RoundToDouble(Value / Step) * Step;
	}
}

bool FMantleChassisDerive::LevelFlight(const FMantleChassisSpec& Spec, const FMantleChassisHandling& Handling, const double SpeedMs, FMantleChassisTrim& OutTrim)
{
	using namespace MantleChassisDerivePrivate;
	if (!Spec.Aircraft.bSet || SpeedMs <= 0.0 || !FMath::IsFinite(SpeedMs) || Handling.Mass <= 0.0 || Handling.EngineThrustNewtons <= 0.0)
	{
		return false;
	}
	const FMantleChassisAircraft& Craft = Spec.Aircraft;
	const FMantleChassisSurface& Wing = Craft.Wing;
	const FMantleChassisSurface& Tail = Craft.Tailplane;
	const FMantleChassisSurface& Fin = Craft.Fin;
	const FVector& Com = Spec.CenterOfMass;
	const double Weight = Handling.Mass * Gravity;
	const double Q = 0.5 * AirDensity * SpeedMs * SpeedMs;
	const double FinZ = Spec.Clearance + Spec.Height + Fin.Span / 2.0;
	// Only the thrust line's height turns the plane, so a nose propeller and a nozzle behind the tail at the same height trim alike.
	double EngineZ = 0.0;
	for (int32 Engine = 0; Engine < Craft.EngineCount; ++Engine)
	{
		EngineZ += Spec.EngineLocation(Engine).Z;
	}
	const double ThrustArm = EngineZ / FMath::Max(1, Craft.EngineCount) - Com.Z;
	const double BodyDrag = Handling.FrontalArea * Handling.DragCoefficient * Q;

	struct FBalance
	{
		double Vertical = 0.0;
		double Moment = 0.0;
		double Throttle = 0.0;
	};
	// The net vertical force less the weight, the pitch moment (nose-up) and the throttle, at a pitch and an elevator input.
	auto Balance = [&](const double Pitch, const double Elevator)
	{
		const double Theta = FMath::DegreesToRadians(Pitch);
		const double AirflowX = -FMath::Cos(Theta);
		const double AirflowZ = FMath::Sin(Theta);
		double Fx = 0.0;
		double Fz = 0.0;
		double Moment = 0.0;
		for (int32 Which = 0; Which < 2; ++Which)
		{
			const bool bWing = Which == 0;
			const FMantleChassisSurface& Surface = bWing ? Wing : Tail;
			double Lift = 0.0;
			double Drag = 0.0;
			AerofoilForce(bWing ? Handling.WingAreaM2 : Handling.TailplaneAreaM2, Surface.Camber, bWing ? 0.0 : Elevator * Tail.MaxControlAngle, Surface.StallAngle, Pitch, Q, Lift,
				Drag);
			const double SurfaceX = Drag * AirflowX;
			const double SurfaceZ = Lift + Drag * AirflowZ;
			double Reach = Surface.X - Com.X;
			if (bWing)
			{
				const double CentreOfLift = Pitch > Wing.StallAngle ? (Pitch - Wing.StallAngle) * 10.0 + 20.0 : (Wing.StallAngle - Pitch) * 20.0 / Wing.StallAngle + 20.0;
				Reach += CentreOfLift - 50.0;
			}
			Fx += SurfaceX;
			Fz += SurfaceZ;
			Moment += Reach * SurfaceZ - (Surface.Z - Com.Z) * SurfaceX;
		}
		// The fin lifts sideways, so only its drag is in this plane; the fuselage's drag acts at BodyDragPoint.
		double FinLift = 0.0;
		double FinDrag = 0.0;
		AerofoilForce(Handling.FinAreaM2, Fin.Camber, 0.0, Fin.StallAngle, 0.0, Q, FinLift, FinDrag);
		Fx += (FinDrag + BodyDrag) * AirflowX;
		Fz += (FinDrag + BodyDrag) * AirflowZ;
		Moment += (Fin.X - Com.X) * FinDrag * AirflowZ - (FinZ - Com.Z) * FinDrag * AirflowX;
		Moment += (BodyDragPoint.X - Com.X) * BodyDrag * AirflowZ - (BodyDragPoint.Z - Com.Z) * BodyDrag * AirflowX;
		// The thrust that holds the speed (no net force along the flight path), pushing along X on its line: below the centre of mass it lifts the nose.
		const double Thrust = Fz * FMath::Tan(Theta) - Fx;
		Moment -= ThrustArm * Thrust;
		FBalance Out;
		Out.Vertical = Fz / FMath::Cos(Theta) - Weight;
		Out.Moment = Moment;
		Out.Throttle = Thrust / (Craft.EngineCount * Handling.EngineThrustNewtons);
		return Out;
	};
	// The elevator that zeroes the moment at a pitch, bisected between full down and full up (held at either end when the moment does not change sign).
	auto ElevatorFor = [&](const double Pitch)
	{
		double Low = -1.0;
		double High = 1.0;
		if (Balance(Pitch, Low).Moment < 0.0)
		{
			return Low;
		}
		if (Balance(Pitch, High).Moment > 0.0)
		{
			return High;
		}
		for (int32 Step = 0; Step < 60; ++Step)
		{
			const double Middle = 0.5 * (Low + High);
			if (Balance(Pitch, Middle).Moment > 0.0)
			{
				Low = Middle;
			}
			else
			{
				High = Middle;
			}
		}
		return 0.5 * (Low + High);
	};

	// The pitch where the lift matches the weight, with the elevator that balances it, bisected from -5 to 14 degrees.
	double Low = -5.0;
	double High = 14.0;
	if (Balance(Low, ElevatorFor(Low)).Vertical > 0.0 || Balance(High, ElevatorFor(High)).Vertical < 0.0)
	{
		return false;
	}
	for (int32 Step = 0; Step < 60; ++Step)
	{
		const double Middle = 0.5 * (Low + High);
		if (Balance(Middle, ElevatorFor(Middle)).Vertical < 0.0)
		{
			Low = Middle;
		}
		else
		{
			High = Middle;
		}
	}
	const double Pitch = 0.5 * (Low + High);
	const double Elevator = ElevatorFor(Pitch);
	const FBalance Held = Balance(Pitch, Elevator);
	if (FMath::Abs(Elevator) >= 1.0 || FMath::Abs(Held.Moment) > 1.0e-6 * Weight * 100.0 || !FMath::IsFinite(Held.Throttle))
	{
		return false;
	}
	OutTrim.PitchDegrees = Pitch;
	OutTrim.Elevator = Elevator;
	OutTrim.Throttle = Held.Throttle;
	return true;
}

bool FMantleChassisDerive::SolveTrim(const FMantleChassisSpec& Spec, const FMantleChassisHandling& Handling, double& OutCamber, double& OutCruiseThrottle)
{
	using namespace MantleChassisDerivePrivate;
	FMantleChassisTrim Held;
	if (!Spec.Aircraft.bSet || Spec.Aircraft.CruiseSpeedCmPerSec <= 0.0 || !LevelFlight(Spec, Handling, Spec.Aircraft.CruiseSpeedCmPerSec / 100.0, Held))
	{
		return false;
	}
	// Only the sum of the camber and the control angle acts on the air (AerofoilForce), so the camber that centres the elevator is the camber now plus the
	// control angle the plane needs. The throttle is the same either way.
	const double Camber = Spec.Aircraft.Tailplane.Camber + Held.Elevator * Spec.Aircraft.Tailplane.MaxControlAngle;
	const double Throttle = Held.Throttle;
	if (!FMath::IsFinite(Camber) || !FMath::IsFinite(Throttle) || Throttle <= 0.0 || Throttle > 1.0)
	{
		return false;
	}
	OutCamber = RoundTo(Camber, 0.001);
	OutCruiseThrottle = RoundTo(Throttle, 0.0001);
	return true;
}
