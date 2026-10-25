// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehicleChassisFit.h"
#include "MantleVehicleSource.h"

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

// A creator's model as a skin on a preset's chassis: the model gives only its wheel layout, and every physical value is the
// chassis's, stretched to that layout and written in the model's units. The arithmetic is pure, so it is tested here without a mesh.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleChassisFitTestPrivate
{
	/** The Truck chassis's numbers (schema/vehicle-presets/truck.json), so the test needs no file. */
	FMantleVehicleChassisReference TruckChassis()
	{
		FMantleVehicleChassisReference Chassis;
		Chassis.Wheelbase = 350.0;
		Chassis.Track = 168.0;
		Chassis.WheelRadius = 42.0;
		Chassis.WheelWidth = 28.0;
		Chassis.BodyLength = 640.0;
		Chassis.BodyWidth = 205.0;
		Chassis.BodyHeight = 96.0;
		Chassis.GroundClearance = 48.0;
		Chassis.CenterOfMass = FVector(0.0, 0.0, 61.0);
		Chassis.Seat = FVector(73.2, -42.0, 130.0);
		Chassis.SteeringWheelHub = FVector(120.0, -42.0, 133.0);
		Chassis.SteeringWheelRadius = 21.0;
		Chassis.SteeringWheelPitch = -20.0;
		Chassis.bPassenger = true;
		return Chassis;
	}

	/** Four wheels of a model whose axles are at FrontX and RearX, its track Track about CentreY, wheel centres CentreZ up, radius Radius. */
	TArray<FMantleVehicleFitWheel> FourWheels(const double FrontX, const double RearX, const double Track, const double CentreY, const double CentreZ, const double Radius)
	{
		TArray<FMantleVehicleFitWheel> Wheels;
		for (const double X : { FrontX, RearX })
		{
			for (const double Side : { -0.5, 0.5 })
			{
				FMantleVehicleFitWheel& Wheel = Wheels.AddDefaulted_GetRef();
				Wheel.Centre = FVector(X, CentreY + Side * Track, CentreZ);
				Wheel.Radius = Radius;
			}
		}
		return Wheels;
	}

	/** The wheels of a model that matches the chassis exactly: a pair at every axle, front first. */
	TArray<FMantleVehicleFitWheel> AxleWheels(const FMantleVehicleChassisReference& Chassis)
	{
		TArray<FMantleVehicleFitWheel> Wheels;
		for (int32 Axle = 0; Axle < Chassis.AxlePositions.Num(); ++Axle)
		{
			for (const double Side : { -0.5, 0.5 })
			{
				FMantleVehicleFitWheel& Wheel = Wheels.AddDefaulted_GetRef();
				Wheel.Centre = FVector(Chassis.AxleX(Axle), Side * Chassis.Track, Chassis.WheelRadius);
				Wheel.Radius = Chassis.WheelRadius;
			}
		}
		return Wheels;
	}

	bool Near(const FVector& A, const FVector& B) { return A.Equals(B, 1e-6); }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleChassisFitLayoutTest,
	"Mantle.Vehicle.ChassisFit.Layout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleChassisFitLayoutTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleChassisFitTestPrivate;

	// A model modelled off its origin: the layout is read from the wheels alone.
	const FMantleChassisLayout Layout = FMantleVehicleChassisFit::LayoutFromWheels(FourWheels(300.0, 20.0, 160.0, 15.0, 40.0, 38.0));
	TestTrue(TEXT("four wheels make a layout"), Layout.bValid);
	TestEqual(TEXT("the front axle"), Layout.FrontX, 300.0);
	TestEqual(TEXT("the rear axle"), Layout.RearX, 20.0);
	TestEqual(TEXT("the wheelbase"), Layout.Wheelbase(), 280.0);
	TestEqual(TEXT("the track"), Layout.Track, 160.0);
	TestEqual(TEXT("the middle across the car"), Layout.CentreY, 15.0);
	TestEqual(TEXT("the wheel centres' height"), Layout.CentreZ, 40.0);
	TestEqual(TEXT("the radius"), Layout.Radius, 38.0);

	// Six wheels (a tandem rear): the end axles give the wheelbase.
	TArray<FMantleVehicleFitWheel> Six = FourWheels(300.0, 0.0, 160.0, 0.0, 40.0, 38.0);
	for (const double Side : { -0.5, 0.5 })
	{
		FMantleVehicleFitWheel& Extra = Six.AddDefaulted_GetRef();
		Extra.Centre = FVector(60.0, Side * 160.0, 40.0);
		Extra.Radius = 38.0;
	}
	const FMantleChassisLayout SixLayout = FMantleVehicleChassisFit::LayoutFromWheels(Six);
	TestEqual(TEXT("six wheels: the wheelbase is front to rear"), SixLayout.Wheelbase(), 300.0);
	TestEqual(TEXT("six wheels: the track is still one axle's"), SixLayout.Track, 160.0);

	// A radius that could not be read leaves the ground at the wheel centres' height below nothing: Z = 0.
	TArray<FMantleVehicleFitWheel> Unknown = FourWheels(300.0, 0.0, 160.0, 0.0, 40.0, 0.0);
	const FMantleChassisLayout UnknownLayout = FMantleVehicleChassisFit::LayoutFromWheels(Unknown);
	TestTrue(TEXT("wheels with no radius still make a layout"), UnknownLayout.bValid);
	TestEqual(TEXT("an unknown radius is the centres' height"), UnknownLayout.FitRadius(), 40.0);

	TestFalse(TEXT("one wheel is no layout"), FMantleVehicleChassisFit::LayoutFromWheels(TConstArrayView<FMantleVehicleFitWheel>(Unknown.GetData(), 1)).bValid);
	TestFalse(TEXT("no wheels is no layout"), FMantleVehicleChassisFit::LayoutFromWheels(TConstArrayView<FMantleVehicleFitWheel>()).bValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleChassisFitIdentityTest,
	"Mantle.Vehicle.ChassisFit.Identity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleChassisFitIdentityTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleChassisFitTestPrivate;
	const FMantleVehicleChassisReference Chassis = TruckChassis();

	// A model whose wheels are exactly the chassis's: nothing is stretched and every value is the chassis's own.
	const FMantleChassisLayout Layout = FMantleVehicleChassisFit::LayoutFromWheels(FourWheels(175.0, -175.0, 168.0, 0.0, 42.0, 42.0));
	const FMantleChassisStretch Stretch = FMantleVehicleChassisFit::Stretch(Chassis, Layout, 1.0);
	TestEqual(TEXT("length is not stretched"), Stretch.Kx, 1.0);
	TestEqual(TEXT("width is not stretched"), Stretch.Ky, 1.0);
	TestEqual(TEXT("height is not stretched"), Stretch.Kz, 1.0);
	TestFalse(TEXT("an exact fit says nothing about stretching"), Stretch.IsStretched());
	TestEqual(TEXT("so there are no stretch notes"), FMantleVehicleChassisFit::StretchNotes(Stretch).Num(), 0);
	TestTrue(TEXT("the chassis origin is the origin"), Near(Stretch.Origin, FVector::ZeroVector));

	const FMantleChassisPlacement Placed = FMantleVehicleChassisFit::Place(Chassis, Stretch);
	TestTrue(TEXT("the box is the chassis body box's centre"), Near(Placed.CollisionCenter, FVector(0.0, 0.0, 96.0)));
	TestTrue(TEXT("the box is the chassis body box's size"), Near(Placed.CollisionExtent, FVector(320.0, 102.5, 48.0)));
	TestTrue(TEXT("the centre of mass"), Near(Placed.CenterOfMass, FVector(0.0, 0.0, 61.0)));
	TestTrue(TEXT("the driver's seat (cab forward)"), Near(Placed.Seat, FVector(73.2, -42.0, 130.0)));
	TestTrue(TEXT("the steering wheel's hub"), Near(Placed.SteeringWheelHub, FVector(120.0, -42.0, 133.0)));
	TestEqual(TEXT("the steering wheel's radius"), Placed.SteeringWheelRadius, 21.0);
	TestEqual(TEXT("the steering wheel's pitch"), Placed.SteeringWheelPitch, -20.0);
	TestTrue(TEXT("there is a passenger seat"), Placed.bPassenger);
	TestTrue(TEXT("the passenger's seat mirrors the driver's"), Near(Placed.PassengerSeat, FVector(73.2, 42.0, 130.0)));

	// Every shipped preset read from its file fits its own wheels exactly: the file and the arithmetic agree.
	for (const FString& Id : FMantleVehicleChassisBuilder::PresetIds())
	{
		FString Text;
		FString Error;
		FMantleVehicleChassisReference Read;
		if (!TestTrue(*FString::Printf(TEXT("preset %s is read"), *Id),
				FMantleVehicleChassisBuilder::LoadPresetText(Id, Text, Error) && FMantleVehicleChassisBuilder::ReadReference(Text, Read, Error)))
		{
			continue;
		}
		if (Read.bCentreline)
		{
			// A two-wheeler has no track to measure, and is a centreline layout rather than an invalid one.
			TArray<FMantleVehicleFitWheel> Centre;
			for (int32 Axle = 0; Axle < Read.AxlePositions.Num(); ++Axle)
			{
				FMantleVehicleFitWheel& Wheel = Centre.AddDefaulted_GetRef();
				Wheel.Centre = FVector(Read.AxleX(Axle), 0.0, Read.WheelRadius);
				Wheel.Radius = Read.WheelRadius;
			}
			const FMantleChassisLayout CentreLayout = FMantleVehicleChassisFit::LayoutFromWheels(Centre);
			TestTrue(*FString::Printf(TEXT("preset %s: centreline wheels make a centreline layout with no track"), *Id), CentreLayout.bValid && CentreLayout.bCentreline && CentreLayout.Track == 0.0);
			continue;
		}
		const FMantleChassisLayout Own = FMantleVehicleChassisFit::LayoutFromWheels(AxleWheels(Read));
		TestEqual(*FString::Printf(TEXT("preset %s: the layout's wheelbase and track are the chassis's"), *Id), Own.Wheelbase(), Read.Wheelbase);
		TestEqual(*FString::Printf(TEXT("preset %s: the layout's track is the chassis's"), *Id), Own.Track, Read.Track);
		const FMantleChassisStretch OwnStretch = FMantleVehicleChassisFit::Stretch(Read, Own, 1.0);
		const FMantleChassisPlacement OwnPlaced = FMantleVehicleChassisFit::Place(Read, OwnStretch);
		TestFalse(*FString::Printf(TEXT("preset %s fits its own wheels without stretching"), *Id), OwnStretch.IsStretched());
		TestTrue(*FString::Printf(TEXT("preset %s seat is its chassis seat"), *Id), Near(OwnPlaced.Seat, Read.Seat));
		TestTrue(*FString::Printf(TEXT("preset %s centre of mass is its chassis's"), *Id), Near(OwnPlaced.CenterOfMass, Read.CenterOfMass));
		TestTrue(*FString::Printf(TEXT("preset %s centre of mass is above the wheel centres"), *Id), OwnPlaced.CenterOfMass.Z > Read.WheelRadius);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleChassisFitThreeAxlesTest,
	"Mantle.Vehicle.ChassisFit.ThreeAxles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleChassisFitThreeAxlesTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleChassisFitTestPrivate;
	FString Text;
	FString Error;
	FMantleVehicleChassisReference Chassis;
	if (!TestTrue(TEXT("the six-wheeler preset is read"),
			FMantleVehicleChassisBuilder::LoadPresetText(TEXT("sixwheeler"), Text, Error) && FMantleVehicleChassisBuilder::ReadReference(Text, Chassis, Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestEqual(TEXT("the six-wheeler has three axles"), Chassis.AxlePositions.Num(), 3))
	{
		return false;
	}

	// A six-wheel model of 700 cm (1.25x the chassis), its middle axle 21 cm behind where the chassis's share puts it, centred
	// off its origin. The end axles give the wheelbase and track; the middle axle is not asked for.
	TArray<FMantleVehicleFitWheel> Wheels;
	const double ModelWheelbase = 1.25 * Chassis.Wheelbase;
	for (const double X : { 500.0, 500.0 - 0.28 * ModelWheelbase, 500.0 - ModelWheelbase })
	{
		for (const double Side : { -0.5, 0.5 })
		{
			FMantleVehicleFitWheel& Wheel = Wheels.AddDefaulted_GetRef();
			Wheel.Centre = FVector(X, 20.0 + Side * Chassis.Track, Chassis.WheelRadius);
			Wheel.Radius = Chassis.WheelRadius;
		}
	}
	const FMantleChassisLayout Layout = FMantleVehicleChassisFit::LayoutFromWheels(Wheels);
	TestTrue(TEXT("six wheels on three axles make a layout"), Layout.bValid);
	TestTrue(TEXT("the wheelbase is front to rearmost axle"), FMath::IsNearlyEqual(Layout.Wheelbase(), ModelWheelbase, 1e-6));
	TestTrue(TEXT("the track is one axle's, not the whole spread"), FMath::IsNearlyEqual(Layout.Track, Chassis.Track, 1e-6));
	TestEqual(TEXT("the middle across the car"), Layout.CentreY, 20.0);

	const FMantleChassisStretch Stretch = FMantleVehicleChassisFit::Stretch(Chassis, Layout, 1.0);
	TestTrue(TEXT("the chassis is 1.25x longer"), FMath::IsNearlyEqual(Stretch.Kx, 1.25, 1e-6));
	TestEqual(TEXT("and as wide"), Stretch.Ky, 1.0);
	// The chassis's middle axle stretches in proportion with the rest: 1.25x its distance from the middle of the wheelbase.
	const FVector MiddleAxle = Stretch.PointToModel(FVector(Chassis.AxleX(1), -0.5 * Chassis.Track, Chassis.WheelRadius));
	TestTrue(TEXT("the middle axle is stretched in proportion"), FMath::IsNearlyEqual(MiddleAxle.X, Stretch.Origin.X + 1.25 * Chassis.AxleX(1), 1e-6));
	TestTrue(TEXT("the front axle lands on the model's front wheels"), FMath::IsNearlyEqual(Stretch.PointToModel(FVector(Chassis.AxleX(0), 0.0, Chassis.WheelRadius)).X, 500.0, 1e-6));
	TestTrue(TEXT("the rearmost axle lands on the model's rear wheels"), FMath::IsNearlyEqual(Stretch.PointToModel(FVector(Chassis.AxleX(2), 0.0, Chassis.WheelRadius)).X, 500.0 - ModelWheelbase, 1e-6));
	const FMantleChassisPlacement Placed = FMantleVehicleChassisFit::Place(Chassis, Stretch);
	TestTrue(TEXT("the seat is the chassis's, stretched"), Near(Placed.Seat, Stretch.PointToModel(Chassis.Seat)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleChassisFitStretchTest,
	"Mantle.Vehicle.ChassisFit.Stretch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleChassisFitStretchTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleChassisFitTestPrivate;
	const FMantleVehicleChassisReference Chassis = TruckChassis();

	// A limousine: one and a half times the wheelbase, the same track and wheels.
	const FMantleChassisLayout Layout = FMantleVehicleChassisFit::LayoutFromWheels(FourWheels(262.5, -262.5, 168.0, 0.0, 42.0, 42.0));
	const FMantleChassisStretch Stretch = FMantleVehicleChassisFit::Stretch(Chassis, Layout, 1.0);
	TestEqual(TEXT("1.5x longer"), Stretch.Kx, 1.5);
	TestEqual(TEXT("as wide"), Stretch.Ky, 1.0);
	TestEqual(TEXT("as tall"), Stretch.Kz, 1.0);
	TestTrue(TEXT("it is stretched"), Stretch.IsStretched());
	TestFalse(TEXT("1.5x is inside the band"), Stretch.bClampedX);
	const TArray<FString> Notes = FMantleVehicleChassisFit::StretchNotes(Stretch);
	if (TestEqual(TEXT("one stretch note"), Notes.Num(), 1))
	{
		TestTrue(TEXT("the note says the chassis was stretched to the wheels"), Notes[0].StartsWith(TEXT("The chassis was stretched to your model's wheels (1.50x the length")));
	}

	const FMantleChassisPlacement Placed = FMantleVehicleChassisFit::Place(Chassis, Stretch);
	TestTrue(TEXT("the box is 1.5x longer, its height and width unchanged"), Near(Placed.CollisionExtent, FVector(480.0, 102.5, 48.0)));
	TestTrue(TEXT("the box's underside is the chassis's clearance above the ground"), FMath::IsNearlyEqual(Placed.CollisionCenter.Z - Placed.CollisionExtent.Z, 48.0));
	TestTrue(TEXT("the seat is 1.5x further forward and as high"), Near(Placed.Seat, FVector(109.8, -42.0, 130.0)));
	TestTrue(TEXT("the centre of mass is still mid-wheelbase"), Near(Placed.CenterOfMass, FVector(0.0, 0.0, 61.0)));

	// Past the band: held to it, and the creator is told.
	const FMantleChassisLayout Huge = FMantleVehicleChassisFit::LayoutFromWheels(FourWheels(875.0, -875.0, 168.0, 0.0, 42.0, 42.0));
	const FMantleChassisStretch Held = FMantleVehicleChassisFit::Stretch(Chassis, Huge, 1.0);
	TestEqual(TEXT("a 5x wheelbase is held to the largest stretch"), Held.Kx, FMantleVehicleChassisFit::MaxStretch);
	TestTrue(TEXT("and says it was held"), Held.bClampedX);
	TestEqual(TEXT("so there is a note for the stretch and one for the limit"), FMantleVehicleChassisFit::StretchNotes(Held).Num(), 2);
	const FMantleChassisLayout Tiny = FMantleVehicleChassisFit::LayoutFromWheels(FourWheels(35.0, -35.0, 168.0, 0.0, 42.0, 42.0));
	TestEqual(TEXT("a tenth of the wheelbase is held to the smallest stretch"), FMantleVehicleChassisFit::Stretch(Chassis, Tiny, 1.0).Kx, FMantleVehicleChassisFit::MinStretch);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleChassisFitOriginTest,
	"Mantle.Vehicle.ChassisFit.Origin",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleChassisFitOriginTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleChassisFitTestPrivate;
	const FMantleVehicleChassisReference Chassis = TruckChassis();

	// A model whose origin is not at the axle midpoint: front axle at 300, rear at 20 (middle 160), wheel centres 50 up with radius 42
	// (so the ground is at 8), the track centred 10 across. The chassis is laid on that middle, that ground and that centre line.
	const FMantleChassisLayout Layout = FMantleVehicleChassisFit::LayoutFromWheels(FourWheels(300.0, 20.0, 168.0, 10.0, 50.0, 42.0));
	const FMantleChassisStretch Stretch = FMantleVehicleChassisFit::Stretch(Chassis, Layout, 1.0);
	TestTrue(TEXT("the origin is the axle middle, the ground, the track centre"), Near(Stretch.Origin, FVector(160.0, 10.0, 8.0)));
	TestEqual(TEXT("the wheelbase is 280 against the chassis's 350"), Stretch.Kx, 0.8);

	const FMantleChassisPlacement Placed = FMantleVehicleChassisFit::Place(Chassis, Stretch);
	TestTrue(TEXT("the box is centred on the axle middle and track centre"), FMath::IsNearlyEqual(Placed.CollisionCenter.X, 160.0) && FMath::IsNearlyEqual(Placed.CollisionCenter.Y, 10.0));
	TestTrue(TEXT("the box's underside is the clearance above the model's ground"), FMath::IsNearlyEqual(Placed.CollisionCenter.Z - Placed.CollisionExtent.Z, 8.0 + 48.0));
	TestTrue(TEXT("the centre of mass is over the axle middle, above the wheel centres"), FMath::IsNearlyEqual(Placed.CenterOfMass.X, 160.0) && FMath::IsNearlyEqual(Placed.CenterOfMass.Y, 10.0) && Placed.CenterOfMass.Z > 50.0);
	TestTrue(TEXT("the seat is the chassis's, stretched about the origin"), Near(Placed.Seat, FVector(160.0 + 73.2 * 0.8, 10.0 - 42.0, 8.0 + 130.0)));
	TestTrue(TEXT("a wheel's chassis position lands on the model's wheel"), Near(Stretch.PointToModel(FVector(0.5 * Chassis.Wheelbase, 0.5 * Chassis.Track, Chassis.WheelRadius)), FVector(300.0, 10.0 + 84.0, 50.0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleChassisFitSizeTest,
	"Mantle.Vehicle.ChassisFit.Size",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleChassisFitSizeTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleChassisFitTestPrivate;
	const FMantleVehicleChassisReference Chassis = TruckChassis();

	// A model modelled at half the chassis's scale, with Size 2: scaled up it is the chassis, so nothing stretches, and every value
	// written is the chassis's divided by 2 (the export multiplies it by Size again).
	const FMantleChassisLayout Layout = FMantleVehicleChassisFit::LayoutFromWheels(FourWheels(87.5, -87.5, 84.0, 0.0, 21.0, 21.0));
	const FMantleChassisStretch Stretch = FMantleVehicleChassisFit::Stretch(Chassis, Layout, 2.0);
	TestEqual(TEXT("at Size 2 length is not stretched"), Stretch.Kx, 1.0);
	TestEqual(TEXT("at Size 2 width is not stretched"), Stretch.Ky, 1.0);
	TestEqual(TEXT("at Size 2 height is not stretched"), Stretch.Kz, 1.0);
	const FMantleChassisPlacement Placed = FMantleVehicleChassisFit::Place(Chassis, Stretch);
	TestTrue(TEXT("the box is half the chassis's, in model units"), Near(Placed.CollisionExtent, FVector(160.0, 51.25, 24.0)));
	TestTrue(TEXT("the centre of mass is half the chassis's"), Near(Placed.CenterOfMass, FVector(0.0, 0.0, 30.5)));
	TestTrue(TEXT("the seat is half the chassis's"), Near(Placed.Seat, FVector(36.6, -21.0, 65.0)));
	TestTrue(TEXT("the steering wheel's hub is half the chassis's"), Near(Placed.SteeringWheelHub, FVector(60.0, -21.0, 66.5)));
	TestEqual(TEXT("the steering wheel's radius is half the chassis's"), Placed.SteeringWheelRadius, 10.5);

	// A size of 0 (not set) counts as 1.
	TestEqual(TEXT("no Size is Size 1"), FMantleVehicleChassisFit::Stretch(Chassis, Layout, 0.0).Size, 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleChassisFitApplyTest,
	"Mantle.Vehicle.ChassisFit.Apply",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleChassisFitApplyTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleChassisFitTestPrivate;
	const FMantleVehicleChassisReference Chassis = TruckChassis();
	const FMantleChassisLayout Layout = FMantleVehicleChassisFit::LayoutFromWheels(FourWheels(175.0, -175.0, 168.0, 0.0, 42.0, 42.0));
	const FMantleChassisPlacement Placed = FMantleVehicleChassisFit::Place(Chassis, FMantleVehicleChassisFit::Stretch(Chassis, Layout, 1.0));

	// A vehicle the converter filled from the model's physics: three boxes, a centre of mass underground, no passenger seat.
	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
	Source->Tuning.Collision.AddDefaulted(3);
	Source->Tuning.bOverrideCenterOfMass = true;
	Source->Tuning.CenterOfMass = FVector(0.0, 0.0, -110.0);
	Source->Tuning.bPassengerSeat = false;
	Source->Tuning.bHideDriver = true;
	Source->Tuning.Mass = 1234.f;

	FMantleVehicleChassisFit::Apply(Placed, *Source);
	const FMantleVehicleSourceTuning& Tuning = Source->Tuning;
	if (TestEqual(TEXT("exactly one collision box"), Tuning.Collision.Num(), 1))
	{
		TestTrue(TEXT("the box is the chassis's"), Near(Tuning.Collision[0].Center, Placed.CollisionCenter) && Near(Tuning.Collision[0].Extent, Placed.CollisionExtent));
	}
	TestTrue(TEXT("the centre of mass is overridden with the chassis's"), Tuning.bOverrideCenterOfMass && Near(Tuning.CenterOfMass, Placed.CenterOfMass));
	TestTrue(TEXT("the driver's seat is placed"), Tuning.bSeatLocation && Near(Tuning.SeatLocation, Placed.Seat));
	TestTrue(TEXT("the steering wheel is placed"), Tuning.bSteeringWheel && Near(Tuning.SteeringWheelLocation, Placed.SteeringWheelHub)
		&& Tuning.SteeringWheelRadius == 21.f && Tuning.SteeringWheelRotation.Pitch == -20.0);
	TestTrue(TEXT("the passenger's seat is placed"), Tuning.bPassengerSeat && Near(Tuning.PassengerSeatLocation, Placed.PassengerSeat));
	TestTrue(TEXT("each seat has contact points again"), Tuning.DriverContacts.HasAny() && Tuning.PassengerContacts.HasAny());
	TestTrue(TEXT("hidden riders are kept"), Tuning.bHideDriver);
	TestEqual(TEXT("the other tuning is untouched"), Tuning.Mass, 1234.f);
	return true;
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleChassisFitTestPrivate
{
	/** A grid of points filling the box Min..Max (every few units), a stand-in for a body's vertices. */
	void FillBox(TArray<FVector>& Points, const FVector& Min, const FVector& Max)
	{
		for (double X = Min.X; X <= Max.X; X += 10.0)
		{
			for (double Y = Min.Y; Y <= Max.Y; Y += 10.0)
			{
				for (double Z = Min.Z; Z <= Max.Z; Z += 10.0)
				{
					Points.Add(FVector(X, Y, Z));
				}
			}
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleChassisFitBodyBoxesTest,
	"Mantle.Vehicle.ChassisFit.BodyBoxes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleChassisFitBodyBoxesTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleChassisFitTestPrivate;

	// The tub: 640 long, 205 wide, from 48 to 144 up (the Truck chassis's body box).
	const FVector TubCenter(0.0, 0.0, 96.0);
	const FVector TubExtent(320.0, 102.5, 48.0);
	const double TubTop = 144.0;

	// A sedan: a low body (inside the tub) and one cabin from 60 behind the middle to 160 ahead, roof 66 over the tub.
	{
		TArray<FVector> Points;
		FillBox(Points, FVector(-300.0, -95.0, 50.0), FVector(300.0, 95.0, 140.0));
		FillBox(Points, FVector(-60.0, -85.0, 150.0), FVector(160.0, 85.0, 210.0));
		const TArray<FMantleChassisBox> Boxes = FMantleVehicleChassisFit::FitBodyBoxes(Points, TubCenter, TubExtent);
		if (TestEqual(TEXT("sedan: one cabin box"), Boxes.Num(), 1))
		{
			TestTrue(TEXT("sedan: it starts at the tub's top, never below"), FMath::IsNearlyEqual(Boxes[0].Center.Z - Boxes[0].Extent.Z, TubTop, 1e-6));
			TestTrue(TEXT("sedan: its roof is the body's roof"), FMath::IsNearlyEqual(Boxes[0].Center.Z + Boxes[0].Extent.Z, 210.0, 1.0));
			TestTrue(TEXT("sedan: it covers the cabin's length"), Boxes[0].Center.X - Boxes[0].Extent.X <= -50.0 && Boxes[0].Center.X + Boxes[0].Extent.X >= 150.0);
		}
	}

	// A cab-over truck: a tall cab at the front, a low box behind it.
	{
		TArray<FVector> Points;
		FillBox(Points, FVector(-300.0, -95.0, 50.0), FVector(300.0, 95.0, 140.0));
		FillBox(Points, FVector(100.0, -90.0, 150.0), FVector(300.0, 90.0, 280.0));
		FillBox(Points, FVector(-300.0, -90.0, 150.0), FVector(90.0, 90.0, 190.0));
		const TArray<FMantleChassisBox> Boxes = FMantleVehicleChassisFit::FitBodyBoxes(Points, TubCenter, TubExtent);
		if (TestEqual(TEXT("cab-over: a cab box and a body box"), Boxes.Num(), 2))
		{
			const FMantleChassisBox& Cab = Boxes[0].Center.X > Boxes[1].Center.X ? Boxes[0] : Boxes[1];
			const FMantleChassisBox& Box = Boxes[0].Center.X > Boxes[1].Center.X ? Boxes[1] : Boxes[0];
			TestTrue(TEXT("cab-over: the cab is tall"), FMath::IsNearlyEqual(Cab.Center.Z + Cab.Extent.Z, 280.0, 1.0));
			TestTrue(TEXT("cab-over: the rear is low, not a second cab"), FMath::IsNearlyEqual(Box.Center.Z + Box.Extent.Z, 190.0, 1.0));
			TestTrue(TEXT("cab-over: both start at the tub's top"), FMath::IsNearlyEqual(Cab.Center.Z - Cab.Extent.Z, TubTop, 1e-6) && FMath::IsNearlyEqual(Box.Center.Z - Box.Extent.Z, TubTop, 1e-6));
		}
	}

	// A pickup: a cab over the front half, only a low bed rail (6 over the tub) behind it.
	{
		TArray<FVector> Points;
		FillBox(Points, FVector(-300.0, -95.0, 50.0), FVector(300.0, 95.0, 140.0));
		FillBox(Points, FVector(0.0, -85.0, 150.0), FVector(150.0, 85.0, 220.0));
		FillBox(Points, FVector(-300.0, -95.0, 150.0), FVector(-10.0, 95.0, 150.0));
		const TArray<FMantleChassisBox> Boxes = FMantleVehicleChassisFit::FitBodyBoxes(Points, TubCenter, TubExtent);
		if (TestEqual(TEXT("pickup: a cab box and a bed box"), Boxes.Num(), 2))
		{
			const double Low = FMath::Min(Boxes[0].Center.Z + Boxes[0].Extent.Z, Boxes[1].Center.Z + Boxes[1].Extent.Z);
			const double High = FMath::Max(Boxes[0].Center.Z + Boxes[0].Extent.Z, Boxes[1].Center.Z + Boxes[1].Extent.Z);
			TestTrue(TEXT("pickup: the bed is not as tall as the cab"), High - Low > 40.0);
		}
	}

	// Nothing above the tub (a low roadster), too few points, and no points: no boxes.
	{
		TArray<FVector> Low;
		FillBox(Low, FVector(-300.0, -95.0, 50.0), FVector(300.0, 95.0, 140.0));
		TestEqual(TEXT("a body inside the tub adds no box"), FMantleVehicleChassisFit::FitBodyBoxes(Low, TubCenter, TubExtent).Num(), 0);
		TArray<FVector> Aerial = { FVector(0.0, 0.0, 300.0), FVector(1.0, 0.0, 301.0) };
		TestEqual(TEXT("a couple of stray points add no box"), FMantleVehicleChassisFit::FitBodyBoxes(Aerial, TubCenter, TubExtent).Num(), 0);
		TestEqual(TEXT("no points add no box"), FMantleVehicleChassisFit::FitBodyBoxes(TConstArrayView<FVector>(), TubCenter, TubExtent).Num(), 0);
	}

	// Every box is inside the document's ranges.
	{
		TArray<FVector> Points;
		FillBox(Points, FVector(-300.0, -95.0, 150.0), FVector(300.0, 95.0, 400.0));
		for (const FMantleChassisBox& Box : FMantleVehicleChassisFit::FitBodyBoxes(Points, TubCenter, TubExtent))
		{
			TestTrue(TEXT("a box's extent is in 1 to 1000"), Box.Extent.GetMin() >= 1.0 && Box.Extent.GetMax() <= 1000.0);
			TestTrue(TEXT("a box's centre is in -1000 to 1000"), Box.Center.GetAbsMax() <= 1000.0);
		}
	}

	// Apply writes the tub then its body boxes, capped at the document's boxes.
	{
		UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
		FMantleChassisPlacement Placed;
		Placed.CollisionCenter = TubCenter;
		Placed.CollisionExtent = TubExtent;
		for (int32 Index = 0; Index < 5; ++Index)
		{
			FMantleChassisBox& Box = Placed.BodyBoxes.AddDefaulted_GetRef();
			Box.Center = FVector(0.0, 0.0, 180.0);
			Box.Extent = FVector(50.0, 50.0, 30.0);
		}
		FMantleVehicleChassisFit::Apply(Placed, *Source);
		TestEqual(TEXT("the tub and at most two body boxes"), Source->Tuning.Collision.Num(), 1 + FMantleVehicleChassisFit::MaxBodyBoxes);
		TestTrue(TEXT("the first is the tub"), Source->Tuning.Collision.Num() > 0 && Near(Source->Tuning.Collision[0].Center, TubCenter));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleChassisFitClampToBodyTest,
	"Mantle.Vehicle.ChassisFit.ClampToBody",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleChassisFitClampToBodyTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleChassisFitTestPrivate;

	// A campervan: its body is 400 long and 180 wide, but the chassis tub, stretched by the wheelbase, is 640 long and 205 wide.
	TArray<FVector> Points;
	FillBox(Points, FVector(-200.0, -90.0, 50.0), FVector(200.0, 90.0, 240.0));

	FVector2D Min;
	FVector2D Max;
	if (TestTrue(TEXT("the body's extents are read"), FMantleVehicleChassisFit::BodyExtentXY(Points, Min, Max)))
	{
		TestTrue(TEXT("its length"), FMath::IsNearlyEqual(Min.X, -200.0, 1e-6) && FMath::IsNearlyEqual(Max.X, 200.0, 1e-6));
		TestTrue(TEXT("its width"), FMath::IsNearlyEqual(Min.Y, -90.0, 1e-6) && FMath::IsNearlyEqual(Max.Y, 90.0, 1e-6));
	}
	TestFalse(TEXT("too few points have no extents"), FMantleVehicleChassisFit::BodyExtentXY(TConstArrayView<FVector>(), Min, Max));

	FMantleChassisPlacement Placement;
	Placement.CollisionCenter = FVector(15.0, 0.0, 96.0);
	Placement.CollisionExtent = FVector(320.0, 102.5, 48.0);
	TestTrue(TEXT("a tub longer than the body is trimmed"), FMantleVehicleChassisFit::ClampTubToBody(Placement, Points));
	const FVector TubMin = Placement.CollisionCenter - Placement.CollisionExtent;
	const FVector TubMax = Placement.CollisionCenter + Placement.CollisionExtent;
	TestTrue(TEXT("the tub ends where the body ends, front and rear"), FMath::IsNearlyEqual(TubMin.X, -200.0, 1e-6) && FMath::IsNearlyEqual(TubMax.X, 200.0, 1e-6));
	TestTrue(TEXT("and where it ends at the sides"), FMath::IsNearlyEqual(TubMin.Y, -90.0, 1e-6) && FMath::IsNearlyEqual(TubMax.Y, 90.0, 1e-6));
	TestTrue(TEXT("its height is the chassis's"), FMath::IsNearlyEqual(Placement.CollisionCenter.Z, 96.0) && FMath::IsNearlyEqual(Placement.CollisionExtent.Z, 48.0));
	TestFalse(TEXT("a tub already inside the body is left alone"), FMantleVehicleChassisFit::ClampTubToBody(Placement, Points));

	// A tub shorter than the body is never grown.
	FMantleChassisPlacement Short;
	Short.CollisionCenter = FVector(0.0, 0.0, 96.0);
	Short.CollisionExtent = FVector(150.0, 80.0, 48.0);
	TestFalse(TEXT("a tub shorter than the body is not changed"), FMantleVehicleChassisFit::ClampTubToBody(Short, Points));
	TestTrue(TEXT("it keeps its size"), FMath::IsNearlyEqual(Short.CollisionExtent.X, 150.0) && FMath::IsNearlyEqual(Short.CollisionExtent.Y, 80.0));

	// A tub with no overlap with the body, or no points to measure, is left as it is.
	FMantleChassisBox Far;
	Far.Center = FVector(900.0, 0.0, 96.0);
	Far.Extent = FVector(100.0, 50.0, 48.0);
	TestFalse(TEXT("a box clear of the body is left alone"), FMantleVehicleChassisFit::ClampBoxToBodyXY(Far, FVector2D(-200.0, -90.0), FVector2D(200.0, 90.0)));
	FMantleChassisPlacement NoBody;
	NoBody.CollisionExtent = FVector(320.0, 100.0, 48.0);
	TestFalse(TEXT("no body points leaves the tub alone"), FMantleVehicleChassisFit::ClampTubToBody(NoBody, TConstArrayView<FVector>()));

	// The boxes above the tub stay inside the body's length and width too, whatever the points above the tub reach.
	{
		TArray<FVector> Body;
		FillBox(Body, FVector(-200.0, -90.0, 50.0), FVector(200.0, 90.0, 140.0));
		FillBox(Body, FVector(-190.0, -88.0, 150.0), FVector(190.0, 88.0, 230.0));
		// A few stray points reach past the body's range at the top: trimmed, so the boxes do not follow them.
		Body.Add(FVector(300.0, 0.0, 240.0));
		Body.Add(FVector(-300.0, 0.0, 240.0));
		FVector2D BodyMin;
		FVector2D BodyMax;
		FMantleVehicleChassisFit::BodyExtentXY(Body, BodyMin, BodyMax);
		for (const FMantleChassisBox& Box : FMantleVehicleChassisFit::FitBodyBoxes(Body, FVector(0.0, 0.0, 96.0), FVector(200.0, 90.0, 48.0)))
		{
			TestTrue(TEXT("a body box lies inside the body's length"), Box.Center.X - Box.Extent.X >= BodyMin.X - 1e-6 && Box.Center.X + Box.Extent.X <= BodyMax.X + 1e-6);
			TestTrue(TEXT("a body box lies inside the body's width"), Box.Center.Y - Box.Extent.Y >= BodyMin.Y - 1e-6 && Box.Center.Y + Box.Extent.Y <= BodyMax.Y + 1e-6);
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
