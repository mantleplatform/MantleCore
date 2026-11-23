// Copyright Joshua Eger. All Rights Reserved.
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

#endif
