// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleChassisFit.h"

#include "MantleVehicleConverter.h"
#include "MantleVehicleSource.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleChassisFitPrivate
{
	/** The axle's wheels are those within this fraction of the wheelbase of the front or rear wheel. */
	constexpr double AxleBand = 0.1;

	/** Wheels spread across the model by less than the larger of this (cm) and this share of the wheelbase are in one line down the middle. */
	constexpr double CentrelineSpreadMin = 4.0;
	constexpr double CentrelineSpreadShare = 0.03;

	/** The most a box's half size may be written as (the vehicle asset's own limit). */
	constexpr double MaxBoxExtent = 1000.0;

	double Clamped(const double Raw, bool& bOutClamped)
	{
		const double Held = FMath::Clamp(Raw, FMantleVehicleChassisFit::MinStretch, FMantleVehicleChassisFit::MaxStretch);
		bOutClamped = Held != Raw;
		return Held;
	}

	/** Fewer points than this above the tub are noise, not a cabin. */
	constexpr int32 MinBodyPoints = 8;

	/** A box shorter than this above the tub (model units) is not worth a collision shape. */
	constexpr double MinBodyBoxHeight = 4.0;

	/** The share of points trimmed from each end of a range, so an aerial or a mirror does not stretch a box. */
	constexpr double TrimShare = 0.005;

	/** Two halves whose roofs differ by less than this share of the taller are one box. */
	constexpr double SameHeightShare = 0.12;

	/** The value Share (0..1) of the way up Values, sorted in place; with few values that is the smallest or largest. */
	double Percentile(TArray<double>& Values, const double Share)
	{
		Values.Sort();
		// Rounded, so the trim is the same at both ends: with few values nothing is dropped at either end.
		return Values[FMath::Clamp(FMath::RoundToInt(Share * (Values.Num() - 1)), 0, Values.Num() - 1)];
	}

	/** One box over Points, from Floor up to the trimmed top of their Z; false when it would be too low to matter. */
	bool BoxOver(const TArray<FVector>& Points, const double Floor, FMantleChassisBox& Out)
	{
		TArray<double> Xs;
		TArray<double> Ys;
		TArray<double> Zs;
		for (const FVector& Point : Points)
		{
			Xs.Add(Point.X);
			Ys.Add(Point.Y);
			Zs.Add(Point.Z);
		}
		if (Points.Num() < 3)
		{
			return false;
		}
		const double Top = Percentile(Zs, 1.0 - TrimShare);
		if (Top - Floor < MinBodyBoxHeight)
		{
			return false;
		}
		const FVector Min(Percentile(Xs, TrimShare), Percentile(Ys, TrimShare), Floor);
		const FVector Max(Percentile(Xs, 1.0 - TrimShare), Percentile(Ys, 1.0 - TrimShare), Top);
		Out.Center = (Min + Max) * 0.5;
		Out.Extent = ((Max - Min) * 0.5).ComponentMax(FVector(1.0)).ComponentMin(FVector(MaxBoxExtent));
		Out.Center = Out.Center.BoundToCube(MaxBoxExtent);
		return true;
	}

	/** "1.12x the length" and so on. */
	FString DescribeStretch(const double K, const TCHAR* What)
	{
		return FString::Printf(TEXT("%.2fx the %s"), K, What);
	}
}

FVector FMantleChassisStretch::PointToModel(const FVector& ChassisPoint) const
{
	return Origin + FVector(ChassisPoint.X * Kx, ChassisPoint.Y * Ky, ChassisPoint.Z * Kz) / Size;
}

FVector FMantleChassisStretch::ExtentToModel(const FVector& ChassisExtent) const
{
	return FVector(ChassisExtent.X * Kx, ChassisExtent.Y * Ky, ChassisExtent.Z * Kz) / Size;
}

