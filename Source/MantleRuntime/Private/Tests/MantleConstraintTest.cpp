// Copyright Joshua Eger. All Rights Reserved.
#include "MantleConstraint.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceTypes.h"

#include "Chaos/ChaosConstraintSettings.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/PhysicsConstraintActor.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

// Joints without the physics running: what the place parser reads and refuses for every type (sliders,
// ball joints and motors with the rest), the value rules a script's Constraints:Create shares
// (FMantleConstraints::CheckValues), the frames a slider's and a ball's joint are made with, the
// motor's gains, and an authoring build's engine constraint read back by the exporter
// (ConfigureEditorConstraint then ReadEditorConstraint), with the settings it refuses. That the motors
// turn and move the right way, as far as they should, is MantleConstraintMotorWorldTest's.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleConstraintTestPrivate
{
	/** A place with two physics crates (ent_crate_a, ent_crate_b) and one Constraint whose properties are Properties (the text between the braces). */
	FMantleParseResult ParseJoint(const FString& Properties)
	{
		/** One physics crate, ent_crate_<Suffix>, X cm along the world's X. */
		auto Crate = [](const TCHAR* Suffix, int32 X)
		{
			return FString::Printf(TEXT("{\"id\":\"ent_crate_%s\",\"type\":\"StaticMesh\",\"parent\":null,"
				"\"transform\":{\"location\":[%d,0,100],\"rotation\":[0,0,0],\"scale\":[1,1,1]},"
				"\"properties\":{\"mesh\":\"ast_cube\",\"physics\":{}}}"), Suffix, X);
		};
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_joints01\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Joints\",\"authorId\":\"usr_4b1d\"},\"settings\":{},"
				 "\"assets\":[{\"id\":\"ast_cube\",\"kind\":\"StaticMesh\",\"hash\":\"md5:0123456789abcdef\",\"bytes\":1024}],\"entities\":["
				 "{\"id\":\"ent_joint\",\"type\":\"Constraint\",\"parent\":null,"
				 "\"transform\":{\"location\":[0,0,100],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{%s}},%s,%s]}"),
			*Properties, *Crate(TEXT("a"), 0), *Crate(TEXT("b"), 300));
		return FMantlePlaceParser::ParseFromString(Json);
	}

	const FMantleConstraintProps* JointOf(const FMantleParseResult& Result)
	{
		return Result.Place.Entities.IsValidIndex(0) ? Result.Place.Entities[0].Props.TryGet<FMantleConstraintProps>() : nullptr;
	}

	bool HasErrorAt(const FMantleParseResult& Result, const FString& Path)
	{
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error) { return Error.Path == Path; });
	}

	/** An empty game world for one test, destroyed with it. */
	struct FJointWorld
	{
		UWorld* World = nullptr;

		FJointWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false,
				FName(*FString::Printf(TEXT("MantleConstraintTestWorld_%s"), *FGuid::NewGuid().ToString())));
		}

		~FJointWorld()
		{
			if (World)
			{
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
				if (World->IsRooted())
				{
					World->RemoveFromRoot();
				}
			}
		}
	};

	/** An actor whose root is a box at Location, set to simulate physics. */
	AActor* SpawnBody(UWorld* World, const FVector& Location)
	{
		AActor* Actor = World->SpawnActor<AActor>();
		if (!Actor)
		{
			return nullptr;
		}
		UBoxComponent* Box = NewObject<UBoxComponent>(Actor);
		Actor->SetRootComponent(Box);
		Box->SetBoxExtent(FVector(50.0));
		Box->SetMobility(EComponentMobility::Movable);
		Box->BodyInstance.bSimulatePhysics = true;
		Box->SetWorldLocation(Location);
		Box->RegisterComponent();
		return Actor;
	}

	/** An engine constraint set up as Props between A and B (null: the world), then read back; false with the problem. */
	bool RoundTrip(UWorld* World, const FMantleConstraintProps& Props, AActor* A, AActor* B, FMantleConstraintProps& OutProps, FString& OutProblem)
	{
		APhysicsConstraintActor* Editor = World->SpawnActor<APhysicsConstraintActor>();
		if (!Editor)
		{
			OutProblem = TEXT("no constraint actor");
			return false;
		}
		FMantleConstraints::ConfigureEditorConstraint(Editor->GetConstraintComp(), Props, A, B);
		return FMantleConstraints::ReadEditorConstraint(Editor->GetConstraintComp(), A, B, OutProps, OutProblem);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleConstraintParseTest,
	"Mantle.Runtime.Constraint.Parser.ReadsSlidersBallsAndMotors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleConstraintParseTest::RunTest(const FString& Parameters)
{
	using namespace MantleConstraintTestPrivate;

	const FMantleParseResult Drawer = ParseJoint(TEXT(
		"\"type\":\"slider\",\"entityA\":\"ent_crate_a\",\"entityB\":\"ent_crate_b\",\"axis\":[1,0,0],\"limits\":[0,40],"
		"\"motor\":{\"mode\":\"position\",\"target\":40,\"maxForce\":200},\"breakForce\":5000"));
	if (!TestTrue(TEXT("a slider with limits and a motor parses"), Drawer.bSuccess))
	{
		AddError(Drawer.ErrorSummary());
		return false;
	}
	if (const FMantleConstraintProps* Props = JointOf(Drawer); TestNotNull(TEXT("the slider"), Props))
	{
		TestTrue(TEXT("a slider"), Props->Type == EMantleConstraintType::Slider);
		TestTrue(TEXT("along X"), Props->Axis.Equals(FVector::XAxisVector));
		TestTrue(TEXT("limited 0..40"), Props->bLimited && Props->LimitMin == 0.0 && Props->LimitMax == 40.0);
		TestTrue(TEXT("with its motor"), Props->Motor.IsSet() && Props->Motor->Mode == EMantleMotorMode::Position
			&& Props->Motor->Target == 40.0 && Props->Motor->MaxForce == 200.0);
		TestEqual(TEXT("and its break force"), Props->BreakForce.Get(0.0), 5000.0);
	}

	const FMantleParseResult Lantern = ParseJoint(TEXT("\"type\":\"ball\",\"entityA\":\"ent_crate_a\",\"axis\":[0,0,-1],\"swingLimit\":45,\"twistLimit\":0"));
	TestTrue(TEXT("a ball with both limits parses"), Lantern.bSuccess);
	if (const FMantleConstraintProps* Props = JointOf(Lantern))
	{
		TestTrue(TEXT("a ball"), Props->Type == EMantleConstraintType::Ball);
		TestEqual(TEXT("its swing limit"), Props->SwingLimit.Get(-1.0), 45.0);
		TestEqual(TEXT("no twist"), Props->TwistLimit.Get(-1.0), 0.0);
		TestTrue(TEXT("about -Z"), Props->Axis.Equals(-FVector::ZAxisVector));
	}
	const FMantleParseResult Bag = ParseJoint(TEXT("\"type\":\"ball\",\"entityA\":\"ent_crate_a\""));
	TestTrue(TEXT("a free ball parses"), Bag.bSuccess);
	if (const FMantleConstraintProps* Props = JointOf(Bag))
	{
		TestFalse(TEXT("swings freely"), Props->SwingLimit.IsSet());
		TestFalse(TEXT("twists freely"), Props->TwistLimit.IsSet());
	}

	const FMantleParseResult Fan = ParseJoint(TEXT(
		"\"type\":\"hinge\",\"entityA\":\"ent_crate_a\",\"motor\":{\"mode\":\"speed\",\"target\":-3600,\"maxForce\":10000000}"));
	TestTrue(TEXT("a hinge speed motor at the edges of its ranges parses"), Fan.bSuccess);
	if (const FMantleConstraintProps* Props = JointOf(Fan))
	{
		TestTrue(TEXT("a speed motor"), Props->Motor.IsSet() && Props->Motor->Mode == EMantleMotorMode::Speed && Props->Motor->Target == -3600.0);
	}
	TestTrue(TEXT("a free slider's position target may be 1 km away"), ParseJoint(TEXT(
		"\"type\":\"slider\",\"entityA\":\"ent_crate_a\",\"motor\":{\"mode\":\"position\",\"target\":-100000,\"maxForce\":1}")).bSuccess);
	TestTrue(TEXT("a slider limited 1 km either way parses"), ParseJoint(TEXT(
		"\"type\":\"slider\",\"entityA\":\"ent_crate_a\",\"limits\":[-100000,100000]")).bSuccess);
	TestTrue(TEXT("a hinge's motor at the edge of its limits parses"), ParseJoint(TEXT(
		"\"type\":\"hinge\",\"entityA\":\"ent_crate_a\",\"limits\":[-30,60],\"motor\":{\"mode\":\"position\",\"target\":-30,\"maxForce\":5}")).bSuccess);
	TestTrue(TEXT("a speed motor is not held to the limits"), ParseJoint(TEXT(
		"\"type\":\"hinge\",\"entityA\":\"ent_crate_a\",\"limits\":[0,10],\"motor\":{\"mode\":\"speed\",\"target\":720,\"maxForce\":5}")).bSuccess);

	struct FRefusal
	{
		const TCHAR* What;
		const TCHAR* Properties;
		const TCHAR* Path;
	};
	const FRefusal Refusals[] = {
		{ TEXT("an unknown type"), TEXT("\"type\":\"spring\",\"entityA\":\"ent_crate_a\""), TEXT("/entities/0/properties/type") },
		{ TEXT("an axis on a rope"), TEXT("\"type\":\"rope\",\"entityA\":\"ent_crate_a\",\"axis\":[1,0,0]"), TEXT("/entities/0/properties/axis") },
		{ TEXT("limits on a ball"), TEXT("\"type\":\"ball\",\"entityA\":\"ent_crate_a\",\"limits\":[-10,10]"), TEXT("/entities/0/properties/limits") },
		{ TEXT("a swing limit on a hinge"), TEXT("\"type\":\"hinge\",\"entityA\":\"ent_crate_a\",\"swingLimit\":30"), TEXT("/entities/0/properties/swingLimit") },
		{ TEXT("a twist limit on a slider"), TEXT("\"type\":\"slider\",\"entityA\":\"ent_crate_a\",\"twistLimit\":30"), TEXT("/entities/0/properties/twistLimit") },
		{ TEXT("a motor on a ball"), TEXT("\"type\":\"ball\",\"entityA\":\"ent_crate_a\",\"motor\":{\"mode\":\"speed\",\"target\":1,\"maxForce\":1}"),
			TEXT("/entities/0/properties/motor") },
		{ TEXT("a motor on a weld"), TEXT("\"type\":\"weld\",\"entityA\":\"ent_crate_a\",\"motor\":{\"mode\":\"speed\",\"target\":1,\"maxForce\":1}"),
			TEXT("/entities/0/properties/motor") },
		{ TEXT("a zero slider axis"), TEXT("\"type\":\"slider\",\"entityA\":\"ent_crate_a\",\"axis\":[0,0,0]"), TEXT("/entities/0/properties/axis") },
		{ TEXT("a slider limit past 1 km"), TEXT("\"type\":\"slider\",\"entityA\":\"ent_crate_a\",\"limits\":[0,100001]"), TEXT("/entities/0/properties/limits/1") },
		{ TEXT("a slider's lower limit above 0"), TEXT("\"type\":\"slider\",\"entityA\":\"ent_crate_a\",\"limits\":[10,40]"), TEXT("/entities/0/properties/limits/0") },
		{ TEXT("a hinge limit past 180, though a slider's may be"), TEXT("\"type\":\"hinge\",\"entityA\":\"ent_crate_a\",\"limits\":[-181,0]"),
			TEXT("/entities/0/properties/limits/0") },
		{ TEXT("a swing limit of 0"), TEXT("\"type\":\"ball\",\"entityA\":\"ent_crate_a\",\"swingLimit\":0"), TEXT("/entities/0/properties/swingLimit") },
		{ TEXT("a twist limit below 0"), TEXT("\"type\":\"ball\",\"entityA\":\"ent_crate_a\",\"twistLimit\":-1"), TEXT("/entities/0/properties/twistLimit") },
		{ TEXT("a twist limit past 180"), TEXT("\"type\":\"ball\",\"entityA\":\"ent_crate_a\",\"twistLimit\":181"), TEXT("/entities/0/properties/twistLimit") },
		{ TEXT("a motor that is not an object"), TEXT("\"type\":\"hinge\",\"entityA\":\"ent_crate_a\",\"motor\":90"), TEXT("/entities/0/properties/motor") },
		{ TEXT("a motor without maxForce"), TEXT("\"type\":\"hinge\",\"entityA\":\"ent_crate_a\",\"motor\":{\"mode\":\"position\",\"target\":1}"),
			TEXT("/entities/0/properties/motor/maxForce") },
		{ TEXT("a motor without a mode"), TEXT("\"type\":\"hinge\",\"entityA\":\"ent_crate_a\",\"motor\":{\"target\":1,\"maxForce\":1}"),
			TEXT("/entities/0/properties/motor/mode") },
		{ TEXT("an unknown motor mode"), TEXT("\"type\":\"hinge\",\"entityA\":\"ent_crate_a\",\"motor\":{\"mode\":\"torque\",\"target\":1,\"maxForce\":1}"),
			TEXT("/entities/0/properties/motor/mode") },
		{ TEXT("an unknown motor key"), TEXT("\"type\":\"hinge\",\"entityA\":\"ent_crate_a\",\"motor\":{\"mode\":\"speed\",\"target\":1,\"maxForce\":1,\"stiffness\":2}"),
			TEXT("/entities/0/properties/motor/stiffness") },
		{ TEXT("a maxForce of 0"), TEXT("\"type\":\"slider\",\"entityA\":\"ent_crate_a\",\"motor\":{\"mode\":\"speed\",\"target\":1,\"maxForce\":0}"),
			TEXT("/entities/0/properties/motor/maxForce") },
		{ TEXT("a maxForce past the cap"), TEXT("\"type\":\"slider\",\"entityA\":\"ent_crate_a\",\"motor\":{\"mode\":\"speed\",\"target\":1,\"maxForce\":10000001}"),
			TEXT("/entities/0/properties/motor/maxForce") },
		{ TEXT("a hinge position past 180"), TEXT("\"type\":\"hinge\",\"entityA\":\"ent_crate_a\",\"motor\":{\"mode\":\"position\",\"target\":181,\"maxForce\":1}"),
			TEXT("/entities/0/properties/motor/target") },
		{ TEXT("a hinge speed past 3600"), TEXT("\"type\":\"hinge\",\"entityA\":\"ent_crate_a\",\"motor\":{\"mode\":\"speed\",\"target\":3601,\"maxForce\":1}"),
			TEXT("/entities/0/properties/motor/target") },
		{ TEXT("a slider speed past 10000"), TEXT("\"type\":\"slider\",\"entityA\":\"ent_crate_a\",\"motor\":{\"mode\":\"speed\",\"target\":-10001,\"maxForce\":1}"),
			TEXT("/entities/0/properties/motor/target") },
		{ TEXT("a position target outside the limits"),
			TEXT("\"type\":\"slider\",\"entityA\":\"ent_crate_a\",\"limits\":[0,40],\"motor\":{\"mode\":\"position\",\"target\":-5,\"maxForce\":1}"),
			TEXT("/entities/0/properties/motor/target") },
	};
	for (const FRefusal& Refusal : Refusals)
	{
		const FMantleParseResult Result = ParseJoint(Refusal.Properties);
		TestFalse(FString::Printf(TEXT("%s is refused"), Refusal.What), Result.bSuccess);
		TestTrue(FString::Printf(TEXT("%s is refused at %s (%s)"), Refusal.What, Refusal.Path, *Result.ErrorSummary()), HasErrorAt(Result, Refusal.Path));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleConstraintValuesTest,
	"Mantle.Runtime.Constraint.Values.SlidersBallsAndMotors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleConstraintValuesTest::RunTest(const FString& Parameters)
{
	FMantleConstraintProps Slider;
	Slider.Type = EMantleConstraintType::Slider;
	Slider.Axis = FVector(2.0, 0.0, 0.0);
	Slider.bLimited = true;
	Slider.LimitMin = -20.0;
	Slider.LimitMax = 60.0;
	Slider.Motor = FMantleConstraintMotor{ EMantleMotorMode::Position, 60.0, 100.0 };
	FString Error;
	TestTrue(TEXT("a limited slider with a motor at its end passes"), FMantleConstraints::CheckValues(Slider, Error));

	FMantleConstraintProps Outside = Slider;
	Outside.Motor->Target = 61.0;
	TestFalse(TEXT("a position target past the limits fails"), FMantleConstraints::CheckValues(Outside, Error));
	TestTrue(TEXT("naming the motor's target"), Error.StartsWith(TEXT("Motor.Target")));
	Outside.Motor = FMantleConstraintMotor{ EMantleMotorMode::Speed, 9000.0, 100.0 };
	TestTrue(TEXT("a speed is not held to the limits"), FMantleConstraints::CheckValues(Outside, Error));
	Outside.Motor->Target = 10001.0;
	TestFalse(TEXT("a slider's speed past 10000 cm a second fails"), FMantleConstraints::CheckValues(Outside, Error));
	Outside.Motor = FMantleConstraintMotor{ EMantleMotorMode::Speed, 1.0, 0.0 };
	TestFalse(TEXT("a maxForce of 0 fails"), FMantleConstraints::CheckValues(Outside, Error));
	TestTrue(TEXT("naming the motor's maxForce"), Error.StartsWith(TEXT("Motor.MaxForce")));

	FMantleConstraintProps Rail = Slider;
	Rail.Motor.Reset();
	Rail.LimitMin = -100001.0;
	TestFalse(TEXT("a slider limit past 1 km fails"), FMantleConstraints::CheckValues(Rail, Error));
	Rail.LimitMin = -100000.0;
	Rail.Axis = FVector::ZeroVector;
	TestFalse(TEXT("a slider with no axis fails"), FMantleConstraints::CheckValues(Rail, Error));

	FMantleConstraintProps Ball;
	Ball.Type = EMantleConstraintType::Ball;
	Ball.SwingLimit = 180.0;
	Ball.TwistLimit = 0.0;
	TestTrue(TEXT("a ball with a 180 degree cone and no twist passes"), FMantleConstraints::CheckValues(Ball, Error));
	Ball.SwingLimit = 0.0;
	TestFalse(TEXT("a swing limit of 0 fails"), FMantleConstraints::CheckValues(Ball, Error));
	Ball.SwingLimit.Reset();
	Ball.TwistLimit = 180.5;
	TestFalse(TEXT("a twist limit past 180 fails"), FMantleConstraints::CheckValues(Ball, Error));

	FMantleConstraintMotor HingeMotor{ EMantleMotorMode::Position, -180.0, 1.0 };
	TestTrue(TEXT("a free hinge may be driven to -180"), FMantleConstraints::CheckMotor(EMantleConstraintType::Hinge, false, 0.0, 0.0, HingeMotor, Error));
	HingeMotor.Target = 180.5;
	TestFalse(TEXT("but not past 180"), FMantleConstraints::CheckMotor(EMantleConstraintType::Hinge, false, 0.0, 0.0, HingeMotor, Error));
	TestTrue(TEXT("naming Target"), Error.StartsWith(TEXT("Target")));
	TestEqual(TEXT("a hinge's speed range"), FMantleConstraints::MotorTargetMost(EMantleConstraintType::Hinge, EMantleMotorMode::Speed),
		FMantlePlaceLimits::MaxMotorAngularSpeed);
	TestEqual(TEXT("a slider's position range"), FMantleConstraints::MotorTargetMost(EMantleConstraintType::Slider, EMantleMotorMode::Position),
		FMantlePlaceLimits::MaxSliderTravel);

	// The gains: a critically damped spring at the response frequency on what the motor moves, Chaos's own
	// scale undone; a speed motor has no spring.
	const double Omega = 2.0 * UE_DOUBLE_PI * FMantleConstraints::MotorPositionResponseHz;
	const FMantleMotorGains Linear = FMantleConstraints::MotorGains(EMantleConstraintType::Slider, EMantleMotorMode::Position, 10.0);
	TestEqual(TEXT("a slider's spring"), Linear.Stiffness, 10.0 * Omega * Omega / Chaos::ConstraintSettings::LinearDriveStiffnessScale(), 1e-6);
	TestEqual(TEXT("a slider's damper"), Linear.Damping, 2.0 * 10.0 * Omega / Chaos::ConstraintSettings::LinearDriveDampingScale(), 1e-6);
	const FMantleMotorGains Angular = FMantleConstraints::MotorGains(EMantleConstraintType::Hinge, EMantleMotorMode::Position, 1000.0);
	TestEqual(TEXT("a hinge's spring"), Angular.Stiffness, 1000.0 * Omega * Omega / Chaos::ConstraintSettings::AngularDriveStiffnessScale(), 1e-3);
	const FMantleMotorGains Speed = FMantleConstraints::MotorGains(EMantleConstraintType::Hinge, EMantleMotorMode::Speed, 1000.0);
	TestEqual(TEXT("a speed motor has no spring"), Speed.Stiffness, 0.0);
	TestEqual(TEXT("a speed motor's damper"), Speed.Damping,
		1000.0 * FMantleConstraints::MotorSpeedResponse / Chaos::ConstraintSettings::AngularDriveDampingScale(), 1e-3);
	TestTrue(TEXT("nothing to move still drives"), FMantleConstraints::MotorGains(EMantleConstraintType::Slider, EMantleMotorMode::Position, 0.0).Stiffness > 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleConstraintFramesTest,
	"Mantle.Runtime.Constraint.Frames.SliderMiddleAndBallAxis",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleConstraintFramesTest::RunTest(const FString& Parameters)
{
	// entityA turned 90 degrees of yaw: its own X is the world's Y.
	const FTransform A(FRotator(0.0, 90.0, 0.0), FVector(100.0, 0.0, 0.0));

	FMantleConstraintProps Slider;
	Slider.Type = EMantleConstraintType::Slider;
	Slider.Axis = FVector::XAxisVector;
	Slider.bLimited = true;
	Slider.LimitMin = 0.0;
	Slider.LimitMax = 40.0;
	const FMantleConstraintFrames SliderFrames = FMantleConstraints::MakeFrames(Slider, A, nullptr);
	TestTrue(TEXT("a slider's axis is entityA's X, turned with it"), SliderFrames.PrimaryA.Equals(FVector::YAxisVector, 1e-6));
	TestEqual(TEXT("its middle"), SliderFrames.Middle, 20.0);
	TestEqual(TEXT("its half range"), SliderFrames.HalfRange, 20.0);
	TestTrue(TEXT("entityB's anchor is at the middle of the range along the axis"),
		SliderFrames.AnchorB.Equals(SliderFrames.AnchorA + FVector(0.0, 20.0, 0.0), 1e-6));
	TestTrue(TEXT("and not turned"), SliderFrames.SecondaryB.Equals(SliderFrames.SecondaryA, 1e-6));

	Slider.bLimited = false;
	const FMantleConstraintFrames FreeFrames = FMantleConstraints::MakeFrames(Slider, A, nullptr);
	TestTrue(TEXT("a free slider's anchors are together"), FreeFrames.AnchorB.Equals(FreeFrames.AnchorA, 1e-6));
	TestEqual(TEXT("with no middle"), FreeFrames.Middle, 0.0);

	FMantleConstraintProps Ball;
	Ball.Type = EMantleConstraintType::Ball;
	Ball.Axis = FVector(0.0, 0.0, -3.0);
	Ball.SwingLimit = 30.0;
	const FMantleConstraintFrames BallFrames = FMantleConstraints::MakeFrames(Ball, A, nullptr);
	TestTrue(TEXT("a ball's axis is its own, normalized"), BallFrames.PrimaryA.Equals(-FVector::ZAxisVector, 1e-6));
	TestTrue(TEXT("both frames share it"), BallFrames.PrimaryB.Equals(BallFrames.PrimaryA, 1e-6) && BallFrames.SecondaryB.Equals(BallFrames.SecondaryA, 1e-6));
	TestEqual(TEXT("a ball has no middle"), BallFrames.Middle, 0.0);

	// A limited hinge's middle turns entityB's frame, as before; its anchors stay together.
	FMantleConstraintProps Hinge;
	Hinge.Type = EMantleConstraintType::Hinge;
	Hinge.bLimited = true;
	Hinge.LimitMin = -30.0;
	Hinge.LimitMax = 90.0;
	const FMantleConstraintFrames HingeFrames = FMantleConstraints::MakeFrames(Hinge, A, nullptr);
	TestEqual(TEXT("a hinge's middle"), HingeFrames.Middle, 30.0);
	TestEqual(TEXT("a hinge's half range"), HingeFrames.HalfRange, 60.0);
	TestTrue(TEXT("a hinge's anchors stay together"), HingeFrames.AnchorB.Equals(HingeFrames.AnchorA, 1e-6));
	TestTrue(TEXT("its frame B is turned by the middle"),
		HingeFrames.SecondaryB.Equals(FQuat(HingeFrames.PrimaryA, FMath::DegreesToRadians(30.0)).RotateVector(HingeFrames.SecondaryA), 1e-6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleConstraintEditorRoundTripTest,
	"Mantle.Runtime.Constraint.Editor.SlidersBallsAndMotorsRoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleConstraintEditorRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace MantleConstraintTestPrivate;

	FJointWorld Scratch;
	UWorld* World = Scratch.World;
	if (!TestNotNull(TEXT("a world"), World))
	{
		return false;
	}
	AActor* A = SpawnBody(World, FVector(0.0, 0.0, 200.0));
	AActor* B = SpawnBody(World, FVector(300.0, 0.0, 200.0));
	if (!TestNotNull(TEXT("body A"), A) || !TestNotNull(TEXT("body B"), B))
	{
		return false;
	}

	// A slider limited the same distance either way, with a position motor: as it went in.
	FMantleConstraintProps Slider;
	Slider.Type = EMantleConstraintType::Slider;
	Slider.Axis = FVector::YAxisVector;
	Slider.bLimited = true;
	Slider.LimitMin = -50.0;
	Slider.LimitMax = 50.0;
	Slider.Motor = FMantleConstraintMotor{ EMantleMotorMode::Position, 30.0, 250.0 };
	Slider.BreakForce = 900.0;
	FMantleConstraintProps Read;
	FString Problem;
	if (TestTrue(FString::Printf(TEXT("a slider reads back (%s)"), *Problem), RoundTrip(World, Slider, A, B, Read, Problem)))
	{
		TestTrue(TEXT("as a slider"), Read.Type == EMantleConstraintType::Slider);
		TestTrue(TEXT("along its axis"), Read.Axis.Equals(FVector::YAxisVector, 1e-4));
		TestTrue(TEXT("limited -50..50"), Read.bLimited && FMath::IsNearlyEqual(Read.LimitMin, -50.0, 1e-3) && FMath::IsNearlyEqual(Read.LimitMax, 50.0, 1e-3));
		TestTrue(TEXT("with its position motor"), Read.Motor.IsSet() && Read.Motor->Mode == EMantleMotorMode::Position
			&& FMath::IsNearlyEqual(Read.Motor->Target, 30.0, 1e-3) && FMath::IsNearlyEqual(Read.Motor->MaxForce, 250.0, 1e-3));
		TestTrue(TEXT("and its break force"), FMath::IsNearlyEqual(Read.BreakForce.Get(0.0), 900.0, 1e-2));
	}

	// An off-center slider: the engine's constraint holds the range's length, centered on the start.
	FMantleConstraintProps Drawer = Slider;
	Drawer.LimitMin = 0.0;
	Drawer.LimitMax = 40.0;
	Drawer.Motor.Reset();
	AddExpectedMessage(TEXT("MANTLE_CONSTRAINT_EDITOR"), EAutomationExpectedMessageFlags::Contains, 1);
	if (TestTrue(FString::Printf(TEXT("an off-center slider reads back (%s)"), *Problem), RoundTrip(World, Drawer, A, B, Read, Problem)))
	{
		TestTrue(TEXT("as -20..20"), FMath::IsNearlyEqual(Read.LimitMin, -20.0, 1e-3) && FMath::IsNearlyEqual(Read.LimitMax, 20.0, 1e-3));
	}

	// A ball joint to the world with a cone and a twist limit.
	FMantleConstraintProps Ball;
	Ball.Type = EMantleConstraintType::Ball;
	Ball.Axis = -FVector::ZAxisVector;
	Ball.SwingLimit = 35.0;
	Ball.TwistLimit = 10.0;
	if (TestTrue(FString::Printf(TEXT("a ball reads back (%s)"), *Problem), RoundTrip(World, Ball, A, nullptr, Read, Problem)))
	{
		TestTrue(TEXT("as a ball"), Read.Type == EMantleConstraintType::Ball);
		TestTrue(TEXT("about its axis"), Read.Axis.Equals(-FVector::ZAxisVector, 1e-4));
		TestTrue(TEXT("its cone"), FMath::IsNearlyEqual(Read.SwingLimit.Get(-1.0), 35.0, 1e-3));
		TestTrue(TEXT("its twist"), FMath::IsNearlyEqual(Read.TwistLimit.Get(-1.0), 10.0, 1e-3));
		TestFalse(TEXT("no motor"), Read.Motor.IsSet());
	}
	FMantleConstraintProps FreeBall;
	FreeBall.Type = EMantleConstraintType::Ball;
	if (TestTrue(FString::Printf(TEXT("a free ball reads back (%s)"), *Problem), RoundTrip(World, FreeBall, A, nullptr, Read, Problem)))
	{
		TestTrue(TEXT("swinging and twisting freely"), Read.Type == EMantleConstraintType::Ball && !Read.SwingLimit.IsSet() && !Read.TwistLimit.IsSet());
	}

	// Hinges with motors: a position motor inside off-center limits, and a speed motor.
	FMantleConstraintProps Door;
	Door.Type = EMantleConstraintType::Hinge;
	Door.bLimited = true;
	Door.LimitMin = -10.0;
	Door.LimitMax = 110.0;
	Door.Motor = FMantleConstraintMotor{ EMantleMotorMode::Position, 90.0, 400.0 };
	if (TestTrue(FString::Printf(TEXT("a motor hinge reads back (%s)"), *Problem), RoundTrip(World, Door, A, B, Read, Problem)))
	{
		TestTrue(TEXT("its limits"), FMath::IsNearlyEqual(Read.LimitMin, -10.0, 1e-2) && FMath::IsNearlyEqual(Read.LimitMax, 110.0, 1e-2));
		TestTrue(TEXT("its position motor"), Read.Motor.IsSet() && Read.Motor->Mode == EMantleMotorMode::Position
			&& FMath::IsNearlyEqual(Read.Motor->Target, 90.0, 1e-2) && FMath::IsNearlyEqual(Read.Motor->MaxForce, 400.0, 1e-3));
	}
	FMantleConstraintProps FanAxle;
	FanAxle.Type = EMantleConstraintType::Hinge;
	FanAxle.Axis = FVector::YAxisVector;
	FanAxle.Motor = FMantleConstraintMotor{ EMantleMotorMode::Speed, -720.0, 50.0 };
	if (TestTrue(FString::Printf(TEXT("a speed hinge reads back (%s)"), *Problem), RoundTrip(World, FanAxle, A, nullptr, Read, Problem)))
	{
		TestTrue(TEXT("its speed motor"), Read.Motor.IsSet() && Read.Motor->Mode == EMantleMotorMode::Speed
			&& FMath::IsNearlyEqual(Read.Motor->Target, -720.0, 1e-2));
	}

	// What the exporter leaves out, each with the setting to change.
	struct FRefused
	{
		const TCHAR* What;
		TFunction<void(FConstraintInstance&)> Change;
		const TCHAR* ProblemContains;
	};
	const FRefused Refused[] = {
		{ TEXT("an oval cone"), [](FConstraintInstance& Instance)
			{
				Instance.SetLinearLimits(LCM_Locked, LCM_Locked, LCM_Locked, 0.0f);
				Instance.SetAngularSwing1Limit(ACM_Limited, 30.0f);
				Instance.SetAngularSwing2Limit(ACM_Limited, 45.0f);
			}, TEXT("oval cone") },
		{ TEXT("one swing and the twist"), [](FConstraintInstance& Instance)
			{
				Instance.SetLinearLimits(LCM_Locked, LCM_Locked, LCM_Locked, 0.0f);
				Instance.SetAngularSwing1Limit(ACM_Free, 0.0f);
				Instance.SetAngularSwing2Limit(ACM_Locked, 0.0f);
				Instance.SetAngularTwistLimit(ACM_Free, 0.0f);
			}, TEXT("two axes") },
		{ TEXT("a slider that turns"), [](FConstraintInstance& Instance)
			{
				Instance.SetLinearLimits(LCM_Free, LCM_Locked, LCM_Locked, 0.0f);
				Instance.SetAngularTwistLimit(ACM_Free, 0.0f);
			}, TEXT("also turns") },
		{ TEXT("two free linear axes"), [](FConstraintInstance& Instance)
			{
				Instance.SetLinearLimits(LCM_Free, LCM_Free, LCM_Locked, 0.0f);
			}, TEXT("more than one axis") },
		{ TEXT("a SLERP hinge drive"), [](FConstraintInstance& Instance)
			{
				Instance.SetLinearLimits(LCM_Locked, LCM_Locked, LCM_Locked, 0.0f);
				Instance.SetAngularTwistLimit(ACM_Free, 0.0f);
				Instance.SetAngularDriveMode(EAngularDriveMode::SLERP);
				Instance.SetOrientationDriveSLERP(true);
			}, TEXT("SLERP") },
		{ TEXT("a capped drive in acceleration mode"), [](FConstraintInstance& Instance)
			{
				Instance.SetLinearLimits(LCM_Free, LCM_Locked, LCM_Locked, 0.0f);
				Instance.SetLinearDriveAccelerationMode(true);
				Instance.SetLinearDriveParams(100.0f, 10.0f, 500.0f);
				Instance.SetLinearPositionDrive(true, false, false);
			}, TEXT("Acceleration mode") },
		{ TEXT("a drive on a ball"), [](FConstraintInstance& Instance)
			{
				Instance.SetLinearLimits(LCM_Locked, LCM_Locked, LCM_Locked, 0.0f);
				Instance.SetAngularSwing1Limit(ACM_Free, 0.0f);
				Instance.SetAngularSwing2Limit(ACM_Free, 0.0f);
				Instance.SetAngularDriveMode(EAngularDriveMode::TwistAndSwing);
				Instance.SetAngularVelocityDriveTwistAndSwing(false, true);
			}, TEXT("does not publish") },
		{ TEXT("a slider drive on another axis"), [](FConstraintInstance& Instance)
			{
				Instance.SetLinearLimits(LCM_Free, LCM_Locked, LCM_Locked, 0.0f);
				Instance.SetLinearVelocityDrive(false, true, false);
			}, TEXT("does not move along") },
		{ TEXT("a drive to a position and at a speed"), [](FConstraintInstance& Instance)
			{
				Instance.SetLinearLimits(LCM_Free, LCM_Locked, LCM_Locked, 0.0f);
				Instance.SetLinearDriveAccelerationMode(false);
				Instance.SetLinearPositionDrive(true, false, false);
				Instance.SetLinearVelocityDrive(true, false, false);
			}, TEXT("both to a position and at a speed") },
		{ TEXT("a hinge driven past its limits"), [](FConstraintInstance& Instance)
			{
				Instance.SetLinearLimits(LCM_Locked, LCM_Locked, LCM_Locked, 0.0f);
				Instance.SetAngularTwistLimit(ACM_Limited, 20.0f);
				Instance.SetAngularDriveMode(EAngularDriveMode::TwistAndSwing);
				Instance.SetAngularDriveAccelerationMode(false);
				Instance.SetAngularOrientationTarget(FQuat(FVector::XAxisVector, FMath::DegreesToRadians(-90.0)));
				Instance.SetOrientationDriveTwistAndSwing(true, false);
			}, TEXT("outside the joint's limits") },
	};
	for (const FRefused& Case : Refused)
	{
		APhysicsConstraintActor* Editor = World->SpawnActor<APhysicsConstraintActor>();
		if (!TestNotNull(Case.What, Editor))
		{
			continue;
		}
		UPhysicsConstraintComponent* Component = Editor->GetConstraintComp();
		FConstraintInstance& Instance = Component->ConstraintInstance;
		// Every motion locked to begin with, so each case says only what it changes.
		Instance.SetLinearLimits(LCM_Locked, LCM_Locked, LCM_Locked, 0.0f);
		Instance.SetAngularSwing1Limit(ACM_Locked, 0.0f);
		Instance.SetAngularSwing2Limit(ACM_Locked, 0.0f);
		Instance.SetAngularTwistLimit(ACM_Locked, 0.0f);
		Case.Change(Instance);
		FMantleConstraintProps Ignored;
		FString CaseProblem;
		TestFalse(FString::Printf(TEXT("%s is left out"), Case.What), FMantleConstraints::ReadEditorConstraint(Component, A, B, Ignored, CaseProblem));
		TestTrue(FString::Printf(TEXT("%s is named (%s)"), Case.What, *CaseProblem), CaseProblem.Contains(Case.ProblemContains));
	}

	// Unreal's "no cap" (Max Force 0) is the most a Mantle motor has.
	{
		APhysicsConstraintActor* Editor = World->SpawnActor<APhysicsConstraintActor>();
		if (TestNotNull(TEXT("an uncapped drive"), Editor))
		{
			FConstraintInstance& Instance = Editor->GetConstraintComp()->ConstraintInstance;
			Instance.SetLinearLimits(LCM_Free, LCM_Locked, LCM_Locked, 0.0f);
			Instance.SetAngularSwing1Limit(ACM_Locked, 0.0f);
			Instance.SetAngularSwing2Limit(ACM_Locked, 0.0f);
			Instance.SetAngularTwistLimit(ACM_Locked, 0.0f);
			Instance.SetLinearDriveParams(0.0f, 50.0f, 0.0f);
			Instance.SetLinearVelocityTarget(FVector(120.0, 0.0, 0.0));
			Instance.SetLinearVelocityDrive(true, false, false);
			if (TestTrue(TEXT("an uncapped slider drive reads back"), FMantleConstraints::ReadEditorConstraint(Editor->GetConstraintComp(), A, B, Read, Problem)))
			{
				TestTrue(TEXT("as a speed motor of 120 cm a second"), Read.Motor.IsSet() && Read.Motor->Mode == EMantleMotorMode::Speed
					&& FMath::IsNearlyEqual(Read.Motor->Target, 120.0, 1e-3));
				TestEqual(TEXT("with the most force"), Read.Motor.IsSet() ? Read.Motor->MaxForce : 0.0, FMantlePlaceLimits::MaxMotorForce);
			}
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
