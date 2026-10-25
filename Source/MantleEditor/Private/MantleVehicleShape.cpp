// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleShape.h"

#include "MantleChassisDerive.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleChassisDocument.h"

#define LOCTEXT_NAMESPACE "MantleVehicleShape"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleShapePrivate
{
	/** Two sizes this close (cm) are the same size. */
	constexpr double SameSizeCm = 0.01;

	/** A size held to its range only says so when it moved by more than this (cm), so typing 340.04 is not "held". */
	constexpr double HeldNoteCm = 0.05;

	/** The least gap between neighbouring axles, as a share of the wheelbase (1 for two axles). */
	double MinAxleShare(const FMantleChassisSpec& Archetype)
	{
		double Least = 1.0;
		for (int32 Axle = 1; Axle < Archetype.Axles.Num(); ++Axle)
		{
			Least = FMath::Min(Least, Archetype.Axles[Axle].Position - Archetype.Axles[Axle - 1].Position);
		}
		return FMath::Max(Least, 0.05);
	}

	/** The wheelbase a turn is measured from: the steered axles to the middle of the others, as FMantleChassisDerive takes it. */
	double EffectiveWheelbase(const FMantleChassisSpec& Spec)
	{
		double Steered = 0.0;
		double Fixed = 0.0;
		int32 SteeredCount = 0;
		int32 FixedCount = 0;
		for (const FMantleChassisAxle& Axle : Spec.Axles)
		{
			(Axle.bSteer ? Steered : Fixed) += Axle.Position;
			++(Axle.bSteer ? SteeredCount : FixedCount);
		}
		double Effective = Spec.Wheelbase;
		if (SteeredCount > 0 && FixedCount > 0)
		{
			Effective = Spec.Wheelbase * (Fixed / FixedCount - Steered / SteeredCount);
		}
		return Effective > 0.0 ? Effective : Spec.Wheelbase;
	}

	/** A weight in a creator's words: "1.6 tonnes", "1 tonne", or whole kilograms (to the nearest ten from 100). */
	FText WeightText(const double MassKg)
	{
		if (MassKg >= 1000.0)
		{
			const double Tonnes = FMath::RoundToDouble(MassKg / 100.0) / 10.0;
			return FMath::IsNearlyEqual(Tonnes, 1.0) ? LOCTEXT("WeightOneTonne", "1 tonne") : FText::Format(LOCTEXT("WeightTonnes", "{0} tonnes"), FText::AsNumber(Tonnes));
		}
		// Whole kilograms under 100, else the nearest ten.
		const double Step = MassKg < 100.0 ? 1.0 : 10.0;
		return FText::Format(LOCTEXT("WeightKilos", "{0} kg"), FText::AsNumber(FMath::RoundToDouble(MassKg / Step) * Step));
	}

	/** A speed in km/h, or a load in kg per square metre, to the nearest five, at least five: more digits than that claim a precision the numbers do not have. */
	int32 NearestFive(const double Value)
	{
		return FMath::Max(5, FMath::RoundToInt(Value / 5.0) * 5);
	}

	/** Two lists of notes say the same thing. */
	bool SameNotes(const TArray<FText>& A, const TArray<FText>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (!A[Index].EqualTo(B[Index]))
			{
				return false;
			}
		}
		return true;
	}

	/** A Size held to its range only says so when it moved by more than this, so typing 1.2501 is not "held". */
	constexpr double HeldSizeNote = 0.0005;

	/** Two sizes this close are the same size. */
	constexpr double SameSize = 1.0e-6;

	/** Every handling line (FMantleChassisHandling::Clamped) of a family that matters to it: a plane's or a helicopter's wheels, engine and gearing are inert, so only its weight counts. */
	bool ClampMatters(const EMantleVehicleFamily Family, const FString& Path)
	{
		switch (Family)
		{
		case EMantleVehicleFamily::Aircraft:
		case EMantleVehicleFamily::Helicopter:
			return Path == TEXT("mass");
		case EMantleVehicleFamily::Hovercraft:
			return Path == TEXT("mass") || Path == TEXT("maxSpeed");
		default:
			return true;
		}
	}

	const FMantleChassisHandling& HandlingOf(const FMantleChassisSpec& Spec)
	{
		return Spec.Derived.IsSet() ? Spec.Derived.GetValue() : Spec.Archetype;
	}

	/** The note for a size that was held: which size, the value it was held at, and why. */
	FText HeldNote(const EMantleVehicleDimension Dimension, const bool bLow, const bool bAtSliderEnd, const double Held)
	{
		const FText Value = FMantleVehicleShape::LengthText(Held);
		if (!bAtSliderEnd)
		{
			// Held by another size: the wheels would touch.
			switch (Dimension)
			{
			case EMantleVehicleDimension::Wheelbase:
				return FText::Format(LOCTEXT("HeldWheelbaseTouch", "The wheelbase is held at {0}: any shorter and the wheels would touch."), Value);
			case EMantleVehicleDimension::WheelRadius:
				return FText::Format(LOCTEXT("HeldRadiusTouch", "The wheel radius is held at {0}: any bigger and the wheels would touch."), Value);
			case EMantleVehicleDimension::Track:
				return FText::Format(LOCTEXT("HeldTrackTouch", "The track is held at {0}: any narrower and the left and right wheels would touch."), Value);
			case EMantleVehicleDimension::WheelWidth:
				return FText::Format(LOCTEXT("HeldWidthTouch", "The wheel width is held at {0}: any wider and the left and right wheels would touch."), Value);
			default:
				break;
			}
		}
		return bLow
			? FText::Format(LOCTEXT("HeldLow", "{0} can't go below {1}, so it is held there."), FMantleVehicleShape::Label(Dimension), Value)
			: FText::Format(LOCTEXT("HeldHigh", "{0} can't go above {1}, so it is held there."), FMantleVehicleShape::Label(Dimension), Value);
	}
}

