// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceValidator.h"
#include "MantleSplineMeshBake.h"

#include "Components/SceneComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/SpringArmComponent.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// A plain actor with a spline mesh component (the publish test level's L5_SplineMeshNoCpuAccess) was told its 'Pipe' is dropped,
// although the spline mesh bake publishes it as a static mesh. Spline meshes and cables are baked on their own, so Check's
// dropped-components warning leaves them out; a component that really is dropped is still named.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleBakedComponentsNotDroppedTestPrivate
{
	/** A transient editor world for one test, destroyed with it. Cleans the spline bake cache up. */
	struct FBakedWorld
	{
		UWorld* World = nullptr;

		FBakedWorld()
		{
			FMantleSplineMeshBake::ClearCache();
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleBakedComponentsTestWorld"));
		}

		~FBakedWorld()
		{
			FMantleSplineMeshBake::ClearCache();
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

	/** Adds a component of class T named Name under the actor's root, as an instance component (what the editor's Add Component makes). */
	template <typename T>
	T* AddUnderRoot(AActor* Actor, const TCHAR* Name)
	{
		T* Component = NewObject<T>(Actor, Name);
		Component->SetupAttachment(Actor->GetRootComponent());
		Actor->AddInstanceComponent(Component);
		Component->RegisterComponent();
		return Component;
	}

	bool NamesDropped(const FMantleValidationResult& Result, const FString& Label, const TCHAR* Component)
	{
		return Result.Issues.ContainsByPredicate([&Label, Component](const FMantleValidationIssue& Issue)
		{
			return Issue.Actor == Label && Issue.Problem.Contains(TEXT("drops when publishing")) && Issue.Problem.Contains(Component);
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBakedComponentsNotDroppedTest,
	"Mantle.Editor.Check.SplineMeshIsNotADroppedComponent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBakedComponentsNotDroppedTest::RunTest(const FString& Parameters)
{
	using namespace MantleBakedComponentsNotDroppedTestPrivate;
	FBakedWorld Fixture;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("a world"), Fixture.World) || !TestNotNull(TEXT("the engine cube"), Cube))
	{
		return false;
	}

	// The level's actor: a plain Actor with a scene root, a spline mesh 'Pipe' over the cube, and a spring arm 'Arm' (which Mantle does drop).
	AActor* Actor = Fixture.World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(FVector(1500.0, 0.0, 0.0)));
	if (!TestNotNull(TEXT("the actor"), Actor))
	{
		return false;
	}
	USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("DefaultSceneRoot"));
	Actor->SetRootComponent(Root);
	Actor->AddInstanceComponent(Root);
	Root->RegisterComponent();
	Actor->SetActorLabel(TEXT("Spline mesh pipe"));

	USplineMeshComponent* Pipe = AddUnderRoot<USplineMeshComponent>(Actor, TEXT("Pipe"));
	Pipe->SetStaticMesh(Cube);
	Pipe->SetStartAndEnd(FVector(0.0, 0.0, 50.0), FVector(300.0, 0.0, 0.0), FVector(0.0, 400.0, 50.0), FVector(300.0, 0.0, 0.0), true);
	AddUnderRoot<USpringArmComponent>(Actor, TEXT("Arm"));
	TestTrue(TEXT("the spline mesh is one the bake publishes"), FMantleSplineMeshBake::IsBakeSource(Pipe));

	const FMantleValidationResult Result = FMantlePlaceValidator::Validate(Fixture.World);
	TestFalse(TEXT("Check does not call the baked spline mesh dropped"), NamesDropped(Result, Actor->GetActorLabel(), TEXT("'Pipe'")));
	TestTrue(TEXT("a component Mantle really drops is still named"), NamesDropped(Result, Actor->GetActorLabel(), TEXT("'Arm'")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
