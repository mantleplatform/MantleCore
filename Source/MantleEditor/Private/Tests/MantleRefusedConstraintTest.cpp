// Copyright Joshua Eger. All Rights Reserved.
#include "MantleExportedEntities.h"
#include "MantlePlaceExporter.h"
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

// A Physics Constraint Actor that is not a weld, a hinge or a rope (a slider: one linear axis free or limited; a ball-and-socket: more
// than one angular axis free or limited) is left out of the place with a Check warning that names it, and the rest of the place still
// publishes: the two bodies stay separate physics objects, nothing is welded in its place, and no error stops the publish. A hinge in the
// same level is the control: it publishes. Built in a transient editor world.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleRefusedConstraintTestPrivate
{
	/** An empty editor world for one test, destroyed with it. */
	struct FConstraintWorld
	{
		UWorld* World = nullptr;

		FConstraintWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false,
				FName(*FString::Printf(TEXT("MantleRefusedConstraintTestWorld_%s"), *FGuid::NewGuid().ToString())));
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

	/** A constraint joining A to B (or to the world when B is null), its motions left as the engine's defaults for the caller to change. */
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
		return Constraint;
	}

	bool HasIssue(const FMantleValidationResult& Result, const TCHAR* Actor, EMantleIssueSeverity Severity, const TCHAR* ProblemContains)
	{
		return Result.Issues.ContainsByPredicate([Actor, Severity, ProblemContains](const FMantleValidationIssue& Issue)
		{
			return Issue.Actor == Actor && Issue.Severity == Severity && Issue.Problem.Contains(ProblemContains);
		});
	}

	bool IsPublishedConstraint(const FMantleExportResult& Result, const TCHAR* Name)
	{
		const FMantleEntity* Entity = MantleExportedEntities::Named(Result, Name);
		return Entity && Entity->Type == EMantleEntityType::Constraint;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleRefusedConstraintTest,
	"Mantle.Editor.Constraints.SliderAndBallJointsAreLeftOutWithAWarning",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleRefusedConstraintTest::RunTest(const FString& Parameters)
{
	using namespace MantleRefusedConstraintTestPrivate;
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
	APhysicsConstraintActor* Slider = BodyA ? SpawnConstraint(World, TEXT("Slider joint"), BodyA, BodyB, FVector(150.0, 0.0, 100.0)) : nullptr;
	APhysicsConstraintActor* Ball = BodyA ? SpawnConstraint(World, TEXT("Ball joint"), BodyA, nullptr, FVector(0.0, 300.0, 100.0)) : nullptr;
	APhysicsConstraintActor* Hinge = BodyB ? SpawnConstraint(World, TEXT("Hinge joint"), BodyB, nullptr, FVector(300.0, 300.0, 100.0)) : nullptr;
	if (!TestNotNull(TEXT("body A"), BodyA) || !TestNotNull(TEXT("body B"), BodyB) || !TestNotNull(TEXT("the slider"), Slider)
		|| !TestNotNull(TEXT("the ball joint"), Ball) || !TestNotNull(TEXT("the hinge"), Hinge))
	{
		return false;
	}

	// A slider: it moves along X (limited) and no other way.
	{
		FConstraintInstance& Instance = Slider->GetConstraintComp()->ConstraintInstance;
		Instance.SetLinearLimits(LCM_Limited, LCM_Locked, LCM_Locked, 100.0f);
		Instance.SetAngularSwing1Limit(ACM_Locked, 0.0f);
		Instance.SetAngularSwing2Limit(ACM_Locked, 0.0f);
		Instance.SetAngularTwistLimit(ACM_Locked, 0.0f);
	}
	// A ball-and-socket: every linear motion locked, turning about all three axes within limits.
	{
		FConstraintInstance& Instance = Ball->GetConstraintComp()->ConstraintInstance;
		Instance.SetLinearLimits(LCM_Locked, LCM_Locked, LCM_Locked, 0.0f);
		Instance.SetAngularSwing1Limit(ACM_Limited, 45.0f);
		Instance.SetAngularSwing2Limit(ACM_Limited, 45.0f);
		Instance.SetAngularTwistLimit(ACM_Limited, 45.0f);
	}
	// The control: a hinge (one angular axis free, the rest locked), which Mantle publishes.
	{
		FConstraintInstance& Instance = Hinge->GetConstraintComp()->ConstraintInstance;
		Instance.SetLinearLimits(LCM_Locked, LCM_Locked, LCM_Locked, 0.0f);
		Instance.SetAngularSwing1Limit(ACM_Locked, 0.0f);
		Instance.SetAngularSwing2Limit(ACM_Locked, 0.0f);
		Instance.SetAngularTwistLimit(ACM_Free, 0.0f);
	}

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(World);
	TestTrue(TEXT("Check warns, naming the slider, that it moves along some axes and not others"),
		HasIssue(Validation, TEXT("Slider joint"), EMantleIssueSeverity::Warning, TEXT("moves along some axes")));
	TestTrue(TEXT("Check warns, naming the ball-and-socket joint, that it turns about more than one axis"),
		HasIssue(Validation, TEXT("Ball joint"), EMantleIssueSeverity::Warning, TEXT("more than one axis")));
	TestFalse(TEXT("neither is an error"), Validation.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Severity == EMantleIssueSeverity::Error && (Issue.Actor == TEXT("Slider joint") || Issue.Actor == TEXT("Ball joint"));
	}));
	TestFalse(TEXT("the hinge raises nothing"), Validation.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Actor == TEXT("Hinge joint");
	}));

	const FMantleExportResult Result = FMantlePlaceExporter::Export(World, FMantleExportOptions());
	TestFalse(TEXT("the slider is left out"), IsPublishedConstraint(Result, TEXT("Slider joint")));
	TestFalse(TEXT("the ball-and-socket joint is left out"), IsPublishedConstraint(Result, TEXT("Ball joint")));
	TestTrue(TEXT("the hinge publishes"), IsPublishedConstraint(Result, TEXT("Hinge joint")));
	TestTrue(TEXT("body A still publishes"), MantleExportedEntities::Named(Result, TEXT("Body A")) != nullptr);
	TestTrue(TEXT("body B still publishes"), MantleExportedEntities::Named(Result, TEXT("Body B")) != nullptr);
	TestTrue(TEXT("no error names either refused joint"), !Result.Errors.ContainsByPredicate([](const FMantleParseError& Error)
	{
		return Error.Message.Contains(TEXT("Slider joint")) || Error.Message.Contains(TEXT("Ball joint"));
	}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