double FMantleVehicleDimensions::Get(const EMantleVehicleDimension Dimension) const
{
	switch (Dimension)
	{
	case EMantleVehicleDimension::Wheelbase: return Wheelbase;
	case EMantleVehicleDimension::Track: return Track;
	case EMantleVehicleDimension::WheelRadius: return WheelRadius;
	case EMantleVehicleDimension::WheelWidth: return WheelWidth;
	case EMantleVehicleDimension::BodyLength: return BodyLength;
	case EMantleVehicleDimension::BodyWidth: return BodyWidth;
	case EMantleVehicleDimension::BodyHeight: return BodyHeight;
	case EMantleVehicleDimension::GroundClearance: return GroundClearance;
	}
	return 0.0;
}

void FMantleVehicleDimensions::Set(const EMantleVehicleDimension Dimension, const double Value)
{
	switch (Dimension)
	{
	case EMantleVehicleDimension::Wheelbase: Wheelbase = Value; break;
	case EMantleVehicleDimension::Track: Track = Value; break;
	case EMantleVehicleDimension::WheelRadius: WheelRadius = Value; break;
	case EMantleVehicleDimension::WheelWidth: WheelWidth = Value; break;
	case EMantleVehicleDimension::BodyLength: BodyLength = Value; break;
	case EMantleVehicleDimension::BodyWidth: BodyWidth = Value; break;
	case EMantleVehicleDimension::BodyHeight: BodyHeight = Value; break;
	case EMantleVehicleDimension::GroundClearance: GroundClearance = Value; break;
	}
}

bool FMantleVehicleDimensions::Equals(const FMantleVehicleDimensions& Other, const double Tolerance) const
{
	for (const EMantleVehicleDimension Dimension : MantleVehicleDimensions)
	{
		if (FMath::Abs(Get(Dimension) - Other.Get(Dimension)) > Tolerance)
		{
			return false;
		}
	}
	return true;
}

FString FMantleVehicleDimensions::Signature() const
{
	return FString::Printf(TEXT("%.3f|%.3f|%.3f|%.3f|%.3f|%.3f|%.3f|%.3f"), Wheelbase, Track, WheelRadius, WheelWidth, BodyLength, BodyWidth, BodyHeight, GroundClearance);
}

FMantleVehicleDimensions FMantleVehicleShape::DimensionsOf(const FMantleChassisSpec& Spec)
{
	FMantleVehicleDimensions Out;
	Out.Wheelbase = Spec.Wheelbase;
	Out.Track = Spec.Track;
	Out.WheelRadius = Spec.WheelRadius;
	Out.WheelWidth = Spec.WheelWidth;
	Out.BodyLength = Spec.Length;
	Out.BodyWidth = Spec.Width;
	Out.BodyHeight = Spec.Height;
	Out.GroundClearance = Spec.Clearance;
	return Out;
}

FMantleVehicleDimensions FMantleVehicleShape::ArchetypeDimensionsOf(const FMantleChassisSpec& Spec)
{
	const FMantleChassisSize& Own = Spec.ArchetypeSize;
	if (!(Own.Wheelbase > 0.0))
	{
		// A spec that was not read from a preset has no archetype size of its own: its sizes are its own.
		return DimensionsOf(Spec);
	}
	FMantleVehicleDimensions Out;
	Out.Wheelbase = Own.Wheelbase;
	Out.Track = Own.Track;
	Out.WheelRadius = Own.WheelRadius;
	Out.WheelWidth = Own.WheelWidth;
	Out.BodyLength = Own.Length;
	Out.BodyWidth = Own.Width;
	Out.BodyHeight = Own.Height;
	Out.GroundClearance = Own.Clearance;
	return Out;
}

EMantleVehicleFamily FMantleVehicleShape::FamilyOf(const FMantleChassisSpec& Spec)
{
	switch (Spec.Type)
	{
	case EMantleChassisType::Plane: return EMantleVehicleFamily::Aircraft;
	case EMantleChassisType::Helicopter: return EMantleVehicleFamily::Helicopter;
	case EMantleChassisType::Hovercraft: return EMantleVehicleFamily::Hovercraft;
	default: return EMantleVehicleFamily::Ground;
	}
}

bool FMantleVehicleShape::WheelsHidden(const FMantleChassisSpec& Spec)
{
	return Spec.Type == EMantleChassisType::Helicopter || Spec.Type == EMantleChassisType::Hovercraft;
}

double FMantleVehicleShape::TakeOffSpeedKmh(const FMantleChassisSpec& Spec)
{
	const FMantleChassisHandling& Handling = MantleVehicleShapePrivate::HandlingOf(Spec);
	const double AreaM2 = Handling.WingAreaM2 > 0.0 ? Handling.WingAreaM2 : Spec.Aircraft.Wing.AreaM2;
	if (Spec.Type != EMantleChassisType::Plane || !Spec.Aircraft.bSet || !(Handling.Mass > 0.0) || !(AreaM2 > 0.0))
	{
		return 0.0;
	}
	// The aerofoil's lift coefficient is 2 sin(angle x 90 / stall angle) up to the stall angle, where it peaks at 2 (FMantleVehicleAerofoil).
	const double StallAngle = FMath::Max(Spec.Aircraft.Wing.StallAngle, 1.0);
	const double Angle = FMath::Clamp((TakeOffPitchDegrees + Spec.Aircraft.Wing.Camber) * 90.0 / StallAngle, 0.0, 90.0);
	const double Coefficient = 2.0 * FMath::Sin(FMath::DegreesToRadians(Angle));
	if (!(Coefficient > 0.05))
	{
		return 0.0;
	}
	const double MetresPerSecond = FMath::Sqrt(2.0 * Handling.Mass * Gravity / (AirDensity * AreaM2 * Coefficient));
	return MetresPerSecond * 3.6;
}

double FMantleVehicleShape::HoverTopSpeedKmh(const FMantleChassisSpec& Spec)
{
	if (Spec.Type != EMantleChassisType::Hovercraft || !Spec.Hovercraft.bSet)
	{
		return 0.0;
	}
	const FMantleChassisHandling& Handling = MantleVehicleShapePrivate::HandlingOf(Spec);
	const double Thrust = Handling.HoverThrustNewtons > 0.0 ? Handling.HoverThrustNewtons : Spec.Hovercraft.FanThrustNewtons;
	// The fan's push falls off linearly to nothing at the speed limit (cm/s to m/s).
	const double Fade = Handling.MaxSpeed / 100.0;
	if (!(Thrust > 0.0) || !(Fade > 0.0))
	{
		return Handling.MaxSpeed * 0.036;
	}
	const double Drag = 0.5 * AirDensity * Handling.DragCoefficient * Handling.FrontalArea;
	// Thrust x (1 - v / Fade) = Drag v^2 + Damping x mass x v, solved for v.
	const double Linear = Handling.Mass * HoverLinearDamping + Thrust / Fade;
	const double MetresPerSecond = Drag > 0.0 ? (-Linear + FMath::Sqrt(Linear * Linear + 4.0 * Drag * Thrust)) / (2.0 * Drag) : Thrust / Linear;
	return FMath::Min(MetresPerSecond, Fade) * 3.6;
}

