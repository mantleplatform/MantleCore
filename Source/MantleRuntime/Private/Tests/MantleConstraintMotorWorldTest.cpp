// Copyright Joshua Eger. All Rights Reserved.
#include "MantleConstraint.h"
#include "MantlePlaceTypes.h"

#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "PhysicsEngine/BodyInstance.h"

#if WITH_DEV_AUTOMATION_TESTS

// Joints in a world the physics engine runs: each motor turns or moves its entity the way its target
// says, as far as it says (the sign and the units the engine's drives take, which the other tests
// cannot see), a motor too weak for its load does not lift it, a slider's off-center limits hold
// min..max, a ball stays inside its cone, and a new target moves a body that had gone to sleep.
// Each joint holds a box to the world, far from the others; every box is 1 m across.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleConstraintMotorWorldTestPrivate
{
	constexpr float StepSeconds = 1.0f / 60.0f;

	/** A game world that simulates physics, with its world context, torn down with it. */
	struct FPhysicsWorld
	{
		UWorld* World = nullptr;
		uint64 Frame = GFrameCounter;

		FPhysicsWorld()
		{
			if (!GEngine)
			{
				return;
			}
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false,
				FName(*FString::Printf(TEXT("MantleConstraintMotorWorld_%s"), *FGuid::NewGuid().ToString())));
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->bShouldSimulatePhysics = true;
			World->InitializeActorsForPlay(FURL());
			World->GetWorldSettings()->NotifyBeginPlay();
		}

		~FPhysicsWorld()
		{
			if (World)
			{
				World->BeginTearingDown();
				World->EndPlay(EEndPlayReason::Quit);
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
				GEngine->DestroyWorldContext(World);
			}
			GFrameCounter = Frame;
		}

		void Run(float Seconds) const
		{
			for (int32 Step = 0, Steps = FMath::CeilToInt(Seconds / StepSeconds); Step < Steps; ++Step)
			{
				++GFrameCounter;
				World->Tick(LEVELTICK_All, StepSeconds);
			}
		}
	};

	/** A box Extent half-size of Mass kg at Location that simulates physics, with gravity or without. */
	UBoxComponent* SpawnBox(UWorld* World, const FVector& Location, double Mass, bool bGravity, const FVector& Extent = FVector(50.0))
	{
		AActor* Actor = World->SpawnActor<AActor>();
		if (!Actor)
		{
			return nullptr;
		}
		UBoxComponent* Box = NewObject<UBoxComponent>(Actor);
		Actor->SetRootComponent(Box);
		Box->SetBoxExtent(Extent);
		Box->SetMobility(EComponentMobility::Movable);
		Box->SetCollisionProfileName(TEXT("PhysicsActor"));
		Box->BodyInstance.bSimulatePhysics = true;
		Box->BodyInstance.SetMassOverride(static_cast<float>(Mass));
		Box->BodyInstance.bEnableGravity = bGravity;
		Box->SetWorldLocation(Location);
		Box->RegisterComponent();
		return Box;
	}

	/** Joins Box to the world as Props say; null with the error added to Test. */
	AMantleConstraint* JoinToWorld(FAutomationTestBase& Test, UWorld* World, UBoxComponent* Box, FMantleConstraintProps Props)
	{
		Props.EntityA = Box ? Box->GetOwner()->GetName() : FString();
		FString Error;
		AMantleConstraint* Joint = Box ? AMantleConstraint::SpawnJoined(World, FString(), Props, Box, nullptr, Error) : nullptr;
		if (!Joint)
		{
			Test.AddError(FString::Printf(TEXT("the %s was not joined: %s"), MantleConstraintTypeToString(Props.Type), *Error));
		}
		return Joint;
	}

	FMantleConstraintProps Hinge(const FVector& AnchorA, bool bLimited, double Min, double Max, const TOptional<FMantleConstraintMotor>& Motor)
	{
		FMantleConstraintProps Props;
		Props.Type = EMantleConstraintType::Hinge;
		Props.AnchorA = AnchorA;
		Props.bLimited = bLimited;
		Props.LimitMin = Min;
		Props.LimitMax = Max;
		Props.Motor = Motor;
		return Props;
	}

	FMantleConstraintProps Slider(const FVector& Axis, bool bLimited, double Min, double Max, const TOptional<FMantleConstraintMotor>& Motor)
	{
		FMantleConstraintProps Props;
		Props.Type = EMantleConstraintType::Slider;
		Props.Axis = Axis;
		Props.bLimited = bLimited;
		Props.LimitMin = Min;
		Props.LimitMax = Max;
		Props.Motor = Motor;
		return Props;
	}

	double PositionOf(const AMantleConstraint* Joint)
	{
		return Joint ? Joint->GetPosition().Get(TNumericLimits<double>::Max()) : TNumericLimits<double>::Max();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleConstraintMotorWorldTest,
	"Mantle.Runtime.Constraint.World.MotorsLimitsAndCones",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleConstraintMotorWorldTest::RunTest(const FString& Parameters)
{
	using namespace MantleConstraintMotorWorldTestPrivate;

	FPhysicsWorld Physics;
	UWorld* World = Physics.World;
	if (!TestNotNull(TEXT("a world that simulates physics"), World))
	{
		return false;
	}

	// A door hinged at its edge, up the world's Z, driven to 90 degrees inside limits that hold -10..110.
	UBoxComponent* Door = SpawnBox(World, FVector(0.0, 0.0, 1000.0), 30.0, false, FVector(50.0, 5.0, 100.0));
	AMantleConstraint* DoorHinge = JoinToWorld(*this, World, Door,
		Hinge(FVector(-50.0, 0.0, 0.0), true, -10.0, 110.0, FMantleConstraintMotor{ EMantleMotorMode::Position, 90.0, 500.0 }));

	// A lift on a slider up Z, with gravity, driven to 500 cm by up to 2000 N (it weighs about 196 N).
	UBoxComponent* Lift = SpawnBox(World, FVector(1000.0, 0.0, 1000.0), 20.0, true);
	AMantleConstraint* LiftRail = JoinToWorld(*this, World, Lift,
		Slider(FVector::ZAxisVector, true, 0.0, 600.0, FMantleConstraintMotor{ EMantleMotorMode::Position, 500.0, 2000.0 }));

	// A lift too heavy for its motor: 100 kg (about 981 N) against 500 N. It stays at its lower limit.
	UBoxComponent* Heavy = SpawnBox(World, FVector(2000.0, 0.0, 1000.0), 100.0, true);
	AMantleConstraint* HeavyRail = JoinToWorld(*this, World, Heavy,
		Slider(FVector::ZAxisVector, true, 0.0, 400.0, FMantleConstraintMotor{ EMantleMotorMode::Position, 300.0, 500.0 }));

	// A fan turning at 360 degrees a second about its own Z.
	UBoxComponent* Fan = SpawnBox(World, FVector(3000.0, 0.0, 1000.0), 5.0, false, FVector(100.0, 10.0, 5.0));
	AMantleConstraint* FanAxle = JoinToWorld(*this, World, Fan,
		Hinge(FVector::ZeroVector, false, 0.0, 0.0, FMantleConstraintMotor{ EMantleMotorMode::Speed, 360.0, 100.0 }));

	// A drawer on X with off-center limits, -20..40, and no motor: pushed both ways, it stops at each end.
	UBoxComponent* Drawer = SpawnBox(World, FVector(4000.0, 0.0, 1000.0), 8.0, false);
	AMantleConstraint* DrawerSlide = JoinToWorld(*this, World, Drawer, Slider(FVector::XAxisVector, true, -20.0, 40.0, TOptional<FMantleConstraintMotor>()));

	// A lantern hanging 100 cm under a ball joint with a 30 degree cone, knocked sideways.
	UBoxComponent* Lantern = SpawnBox(World, FVector(5000.0, 0.0, 900.0), 4.0, true, FVector(10.0));
	FMantleConstraintProps Hook;
	Hook.Type = EMantleConstraintType::Ball;
	Hook.AnchorA = FVector(0.0, 0.0, 100.0);
	Hook.Axis = FVector::ZAxisVector;
	Hook.SwingLimit = 30.0;
	AMantleConstraint* LanternHook = JoinToWorld(*this, World, Lantern, Hook);

	// A hinge put to sleep before its motor is given a target.
	UBoxComponent* Gate = SpawnBox(World, FVector(6000.0, 0.0, 1000.0), 10.0, false, FVector(50.0, 5.0, 50.0));
	AMantleConstraint* GateHinge = JoinToWorld(*this, World, Gate,
		Hinge(FVector(-50.0, 0.0, 0.0), false, 0.0, 0.0, FMantleConstraintMotor{ EMantleMotorMode::Position, 0.0, 300.0 }));

	if (!DoorHinge || !LiftRail || !HeavyRail || !FanAxle || !DrawerSlide || !LanternHook || !GateHinge)
	{
		return false;
	}
	TestEqual(TEXT("every joint starts at 0"), PositionOf(DoorHinge), 0.0, 0.01);

	Drawer->SetPhysicsLinearVelocity(FVector(300.0, 0.0, 0.0));
	Lantern->SetPhysicsLinearVelocity(FVector(0.0, 600.0, 0.0));
	double MostTilt = 0.0;
	// How far past their targets the position motors go: the play test's door swung 56 degrees past 90 when its
	// motor's whole push was the spring's (the cap took the braking too). Its setpoint now moves at the motor's
	// top speed, so it arrives without swinging past.
	double DoorMost = -TNumericLimits<double>::Max();
	double LiftMost = -TNumericLimits<double>::Max();
	auto Follow = [&]()
	{
		DoorMost = FMath::Max(DoorMost, PositionOf(DoorHinge));
		LiftMost = FMath::Max(LiftMost, PositionOf(LiftRail));
	};
	const FVector HookPoint = Lantern->GetComponentTransform().TransformPosition(Hook.AnchorA);
	for (int32 Step = 0; Step < 120; ++Step)
	{
		Physics.Run(StepSeconds);
		Follow();
		// The lantern hangs straight down from the hook at first: its tilt is the angle off that.
		const FVector Down = (Lantern->GetComponentLocation() - HookPoint).GetSafeNormal();
		MostTilt = FMath::Max(MostTilt, FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Down | -FVector::ZAxisVector, -1.0, 1.0))));
	}
	TestTrue(FString::Printf(TEXT("the lantern swung (%.1f degrees)"), MostTilt), MostTilt > 10.0);
	TestTrue(FString::Printf(TEXT("and stayed inside its 30 degree cone (%.1f degrees)"), MostTilt), MostTilt < 33.0);
	TestEqual(TEXT("the drawer stops at its upper limit, 40"), PositionOf(DrawerSlide), 40.0, 2.0);
	Drawer->SetPhysicsLinearVelocity(FVector(-300.0, 0.0, 0.0));

	// At their default top speeds the door (about 28 degrees a second) and the lift (about 100 cm a second) take 3 to 5 s.
	for (int32 Step = 0; Step < 360; ++Step)
	{
		Physics.Run(StepSeconds);
		Follow();
	}
	TestEqual(TEXT("the door turned to 90 degrees"), PositionOf(DoorHinge), 90.0, 2.0);
	TestTrue(FString::Printf(TEXT("without swinging more than 3 degrees past it (it reached %.1f)"), DoorMost), DoorMost < 93.0);
	TestTrue(FString::Printf(TEXT("the lift went no more than 3 cm past 500 (it reached %.1f)"), LiftMost), LiftMost < 503.0);
	const FVector DoorFacing = Door->GetComponentQuat().GetAxisX();
	TestTrue(FString::Printf(TEXT("a positive turn about Z is a positive yaw (facing %s)"), *DoorFacing.ToString()),
		DoorFacing.Equals(FVector::YAxisVector, 0.05));
	// A position motor is a spring of MotorPositionResponseHz on what it moves, so a load it holds up
	// sags below the target by gravity over the spring's (2 pi f)^2: about 6 cm at 2 Hz.
	const double Omega = 2.0 * UE_DOUBLE_PI * FMantleConstraints::MotorPositionResponseHz;
	const double Sag = -World->GetGravityZ() / (Omega * Omega);
	TestEqual(FString::Printf(TEXT("the lift rose to 500 cm, less its %.1f cm sag"), Sag), PositionOf(LiftRail), 500.0 - Sag, 3.0);
	TestEqual(TEXT("up the world's Z"), Lift->GetComponentLocation().Z, 1500.0 - Sag, 3.0);
	TestTrue(FString::Printf(TEXT("the heavy lift's motor could not raise it (%.1f cm)"), PositionOf(HeavyRail)), PositionOf(HeavyRail) < 10.0);
	TestEqual(TEXT("the drawer stops at its lower limit, -20"), PositionOf(DrawerSlide), -20.0, 2.0);
	const double FanSpeed = Fan->GetPhysicsAngularVelocityInDegrees() | FVector::ZAxisVector;
	TestEqual(TEXT("the fan turns at 360 degrees a second"), FanSpeed, 360.0, 20.0);
	const double FanBefore = PositionOf(FanAxle);
	Physics.Run(0.25f);
	const double FanTurn = FRotator::NormalizeAxis(PositionOf(FanAxle) - FanBefore);
	TestEqual(TEXT("its position grows with a positive speed"), FanTurn, 90.0, 10.0);

	// A new target wakes the sleeping gate and drives it, and nil frees it.
	if (FBodyInstance* Body = Gate->GetBodyInstance())
	{
		Body->PutInstanceToSleep();
	}
	Physics.Run(0.1f);
	FString Error;
	TestTrue(TEXT("the gate takes a new target"), GateHinge->SetMotor(FMantleConstraintMotor{ EMantleMotorMode::Position, -45.0, 300.0 }, Error));
	double GateLeast = TNumericLimits<double>::Max();
	for (int32 Step = 0; Step < 180; ++Step)
	{
		Physics.Run(StepSeconds);
		GateLeast = FMath::Min(GateLeast, PositionOf(GateHinge));
	}
	TestEqual(TEXT("the sleeping gate turned to -45 degrees"), PositionOf(GateHinge), -45.0, 2.0);
	// The gate swings 3.8 degrees past, more than the door and lift's 3: Joshua accepted that on 2026-11-23 and the test
	// allows 5. Why it swings further is still open, a follow-up (more damping did not change it).
	TestTrue(FString::Printf(TEXT("without swinging more than 5 degrees past it (it reached %.1f)"), GateLeast), GateLeast > -50.0);
	// A top speed of its own: 30 degrees a second takes the gate from -45 to 0 in 1.5 s, not sooner.
	FMantleConstraintMotor Slow{ EMantleMotorMode::Position, 0.0, 300.0 };
	Slow.MaxSpeed = 30.0;
	TestTrue(TEXT("the gate takes a top speed"), GateHinge->SetMotor(Slow, Error));
	Physics.Run(0.75f);
	// Its setpoint is half way; the gate follows a little behind it (a few degrees), never ahead.
	const double HalfWay = PositionOf(GateHinge);
	TestTrue(FString::Printf(TEXT("half way at 30 degrees a second, a little behind its setpoint (%.1f against -22.5)"), HalfWay),
		HalfWay <= -22.0 && HalfWay > -32.0);
	Physics.Run(1.5f);
	TestEqual(TEXT("and there after 1.5 s"), PositionOf(GateHinge), 0.0, 2.0);
	FMantleConstraintMotor Spinning{ EMantleMotorMode::Speed, 90.0, 300.0 };
	Spinning.MaxSpeed = 30.0;
	TestFalse(TEXT("a speed motor takes no top speed"), GateHinge->SetMotor(Spinning, Error));
	TestTrue(TEXT("naming MaxSpeed"), Error.StartsWith(TEXT("MaxSpeed")));
	TestFalse(TEXT("a target past 180 is refused"), GateHinge->SetMotor(FMantleConstraintMotor{ EMantleMotorMode::Position, 200.0, 300.0 }, Error));
	TestTrue(TEXT("naming Target"), Error.StartsWith(TEXT("Target")));
	TestTrue(TEXT("the motor can be removed"), GateHinge->SetMotor(TOptional<FMantleConstraintMotor>(), Error));
	TestFalse(TEXT("and is gone"), GateHinge->GetProps().Motor.IsSet());
	TestFalse(TEXT("a ball has no motor"), LanternHook->SetMotor(FMantleConstraintMotor{ EMantleMotorMode::Speed, 10.0, 10.0 }, Error));
	TestFalse(TEXT("nor a position"), LanternHook->GetPosition().IsSet());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
