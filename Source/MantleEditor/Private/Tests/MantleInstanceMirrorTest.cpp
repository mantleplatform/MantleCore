// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCoverage.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceValidator.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// Instances of a mesh on any actor (a Blueprint's instanced component, a level's foliage) with a negative scale
// publish as instances of a mirrored copy of the mesh, and ones scaled to zero are left out with a coverage line.
// Neither refuses the place (GhzGarage/mantle#818). The scales are read from the exported struct, not from text.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleInstanceMirrorTestPrivate
{
	using EOutcome = EMantleCoverageOutcome;

	struct FInstanceWorld
	{
		UWorld* World = nullptr;

		FInstanceWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleInstanceMirrorTestWorld"));
		}

		~FInstanceWorld()
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

	/** An actor holding one instanced component of the engine cube with an instance at each of Scales. */
	UInstancedStaticMeshComponent* SpawnInstances(UWorld* World, UStaticMesh* Mesh, const TArray<FVector>& Scales)
	{
		AActor* Actor = World->SpawnActor<AActor>();
		if (!Actor)
		{
			return nullptr;
		}
		Actor->SetActorLabel(TEXT("BP_Room"));
		USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"));
		Actor->SetRootComponent(Root);
		Root->RegisterComponent();
		UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(Actor, TEXT("Floors"));
		Component->SetStaticMesh(Mesh);
		Component->SetupAttachment(Root);
		Component->RegisterComponent();
		double X = 0.0;
		for (const FVector& Scale : Scales)
		{
			Component->AddInstance(FTransform(FRotator::ZeroRotator, FVector(X, 0.0, 0.0), Scale), /*bWorldSpace*/ true);
			X += 500.0;
		}
		return Component;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleInstanceMirrorTest,
	"Mantle.Editor.PlaceExporter.MirroredAndZeroInstances",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleInstanceMirrorTest::RunTest(const FString& Parameters)
{
	using namespace MantleInstanceMirrorTestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Cube)
	{
		AddWarning(TEXT("the engine cube did not load; skipped"));
		return true;
	}
	FInstanceWorld Fixture;
	if (!TestNotNull(TEXT("the instances spawned"), SpawnInstances(Fixture.World, Cube,
		{ FVector(1.0, 1.0, 1.0), FVector(-1.0, 1.0, 1.0), FVector(2.0, -2.0, 1.0), FVector(0.0, 0.0, 0.0) })))
	{
		return false;
	}

	// Check: both are notes, not errors.
	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestFalse(TEXT("neither the mirrored nor the zero-scale instances are errors"), Validation.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Actor == TEXT("BP_Room") && Issue.Severity == EMantleIssueSeverity::Error;
	}));

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	int32 Instances = 0;
	int32 Sets = 0;
	for (const FMantleEntity& Entity : Result.Place.Entities)
	{
		if (const FMantleInstancedMeshProps* Props = Entity.Props.TryGet<FMantleInstancedMeshProps>())
		{
			++Sets;
			for (const FTransform& Instance : Props->Instances)
			{
				++Instances;
				const FVector Scale = Instance.GetScale3D();
				TestTrue(TEXT("every published scale is above zero"), Scale.X > 0.0 && Scale.Y > 0.0 && Scale.Z > 0.0);
			}
		}
	}
	TestEqual(TEXT("three of the four instances are published"), Instances, 3);
	// Normalising an instance (ExporterNormalizeInstance) decomposes its matrix, which always puts a single mirror on X and turns the rest
	// into rotation, so a mirror on X and one on Y share one mirrored copy: two sets, the plain cube and the mirrored one.
	TestEqual(TEXT("in one set for the plain cube and one for the mirrored copy"), Sets, 2);
	TestEqual(TEXT("two are reported as approximated"), Result.Coverage.Count(MantleCoverageCategory::StaticMeshes, EOutcome::Approximated), static_cast<int64>(2));
	TestEqual(TEXT("and the zero-scale one as dropped"), Result.Coverage.Count(MantleCoverageCategory::StaticMeshes, EOutcome::Dropped), static_cast<int64>(1));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