bool FMantleVehicleShape::IsEditable(const FMantleChassisSpec& Archetype, const EMantleVehicleDimension Dimension)
{
	if (Dimension == EMantleVehicleDimension::Track && Archetype.bCentreline)
	{
		return false;
	}
	// A helicopter's skids and a hovercraft's pads are never drawn: their wheel size is not the creator's to change.
	return !(WheelsHidden(Archetype) && (Dimension == EMantleVehicleDimension::WheelRadius || Dimension == EMantleVehicleDimension::WheelWidth));
}

FMantleDimensionRange FMantleVehicleShape::SliderRange(const FMantleChassisSpec& Archetype, const EMantleVehicleDimension Dimension)
{
	const double Base = ArchetypeDimensionsOf(Archetype).Get(Dimension);
	FMantleDimensionRange Range;
	Range.Min = Base > 0.0 ? Base * FMantleChassisSpec::MinScale : 0.0;
	Range.Max = Base > 0.0 ? Base * FMantleChassisSpec::MaxScale : 30.0;

	// The most and least each size may be, whatever the preset: a wheel's size and a body box's half size come from the document's own limits
	// (limits.json placeAssets.vehicle.ranges); the wheelbase, track and ground clearance are this window's own, provisional until the size
	// envelope is measured (#793 section 4).
	double Low = 1.0;
	double High = 2000.0;
	double LimitMin = 0.0;
	double LimitMax = 0.0;
	switch (Dimension)
	{
	case EMantleVehicleDimension::Wheelbase:
		High = FMantleVehicleShape::MaxWheelbase;
		break;
	case EMantleVehicleDimension::Track:
		Low = 20.0;
		High = 800.0;
		break;
	case EMantleVehicleDimension::WheelRadius:
		if (FMantleVehicleLimits::GetRange(TEXT("wheels/*/radius"), LimitMin, LimitMax))
		{
			Low = LimitMin;
			High = LimitMax;
		}
		break;
	case EMantleVehicleDimension::WheelWidth:
		if (FMantleVehicleLimits::GetRange(TEXT("wheels/*/width"), LimitMin, LimitMax))
		{
			Low = LimitMin;
			High = LimitMax;
		}
		break;
	case EMantleVehicleDimension::BodyLength:
	case EMantleVehicleDimension::BodyWidth:
	case EMantleVehicleDimension::BodyHeight:
		// The collision box is the body: its half size is held to the document's range.
		if (FMantleVehicleLimits::GetRange(TEXT("collision/*/extent/*"), LimitMin, LimitMax))
		{
			Low = 2.0 * LimitMin;
			High = 2.0 * LimitMax;
		}
		break;
	case EMantleVehicleDimension::GroundClearance:
		// The box's centre (clearance and half the height) is held to the document's range too.
		Low = 0.0;
		High = 500.0;
		break;
	}
	Range.Min = FMath::Max(Range.Min, Low);
	Range.Max = FMath::Min(Range.Max, High);
	if (Range.Max < Range.Min)
	{
		Range.Max = Range.Min;
	}
	return Range;
}

FMantleDimensionRange FMantleVehicleShape::HeldRange(const FMantleChassisSpec& Archetype, const FMantleVehicleDimensions& Dimensions, const EMantleVehicleDimension Dimension)
{
	FMantleDimensionRange Range = SliderRange(Archetype, Dimension);
	const double Share = MantleVehicleShapePrivate::MinAxleShare(Archetype);
	switch (Dimension)
	{
	case EMantleVehicleDimension::Wheelbase:
		Range.Min = FMath::Max(Range.Min, MinAxleGapPerRadius * Dimensions.WheelRadius / Share);
		break;
	case EMantleVehicleDimension::WheelRadius:
		Range.Max = FMath::Min(Range.Max, Dimensions.Wheelbase * Share / MinAxleGapPerRadius);
		break;
	case EMantleVehicleDimension::Track:
		if (!Archetype.bCentreline)
		{
			Range.Min = FMath::Max(Range.Min, MinTrackPerWidth * Dimensions.WheelWidth);
		}
		break;
	case EMantleVehicleDimension::WheelWidth:
		if (!Archetype.bCentreline)
		{
			Range.Max = FMath::Min(Range.Max, Dimensions.Track / MinTrackPerWidth);
		}
		break;
	default:
		break;
	}
	if (Range.Max < Range.Min)
	{
		Range.Max = Range.Min;
	}
	return Range;
}

FMantleVehicleDimensions FMantleVehicleShape::Edit(const FMantleChassisSpec& Archetype, const FMantleVehicleDimensions& Dimensions,
	const EMantleVehicleDimension Dimension, const double Wanted, TArray<FText>& OutNotes)
{
	using namespace MantleVehicleShapePrivate;
	FMantleVehicleDimensions Out = Dimensions;
	if (!FMath::IsFinite(Wanted) || !IsEditable(Archetype, Dimension))
	{
		return Out;
	}
	const FMantleDimensionRange Range = HeldRange(Archetype, Dimensions, Dimension);
	const double Held = Range.Clamp(Wanted);
	Out.Set(Dimension, Held);
	if (FMath::Abs(Held - Wanted) > HeldNoteCm)
	{
		const FMantleDimensionRange Slider = SliderRange(Archetype, Dimension);
		const bool bLow = Wanted < Range.Min;
		const bool bAtSliderEnd = FMath::IsNearlyEqual(bLow ? Range.Min : Range.Max, bLow ? Slider.Min : Slider.Max, SameSizeCm);
		OutNotes.Add(HeldNote(Dimension, bLow, bAtSliderEnd, Held));
	}
	return Out;
}

