// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleChassisBuilder.h"

#include "MantleChassisSpec.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The vehicle presets' bare chassis: every shipped preset makes a complete vehicle with no creator model.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleChassisPresetsMakeAssetsTest,
	"Mantle.Vehicle.Chassis.EveryPresetMakesAValidAsset",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleChassisPresetsMakeAssetsTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Ids = FMantleVehicleChassisBuilder::PresetIds();
	if (!TestTrue(TEXT("the plugin has vehicle presets"), Ids.Num() >= 8))
	{
		return false;
	}
	for (const FString& Id : Ids)
	{
		FString Text;
		FString Error;
		const bool bRead = FMantleVehicleChassisBuilder::LoadPresetText(Id, Text, Error);
		if (!TestTrue(FString::Printf(TEXT("%s: the preset reads (%s)"), *Id, *Error), bRead))
		{
			continue;
		}
		FMantleVehicleAsset Asset;
		const bool bMade = FMantleVehicleChassisBuilder::MakeAsset(Text, Asset, Error);
		if (!TestTrue(FString::Printf(TEXT("%s: it makes an asset (%s)"), *Id, *Error), bMade))
		{
			continue;
		}
		const FMantleVehicleDesc& Desc = Asset.Desc;
		// A plane has its own tests (Mantle.Vehicle.Chassis.PlaneHasSurfacesAndGear and the others): here it is only a valid asset, with its three wheels, a
		// steered one and a driven one, on bones, a body that validates and a centre of mass, and its fuselage, wing, tailplane and fin as collision.
		FMantleChassisSpec Spec;
		if (FMantleChassisSpec::Parse(Text, Spec, Error) && Spec.Aircraft.bSet)
		{
			TestEqual(*FString::Printf(TEXT("%s: three wheels"), *Id), Desc.Wheels.Num(), 3);
			TestEqual(*FString::Printf(TEXT("%s: and a position each"), *Id), Asset.WheelPositions.Num(), 3);
			TestEqual(*FString::Printf(TEXT("%s: four collision boxes"), *Id), Desc.Collision.Num(), 4);
			TestTrue(*FString::Printf(TEXT("%s: it has a centre of mass"), *Id), Desc.bCenterOfMass);
			TestTrue(*FString::Printf(TEXT("%s: it steers and its reader's driven wheel is there"), *Id),
				Desc.Wheels.ContainsByPredicate([](const FMantleVehicleWheelDesc& Wheel) { return Wheel.bSteering; })
				&& Desc.Wheels.ContainsByPredicate([](const FMantleVehicleWheelDesc& Wheel) { return Wheel.bDriven; }));
			FString PlaneWhy;
			TestTrue(*FString::Printf(TEXT("%s: the body validates (%s)"), *Id, *PlaneWhy), Asset.Body.Validate(PlaneWhy));
			continue;
		}
		// A helicopter has its own tests too (Mantle.Vehicle.Chassis.HelicopterBodyIsBlocksOnRotorBones and the others): here it is only a valid asset, with its
		// four skid wheels, a steered one and a driven one (which its definition clears), on bones, a body that validates and a centre of mass, and its cabin,
		// boom and fin as collision.
		if (Spec.Helicopter.bSet)
		{
			TestEqual(*FString::Printf(TEXT("%s: four skid wheels"), *Id), Desc.Wheels.Num(), 4);
			TestEqual(*FString::Printf(TEXT("%s: and a position each"), *Id), Asset.WheelPositions.Num(), 4);
			TestEqual(*FString::Printf(TEXT("%s: three collision boxes"), *Id), Desc.Collision.Num(), 3);
			TestTrue(*FString::Printf(TEXT("%s: it has a centre of mass"), *Id), Desc.bCenterOfMass);
			TestTrue(*FString::Printf(TEXT("%s: its reader's steered and driven wheels are there"), *Id),
				Desc.Wheels.ContainsByPredicate([](const FMantleVehicleWheelDesc& Wheel) { return Wheel.bSteering; })
				&& Desc.Wheels.ContainsByPredicate([](const FMantleVehicleWheelDesc& Wheel) { return Wheel.bDriven; }));
			TestTrue(*FString::Printf(TEXT("%s: a pilot sits on the centreline, with no steering wheel and no passenger"), *Id),
				FMath::IsNearlyZero(Desc.SeatLocation.Y) && !Desc.bSteeringWheel && !Desc.bPassengerSeat);
			FString HeliWhy;
			TestTrue(*FString::Printf(TEXT("%s: the body validates (%s)"), *Id, *HeliWhy), Asset.Body.Validate(HeliWhy));
			continue;
		}
		FMantleVehicleChassisReference Reference;
		if (TestTrue(*FString::Printf(TEXT("%s: the chassis reads (%s)"), *Id, *Error), FMantleVehicleChassisBuilder::ReadReference(Text, Reference, Error)))
		{
			const int32 Axles = Reference.AxlePositions.Num();
			// A centreline chassis (a motorcycle) has one wheel per axle and a fork bone above its steered wheel.
			const int32 PerAxle = Reference.bCentreline ? 1 : 2;
			TestEqual(*FString::Printf(TEXT("%s: one bone per wheel, a root and a fork if it has one"), *Id), Asset.Body.Bones.Num(),
				Desc.Wheels.Num() + 1 + (Reference.ForkBone.IsNone() ? 0 : 1));
			TestTrue(*FString::Printf(TEXT("%s: two to four axles"), *Id), Axles >= 2 && Axles <= 4);
			TestEqual(*FString::Printf(TEXT("%s: a wheel each side of every axle, or one on the centreline"), *Id), Desc.Wheels.Num(), PerAxle * Axles);
			TestEqual(*FString::Printf(TEXT("%s: the front axle is at the front"), *Id), Asset.WheelPositions[0].X, 0.5 * Reference.Wheelbase);
			TestEqual(*FString::Printf(TEXT("%s: the rearmost axle is at the back"), *Id), Asset.WheelPositions.Last().X, -0.5 * Reference.Wheelbase);
			for (int32 Axle = 0; Axle < Axles; ++Axle)
			{
				TestEqual(*FString::Printf(TEXT("%s: axle %d's left wheel"), *Id, Axle), Asset.WheelPositions[PerAxle * Axle].X, Reference.AxleX(Axle));
				if (Reference.bCentreline)
				{
					TestEqual(*FString::Printf(TEXT("%s: axle %d's wheel is on the centreline"), *Id, Axle), Asset.WheelPositions[Axle].Y, 0.0);
				}
				else
				{
					TestEqual(*FString::Printf(TEXT("%s: axle %d's right wheel is across from it"), *Id, Axle), Asset.WheelPositions[2 * Axle + 1].Y, -Asset.WheelPositions[2 * Axle].Y);
				}
				if (Axle > 0)
				{
					TestTrue(*FString::Printf(TEXT("%s: axle %d is behind axle %d by more than a wheel"), *Id, Axle, Axle - 1),
						Reference.AxleX(Axle - 1) - Reference.AxleX(Axle) > 2.0 * Reference.WheelRadius);
				}
			}
			TArray<FName> Names;
			for (const FMantleVehicleWheelDesc& Wheel : Desc.Wheels)
			{
				TestFalse(*FString::Printf(TEXT("%s: bone %s is named once"), *Id, *Wheel.Bone.ToString()), Names.Contains(Wheel.Bone));
				Names.Add(Wheel.Bone);
			}
			if (Reference.bCentreline)
			{
				TestEqual(*FString::Printf(TEXT("%s: a two-axle centreline chassis names its wheels wheel_f and wheel_r"), *Id), Desc.Wheels[0].Bone, FName(TEXT("wheel_f")));
			}
			else if (Axles == 2)
			{
				TestEqual(*FString::Printf(TEXT("%s: a two-axle chassis keeps its bone names"), *Id), Desc.Wheels[0].Bone, FName(TEXT("wheel_fl")));
				TestEqual(*FString::Printf(TEXT("%s: the rear right"), *Id), Desc.Wheels[3].Bone, FName(TEXT("wheel_rr")));
			}
			else
			{
				TestEqual(*FString::Printf(TEXT("%s: three or more axles are numbered"), *Id), Desc.Wheels[0].Bone, FName(TEXT("wheel_a1_l")));
			}
		}
		TestEqual(*FString::Printf(TEXT("%s: a wheel position per wheel"), *Id), Asset.WheelPositions.Num(), Desc.Wheels.Num());
		TestTrue(*FString::Printf(TEXT("%s: the body has triangles"), *Id), Asset.Body.LODs.Num() > 0 && Asset.Body.LODs[0].NumTriangles() > 0);
		FString Why;
		const bool bValid = Asset.Body.Validate(Why);
		TestTrue(*FString::Printf(TEXT("%s: the body validates (%s)"), *Id, *Why), bValid);
		TestTrue(*FString::Printf(TEXT("%s: it steers and is driven"), *Id),
			Desc.Wheels.ContainsByPredicate([](const FMantleVehicleWheelDesc& Wheel) { return Wheel.bSteering; })
			&& Desc.Wheels.ContainsByPredicate([](const FMantleVehicleWheelDesc& Wheel) { return Wheel.bDriven; }));
		for (int32 Index = 0; Index < Desc.Wheels.Num() && Index < Asset.WheelPositions.Num(); ++Index)
		{
			TestEqual(*FString::Printf(TEXT("%s: wheel %d's centre is at its radius"), *Id, Index), Asset.WheelPositions[Index].Z, Desc.Wheels[Index].Radius);
			TestTrue(*FString::Printf(TEXT("%s: wheel %d has a width"), *Id, Index), Desc.Wheels[Index].Width > 0.0);
		}
		TestEqual(*FString::Printf(TEXT("%s: one collision box"), *Id), Desc.Collision.Num(), 1);
		TestTrue(*FString::Printf(TEXT("%s: it has a centre of mass"), *Id), Desc.bCenterOfMass);
		if (Reference.bCentreline)
		{
			TestTrue(*FString::Printf(TEXT("%s: the rider sits on the centreline"), *Id), FMath::IsNearlyZero(Desc.SeatLocation.Y));
			TestFalse(*FString::Printf(TEXT("%s: a rider has handlebars, not a steering wheel"), *Id), Desc.bSteeringWheel);
			TestFalse(*FString::Printf(TEXT("%s: and no passenger"), *Id), Desc.bPassengerSeat);
			continue;
		}
		TestTrue(*FString::Printf(TEXT("%s: the driver sits on the left, -Y"), *Id), Desc.SeatLocation.Y < 0.0);
		TestTrue(*FString::Printf(TEXT("%s: it has a steering wheel ahead of the seat"), *Id),
			Desc.bSteeringWheel && Desc.SteeringWheelRadius > 0.0 && Desc.SteeringWheelLocation.X > Desc.SeatLocation.X);
		TestTrue(*FString::Printf(TEXT("%s: the passenger sits across from the driver"), *Id),
			Desc.bPassengerSeat && FMath::IsNearlyEqual(Desc.PassengerSeatLocation.Y, -Desc.SeatLocation.Y)
			&& FMath::IsNearlyEqual(Desc.PassengerSeatLocation.X, Desc.SeatLocation.X) && FMath::IsNearlyEqual(Desc.PassengerSeatLocation.Z, Desc.SeatLocation.Z));
	}

	FMantleVehicleAsset Rejected;
	FString Error;
	TestFalse(TEXT("text that is not a preset makes nothing"), FMantleVehicleChassisBuilder::MakeAsset(TEXT("{ \"id\": \"nothing\" }"), Rejected, Error));
	TestFalse(TEXT("and says why"), Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleChassisCenterOfMassTest,
	"Mantle.Vehicle.Chassis.CenterOfMassIsAboveTheWheelCentresAndInsideTheBody",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleChassisCenterOfMassTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Ids = FMantleVehicleChassisBuilder::PresetIds();
	if (!TestTrue(TEXT("the plugin has vehicle presets"), Ids.Num() >= 8))
	{
		return false;
	}
	for (const FString& Id : Ids)
	{
		FString Text;
		FString Error;
		FMantleVehicleAsset Asset;
		const bool bMade = FMantleVehicleChassisBuilder::LoadPresetText(Id, Text, Error) && FMantleVehicleChassisBuilder::MakeAsset(Text, Asset, Error);
		if (!TestTrue(FString::Printf(TEXT("%s: it makes an asset (%s)"), *Id, *Error), bMade)
			|| !TestTrue(TEXT("with a collision box"), Asset.Desc.Collision.Num() >= 1 && Asset.WheelPositions.Num() > 0))
		{
			continue;
		}
		const FVector& Centre = Asset.Desc.CenterOfMass;
		const FMantleVehicleBoxDesc& Box = Asset.Desc.Collision[0];
		TestTrue(*FString::Printf(TEXT("%s: the centre of mass (z %.1f) is above the wheel centres (z %.1f)"), *Id, Centre.Z, Asset.WheelPositions[0].Z),
			Centre.Z > Asset.WheelPositions[0].Z);
		const FVector Offset = (Centre - Box.Center).GetAbs();
		TestTrue(*FString::Printf(TEXT("%s: the centre of mass is inside the body box"), *Id),
			Offset.X <= Box.Extent.X && Offset.Y <= Box.Extent.Y && Offset.Z <= Box.Extent.Z);
		// Mid-wheelbase and on the centre line, as the presets put it.
		TestTrue(*FString::Printf(TEXT("%s: the centre of mass is on the centre line"), *Id), FMath::IsNearlyZero(Centre.Y, 1.0));
		TestTrue(*FString::Printf(TEXT("%s: the body box rests above the ground"), *Id), Box.Center.Z - Box.Extent.Z >= 0.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleChassisSixWheelerTest,
	"Mantle.Vehicle.Chassis.SixWheelerHasThreeAxles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleChassisSixWheelerTest::RunTest(const FString& Parameters)
{
	FString Text;
	FString Error;
	FMantleVehicleAsset Asset;
	FMantleVehicleChassisReference Reference;
	if (!TestTrue(TEXT("the six-wheeler reads and makes an asset"),
			FMantleVehicleChassisBuilder::LoadPresetText(TEXT("sixwheeler"), Text, Error) && FMantleVehicleChassisBuilder::ReadReference(Text, Reference, Error)
			&& FMantleVehicleChassisBuilder::MakeAsset(Text, Asset, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("three axles"), Reference.AxlePositions.Num(), 3);
	TestEqual(TEXT("six wheels"), Asset.Desc.Wheels.Num(), 6);
	TestEqual(TEXT("a root and six wheel bones"), Asset.Body.Bones.Num(), 7);
	// The tandem: the middle axle sits 140 cm ahead of the rearmost, the front 420 cm ahead of the middle.
	TestTrue(TEXT("the rear axles are 140 cm apart"), FMath::IsNearlyEqual(Reference.AxleX(1) - Reference.AxleX(2), 140.0, 0.01));
	TestTrue(TEXT("the front axle is 420 cm ahead of the middle"), FMath::IsNearlyEqual(Reference.AxleX(0) - Reference.AxleX(1), 420.0, 0.01));
	TestTrue(TEXT("the front axle steers and is not driven"), Asset.Desc.Wheels[0].bSteering && !Asset.Desc.Wheels[0].bDriven && Asset.Desc.Wheels[1].bSteering);
	for (int32 Index = 2; Index < 6; ++Index)
	{
		TestTrue(*FString::Printf(TEXT("wheel %d (a rear axle) drives and does not steer"), Index), Asset.Desc.Wheels[Index].bDriven && !Asset.Desc.Wheels[Index].bSteering);
	}
	TestEqual(TEXT("the middle axle's bone"), Asset.Body.Bones[3].Name, FName(TEXT("wheel_a2_l")));
	TestEqual(TEXT("a wheel bone hangs off the root"), Asset.Body.Bones[6].ParentIndex, 0);
	// The centre of mass sits over the axles, not at the middle of the wheelbase: the load is over the tandem.
	TestTrue(TEXT("the centre of mass is between the front and rearmost axles"), FMath::Abs(Asset.Desc.CenterOfMass.X) < 0.5 * Reference.Wheelbase);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleChassisMotorcycleTest,
	"Mantle.Vehicle.Chassis.MotorcycleHasTwoCentrelineWheels",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleChassisMotorcycleTest::RunTest(const FString& Parameters)
{
	FString Text;
	FString Error;
	FMantleVehicleAsset Asset;
	FMantleVehicleChassisReference Reference;
	if (!TestTrue(TEXT("the motorcycle reads and makes an asset"),
			FMantleVehicleChassisBuilder::LoadPresetText(TEXT("motorcycle"), Text, Error) && FMantleVehicleChassisBuilder::ReadReference(Text, Reference, Error)
			&& FMantleVehicleChassisBuilder::MakeAsset(Text, Asset, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("a centreline rider chassis"),
		Reference.bCentreline && Reference.Rider.bSet && Reference.DriverSeat.Equals(TEXT("motorcycle"), ESearchCase::CaseSensitive) && !Reference.bSteeringWheel);
	TestEqual(TEXT("two axles"), Reference.AxlePositions.Num(), 2);
	if (!TestEqual(TEXT("two wheels"), Asset.Desc.Wheels.Num(), 2) || !TestEqual(TEXT("two wheel positions"), Asset.WheelPositions.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("the front wheel is wheel_f"), Asset.Desc.Wheels[0].Bone, FName(TEXT("wheel_f")));
	TestEqual(TEXT("the rear wheel is wheel_r"), Asset.Desc.Wheels[1].Bone, FName(TEXT("wheel_r")));
	TestEqual(TEXT("the front wheel is on the centreline"), Asset.WheelPositions[0].Y, 0.0);
	TestEqual(TEXT("the rear wheel is on the centreline"), Asset.WheelPositions[1].Y, 0.0);
	TestEqual(TEXT("the front wheel is at the front"), Asset.WheelPositions[0].X, 0.5 * Reference.Wheelbase);
	TestEqual(TEXT("the rear wheel is at the back"), Asset.WheelPositions[1].X, -0.5 * Reference.Wheelbase);
	TestTrue(TEXT("the front wheel steers and the rear drives"),
		Asset.Desc.Wheels[0].bSteering && !Asset.Desc.Wheels[0].bDriven && Asset.Desc.Wheels[1].bDriven && !Asset.Desc.Wheels[1].bSteering);

	// The fork bone is the front wheel bone's parent, at the wheel centre, and the wheel bone sits at the fork's origin.
	TestEqual(TEXT("the fork is named fork_f"), Reference.ForkBone, FMantleVehicleChassisBuilder::FrontForkBoneName());
	TestEqual(TEXT("a root, the fork and two wheels"), Asset.Body.Bones.Num(), 4);
	const int32 Fork = Asset.Body.Bones.IndexOfByPredicate([&Reference](const FMantleSkinnedMeshBone& Bone) { return Bone.Name == Reference.ForkBone; });
	const int32 Front = Asset.Body.Bones.IndexOfByPredicate([](const FMantleSkinnedMeshBone& Bone) { return Bone.Name == FName(TEXT("wheel_f")); });
	const int32 Rear = Asset.Body.Bones.IndexOfByPredicate([](const FMantleSkinnedMeshBone& Bone) { return Bone.Name == FName(TEXT("wheel_r")); });
	if (!TestTrue(TEXT("the fork and both wheel bones exist"), Fork != INDEX_NONE && Front != INDEX_NONE && Rear != INDEX_NONE))
	{
		return false;
	}
	TestEqual(TEXT("the fork hangs off the root"), Asset.Body.Bones[Fork].ParentIndex, 0);
	TestEqual(TEXT("the front wheel bone's parent is the fork"), Asset.Body.Bones[Front].ParentIndex, Fork);
	TestEqual(TEXT("the rear wheel hangs off the root"), Asset.Body.Bones[Rear].ParentIndex, 0);
	TestTrue(TEXT("the fork is at the front wheel centre"), Asset.Body.ReferencePose[Fork].GetLocation().Equals(Asset.WheelPositions[0]));
	TestTrue(TEXT("the front wheel bone is at the fork's origin"), Asset.Body.ReferencePose[Front].GetLocation().IsNearlyZero());
	FString Why;
	const bool bValid = Asset.Body.Validate(Why);
	TestTrue(*FString::Printf(TEXT("the body validates (%s)"), *Why), bValid);

	// The rider: grips ahead of the seat, one each side, the pegs below it, and the balance on.
	TestTrue(TEXT("the grips are ahead of the seat"), Reference.Rider.LeftGrip.X > Reference.Seat.X && Reference.Rider.RightGrip.X > Reference.Seat.X);
	TestTrue(TEXT("left is -Y and right +Y"),
		Reference.Rider.LeftGrip.Y < 0.0 && Reference.Rider.RightGrip.Y > 0.0 && Reference.Rider.LeftPeg.Y < 0.0 && Reference.Rider.RightPeg.Y > 0.0);
	TestTrue(TEXT("the pegs are below the seat"), Reference.Rider.LeftPeg.Z < Reference.Seat.Z);
	TestTrue(TEXT("the balance is on"), Reference.Rider.RotationControl.bTorqueControl && Reference.Rider.RotationControl.RollMaxAngle > 0.0);

	// A car has none of this.
	FMantleVehicleChassisReference Car;
	if (TestTrue(TEXT("the sedan reads"), FMantleVehicleChassisBuilder::LoadPresetText(TEXT("sedan"), Text, Error) && FMantleVehicleChassisBuilder::ReadReference(Text, Car, Error)))
	{
		TestTrue(TEXT("the sedan is not a centreline rider chassis"),
			!Car.bCentreline && !Car.Rider.bSet && Car.ForkBone.IsNone() && Car.bSteeringWheel && Car.DriverSeat.Equals(TEXT("car"), ESearchCase::CaseSensitive));
	}
	return true;
}

// The exact character collision is the body's triangles off the wheels.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBodyCollisionLeavesWheelsOutTest,
	"Mantle.Vehicle.Chassis.BodyCollisionLeavesWheelsOut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBodyCollisionLeavesWheelsOutTest::RunTest(const FString& Parameters)
{
	// Two bones: the root (0) and a wheel (1). Vertices 0 to 3 are the body (a quad); 4, 5 and 6 are a wheel's triangle, whose
	// vertex 4 lists the root first but is weighted more to the wheel (the heaviest slot decides, not the first); 7 and 8 are body
	// points that share a triangle with the wheel's vertex 6.
	FMantleSkinnedMesh Body;
	Body.Bones.SetNum(2);
	Body.Bones[0].Name = TEXT("Root");
	Body.Bones[1].Name = TEXT("Wheel_FL");
	FMantleSkinnedMeshLOD& Lod = Body.LODs.AddDefaulted_GetRef();
	Lod.NumInfluences = 2;
	const FVector3f Points[9] = {
		FVector3f(0, 0, 0), FVector3f(100, 0, 0), FVector3f(100, 100, 0), FVector3f(0, 100, 0),
		FVector3f(200, 0, 0), FVector3f(260, 0, 0), FVector3f(230, 0, 60),
		FVector3f(0, 200, 0), FVector3f(100, 200, 0) };
	// Heaviest bone per vertex; the second slot is the other bone at the weight left over.
	const uint16 Heaviest[9] = { 0, 0, 0, 0, 1, 1, 1, 0, 0 };
	for (int32 Vertex = 0; Vertex < 9; ++Vertex)
	{
		Lod.Positions.Add(Points[Vertex]);
		const uint16 Other = static_cast<uint16>(Heaviest[Vertex] == 0 ? 1 : 0);
		const bool bTricky = Vertex == 4;
		// Vertex 4's first slot is the root at 100, its second the wheel at 155.
		Lod.InfluenceBones.Add(bTricky ? static_cast<uint16>(0) : Heaviest[Vertex]);
		Lod.InfluenceBones.Add(bTricky ? static_cast<uint16>(1) : Other);
		Lod.InfluenceWeights.Add(static_cast<uint8>(bTricky ? 100 : 200));
		Lod.InfluenceWeights.Add(static_cast<uint8>(bTricky ? 155 : 55));
	}
	Lod.Indices = { 0, 1, 2, 0, 2, 3, 4, 5, 6, 3, 2, 8, 3, 8, 7, 1, 5, 2 };
	// 0,1,2 and 0,2,3 and 3,2,8 and 3,8,7: the body. 4,5,6: the wheel. 1,5,2: the body touching the wheel at vertex 5.

	const TArray<FName> Wheels = { FName(TEXT("Wheel_FL")) };
	const TSharedPtr<const FMantleVehicleBodyCollision> Shape = FMantleVehicleBodyCollision::FromBody(Body, Wheels);
	if (!TestTrue(TEXT("a body with wheels has a collision shape"), Shape.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("the four body triangles stay; the wheel's and the one touching it go"), Shape->NumTriangles(), 4);
	for (const int32 Index : Shape->Indices)
	{
		TestTrue(TEXT("every index names a kept vertex"), Shape->Positions.IsValidIndex(Index));
	}
	for (const FVector3f& Point : Shape->Positions)
	{
		TestTrue(TEXT("no wheel vertex is kept"), Point.X < 150.f);
	}
	TestEqual(TEXT("only the vertices a kept triangle uses are kept"), Shape->Positions.Num(), 6);

	// With no wheel bone named, nothing is a wheel: every triangle stays (the wheel's triangle has area).
	const TSharedPtr<const FMantleVehicleBodyCollision> All = FMantleVehicleBodyCollision::FromBody(Body, TConstArrayView<FName>());
	if (TestTrue(TEXT("with no wheels named every triangle stays"), All.IsValid()))
	{
		TestEqual(TEXT("six triangles"), All->NumTriangles(), 6);
	}

	// A body with no skin data cannot say what a wheel is: no shape, rather than a guess.
	FMantleSkinnedMesh Bare;
	Bare.LODs.AddDefaulted();
	TestFalse(TEXT("a body with no skin has no shape"), FMantleVehicleBodyCollision::FromBody(Bare, Wheels).IsValid());

	// Every shipped preset's bare chassis: the body, less its four wheels. A helicopter draws no wheels (its skids, on the root, stand for them), nor does
	// a hovercraft (its pads lift where its wheels would be), so its shape is its whole body: no wheel bone carries a triangle to leave out.
	for (const FString& Id : FMantleVehicleChassisBuilder::PresetIds())
	{
		FString Text;
		FString Error;
		FMantleVehicleAsset Asset;
		FMantleChassisSpec Spec;
		if (!FMantleVehicleChassisBuilder::LoadPresetText(Id, Text, Error) || !FMantleVehicleChassisBuilder::MakeAsset(Text, Asset, Error)
			|| !FMantleChassisSpec::Parse(Text, Spec, Error))
		{
			continue;
		}
		TArray<FName> Names;
		for (const FMantleVehicleWheelDesc& Wheel : Asset.Desc.Wheels)
		{
			Names.Add(Wheel.Bone);
		}
		const TSharedPtr<const FMantleVehicleBodyCollision> Chassis = FMantleVehicleBodyCollision::FromBody(Asset.Body, Names);
		if (TestTrue(*FString::Printf(TEXT("%s: the chassis body has a collision shape"), *Id), Chassis.IsValid()))
		{
			if (Spec.Helicopter.bSet || Spec.Hovercraft.bSet)
			{
				TestEqual(*FString::Printf(TEXT("%s: with no wheels drawn, every triangle is in it"), *Id), Chassis->NumTriangles(), Asset.Body.LODs[0].NumTriangles());
			}
			else
			{
				TestTrue(*FString::Printf(TEXT("%s: the wheels' triangles are not in it"), *Id), Chassis->NumTriangles() < Asset.Body.LODs[0].NumTriangles());
			}
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
