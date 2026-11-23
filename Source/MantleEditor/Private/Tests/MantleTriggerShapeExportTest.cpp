// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCoverage.h"
#include "MantleExportedEntities.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceValidator.h"
#include "MantleTriggerShapeExport.h"

#include "Builders/CubeBuilder.h"
#include "Builders/CylinderBuilder.h"
#include "Components/BrushComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TriggerCapsule.h"
#include "GameFramework/Character.h"
#include "Engine/TriggerVolume.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Model.h"

#if WITH_DEV_AUTOMATION_TESTS

// Trigger shapes beyond a Trigger Box and a Trigger Sphere (F18): a Trigger Volume brush that is a box publishes as a Box trigger, a
// Trigger Capsule as a Box (or a Sphere, when it is no taller than wide) around it. A Trigger Volume of another shape is still left out
// with a warning. Built in a transient editor world.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleTriggerShapeExportTestPrivate
{
	/** An empty editor world for one test, destroyed with it. */
	struct FTriggerWorld
	{
		UWorld* World = nullptr;

		FTriggerWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false,
				FName(*FString::Printf(TEXT("MantleTriggerShapeTestWorld_%s"), *FGuid::NewGuid().ToString())));
		}

		~FTriggerWorld()
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

	/** A Trigger Volume with an (empty) brush model, ready for a builder to shape. */
	ATriggerVolume* SpawnTriggerVolume(UWorld* World, const TCHAR* Label, const FVector& Location)
	{
		ATriggerVolume* Volume = World->SpawnActor<ATriggerVolume>(Location, FRotator::ZeroRotator);
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
			// UModel::Initialize puts the actor back at the origin (ABrush::InitPosRotScale): it goes where it was spawned again.
			Volume->SetActorLocation(Location, false);
		}
		return Volume;
	}

	ATriggerCapsule* SpawnTriggerCapsule(UWorld* World, const TCHAR* Label, const FVector& Location, float Radius, float HalfHeight)
	{
		ATriggerCapsule* Trigger = World->SpawnActor<ATriggerCapsule>(Location, FRotator::ZeroRotator);
		if (Trigger)
		{
			Trigger->SetActorLabel(Label);
			if (UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(Trigger->GetCollisionComponent()))
			{
				Capsule->SetCapsuleSize(Radius, HalfHeight);
			}
		}
		return Trigger;
	}

	bool HasIssue(const FMantleValidationResult& Result, const TCHAR* Actor, EMantleIssueSeverity Severity)
	{
		return Result.Issues.ContainsByPredicate([Actor, Severity](const FMantleValidationIssue& Issue)
		{
			return Issue.Actor == Actor && Issue.Severity == Severity;
		});
	}

	const FMantleTriggerVolumeProps* TriggerPropsOf(const FMantleExportResult& Result, const TCHAR* Name)
	{
		const FMantleEntity* Entity = MantleExportedEntities::Named(Result, Name);
		return Entity ? Entity->Props.TryGet<FMantleTriggerVolumeProps>() : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTriggerShapeExportTest,
	"Mantle.Editor.TriggerShapes.BrushAndCapsuleTriggersPublish",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTriggerShapeExportTest::RunTest(const FString& Parameters)
{
	using namespace MantleTriggerShapeExportTestPrivate;
	if (!GEditor)
	{
		AddWarning(TEXT("no editor; skipped"));
		return true;
	}
	FTriggerWorld Fixture;
	UWorld* World = Fixture.World;
	if (!TestNotNull(TEXT("a test world"), World))
	{
		return false;
	}

	ATriggerVolume* Box = SpawnTriggerVolume(World, TEXT("Door zone"), FVector(1000.0, 200.0, 50.0));
	ATriggerVolume* Round = SpawnTriggerVolume(World, TEXT("Round zone"), FVector(-2000.0, 0.0, 0.0));
	ATriggerCapsule* Tall = SpawnTriggerCapsule(World, TEXT("Tall capsule"), FVector(0.0, 3000.0, 0.0), 50.0f, 150.0f);
	ATriggerCapsule* Ball = SpawnTriggerCapsule(World, TEXT("Ball capsule"), FVector(0.0, -3000.0, 0.0), 80.0f, 80.0f);
	if (!TestNotNull(TEXT("the box zone"), Box) || !TestNotNull(TEXT("the round zone"), Round)
		|| !TestNotNull(TEXT("the tall capsule"), Tall) || !TestNotNull(TEXT("the ball capsule"), Ball))
	{
		return false;
	}
	UCubeBuilder* Cube = NewObject<UCubeBuilder>();
	Cube->X = 400.0f;
	Cube->Y = 200.0f;
	Cube->Z = 300.0f;
	Cube->Build(World, Box);
	UCylinderBuilder* Cylinder = NewObject<UCylinderBuilder>();
	Cylinder->Sides = 8;
	Cylinder->OuterRadius = 100.0f;
	Cylinder->Z = 300.0f;
	Cylinder->Build(World, Round);

	TestTrue(TEXT("a box brush is a published trigger"), FMantleTriggerShapeExport::IsBoxTriggerBrush(Box));
	TestTrue(TEXT("a cylinder brush is one too, as the box around its bounds"), FMantleTriggerShapeExport::IsBoxTriggerBrush(Round));

	const FMantleExportResult Result = FMantlePlaceExporter::Export(World, FMantleExportOptions());

	// The brush: a Box trigger of the brush's half-size, placed where the actor is.
	const FMantleEntity* BoxEntity = MantleExportedEntities::Named(Result, TEXT("Door zone"));
	if (TestNotNull(TEXT("the brush trigger is published"), BoxEntity))
	{
		TestTrue(TEXT("as a trigger"), BoxEntity->Type == EMantleEntityType::TriggerVolume);
		TestTrue(TEXT("centered on the brush"), BoxEntity->Transform.GetLocation().Equals(Box->GetActorLocation(), 0.5));
	}
	const FMantleTriggerVolumeProps* BoxProps = TriggerPropsOf(Result, TEXT("Door zone"));
	if (TestNotNull(TEXT("with trigger properties"), BoxProps))
	{
		TestTrue(TEXT("a box"), BoxProps->Shape == EMantleTriggerShape::Box);
		TestTrue(TEXT("of half-size 200 by 100 by 150"), BoxProps->Extent.Equals(FVector(200.0, 100.0, 150.0), 0.01));
	}

	// The capsules: a box around the tall one, a sphere for the one no taller than wide.
	const FMantleTriggerVolumeProps* TallProps = TriggerPropsOf(Result, TEXT("Tall capsule"));
	if (TestNotNull(TEXT("the tall capsule is published as a trigger"), TallProps))
	{
		TestTrue(TEXT("a box"), TallProps->Shape == EMantleTriggerShape::Box);
		TestTrue(TEXT("50 wide each way, 150 up"), TallProps->Extent.Equals(FVector(50.0, 50.0, 150.0), 0.01));
	}
	const FMantleTriggerVolumeProps* BallProps = TriggerPropsOf(Result, TEXT("Ball capsule"));
	if (TestNotNull(TEXT("the round capsule is published as a trigger"), BallProps))
	{
		TestTrue(TEXT("a sphere"), BallProps->Shape == EMantleTriggerShape::Sphere);
		TestTrue(TEXT("of radius 80"), BallProps->Extent.Equals(FVector(80.0, 80.0, 80.0), 0.01));
	}

	// The cylinder brush publishes as a box around its bounds (radius 100, height 300: about 100 by 100 by 150 half-size).
	const FMantleTriggerVolumeProps* RoundProps = TriggerPropsOf(Result, TEXT("Round zone"));
	if (TestNotNull(TEXT("a cylinder brush is published as a trigger"), RoundProps))
	{
		TestTrue(TEXT("a box"), RoundProps->Shape == EMantleTriggerShape::Box);
		TestTrue(TEXT("about the cylinder's width each way"), RoundProps->Extent.X > 80.0 && RoundProps->Extent.X <= 100.5 && RoundProps->Extent.Y > 80.0 && RoundProps->Extent.Y <= 100.5);
		TestEqual(TEXT("and half its height up"), RoundProps->Extent.Z, 150.0, 0.5);
	}
	TestFalse(TEXT("Export refuses nothing here"), Result.Errors.ContainsByPredicate([](const FMantleParseError& Error)
	{
		return Error.Message.Contains(TEXT("Door zone")) || Error.Message.Contains(TEXT("capsule")) || Error.Message.Contains(TEXT("Round zone"));
	}));
	TestEqual(TEXT("no volume is dropped"), Result.Coverage.Count(MantleCoverageCategory::Volumes, EMantleCoverageOutcome::Dropped), static_cast<int64>(0));
	TestEqual(TEXT("one is kept"), Result.Coverage.Count(MantleCoverageCategory::Volumes, EMantleCoverageOutcome::Kept), static_cast<int64>(1));
	TestEqual(TEXT("both capsules and the cylinder are approximated"), Result.Coverage.Count(MantleCoverageCategory::Volumes, EMantleCoverageOutcome::Approximated), static_cast<int64>(3));

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(World);
	TestFalse(TEXT("Check says nothing about the brush trigger"), HasIssue(Validation, TEXT("Door zone"), EMantleIssueSeverity::Warning) || HasIssue(Validation, TEXT("Door zone"), EMantleIssueSeverity::Error));
	TestFalse(TEXT("nor about the capsules"), HasIssue(Validation, TEXT("Tall capsule"), EMantleIssueSeverity::Error) || HasIssue(Validation, TEXT("Ball capsule"), EMantleIssueSeverity::Error));
	TestTrue(TEXT("Check says each capsule publishes as another shape"), HasIssue(Validation, TEXT("Tall capsule"), EMantleIssueSeverity::Warning) && HasIssue(Validation, TEXT("Ball capsule"), EMantleIssueSeverity::Warning));
	TestTrue(TEXT("Check says the cylinder publishes as a box around it"), HasIssue(Validation, TEXT("Round zone"), EMantleIssueSeverity::Warning));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTriggerCapsuleGuardTest,
	"Mantle.Editor.TriggerShapes.OnlyAnOverlappingCapsuleIsATrigger",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTriggerCapsuleGuardTest::RunTest(const FString& Parameters)
{
	using namespace MantleTriggerShapeExportTestPrivate;
	FTriggerWorld Fixture;
	UWorld* World = Fixture.World;
	if (!TestNotNull(TEXT("a test world"), World))
	{
		return false;
	}

	// A placed Character: its root is a capsule that blocks. It classifies as it did before (not at all), and is no trigger.
	ACharacter* Character = World->SpawnActor<ACharacter>(FVector(0.0, 0.0, 200.0), FRotator::ZeroRotator);
	// A plain actor whose root is a blocking capsule (a Blueprint prop's collision).
	AActor* Prop = World->SpawnActor<AActor>(FVector(1000.0, 0.0, 0.0), FRotator::ZeroRotator);
	UCapsuleComponent* PropCapsule = Prop ? NewObject<UCapsuleComponent>(Prop, TEXT("Collision")) : nullptr;
	// A capsule that overlaps but generates no overlap events.
	AActor* Silent = World->SpawnActor<AActor>(FVector(2000.0, 0.0, 0.0), FRotator::ZeroRotator);
	UCapsuleComponent* SilentCapsule = Silent ? NewObject<UCapsuleComponent>(Silent, TEXT("Collision")) : nullptr;
	ATriggerCapsule* Real = SpawnTriggerCapsule(World, TEXT("Real trigger"), FVector(3000.0, 0.0, 0.0), 50.0f, 150.0f);
	if (!TestNotNull(TEXT("the character"), Character) || !TestNotNull(TEXT("the prop capsule"), PropCapsule)
		|| !TestNotNull(TEXT("the silent capsule"), SilentCapsule) || !TestNotNull(TEXT("the trigger"), Real))
	{
		return false;
	}
	Prop->SetRootComponent(PropCapsule);
	PropCapsule->SetCollisionProfileName(TEXT("BlockAll"));
	PropCapsule->RegisterComponent();
	Prop->SetActorLabel(TEXT("Blocking prop"));
	Silent->SetRootComponent(SilentCapsule);
	SilentCapsule->SetCollisionProfileName(TEXT("OverlapAllDynamic"));
	SilentCapsule->SetGenerateOverlapEvents(false);
	SilentCapsule->RegisterComponent();
	Silent->SetActorLabel(TEXT("Silent capsule"));

	TestFalse(TEXT("a character's capsule is not a trigger"), FMantleTriggerShapeExport::IsTriggerCapsule(Character->GetCapsuleComponent()));
	TestFalse(TEXT("nor a blocking prop's"), FMantleTriggerShapeExport::IsTriggerCapsule(PropCapsule));
	TestFalse(TEXT("nor one that makes no overlap events"), FMantleTriggerShapeExport::IsTriggerCapsule(SilentCapsule));
	TestTrue(TEXT("a Trigger Capsule is"), FMantleTriggerShapeExport::IsTriggerCapsule(Real->GetCollisionComponent()));

	EMantleEntityType Type;
	TestFalse(TEXT("a Character does not classify (as before)"), FMantlePlaceExporter::ClassifyActor(Character, Type));
	TestFalse(TEXT("a blocking capsule actor does not classify as a trigger (as before)"), FMantlePlaceExporter::ClassifyActor(Prop, Type));
	TestFalse(TEXT("a capsule with no overlap events does not either"), FMantlePlaceExporter::ClassifyActor(Silent, Type));
	TestTrue(TEXT("a Trigger Capsule does"), FMantlePlaceExporter::ClassifyActor(Real, Type) && Type == EMantleEntityType::TriggerVolume);

	const FMantleExportResult Result = FMantlePlaceExporter::Export(World, FMantleExportOptions());
	TestNull(TEXT("the character is not published"), MantleExportedEntities::Named(Result, Character->GetActorLabel()));
	TestNull(TEXT("the blocking prop is not published as a trigger"), TriggerPropsOf(Result, TEXT("Blocking prop")));
	TestNull(TEXT("the silent capsule is not published as a trigger"), TriggerPropsOf(Result, TEXT("Silent capsule")));
	TestNotNull(TEXT("the real trigger is"), TriggerPropsOf(Result, TEXT("Real trigger")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTriggerRootWithContentTest,
	"Mantle.Editor.TriggerShapes.ATriggerRootKeepsItsDrawnContent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTriggerRootWithContentTest::RunTest(const FString& Parameters)
{
	using namespace MantleTriggerShapeExportTestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	FTriggerWorld Fixture;
	UWorld* World = Fixture.World;
	if (!Cube || !TestNotNull(TEXT("a test world"), World))
	{
		AddWarning(TEXT("the engine cube or the world is missing; skipped"));
		return true;
	}

	// An actor whose root only overlaps (a capsule, then a box) with a drawn mesh beside it: a Blueprint prop of "trigger zone + statue".
	struct FCase
	{
		const TCHAR* Label;
		const TCHAR* RootName;
		const TCHAR* MeshName;
		bool bCapsule;
	};
	const FCase Cases[] = { { TEXT("Statue zone"), TEXT("ZoneCapsule"), TEXT("Statue"), true }, { TEXT("Bust zone"), TEXT("ZoneBox"), TEXT("Bust"), false } };
	double X = 0.0;
	for (const FCase& Case : Cases)
	{
		AActor* Actor = World->SpawnActor<AActor>(FVector(X, 0.0, 0.0), FRotator::ZeroRotator);
		X += 1000.0;
		if (!TestNotNull(TEXT("the actor"), Actor))
		{
			return false;
		}
		Actor->SetActorLabel(Case.Label);
		USceneComponent* Root = nullptr;
		if (Case.bCapsule)
		{
			UCapsuleComponent* Capsule = NewObject<UCapsuleComponent>(Actor, Case.RootName);
			Capsule->SetCapsuleSize(50.0f, 150.0f);
			Root = Capsule;
		}
		else
		{
			UBoxComponent* Box = NewObject<UBoxComponent>(Actor, Case.RootName);
			Box->SetBoxExtent(FVector(100.0, 100.0, 50.0));
			Root = Box;
		}
		Actor->SetRootComponent(Root);
		Root->RegisterComponent();
		UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(Actor, Case.MeshName);
		Mesh->SetStaticMesh(Cube);
		Mesh->SetupAttachment(Root);
		Mesh->RegisterComponent();
		TestTrue(TEXT("the root is a trigger shape"), FMantleTriggerShapeExport::IsTriggerShape(Root));
	}

	const FMantleExportResult Result = FMantlePlaceExporter::Export(World, FMantleExportOptions());
	for (const FCase& Case : Cases)
	{
		// Both come out: the trigger as one part and the mesh as another, in a Group. Nothing is lost silently.
		const FMantleEntity* Group = MantleExportedEntities::Named(Result, Case.Label);
		if (TestNotNull(TEXT("the actor is published"), Group))
		{
			TestTrue(TEXT("as a Group of parts"), Group->Type == EMantleEntityType::Group);
		}
		const FMantleEntity* Zone = MantleExportedEntities::Named(Result, Case.RootName);
		TestTrue(TEXT("its trigger shape is a trigger part"), Zone && Zone->Props.TryGet<FMantleTriggerVolumeProps>() && Zone->Parent == (Group ? Group->Id : FString()));
		const FMantleEntity* Statue = MantleExportedEntities::Named(Result, Case.MeshName);
		const FMantleStaticMeshProps* StatueProps = Statue ? Statue->Props.TryGet<FMantleStaticMeshProps>() : nullptr;
		if (TestNotNull(TEXT("its drawn mesh is a mesh part"), StatueProps))
		{
			TestTrue(TEXT("drawn"), StatueProps->bVisible);
			TestTrue(TEXT("a child of the same group"), Statue->Parent == (Group ? Group->Id : FString()));
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