bool FMantleVehicleShape::IsShaped(const FMantleChassisSpec& Archetype, const FMantleVehicleDimensions& Dimensions)
{
	const FMantleVehicleDimensions Own = ArchetypeDimensionsOf(Archetype);
	for (const EMantleVehicleDimension Dimension : MantleVehicleDimensions)
	{
		if (IsEditable(Archetype, Dimension) && FMath::Abs(Own.Get(Dimension) - Dimensions.Get(Dimension)) > MantleVehicleShapePrivate::SameSizeCm)
		{
			return true;
		}
	}
	return false;
}

FMantleChassisSpec FMantleVehicleShape::Reshape(const FMantleChassisSpec& Archetype, const FMantleVehicleDimensions& Dimensions)
{
	if (!IsShaped(Archetype, Dimensions))
	{
		return Archetype;
	}
	FMantleChassisSpec Out = Archetype;
	Out.Wheelbase = Dimensions.Wheelbase;
	Out.WheelRadius = Dimensions.WheelRadius;
	Out.WheelWidth = Dimensions.WheelWidth;
	Out.Length = Dimensions.BodyLength;
	Out.Width = Dimensions.BodyWidth;
	Out.Height = Dimensions.BodyHeight;
	Out.Clearance = Dimensions.GroundClearance;
	if (!Archetype.bCentreline)
	{
		Out.Track = Dimensions.Track;
	}

	// The axles keep their shares of the wheelbase. The seat goes where the body went: along with the wheelbase (the cabin sits between
	// the axles), across with the body's width, and up with the ground clearance (a driver is the same size in any vehicle, so a taller body
	// is more headroom, not a higher seat). The steering wheel stays the same reach from the seat, and a rider's grips and pegs move with it.
	const double Along = Archetype.Wheelbase > 0.0 ? Dimensions.Wheelbase / Archetype.Wheelbase : 1.0;
	const double Across = Archetype.Width > 0.0 ? Dimensions.BodyWidth / Archetype.Width : 1.0;
	Out.Seat = FVector(Archetype.Seat.X * Along, Archetype.Seat.Y * Across, Archetype.Seat.Z + (Dimensions.GroundClearance - Archetype.Clearance));
	if (Archetype.bSteeringWheel)
	{
		Out.Hub = Out.Seat + (Archetype.Hub - Archetype.Seat);
	}
	const FVector SeatMoved = Out.Seat - Archetype.Seat;
	const FVector RiderShift(SeatMoved.X, 0.0, SeatMoved.Z);
	Out.Rider.LeftGrip += RiderShift;
	Out.Rider.RightGrip += RiderShift;
	Out.Rider.LeftPeg += RiderShift;
	Out.Rider.RightPeg += RiderShift;

	// The handling follows the sizes, and the centre of mass is the derived one, as for a uniform resize (FMantleChassisSpec::Scaled). The
	// rider's rear brake and tyre keep their share of the front's, whatever the derivation made of the front's.
	const FMantleChassisHandling Before = Archetype.Derived.IsSet() ? Archetype.Derived.GetValue() : Archetype.Archetype;
	const FMantleChassisHandling After = FMantleChassisDerive::Derive(Out);
	if (Before.BrakeTorque > 0.0)
	{
		Out.Rider.RearBrakeTorque *= After.BrakeTorque / Before.BrakeTorque;
	}
	if (Before.TireFriction > 0.0)
	{
		Out.Rider.RearTireFriction *= After.TireFriction / Before.TireFriction;
	}
	Out.CenterOfMass = After.CenterOfMass;
	Out.Derived = After;
	return Out;
}

double FMantleVehicleShape::TurningCircleCm(const FMantleChassisSpec& Spec, const FMantleChassisHandling& Handling)
{
	// The least lock that still turns: a degree, so a vehicle that cannot steer reads as a very wide circle, not as a division by nothing.
	const double LockDegrees = FMath::Max(Handling.MaxSteerAngle, 1.0);
	return 2.0 * MantleVehicleShapePrivate::EffectiveWheelbase(Spec) / FMath::Tan(FMath::DegreesToRadians(LockDegrees));
}

FMantleVehicleSummary FMantleVehicleShape::Summarise(const FMantleChassisSpec& Spec)
{
	const FMantleChassisHandling& Handling = MantleVehicleShapePrivate::HandlingOf(Spec);
	FMantleVehicleSummary Out;
	Out.Family = FamilyOf(Spec);
	Out.MassKg = Handling.Mass;
	switch (Out.Family)
	{
	case EMantleVehicleFamily::Aircraft:
	{
		// The wing's area follows the Size (the handling's, once resized), and the speed the air allows is the preset's own: speeds do not scale. A plane's
		// own engine and wheel numbers are inert, so its ground top speed is not what it is said to reach.
		const double AreaM2 = Handling.WingAreaM2 > 0.0 ? Handling.WingAreaM2 : Spec.Aircraft.Wing.AreaM2;
		Out.WingLoadingKgM2 = AreaM2 > 0.0 ? Handling.Mass / AreaM2 : 0.0;
		Out.TakeOffKmh = TakeOffSpeedKmh(Spec);
		// cm/s to km/h.
		Out.TopSpeedKmh = Spec.Aircraft.AirSpeedLimitCmPerSec * 0.036;
		break;
	}
	case EMantleVehicleFamily::Helicopter:
	{
		// The rotor lifts so much at full collective; the share of it that holds the weight is the throttle that hovers.
		const double ThrustNewtons = Handling.RotorThrustNewtons > 0.0 ? Handling.RotorThrustNewtons : Spec.Helicopter.RotorThrustNewtons;
		Out.HoverThrottle = ThrustNewtons > 0.0 ? Handling.Mass * Gravity / ThrustNewtons : 0.0;
		break;
	}
	case EMantleVehicleFamily::Hovercraft:
		// The body's ground clearance is the hover height.
		Out.HoverHeightCm = Spec.Clearance;
		Out.TopSpeedKmh = HoverTopSpeedKmh(Spec);
		break;
	default:
		Out.TurningCircleCm = TurningCircleCm(Spec, Handling);
		// cm/s to km/h.
		Out.TopSpeedKmh = Handling.MaxSpeed * 0.036;
		break;
	}
	return Out;
}

