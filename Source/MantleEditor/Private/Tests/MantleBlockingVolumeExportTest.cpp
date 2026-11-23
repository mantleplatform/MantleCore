// Copyright Joshua Eger. All Rights Reserved.
#include "MantleBlockingVolumeExport.h"
#include "MantleCoverage.h"
#include "MantleExportedEntities.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceValidator.h"

#include "Builders/CubeBuilder.h"
#include "Builders/CylinderBuilder.h"
#include "Components/BrushComponent.h"
#include "Engine/BlockingVolume.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model.h"

#if WITH_DEV_AUTOMATION_TESTS

// Blocking Volumes (F17): a box-shaped one publishes as an invisible wall (a hidden, blocking library cube stretched over the box); a
// volume of another shape is still left out with a warning; one that collides with nothing is left out silently.
// Built in a transient editor world.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleBlockingVolumeExportTestPrivate
{
	/** An empty editor world for one test, destroyed with it. */
	struct FWallWorld
	{
		UWorld* World = nullptr;

		FWallWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false,
				FName(*FString::Printf(TEXT("MantleBlockingVolumeTestWorld_%s"), *FGuid::NewGuid().ToString())));
		}

		~FWallWorld()
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

	/** A Blocking Volume with an (empty) brush model, ready for a builder to shape. */
	ABlockingVolume* SpawnVolume(UWorld* World, const TCHAR* Label, const FVector& Location, const FRotator& Rotation = FRotator::ZeroRotator)
	{
		ABlockingVolume* Volume = World->SpawnActor<ABlockingVolume>(Location, Rotation);
		if (!Volume)
		{
			return nullptr;
		}
		Volume->SetActorLabel(Label);
		if (!Volume->Brush)
		{
			Volume->Brush = NewObject<UModel>(Volume, NAME_None, RF_Transactional);
			Volume->Brush->Initialize(Volume, /*InRootOutside*/ true);
			Volume->GetBrushComponent()->Brush = Volume->Brush;
		}
		return Volume;
	}

	void ShapeAsBox(UWorld* World, ABlockingVolume* Volume, float X, float Y, float Z)
	{
		UCubeBuilder* Builder = NewObject<UCubeBuilder>();
		Builder->X = X;
		Builder->Y = Y;
		Builder->Z = Z;
		Builder->Build(World, Volume);
	}

	bool HasIssue(const FMantleValidationResult& Result, const TCHAR* Actor, EMantleIssueSeverity Severity)
	{
		return Result.Issues.ContainsByPredicate([Actor, Severity](const FMantleValidationIssue& Issue)
		{
			return Issue.Actor == Actor && Issue.Severity == Severity;
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBlockingVolumeExportTest,
	"Mantle.Editor.BlockingVolume.BoxPublishesAsAnInvisibleWall",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBlockingVolumeExportTest::RunTest(const FString& Parameters)
{
	using namespace MantleBlockingVolumeExportTestPrivate;
	if (!GEditor)
	{
		AddWarning(TEXT("no editor; skipped"));
		return true;
	}
	FWallWorld Fixture;
	UWorld* World = Fixture.World;
	if (!TestNotNull(TEXT("a test world"), World))
	{
		return false;
	}

	ABlockingVolume* Wall = SpawnVolume(World, TEXT("Map edge wall"), FVector(1000.0, 200.0, 50.0));
	ABlockingVolume* Turned = SpawnVolume(World, TEXT("Turned wall"), FVector(0.0, 3000.0, 0.0), FRotator(0.0, 90.0, 0.0));
	ABlockingVolume* Round = SpawnVolume(World, TEXT("Round pillar"), FVector(-2000.0, 0.0, 0.0));
	ABlockingVolume* Loose = SpawnVolume(World, TEXT("Loose volume"), FVector(-3000.0, 0.0, 0.0));
	ABlockingVolume* Ghost = SpawnVolume(World, TEXT("Ghost box"), FVector(-4000.0, 0.0, 0.0));
	if (!TestNotNull(TEXT("the wall"), Wall) || !TestNotNull(TEXT("the turned wall"), Turned) || !TestNotNull(TEXT("the pillar"), Round)
		|| !TestNotNull(TEXT("the loose volume"), Loose) || !TestNotNull(TEXT("the ghost"), Ghost))
	{
		return false;
	}
	ShapeAsBox(World, Wall, 400.0f, 200.0f, 300.0f);
	ShapeAsBox(World, Turned, 1000.0f, 100.0f, 500.0f);
	Turned->SetActorScale3D(FVector(2.0, 1.0, 1.0));
	ShapeAsBox(World, Ghost, 100.0f, 100.0f, 100.0f);
	Ghost->GetBrushComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	UCylinderBuilder* Cylinder = NewObject<UCylinderBuilder>();
	Cylinder->Sides = 8;
	Cylinder->OuterRadius = 100.0f;
	Cylinder->Z = 300.0f;
	Cylinder->Build(World, Round);

	// Read as walls: the boxes, not the cylinder, not a volume with no shape.
	FMantleBlockingWall Read;
	TestTrue(TEXT("a box volume is a wall"), FMantleBlockingVolumeExport::ReadWall(Wall, Read));
	TestFalse(TEXT("a cylinder volume is not"), FMantleBlockingVolumeExport::ReadWall(Round, Read));
	TestFalse(TEXT("a volume with no shape is not"), FMantleBlockingVolumeExport::ReadWall(Loose, Read));
	TestFalse(TEXT("a box that collides with nothing is not"), FMantleBlockingVolumeExport::ReadWall(Ghost, Read));
	TestTrue(TEXT("and it says it has no collision"), FMantleBlockingVolumeExport::HasNoCollision(Ghost));

	const FMantleExportResult Result = FMantlePlaceExporter::Export(World, FMantleExportOptions());

	// The wall: a hidden blocking library cube, stretched to the box, never drawn.
	const FMantleEntity* WallEntity = MantleExportedEntities::Named(Result, TEXT("Map edge wall"));
	if (TestNotNull(TEXT("the wall is published"), WallEntity))
	{
		TestTrue(TEXT("as a mesh"), WallEntity->Type == EMantleEntityType::StaticMesh);
		const FMantleStaticMeshProps* Props = WallEntity->Props.TryGet<FMantleStaticMeshProps>();
		if (TestNotNull(TEXT("with mesh properties"), Props))
		{
			TestEqual(TEXT("the library cube"), Props->Mesh, FString(TEXT("ast_cube")));
			TestFalse(TEXT("never drawn"), Props->bVisible);
			TestTrue(TEXT("blocking"), Props->Collision == EMantleCollision::Blocking);
			TestFalse(TEXT("an invisible wall lets sight traces through"), Props->bBlocksVisibility);
			TestEqual(TEXT("no materials"), Props->Materials.Num(), 0);
			TestFalse(TEXT("no shadow"), Props->bCastShadow);
		}
		TestTrue(TEXT("stretched over the box: 4 by 2 by 3 hundred cm"), WallEntity->Transform.GetScale3D().Equals(FVector(4.0, 2.0, 3.0), 0.01));
		TestTrue(TEXT("centered on the box"), WallEntity->Transform.GetLocation().Equals(Wall->GetActorLocation(), 0.5));
	}

	// The turned, stretched one: the actor's own scale and turn carry over.
	const FMantleEntity* TurnedEntity = MantleExportedEntities::Named(Result, TEXT("Turned wall"));
	if (TestNotNull(TEXT("the turned wall is published"), TurnedEntity))
	{
		TestTrue(TEXT("its scale is the box's size times the actor's: 20 by 1 by 5"), TurnedEntity->Transform.GetScale3D().Equals(FVector(20.0, 1.0, 5.0), 0.01));
		const FVector Along = TurnedEntity->Transform.GetRotation().RotateVector(FVector::XAxisVector);
		TestTrue(TEXT("and it is turned a quarter"), Along.Equals(FVector::YAxisVector, 0.01));
	}

	// Left out, and said: the cylinder and the shapeless volume warn; the one that collides with nothing does not.
	TestNull(TEXT("a cylinder is not published"), MantleExportedEntities::Named(Result, TEXT("Round pillar")));
	TestNull(TEXT("a shapeless volume is not published"), MantleExportedEntities::Named(Result, TEXT("Loose volume")));
	TestNull(TEXT("a volume that collides with nothing is not published"), MantleExportedEntities::Named(Result, TEXT("Ghost box")));
	TestEqual(TEXT("two volumes are named as dropped"), Result.Coverage.Count(MantleCoverageCategory::Volumes, EMantleCoverageOutcome::Dropped), static_cast<int64>(2));
	TestEqual(TEXT("two walls are kept"), Result.Coverage.Count(MantleCoverageCategory::Volumes, EMantleCoverageOutcome::Kept), static_cast<int64>(2));
	TestEqual(TEXT("both walls count as collision only"), Result.CollisionOnlyEntities, 2);

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(World);
	TestFalse(TEXT("Check says nothing about the wall"), HasIssue(Validation, TEXT("Map edge wall"), EMantleIssueSeverity::Warning) || HasIssue(Validation, TEXT("Map edge wall"), EMantleIssueSeverity::Error));
	TestFalse(TEXT("nor the ghost"), HasIssue(Validation, TEXT("Ghost box"), EMantleIssueSeverity::Warning));
	TestTrue(TEXT("Check warns about the cylinder"), HasIssue(Validation, TEXT("Round pillar"), EMantleIssueSeverity::Warning));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
