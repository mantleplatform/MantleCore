// Copyright Joshua Eger. All Rights Reserved.
#include "MantleExportedEntities.h"
#include "MantlePlaceExporter.h"
#include "MantleTriggerShapeExport.h"

#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TriggerBox.h"
#include "Engine/TriggerSphere.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// A box or sphere is a trigger only when it overlaps and blocks nothing (the rule a capsule is held to). A box or sphere that blocks is
// collision: a Trigger Box and Trigger Sphere still publish as triggers, and a blocking box root, or a blocking sphere part of a prop,
// publishes as a hidden library cube or sphere the size of the shape (before, it published as a trigger and the collision was lost).
// Built in a transient editor world.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleShapeGuardTestPrivate
{
	/** An empty editor world for one test, destroyed with it. */
	struct FShapeWorld
	{
		UWorld* World = nullptr;

		FShapeWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false,
				FName(*FString::Printf(TEXT("MantleShapeGuardTestWorld_%s"), *FGuid::NewGuid().ToString())));
		}

		~FShapeWorld()
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

	const FMantleStaticMeshProps* MeshPropsOf(const FMantleExportResult& Result, const TCHAR* Name)
	{
		const FMantleEntity* Entity = MantleExportedEntities::Named(Result, Name);
		return Entity ? Entity->Props.TryGet<FMantleStaticMeshProps>() : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleShapeGuardTest,
	"Mantle.Editor.TriggerShapes.OnlyAnOverlappingBoxOrSphereIsATrigger",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleShapeGuardTest::RunTest(const FString& Parameters)
{
	using namespace MantleShapeGuardTestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	FShapeWorld Fixture;
	UWorld* World = Fixture.World;
	if (!Cube || !TestNotNull(TEXT("a test world"), World))
	{
		AddWarning(TEXT("the engine cube or the world is missing; skipped"));
		return true;
	}

	// The engine's own triggers, untouched.
	ATriggerBox* TriggerBox = World->SpawnActor<ATriggerBox>(FVector(0.0, 0.0, 0.0), FRotator::ZeroRotator);
	ATriggerSphere* TriggerSphere = World->SpawnActor<ATriggerSphere>(FVector(1000.0, 0.0, 0.0), FRotator::ZeroRotator);
	// A plain actor whose root is a blocking box (a Blueprint's hit box).
	AActor* Wall = World->SpawnActor<AActor>(FVector(2000.0, 0.0, 0.0), FRotator::ZeroRotator);
	UBoxComponent* WallBox = Wall ? NewObject<UBoxComponent>(Wall, TEXT("WallBox")) : nullptr;
	// A prop (a mesh) with a blocking sphere part.
	AStaticMeshActor* Prop = World->SpawnActor<AStaticMeshActor>(FVector(3000.0, 0.0, 0.0), FRotator::ZeroRotator);
	USphereComponent* HitSphere = Prop ? NewObject<USphereComponent>(Prop, TEXT("HitSphere")) : nullptr;
	if (!TestNotNull(TEXT("the trigger box"), TriggerBox) || !TestNotNull(TEXT("the trigger sphere"), TriggerSphere)
		|| !TestNotNull(TEXT("the wall box"), WallBox) || !TestNotNull(TEXT("the hit sphere"), HitSphere))
	{
		return false;
	}
	TriggerBox->SetActorLabel(TEXT("Zone box"));
	TriggerSphere->SetActorLabel(TEXT("Zone sphere"));
	Wall->SetActorLabel(TEXT("Wall prop"));
	Wall->SetRootComponent(WallBox);
	WallBox->SetBoxExtent(FVector(200.0, 100.0, 50.0));
	WallBox->SetCollisionProfileName(TEXT("BlockAll"));
	WallBox->RegisterComponent();
	Prop->SetActorLabel(TEXT("Crate prop"));
	Prop->GetStaticMeshComponent()->SetStaticMesh(Cube);
	HitSphere->SetupAttachment(Prop->GetRootComponent());
	HitSphere->SetSphereRadius(100.0f);
	HitSphere->SetCollisionProfileName(TEXT("BlockAll"));
	HitSphere->RegisterComponent();

	TestTrue(TEXT("a Trigger Box's box is a trigger shape"), FMantleTriggerShapeExport::IsTriggerShape(TriggerBox->GetCollisionComponent()));
	TestTrue(TEXT("so is a Trigger Sphere's"), FMantleTriggerShapeExport::IsTriggerShape(TriggerSphere->GetCollisionComponent()));
	TestFalse(TEXT("a BlockAll box is not"), FMantleTriggerShapeExport::IsTriggerShape(WallBox));
	TestTrue(TEXT("it is collision"), FMantleTriggerShapeExport::IsCollisionShape(WallBox));
	TestFalse(TEXT("a BlockAll sphere is not a trigger"), FMantleTriggerShapeExport::IsTriggerShape(HitSphere));
	TestTrue(TEXT("it is collision"), FMantleTriggerShapeExport::IsCollisionShape(HitSphere));

	EMantleEntityType Type;
	TestTrue(TEXT("a Trigger Box classifies as a trigger"), FMantlePlaceExporter::ClassifyActor(TriggerBox, Type) && Type == EMantleEntityType::TriggerVolume);
	TestTrue(TEXT("a Trigger Sphere too"), FMantlePlaceExporter::ClassifyActor(TriggerSphere, Type) && Type == EMantleEntityType::TriggerVolume);
	TestFalse(TEXT("a blocking box root is not a trigger"), FMantlePlaceExporter::ClassifyActor(Wall, Type) && Type == EMantleEntityType::TriggerVolume);

	const FMantleExportResult Result = FMantlePlaceExporter::Export(World, FMantleExportOptions());

	// The triggers still publish as triggers.
	const FMantleEntity* BoxEntity = MantleExportedEntities::Named(Result, TEXT("Zone box"));
	const FMantleEntity* SphereEntity = MantleExportedEntities::Named(Result, TEXT("Zone sphere"));
	TestTrue(TEXT("the Trigger Box is a trigger"), BoxEntity && BoxEntity->Props.TryGet<FMantleTriggerVolumeProps>());
	TestTrue(TEXT("the Trigger Sphere is a trigger"), SphereEntity && SphereEntity->Props.TryGet<FMantleTriggerVolumeProps>());

	// The blocking box root: a group whose one part is a hidden, blocking cube 400 by 200 by 100 cm.
	const FMantleEntity* WallGroup = MantleExportedEntities::Named(Result, TEXT("Wall prop"));
	if (TestNotNull(TEXT("the wall prop is published"), WallGroup))
	{
		TestFalse(TEXT("not as a trigger"), WallGroup->Props.TryGet<FMantleTriggerVolumeProps>() != nullptr);
	}
	const FMantleStaticMeshProps* WallProps = MeshPropsOf(Result, TEXT("WallBox"));
	if (TestNotNull(TEXT("the wall's box is a part with a mesh"), WallProps))
	{
		TestEqual(TEXT("the library cube"), WallProps->Mesh, FString(TEXT("ast_cube")));
		TestFalse(TEXT("never drawn"), WallProps->bVisible);
		TestTrue(TEXT("blocking"), WallProps->Collision == EMantleCollision::Blocking);
		TestTrue(TEXT("and it blocks sight traces as BlockAll does"), WallProps->bBlocksVisibility);
	}
	const FMantleEntity* WallPart = MantleExportedEntities::Named(Result, TEXT("WallBox"));
	if (TestNotNull(TEXT("the wall's part"), WallPart))
	{
		TestTrue(TEXT("sized to the box: 4 by 2 by 1"), WallPart->Transform.GetScale3D().Equals(FVector(4.0, 2.0, 1.0), 0.01));
	}

	// The prop keeps its blocking sphere as a hidden, blocking library sphere 200 cm across.
	const FMantleStaticMeshProps* SphereProps = MeshPropsOf(Result, TEXT("HitSphere"));
	if (TestNotNull(TEXT("the prop's sphere is a part with a mesh"), SphereProps))
	{
		TestEqual(TEXT("the library sphere"), SphereProps->Mesh, FString(TEXT("ast_sphere")));
		TestFalse(TEXT("never drawn"), SphereProps->bVisible);
		TestTrue(TEXT("blocking"), SphereProps->Collision == EMantleCollision::Blocking);
	}
	const FMantleEntity* SpherePart = MantleExportedEntities::Named(Result, TEXT("HitSphere"));
	if (TestNotNull(TEXT("the sphere's part"), SpherePart))
	{
		TestTrue(TEXT("sized to the sphere: 2 across"), SpherePart->Transform.GetScale3D().Equals(FVector(2.0, 2.0, 2.0), 0.01));
		TestTrue(TEXT("not a trigger"), SpherePart->Props.TryGet<FMantleTriggerVolumeProps>() == nullptr);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