FText FMantleVehicleShape::Describe(const FMantleVehicleSummary& Summary)
{
	using namespace MantleVehicleShapePrivate;
	const FText Weight = WeightText(Summary.MassKg);
	const FText Speed = FText::AsNumber(NearestFive(Summary.TopSpeedKmh));
	switch (Summary.Family)
	{
	case EMantleVehicleFamily::Aircraft:
	{
		// A wing too small to lift the plane at the pitch its tail allows never leaves the ground, which is worth saying instead of a speed.
		const FText TakeOff = Summary.TakeOffKmh > 0.0
			? FText::Format(LOCTEXT("SummaryTakeOff", "Takes off at about {0} km/h."), FText::AsNumber(NearestFive(Summary.TakeOffKmh)))
			: LOCTEXT("SummaryNoTakeOff", "Its wing is too small to lift it off the ground.");
		return FText::Format(LOCTEXT("SummaryAircraft", "Weighs about {0}. Carries about {1} kg on each square metre of wing. {2} Top speed about {3} km/h."),
			Weight, FText::AsNumber(NearestFive(Summary.WingLoadingKgM2)), TakeOff, Speed);
	}
	case EMantleVehicleFamily::Helicopter:
	{
		if (!(Summary.HoverThrottle > 0.0))
		{
			return FText::Format(LOCTEXT("SummaryHelicopterBare", "Weighs about {0}."), Weight);
		}
		if (Summary.HoverThrottle >= 1.0)
		{
			return FText::Format(LOCTEXT("SummaryHelicopterHeavy", "Weighs about {0}. Its rotor is too weak to lift it."), Weight);
		}
		return FText::Format(LOCTEXT("SummaryHelicopter", "Weighs about {0}. Hovers at about {1} percent throttle."), Weight,
			FText::AsNumber(FMath::Clamp(FMath::RoundToInt(Summary.HoverThrottle * 100.0), 1, 99)));
	}
	case EMantleVehicleFamily::Hovercraft:
		return FText::Format(LOCTEXT("SummaryHovercraft", "Weighs about {0}. Floats about {1} above the ground. Top speed about {2} km/h."), Weight,
			LengthText(Summary.HoverHeightCm), Speed);
	default:
		break;
	}
	const int32 Turn = FMath::Max(1, FMath::RoundToInt(Summary.TurningCircleCm / 100.0));
	return FText::Format(LOCTEXT("Summary", "Weighs about {0}. Turns in a circle about {1} m across. Top speed about {2} km/h."), Weight, FText::AsNumber(Turn), Speed);
}

TArray<FText> FMantleVehicleShape::Warnings(const FMantleChassisSpec& Spec)
{
	using namespace MantleVehicleShapePrivate;
	TArray<FText> Out;

	// Wheels beyond the body: the front and rear axles sit half a wheelbase either side of the middle, the body half its length. (A
	// two-wheeler's body box is only its engine and tank, a plane's main wheels stand under its wing, a helicopter's skids either side of its
	// cabin, and a hovercraft's pads stand at the layout's hidden wheel points, a share of its hull, so their wheels are meant to stand out of it.)
	const bool bCheckWheelsAgainstBody = !Spec.bCentreline && Spec.Type != EMantleChassisType::Plane && Spec.Type != EMantleChassisType::Helicopter
		&& Spec.Type != EMantleChassisType::Hovercraft;
	if (bCheckWheelsAgainstBody && 0.5 * Spec.Wheelbase + Spec.WheelRadius > 0.5 * Spec.Length + WheelsOutsideBody)
	{
		Out.Add(LOCTEXT("WheelsPastEnds", "The wheels stick out past the front and back of the body. Make the body longer, or the wheelbase shorter."));
	}
	if (bCheckWheelsAgainstBody && 0.5 * Spec.Track + 0.5 * Spec.WheelWidth > 0.5 * Spec.Width + WheelsOutsideBody)
	{
		Out.Add(LOCTEXT("WheelsPastSides", "The wheels stick out past the sides of the body. Make the body wider, or the track narrower."));
	}

	// A body whose weight sits too high for its track tips over in a turn. The derivation lowers the centre of mass as far as the wheels allow, and
	// rounds it to six digits, so a track it just meets reads a hair under the target: the check allows a ten-thousandth.
	const double Stability = Spec.Constants.Stability;
	const double CentreHeight = Spec.CenterOfMass.Z;
	if (!Spec.bCentreline && Stability > 0.0 && CentreHeight > 0.0 && Spec.Track / (2.0 * CentreHeight) < Stability * (1.0 - 1.0e-4))
	{
		Out.Add(LOCTEXT("TopHeavy", "This is top-heavy and may tip over in turns. Make the wheels smaller, or the track wider."));
	}

	// Every handling number the derivation had to hold at a limit (FMantleChassisHandling::Clamped): the vehicle will not drive quite as its size
	// suggests. A line reads "<document path> <the rule's answer> held to <the limit>"; which way it was held says too heavy or too light.
	for (const FString& Line : HandlingOf(Spec).Clamped)
	{
		TArray<FString> Parts;
		Line.ParseIntoArray(Parts, TEXT(" "));
		if (Parts.Num() != 5 || Parts[2] != TEXT("held") || Parts[3] != TEXT("to") || !ClampMatters(FamilyOf(Spec), Parts[0]))
		{
			continue;
		}
		const FText Said = ClampedSentence(Parts[0], FCString::Atod(*Parts[1]) > FCString::Atod(*Parts[4]));
		if (!Said.IsEmpty() && !Out.ContainsByPredicate([&Said](const FText& Have) { return Have.EqualTo(Said); }))
		{
			Out.Add(Said);
		}
	}

	// Every number the vehicle document holds to its limits when this is published (FMantleVehicleChassisDocument::HeldNumbers): the ones the handling list above does not
	// carry (a wing's area, a rotor's thrust, a tyre's grip), so the step says every number that is not what the size suggests.
	for (const FString& Line : FMantleVehicleChassisDocument::HeldNumbers(Spec))
	{
		const FText Said = FText::FromString(Line);
		if (!Out.ContainsByPredicate([&Said](const FText& Have) { return Have.EqualTo(Said); }))
		{
			Out.Add(Said);
		}
	}
	return Out;
}

