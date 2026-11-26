// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleBuilder.h"
#include "MantleVehicleChassisSkin.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleSource.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The converter's doors, hood and trunk (#546): the hinges a converted Fab car opens with, found from its bones' names. The models here are bones and boxes made in the
// test, as arrays (nothing is loaded), shaped like the Fab packs' skeletons: SUV04 and SUV01 (B_SUV04_FLDoor ... B_SUV04_RearDoor), Sedan03 (B_Door_FL ...) and the
// NYC 1950 car (B_NYC_Car02_DoorL, DoorR).

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleConverterHingesTestPrivate
{
	struct FConvHingeBone
	{
		const TCHAR* Name;
		int32 Parent;
		FVector Position;
	};

	/** A model of the bones, with no vertices yet; the first is the root. */
	FMantleVehicleModel BonesOnly(const TArray<FConvHingeBone>& Bones)
	{
		FMantleVehicleModel Model;
		Model.Name = TEXT("Test");
		Model.bSkeletal = true;
		for (const FConvHingeBone& Bone : Bones)
		{
			Model.Bones.Add({ FName(Bone.Name), Bone.Parent });
			Model.BonePositions.Add(Bone.Position);
		}
		Model.LOD.NumInfluences = 1;
		return Model;
	}

	/** A box of eight vertices on Bone. */
	void AddBox(FMantleVehicleModel& Model, const FVector& Min, const FVector& Max, const int32 Bone)
	{
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			Model.LOD.Positions.Add(FVector3f((Corner & 1) ? Max.X : Min.X, (Corner & 2) ? Max.Y : Min.Y, (Corner & 4) ? Max.Z : Min.Z));
			Model.LOD.InfluenceBones.Add(static_cast<uint16>(Bone));
			Model.LOD.InfluenceWeights.Add(255);
		}
	}

	/** The geometry of SUV04's doors, nose on +X: the left side is -Y. Doors are 80 cm long and 10 thick, hinged at the front, with each bone at its door's front edge. */
	const FBox DoorFLBox(FVector(30, -96, 70), FVector(110, -86, 130));
	const FBox DoorFRBox(FVector(30, 86, 70), FVector(110, 96, 130));
	const FBox DoorRLBox(FVector(-70, -96, 70), FVector(10, -86, 130));
	const FBox DoorRRBox(FVector(-70, 86, 70), FVector(10, 96, 130));
	const FBox TailgateBox(FVector(-225, -70, 30), FVector(-215, 70, 120));

	/** SUV04's skeleton: the body at the root, four doors and a tailgate under it, a steering wheel and four wheels with their brakes. */
	TArray<FConvHingeBone> Suv04Bones()
	{
		return {
			{ TEXT("B_SUV04_Body"), INDEX_NONE, FVector::ZeroVector },
			{ TEXT("B_SUV04_FLDoor"), 0, FVector(105, -95, 100) },
			{ TEXT("B_SUV04_FRDoor"), 0, FVector(105, 95, 100) },
			{ TEXT("B_SUV04_RLDoor"), 0, FVector(5, -95, 100) },
			{ TEXT("B_SUV04_RRDoor"), 0, FVector(5, 95, 100) },
			{ TEXT("B_SUV04_RearDoor"), 0, FVector(-215, 0, 120) },
			{ TEXT("B_SUV04_SW"), 0, FVector(70, -40, 110) },
			{ TEXT("B_SUV04_FLWheel"), 0, FVector(130, -80, 35) },
			{ TEXT("B_SUV04_FRWheel"), 0, FVector(130, 80, 35) },
			{ TEXT("B_SUV04_RLWheel"), 0, FVector(-130, -80, 35) },
			{ TEXT("B_SUV04_RRWheel"), 0, FVector(-130, 80, 35) },
			{ TEXT("B_SUV04_FLBrake"), 7, FVector(130, -80, 35) },
		};
	}

	TArray<FName> Suv04Wheels()
	{
		return { FName(TEXT("B_SUV04_FLWheel")), FName(TEXT("B_SUV04_FRWheel")), FName(TEXT("B_SUV04_RLWheel")), FName(TEXT("B_SUV04_RRWheel")) };
	}

	/** The SUV with its doors in the model, as a joined body has them: each door's vertices weighted to its bone. */
	FMantleVehicleModel Suv04Joined()
	{
		FMantleVehicleModel Model = BonesOnly(Suv04Bones());
		AddBox(Model, FVector(-220, -90, 20), FVector(220, 90, 70), 0);
		AddBox(Model, DoorFLBox.Min, DoorFLBox.Max, 1);
		AddBox(Model, DoorFRBox.Min, DoorFRBox.Max, 2);
		AddBox(Model, DoorRLBox.Min, DoorRLBox.Max, 3);
		AddBox(Model, DoorRRBox.Min, DoorRRBox.Max, 4);
		AddBox(Model, TailgateBox.Min, TailgateBox.Max, 5);
		return Model;
	}

	UMantleVehicleSource* NewSource()
	{
		return NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	}

	bool AnyNote(const TArray<FString>& Notes, const TCHAR* Text)
	{
		return Notes.ContainsByPredicate([Text](const FString& Note) { return Note.Contains(Text); });
	}

	bool AnyOpens(const FMantleVehicleSourceTuning& T)
	{
		return T.bHasDriverDoor || T.bHasPassengerDoor || T.bRearLeftDoor || T.bRearRightDoor || T.bHood || T.bTrunk;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleConverterHingesFromNamesTest,
	"Mantle.Vehicle.Convert.HingesFromBoneNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleConverterHingesFromNamesTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleConverterHingesTestPrivate;

	// SUV04's bones, doors in the model: all five open, each on its own bone, with the hinge the builder's own numbers give.
	const FMantleVehicleModel Model = Suv04Joined();
	UMantleVehicleSource* Source = NewSource();
	Source->Tuning.bSeatLocation = true;
	Source->Tuning.bPassengerSeat = true;
	TArray<FString> Notes;
	FMantleVehicleConverter::AddHinges(Model, FMantleVehicleHingeEvidence(), Suv04Wheels(), *Source, Notes);
	const FMantleVehicleSourceTuning& T = Source->Tuning;
	TestTrue(TEXT("the four doors and the tailgate are on"), T.bHasDriverDoor && T.bHasPassengerDoor && T.bRearLeftDoor && T.bRearRightDoor && T.bTrunk);
	TestFalse(TEXT("the SUV has no hood bone, so no hood"), T.bHood);
	TestEqual(TEXT("the driver door is FLDoor"), T.DriverDoor.Bone.ToString(), FString(TEXT("B_SUV04_FLDoor")));
	TestEqual(TEXT("the passenger door is FRDoor"), T.PassengerDoor.Bone.ToString(), FString(TEXT("B_SUV04_FRDoor")));
	TestEqual(TEXT("the rear left door is RLDoor"), T.RearLeftDoor.Bone.ToString(), FString(TEXT("B_SUV04_RLDoor")));
	TestEqual(TEXT("the rear right door is RRDoor"), T.RearRightDoor.Bone.ToString(), FString(TEXT("B_SUV04_RRDoor")));
	TestEqual(TEXT("the trunk is RearDoor"), T.TrunkLid.Bone.ToString(), FString(TEXT("B_SUV04_RearDoor")));

	// A door turns about its bone, which sits at its front edge, on an upright axis: left (the driver's) one way, right the other.
	TestTrue(*FString::Printf(TEXT("the driver door's hinge is its bone (%s)"), *T.DriverDoor.Pivot.ToString()), T.DriverDoor.Pivot.Equals(FVector(105, -95, 100), 0.01));
	TestTrue(TEXT("the driver door turns about +Z"), T.DriverDoor.Axis.Equals(FVector(0, 0, 1)));
	TestTrue(TEXT("the passenger door turns about -Z"), T.PassengerDoor.Axis.Equals(FVector(0, 0, -1)));
	TestEqual(TEXT("a door turns at three quarters"), T.DriverDoor.Scale, 0.75f);
	// The rear doors say where someone stands to open them: 60 cm out from the body's side, facing the car.
	TestTrue(*FString::Printf(TEXT("the rear left door's stand is 60 cm out on the left (%s)"), *T.RearLeftDoorStand.ToString()), T.RearLeftDoorStand.Equals(FVector(-30, -156, 0), 0.01));
	TestEqual(TEXT("and faces the car"), T.RearLeftDoorYaw, 90.f);
	TestTrue(TEXT("the rear right door's is 60 cm out on the right"), T.RearRightDoorStand.Equals(FVector(-30, 156, 0), 0.01));
	TestEqual(TEXT("and faces the car from there"), T.RearRightDoorYaw, -90.f);
	// The tailgate turns about its top front edge on a sideways axis, and its stand is 60 cm behind the car.
	TestTrue(*FString::Printf(TEXT("the tailgate's hinge is its top edge (%s)"), *T.TrunkLid.Pivot.ToString()), T.TrunkLid.Pivot.Equals(FVector(-215, 0, 120), 0.01));
	TestTrue(TEXT("the tailgate turns about +Y"), T.TrunkLid.Axis.Equals(FVector(0, 1, 0)));
	TestTrue(*FString::Printf(TEXT("its stand is behind the car (%s)"), *T.TrunkStand.ToString()), T.TrunkStand.Equals(FVector(-285, 0, 0), 0.01));
	TestEqual(TEXT("facing forward"), T.TrunkYaw, 0.f);
	TestTrue(TEXT("the notes say what opens in one plain sentence"), AnyNote(Notes, TEXT("open in the game.")) && AnyNote(Notes, TEXT("driver door")) && AnyNote(Notes, TEXT("trunk")));

	// The converter's own check has nothing to say about them.
	TArray<FMantleValidationIssue> Issues;
	FMantleVehicleConverter::CheckSource(Source, FString(), Issues);
	TestFalse(TEXT("the check finds nothing wrong with the hinges"), Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Problem.Contains(TEXT("Door")) || Issue.Problem.Contains(TEXT("Trunk")) || Issue.Problem.Contains(TEXT("hinge"));
	}));

	// No seat placed, as a converted car comes: the front doors belong to their seats, so they stay shut and the notes say so; the rest open.
	UMantleVehicleSource* Seatless = NewSource();
	TArray<FString> SeatlessNotes;
	FMantleVehicleConverter::AddHinges(Model, FMantleVehicleHingeEvidence(), Suv04Wheels(), *Seatless, SeatlessNotes);
	TestFalse(TEXT("without a seat the driver door stays shut"), Seatless->Tuning.bHasDriverDoor);
	TestFalse(TEXT("and the passenger door"), Seatless->Tuning.bHasPassengerDoor);
	TestTrue(TEXT("the rear doors and the tailgate still open"), Seatless->Tuning.bRearLeftDoor && Seatless->Tuning.bRearRightDoor && Seatless->Tuning.bTrunk);
	TestTrue(TEXT("and the notes say why the front doors did not"), AnyNote(SeatlessNotes, TEXT("The driver door was left shut")) && AnyNote(SeatlessNotes, TEXT("The passenger door was left shut")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleConverterHingesFromPartsTest,
	"Mantle.Vehicle.Convert.HingesFromAttachedParts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleConverterHingesFromPartsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleConverterHingesTestPrivate;

	// A bare skeleton: its body is one box on the root, and each door is a separate mesh riding its bone (Source.Parts), so no vertex of the model is on a door bone.
	FMantleVehicleModel Model = BonesOnly(Suv04Bones());
	AddBox(Model, FVector(-220, -90, 20), FVector(220, 90, 70), 0);
	FMantleVehicleHingeEvidence Evidence;
	Evidence.Riding.Add(TPair<FName, FBox>(FName(TEXT("B_SUV04_FLDoor")), DoorFLBox));
	Evidence.Riding.Add(TPair<FName, FBox>(FName(TEXT("B_SUV04_FRDoor")), DoorFRBox));
	Evidence.Riding.Add(TPair<FName, FBox>(FName(TEXT("B_SUV04_RLDoor")), DoorRLBox));
	Evidence.Riding.Add(TPair<FName, FBox>(FName(TEXT("B_SUV04_RRDoor")), DoorRRBox));
	Evidence.Riding.Add(TPair<FName, FBox>(FName(TEXT("B_SUV04_RearDoor")), TailgateBox));

	UMantleVehicleSource* Source = NewSource();
	Source->Tuning.bSeatLocation = true;
	Source->Tuning.bPassengerSeat = true;
	TArray<FString> Notes;
	FMantleVehicleConverter::AddHinges(Model, Evidence, Suv04Wheels(), *Source, Notes);
	const FMantleVehicleSourceTuning& T = Source->Tuning;
	TestTrue(TEXT("the doors that are parts open too"), T.bHasDriverDoor && T.bHasPassengerDoor && T.bRearLeftDoor && T.bRearRightDoor && T.bTrunk);
	TestTrue(*FString::Printf(TEXT("the driver door's hinge is measured from its part (%s)"), *T.DriverDoor.Pivot.ToString()), T.DriverDoor.Pivot.Equals(FVector(105, -95, 100), 0.01));
	TestTrue(*FString::Printf(TEXT("the rear left door's stand is beside its part (%s)"), *T.RearLeftDoorStand.ToString()), T.RearLeftDoorStand.Equals(FVector(-30, -156, 0), 0.01));

	// Without the parts, the same bare skeleton has nothing on the door bones and says so; the car still converts and nothing is written for them.
	UMantleVehicleSource* Bare = NewSource();
	Bare->Tuning.bSeatLocation = true;
	TArray<FString> BareNotes;
	FMantleVehicleConverter::AddHinges(Model, FMantleVehicleHingeEvidence(), Suv04Wheels(), *Bare, BareNotes);
	TestFalse(TEXT("a door bone with nothing on it opens nothing"), AnyOpens(Bare->Tuning));
	TestTrue(TEXT("and the notes say so"), AnyNote(BareNotes, TEXT("will not open")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleConverterHingesKeepsOursTest,
	"Mantle.Vehicle.Convert.HingesKeepCreatorsOwn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleConverterHingesKeepsOursTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleConverterHingesTestPrivate;

	// The creator set the rear left door by hand: that one is kept as it is, and the others the car has are found.
	const FMantleVehicleModel Model = Suv04Joined();
	UMantleVehicleSource* Source = NewSource();
	Source->Tuning.bSeatLocation = true;
	Source->Tuning.bRearLeftDoor = true;
	Source->Tuning.RearLeftDoor.Bone = FName(TEXT("MyDoor"));
	Source->Tuning.RearLeftDoor.Pivot = FVector(1, 2, 3);
	Source->Tuning.RearLeftDoorYaw = 33.f;
	TArray<FString> Notes;
	FMantleVehicleConverter::AddHinges(Model, FMantleVehicleHingeEvidence(), Suv04Wheels(), *Source, Notes);
	const FMantleVehicleSourceTuning& T = Source->Tuning;
	TestEqual(TEXT("the creator's bone is kept"), T.RearLeftDoor.Bone.ToString(), FString(TEXT("MyDoor")));
	TestTrue(TEXT("and its pivot"), T.RearLeftDoor.Pivot.Equals(FVector(1, 2, 3)));
	TestEqual(TEXT("and its yaw"), T.RearLeftDoorYaw, 33.f);
	TestTrue(TEXT("the driver door, the rear right door and the tailgate are found beside it"), T.bHasDriverDoor && T.bRearRightDoor && T.bTrunk);
	TestFalse(TEXT("the passenger door has no seat to belong to"), T.bHasPassengerDoor);
	TestTrue(TEXT("the notes say the creator's one is kept"), AnyNote(Notes, TEXT("your 1 (rear left door) are kept")));

	// All six already set: nothing is looked for.
	UMantleVehicleSource* Full = NewSource();
	Full->Tuning.bHasDriverDoor = Full->Tuning.bHasPassengerDoor = Full->Tuning.bRearLeftDoor = Full->Tuning.bRearRightDoor = Full->Tuning.bHood = Full->Tuning.bTrunk = true;
	Full->Tuning.DriverDoor.Bone = FName(TEXT("Mine"));
	TArray<FString> FullNotes;
	FMantleVehicleConverter::AddHinges(Model, FMantleVehicleHingeEvidence(), Suv04Wheels(), *Full, FullNotes);
	TestEqual(TEXT("a hinge that is set is not touched"), Full->Tuning.DriverDoor.Bone.ToString(), FString(TEXT("Mine")));
	TestEqual(TEXT("and only the one note is said"), FullNotes.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleConverterHingesAfterSeatsTest,
	"Mantle.Vehicle.Convert.HingesFrontDoorsAfterSeats",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleConverterHingesAfterSeatsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleConverterHingesTestPrivate;

	// Convert a car that has no seat yet: the rear doors and the tailgate open, the front doors are left shut. No seat is made up.
	const FMantleVehicleModel Model = Suv04Joined();
	UMantleVehicleSource* Source = NewSource();
	TArray<FString> First;
	FMantleVehicleConverter::AddHinges(Model, FMantleVehicleHingeEvidence(), Suv04Wheels(), *Source, First);
	FMantleVehicleSourceTuning& T = Source->Tuning;
	TestTrue(TEXT("the rear doors and the tailgate open"), T.bRearLeftDoor && T.bRearRightDoor && T.bTrunk);
	TestFalse(TEXT("the front doors do not"), T.bHasDriverDoor || T.bHasPassengerDoor);
	TestFalse(TEXT("and no seat was made up"), T.bSeatLocation || T.bPassengerSeat);
	const FMantleVehicleSourceHinge RearLeft = T.RearLeftDoor;
	const FVector RearLeftStand = T.RearLeftDoorStand;
	const FMantleVehicleSourceHinge Trunk = T.TrunkLid;

	// The creator then sets the seats (and turns the rear right door's stand a little): converting again adds the front doors and leaves the rest as it was.
	T.bSeatLocation = true;
	T.bPassengerSeat = true;
	T.RearRightDoorYaw = 45.f;
	TArray<FString> Second;
	FMantleVehicleConverter::AddHinges(Model, FMantleVehicleHingeEvidence(), Suv04Wheels(), *Source, Second);
	TestTrue(TEXT("the front doors are there now"), T.bHasDriverDoor && T.bHasPassengerDoor);
	TestEqual(TEXT("the driver door is FLDoor"), T.DriverDoor.Bone.ToString(), FString(TEXT("B_SUV04_FLDoor")));
	TestEqual(TEXT("the passenger door is FRDoor"), T.PassengerDoor.Bone.ToString(), FString(TEXT("B_SUV04_FRDoor")));
	TestTrue(TEXT("the driver door's hinge is its bone"), T.DriverDoor.Pivot.Equals(FVector(105, -95, 100), 0.01));
	TestTrue(TEXT("the rear left door is as it was"), T.RearLeftDoor.Bone == RearLeft.Bone && T.RearLeftDoor.Pivot.Equals(RearLeft.Pivot) && T.RearLeftDoor.Axis.Equals(RearLeft.Axis)
		&& T.RearLeftDoorStand.Equals(RearLeftStand));
	TestTrue(TEXT("so is the tailgate"), T.TrunkLid.Bone == Trunk.Bone && T.TrunkLid.Pivot.Equals(Trunk.Pivot));
	TestEqual(TEXT("and the creator's change to the rear right door"), T.RearRightDoorYaw, 45.f);
	TestTrue(TEXT("the notes say what was kept and what opens now"), AnyNote(Second, TEXT("are kept")) && AnyNote(Second, TEXT("driver door and passenger door open in the game.")));

	// Once more: nothing left to find, nothing changes.
	const FVector Pivot = T.DriverDoor.Pivot;
	TArray<FString> Third;
	FMantleVehicleConverter::AddHinges(Model, FMantleVehicleHingeEvidence(), Suv04Wheels(), *Source, Third);
	TestTrue(TEXT("a third conversion changes nothing"), T.DriverDoor.Pivot.Equals(Pivot) && T.RearRightDoorYaw == 45.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleConverterHingesNoInteriorTest,
	"Mantle.Vehicle.Convert.HingesNoInteriorKeepsDoorsShut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleConverterHingesNoInteriorTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleConverterHingesTestPrivate;

	// A car with no interior hides its riders: it has seats, but none of its four doors swings. The hood and the tailgate still open.
	const FMantleVehicleModel Model = Suv04Joined();
	UMantleVehicleSource* Source = NewSource();
	Source->Tuning.bSeatLocation = true;
	Source->Tuning.bPassengerSeat = true;
	Source->Tuning.bHideDriver = true;
	Source->Tuning.bHidePassenger = true;
	TArray<FString> Notes;
	FMantleVehicleConverter::AddHinges(Model, FMantleVehicleHingeEvidence(), Suv04Wheels(), *Source, Notes);
	const FMantleVehicleSourceTuning& T = Source->Tuning;
	TestFalse(TEXT("all four doors stay empty"), T.bHasDriverDoor || T.bHasPassengerDoor || T.bRearLeftDoor || T.bRearRightDoor);
	TestTrue(TEXT("the tailgate is written"), T.bTrunk && T.TrunkLid.Bone == FName(TEXT("B_SUV04_RearDoor")));
	TestTrue(TEXT("the notes say why the doors stay shut"), AnyNote(Notes, TEXT("Doors stay shut: this vehicle hides its riders (no interior). The hood and trunk still open.")));
	TestTrue(TEXT("the seats and the hide settings are as they were"), T.bSeatLocation && T.bPassengerSeat && T.bHideDriver && T.bHidePassenger);

	// The hood is written as well when the car has one (a model with a Hood bone), while every door stays empty; either flag alone is enough.
	FMantleVehicleModel WithHood = Model;
	WithHood.Bones.Add({ FName(TEXT("Hood")), 0 });
	WithHood.BonePositions.Add(FVector(110, 0, 106));
	AddBox(WithHood, FVector(110, -70, 100), FVector(220, 70, 106), WithHood.Bones.Num() - 1);
	UMantleVehicleSource* Half = NewSource();
	Half->Tuning.bSeatLocation = true;
	Half->Tuning.bPassengerSeat = true;
	Half->Tuning.bHideDriver = true;
	TArray<FString> HalfNotes;
	FMantleVehicleConverter::AddHinges(WithHood, FMantleVehicleHingeEvidence(), Suv04Wheels(), *Half, HalfNotes);
	TestFalse(TEXT("with only the driver hidden no door opens either"), Half->Tuning.bHasDriverDoor || Half->Tuning.bHasPassengerDoor || Half->Tuning.bRearLeftDoor || Half->Tuning.bRearRightDoor);
	TestTrue(TEXT("and the hood and the tailgate do"), Half->Tuning.bHood && Half->Tuning.bTrunk);

	// A door hinge the creator set is kept all the same.
	UMantleVehicleSource* Own = NewSource();
	Own->Tuning.bSeatLocation = true;
	Own->Tuning.bHideDriver = true;
	Own->Tuning.bHasDriverDoor = true;
	Own->Tuning.DriverDoor.Bone = FName(TEXT("MyDoor"));
	TArray<FString> OwnNotes;
	FMantleVehicleConverter::AddHinges(Model, FMantleVehicleHingeEvidence(), Suv04Wheels(), *Own, OwnNotes);
	TestEqual(TEXT("the creator's driver door is kept"), Own->Tuning.DriverDoor.Bone.ToString(), FString(TEXT("MyDoor")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleConverterHingesNoDoorsTest,
	"Mantle.Vehicle.Convert.HingesNoneWhenNoDoorBones",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleConverterHingesNoDoorsTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleConverterHingesTestPrivate;

	// A car whose bones are the body and its wheels: nothing is written, and the notes say nothing opens and what to do about it. No error.
	FMantleVehicleModel Model = BonesOnly({
		{ TEXT("Body"), INDEX_NONE, FVector::ZeroVector },
		{ TEXT("Wheel_FL"), 0, FVector(130, -80, 35) },
		{ TEXT("Wheel_FR"), 0, FVector(130, 80, 35) },
		{ TEXT("Wheel_BL"), 0, FVector(-130, -80, 35) },
		{ TEXT("Wheel_BR"), 0, FVector(-130, 80, 35) },
		{ TEXT("Steering"), 0, FVector(70, -40, 110) } });
	AddBox(Model, FVector(-220, -90, 20), FVector(220, 90, 70), 0);
	UMantleVehicleSource* Source = NewSource();
	Source->Tuning.bSeatLocation = true;
	TArray<FString> Notes;
	const TArray<FName> Wheels = { FName(TEXT("Wheel_FL")), FName(TEXT("Wheel_FR")), FName(TEXT("Wheel_BL")), FName(TEXT("Wheel_BR")) };
	FMantleVehicleConverter::AddHinges(Model, FMantleVehicleHingeEvidence(), Wheels, *Source, Notes);
	TestFalse(TEXT("nothing opens"), AnyOpens(Source->Tuning));
	TestTrue(TEXT("and the notes say so"), AnyNote(Notes, TEXT("nothing opens")));

	// A model with no skeleton (a static mesh read as a model): nothing opens either.
	FMantleVehicleModel Static;
	Static.bSkeletal = false;
	UMantleVehicleSource* StaticSource = NewSource();
	TArray<FString> StaticNotes;
	FMantleVehicleConverter::AddHinges(Static, FMantleVehicleHingeEvidence(), TArray<FName>(), *StaticSource, StaticNotes);
	TestFalse(TEXT("a model with no skeleton opens nothing"), AnyOpens(StaticSource->Tuning));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleConverterHingesSkippedTest,
	"Mantle.Vehicle.Convert.HingesSkippedBonesAreSaid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleConverterHingesSkippedTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleConverterHingesTestPrivate;

	// A handle, a door with no side, a second bone for the same door, a door that a wheel hangs from, and a "left" door that sits on the right: each is left out, and said.
	FMantleVehicleModel Model = BonesOnly({
		{ TEXT("Body"), INDEX_NONE, FVector::ZeroVector },
		{ TEXT("Door_FL"), 0, FVector(105, -95, 100) },
		{ TEXT("Door_FL_Handle"), 1, FVector(90, -97, 95) },
		{ TEXT("Door"), 0, FVector(0, 0, 100) },
		{ TEXT("Door_Left_Front"), 0, FVector(105, -95, 100) },
		{ TEXT("Door_RL"), 0, FVector(5, -95, 100) },
		{ TEXT("Wheel_RL"), 5, FVector(-130, -80, 35) },
		{ TEXT("Door_FR"), 0, FVector(105, 95, 100) },
		{ TEXT("Hood"), 0, FVector(110, 0, 106) } });
	AddBox(Model, FVector(-220, -90, 20), FVector(220, 90, 70), 0);
	AddBox(Model, DoorFLBox.Min, DoorFLBox.Max, 1);
	// Door_FR was modeled on the left: its name says right, its box says left.
	AddBox(Model, FVector(30, -96, 70), FVector(110, -86, 130), 7);
	AddBox(Model, FVector(110, -70, 100), FVector(220, 70, 106), 8);
	UMantleVehicleSource* Source = NewSource();
	Source->Tuning.bSeatLocation = true;
	Source->Tuning.bPassengerSeat = true;
	TArray<FString> Notes;
	const TArray<FName> Wheels = { FName(TEXT("Wheel_RL")) };
	FMantleVehicleConverter::AddHinges(Model, FMantleVehicleHingeEvidence(), Wheels, *Source, Notes);
	const FMantleVehicleSourceTuning& T = Source->Tuning;
	TestEqual(TEXT("the driver door is Door_FL, the first of the two named like it"), T.DriverDoor.Bone.ToString(), FString(TEXT("Door_FL")));
	TestTrue(TEXT("the hood opens"), T.bHood && T.HoodLid.Bone == FName(TEXT("Hood")));
	TestFalse(TEXT("the door a wheel hangs from does not"), T.bRearLeftDoor);
	TestFalse(TEXT("nor the one on the wrong side"), T.bHasPassengerDoor);
	TestTrue(TEXT("the handle is said to be a piece of a door"), AnyNote(Notes, TEXT("Door_FL_Handle")) && AnyNote(Notes, TEXT("handle")));
	TestTrue(TEXT("the door with no side is said to have none"), AnyNote(Notes, TEXT("side (left or right)")));
	TestTrue(TEXT("the second front left door is said to be left alone"), AnyNote(Notes, TEXT("Door_Left_Front is also named like the driver door")));
	TestTrue(TEXT("the door with a wheel is said to have one"), AnyNote(Notes, TEXT("Door_RL is named like the rear left door, but a wheel")));
	TestTrue(TEXT("the door on the wrong side is said to be"), AnyNote(Notes, TEXT("Door_FR is named like the passenger door, but it sits at the left")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleConverterHingesPackNamesTest,
	"Mantle.Vehicle.Convert.HingesFabPackNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleConverterHingesPackNamesTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleConverterHingesTestPrivate;

	// The bones of the four Fab packs, as their skeletons have them, and the part each is taken for.
	const auto Names = [](const TArray<FString>& Bones)
	{
		FMantleVehicleModel Model = BonesOnly({ { TEXT("Root"), INDEX_NONE, FVector::ZeroVector } });
		for (const FString& Bone : Bones)
		{
			Model.Bones.Add({ FName(*Bone), 0 });
			Model.BonePositions.Add(FVector::ZeroVector);
		}
		return Model;
	};
	const auto Check = [this](const FMantleVehicleHingeMap& Map, const TCHAR* Pack, const EMantleVehicleHingePart Part, const TCHAR* Bone)
	{
		TestEqual(*FString::Printf(TEXT("%s: %s"), Pack, *FMantleVehicleChassisSkin::HingeLabel(Part).ToString()), Map.BoneOf(Part).ToString(), FString(Bone));
	};

	const FMantleVehicleHingeMap Suv04 = FMantleVehicleChassisSkin::SuggestHinges(Names({ TEXT("B_SUV04_FLDoor"), TEXT("B_SUV04_FRDoor"), TEXT("B_SUV04_RLDoor"), TEXT("B_SUV04_RRDoor"),
		TEXT("B_SUV04_RearDoor"), TEXT("B_SUV04_SW"), TEXT("B_SUV04_FLBrake"), TEXT("B_SUV04_FLWheel"), TEXT("B_SUV04_RRWheel") }));
	TestEqual(TEXT("SUV04 has five"), Suv04.Bones.Num(), 5);
	Check(Suv04, TEXT("SUV04"), EMantleVehicleHingePart::DriverDoor, TEXT("B_SUV04_FLDoor"));
	Check(Suv04, TEXT("SUV04"), EMantleVehicleHingePart::PassengerDoor, TEXT("B_SUV04_FRDoor"));
	Check(Suv04, TEXT("SUV04"), EMantleVehicleHingePart::RearLeftDoor, TEXT("B_SUV04_RLDoor"));
	Check(Suv04, TEXT("SUV04"), EMantleVehicleHingePart::RearRightDoor, TEXT("B_SUV04_RRDoor"));
	Check(Suv04, TEXT("SUV04"), EMantleVehicleHingePart::Trunk, TEXT("B_SUV04_RearDoor"));

	const FMantleVehicleHingeMap Suv01 = FMantleVehicleChassisSkin::SuggestHinges(Names({ TEXT("B_SUV01_FLDoor"), TEXT("B_SUV01_FRDoor"), TEXT("B_SUV01_RLDoor"), TEXT("B_SUV01_RRDoor"),
		TEXT("B_SUV01_RearDoor"), TEXT("B_SUV01_SW"), TEXT("B_SUV01_RLBrake") }));
	TestEqual(TEXT("SUV01 has five"), Suv01.Bones.Num(), 5);
	Check(Suv01, TEXT("SUV01"), EMantleVehicleHingePart::Trunk, TEXT("B_SUV01_RearDoor"));
	Check(Suv01, TEXT("SUV01"), EMantleVehicleHingePart::RearRightDoor, TEXT("B_SUV01_RRDoor"));

	const FMantleVehicleHingeMap Sedan = FMantleVehicleChassisSkin::SuggestHinges(Names({ TEXT("B_Door_FL"), TEXT("B_Door_FR"), TEXT("B_Door_RL"), TEXT("B_Door_RR"), TEXT("B_SteeringWheel"),
		TEXT("B_Wheel_FL"), TEXT("B_Brake_FL") }));
	TestEqual(TEXT("Sedan03 has four doors"), Sedan.Bones.Num(), 4);
	Check(Sedan, TEXT("Sedan03"), EMantleVehicleHingePart::DriverDoor, TEXT("B_Door_FL"));
	Check(Sedan, TEXT("Sedan03"), EMantleVehicleHingePart::RearRightDoor, TEXT("B_Door_RR"));

	// The two-door classic: DoorL is the driver's and DoorR the passenger's, both front doors.
	const FMantleVehicleHingeMap Nyc = FMantleVehicleChassisSkin::SuggestHinges(Names({ TEXT("B_NYC_Car02_DoorL"), TEXT("B_NYC_Car02_DoorR"), TEXT("B_NYC_Car02_SW"), TEXT("B_NYC_Car02_WheelFL"),
		TEXT("B_NYC_Car02_WiperL"), TEXT("B_NYC_Car02_WiperR") }));
	TestEqual(TEXT("the NYC car has two"), Nyc.Bones.Num(), 2);
	Check(Nyc, TEXT("NYC"), EMantleVehicleHingePart::DriverDoor, TEXT("B_NYC_Car02_DoorL"));
	Check(Nyc, TEXT("NYC"), EMantleVehicleHingePart::PassengerDoor, TEXT("B_NYC_Car02_DoorR"));

	// Other spellings the builder already took keep working with the words: GTA's, and a hood and a boot.
	const FMantleVehicleHingeMap Gta = FMantleVehicleChassisSkin::SuggestHinges(Names({ TEXT("door_dside_f"), TEXT("door_pside_r"), TEXT("bonnet"), TEXT("Boot"), TEXT("HoodLid") }));
	Check(Gta, TEXT("GTA"), EMantleVehicleHingePart::DriverDoor, TEXT("door_dside_f"));
	Check(Gta, TEXT("GTA"), EMantleVehicleHingePart::RearRightDoor, TEXT("door_pside_r"));
	Check(Gta, TEXT("GTA"), EMantleVehicleHingePart::Hood, TEXT("bonnet"));
	Check(Gta, TEXT("GTA"), EMantleVehicleHingePart::Trunk, TEXT("Boot"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleSteeringWheelBoneNameTest,
	"Mantle.Vehicle.Convert.SteeringWheelBoneName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleSteeringWheelBoneNameTest::RunTest(const FString& Parameters)
{
	// The names the real skeletons use: the Fab cars' B_<car>_SW, Sedan03, the Military pack, our FiveM rebuilds, and GTA V's steeringwheel (from GTA's known bone list, not a file).
	for (const TCHAR* Name : { TEXT("B_SUV01_SW"), TEXT("B_SUV03_SW"), TEXT("B_NYC_Car02_SW"), TEXT("b_suv01_sw"), TEXT("B_SteeringWheel"), TEXT("Steering_Wheel"), TEXT("Steering_wheel"),
		TEXT("steering"), TEXT("steering_wheel"), TEXT("steeringwheel") })
	{
		TestTrue(FString::Printf(TEXT("%s is a steering wheel"), Name), FMantleVehicleChassisSkin::IsSteeringWheelBoneName(Name));
	}
	// "Sw" inside a word or alone, other steering parts, and bones that have nothing to do with it.
	for (const TCHAR* Name : { TEXT("Sw"), TEXT("SW"), TEXT("Swing"), TEXT("swing_arm"), TEXT("B_Swivel"), TEXT("B_SUV01_Body"), TEXT("wheel_lf"), TEXT("B_SUV01_FLWheel"), TEXT("steering_column"),
		TEXT("steering_rack"), TEXT("B_SUV01_Swaybar") })
	{
		TestFalse(FString::Printf(TEXT("%s is not a steering wheel"), Name), FMantleVehicleChassisSkin::IsSteeringWheelBoneName(Name));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleSteeringWheelBoneFillTest,
	"Mantle.Vehicle.Convert.SteeringWheelBoneFill",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleSteeringWheelBoneFillTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleConverterHingesTestPrivate;

	// SUV04: B_SUV04_SW is found and written, with the note.
	const FMantleVehicleModel Suv = Suv04Joined();
	UMantleVehicleSource* Source = NewSource();
	Source->Tuning.bSeatLocation = true;
	Source->Tuning.SeatLocation = FVector(30, -40, 100);
	TArray<FString> Notes;
	TestTrue(TEXT("a bone was filled"), FMantleVehicleBuilder::ApplySteeringWheelBone(Suv, *Source, Notes));
	TestEqual(TEXT("it is the SW bone"), Source->Tuning.SteeringWheelBone.ToString(), FString(TEXT("B_SUV04_SW")));
	TestTrue(TEXT("the notes say so"), AnyNote(Notes, TEXT("Steering wheel: the bone B_SUV04_SW turns with the steering (found from its name). Change it under Driver > Steering Wheel Bone in the vehicle asset.")));

	// The creator's own bone is kept, and nothing is said.
	UMantleVehicleSource* Own = NewSource();
	Own->Tuning.SteeringWheelBone = FName(TEXT("MyWheel"));
	TArray<FString> OwnNotes;
	TestFalse(TEXT("a bone the creator set is not replaced"), FMantleVehicleBuilder::ApplySteeringWheelBone(Suv, *Own, OwnNotes));
	TestEqual(TEXT("it stays"), Own->Tuning.SteeringWheelBone.ToString(), FString(TEXT("MyWheel")));
	TestEqual(TEXT("with no note"), OwnNotes.Num(), 0);

	// A stray SW bone outside the cabin (ahead of the nose) loses to the one inside, though it comes first; and one alone outside is not taken.
	TArray<FConvHingeBone> Bones = Suv04Bones();
	Bones.Insert({ TEXT("B_Extra_SW"), 0, FVector(300, -40, 110) }, 1);
	FMantleVehicleModel Stray = BonesOnly(Bones);
	AddBox(Stray, FVector(-220, -90, 20), FVector(220, 90, 130), 0);
	TestEqual(TEXT("the one inside the cabin wins"), FMantleVehicleChassisSkin::SuggestSteeringWheelBone(Stray, nullptr, TArray<FName>()).ToString(), FString(TEXT("B_SUV04_SW")));
	Bones.RemoveAll([](const FConvHingeBone& Bone) { return FString(Bone.Name) == TEXT("B_SUV04_SW"); });
	FMantleVehicleModel Alone = BonesOnly(Bones);
	AddBox(Alone, FVector(-220, -90, 20), FVector(220, 90, 130), 0);
	TestTrue(TEXT("one outside the cabin alone is not taken"), FMantleVehicleChassisSkin::SuggestSteeringWheelBone(Alone, nullptr, TArray<FName>()).IsNone());

	// Behind the seat is not in front of it.
	const FVector Seat(100, -40, 100);
	TestTrue(TEXT("a wheel behind the driver's seat is not taken"), FMantleVehicleChassisSkin::SuggestSteeringWheelBone(Stray, &Seat, TArray<FName>()).IsNone());
	return true;
}


// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleSteeringWheelTestPrivate
{
	/** Count vertices of a wheel's rim on Bone: a ring of Radius about Center, its column pitched PitchDegrees (negative: forward and down), a little thick along it. */
	void AddRing(FMantleVehicleModel& Model, const FVector& Center, const double Radius, const double PitchDegrees, const int32 Bone, const int32 Count)
	{
		const FMatrix Frame = FRotationMatrix::MakeFromXZ(FRotator(PitchDegrees, 0.0, 0.0).Vector(), FVector::UpVector);
		const FVector Column = Frame.GetUnitAxis(EAxis::X);
		const FVector Side = Frame.GetUnitAxis(EAxis::Y);
		const FVector Top = Frame.GetUnitAxis(EAxis::Z);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const double Angle = 2.0 * PI * Index / Count;
			const FVector Point = Center + Radius * (FMath::Cos(Angle) * Side + FMath::Sin(Angle) * Top) + (Index % 2 ? 0.3 : -0.3) * Column;
			Model.LOD.Positions.Add(FVector3f(Point));
			Model.LOD.InfluenceBones.Add(static_cast<uint16>(Bone));
			Model.LOD.InfluenceWeights.Add(255);
		}
	}

	/** SUV04 with its steering wheel bone (the last of the skeleton's wheel-free bones is B_SUV04_SW, index 6) carrying Count vertices of a ring. */
	FMantleVehicleModel SuvWithWheel(const int32 Count, const double PitchDegrees)
	{
		using namespace MantleVehicleConverterHingesTestPrivate;
		FMantleVehicleModel Model = BonesOnly(Suv04Bones());
		AddBox(Model, FVector(-220, -90, 20), FVector(220, 90, 130), 0);
		AddRing(Model, FVector(70, -40, 110), 19.0, PitchDegrees, 6, Count);
		return Model;
	}

	UMantleVehicleSource* SeatedSource()
	{
		UMantleVehicleSource* Source = MantleVehicleConverterHingesTestPrivate::NewSource();
		Source->Tuning.bSeatLocation = true;
		Source->Tuning.SeatLocation = FVector(30, -40, 100);
		return Source;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleSteeringWheelPlaceTest,
	"Mantle.Vehicle.Convert.SteeringWheelPlaced",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleSteeringWheelPlaceTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleConverterHingesTestPrivate;
	using namespace MantleVehicleSteeringWheelTestPrivate;

	// A ring of 32 vertices on the bone: the wheel is placed at the bone, 19 cm across, its column forward and down (pitch -20).
	{
		const FMantleVehicleModel Model = SuvWithWheel(32, -20.0);
		UMantleVehicleSource* Source = SeatedSource();
		TArray<FString> Notes;
		TestTrue(TEXT("the source changed"), FMantleVehicleBuilder::ApplySteeringWheelBone(Model, *Source, Notes));
		const FMantleVehicleSourceTuning& T = Source->Tuning;
		TestEqual(TEXT("its bone is the SW bone"), T.SteeringWheelBone.ToString(), FString(TEXT("B_SUV04_SW")));
		TestTrue(TEXT("the wheel is on"), T.bSteeringWheel);
		TestTrue(TEXT("at the bone"), T.SteeringWheelLocation.Equals(FVector(70, -40, 110), 0.01));
		TestTrue(*FString::Printf(TEXT("19 cm across (%.2f)"), T.SteeringWheelRadius), FMath::IsNearlyEqual(T.SteeringWheelRadius, 19.f, 2.f));
		TestTrue(*FString::Printf(TEXT("pitched -20 (%.2f)"), T.SteeringWheelRotation.Pitch), FMath::IsNearlyEqual(T.SteeringWheelRotation.Pitch, -20.f, 2.f));
		TestTrue(TEXT("facing forward and not rolled"), FMath::Abs(T.SteeringWheelRotation.Yaw) < 2.f && FMath::Abs(T.SteeringWheelRotation.Roll) < 2.f);
		TestTrue(TEXT("the note says where it came from"), AnyNote(Notes, TEXT("Steering wheel: placed from the bone B_SUV04_SW (radius 19 cm, pitched -20")));
	}

	// Pitched the other way it reads +15: the sign follows the ring.
	{
		const FMantleVehicleModel Model = SuvWithWheel(32, 15.0);
		UMantleVehicleSource* Source = SeatedSource();
		TArray<FString> Notes;
		FMantleVehicleBuilder::ApplySteeringWheelBone(Model, *Source, Notes);
		TestTrue(*FString::Printf(TEXT("pitched +15 (%.2f)"), Source->Tuning.SteeringWheelRotation.Pitch), FMath::IsNearlyEqual(Source->Tuning.SteeringWheelRotation.Pitch, 15.f, 2.f));
	}

	// Too few vertices: the bone is set, nothing is placed, and the note says why.
	{
		const FMantleVehicleModel Model = SuvWithWheel(10, -20.0);
		UMantleVehicleSource* Source = SeatedSource();
		TArray<FString> Notes;
		FMantleVehicleBuilder::ApplySteeringWheelBone(Model, *Source, Notes);
		TestEqual(TEXT("the bone is still found"), Source->Tuning.SteeringWheelBone.ToString(), FString(TEXT("B_SUV04_SW")));
		TestFalse(TEXT("but no wheel is placed"), Source->Tuning.bSteeringWheel);
		TestTrue(TEXT("the note says why"), AnyNote(Notes, TEXT("Steering wheel: not placed, because only 10 of the model's vertices are weighted to it")));
	}

	// Not a ring: a blob of vertices (not flat) is not a wheel.
	{
		FMantleVehicleModel Model = BonesOnly(Suv04Bones());
		AddBox(Model, FVector(-220, -90, 20), FVector(220, 90, 130), 0);
		FRandomStream Random(7);
		for (int32 Index = 0; Index < 60; ++Index)
		{
			const FVector Point = FVector(70, -40, 110) + Random.GetUnitVector() * Random.FRandRange(5.0f, 19.0f);
			Model.LOD.Positions.Add(FVector3f(Point));
			Model.LOD.InfluenceBones.Add(6);
			Model.LOD.InfluenceWeights.Add(255);
		}
		UMantleVehicleSource* Source = SeatedSource();
		TArray<FString> Notes;
		FMantleVehicleBuilder::ApplySteeringWheelBone(Model, *Source, Notes);
		TestFalse(TEXT("a blob is not placed as a wheel"), Source->Tuning.bSteeringWheel);
		TestTrue(TEXT("and it is said"), AnyNote(Notes, TEXT("Steering wheel: not placed, because")));
	}

	// A ring too big for a wheel (60 cm across) is not placed.
	{
		FMantleVehicleModel Model = BonesOnly(Suv04Bones());
		AddBox(Model, FVector(-220, -90, 20), FVector(220, 90, 130), 0);
		AddRing(Model, FVector(70, -40, 110), 60.0, -20.0, 6, 32);
		UMantleVehicleSource* Source = SeatedSource();
		TArray<FString> Notes;
		FMantleVehicleBuilder::ApplySteeringWheelBone(Model, *Source, Notes);
		TestFalse(TEXT("a rim outside 10 to 30 cm is not placed"), Source->Tuning.bSteeringWheel);
	}

	// A detailed hub (an airbag and a row of buttons, as the FiveM rebuilds' wheels have): most of the vertices are the hub's, a wide bar across the middle. The
	// face is still round and the rim goes all the way round, so it is placed, at the rim's size (the FiveM BMW was refused as "long, not round").
	{
		FMantleVehicleModel Model = SuvWithWheel(32, -20.0);
		const FMatrix Frame = FRotationMatrix::MakeFromXZ(FRotator(-20.0, 0.0, 0.0).Vector(), FVector::UpVector);
		for (int32 Index = 0; Index < 160; ++Index)
		{
			const double Along = -12.0 + 24.0 * (Index / 2) / 79.0;
			const FVector Point = FVector(70, -40, 110) + Along * Frame.GetUnitAxis(EAxis::Y) + (Index % 2 ? 2.0 : -2.0) * Frame.GetUnitAxis(EAxis::Z);
			Model.LOD.Positions.Add(FVector3f(Point));
			Model.LOD.InfluenceBones.Add(6);
			Model.LOD.InfluenceWeights.Add(255);
		}
		UMantleVehicleSource* Source = SeatedSource();
		TArray<FString> Notes;
		FMantleVehicleBuilder::ApplySteeringWheelBone(Model, *Source, Notes);
		TestTrue(*FString::Printf(TEXT("a wheel with a detailed hub is placed (%s)"), *FString::Join(Notes, TEXT(" "))), Source->Tuning.bSteeringWheel);
		TestTrue(*FString::Printf(TEXT("at its rim's size, not the hub's (%.2f)"), Source->Tuning.SteeringWheelRadius), FMath::IsNearlyEqual(Source->Tuning.SteeringWheelRadius, 19.f, 2.f));
	}

	// Half a ring (an arc over the top, nothing below) is not a wheel: the rim does not go round.
	{
		FMantleVehicleModel Model = BonesOnly(Suv04Bones());
		AddBox(Model, FVector(-220, -90, 20), FVector(220, 90, 130), 0);
		const FMatrix Frame = FRotationMatrix::MakeFromXZ(FRotator(-20.0, 0.0, 0.0).Vector(), FVector::UpVector);
		for (int32 Index = 0; Index < 48; ++Index)
		{
			const double Angle = PI * Index / 47.0;
			const double Radial = Index % 3 == 0 ? 6.0 : 19.0;
			const FVector Point = FVector(70, -40, 110) + Radial * (FMath::Cos(Angle) * Frame.GetUnitAxis(EAxis::Y) + FMath::Sin(Angle) * Frame.GetUnitAxis(EAxis::Z))
				+ (Index % 2 ? 0.3 : -0.3) * Frame.GetUnitAxis(EAxis::X);
			Model.LOD.Positions.Add(FVector3f(Point));
			Model.LOD.InfluenceBones.Add(6);
			Model.LOD.InfluenceWeights.Add(255);
		}
		UMantleVehicleSource* Source = SeatedSource();
		TArray<FString> Notes;
		FMantleVehicleBuilder::ApplySteeringWheelBone(Model, *Source, Notes);
		TestFalse(TEXT("half a ring is not placed as a wheel"), Source->Tuning.bSteeringWheel);
	}

	// No driver seat yet: nothing is placed, and the note asks for the seat.
	{
		const FMantleVehicleModel Model = SuvWithWheel(32, -20.0);
		UMantleVehicleSource* Source = NewSource();
		TArray<FString> Notes;
		FMantleVehicleBuilder::ApplySteeringWheelBone(Model, *Source, Notes);
		TestFalse(TEXT("the wheel is part of the seat, so it waits for one"), Source->Tuning.bSteeringWheel);
		TestTrue(TEXT("the note says so"), AnyNote(Notes, TEXT("Set Seat Location and convert again")));
	}

	// The creator's own placement is kept, every number.
	{
		const FMantleVehicleModel Model = SuvWithWheel(32, -20.0);
		UMantleVehicleSource* Source = SeatedSource();
		Source->Tuning.bSteeringWheel = true;
		Source->Tuning.SteeringWheelLocation = FVector(1, 2, 3);
		Source->Tuning.SteeringWheelRadius = 12.f;
		Source->Tuning.SteeringWheelRotation = FRotator(-5, 0, 0);
		TArray<FString> Notes;
		FMantleVehicleBuilder::ApplySteeringWheelBone(Model, *Source, Notes);
		const FMantleVehicleSourceTuning& T = Source->Tuning;
		TestTrue(TEXT("their placement stays"), T.SteeringWheelLocation.Equals(FVector(1, 2, 3)) && T.SteeringWheelRadius == 12.f && T.SteeringWheelRotation.Equals(FRotator(-5, 0, 0)));
		TestFalse(TEXT("with no note about placing"), AnyNote(Notes, TEXT("placed from the bone")) || AnyNote(Notes, TEXT("not placed")));
	}
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleSideHingedTailgateTest,
	"Mantle.Vehicle.Hinges.SideHingedTailgateSwingsOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleSideHingedTailgateTest::RunTest(const FString& Parameters)
{
	using FSkin = FMantleVehicleChassisSkin;
	// The FiveM Jeep's gate (rerun11, from the converter, cm in the body's space: nose +X, driver -Y): the boot bone sits at the passenger-side edge, halfway up, so the
	// gate swings out on an upright axis like a door. It lifted about +Y and looked torn off (Jeep v6, car-prep\v6-pictures).
	const FBox Body(FVector(-260.0, -100.0, 0.0), FVector(230.0, 100.0, 200.0));
	const FBox Gate(FVector(-247.2, -58.3, 73.0), FVector(-184.2, 67.4, 179.7));
	const FVector RightBone(-199.6, 58.3, 115.1);
	// Where a hinge takes a point when the part opens by Degrees, as the game turns it (the same quaternion that swings a left door, +Z, out to -Y).
	const auto Opened = [](const FMantleVehicleSkinnedHinge& Hinge, const FVector& Point, const double Degrees)
	{
		return Hinge.Pivot + FQuat(Hinge.Axis.GetSafeNormal(), FMath::DegreesToRadians(Degrees)).RotateVector(Point - Hinge.Pivot);
	};
	const FMantleVehicleSkinnedHinge Right = FSkin::HingeFor(EMantleVehicleHingePart::Trunk, TEXT("boot"), Gate, Body, &RightBone, false);
	TestTrue(*FString::Printf(TEXT("hinged at the right edge, the gate turns about -Z (%s)"), *Right.Axis.ToString()), Right.Axis.Equals(FVector(0, 0, -1)));
	TestTrue(*FString::Printf(TEXT("through the bone (%s)"), *Right.Pivot.ToString()), Right.Pivot.Equals(RightBone, 0.01));
	TestTrue(*FString::Printf(TEXT("its stand stays behind the car (%s)"), *Right.Stand.ToString()), Right.Stand.X < Body.Min.X && FMath::IsNearlyZero(Right.Stand.Z));
	// The gate's far edge (the side away from the hinge) swings back, out of the car, not into it.
	const FVector FarEdge(Gate.Min.X, Gate.Min.Y, RightBone.Z);
	const FVector RightOpen = Opened(Right, FarEdge, 80.0);
	TestTrue(*FString::Printf(TEXT("its far edge swings back behind the car (%s)"), *RightOpen.ToString()), RightOpen.X < Gate.Min.X - 50.0);
	TestTrue(TEXT("and comes round to the hinge's side"), RightOpen.Y > FarEdge.Y + 50.0);

	// Mirrored: hinged at the left edge it turns about +Z and swings back the same way.
	const FVector LeftBone(-199.6, -58.3, 115.1);
	const FBox Mirrored(FVector(-247.2, -67.4, 73.0), FVector(-184.2, 58.3, 179.7));
	const FMantleVehicleSkinnedHinge Left = FSkin::HingeFor(EMantleVehicleHingePart::Trunk, TEXT("boot"), Mirrored, Body, &LeftBone, false);
	TestTrue(*FString::Printf(TEXT("hinged at the left edge, it turns about +Z (%s)"), *Left.Axis.ToString()), Left.Axis.Equals(FVector(0, 0, 1)));
	const FVector LeftOpen = Opened(Left, FVector(Mirrored.Min.X, Mirrored.Max.Y, LeftBone.Z), 80.0);
	TestTrue(*FString::Printf(TEXT("and its far edge swings back too (%s)"), *LeftOpen.ToString()), LeftOpen.X < Mirrored.Min.X - 50.0);

	// The door rule this follows: a left door's +Z takes its back edge out to the left.
	const FBox Door(FVector(0.0, -95.0, 40.0), FVector(110.0, -80.0, 120.0));
	const FMantleVehicleSkinnedHinge DriverDoor = FSkin::HingeFor(EMantleVehicleHingePart::DriverDoor, TEXT("door_dside_f"), Door, Body, nullptr, false);
	TestTrue(TEXT("(a left door swings out to -Y the same way)"), Opened(DriverDoor, FVector(Door.Min.X, Door.Min.Y, 80.0), 60.0).Y < Door.Min.Y - 50.0);

	// Everything else keeps the lift about +Y at the top front edge: a bone in the middle, a bone at the top (even at a side), and no bone at all.
	const FVector Lift(Gate.Max.X, Gate.GetCenter().Y, Gate.Max.Z);
	for (const FVector& Bone : { FVector(-199.6, 4.5, 175.0), FVector(-199.6, 58.3, 175.0), FVector(-199.6, 4.5, 115.1) })
	{
		const FMantleVehicleSkinnedHinge Kept = FSkin::HingeFor(EMantleVehicleHingePart::Trunk, TEXT("boot"), Gate, Body, &Bone, false);
		TestTrue(*FString::Printf(TEXT("a bone at %s keeps the lift about +Y (%s, %s)"), *Bone.ToString(), *Kept.Axis.ToString(), *Kept.Pivot.ToString()),
			Kept.Axis.Equals(FVector(0, 1, 0)) && Kept.Pivot.Equals(Lift, 0.01));
	}
	const FMantleVehicleSkinnedHinge NoBone = FSkin::HingeFor(EMantleVehicleHingePart::Trunk, TEXT("boot"), Gate, Body, nullptr, false);
	TestTrue(TEXT("no bone keeps the lift"), NoBone.Axis.Equals(FVector(0, 1, 0)) && NoBone.Pivot.Equals(Lift, 0.01));
	// Flipped still turns the axis the other way.
	TestTrue(TEXT("flipped turns the upright axis round"), FSkin::HingeFor(EMantleVehicleHingePart::Trunk, TEXT("boot"), Gate, Body, &RightBone, true).Axis.Equals(FVector(0, 0, 1)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleDropDownTailgateTest,
	"Mantle.Vehicle.Hinges.BottomHingedTailgateDropsDown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleDropDownTailgateTest::RunTest(const FString& Parameters)
{
	using FSkin = FMantleVehicleChassisSkin;
	// A pickup's gate, like the FiveM F-150's (GODz21F150: boot bone (-3.165, 0, 0.928) m, the gate's box 0.885 to 1.515 m high), in cm in the body's space (nose +X,
	// driver -Y): the boot bone sits at the gate's bottom edge, in the middle, so the gate drops down about the bone. It lifted from the top (Joshua's review).
	const FBox Body(FVector(-330.0, -100.0, 0.0), FVector(270.0, 100.0, 200.0));
	const FBox Gate(FVector(-320.0, -80.0, 88.5), FVector(-311.0, 80.0, 151.5));
	const FVector Bone(-316.5, 0.0, 92.8);
	const auto Opened = [](const FMantleVehicleSkinnedHinge& Hinge, const FVector& Point, const double Degrees)
	{
		return Hinge.Pivot + FQuat(Hinge.Axis.GetSafeNormal(), FMath::DegreesToRadians(Degrees)).RotateVector(Point - Hinge.Pivot);
	};
	const FMantleVehicleSkinnedHinge Drop = FSkin::HingeFor(EMantleVehicleHingePart::Trunk, TEXT("boot"), Gate, Body, &Bone, false);
	TestTrue(*FString::Printf(TEXT("a bone at the bottom edge turns the gate about -Y (%s)"), *Drop.Axis.ToString()), Drop.Axis.Equals(FVector(0, -1, 0)));
	TestTrue(*FString::Printf(TEXT("through the bone (%s)"), *Drop.Pivot.ToString()), Drop.Pivot.Equals(Bone, 0.01));
	TestTrue(*FString::Printf(TEXT("its stand stays behind the car (%s)"), *Drop.Stand.ToString()), Drop.Stand.X < Body.Min.X && FMath::IsNearlyZero(Drop.Stand.Z));
	// The gate's top edge swings back out of the bed and down, never forward into it.
	const FVector Top(Gate.Min.X, 0.0, Gate.Max.Z);
	const FVector Open = Opened(Drop, Top, 85.0);
	TestTrue(*FString::Printf(TEXT("its top swings back behind the gate (%s)"), *Open.ToString()), Open.X < Gate.Min.X - 40.0);
	TestTrue(*FString::Printf(TEXT("and down (%s)"), *Open.ToString()), Open.Z < Gate.Max.Z - 40.0);
	TestTrue(TEXT("flipped turns it about +Y"), FSkin::HingeFor(EMantleVehicleHingePart::Trunk, TEXT("boot"), Gate, Body, &Bone, true).Axis.Equals(FVector(0, 1, 0)));

	// The side rule comes first: a bone low at one side edge (a swing-out door, like the G63 limo's) still swings out on an upright axis.
	const FVector LowSide(-316.5, 78.0, 92.8);
	const FMantleVehicleSkinnedHinge Side = FSkin::HingeFor(EMantleVehicleHingePart::Trunk, TEXT("boot"), Gate, Body, &LowSide, false);
	TestTrue(*FString::Printf(TEXT("a low bone at a side edge still swings out about -Z (%s)"), *Side.Axis.ToString()), Side.Axis.Equals(FVector(0, 0, -1)));
	// A bone at the top, or in the middle of the gate's height, keeps the lift about +Y at the top front edge.
	const FVector Lift(Gate.Max.X, Gate.GetCenter().Y, Gate.Max.Z);
	for (const FVector& Kept : { FVector(-316.5, 0.0, 148.0), FVector(-316.5, 0.0, 120.0) })
	{
		const FMantleVehicleSkinnedHinge Hinge = FSkin::HingeFor(EMantleVehicleHingePart::Trunk, TEXT("boot"), Gate, Body, &Kept, false);
		TestTrue(*FString::Printf(TEXT("a bone at %s keeps the lift (%s, %s)"), *Kept.ToString(), *Hinge.Axis.ToString(), *Hinge.Pivot.ToString()),
			Hinge.Axis.Equals(FVector(0, 1, 0)) && Hinge.Pivot.Equals(Lift, 0.01));
	}
	// A flat lid (a sedan's boot, like the GTA test car's: 100 cm long, 6 cm high) keeps the lift with its bone at its bottom: it isn't an upright gate.
	const FBox Lid(FVector(-220.0, -70.0, 100.0), FVector(-120.0, 70.0, 106.0));
	const FVector LidBone(-120.0, 0.0, 100.0);
	const FMantleVehicleSkinnedHinge Flat = FSkin::HingeFor(EMantleVehicleHingePart::Trunk, TEXT("boot"), Lid, Body, &LidBone, false);
	TestTrue(*FString::Printf(TEXT("a flat lid keeps the lift about +Y (%s)"), *Flat.Axis.ToString()), Flat.Axis.Equals(FVector(0, 1, 0)));
	return true;
}

#endif