bool FMantleChassisStretch::IsStretched() const
{
	return FMath::Abs(Kx - 1.0) > 0.02 || FMath::Abs(Ky - 1.0) > 0.02 || FMath::Abs(Kz - 1.0) > 0.02;
}

FMantleChassisLayout FMantleVehicleChassisFit::LayoutFromWheels(const TConstArrayView<FMantleVehicleFitWheel> Wheels)
{
	using namespace MantleVehicleChassisFitPrivate;
	FMantleChassisLayout Layout;
	if (Wheels.Num() < 2)
	{
		return Layout;
	}
	FVector Min = Wheels[0].Centre;
	FVector Max = Wheels[0].Centre;
	double SumZ = 0.0;
	double SumRadius = 0.0;
	int32 RadiusCount = 0;
	for (const FMantleVehicleFitWheel& Wheel : Wheels)
	{
		Min = Min.ComponentMin(Wheel.Centre);
		Max = Max.ComponentMax(Wheel.Centre);
		SumZ += Wheel.Centre.Z;
		if (Wheel.Radius > 0.0)
		{
			SumRadius += Wheel.Radius;
			++RadiusCount;
		}
	}
	const double Length = Max.X - Min.X;
	if (Length < 1.0)
	{
		return Layout;
	}

	// Every wheel on the centre line (a motorcycle's two): the layout has no track to find, and Track = 0 is not an invalid layout.
	if (Max.Y - Min.Y < FMath::Max(CentrelineSpreadMin, CentrelineSpreadShare * Length))
	{
		Layout.bCentreline = true;
		Layout.Track = 0.0;
		Layout.FrontX = Max.X;
		Layout.RearX = Min.X;
		Layout.CentreY = 0.5 * (Min.Y + Max.Y);
		Layout.CentreZ = SumZ / Wheels.Num();
		Layout.Radius = RadiusCount > 0 ? SumRadius / RadiusCount : 0.0;
		Layout.bValid = true;
		return Layout;
	}

	// The track is each end axle's spread across the car, averaged; with no end axle that has a wheel on each side, the spread of them all.
	const double Band = AxleBand * Length;
	double TrackSum = 0.0;
	int32 TrackCount = 0;
	for (const bool bFront : { true, false })
	{
		double LowY = TNumericLimits<double>::Max();
		double HighY = TNumericLimits<double>::Lowest();
		int32 Count = 0;
		for (const FMantleVehicleFitWheel& Wheel : Wheels)
		{
			if (bFront ? Wheel.Centre.X >= Max.X - Band : Wheel.Centre.X <= Min.X + Band)
			{
				LowY = FMath::Min(LowY, Wheel.Centre.Y);
				HighY = FMath::Max(HighY, Wheel.Centre.Y);
				++Count;
			}
		}
		if (Count >= 2 && HighY - LowY >= 1.0)
		{
			TrackSum += HighY - LowY;
			++TrackCount;
		}
	}
	Layout.Track = TrackCount > 0 ? TrackSum / TrackCount : Max.Y - Min.Y;
	if (Layout.Track < 1.0)
	{
		return Layout;
	}
	Layout.FrontX = Max.X;
	Layout.RearX = Min.X;
	Layout.CentreY = 0.5 * (Min.Y + Max.Y);
	Layout.CentreZ = SumZ / Wheels.Num();
	Layout.Radius = RadiusCount > 0 ? SumRadius / RadiusCount : 0.0;
	Layout.bValid = true;
	return Layout;
}