FText FMantleVehicleShape::ClampedSentence(const FString& Path, const bool bHeldDown)
{
	if (Path == TEXT("mass"))
	{
		return bHeldDown ? LOCTEXT("MassMost", "It is as heavy as Mantle allows, so it may feel lighter than its size suggests.")
			: LOCTEXT("MassLeast", "It is as light as Mantle allows, so it may feel heavier than its size suggests.");
	}
	if (Path == TEXT("engine/maxTorque"))
	{
		return bHeldDown ? LOCTEXT("EngineMost", "Its engine is as strong as Mantle allows, so it may feel less powerful than its weight suggests.")
			: LOCTEXT("EngineLeast", "Its engine is as weak as Mantle allows, so it may feel slow to pull away.");
	}
	if (Path == TEXT("transmission/finalDrive"))
	{
		return LOCTEXT("Gears", "Its gearing is at the edge of what Mantle allows, so it may not reach the top speed its size suggests.");
	}
	if (Path == TEXT("suspension/springRate") || Path == TEXT("suspension/springPreload"))
	{
		return bHeldDown ? LOCTEXT("SpringsMost", "Its springs can't be made stiff enough for this weight, so it may sag and bounce.")
			: LOCTEXT("SpringsLeast", "Its springs can't be made soft enough for this light a body, so it may ride hard.");
	}
	if (Path == TEXT("suspension/maxRaise") || Path == TEXT("suspension/maxDrop"))
	{
		return bHeldDown ? LOCTEXT("TravelMost", "Its suspension can't have as much give as these wheels want, so it may bottom out on bumps.")
			: LOCTEXT("TravelLeast", "Its suspension has more give than these small wheels want, so it may feel floaty.");
	}
	if (Path == TEXT("steering/maxAngle"))
	{
		return bHeldDown ? LOCTEXT("LockMost", "Its steering turns as far as Mantle allows, so it may turn in a wider circle than its size suggests.")
			: LOCTEXT("LockLeast", "Its steering turns as little as Mantle allows, so it may turn in a tighter circle than its size suggests.");
	}
	if (Path == TEXT("brakes/brakeTorque") || Path == TEXT("brakes/handbrakeTorque"))
	{
		return bHeldDown ? LOCTEXT("BrakesMost", "Its brakes are as strong as Mantle allows, so it may take longer to stop than its weight suggests.")
			: LOCTEXT("BrakesLeast", "Its brakes are as weak as Mantle allows, so it may stop quicker than its weight suggests.");
	}
	if (Path == TEXT("maxSpeed"))
	{
		return bHeldDown ? LOCTEXT("SpeedMost", "Its top speed is as high as Mantle allows.") : LOCTEXT("SpeedLeast", "Its top speed is as low as Mantle allows.");
	}
	return FText::GetEmpty();
}

FText FMantleVehicleShape::Label(const EMantleVehicleDimension Dimension)
{
	switch (Dimension)
	{
	case EMantleVehicleDimension::Wheelbase: return LOCTEXT("LabelWheelbase", "Wheelbase");
	case EMantleVehicleDimension::Track: return LOCTEXT("LabelTrack", "Track");
	case EMantleVehicleDimension::WheelRadius: return LOCTEXT("LabelWheelRadius", "Wheel radius");
	case EMantleVehicleDimension::WheelWidth: return LOCTEXT("LabelWheelWidth", "Wheel width");
	case EMantleVehicleDimension::BodyLength: return LOCTEXT("LabelBodyLength", "Body length");
	case EMantleVehicleDimension::BodyWidth: return LOCTEXT("LabelBodyWidth", "Body width");
	case EMantleVehicleDimension::BodyHeight: return LOCTEXT("LabelBodyHeight", "Body height");
	case EMantleVehicleDimension::GroundClearance: return LOCTEXT("LabelClearance", "Ground clearance");
	}
	return FText::GetEmpty();
}

FText FMantleVehicleShape::Help(const EMantleVehicleDimension Dimension)
{
	switch (Dimension)
	{
	case EMantleVehicleDimension::Wheelbase: return LOCTEXT("HelpWheelbase", "How far the front wheels are from the back wheels. A longer vehicle turns in a wider circle.");
	case EMantleVehicleDimension::Track: return LOCTEXT("HelpTrack", "How far apart the left and right wheels are, measured from the middle of one to the middle of the other.");
	case EMantleVehicleDimension::WheelRadius: return LOCTEXT("HelpWheelRadius", "How big the wheels are, measured from the middle of a wheel to its edge.");
	case EMantleVehicleDimension::WheelWidth: return LOCTEXT("HelpWheelWidth", "How wide each wheel is.");
	case EMantleVehicleDimension::BodyLength: return LOCTEXT("HelpBodyLength", "How long the body is, front to back. The body is what players bump into.");
	case EMantleVehicleDimension::BodyWidth: return LOCTEXT("HelpBodyWidth", "How wide the body is, side to side.");
	case EMantleVehicleDimension::BodyHeight: return LOCTEXT("HelpBodyHeight", "How tall the body is.");
	case EMantleVehicleDimension::GroundClearance: return LOCTEXT("HelpClearance", "The gap between the ground and the underside of the body.");
	}
	return FText::GetEmpty();
}

FText FMantleVehicleShape::SizeLabel()
{
	return LOCTEXT("LabelSize", "Size");
}

FText FMantleVehicleShape::SizeHelp(const EMantleVehicleFamily Family)
{
	switch (Family)
	{
	case EMantleVehicleFamily::Aircraft:
		return LOCTEXT("HelpSizeAircraft", "How big the whole plane is: its body, wings, tail and engines grow together, and it is re-balanced for its size. A bigger plane weighs more and takes off faster.");
	case EMantleVehicleFamily::Helicopter:
		return LOCTEXT("HelpSizeHelicopter", "How big the whole helicopter is: its cabin, rotor and tail grow together, so it still lifts what it weighs. A bigger helicopter weighs more.");
	case EMantleVehicleFamily::Hovercraft:
		return LOCTEXT("HelpSizeHovercraft", "How big the whole hovercraft is: its hull, fan and skirt grow together. A bigger hovercraft weighs more and floats higher.");
	default:
		return LOCTEXT("HelpSizeGround", "How big the whole vehicle is, all of its sizes together.");
	}
}

