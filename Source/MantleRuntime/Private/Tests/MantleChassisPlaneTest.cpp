// Copyright Joshua Eger. All Rights Reserved.
#include "MantleChassisSpec.h"

#include "MantleVehicleChassisBuilder.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// A plane's chassis: tricycle gear with its nose wheel on the centreline, a grey airframe of blocks on bones that hinge where the
// aerofoils act, and a document whose collision is the fuselage, wing, tailplane and fin. The cars' layouts are untouched by the per-axle centreline.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleChassisPlaneTestPrivate
{
	bool LoadPlane(FAutomationTestBase& Test, FMantleChassisSpec& OutSpec)
	{
		FString Text;
		FString Error;
		const bool bOk = FMantleVehicleChassisBuilder::LoadPresetText(TEXT("plane"), Text, Error) && FMantleChassisSpec::Parse(Text, OutSpec, Error);
		Test.TestTrue(*FString::Printf(TEXT("the plane preset reads (%s)"), *Error), bOk);
		return bOk;
	}

	bool LoadJet(FAutomationTestBase& Test, FMantleChassisSpec& OutSpec)
	{
		FString Text;
		FString Error;
		const bool bOk = FMantleVehicleChassisBuilder::LoadPresetText(TEXT("jet"), Text, Error) && FMantleChassisSpec::Parse(Text, OutSpec, Error);
		Test.TestTrue(*FString::Printf(TEXT("the jet preset reads (%s)"), *Error), bOk);
		return bOk;
	}

	int32 BoneIndex(const FMantleVehicleAsset& Asset, const FName Name)
	{
		return Asset.Body.Bones.IndexOfByPredicate([Name](const FMantleSkinnedMeshBone& Bone) { return Bone.Name == Name; });
	}

	/** The bounds of the vertices whose heaviest influence (the first slot, as the chassis builder skins them) is Bone; false when it has none. */
	bool BoundsOfBone(const FMantleSkinnedMeshLOD& LOD, const int32 Bone, FVector& OutMin, FVector& OutMax)
	{
		OutMin = FVector(TNumericLimits<double>::Max());
		OutMax = FVector(-TNumericLimits<double>::Max());
		bool bAny = false;
		for (int32 Vertex = 0; Vertex < LOD.Positions.Num(); ++Vertex)
		{
			if (LOD.InfluenceBones[Vertex * LOD.NumInfluences] != Bone)
			{
				continue;
			}
			const FVector Point(LOD.Positions[Vertex]);
			OutMin = FVector(FMath::Min(OutMin.X, Point.X), FMath::Min(OutMin.Y, Point.Y), FMath::Min(OutMin.Z, Point.Z));
			OutMax = FVector(FMath::Max(OutMax.X, Point.X), FMath::Max(OutMax.Y, Point.Y), FMath::Max(OutMax.Z, Point.Z));
			bAny = true;
		}
		return bAny;
	}

	bool Same(const FVector& A, const FVector& B)
	{
		return A.Equals(B, 1.0e-3);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisPlaneSpecTest,
	"Mantle.Vehicle.Chassis.PlaneSpecIsTricycleGear",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisPlaneSpecTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisPlaneTestPrivate;
	FMantleChassisSpec Plane;
	if (!LoadPlane(*this, Plane))
	{
		return false;
	}
	TestTrue(TEXT("a plane with an airframe and a pilot's seat, no steering wheel and no passenger"),
		Plane.Type == EMantleChassisType::Plane && Plane.Aircraft.bSet && Plane.DriverSeat == TEXT("pilot") && !Plane.bSteeringWheel && !Plane.bPassenger);
	TestEqual(TEXT("the preset's cruise throttle, the share of thrust level flight at cruise speed needs (schema/validate.py holds it to the arithmetic)"),
		Plane.Aircraft.CruiseThrottle, 0.37);
	TestEqual(TEXT("and the speed it is trimmed for, 70 m/s"), Plane.Aircraft.CruiseSpeedCmPerSec, 7000.0);
	// A resized plane's trim is solved again (its weight, wings and thrust no longer follow the preset's): a share of the thrust that is not the preset's own.
	TestTrue(TEXT("a resized plane's cruise throttle is its own, not the preset's"), FMath::Abs(FMantleChassisSpec::Scaled(Plane, 1.15).Aircraft.CruiseThrottle - 0.37) > 1.0e-3);
	TestEqual(TEXT("the type's name"), FString(FMantleChassisSpec::TypeName(Plane.Type)), FString(TEXT("plane")));
	EMantleChassisType Parsed = EMantleChassisType::Car;
	TestTrue(TEXT("and it parses back"), FMantleChassisSpec::ParseType(TEXT("plane"), Parsed) && Parsed == EMantleChassisType::Plane);

	// Tricycle gear: the layout is not centreline all through, but its nose axle is.
	TestFalse(TEXT("the layout as a whole is not a centreline one"), Plane.bCentreline);
	if (!TestEqual(TEXT("two axles"), Plane.Axles.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("the nose axle is one steered wheel on the centreline"), Plane.Axles[0].bCentreline && Plane.Axles[0].bSteer && !Plane.Axles[0].bDrive);
	TestTrue(TEXT("the mains are a pair"), !Plane.Axles[1].bCentreline && !Plane.Axles[1].bSteer);
	TestTrue(TEXT("axle 0 has one wheel, axle 1 two"), Plane.AxleIsCentreline(0) && !Plane.AxleIsCentreline(1));
	TestEqual(TEXT("three wheels"), Plane.WheelCount(), 3);
	TestEqual(TEXT("the archetype's size counts three too"), Plane.ArchetypeSize.WheelCount(), 3);
	TestTrue(TEXT("wheel 0 is the nose axle's, wheels 1 and 2 the mains'"), Plane.AxleOf(0) == 0 && Plane.AxleOf(1) == 1 && Plane.AxleOf(2) == 1);
	TestTrue(TEXT("the mains begin at wheel 1"), Plane.FirstWheelOf(0) == 0 && Plane.FirstWheelOf(1) == 1);
	TestEqual(TEXT("the nose wheel is wheel_f"), Plane.WheelBone(0), FName(TEXT("wheel_f")));
	TestEqual(TEXT("the left main is wheel_rl"), Plane.WheelBone(1), FName(TEXT("wheel_rl")));
	TestEqual(TEXT("the right main is wheel_rr"), Plane.WheelBone(2), FName(TEXT("wheel_rr")));
	TestTrue(TEXT("the nose wheel is ahead on the centreline"), Same(Plane.WheelCentre(0), FVector(190.0, 0.0, 40.0)));
	TestTrue(TEXT("the left main is behind, on the left"), Same(Plane.WheelCentre(1), FVector(-190.0, -160.0, 40.0)));
	TestTrue(TEXT("the right main is across from it"), Same(Plane.WheelCentre(2), FVector(-190.0, 160.0, 40.0)));
	TestTrue(TEXT("no fork: a plane's nose wheel steers on its own bone"), Plane.ForkBone.IsNone());

	// The centre of mass is ahead of the mains, so the nose wheel carries a part of the weight and the mains most of it.
	const double NoseShare = (Plane.CenterOfMass.X - Plane.WheelCentre(1).X) / (Plane.WheelCentre(0).X - Plane.WheelCentre(1).X);
	TestTrue(*FString::Printf(TEXT("the nose wheel carries %.0f%% of the weight, 5 to 25"), 100.0 * NoseShare), NoseShare > 0.05 && NoseShare < 0.25);

	// The cars' and the motorcycle's layouts are as they were: no axle has a centreline of its own, and the motorcycle's whole layout has. (The planes, the
	// plane and the jet, are the ones with a nose wheel of their own; the helicopter, whose cabin is not centred on its skids, has its own test.)
	for (const FString& Id : FMantleVehicleChassisBuilder::PresetIds())
	{
		FMantleChassisSpec Other;
		FString Text;
		FString Error;
		if (!FMantleVehicleChassisBuilder::LoadPresetText(Id, Text, Error) || !FMantleChassisSpec::Parse(Text, Other, Error) || Other.Type == EMantleChassisType::Plane
			|| Other.Type == EMantleChassisType::Helicopter)
		{
			continue;
		}
		TestFalse(*FString::Printf(TEXT("%s: no axle has a centreline of its own"), *Id), Other.Axles.ContainsByPredicate([](const FMantleChassisAxle& Axle) { return Axle.bCentreline; }));
		TestEqual(*FString::Printf(TEXT("%s: a wheel each side, or one on the centreline all through"), *Id), Other.WheelCount(), (Other.bCentreline ? 1 : 2) * Other.Axles.Num());
		TestFalse(*FString::Printf(TEXT("%s: it has no airframe"), *Id), Other.Aircraft.bSet);
		TestEqual(*FString::Printf(TEXT("%s: its body is centred on the wheelbase"), *Id), Other.BodyX, 0.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisPlaneAirframeTest,
	"Mantle.Vehicle.Chassis.PlaneAirframeIsBlocksOnHingeBones",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisPlaneAirframeTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisPlaneTestPrivate;
	FMantleChassisSpec Plane;
	FString Error;
	FMantleVehicleAsset Asset;
	if (!LoadPlane(*this, Plane))
	{
		return false;
	}
	const bool bMade = FMantleVehicleChassisBuilder::MakeAsset(Plane, Asset, Error);
	if (!TestTrue(*FString::Printf(TEXT("the plane makes an asset (%s)"), *Error), bMade))
	{
		return false;
	}

	// The bones: the root, the three wheels, then each control surface and the propeller, every one a child of the root, at its hinge, unturned.
	const FName Names[] = { FMantleVehicleChassisBuilder::RootBoneName(), TEXT("wheel_f"), TEXT("wheel_rl"), TEXT("wheel_rr"), TEXT("aileron_l"), TEXT("aileron_r"),
		TEXT("elevator"), TEXT("rudder"), TEXT("propeller") };
	if (!TestEqual(TEXT("nine bones"), Asset.Body.Bones.Num(), static_cast<int32>(UE_ARRAY_COUNT(Names))))
	{
		return false;
	}
	for (int32 Index = 0; Index < Asset.Body.Bones.Num(); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("bone %d"), Index), Asset.Body.Bones[Index].Name, Names[Index]);
		TestEqual(*FString::Printf(TEXT("bone %d is a child of the root"), Index), Asset.Body.Bones[Index].ParentIndex, Index == 0 ? INDEX_NONE : 0);
		TestTrue(*FString::Printf(TEXT("bone %d is not turned"), Index), Asset.Body.ReferencePose[Index].GetRotation().IsIdentity(1.0e-6));
	}
	TestEqual(TEXT("the bone names are the builder's"), FMantleVehicleChassisBuilder::AileronBoneName(false), FName(TEXT("aileron_l")));
	TestEqual(TEXT("and the right aileron's"), FMantleVehicleChassisBuilder::AileronBoneName(true), FName(TEXT("aileron_r")));
	TestEqual(TEXT("and the elevator's"), FMantleVehicleChassisBuilder::ElevatorBoneName(), FName(TEXT("elevator")));
	TestEqual(TEXT("and the rudder's"), FMantleVehicleChassisBuilder::RudderBoneName(), FName(TEXT("rudder")));
	TestEqual(TEXT("and a single engine's propeller"), FMantleVehicleChassisBuilder::PropellerBoneName(0, 1), FName(TEXT("propeller")));
	TestEqual(TEXT("and a twin's"), FMantleVehicleChassisBuilder::PropellerBoneName(1, 2), FName(TEXT("propeller_r")));

	// Where each hinges: at the aerofoil it turns, which is where the lift acts.
	TestTrue(TEXT("the left aileron hinges at the middle of the left half-wing's quarter-chord line"), Same(Asset.Body.ReferencePose[4].GetLocation(), FVector(-156.0, -362.5, 252.0)));
	TestTrue(TEXT("and the right across from it"), Same(Asset.Body.ReferencePose[5].GetLocation(), FVector(-156.0, 362.5, 252.0)));
	TestTrue(TEXT("the elevator hinges at the elevator's aerofoil"), Same(Asset.Body.ReferencePose[6].GetLocation(), Plane.ElevatorLocation()));
	TestTrue(TEXT("the rudder at the rudder's"), Same(Asset.Body.ReferencePose[7].GetLocation(), Plane.RudderLocation()));
	TestTrue(TEXT("the propeller at its hub"), Same(Asset.Body.ReferencePose[8].GetLocation(), Plane.PropellerHub(0)));
	TestTrue(TEXT("the elevator is where the preset says"), Same(Plane.ElevatorLocation(), FVector(-835.0, 0.0, 195.0)));
	TestTrue(TEXT("the rudder is the middle of the fin, over the fuselage"), Same(Plane.RudderLocation(), FVector(-797.5, 0.0, 380.0)));
	TestTrue(TEXT("the hub is ahead of the nose, at the fuselage's middle height"), Same(Plane.PropellerHub(0), FVector(250.0, 0.0, 200.0)));
	TestTrue(TEXT("the wing's two aerofoils are 70 percent of the half-span out, level with the wing"),
		Same(Plane.WingAerofoilLocation(false), FVector(-156.0, -507.5, 252.0)) && Same(Plane.WingAerofoilLocation(true), FVector(-156.0, 507.5, 252.0)));
	const FMantleChassisBlock LeftHalf = Plane.HalfWingBlock(false);
	const FMantleChassisBlock RightHalf = Plane.HalfWingBlock(true);
	TestTrue(TEXT("each aerofoil is inside its half-wing's block"),
		Plane.WingAerofoilLocation(false).Y > LeftHalf.Min.Y && Plane.WingAerofoilLocation(false).Y < LeftHalf.Max.Y
		&& Plane.WingAerofoilLocation(true).Y > RightHalf.Min.Y && Plane.WingAerofoilLocation(true).Y < RightHalf.Max.Y);

	// The skin: every block is skinned wholly to the bone that turns it, so deflecting a bone moves that block and nothing else.
	if (!TestEqual(TEXT("one LOD"), Asset.Body.LODs.Num(), 1))
	{
		return false;
	}
	const FMantleSkinnedMeshLOD& LOD = Asset.Body.LODs[0];
	struct FExpected
	{
		int32 Bone;
		FVector Min;
		FVector Max;
		const TCHAR* What;
	};
	const FMantleChassisBlock Body = Plane.BodyBlock();
	const FMantleChassisBlock Tail = Plane.TailplaneBlock();
	const FMantleChassisBlock Fin = Plane.FinBlock();
	const FVector Hub = Plane.PropellerHub(0);
	const FExpected Expected[] = {
		{ 0, Body.Min, Body.Max, TEXT("the fuselage on the root") },
		{ 4, LeftHalf.Min, LeftHalf.Max, TEXT("the left half-wing on aileron_l") },
		{ 5, RightHalf.Min, RightHalf.Max, TEXT("the right half-wing on aileron_r") },
		{ 6, Tail.Min, Tail.Max, TEXT("the tailplane on the elevator") },
		{ 7, Fin.Min, Fin.Max, TEXT("the fin on the rudder") },
		{ 8, Hub + FVector(-3.0, -110.0, -110.0), Hub + FVector(3.0, 110.0, 110.0), TEXT("the propeller's cross on the propeller") },
		{ 1, FVector(150.0, -14.0, 0.0), FVector(230.0, 14.0, 80.0), TEXT("the nose wheel") },
		{ 2, FVector(-230.0, -174.0, 0.0), FVector(-150.0, -146.0, 80.0), TEXT("the left main") },
		{ 3, FVector(-230.0, 146.0, 0.0), FVector(-150.0, 174.0, 80.0), TEXT("the right main") },
	};
	for (const FExpected& Want : Expected)
	{
		FVector Min;
		FVector Max;
		if (TestTrue(*FString::Printf(TEXT("%s has vertices"), Want.What), BoundsOfBone(LOD, Want.Bone, Min, Max)))
		{
			TestTrue(*FString::Printf(TEXT("%s: skinned to a block %s to %s, not %s to %s"), Want.What, *Min.ToString(), *Max.ToString(), *Want.Min.ToString(), *Want.Max.ToString()),
				Same(Min, Want.Min) && Same(Max, Want.Max));
		}
	}
	TestEqual(TEXT("the root's block, seven boxes in all and three wheels: 390 vertices"), LOD.Positions.Num(), 390);
	FString Why;
	TestTrue(*FString::Printf(TEXT("the body validates (%s)"), *Why), Asset.Body.Validate(Why));

	// The document's collision: the fuselage, wing, tailplane and fin, as the blocks are.
	TestEqual(TEXT("four collision boxes"), Asset.Desc.Collision.Num(), 4);
	if (Asset.Desc.Collision.Num() == 4)
	{
		const FMantleChassisBlock Wing = Plane.WingBlock();
		const FMantleChassisBlock Blocks[] = { Body, Wing, Tail, Fin };
		for (int32 Index = 0; Index < 4; ++Index)
		{
			const FMantleVehicleBoxDesc& Box = Asset.Desc.Collision[Index];
			TestTrue(*FString::Printf(TEXT("collision box %d is block %d"), Index, Index),
				Same(Box.Center - Box.Extent, Blocks[Index].Min) && Same(Box.Center + Box.Extent, Blocks[Index].Max));
		}
		// The fuselage is first: the body box the cars' tests read, with the centre of mass inside it.
		TestTrue(TEXT("the centre of mass is inside the fuselage's box"), FMath::Abs(Asset.Desc.CenterOfMass.X - Asset.Desc.Collision[0].Center.X) <= Asset.Desc.Collision[0].Extent.X
			&& Asset.Desc.CenterOfMass.Z > Asset.WheelPositions[0].Z);
	}

	// The exact body (the characters' collision) is every triangle but the wheels': it covers the fuselage, both wings, the tail, the fin and the propeller.
	TArray<FName> WheelBones;
	for (const FMantleVehicleWheelDesc& Wheel : Asset.Desc.Wheels)
	{
		WheelBones.Add(Wheel.Bone);
	}
	const TSharedPtr<const FMantleVehicleBodyCollision> Shape = FMantleVehicleBodyCollision::FromBody(Asset.Body, WheelBones);
	if (TestTrue(TEXT("the body has an exact collision shape"), Shape.IsValid()))
	{
		TestEqual(TEXT("seven boxes' triangles, and none of the wheels'"), Shape->NumTriangles(), 84);
		FVector Min(TNumericLimits<double>::Max());
		FVector Max(-TNumericLimits<double>::Max());
		for (const FVector3f& Corner : Shape->Positions)
		{
			const FVector Point(Corner);
			Min = FVector(FMath::Min(Min.X, Point.X), FMath::Min(Min.Y, Point.Y), FMath::Min(Min.Z, Point.Z));
			Max = FVector(FMath::Max(Max.X, Point.X), FMath::Max(Max.Y, Point.Y), FMath::Max(Max.Z, Point.Z));
		}
		TestTrue(TEXT("it reaches the wing tips, the tail, the fin's top and the propeller's hub"),
			FMath::IsNearlyEqual(Min.Y, -725.0, 1.0e-3) && FMath::IsNearlyEqual(Max.Y, 725.0, 1.0e-3) && FMath::IsNearlyEqual(Min.X, -910.0, 1.0e-3)
			&& FMath::IsNearlyEqual(Max.Z, 480.0, 1.0e-3) && FMath::IsNearlyEqual(Max.X, 253.0, 1.0e-3));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisPlaneTwinTest,
	"Mantle.Vehicle.Chassis.PlaneTwinEnginesHaveBonesAndNacelles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisPlaneTwinTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisPlaneTestPrivate;
	FMantleChassisSpec Plane;
	FString Error;
	FMantleVehicleAsset Asset;
	if (!LoadPlane(*this, Plane))
	{
		return false;
	}
	Plane.Aircraft.EngineCount = 2;
	const bool bMade = FMantleVehicleChassisBuilder::MakeAsset(Plane, Asset, Error);
	if (!TestTrue(*FString::Printf(TEXT("a twin makes an asset (%s)"), *Error), bMade))
	{
		return false;
	}
	TestEqual(TEXT("one more bone: a propeller each side"), Asset.Body.Bones.Num(), 10);
	TestTrue(TEXT("propeller_l and propeller_r, and no propeller"),
		BoneIndex(Asset, TEXT("propeller_l")) != INDEX_NONE && BoneIndex(Asset, TEXT("propeller_r")) != INDEX_NONE && BoneIndex(Asset, TEXT("propeller")) == INDEX_NONE);
	TestTrue(TEXT("the hubs are a third of the half-span out, the left on -Y, level with the fuselage's middle"),
		Same(Plane.PropellerHub(0), FVector(250.0, -1450.0 / 6.0, 200.0)) && Same(Plane.PropellerHub(1), FVector(250.0, 1450.0 / 6.0, 200.0)));
	const int32 Left = BoneIndex(Asset, TEXT("propeller_l"));
	const int32 Right = BoneIndex(Asset, TEXT("propeller_r"));
	if (Left != INDEX_NONE && Right != INDEX_NONE)
	{
		TestTrue(TEXT("each bone is at its hub"),
			Same(Asset.Body.ReferencePose[Left].GetLocation(), Plane.PropellerHub(0)) && Same(Asset.Body.ReferencePose[Right].GetLocation(), Plane.PropellerHub(1)));
	}
	// Seven boxes and a second propeller's two, and a nacelle behind each propeller on the root.
	TestEqual(TEXT("390 vertices, a second propeller's 48 and two nacelles' 48"), Asset.Body.LODs[0].Positions.Num(), 390 + 48 + 48);
	FString Why;
	TestTrue(*FString::Printf(TEXT("the body validates (%s)"), *Why), Asset.Body.Validate(Why));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisPlaneRefusalsTest,
	"Mantle.Vehicle.Chassis.PlanePresetNeedsItsParts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisPlaneRefusalsTest::RunTest(const FString& Parameters)
{
	// A plane's type, its pilot's seat and its airframe go together, and each of its numbers is read.
	FString Plane;
	FString Sedan;
	FString Error;
	if (!TestTrue(TEXT("the presets read"), FMantleVehicleChassisBuilder::LoadPresetText(TEXT("plane"), Plane, Error) && FMantleVehicleChassisBuilder::LoadPresetText(TEXT("sedan"), Sedan, Error)))
	{
		return false;
	}
	FMantleChassisSpec Spec;
	TestTrue(TEXT("the plane as it is reads"), FMantleChassisSpec::Parse(Plane, Spec, Error));

	const auto Refused = [this, &Spec, &Error](const TCHAR* Label, const FString& Text, const TCHAR* Why)
	{
		TestFalse(*FString::Printf(TEXT("%s is refused"), Label), FMantleChassisSpec::Parse(Text, Spec, Error));
		TestTrue(*FString::Printf(TEXT("%s: and says why (%s)"), Label, *Error), Error.Contains(Why));
	};
	Refused(TEXT("a plane that says it is a car"), Plane.Replace(TEXT("\"type\": \"plane\""), TEXT("\"type\": \"car\"")), TEXT("chassis.aircraft"));
	Refused(TEXT("a car that says it is a plane"), Sedan.Replace(TEXT("\"type\": \"car\""), TEXT("\"type\": \"plane\"")), TEXT("chassis.aircraft"));
	Refused(TEXT("a plane with no airframe"), Plane.Replace(TEXT("\"aircraft\":"), TEXT("\"airframe\":")), TEXT("chassis.aircraft"));
	Refused(TEXT("a plane in a car's seat"), Plane.Replace(TEXT("\"driver\": \"pilot\""), TEXT("\"driver\": \"car\"")), TEXT("steeringWheel"));
	Refused(TEXT("a plane with three engines"), Plane.Replace(TEXT("\"count\": 1"), TEXT("\"count\": 3")), TEXT("engines.count"));
	Refused(TEXT("a plane with no fin"), Plane.Replace(TEXT("\"fin\":"), TEXT("\"stabiliser\":")), TEXT("fin"));
	Refused(TEXT("a plane whose surfaces do not stall"), Plane.Replace(TEXT("\"stallAngle\": 20"), TEXT("\"stallAngle\": 0")), TEXT("stallAngle"));
	Refused(TEXT("a plane with no pilot's controls"), Plane.Replace(TEXT("\"controls\":"), TEXT("\"hands\":")), TEXT("controls"));
	return true;
}

// The jet (jet.json): a plane whose airframe has no propeller block, so its engine is a jet that pushes from a nozzle behind the fuselage's tail.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisJetSpecTest,
	"Mantle.Vehicle.Chassis.JetSpecHasANozzleBehindTheTail",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisJetSpecTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisPlaneTestPrivate;
	FMantleChassisSpec Jet;
	if (!LoadJet(*this, Jet))
	{
		return false;
	}
	TestTrue(TEXT("a plane with an airframe and a pilot's seat, no steering wheel and no passenger"),
		Jet.Type == EMantleChassisType::Plane && Jet.Aircraft.bSet && Jet.DriverSeat == TEXT("pilot") && !Jet.bSteeringWheel && !Jet.bPassenger);
	TestTrue(TEXT("no propeller: its engine is a jet, with no blades to draw or spin"), !Jet.Aircraft.bPropeller && Jet.Aircraft.PropellerRadius == 0.0 && Jet.Aircraft.PropellerRpm == 0.0);
	TestTrue(TEXT("one engine of 100 kN"), Jet.Aircraft.EngineCount == 1 && Jet.Aircraft.EngineThrustNewtons == 100000.0);
	TestEqual(TEXT("its cruise throttle, the share of thrust level flight at 120 m/s needs (schema/validate.py holds it to the arithmetic)"), Jet.Aircraft.CruiseThrottle, 0.255);
	TestEqual(TEXT("its air-speed limit, 250 m/s"), Jet.Aircraft.AirSpeedLimitCmPerSec, 25000.0);
	TestEqual(TEXT("tricycle gear: three wheels"), Jet.WheelCount(), 3);

	// The nozzle: at engines.x, behind the fuselage's rear end, level with the fuselage's middle; it is where the engine pushes from.
	const FMantleChassisBlock Body = Jet.BodyBlock();
	TestTrue(TEXT("the fuselage runs from -950 to 550"), Same(Body.Min, FVector(-950.0, -60.0, 110.0)) && Same(Body.Max, FVector(550.0, 60.0, 290.0)));
	TestTrue(TEXT("the nozzle's exit is on the centreline at the fuselage's middle height"), Same(Jet.NozzleLocation(0), FVector(-1000.0, 0.0, 200.0)));
	TestTrue(TEXT("and it is behind the fuselage"), Jet.NozzleLocation(0).X < Body.Min.X);
	TestTrue(TEXT("the engine pushes from the nozzle"), Same(Jet.EngineLocation(0), Jet.NozzleLocation(0)));
	const FMantleChassisBlock Nozzle = Jet.NozzleBlock(0);
	TestTrue(TEXT("the nozzle's box runs from its exit to the fuselage's rear end, 0.7 of the fuselage's width square"),
		Same(Nozzle.Min, FVector(-1000.0, -42.0, 158.0)) && Same(Nozzle.Max, FVector(-950.0, 42.0, 242.0)));
	TestTrue(TEXT("an intake stands on each side of the fuselage, from the wing's leading edge forward"),
		Same(Jet.IntakeBlock(false).Min, FVector(-140.0, -90.0, 155.0)) && Same(Jet.IntakeBlock(false).Max, FVector(160.0, -60.0, 245.0))
		&& Same(Jet.IntakeBlock(true).Min, FVector(-140.0, 60.0, 155.0)) && Same(Jet.IntakeBlock(true).Max, FVector(160.0, 90.0, 245.0)));

	// A resized jet keeps it a jet: the nozzle grows with it, still behind the tail.
	const FMantleChassisSpec Bigger = FMantleChassisSpec::Scaled(Jet, 1.3);
	TestTrue(TEXT("a resized jet is still one"), !Bigger.Aircraft.bPropeller && Bigger.Aircraft.PropellerRadius == 0.0);
	TestTrue(TEXT("its nozzle is 1.3 times as far back, still behind its tail"),
		FMath::IsNearlyEqual(Bigger.NozzleLocation(0).X, -1300.0, 1.0e-6) && Bigger.NozzleLocation(0).X < Bigger.BodyBlock().Min.X);
	TestEqual(TEXT("and the speed it is trimmed for, 120 m/s"), Bigger.Aircraft.CruiseSpeedCmPerSec, 12000.0);
	TestTrue(TEXT("its cruise throttle is solved again for its own size"), FMath::Abs(Bigger.Aircraft.CruiseThrottle - 0.255) > 1.0e-3);

	// The plane is as it was: its engine turns a propeller at the nose, and pushes from the hub.
	FMantleChassisSpec Plane;
	if (LoadPlane(*this, Plane))
	{
		TestTrue(TEXT("the plane has a propeller"), Plane.Aircraft.bPropeller && Plane.Aircraft.PropellerRadius == 110.0 && Plane.Aircraft.PropellerRpm == 1200.0);
		TestTrue(TEXT("and its engine pushes from the hub, ahead of the nose"), Same(Plane.EngineLocation(0), Plane.PropellerHub(0)) && Plane.EngineLocation(0).X > Plane.BodyBlock().Max.X);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisJetAirframeTest,
	"Mantle.Vehicle.Chassis.JetAirframeHasANozzleBone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisJetAirframeTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisPlaneTestPrivate;
	FMantleChassisSpec Jet;
	FString Error;
	FMantleVehicleAsset Asset;
	if (!LoadJet(*this, Jet))
	{
		return false;
	}
	if (!TestTrue(*FString::Printf(TEXT("the jet makes an asset (%s)"), *Error), FMantleVehicleChassisBuilder::MakeAsset(Jet, Asset, Error)))
	{
		return false;
	}

	// The bones: the plane's, with the nozzle where the propeller was; every one a child of the root, unturned.
	const FName Names[] = { FMantleVehicleChassisBuilder::RootBoneName(), TEXT("wheel_f"), TEXT("wheel_rl"), TEXT("wheel_rr"), TEXT("aileron_l"), TEXT("aileron_r"),
		TEXT("elevator"), TEXT("rudder"), TEXT("nozzle") };
	if (!TestEqual(TEXT("nine bones"), Asset.Body.Bones.Num(), static_cast<int32>(UE_ARRAY_COUNT(Names))))
	{
		return false;
	}
	for (int32 Index = 0; Index < Asset.Body.Bones.Num(); ++Index)
	{
		TestEqual(*FString::Printf(TEXT("bone %d"), Index), Asset.Body.Bones[Index].Name, Names[Index]);
		TestEqual(*FString::Printf(TEXT("bone %d is a child of the root"), Index), Asset.Body.Bones[Index].ParentIndex, Index == 0 ? INDEX_NONE : 0);
		TestTrue(*FString::Printf(TEXT("bone %d is not turned"), Index), Asset.Body.ReferencePose[Index].GetRotation().IsIdentity(1.0e-6));
	}
	TestEqual(TEXT("no propeller bone"), BoneIndex(Asset, TEXT("propeller")), INDEX_NONE);
	TestEqual(TEXT("the nozzle bone is the builder's"), FMantleVehicleChassisBuilder::NozzleBoneName(0, 1), FName(TEXT("nozzle")));
	TestEqual(TEXT("and a twin's left one"), FMantleVehicleChassisBuilder::NozzleBoneName(0, 2), FName(TEXT("nozzle_l")));
	TestEqual(TEXT("the jet's engine bone is the nozzle"), FMantleVehicleChassisBuilder::EngineBoneName(Jet.Aircraft, 0), FName(TEXT("nozzle")));
	TestTrue(TEXT("the ailerons hinge at the middle of each half-wing's quarter-chord line"),
		Same(Asset.Body.ReferencePose[4].GetLocation(), FVector(-215.0, -245.0, 200.0)) && Same(Asset.Body.ReferencePose[5].GetLocation(), FVector(-215.0, 245.0, 200.0)));
	TestTrue(TEXT("the elevator and the rudder at their aerofoils"),
		Same(Asset.Body.ReferencePose[6].GetLocation(), FVector(-835.0, 0.0, 215.0)) && Same(Asset.Body.ReferencePose[7].GetLocation(), FVector(-800.0, 0.0, 440.0)));
	TestTrue(TEXT("the nozzle bone at the nozzle's exit, behind the fuselage"),
		Same(Asset.Body.ReferencePose[8].GetLocation(), Jet.NozzleLocation(0)) && Asset.Body.ReferencePose[8].GetLocation().X < Jet.BodyBlock().Min.X);

	// The skin: the nozzle's box on the nozzle bone, the fuselage and the intakes on the root.
	const FMantleSkinnedMeshLOD& LOD = Asset.Body.LODs[0];
	FVector Min;
	FVector Max;
	const FMantleChassisBlock Nozzle = Jet.NozzleBlock(0);
	if (TestTrue(TEXT("the nozzle bone has vertices"), BoundsOfBone(LOD, 8, Min, Max)))
	{
		TestTrue(*FString::Printf(TEXT("the nozzle's box on its bone, %s to %s"), *Min.ToString(), *Max.ToString()), Same(Min, Nozzle.Min) && Same(Max, Nozzle.Max));
		TestTrue(TEXT("the box is behind the fuselage, meeting its rear end"), Max.X <= Jet.BodyBlock().Min.X + 1.0e-3 && Min.X < Max.X);
	}
	if (TestTrue(TEXT("the root has vertices"), BoundsOfBone(LOD, 0, Min, Max)))
	{
		TestTrue(*FString::Printf(TEXT("the root carries the fuselage and an intake each side, %s to %s"), *Min.ToString(), *Max.ToString()),
			Same(Min, FVector(-950.0, -90.0, 110.0)) && Same(Max, FVector(550.0, 90.0, 290.0)));
	}
	TestEqual(TEXT("eight boxes (the fuselage, two half-wings, the tailplane, the fin, the nozzle and two intakes) and three wheels: 414 vertices"), LOD.Positions.Num(), 414);
	FString Why;
	TestTrue(*FString::Printf(TEXT("the body validates (%s)"), *Why), Asset.Body.Validate(Why));
	TestEqual(TEXT("the document collides the fuselage, wing, tailplane and fin, as a plane's does"), Asset.Desc.Collision.Num(), 4);

	// The exact body (the characters' collision) is every triangle but the wheels': the nozzle and the intakes too.
	TArray<FName> WheelBones;
	for (const FMantleVehicleWheelDesc& Wheel : Asset.Desc.Wheels)
	{
		WheelBones.Add(Wheel.Bone);
	}
	const TSharedPtr<const FMantleVehicleBodyCollision> Shape = FMantleVehicleBodyCollision::FromBody(Asset.Body, WheelBones);
	if (TestTrue(TEXT("the body has an exact collision shape"), Shape.IsValid()))
	{
		TestEqual(TEXT("eight boxes' triangles, and none of the wheels'"), Shape->NumTriangles(), 96);
	}

	// The plane still gets its propeller bone and blades, and no nozzle.
	FMantleChassisSpec Plane;
	FMantleVehicleAsset PlaneAsset;
	if (LoadPlane(*this, Plane) && TestTrue(*FString::Printf(TEXT("the plane makes an asset (%s)"), *Error), FMantleVehicleChassisBuilder::MakeAsset(Plane, PlaneAsset, Error)))
	{
		TestTrue(TEXT("the plane has its propeller bone and no nozzle"),
			BoneIndex(PlaneAsset, TEXT("propeller")) != INDEX_NONE && BoneIndex(PlaneAsset, TEXT("nozzle")) == INDEX_NONE);
		TestEqual(TEXT("its engine bone is the propeller"), FMantleVehicleChassisBuilder::EngineBoneName(Plane.Aircraft, 0), FName(TEXT("propeller")));
		TestEqual(TEXT("and its body is the 390 vertices it was"), PlaneAsset.Body.LODs[0].Positions.Num(), 390);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisJetTwinTest,
	"Mantle.Vehicle.Chassis.JetTwinNozzlesSitSideBySide",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisJetTwinTest::RunTest(const FString& Parameters)
{
	using namespace MantleChassisPlaneTestPrivate;
	FMantleChassisSpec Jet;
	FString Error;
	FMantleVehicleAsset Asset;
	if (!LoadJet(*this, Jet))
	{
		return false;
	}
	Jet.Aircraft.EngineCount = 2;
	if (!TestTrue(*FString::Printf(TEXT("a twin jet makes an asset (%s)"), *Error), FMantleVehicleChassisBuilder::MakeAsset(Jet, Asset, Error)))
	{
		return false;
	}
	TestEqual(TEXT("one more bone: a nozzle each side"), Asset.Body.Bones.Num(), 10);
	const int32 Left = BoneIndex(Asset, TEXT("nozzle_l"));
	const int32 Right = BoneIndex(Asset, TEXT("nozzle_r"));
	TestTrue(TEXT("nozzle_l and nozzle_r, and no nozzle or propeller"),
		Left != INDEX_NONE && Right != INDEX_NONE && BoneIndex(Asset, TEXT("nozzle")) == INDEX_NONE && BoneIndex(Asset, TEXT("propeller_l")) == INDEX_NONE);
	TestTrue(TEXT("the exits are a quarter of the fuselage's width either side of the centreline, the left on -Y"),
		Same(Jet.NozzleLocation(0), FVector(-1000.0, -30.0, 200.0)) && Same(Jet.NozzleLocation(1), FVector(-1000.0, 30.0, 200.0)));
	if (Left != INDEX_NONE && Right != INDEX_NONE)
	{
		TestTrue(TEXT("each bone is at its exit"),
			Same(Asset.Body.ReferencePose[Left].GetLocation(), Jet.NozzleLocation(0)) && Same(Asset.Body.ReferencePose[Right].GetLocation(), Jet.NozzleLocation(1)));
	}
	const FMantleChassisBlock LeftBox = Jet.NozzleBlock(0);
	const FMantleChassisBlock RightBox = Jet.NozzleBlock(1);
	TestTrue(TEXT("the two boxes sit side by side without touching, inside the fuselage's width"),
		LeftBox.Max.Y < RightBox.Min.Y && LeftBox.Min.Y >= -60.0 && RightBox.Max.Y <= 60.0);
	TestEqual(TEXT("414 vertices and a second nozzle's 24"), Asset.Body.LODs[0].Positions.Num(), 414 + 24);
	FString Why;
	TestTrue(*FString::Printf(TEXT("the body validates (%s)"), *Why), Asset.Body.Validate(Why));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleChassisJetPropellerOptionalTest,
	"Mantle.Vehicle.Chassis.PropellerBlockIsOptional",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleChassisJetPropellerOptionalTest::RunTest(const FString& Parameters)
{
	// An airframe with no propeller block is a jet; one with a propeller key that is not a whole block is refused, not read as a jet.
	FString Plane;
	FString Jet;
	FString Error;
	if (!TestTrue(TEXT("the presets read"), FMantleVehicleChassisBuilder::LoadPresetText(TEXT("plane"), Plane, Error) && FMantleVehicleChassisBuilder::LoadPresetText(TEXT("jet"), Jet, Error)))
	{
		return false;
	}
	FMantleChassisSpec Spec;
	const FString Unpropped = Plane.Replace(TEXT("\"propeller\": { \"radius\": 110, \"rpm\": 1200 },"), TEXT(""));
	TestNotEqual(TEXT("the plane's propeller block is taken out"), Unpropped, Plane);
	TestTrue(*FString::Printf(TEXT("the plane without it reads (%s)"), *Error), FMantleChassisSpec::Parse(Unpropped, Spec, Error));
	TestTrue(TEXT("as a jet"), Spec.Aircraft.bSet && !Spec.Aircraft.bPropeller);
	const auto Refused = [this, &Spec, &Error](const TCHAR* Label, const FString& Text, const TCHAR* Why)
	{
		TestFalse(*FString::Printf(TEXT("%s is refused"), Label), FMantleChassisSpec::Parse(Text, Spec, Error));
		TestTrue(*FString::Printf(TEXT("%s: and says why (%s)"), Label, *Error), Error.Contains(Why));
	};
	Refused(TEXT("a jet whose propeller is a number"), Jet.Replace(TEXT("\"airSpeedLimit\":"), TEXT("\"propeller\": 3, \"airSpeedLimit\":")), TEXT("chassis.aircraft.propeller"));
	Refused(TEXT("a jet whose propeller has no spin"), Jet.Replace(TEXT("\"airSpeedLimit\":"), TEXT("\"propeller\": { \"radius\": 50 }, \"airSpeedLimit\":")), TEXT("propeller.rpm"));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
