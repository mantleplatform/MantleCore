// Copyright Joshua Eger. All Rights Reserved.
#include "MantleExportedEntities.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceValidator.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/PhysicsConstraintActor.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

// Physics Constraint Actors publish as Constraints: a slider (one linear axis free or limited, the rest
// locked), a ball-and-socket joint (both swings free or limited alike) and a hinge or a slider with a
// drive (its motor), beside the plain hinge. The place file Export writes for them reads back through
// the place parser as the same joints. What Mantle cannot publish (an oval cone, a SLERP drive on a
// hinge) is left out with a Check warning that names it, and the rest of the place still publishes.
// Built in a transient editor world.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleConstraintPublishTestPrivate
{
	/** An empty editor world for one test, destroyed with it. */
	struct FConstraintWorld
	{
		UWorld* World = nullptr;

		FConstraintWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false,
				FName(*FString::Printf(TEXT("MantleConstraintPublishTestWorld_%s"), *FGuid::NewGuid().ToString())));
		}

		~FConstraintWorld()
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

	/** A cube that simulates physics, at Location. */
	AStaticMeshActor* SpawnPhysicsCube(UWorld* World, UStaticMesh* Cube, const TCHAR* Label, const FVector& Location)
	{
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
		if (!Actor)
		{
			return nullptr;
		}
		Actor->SetActorLabel(Label);
		UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetStaticMesh(Cube);
		Component->BodyInstance.bSimulatePhysics = true;
		return Actor;
	}

	/**
	 * A constraint joining A to B (or to the world when B is null), every motion Locked, so the caller
	 * says only what it frees.
	 */
	APhysicsConstraintActor* SpawnConstraint(UWorld* World, const TCHAR* Label, AActor* A, AActor* B, const FVector& Location)
	{
		APhysicsConstraintActor* Constraint = World->SpawnActor<APhysicsConstraintActor>(Location, FRotator::ZeroRotator);
		if (!Constraint)
		{
			return nullptr;
		}
		Constraint->SetActorLabel(Label);
		UPhysicsConstraintComponent* Component = Constraint->GetConstraintComp();
		Component->ConstraintActor1 = A;
		Component->ConstraintActor2 = B;
		FConstraintInstance& Instance = Component->ConstraintInstance;
		Instance.SetLinearLimits(LCM_Locked, LCM_Locked, LCM_Locked, 0.0f);
		Instance.SetAngularSwing1Limit(ACM_Locked, 0.0f);
		Instance.SetAngularSwing2Limit(ACM_Locked, 0.0f);
		Instance.SetAngularTwistLimit(ACM_Locked, 0.0f);
		return Constraint;
	}

	bool HasIssue(const FMantleValidationResult& Result, const TCHAR* Actor, EMantleIssueSeverity Severity, const TCHAR* ProblemContains)
	{
		return Result.Issues.ContainsByPredicate([Actor, Severity, ProblemContains](const FMantleValidationIssue& Issue)
		{
			return Issue.Actor == Actor && Issue.Severity == Severity && Issue.Problem.Contains(ProblemContains);
		});
	}

	bool HasAnyIssue(const FMantleValidationResult& Result, const TCHAR* Actor)
	{
		return Result.Issues.ContainsByPredicate([Actor](const FMantleValidationIssue& Issue) { return Issue.Actor == Actor; });
	}

	const FMantleConstraintProps* PublishedConstraint(const FMantleExportResult& Result, const TCHAR* Name)
	{
		const FMantleEntity* Entity = MantleExportedEntities::Named(Result, Name);
		return Entity && Entity->Type == EMantleEntityType::Constraint ? Entity->Props.TryGet<FMantleConstraintProps>() : nullptr;
	}

	/** The constraint named Name in a place the parser read. */
	const FMantleConstraintProps* ParsedConstraint(const FMantleParseResult& Parsed, const TCHAR* Name)
	{
		for (const FMantleEntity& Entity : Parsed.Place.Entities)
		{
			if (Entity.Name == Name && Entity.Type == EMantleEntityType::Constraint)
			{
				return Entity.Props.TryGet<FMantleConstraintProps>();
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleConstraintPublishTest,
	"Mantle.Editor.Constraints.SlidersBallsAndMotorsPublish",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleConstraintPublishTest::RunTest(const FString& Parameters)
{
	using namespace MantleConstraintPublishTestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	FConstraintWorld Fixture;
	UWorld* World = Fixture.World;
	if (!Cube || !TestNotNull(TEXT("a test world"), World))
	{
		AddWarning(TEXT("the engine cube or the world is missing; skipped"));
		return true;
	}

	AStaticMeshActor* BodyA = SpawnPhysicsCube(World, Cube, TEXT("Body A"), FVector(0.0, 0.0, 100.0));
	AStaticMeshActor* BodyB = SpawnPhysicsCube(World, Cube, TEXT("Body B"), FVector(300.0, 0.0, 100.0));
	AStaticMeshActor* BodyC = SpawnPhysicsCube(World, Cube, TEXT("Body C"), FVector(600.0, 0.0, 100.0));
	APhysicsConstraintActor* Slider = BodyA ? SpawnConstraint(World, TEXT("Slider joint"), BodyA, BodyB, FVector(150.0, 0.0, 100.0)) : nullptr;
	APhysicsConstraintActor* Ball = BodyA ? SpawnConstraint(World, TEXT("Ball joint"), BodyA, nullptr, FVector(0.0, 300.0, 100.0)) : nullptr;
	APhysicsConstraintActor* Door = BodyB ? SpawnConstraint(World, TEXT("Motor hinge"), BodyB, nullptr, FVector(300.0, 300.0, 100.0)) : nullptr;
	APhysicsConstraintActor* Hinge = BodyC ? SpawnConstraint(World, TEXT("Hinge joint"), BodyC, nullptr, FVector(600.0, 300.0, 100.0)) : nullptr;
	APhysicsConstraintActor* Oval = BodyC ? SpawnConstraint(World, TEXT("Oval cone"), BodyC, nullptr, FVector(600.0, 600.0, 100.0)) : nullptr;
	APhysicsConstraintActor* Slerp = BodyB ? SpawnConstraint(World, TEXT("SLERP hinge"), BodyB, BodyC, FVector(450.0, 0.0, 100.0)) : nullptr;
	if (!TestNotNull(TEXT("body A"), BodyA) || !TestNotNull(TEXT("body B"), BodyB) || !TestNotNull(TEXT("body C"), BodyC)
		|| !TestNotNull(TEXT("the slider"), Slider) || !TestNotNull(TEXT("the ball joint"), Ball) || !TestNotNull(TEXT("the motor hinge"), Door)
		|| !TestNotNull(TEXT("the hinge"), Hinge) || !TestNotNull(TEXT("the oval cone"), Oval) || !TestNotNull(TEXT("the SLERP hinge"), Slerp))
	{
		return false;
	}

	// A slider: it moves along X, up to 100 cm either way, driven to 30 cm with up to 250 N (Unreal's
	// target is the same number as Mantle's for a joint centered on where it starts).
	{
		FConstraintInstance& Instance = Slider->GetConstraintComp()->ConstraintInstance;
		Instance.SetLinearLimits(LCM_Limited, LCM_Locked, LCM_Locked, 100.0f);
		Instance.SetLinearDriveAccelerationMode(false);
		Instance.SetLinearDriveParams(500.0f, 50.0f, 250.0f * 100.0f);
		Instance.SetLinearPositionTarget(FVector(30.0, 0.0, 0.0));
		Instance.SetLinearPositionDrive(true, false, false);
	}
	// A ball-and-socket: a 45 degree cone and 20 degrees of twist either way.
	{
		FConstraintInstance& Instance = Ball->GetConstraintComp()->ConstraintInstance;
		Instance.SetAngularSwing1Limit(ACM_Limited, 45.0f);
		Instance.SetAngularSwing2Limit(ACM_Limited, 45.0f);
		Instance.SetAngularTwistLimit(ACM_Limited, 20.0f);
	}
	// A hinge (its twist free) spinning at a quarter turn a second with up to 40 N*m, in force mode.
	{
		FConstraintInstance& Instance = Door->GetConstraintComp()->ConstraintInstance;
		Instance.SetAngularTwistLimit(ACM_Free, 0.0f);
		Instance.SetAngularDriveMode(EAngularDriveMode::TwistAndSwing);
		Instance.SetAngularDriveAccelerationMode(false);
		Instance.SetAngularDriveParams(0.0f, 100.0f, 40.0f * 10000.0f);
		Instance.SetAngularVelocityTarget(FVector(0.25, 0.0, 0.0));
		Instance.SetAngularVelocityDriveTwistAndSwing(true, false);
	}
	// The control: a plain hinge.
	Hinge->GetConstraintComp()->ConstraintInstance.SetAngularTwistLimit(ACM_Free, 0.0f);
	// Left out: an oval cone, and a hinge driven by a SLERP drive.
	{
		FConstraintInstance& Instance = Oval->GetConstraintComp()->ConstraintInstance;
		Instance.SetAngularSwing1Limit(ACM_Limited, 30.0f);
		Instance.SetAngularSwing2Limit(ACM_Limited, 60.0f);
	}
	{
		FConstraintInstance& Instance = Slerp->GetConstraintComp()->ConstraintInstance;
		Instance.SetAngularTwistLimit(ACM_Free, 0.0f);
		Instance.SetAngularDriveMode(EAngularDriveMode::SLERP);
		Instance.SetOrientationDriveSLERP(true);
	}

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(World);
	for (const TCHAR* Published : { TEXT("Slider joint"), TEXT("Ball joint"), TEXT("Motor hinge"), TEXT("Hinge joint") })
	{
		TestFalse(FString::Printf(TEXT("Check raises nothing for the %s"), Published), HasAnyIssue(Validation, Published));
	}
	TestTrue(TEXT("Check warns, naming the oval cone"), HasIssue(Validation, TEXT("Oval cone"), EMantleIssueSeverity::Warning, TEXT("oval cone")));
	TestTrue(TEXT("Check warns, naming the SLERP drive"), HasIssue(Validation, TEXT("SLERP hinge"), EMantleIssueSeverity::Warning, TEXT("SLERP")));
	TestFalse(TEXT("neither is an error"), Validation.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Severity == EMantleIssueSeverity::Error && (Issue.Actor == TEXT("Oval cone") || Issue.Actor == TEXT("SLERP hinge"));
	}));

	const FMantleExportResult Result = FMantlePlaceExporter::Export(World, FMantleExportOptions());
	const FMantleConstraintProps* SliderProps = PublishedConstraint(Result, TEXT("Slider joint"));
	if (TestNotNull(TEXT("the slider publishes"), SliderProps))
	{
		TestTrue(TEXT("as a slider"), SliderProps->Type == EMantleConstraintType::Slider);
		TestTrue(TEXT("along X"), SliderProps->Axis.Equals(FVector::XAxisVector, 1e-4));
		TestTrue(TEXT("limited -100..100"), SliderProps->bLimited && FMath::IsNearlyEqual(SliderProps->LimitMin, -100.0, 1e-3)
			&& FMath::IsNearlyEqual(SliderProps->LimitMax, 100.0, 1e-3));
		TestTrue(TEXT("with a position motor to 30 cm of 250 N"), SliderProps->Motor.IsSet() && SliderProps->Motor->Mode == EMantleMotorMode::Position
			&& FMath::IsNearlyEqual(SliderProps->Motor->Target, 30.0, 1e-3) && FMath::IsNearlyEqual(SliderProps->Motor->MaxForce, 250.0, 1e-3));
	}
	const FMantleConstraintProps* BallProps = PublishedConstraint(Result, TEXT("Ball joint"));
	if (TestNotNull(TEXT("the ball joint publishes"), BallProps))
	{
		TestTrue(TEXT("as a ball"), BallProps->Type == EMantleConstraintType::Ball);
		TestTrue(TEXT("its 45 degree cone"), FMath::IsNearlyEqual(BallProps->SwingLimit.Get(-1.0), 45.0, 1e-3));
		TestTrue(TEXT("its 20 degree twist"), FMath::IsNearlyEqual(BallProps->TwistLimit.Get(-1.0), 20.0, 1e-3));
		TestFalse(TEXT("no motor"), BallProps->Motor.IsSet());
	}
	const FMantleConstraintProps* DoorProps = PublishedConstraint(Result, TEXT("Motor hinge"));
	if (TestNotNull(TEXT("the motor hinge publishes"), DoorProps))
	{
		TestTrue(TEXT("as a hinge"), DoorProps->Type == EMantleConstraintType::Hinge);
		TestTrue(TEXT("with a speed motor of 90 degrees a second and 40 N*m"), DoorProps->Motor.IsSet() && DoorProps->Motor->Mode == EMantleMotorMode::Speed
			&& FMath::IsNearlyEqual(DoorProps->Motor->Target, 90.0, 1e-3) && FMath::IsNearlyEqual(DoorProps->Motor->MaxForce, 40.0, 1e-3));
	}
	TestNotNull(TEXT("the plain hinge publishes"), PublishedConstraint(Result, TEXT("Hinge joint")));
	TestNull(TEXT("the oval cone is left out"), PublishedConstraint(Result, TEXT("Oval cone")));
	TestNull(TEXT("the SLERP hinge is left out"), PublishedConstraint(Result, TEXT("SLERP hinge")));
	for (const TCHAR* Body : { TEXT("Body A"), TEXT("Body B"), TEXT("Body C") })
	{
		TestTrue(FString::Printf(TEXT("%s still publishes"), Body), MantleExportedEntities::Named(Result, Body) != nullptr);
	}

	// The place file Export writes reads back through the place parser as the same joints. A body planned
	// into a cell file is put beside them, so the root names every entity its joints join.
	FMantlePlace Place = Result.Place;
	for (const FMantleEntity* Entity : MantleExportedEntities::All(Result))
	{
		if (!Place.Entities.ContainsByPredicate([Entity](const FMantleEntity& Root) { return Root.Id == Entity->Id; }))
		{
			Place.Entities.Add(*Entity);
		}
	}
	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(FMantlePlaceExporter::ToJson(Place));
	if (!TestTrue(TEXT("the exported place parses"), Parsed.bSuccess))
	{
		AddError(Parsed.ErrorSummary());
		return false;
	}
	const FMantleConstraintProps* SliderRead = ParsedConstraint(Parsed, TEXT("Slider joint"));
	TestTrue(TEXT("the slider's motor reads back"), SliderRead && SliderRead->Motor.IsSet() && SliderProps && SliderRead->Motor.GetValue() == SliderProps->Motor.GetValue()
		&& SliderRead->LimitMin == SliderProps->LimitMin && SliderRead->LimitMax == SliderProps->LimitMax);
	const FMantleConstraintProps* BallRead = ParsedConstraint(Parsed, TEXT("Ball joint"));
	TestTrue(TEXT("the ball's limits read back"), BallRead && BallProps && BallRead->Type == EMantleConstraintType::Ball
		&& BallRead->SwingLimit == BallProps->SwingLimit && BallRead->TwistLimit == BallProps->TwistLimit);
	const FMantleConstraintProps* DoorRead = ParsedConstraint(Parsed, TEXT("Motor hinge"));
	TestTrue(TEXT("the hinge's motor reads back"), DoorRead && DoorProps && DoorRead->Motor.IsSet() && DoorRead->Motor.GetValue() == DoorProps->Motor.GetValue());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