FMantleChassisStretch FMantleVehicleChassisFit::Stretch(const FMantleVehicleChassisReference& Chassis, const FMantleChassisLayout& Layout, const double Size)
{
	using namespace MantleVehicleChassisFitPrivate;
	FMantleChassisStretch Out;
	Out.Size = Size > 0.0 ? Size : 1.0;
	const double Radius = Layout.FitRadius();
	Out.Kx = Clamped(Out.Size * Layout.Wheelbase() / Chassis.Wheelbase, Out.bClampedX);
	// A centreline chassis (a motorcycle) or layout has no track to compare: its width grows as its wheels do, so a bike with big wheels is a bigger bike.
	const double TrackRatio = Layout.bCentreline || Chassis.bCentreline ? Layout.FitRadius() / Chassis.WheelRadius : Layout.Track / Chassis.Track;
	Out.Ky = Clamped(Out.Size * TrackRatio, Out.bClampedY);
	Out.Kz = Clamped(Out.Size * Radius / Chassis.WheelRadius, Out.bClampedZ);
	// The chassis's origin on the model: the middle of its axles, the ground under its wheels, the middle of its track.
	Out.Origin = FVector(0.5 * (Layout.FrontX + Layout.RearX), Layout.CentreY, Layout.CentreZ - Radius);
	return Out;
}

FMantleChassisPlacement FMantleVehicleChassisFit::Place(const FMantleVehicleChassisReference& Chassis, const FMantleChassisStretch& Stretch)
{
	using namespace MantleVehicleChassisFitPrivate;
	FMantleChassisPlacement Out;
	Out.CollisionCenter = Stretch.PointToModel(FVector(0.0, 0.0, Chassis.GroundClearance + 0.5 * Chassis.BodyHeight));
	Out.CollisionExtent = Stretch.ExtentToModel(FVector(0.5 * Chassis.BodyLength, 0.5 * Chassis.BodyWidth, 0.5 * Chassis.BodyHeight));
	Out.CollisionExtent = Out.CollisionExtent.ComponentMin(FVector(MaxBoxExtent));
	Out.CenterOfMass = Stretch.PointToModel(Chassis.CenterOfMass);
	Out.Seat = Stretch.PointToModel(Chassis.Seat);
	Out.bSteeringWheel = Chassis.bSteeringWheel;
	Out.SteeringWheelHub = Stretch.PointToModel(Chassis.SteeringWheelHub);
	Out.SteeringWheelRadius = Chassis.SteeringWheelRadius * Stretch.Kz / Stretch.Size;
	Out.SteeringWheelPitch = Chassis.SteeringWheelPitch;
	Out.bPassenger = Chassis.bPassenger;
	if (Chassis.bPassenger)
	{
		Out.PassengerSeat = Stretch.PointToModel(Chassis.PassengerSeat());
	}
	return Out;
}

TArray<FMantleChassisBox> FMantleVehicleChassisFit::FitBodyBoxes(const TConstArrayView<FVector> BodyPoints, const FVector& TubCenter, const FVector& TubExtent)
{
	using namespace MantleVehicleChassisFitPrivate;
	TArray<FMantleChassisBox> Boxes;
	const double TubTop = TubCenter.Z + TubExtent.Z;

	TArray<FVector> Above;
	for (const FVector& Point : BodyPoints)
	{
		if (Point.Z > TubTop + 1.0 && !Point.ContainsNaN())
		{
			Above.Add(Point);
		}
	}
	if (Above.Num() < MinBodyPoints)
	{
		return Boxes;
	}

	// The middle of the trimmed X range, so a stray point does not move the cut.
	TArray<double> Xs;
	for (const FVector& Point : Above)
	{
		Xs.Add(Point.X);
	}
	const double Middle = 0.5 * (Percentile(Xs, TrimShare) + Percentile(Xs, 1.0 - TrimShare));
	TArray<FVector> Front;
	TArray<FVector> Rear;
	for (const FVector& Point : Above)
	{
		(Point.X >= Middle ? Front : Rear).Add(Point);
	}

	FMantleChassisBox FrontBox;
	FMantleChassisBox RearBox;
	const bool bFront = BoxOver(Front, TubTop, FrontBox);
	const bool bRear = BoxOver(Rear, TubTop, RearBox);
	if (bFront && bRear)
	{
		const double FrontHeight = 2.0 * FrontBox.Extent.Z;
		const double RearHeight = 2.0 * RearBox.Extent.Z;
		if (FMath::Abs(FrontHeight - RearHeight) <= SameHeightShare * FMath::Max(FrontHeight, RearHeight))
		{
			// One roof line: one box over the whole body above the tub.
			FMantleChassisBox Whole;
			if (BoxOver(Above, TubTop, Whole))
			{
				Boxes.Add(Whole);
			}
		}
		else
		{
			Boxes.Add(FrontBox);
			Boxes.Add(RearBox);
		}
	}
	else if (bFront)
	{
		Boxes.Add(FrontBox);
	}
	else if (bRear)
	{
		Boxes.Add(RearBox);
	}

	// None sticks out past the visible body in length or width.
	FVector2D BodyMin;
	FVector2D BodyMax;
	if (BodyExtentXY(BodyPoints, BodyMin, BodyMax))
	{
		for (FMantleChassisBox& Box : Boxes)
		{
			ClampBoxToBodyXY(Box, BodyMin, BodyMax);
		}
	}
	return Boxes;
}