FText FMantleVehicleShape::FeelLabel(const EMantleVehicleFeel Feel)
{
	switch (Feel)
	{
	case EMantleVehicleFeel::Grip: return LOCTEXT("LabelGrip", "Grip");
	case EMantleVehicleFeel::Power: return LOCTEXT("LabelPower", "Power");
	case EMantleVehicleFeel::Stiffness: return LOCTEXT("LabelStiffness", "Suspension");
	case EMantleVehicleFeel::TopSpeed: return LOCTEXT("LabelTopSpeed", "Top speed");
	}
	return FText::GetEmpty();
}

FText FMantleVehicleShape::FeelHelp(const EMantleVehicleFeel Feel)
{
	switch (Feel)
	{
	case EMantleVehicleFeel::Grip: return LOCTEXT("HelpGrip", "How well the tyres hold the road. Slide towards grippy for up to 30 percent more grip, or towards slippery for up to 30 percent less.");
	case EMantleVehicleFeel::Power: return LOCTEXT("HelpPower", "How hard the engine pulls for its weight. Slide towards strong for up to 30 percent more power, or towards weak for up to 30 percent less.");
	case EMantleVehicleFeel::Stiffness: return LOCTEXT("HelpStiffness", "How firm the ride is. Slide towards stiff for up to 30 percent stiffer springs, or towards soft for up to 30 percent softer.");
	case EMantleVehicleFeel::TopSpeed: return LOCTEXT("HelpTopSpeed", "Slide towards fast for up to 30 percent more top speed at this size, or towards slow for up to 30 percent less.");
	}
	return FText::GetEmpty();
}

FText FMantleVehicleShape::PassengerLabel()
{
	return LOCTEXT("LabelPassenger", "Passenger seat");
}

FText FMantleVehicleShape::PassengerHelp()
{
	return LOCTEXT("HelpPassenger", "A second seat, beside the driver's, for a passenger. Turn it off for a vehicle that carries one person.");
}

FText FMantleVehicleShape::DriveAdvice(const FMantleChassisSpec& Spec)
{
	if (Spec.Aircraft.bSet)
	{
		// Take-off rolls of about 130 m for the jet and 340 m for the plane, with some to spare.
		return Spec.Aircraft.bPropeller
			? LOCTEXT("AdvicePlane", "A plane needs a long, flat, open stretch to take off from, about 350 m, or a runway. Mantle puts it on the flattest ground it finds ahead of you and tells you where.")
			: LOCTEXT("AdviceJet", "A jet needs a long, flat, open stretch to take off from, about 150 m, or a runway. Mantle puts it on the flattest ground it finds ahead of you and tells you where.");
	}
	if (Spec.Helicopter.bSet)
	{
		return LOCTEXT("AdviceHelicopter", "A helicopter needs open space above it to lift off: nothing overhead, no roof, trees or ceiling.");
	}
	return FText::GetEmpty();
}

FText FMantleVehicleShape::SizeText(const double Size)
{
	return FText::Format(LOCTEXT("SizeValue", "{0}x"), FText::AsNumber(FMath::RoundToDouble(Size * 100.0) / 100.0));
}

FString FMantleVehicleShape::PlainReason(const FString& Reason)
{
	FString Out = Reason;
	Out.TrimEndInline();
	while (Out.EndsWith(TEXT(".")))
	{
		Out.LeftChopInline(1);
		Out.TrimEndInline();
	}
	return Out;
}

FText FMantleVehicleShape::LengthText(const double Centimetres)
{
	if (FMath::Abs(Centimetres) < 100.0)
	{
		return FText::Format(LOCTEXT("LengthCm", "{0} cm"), FText::AsNumber(FMath::RoundToDouble(Centimetres * 10.0) / 10.0));
	}
	return FText::Format(LOCTEXT("LengthM", "{0} m"), FText::AsNumber(FMath::RoundToDouble(Centimetres) / 100.0));
}

void FMantleVehicleShapeSession::Start(const FMantleChassisSpec& InArchetype, const double InMinSize, const double InMaxSize)
{
	// The preset's own fit range when it has one, otherwise FMantleChassisSpec's.
	MinSize = InMinSize > 0.0 ? InMinSize : FMantleChassisSpec::MinScale;
	MaxSize = InMaxSize > 0.0 ? InMaxSize : FMantleChassisSpec::MaxScale;
	if (MaxSize < MinSize)
	{
		MaxSize = MinSize;
	}
	Archetype = InArchetype;
	Dimensions = FMantleVehicleShape::ArchetypeDimensionsOf(Archetype);
	Size = 1.0;
	Feel = FMantleChassisFeel();
	bPassenger = Archetype.bPassenger;
	HeldNotes.Reset();
	bStarted = true;
	Refresh();
}

bool FMantleVehicleShapeSession::SetDimension(const EMantleVehicleDimension Dimension, const double Wanted)
{
	// A type shaped by one Size has no eight sizes to move: a wing or a rotor moved on its own would leave it out of trim.
	if (!bStarted || UsesSize())
	{
		return false;
	}
	TArray<FText> Notes;
	const FMantleVehicleDimensions Next = FMantleVehicleShape::Edit(Archetype, Dimensions, Dimension, Wanted, Notes);
	const bool bSizesChanged = !Next.Equals(Dimensions);
	if (!bSizesChanged && MantleVehicleShapePrivate::SameNotes(Notes, HeldNotes))
	{
		// The same size again (a drag that ended where it was, a commit after the drag, a number held where it already is): nothing to do.
		return false;
	}
	Dimensions = Next;
	HeldNotes = MoveTemp(Notes);
	Refresh();
	return bSizesChanged;
}

bool FMantleVehicleShapeSession::Reset()
{
	if (!bStarted)
	{
		return false;
	}
	const bool bWasShaped = IsShaped();
	Dimensions = FMantleVehicleShape::ArchetypeDimensionsOf(Archetype);
	Size = 1.0;
	Feel = FMantleChassisFeel();
	bPassenger = Archetype.bPassenger;
	HeldNotes.Reset();
	Refresh();
	return bWasShaped;
}

