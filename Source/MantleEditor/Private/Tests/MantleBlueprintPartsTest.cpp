// Copyright Joshua Eger. All Rights Reserved.
#include "MantleActorDecompose.h"
#include "MantleCoverage.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceValidator.h"

#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LocalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/PointLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// What a Blueprint made of several components does at Check, Export and Gather (the three ask the same questions): a spline, a
// hidden or empty component and an instanced mesh with nothing to export are not extra components; a negative scale on a
// Blueprint's root or parts publishes as a mirrored copy at the positive scale; a light with an attenuation radius of 0 lights
// nothing in Unreal and is left out with a warning. Built in a transient editor world.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleBlueprintPartsTestPrivate
{
	const TCHAR* const CubePath = TEXT("/Engine/BasicShapes/Cube.Cube");

	/** An empty editor world for one test, destroyed with it. */
	struct FPartsWorld
	{
		UWorld* World = nullptr;

		FPartsWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleBlueprintPartsTestWorld"));
		}

		~FPartsWorld()
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

	/** A plain actor with a plain scene component as its root: a Group by ClassifyActor's rule. */
	AActor* SpawnGroupActor(UWorld* World, const TCHAR* Label)
	{
		AActor* Actor = World->SpawnActor<AActor>();
		if (Actor)
		{
			USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"));
			Actor->SetRootComponent(Root);
			Root->RegisterComponent();
			Actor->SetActorLabel(Label);
		}
		return Actor;
	}

	template <typename T>
	T* AddScene(AActor* Actor, const TCHAR* Name)
	{
		T* Component = NewObject<T>(Actor, Name);
		Component->SetupAttachment(Actor->GetRootComponent());
		Component->RegisterComponent();
		return Component;
	}

	UStaticMeshComponent* AddMesh(AActor* Actor, const TCHAR* Name, UStaticMesh* Mesh)
	{
		UStaticMeshComponent* Component = AddScene<UStaticMeshComponent>(Actor, Name);
		Component->SetStaticMesh(Mesh);
		return Component;
	}

	/** Whether Result holds an issue of Severity for Actor. */
	bool HasIssue(const FMantleValidationResult& Result, const TCHAR* Actor, EMantleIssueSeverity Severity, const TCHAR* Contains = nullptr)
	{
		return Result.Issues.ContainsByPredicate([Actor, Severity, Contains](const FMantleValidationIssue& Issue)
		{
			return Issue.Actor == Actor && Issue.Severity == Severity && (!Contains || Issue.Problem.Contains(Contains));
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBlueprintPartsNothingToPublishTest,
	"Mantle.Editor.BlueprintParts.NothingToPublishIsNotAnExtraComponent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBlueprintPartsNothingToPublishTest::RunTest(const FString& Parameters)
{
	using namespace MantleBlueprintPartsTestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
	if (!Cube)
	{
		AddWarning(TEXT("the engine cube did not load; skipped"));
		return true;
	}
	FPartsWorld Fixture;
	AActor* Actor = SpawnGroupActor(Fixture.World, TEXT("BP_SplineMeshActor"));
	if (!TestNotNull(TEXT("the actor spawned"), Actor))
	{
		return false;
	}

	// A spline is authoring data.
	AddScene<USplineComponent>(Actor, TEXT("Spline"));
	// A collision shape nobody can see or touch.
	UCapsuleComponent* Capsule = AddScene<UCapsuleComponent>(Actor, TEXT("Capsule"));
	Capsule->SetHiddenInGame(true);
	Capsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// A mesh component hidden in game that does not collide, and one with no mesh at all.
	UStaticMeshComponent* Hidden = AddMesh(Actor, TEXT("Hidden"), Cube);
	Hidden->SetVisibility(false);
	Hidden->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	AddMesh(Actor, TEXT("Empty"), nullptr);
	// An instanced mesh with no instances: nothing for Export to write.
	UInstancedStaticMeshComponent* Planks = AddScene<UInstancedStaticMeshComponent>(Actor, TEXT("FencePlank1"));
	Planks->SetStaticMesh(Cube);

	TestFalse(TEXT("a spline is not game content"), FMantlePlaceExporter::IsGameContent(Actor->FindComponentByClass<USplineComponent>()));
	TestFalse(TEXT("nor is a hidden capsule that does not collide"), FMantlePlaceExporter::IsGameContent(Capsule));

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestFalse(TEXT("Check raises no extra-component error for the actor"), HasIssue(Validation, TEXT("BP_SplineMeshActor"), EMantleIssueSeverity::Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBlueprintPartsCapsuleWarningTest,
	"Mantle.Editor.BlueprintParts.CapsuleOnAnotherTypeIsAWarning",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBlueprintPartsCapsuleWarningTest::RunTest(const FString& Parameters)
{
	using namespace MantleBlueprintPartsTestPrivate;
	FPartsWorld Fixture;
	APointLight* Light = Fixture.World->SpawnActor<APointLight>();
	if (!TestNotNull(TEXT("the light spawned"), Light))
	{
		return false;
	}
	Light->SetActorLabel(TEXT("BP_PCG_Jungle_Floating"));
	// A capsule a player could touch: a point light has room for one component only, and Mantle has no entity for a capsule.
	UCapsuleComponent* Capsule = AddScene<UCapsuleComponent>(Light, TEXT("Capsule"));
	Capsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Capsule->SetHiddenInGame(true);
	TestTrue(TEXT("it is content a player could touch"), FMantlePlaceExporter::IsGameContent(Capsule));

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestFalse(TEXT("it is not an error"), HasIssue(Validation, TEXT("BP_PCG_Jungle_Floating"), EMantleIssueSeverity::Error));
	TestTrue(TEXT("it is named, with a warning"), HasIssue(Validation, TEXT("BP_PCG_Jungle_Floating"), EMantleIssueSeverity::Warning, TEXT("CapsuleComponent")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBlueprintPartsMirroredPartTest,
	"Mantle.Editor.BlueprintParts.NegativeScalePartPublishesMirrored",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBlueprintPartsMirroredPartTest::RunTest(const FString& Parameters)
{
	using namespace MantleBlueprintPartsTestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
	if (!Cube)
	{
		AddWarning(TEXT("the engine cube did not load; skipped"));
		return true;
	}
	FPartsWorld Fixture;

	// A part with a negative scale of its own, in a group that is not mirrored: a mirrored copy at the positive scale.
	AActor* Building = SpawnGroupActor(Fixture.World, TEXT("BP_Building_07"));
	if (!TestNotNull(TEXT("the building spawned"), Building))
	{
		return false;
	}
	AddMesh(Building, TEXT("SM_MetalPlate_06_StaticMeshComponent0"), Cube)->SetRelativeScale3D(FVector(-1.2907, 1.29071, 1.24714));

	TArray<FMantlePrefabPart> Parts;
	FMantlePlaceExporter::CollectPrefabParts(Building, Parts);
	if (TestEqual(TEXT("the building has its one part"), Parts.Num(), 1))
	{
		TestNotNull(TEXT("published as a mirrored copy of its mesh"), Parts[0].MirroredMesh);
		TestTrue(TEXT("at the positive scale"), Parts[0].Transform.GetScale3D().Equals(FVector(1.2907, 1.29071, 1.24714), 0.0001));
	}

	// A part whose negative axis the group's own mirror cancels: no mirrored copy is needed, and the scale is positive.
	AActor* Lantern = SpawnGroupActor(Fixture.World, TEXT("BP_WallLantern"));
	if (!TestNotNull(TEXT("the lantern spawned"), Lantern))
	{
		return false;
	}
	Lantern->SetActorScale3D(FVector(-1.0, 1.0, 1.0));
	AddMesh(Lantern, TEXT("Arm"), Cube)->SetRelativeScale3D(FVector(-1.0, 1.0, 1.0));
	AddMesh(Lantern, TEXT("Lamp"), Cube);

	FMantlePlaceExporter::CollectPrefabParts(Lantern, Parts);
	if (TestEqual(TEXT("the lantern has two parts"), Parts.Num(), 2))
	{
		for (const FMantlePrefabPart& Part : Parts)
		{
			TestTrue(*FString::Printf(TEXT("'%s' is at the positive scale"), *Part.Component->GetName()), Part.Transform.GetScale3D().Equals(FVector::OneVector, 0.0001));
			// The arm was mirrored twice (its own scale and the group's), so it needs no copy; the lamp is mirrored once.
			TestEqual(*FString::Printf(TEXT("'%s' is a mirrored copy only when mirrored once"), *Part.Component->GetName()),
				Part.MirroredMesh != nullptr, Part.Component->GetName() == TEXT("Lamp"));
		}
	}

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestFalse(TEXT("Check refuses neither building part"), HasIssue(Validation, TEXT("BP_Building_07"), EMantleIssueSeverity::Error));
	TestFalse(TEXT("nor the mirrored lantern"), HasIssue(Validation, TEXT("BP_WallLantern"), EMantleIssueSeverity::Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBlueprintPartsChildActorTest,
	"Mantle.Editor.BlueprintParts.MirroredGroupWithAChildActorPublishes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBlueprintPartsChildActorTest::RunTest(const FString& Parameters)
{
	using namespace MantleBlueprintPartsTestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
	if (!Cube)
	{
		AddWarning(TEXT("the engine cube did not load; skipped"));
		return true;
	}
	FPartsWorld Fixture;
	AActor* Lantern = SpawnGroupActor(Fixture.World, TEXT("BP_WallLantern10"));
	AStaticMeshActor* Child = Fixture.World->SpawnActor<AStaticMeshActor>();
	if (!TestNotNull(TEXT("the lantern spawned"), Lantern) || !TestNotNull(TEXT("and its child actor"), Child))
	{
		return false;
	}
	Child->SetActorLabel(TEXT("LanternChild"));
	Child->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
	Child->GetStaticMeshComponent()->SetStaticMesh(Cube);
	Child->AttachToActor(Lantern, FAttachmentTransformRules::KeepRelativeTransform);
	Child->GetRootComponent()->SetRelativeLocation(FVector(100.0, 0.0, 0.0));
	Lantern->SetActorScale3D(FVector(-1.0, 1.0, 1.0));

	// A child actor no longer keeps the group from mirroring.
	TestTrue(TEXT("the group still mirrors"), FMantleActorDecompose::GroupMirrorSigns(Lantern).Equals(FVector(-1.0, 1.0, 1.0), 0.0001));

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestFalse(TEXT("Check does not refuse the lantern"), HasIssue(Validation, TEXT("BP_WallLantern10"), EMantleIssueSeverity::Error));
	TestFalse(TEXT("nor its child"), HasIssue(Validation, TEXT("LanternChild"), EMantleIssueSeverity::Error));

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	const FMantleEntity* Group = Result.Place.Entities.FindByPredicate([](const FMantleEntity& Candidate) { return Candidate.Name == TEXT("BP_WallLantern10"); });
	const FMantleEntity* Moved = Result.Place.Entities.FindByPredicate([](const FMantleEntity& Candidate) { return Candidate.Name == TEXT("LanternChild"); });
	if (TestNotNull(TEXT("the lantern is exported"), Group) && TestNotNull(TEXT("and its child"), Moved))
	{
		TestTrue(TEXT("the lantern is at the positive scale"), Group->Transform.GetScale3D().Equals(FVector::OneVector, 0.0001));
		TestEqual(TEXT("the child is still its child"), Moved->Parent, Group->Id);
		TestTrue(TEXT("and sits where the mirror put it"), Moved->Transform.GetLocation().Equals(FVector(-100.0, 0.0, 0.0), 0.001));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleBlueprintPartsZeroRadiusLightTest,
	"Mantle.Editor.BlueprintParts.LightWithRadiusZeroIsLeftOutWithAWarning",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleBlueprintPartsZeroRadiusLightTest::RunTest(const FString& Parameters)
{
	using namespace MantleBlueprintPartsTestPrivate;
	FPartsWorld Fixture;

	// A light actor on its own.
	APointLight* Lamp = Fixture.World->SpawnActor<APointLight>();
	if (!TestNotNull(TEXT("the lamp spawned"), Lamp))
	{
		return false;
	}
	Lamp->SetActorLabel(TEXT("DeadLamp"));
	Cast<ULocalLightComponent>(Lamp->GetRootComponent())->AttenuationRadius = 0.0f;

	// A prefab whose only part is a light with a radius of 0, and one whose second light has a radius.
	AActor* Prefab = SpawnGroupActor(Fixture.World, TEXT("BP_lamps_tree_light"));
	if (!TestNotNull(TEXT("the prefab spawned"), Prefab))
	{
		return false;
	}
	AddScene<UPointLightComponent>(Prefab, TEXT("PointLight"))->AttenuationRadius = 0.0f;

	AActor* Pair = SpawnGroupActor(Fixture.World, TEXT("BP_LampPair"));
	if (!TestNotNull(TEXT("the pair spawned"), Pair))
	{
		return false;
	}
	AddScene<UPointLightComponent>(Pair, TEXT("Dead"))->AttenuationRadius = 0.0f;
	AddScene<UPointLightComponent>(Pair, TEXT("Lit"))->AttenuationRadius = 500.0f;

	TArray<FMantlePrefabPart> Parts;
	TArray<FMantleUnpublishedPart> Unsupported;
	FMantlePlaceExporter::CollectPrefabParts(Pair, Parts, &Unsupported);
	if (TestEqual(TEXT("only the light with a radius is a part"), Parts.Num(), 1))
	{
		TestEqual(TEXT("and it is the lit one"), Parts[0].Component->GetName(), FString(TEXT("Lit")));
	}
	const FMantleUnpublishedPart* Dead = Unsupported.FindByPredicate([](const FMantleUnpublishedPart& Part) { return Part.What.Contains(TEXT("attenuation radius of 0")); });
	if (TestNotNull(TEXT("the other is named"), Dead))
	{
		TestFalse(TEXT("and does not block the publish"), Dead->bBlocksPublish);
	}

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
	for (const TCHAR* Label : { TEXT("DeadLamp"), TEXT("BP_lamps_tree_light"), TEXT("BP_LampPair") })
	{
		TestFalse(*FString::Printf(TEXT("'%s' is not an error"), Label), HasIssue(Validation, Label, EMantleIssueSeverity::Error));
		TestTrue(*FString::Printf(TEXT("'%s' is a warning that says why"), Label), HasIssue(Validation, Label, EMantleIssueSeverity::Warning, TEXT("attenuation radius of 0")));
	}

	// Export publishes only the light with a radius.
	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	TestFalse(TEXT("the lamp actor is not exported"), Result.Place.Entities.ContainsByPredicate([](const FMantleEntity& Candidate) { return Candidate.Name == TEXT("DeadLamp"); }));
	int32 PointLights = 0;
	for (const FMantleEntity& Entity : Result.Place.Entities)
	{
		PointLights += Entity.Type == EMantleEntityType::PointLight ? 1 : 0;
	}
	TestEqual(TEXT("one point light is exported, the lit one"), PointLights, 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