bool FMantleVehicleChassisFit::BodyExtentXY(const TConstArrayView<FVector> BodyPoints, FVector2D& OutMin, FVector2D& OutMax)
{
	using namespace MantleVehicleChassisFitPrivate;
	TArray<double> Xs;
	TArray<double> Ys;
	for (const FVector& Point : BodyPoints)
	{
		if (!Point.ContainsNaN())
		{
			Xs.Add(Point.X);
			Ys.Add(Point.Y);
		}
	}
	if (Xs.Num() < MinBodyPoints)
	{
		return false;
	}
	OutMin = FVector2D(Percentile(Xs, TrimShare), Percentile(Ys, TrimShare));
	OutMax = FVector2D(Percentile(Xs, 1.0 - TrimShare), Percentile(Ys, 1.0 - TrimShare));
	return true;
}

bool FMantleVehicleChassisFit::ClampBoxToBodyXY(FMantleChassisBox& Box, const FVector2D& BodyMin, const FVector2D& BodyMax)
{
	bool bChanged = false;
	for (int32 Axis = 0; Axis < 2; ++Axis)
	{
		const double Low = FMath::Max(Box.Center[Axis] - Box.Extent[Axis], BodyMin[Axis]);
		const double High = FMath::Min(Box.Center[Axis] + Box.Extent[Axis], BodyMax[Axis]);
		if (High - Low < 2.0)
		{
			continue;
		}
		const double Centre = 0.5 * (Low + High);
		const double Half = 0.5 * (High - Low);
		if (!FMath::IsNearlyEqual(Centre, Box.Center[Axis], 1e-6) || !FMath::IsNearlyEqual(Half, Box.Extent[Axis], 1e-6))
		{
			bChanged = true;
		}
		Box.Center[Axis] = Centre;
		Box.Extent[Axis] = Half;
	}
	return bChanged;
}

bool FMantleVehicleChassisFit::ClampTubToBody(FMantleChassisPlacement& Placement, const TConstArrayView<FVector> BodyPoints)
{
	FVector2D BodyMin;
	FVector2D BodyMax;
	if (!BodyExtentXY(BodyPoints, BodyMin, BodyMax))
	{
		return false;
	}
	FMantleChassisBox Tub;
	Tub.Center = Placement.CollisionCenter;
	Tub.Extent = Placement.CollisionExtent;
	const bool bChanged = ClampBoxToBodyXY(Tub, BodyMin, BodyMax);
	Placement.CollisionCenter = Tub.Center;
	Placement.CollisionExtent = Tub.Extent;
	return bChanged;
}

