// Copyright Joshua Eger. All Rights Reserved.
#include "MantleChassisSpec.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehicleChassisFit.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleSource.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

// Vehicles with many seats (#449): the seats after the driver's and the passenger's, as the converter writes them into the vehicle document (extraSeats) and
// reads them back, as the check holds them, and as the chassis fit puts a preset's onto a creator's model.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleManySeatsTestPrivate
{
	/** A transient car with a driver, a passenger, one collision box and both rear doors, and no extra seats. */
	UMantleVehicleSource* MakeCar()
	{
		UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
		FMantleVehicleSourceTuning& T = Source->Tuning;
		T.bSeatLocation = true;
		T.SeatLocation = FVector(0.0, -36.0, 76.0);
		T.bPassengerSeat = true;
		T.PassengerSeatLocation = FVector(0.0, 36.0, 76.0);
		FMantleVehicleSourceBox& Box = T.Collision.AddDefaulted_GetRef();
		Box.Center = FVector(0.0, 0.0, 80.0);
		Box.Extent = FVector(220.0, 90.0, 50.0);
		T.bRearLeftDoor = true;
		T.RearLeftDoor.Bone = TEXT("Door_RL");
		T.RearLeftDoor.Pivot = FVector(-40.0, -88.0, 80.0);
		T.RearLeftDoorStand = FVector(-80.0, -160.0, 0.0);
		T.RearLeftDoorYaw = 90.f;
		T.bRearRightDoor = true;
		T.RearRightDoor.Bone = TEXT("Door_RR");
		T.RearRightDoor.Pivot = FVector(-40.0, 88.0, 80.0);
		T.RearRightDoor.Axis = FVector(0.0, 0.0, -1.0);
		T.RearRightDoorStand = FVector(-80.0, 160.0, 0.0);
		T.RearRightDoorYaw = -90.f;
		return Source;
	}

	/** Adds the three seats the round trip uses: a rear left sit seat by its door, a rear-facing middle sit seat that is hidden and has its own arc, and a stand spot with a grip. */
	void AddThreeSeats(UMantleVehicleSource& Source)
	{
		FMantleVehicleSourceTuning& T = Source.Tuning;
		FMantleVehicleSourceSeat& Left = T.ExtraSeats.AddDefaulted_GetRef();
		Left.Location = FVector(-85.0, -36.0, 76.0);
		Left.Door = EMantleVehicleSourceSeatDoor::RearLeftDoor;
		FMantleVehicleSourceSeat& Facing = T.ExtraSeats.AddDefaulted_GetRef();
		Facing.Location = FVector(-85.0, 0.0, 76.0);
		Facing.Yaw = 180.f;
		Facing.Door = EMantleVehicleSourceSeatDoor::RearRightDoor;
		Facing.bHidden = true;
		Facing.WeaponArc.bEnabled = true;
		Facing.WeaponArc.YawMin = -60.f;
		Facing.WeaponArc.YawMax = 60.f;
		Facing.WeaponArc.PitchMin = -20.f;
		Facing.WeaponArc.PitchMax = 30.f;
		FMantleVehicleSourceSeat& Standing = T.ExtraSeats.AddDefaulted_GetRef();
		Standing.Location = FVector(-150.0, 0.0, 40.0);
		Standing.Yaw = 90.f;
		Standing.Pose = EMantleVehicleSourceSeatPose::Stand;
		Standing.bHold = true;
		Standing.Hold = FVector(-150.0, 10.0, 180.0);
		for (FMantleVehicleSourceSeat& Extra : T.ExtraSeats)
		{
			Extra.Contacts = FMantleVehicleConverter::GuessExtraSeatContacts(T, Extra);
		}
	}

	/** A document as condensed JSON text, to compare two for being identical. */
	FString Text(const TSharedRef<FJsonObject>& Doc)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Doc, Writer);
		return Out;
	}

	bool ParseDocumentText(const FString& Json, FMantleVehicleDesc& Out, FString& OutError)
	{
		const FTCHARToUTF8 Utf8(*Json);
		return FMantleVehicleAssetReader::ParseDocument(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), Out, OutError);
	}

	/** Whether any issue's problem says Words. */
	bool Says(const TArray<FMantleValidationIssue>& Issues, const FString& Words)
	{
		return Issues.ContainsByPredicate([&Words](const FMantleValidationIssue& Issue) { return Issue.Problem.Contains(Words); });
	}

	bool Near(const FVector& A, const FVector& B)
	{
		return A.Equals(B, 0.01);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleManySeatsDocumentTest,
	"Mantle.Vehicle.ManySeats.Document",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleManySeatsDocumentTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleManySeatsTestPrivate;
	const TMap<FString, FString> NoMaterials;
	UMantleVehicleSource* Source = MakeCar();
	AddThreeSeats(*Source);

	const TSharedRef<FJsonObject> Doc = FMantleVehicleConverter::MakeDocument(*Source, NoMaterials);
	const TArray<TSharedPtr<FJsonValue>>* Seats = nullptr;
	if (!TestTrue(TEXT("the document has three extraSeats"), Doc->TryGetArrayField(TEXT("extraSeats"), Seats) && Seats && Seats->Num() == 3))
	{
		return false;
	}
	const TSharedPtr<FJsonObject> Left = (*Seats)[0]->AsObject();
	const TSharedPtr<FJsonObject> Facing = (*Seats)[1]->AsObject();
	const TSharedPtr<FJsonObject> Standing = (*Seats)[2]->AsObject();
	TestEqual(TEXT("the first is a sit seat"), Left->GetStringField(TEXT("pose")), FString(TEXT("sit")));
	TestEqual(TEXT("by the rear left door"), Left->GetStringField(TEXT("door")), FString(TEXT("rearLeftDoor")));
	TestFalse(TEXT("a seat that is not hidden writes no hidden"), Left->HasField(TEXT("hidden")));
	TestFalse(TEXT("a seat without its own arc writes no weaponArc"), Left->HasField(TEXT("weaponArc")));
	TestTrue(TEXT("a sit seat writes its pelvis and feet"), Left->HasField(TEXT("pelvis")) && Left->HasField(TEXT("feet")) && Left->HasField(TEXT("exits")));
	TestFalse(TEXT("an extra seat never writes hands"), Left->HasField(TEXT("hands")) || Facing->HasField(TEXT("hands")) || Standing->HasField(TEXT("hands")));
	TestFalse(TEXT("a sit seat writes no hold"), Left->HasField(TEXT("hold")));
	TestTrue(TEXT("a hidden seat writes hidden"), Facing->GetBoolField(TEXT("hidden")));
	TestTrue(TEXT("a seat with its own arc writes it"), Facing->HasField(TEXT("weaponArc")));
	TestEqual(TEXT("the stand spot is one"), Standing->GetStringField(TEXT("pose")), FString(TEXT("stand")));
	TestFalse(TEXT("an open seat writes no door"), Standing->HasField(TEXT("door")));
	TestFalse(TEXT("a stand spot writes no pelvis or feet"), Standing->HasField(TEXT("pelvis")) || Standing->HasField(TEXT("feet")));
	TestTrue(TEXT("a stand spot writes its exits and its hold"), Standing->HasField(TEXT("exits")) && Standing->HasField(TEXT("hold")));

	// Read back: the seats over a complete document (the sedan chassis's), with the rear doors they name.
	FMantleChassisSpec Sedan;
	FString Error;
	if (!TestTrue(TEXT("the sedan chassis loads"), FMantleVehicleChassisBuilder::LoadSpec(TEXT("sedan"), Sedan, Error)))
	{
		AddError(Error);
		return false;
	}
	TSharedPtr<FJsonObject> Base;
	if (!TestTrue(TEXT("its document is JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FMantleChassisSpec::ComposeDocument(Sedan)), Base) && Base.IsValid()))
	{
		return false;
	}
	for (const TCHAR* Key : { TEXT("extraSeats"), TEXT("rearLeftDoor"), TEXT("rearRightDoor") })
	{
		Base->SetField(Key, Doc->TryGetField(Key));
	}
	FMantleVehicleDesc Desc;
	if (!TestTrue(*FString::Printf(TEXT("the document with the seats reads (%s)"), *Error), ParseDocumentText(Text(Base.ToSharedRef()), Desc, Error))
		|| !TestEqual(TEXT("three extra seats are read"), Desc.ExtraSeats.Num(), 3))
	{
		return false;
	}
	TestTrue(TEXT("the first is where it was"), Near(Desc.ExtraSeats[0].Location, FVector(-85.0, -36.0, 76.0)));
	TestTrue(TEXT("by the rear left door"), Desc.ExtraSeats[0].Door == EMantleVehicleSeatDoor::RearLeft);
	TestTrue(TEXT("and sits"), Desc.ExtraSeats[0].Pose == EMantleVehicleSeatPose::Sit);
	TestTrue(TEXT("with its pelvis and feet"), Desc.ExtraSeats[0].Contacts.bPelvis && Desc.ExtraSeats[0].Contacts.bFeet);
	TestEqual(TEXT("the second faces the rear"), FMath::Abs(Desc.ExtraSeats[1].Yaw), 180.0, 0.001);
	TestTrue(TEXT("by the rear right door"), Desc.ExtraSeats[1].Door == EMantleVehicleSeatDoor::RearRight);
	TestTrue(TEXT("hidden"), Desc.ExtraSeats[1].bHidden);
	TestTrue(TEXT("with its own arc"), Desc.ExtraSeats[1].WeaponArc.bSet && FMath::IsNearlyEqual(Desc.ExtraSeats[1].WeaponArc.YawMin, -60.0)
		&& FMath::IsNearlyEqual(Desc.ExtraSeats[1].WeaponArc.PitchMax, 30.0));
	TestTrue(TEXT("its feet are ahead of the way it faces (behind the seat)"), Desc.ExtraSeats[1].Contacts.bFeet && Desc.ExtraSeats[1].Contacts.LeftFoot.Location.X < -85.0);
	TestTrue(TEXT("the third stands"), Desc.ExtraSeats[2].Pose == EMantleVehicleSeatPose::Stand);
	TestTrue(TEXT("in the open"), Desc.ExtraSeats[2].Door == EMantleVehicleSeatDoor::None);
	TestTrue(TEXT("holding its grip"), Desc.ExtraSeats[2].bHold && Near(Desc.ExtraSeats[2].Hold, FVector(-150.0, 10.0, 180.0)));
	TestTrue(TEXT("with exits and no pelvis or feet"), Desc.ExtraSeats[2].Contacts.Exits.Num() == 2 && !Desc.ExtraSeats[2].Contacts.bPelvis
		&& !Desc.ExtraSeats[2].Contacts.bFeet);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleManySeatsUnchangedTest,
	"Mantle.Vehicle.ManySeats.UnchangedWithout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleManySeatsUnchangedTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleManySeatsTestPrivate;
	const TMap<FString, FString> NoMaterials;

	// A vehicle without extra seats writes no extraSeats key at all (the document holds 1 to 126 of them, never an empty list).
	const TSharedRef<FJsonObject> Without = FMantleVehicleConverter::MakeDocument(*MakeCar(), NoMaterials);
	TestFalse(TEXT("no extra seats writes no extraSeats"), Without->HasField(TEXT("extraSeats")));

	// And everything else is byte for byte what it was: the same car with seats, its extraSeats taken out, is the document without them.
	UMantleVehicleSource* With = MakeCar();
	AddThreeSeats(*With);
	const TSharedRef<FJsonObject> Seated = FMantleVehicleConverter::MakeDocument(*With, NoMaterials);
	TestTrue(TEXT("the seats are written"), Seated->HasField(TEXT("extraSeats")));
	Seated->RemoveField(TEXT("extraSeats"));
	TestEqual(TEXT("the rest of the document is identical"), Text(Seated), Text(Without));

	// The driver's and the passenger's guesses are the ones they always were: facing ahead, the driver's hands on the lap without a steering wheel.
	const FMantleVehicleSourceSeatContacts Passenger = FMantleVehicleConverter::GuessSeatContacts(With->Tuning, /*bPassenger*/ true);
	TestTrue(TEXT("the passenger's pelvis is under the seat, a little ahead"), Near(Passenger.Pelvis.Location, FVector(4.3, 36.0, 42.0)));
	TestTrue(TEXT("the passenger's hands are on the lap"), Passenger.Hands == EMantleVehicleHandsKind::Fixed);
	TestTrue(TEXT("the passenger's pelvis is not turned"), Passenger.Pelvis.Rotation.IsZero());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleManySeatsCheckTest,
	"Mantle.Vehicle.ManySeats.Check",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleManySeatsCheckTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleManySeatsTestPrivate;

	// Seats by the doors the car has: nothing to say about them.
	{
		UMantleVehicleSource* Source = MakeCar();
		AddThreeSeats(*Source);
		TArray<FMantleValidationIssue> Issues;
		FMantleVehicleConverter::CheckSource(Source, FString(), Issues);
		TestFalse(TEXT("seats by the car's own doors are fine"), Says(Issues, TEXT("Extra Seat")) || Says(Issues, TEXT("extra seats")));
	}

	// 127 is one more than the document holds: said, never trimmed without a word.
	{
		UMantleVehicleSource* Source = MakeCar();
		for (int32 Index = 0; Index < FMantleVehicleLimits::MaxExtraSeats + 1; ++Index)
		{
			FMantleVehicleSourceSeat& Extra = Source->Tuning.ExtraSeats.AddDefaulted_GetRef();
			Extra.Location = FVector(-50.0 - Index, 0.0, 76.0);
		}
		TArray<FMantleValidationIssue> Issues;
		FMantleVehicleConverter::CheckSource(Source, FString(), Issues);
		TestTrue(TEXT("127 extra seats are refused"), Says(Issues, TEXT("has 127 extra seats")));
		const TArray<TSharedPtr<FJsonValue>>* Written = nullptr;
		TestTrue(TEXT("and the document keeps every one, so the cook refuses it too"), FMantleVehicleConverter::MakeDocument(*Source, TMap<FString, FString>())
			->TryGetArrayField(TEXT("extraSeats"), Written) && Written && Written->Num() == 127);

		Source->Tuning.ExtraSeats.SetNum(FMantleVehicleLimits::MaxExtraSeats);
		Issues.Reset();
		FMantleVehicleConverter::CheckSource(Source, FString(), Issues);
		TestFalse(TEXT("126 are fine"), Says(Issues, TEXT("extra seats;")));
	}

	// A door the car doesn't have, hands, and a stand spot's pelvis: each said.
	{
		UMantleVehicleSource* Source = MakeCar();
		Source->Tuning.bRearRightDoor = false;
		FMantleVehicleSourceSeat& ByDoor = Source->Tuning.ExtraSeats.AddDefaulted_GetRef();
		ByDoor.Location = FVector(-85.0, 36.0, 76.0);
		ByDoor.Door = EMantleVehicleSourceSeatDoor::RearRightDoor;
		FMantleVehicleSourceSeat& ByDriverDoor = Source->Tuning.ExtraSeats.AddDefaulted_GetRef();
		ByDriverDoor.Location = FVector(-85.0, -36.0, 76.0);
		ByDriverDoor.Door = EMantleVehicleSourceSeatDoor::DriverDoor;
		FMantleVehicleSourceSeat& Holding = Source->Tuning.ExtraSeats.AddDefaulted_GetRef();
		Holding.Location = FVector(-85.0, 0.0, 76.0);
		Holding.Contacts.Hands = EMantleVehicleHandsKind::Fixed;
		FMantleVehicleSourceSeat& Standing = Source->Tuning.ExtraSeats.AddDefaulted_GetRef();
		Standing.Location = FVector(-150.0, 0.0, 40.0);
		Standing.Pose = EMantleVehicleSourceSeatPose::Stand;
		Standing.Contacts.bPelvis = true;
		Standing.Contacts.Pelvis.Location = Standing.Location;
		TArray<FMantleValidationIssue> Issues;
		FMantleVehicleConverter::CheckSource(Source, FString(), Issues);
		TestTrue(TEXT("a rear right door the car doesn't have is said"), Says(Issues, TEXT("Extra Seat 1 got into through the rear right door")));
		TestTrue(TEXT("a driver door the car doesn't have is said"), Says(Issues, TEXT("Extra Seat 2 got into through the driver door")));
		TestTrue(TEXT("hands on an extra seat are said"), Says(Issues, TEXT("hands set for Extra Seat 3")));
		TestTrue(TEXT("a stand spot's pelvis is said"), Says(Issues, TEXT("pelvis or feet for Extra Seat 4")));

		// The build's door rule: a seat by a door the vehicle did not get is an open seat.
		TestEqual(TEXT("the two seats by missing doors are opened"), FMantleVehicleChassisFit::KeepSeatDoors(Source->Tuning), 2);
		TestTrue(TEXT("both are open now"), Source->Tuning.ExtraSeats[0].Door == EMantleVehicleSourceSeatDoor::None
			&& Source->Tuning.ExtraSeats[1].Door == EMantleVehicleSourceSeatDoor::None);
	}

	// Never on a rider's machine.
	{
		UMantleVehicleSource* Source = MakeCar();
		Source->DriverSeatKind = EMantleVehicleSeatKind::Motorcycle;
		Source->Tuning.ExtraSeats.AddDefaulted_GetRef().Location = FVector(-85.0, 0.0, 76.0);
		TArray<FMantleValidationIssue> Issues;
		FMantleVehicleConverter::CheckSource(Source, FString(), Issues);
		TestTrue(TEXT("extra seats on a motorcycle are said"), Says(Issues, TEXT("ridden like a motorcycle")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleManySeatsChassisFitTest,
	"Mantle.Vehicle.ManySeats.ChassisFit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleManySeatsChassisFitTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleManySeatsTestPrivate;

	// The van preset's 13 seats are read with the rest of its chassis.
	{
		FString Text;
		FString Error;
		FMantleVehicleChassisReference Van;
		if (TestTrue(*FString::Printf(TEXT("the van chassis reads (%s)"), *Error),
			FMantleVehicleChassisBuilder::LoadPresetText(TEXT("van"), Text, Error) && FMantleVehicleChassisBuilder::ReadReference(Text, Van, Error)))
		{
			TestEqual(TEXT("the van has 13 extra seats"), Van.ExtraSeats.Num(), 13);
		}
	}

	// A chassis with a rear left sit seat and a stand spot with a grip, on a model 0.8 times as long and 1.5 times as tall: each seat goes where the body went.
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
	Chassis.bPassenger = true;
	FMantleChassisExtraSeat& Rear = Chassis.ExtraSeats.AddDefaulted_GetRef();
	Rear.Location = FVector(-20.0, -42.0, 130.0);
	Rear.Door = TEXT("rearLeftDoor");
	FMantleChassisExtraSeat& Stand = Chassis.ExtraSeats.AddDefaulted_GetRef();
	Stand.Location = FVector(-150.0, 0.0, 60.0);
	Stand.Yaw = 90.0;
	Stand.Pose = TEXT("stand");
	Stand.bHold = true;
	Stand.Hold = FVector(-150.0, 10.0, 200.0);

	TArray<FMantleVehicleFitWheel> Wheels;
	for (const double X : { 300.0, 20.0 })
	{
		for (const double Y : { -74.0, 94.0 })
		{
			FMantleVehicleFitWheel& Wheel = Wheels.AddDefaulted_GetRef();
			Wheel.Center = FVector(X, Y, 63.0);
			Wheel.Radius = 63.0;
		}
	}
	const FMantleChassisLayout Layout = FMantleVehicleChassisFit::LayoutFromWheels(Wheels);
	const FMantleChassisStretch Stretch = FMantleVehicleChassisFit::Stretch(Chassis, Layout, 1.0);
	const FMantleChassisPlacement Placed = FMantleVehicleChassisFit::Place(Chassis, Stretch);
	if (!TestEqual(TEXT("both extra seats are placed"), Placed.ExtraSeats.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("the sit seat is stretched like the driver's"), Near(Placed.ExtraSeats[0].Location, Stretch.PointToModel(Rear.Location)));
	TestTrue(TEXT("and moved with the body (its offset from the driver's seat stretched)"),
		Near(Placed.ExtraSeats[0].Location - Placed.Seat, FVector((Rear.Location.X - Chassis.Seat.X) * Stretch.Kx, 0.0, 0.0)));
	TestTrue(TEXT("the stand spot is stretched"), Near(Placed.ExtraSeats[1].Location, Stretch.PointToModel(Stand.Location)));
	TestTrue(TEXT("and its grip with it"), Near(Placed.ExtraSeats[1].Hold, Stretch.PointToModel(Stand.Hold)));
	TestEqual(TEXT("each keeps the way it faces"), Placed.ExtraSeats[1].Yaw, 90.0);

	// Written into the vehicle, each with its pose, its door for now, and a first guess of where its body goes (no hands; a stand spot's exits only).
	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage());
	FMantleVehicleChassisFit::Apply(Placed, *Source);
	const TArray<FMantleVehicleSourceSeat>& Extras = Source->Tuning.ExtraSeats;
	if (!TestEqual(TEXT("both are written into the vehicle"), Extras.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("the sit seat sits where it was placed"), Extras[0].Pose == EMantleVehicleSourceSeatPose::Sit && Near(Extras[0].Location, Placed.ExtraSeats[0].Location));
	TestTrue(TEXT("by the chassis's door, until the vehicle's doors are known"), Extras[0].Door == EMantleVehicleSourceSeatDoor::RearLeftDoor);
	TestTrue(TEXT("with a pelvis and feet and no hands"), Extras[0].Contacts.bPelvis && Extras[0].Contacts.bFeet && Extras[0].Contacts.Hands == EMantleVehicleHandsKind::None);
	TestTrue(TEXT("the stand spot stands, holding its grip"), Extras[1].Pose == EMantleVehicleSourceSeatPose::Stand && Extras[1].bHold
		&& Near(Extras[1].Hold, Placed.ExtraSeats[1].Hold));
	TestTrue(TEXT("with exits only"), !Extras[1].Contacts.bPelvis && !Extras[1].Contacts.bFeet && Extras[1].Contacts.Exits.Num() == 2);
	TestEqual(TEXT("facing the way the chassis has it"), Extras[1].Yaw, 90.f);

	// The model's doors: the rear left door is kept only when the model gets it.
	FMantleVehicleSourceTuning Doored = Source->Tuning;
	Doored.bRearLeftDoor = true;
	TestEqual(TEXT("with the model's rear left door nothing changes"), FMantleVehicleChassisFit::KeepSeatDoors(Doored), 0);
	TestTrue(TEXT("and the seat keeps it"), Doored.ExtraSeats[0].Door == EMantleVehicleSourceSeatDoor::RearLeftDoor);
	TestEqual(TEXT("without it the seat becomes an open one"), FMantleVehicleChassisFit::KeepSeatDoors(Source->Tuning), 1);
	TestTrue(TEXT("with no door"), Source->Tuning.ExtraSeats[0].Door == EMantleVehicleSourceSeatDoor::None);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