bool FMantleVehicleShapeSession::IsShaped() const
{
	const bool bFeelMoved = FeelMoved();
	if (UsesSize())
	{
		return FMath::Abs(Size - 1.0) > MantleVehicleShapePrivate::SameSize || bFeelMoved || bPassenger != Archetype.bPassenger;
	}
	return FMantleVehicleShape::IsShaped(Archetype, Dimensions) || bFeelMoved || bPassenger != Archetype.bPassenger;
}

bool FMantleVehicleShapeSession::SetSize(const double Wanted)
{
	using namespace MantleVehicleShapePrivate;
	if (!UsesSize() || !FMath::IsFinite(Wanted))
	{
		return false;
	}
	const double Held = FMath::Clamp(Wanted, MinSize, MaxSize);
	TArray<FText> Notes;
	if (FMath::Abs(Held - Wanted) > HeldSizeNote)
	{
		Notes.Add(Wanted < Held
			? FText::Format(LOCTEXT("HeldSizeLow", "The size can't go below {0}, so it is held there."), FMantleVehicleShape::SizeText(Held))
			: FText::Format(LOCTEXT("HeldSizeHigh", "The size can't go above {0}, so it is held there."), FMantleVehicleShape::SizeText(Held)));
	}
	const bool bSizeChanged = FMath::Abs(Held - Size) > SameSize;
	if (!bSizeChanged && SameNotes(Notes, HeldNotes))
	{
		// The same size again (a drag's last step and its commit): nothing to do.
		return false;
	}
	Size = Held;
	HeldNotes = MoveTemp(Notes);
	Refresh();
	return bSizeChanged;
}

bool FMantleVehicleShapeSession::HasFeel(const EMantleVehicleFeel Which) const
{
	if (!bStarted)
	{
		return false;
	}
	switch (FMantleVehicleShape::FamilyOf(Archetype))
	{
	case EMantleVehicleFamily::Ground:
		return true;
	case EMantleVehicleFamily::Hovercraft:
		return Which == EMantleVehicleFeel::TopSpeed;
	default:
		return false;
	}
}

double* FMantleVehicleShapeSession::FeelField(const EMantleVehicleFeel Which)
{
	switch (Which)
	{
	case EMantleVehicleFeel::Grip: return &Feel.Grip;
	case EMantleVehicleFeel::Power: return &Feel.Power;
	case EMantleVehicleFeel::Stiffness: return &Feel.Stiffness;
	default: return &Feel.TopSpeed;
	}
}

double FMantleVehicleShapeSession::GetFeel(const EMantleVehicleFeel Which) const
{
	switch (Which)
	{
	case EMantleVehicleFeel::Grip: return Feel.Grip;
	case EMantleVehicleFeel::Power: return Feel.Power;
	case EMantleVehicleFeel::Stiffness: return Feel.Stiffness;
	default: return Feel.TopSpeed;
	}
}

bool FMantleVehicleShapeSession::FeelMoved() const
{
	for (const EMantleVehicleFeel Which : MantleVehicleFeels)
	{
		if (FMath::Abs(GetFeel(Which)) > MantleVehicleShapePrivate::SameSize)
		{
			return true;
		}
	}
	return false;
}

bool FMantleVehicleShapeSession::SetFeel(const EMantleVehicleFeel Which, const double Wanted)
{
	if (!HasFeel(Which) || !FMath::IsFinite(Wanted))
	{
		return false;
	}
	const double Held = FMath::Clamp(Wanted, -1.0, 1.0);
	double* Field = FeelField(Which);
	if (FMath::Abs(Held - *Field) <= MantleVehicleShapePrivate::SameSize)
	{
		return false;
	}
	*Field = Held;
	Refresh();
	return true;
}

bool FMantleVehicleShapeSession::SetPassenger(const bool bWanted)
{
	if (!HasPassengerOption() || bWanted == bPassenger)
	{
		return false;
	}
	bPassenger = bWanted;
	Refresh();
	return true;
}

FString FMantleVehicleShapeSession::Signature() const
{
	return FString::Printf(TEXT("%s|%.4f|%.4f,%.4f,%.4f,%.4f|%d"), *Dimensions.Signature(), Size, Feel.Grip, Feel.Power, Feel.Stiffness, Feel.TopSpeed, bPassenger ? 1 : 0);
}

void FMantleVehicleShapeSession::Refresh()
{
	if (UsesSize())
	{
		// One Size grows the whole vehicle together, its airframe, rotors or pads with its body, and derives its handling again: a Size of 1 is the
		// preset itself, hand-tuned.
		Shaped = FMantleChassisSpec::Scaled(Archetype, Size);
	}
	else
	{
		Shaped = FMantleVehicleShape::Reshape(Archetype, Dimensions);
	}
	Shaped.bPassenger = bPassenger;
	if (FeelMoved())
	{
		// The feel is one more thing the handling is derived from. A chassis still at its own sizes has no derived handling yet (it is the hand-tuned preset), so this
		// is what makes it a shaped one. A motorcycle's rear tyre keeps its share of the front's, as Reshape keeps it when the sizes move.
		const FMantleChassisHandling Before = MantleVehicleShapePrivate::HandlingOf(Shaped);
		const FMantleChassisHandling After = FMantleChassisDerive::Derive(Shaped, Feel);
		if (Before.TireFriction > 0.0)
		{
			Shaped.Rider.RearTireFriction *= After.TireFriction / Before.TireFriction;
		}
		Shaped.CenterOfMass = After.CenterOfMass;
		Shaped.Derived = After;
	}
	Summary = FMantleVehicleShape::Describe(FMantleVehicleShape::Summarise(Shaped));
	TArray<FText> Lines = HeldNotes;
	Lines.Append(FMantleVehicleShape::Warnings(Shaped));
	FString List;
	for (const FText& Line : Lines)
	{
		List += (List.IsEmpty() ? TEXT("- ") : TEXT("\n- ")) + Line.ToString();
	}
	Warnings = FText::FromString(List);
}

#undef LOCTEXT_NAMESPACE