void FMantleVehicleChassisFit::Apply(const FMantleChassisPlacement& Placement, UMantleVehicleSource& Source, const int32 MaxBoxes)
{
	Source.Modify();
	FMantleVehicleSourceTuning& Tuning = Source.Tuning;

	// The model's own physics asset gave the converter its boxes and centre of mass; neither survives.
	Tuning.Collision.Reset();
	FMantleVehicleSourceBox& Box = Tuning.Collision.AddDefaulted_GetRef();
	Box.Center = Placement.CollisionCenter;
	Box.Extent = Placement.CollisionExtent;
	for (const FMantleChassisBox& Body : Placement.BodyBoxes)
	{
		if (Tuning.Collision.Num() >= MaxBoxes)
		{
			break;
		}
		FMantleVehicleSourceBox& Added = Tuning.Collision.AddDefaulted_GetRef();
		Added.Center = Body.Center;
		Added.Extent = Body.Extent;
	}
	Tuning.bOverrideCenterOfMass = true;
	Tuning.CenterOfMass = Placement.CenterOfMass;

	Tuning.bSeatLocation = true;
	Tuning.SeatLocation = Placement.Seat;
	// A rider's seat has handlebars, not a steering wheel, so a motorcycle's is not forced on; a pilot's has none either.
	Tuning.bSteeringWheel = Placement.bSteeringWheel;
	Tuning.SteeringWheelLocation = Placement.SteeringWheelHub;
	Tuning.SteeringWheelRotation = FRotator(Placement.SteeringWheelPitch, 0.0, 0.0);
	Tuning.SteeringWheelRadius = static_cast<float>(Placement.SteeringWheelRadius);

	Tuning.bPassengerSeat = Placement.bPassenger;
	if (Placement.bPassenger)
	{
		Tuning.PassengerSeatLocation = Placement.PassengerSeat;
	}
	else
	{
		Tuning.bHasPassengerDoor = false;
	}

	// The contact points hang off the seats and the collision, so they are guessed again from the chassis's. A car's guess sees all its boxes (the body boxes sit inside the tub's
	// width); a model's collision may have wings and a tail, so its exits are guessed beside the first box alone.
	const bool bFirstBoxOnly = MaxBoxes > MaxBodyBoxes + 1 && Tuning.Collision.Num() > 1;
	TArray<FMantleVehicleSourceBox> AllBoxes;
	if (bFirstBoxOnly)
	{
		AllBoxes = MoveTemp(Tuning.Collision);
		Tuning.Collision.Reset();
		Tuning.Collision.Add(AllBoxes[0]);
	}
	Tuning.DriverContacts = FMantleVehicleConverter::GuessSeatContacts(Tuning, /*bPassenger*/ false);
	Tuning.PassengerContacts = Placement.bPassenger ? FMantleVehicleConverter::GuessSeatContacts(Tuning, /*bPassenger*/ true) : FMantleVehicleSourceSeatContacts();
	if (bFirstBoxOnly)
	{
		Tuning.Collision = MoveTemp(AllBoxes);
	}
}

TArray<FString> FMantleVehicleChassisFit::StretchNotes(const FMantleChassisStretch& Stretch)
{
	using namespace MantleVehicleChassisFitPrivate;
	TArray<FString> Notes;
	if (!Stretch.IsStretched())
	{
		return Notes;
	}
	Notes.Add(FString::Printf(TEXT("The chassis was stretched to your model's wheels (%s, %s, %s)."),
		*DescribeStretch(Stretch.Kx, TEXT("length")), *DescribeStretch(Stretch.Ky, TEXT("width")), *DescribeStretch(Stretch.Kz, TEXT("height"))));
	if (Stretch.bClampedX || Stretch.bClampedY || Stretch.bClampedZ)
	{
		Notes.Add(FString::Printf(TEXT("Your model's wheels are a different shape from this chassis by more than it can stretch (%.1fx to %.1fx), so it was stretched as far as it goes. Check that the wheels sit under the body, or pick a vehicle type closer to your model."),
			FMantleVehicleChassisFit::MinStretch, FMantleVehicleChassisFit::MaxStretch));
	}
	return Notes;
}
