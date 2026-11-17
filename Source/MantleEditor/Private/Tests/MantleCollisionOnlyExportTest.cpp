// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCheck.h"
#include "MantleCoverage.h"
#include "MantleExportedEntities.h"
#include "MantleMirrorBake.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceValidator.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// Collision-only meshes: a static mesh hidden in game that still collides (City Sample's freeway slabs, traffic-light blockers,
// building shells) publishes as visible false, with its own collision and no materials; one hidden with no collision, or hidden
// and simulating physics, is left out. Check counts what is kept, and Gather uploads no material, Lumen or Nanite data for a mesh
// only ever used that way. Built in a transient editor world.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCollisionOnlyExportTestPrivate
{
	const TCHAR* const CubePath = TEXT("/Engine/BasicShapes/Cube.Cube");

	/** An empty editor world for one test, destroyed with it. */
	struct FCollisionWorld
	{
		UWorld* World = nullptr;

		FCollisionWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false,
				FName(*FString::Printf(TEXT("MantleCollisionOnlyTestWorld_%s"), *FGuid::NewGuid().ToString())));
		}

		~FCollisionWorld()
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

	AStaticMeshActor* SpawnMeshActor(UWorld* World, const TCHAR* Label, UStaticMesh* Mesh, const FVector& Location)
	{
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
		if (Actor)
		{
			Actor->SetActorLabel(Label);
			Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		}
		return Actor;
	}

	/** A plain Actor whose root is the component itself, as City Sample's FREEWAY_DECK_COLLISION_* actors are. */
	template <typename T>
	T* SpawnRootedOn(UWorld* World, const TCHAR* Label, const FVector& Location)
	{
		AActor* Actor = World->SpawnActor<AActor>(Location, FRotator::ZeroRotator);
		if (!Actor)
		{
			return nullptr;
		}
		T* Component = NewObject<T>(Actor, TEXT("Collision"));
		Actor->SetRootComponent(Component);
		Component->SetWorldLocation(Location);
		Component->RegisterComponent();
		Actor->SetActorLabel(Label);
		return Component;
	}

	TArray<const FMantleEntity*> ChildrenOf(const FMantleExportResult& Result, const FString& ParentId)
	{
		TArray<const FMantleEntity*> Children;
		for (const FMantleEntity* Entity : MantleExportedEntities::All(Result))
		{
			if (Entity->Parent == ParentId)
			{
				Children.Add(Entity);
			}
		}
		return Children;
	}

	bool HasIssue(const FMantleValidationResult& Result, const TCHAR* Actor, EMantleIssueSeverity Severity)
	{
		return Result.Issues.ContainsByPredicate([Actor, Severity](const FMantleValidationIssue& Issue)
		{
			return Issue.Actor == Actor && Issue.Severity == Severity;
		});
	}

	TSharedPtr<FJsonObject> PropertiesJson(const FMantleEntity& Entity)
	{
		TSharedPtr<FJsonObject> Root;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FMantlePlaceExporter::EntityToJson(Entity)), Root);
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		if (Root.IsValid())
		{
			Root->TryGetObjectField(TEXT("properties"), Properties);
		}
		return Properties ? *Properties : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCollisionOnlyExportTest,
	"Mantle.Editor.CollisionOnly.HiddenCollidingMeshesPublish",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCollisionOnlyExportTest::RunTest(const FString& Parameters)
{
	using namespace MantleCollisionOnlyExportTestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
	if (!Cube)
	{
		AddWarning(TEXT("the engine cube did not load; skipped"));
		return true;
	}
	FCollisionWorld Fixture;
	UWorld* World = Fixture.World;

	// A drawn deck with no collision over a hidden slab that collides (the City Sample freeway).
	AStaticMeshActor* Deck = SpawnMeshActor(World, TEXT("Freeway deck"), Cube, FVector(0.0, 0.0, 100.0));
	AStaticMeshActor* Slab = SpawnMeshActor(World, TEXT("Freeway slab"), Cube, FVector(0.0, 0.0, 0.0));
	if (!TestNotNull(TEXT("the deck"), Deck) || !TestNotNull(TEXT("the slab"), Slab))
	{
		return false;
	}
	Deck->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	UStaticMeshComponent* SlabMesh = Slab->GetStaticMeshComponent();
	SlabMesh->SetHiddenInGame(true);
	SlabMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	SlabMesh->SetMaterial(0, Cube->GetMaterial(0)); // an override, never written for a mesh nobody sees

	// A plain Actor holding only a hidden colliding mesh: refused as a plain Actor before, a Group of one collision-only part now.
	UStaticMeshComponent* Blocker = SpawnRootedOn<UStaticMeshComponent>(World, TEXT("FREEWAY_DECK_COLLISION_1"), FVector(500.0, 0.0, 0.0));
	// A plain Actor holding hidden colliding instances (a building's simple shells).
	UInstancedStaticMeshComponent* Shells = SpawnRootedOn<UInstancedStaticMeshComponent>(World, TEXT("Buildings_Collision"), FVector(1000.0, 0.0, 0.0));
	if (!TestNotNull(TEXT("the blocker"), Blocker) || !TestNotNull(TEXT("the shells"), Shells))
	{
		return false;
	}
	Blocker->SetStaticMesh(Cube);
	Blocker->SetVisibility(false);
	Blocker->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Shells->SetStaticMesh(Cube);
	Shells->SetHiddenInGame(true);
	Shells->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Shells->AddInstance(FTransform(FVector(Index * 300.0, 0.0, 0.0)));
	}

	// Hidden with no collision: nothing to see or touch. Hidden and simulating physics: a prop no one could see.
	AStaticMeshActor* Nothing = SpawnMeshActor(World, TEXT("Hidden marker"), Cube, FVector(2000.0, 0.0, 0.0));
	AStaticMeshActor* Ghost = SpawnMeshActor(World, TEXT("Ghost crate"), Cube, FVector(2500.0, 0.0, 0.0));
	if (!TestNotNull(TEXT("the marker"), Nothing) || !TestNotNull(TEXT("the crate"), Ghost))
	{
		return false;
	}
	Nothing->GetStaticMeshComponent()->SetHiddenInGame(true);
	Nothing->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Ghost->GetStaticMeshComponent()->SetHiddenInGame(true);
	Ghost->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Ghost->GetStaticMeshComponent()->BodyInstance.bSimulatePhysics = true;

	// The actor's own Hidden In Game, its component left as it is.
	AStaticMeshActor* ActorHidden = SpawnMeshActor(World, TEXT("Ground collision"), Cube, FVector(3000.0, 0.0, 0.0));
	// An invisible wall: Unreal's InvisibleWall preset blocks everything but the Visibility channel.
	AStaticMeshActor* Wall = SpawnMeshActor(World, TEXT("Invisible wall"), Cube, FVector(3500.0, 0.0, 0.0));
	if (!TestNotNull(TEXT("the ground"), ActorHidden) || !TestNotNull(TEXT("the wall"), Wall))
	{
		return false;
	}
	ActorHidden->SetActorHiddenInGame(true);
	ActorHidden->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Wall->GetStaticMeshComponent()->SetHiddenInGame(true);
	Wall->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("InvisibleWall"));

	TestFalse(TEXT("an actor hidden in game is not shown"), FMantlePlaceExporter::IsShownInGame(ActorHidden->GetStaticMeshComponent()));
	TestTrue(TEXT("and its colliding mesh is collision only"), FMantlePlaceExporter::IsCollisionOnly(ActorHidden->GetStaticMeshComponent()));
	TestFalse(TEXT("an invisible wall lets sight traces through"), FMantlePlaceExporter::BlocksVisibility(Wall->GetStaticMeshComponent()));
	TestTrue(TEXT("the slab is collision only"), FMantlePlaceExporter::IsCollisionOnly(SlabMesh));
	TestTrue(TEXT("so is the blocker (not visible)"), FMantlePlaceExporter::IsCollisionOnly(Blocker));
	TestTrue(TEXT("and the shells"), FMantlePlaceExporter::IsCollisionOnly(Shells));
	TestFalse(TEXT("the drawn deck is not"), FMantlePlaceExporter::IsCollisionOnly(Deck->GetStaticMeshComponent()));
	TestFalse(TEXT("a hidden mesh with no collision is not"), FMantlePlaceExporter::IsCollisionOnly(Nothing->GetStaticMeshComponent()));
	TestFalse(TEXT("a hidden physics prop is not"), FMantlePlaceExporter::IsCollisionOnly(Ghost->GetStaticMeshComponent()));

	const FMantleExportResult Result = FMantlePlaceExporter::Export(World, FMantleExportOptions());

	// The slab: solid, never drawn, its own collision, none of its materials and no shadow.
	const FMantleEntity* SlabEntity = MantleExportedEntities::Named(Result, TEXT("Freeway slab"));
	if (TestNotNull(TEXT("the slab is published"), SlabEntity))
	{
		const FMantleStaticMeshProps* Props = MantleMeshPropsOf(*SlabEntity);
		if (TestNotNull(TEXT("as a mesh"), Props))
		{
			TestFalse(TEXT("collision only"), Props->bVisible);
			TestTrue(TEXT("blocking"), Props->Collision == EMantleCollision::Blocking);
			TestEqual(TEXT("with no materials"), Props->Materials.Num(), 0);
			TestFalse(TEXT("and no shadow"), Props->bCastShadow);
			TestFalse(TEXT("on a mesh"), Props->Mesh.IsEmpty());
		}
	}
	const FMantleEntity* DeckEntity = MantleExportedEntities::Named(Result, TEXT("Freeway deck"));
	if (TestNotNull(TEXT("the deck is published"), DeckEntity) && MantleMeshPropsOf(*DeckEntity))
	{
		TestTrue(TEXT("drawn"), MantleMeshPropsOf(*DeckEntity)->bVisible);
	}

	// The blocker actor: a Group whose one part is collision only, and no refusal.
	const FMantleEntity* BlockerGroup = MantleExportedEntities::Named(Result, TEXT("FREEWAY_DECK_COLLISION_1"));
	if (TestNotNull(TEXT("the blocker actor is published"), BlockerGroup))
	{
		TestTrue(TEXT("as a Group"), BlockerGroup->Type == EMantleEntityType::Group);
		const TArray<const FMantleEntity*> Parts = ChildrenOf(Result, BlockerGroup->Id);
		if (TestEqual(TEXT("of one part"), Parts.Num(), 1))
		{
			const FMantleStaticMeshProps* Props = MantleMeshPropsOf(*Parts[0]);
			TestTrue(TEXT("a collision-only mesh"), Props && !Props->bVisible && Props->Collision == EMantleCollision::Blocking);
		}
	}
	TestFalse(TEXT("no error names the blocker"), Result.Errors.ContainsByPredicate([](const FMantleParseError& Error)
	{
		return Error.Message.Contains(TEXT("FREEWAY_DECK_COLLISION_1"));
	}));

	// The shells: instance sets never drawn, every instance kept.
	int32 HiddenInstances = 0;
	for (const FMantleEntity* Entity : MantleExportedEntities::All(Result))
	{
		const FMantleInstancedMeshProps* Instanced = Entity->Props.TryGet<FMantleInstancedMeshProps>();
		if (Instanced && !Instanced->Mesh.bVisible)
		{
			HiddenInstances += Instanced->Instances.Num();
			TestEqual(TEXT("a hidden set has no materials"), Instanced->Mesh.Materials.Num(), 0);
		}
	}
	TestEqual(TEXT("the three hidden instances publish as collision only"), HiddenInstances, 3);

	// The actor hidden in game: collision only, drawn nowhere.
	const FMantleEntity* GroundEntity = MantleExportedEntities::Named(Result, TEXT("Ground collision"));
	if (TestNotNull(TEXT("the actor hidden in game is published"), GroundEntity) && MantleMeshPropsOf(*GroundEntity))
	{
		TestFalse(TEXT("collision only"), MantleMeshPropsOf(*GroundEntity)->bVisible);
		TestEqual(TEXT("with no materials"), MantleMeshPropsOf(*GroundEntity)->Materials.Num(), 0);
		TestTrue(TEXT("blocking sight traces"), MantleMeshPropsOf(*GroundEntity)->bBlocksVisibility);
	}
	// The invisible wall: collision only, and it lets sight traces through.
	const FMantleEntity* WallEntity = MantleExportedEntities::Named(Result, TEXT("Invisible wall"));
	if (TestNotNull(TEXT("the invisible wall is published"), WallEntity) && MantleMeshPropsOf(*WallEntity))
	{
		TestFalse(TEXT("collision only"), MantleMeshPropsOf(*WallEntity)->bVisible);
		TestTrue(TEXT("blocking"), MantleMeshPropsOf(*WallEntity)->Collision == EMantleCollision::Blocking);
		TestFalse(TEXT("but not sight traces"), MantleMeshPropsOf(*WallEntity)->bBlocksVisibility);
	}

	// Left out: the marker and the crate.
	TestNull(TEXT("a hidden mesh with no collision is left out"), MantleExportedEntities::Named(Result, TEXT("Hidden marker")));
	TestNull(TEXT("a hidden physics prop is left out"), MantleExportedEntities::Named(Result, TEXT("Ghost crate")));

	// Counted, and said in the coverage.
	// The slab, the blocker's part, the shells' one instance set, the actor hidden in game and the wall.
	TestEqual(TEXT("every collision-only entity is counted"), Result.CollisionOnlyEntities, 5);
	const FMantleCoverageCategory* Meshes = Result.Coverage.Find(MantleCoverageCategory::StaticMeshes);
	TestTrue(TEXT("the coverage keeps them"), Meshes && Meshes->Get(EMantleCoverageOutcome::Kept).Examples.ContainsByPredicate([](const FMantleCoverageExample& Example)
	{
		return Example.Name == TEXT("collision-only meshes");
	}));

	// Check agrees: no refusal for the blocker, a warning for the invisible physics prop, none for the plain hidden marker.
	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(World);
	TestFalse(TEXT("Check refuses no collision-only actor"), HasIssue(Validation, TEXT("FREEWAY_DECK_COLLISION_1"), EMantleIssueSeverity::Error));
	TestFalse(TEXT("nor the slab"), HasIssue(Validation, TEXT("Freeway slab"), EMantleIssueSeverity::Error));
	TestTrue(TEXT("it warns about the invisible physics prop"), HasIssue(Validation, TEXT("Ghost crate"), EMantleIssueSeverity::Warning));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCollisionOnlyWrittenTest,
	"Mantle.Editor.CollisionOnly.WrittenKeyedAndCounted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCollisionOnlyWrittenTest::RunTest(const FString& Parameters)
{
	using namespace MantleCollisionOnlyExportTestPrivate;
	FMantleStaticMeshProps Drawn;
	Drawn.Mesh = TEXT("ast_cube");
	FMantleStaticMeshProps Hidden = Drawn;
	Hidden.bVisible = false;
	Hidden.bCastShadow = false;

	FMantleEntity Entity;
	Entity.Id = TEXT("ent_slab");
	Entity.Type = EMantleEntityType::StaticMesh;
	Entity.Props.Set<FMantleStaticMeshProps>(Hidden);
	const TSharedPtr<FJsonObject> HiddenJson = PropertiesJson(Entity);
	if (TestNotNull(TEXT("the hidden mesh's properties"), HiddenJson.Get()))
	{
		bool bVisible = true;
		TestTrue(TEXT("visible is written"), HiddenJson->TryGetBoolField(TEXT("visible"), bVisible));
		TestFalse(TEXT("as false"), bVisible);
	}
	if (HiddenJson.IsValid())
	{
		TestFalse(TEXT("a wall that blocks sight writes no blocksVisibility"), HiddenJson->HasField(TEXT("blocksVisibility")));
	}
	FMantleStaticMeshProps Wall = Hidden;
	Wall.bBlocksVisibility = false;
	Entity.Props.Set<FMantleStaticMeshProps>(Wall);
	const TSharedPtr<FJsonObject> WallJson = PropertiesJson(Entity);
	if (TestNotNull(TEXT("the invisible wall's properties"), WallJson.Get()))
	{
		bool bBlocks = true;
		TestTrue(TEXT("blocksVisibility is written"), WallJson->TryGetBoolField(TEXT("blocksVisibility"), bBlocks));
		TestFalse(TEXT("as false"), bBlocks);
	}
	TestNotEqual(TEXT("a wall and a slab are different sets"), FMantlePlaceExporter::InstancedGroupKey(Wall), FMantlePlaceExporter::InstancedGroupKey(Hidden));
	Entity.Props.Set<FMantleStaticMeshProps>(Drawn);
	const TSharedPtr<FJsonObject> DrawnJson = PropertiesJson(Entity);
	if (TestNotNull(TEXT("the drawn mesh's properties"), DrawnJson.Get()))
	{
		TestFalse(TEXT("a drawn mesh writes no visible key"), DrawnJson->HasField(TEXT("visible")));
		TestFalse(TEXT("nor blocksVisibility"), DrawnJson->HasField(TEXT("blocksVisibility")));
	}

	// A collision-only set never joins a drawn one, and a drawn key stays as it was.
	TestNotEqual(TEXT("hidden and drawn are different sets"), FMantlePlaceExporter::InstancedGroupKey(Hidden), FMantlePlaceExporter::InstancedGroupKey(Drawn));
	TestFalse(TEXT("a drawn key is unchanged"), FMantlePlaceExporter::InstancedGroupKey(Drawn).Contains(TEXT("hidden")));

	// Mantle.Check's last line carries the count.
	FMantleCheckReport Report;
	Report.CollisionOnly = 7;
	TestTrue(TEXT("Mantle.Check reports it"), FMantleCheck::DoneLine(Report).EndsWith(TEXT(" collisionOnly=7")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCollisionOnlyGatherTest,
	"Mantle.Editor.CollisionOnly.GatherBakesNothingForAMeshNeverDrawn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCollisionOnlyGatherTest::RunTest(const FString& Parameters)
{
	using namespace MantleCollisionOnlyExportTestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
	if (!Cube)
	{
		AddWarning(TEXT("the engine cube did not load; skipped"));
		return true;
	}
	// A mirrored cube is a project mesh (it uploads); the library cube itself would not.
	const FVector Mirror(-1.0, 1.0, 1.0);
	FVector PositiveScale;
	FString Why;
	UStaticMesh* Copy = FMantleMirrorBake::MirroredCopy(Cube, Mirror, PositiveScale, Why);
	if (!TestNotNull(TEXT("the mirrored copy"), Copy))
	{
		return false;
	}

	FCollisionWorld Fixture;
	AStaticMeshActor* Slab = SpawnMeshActor(Fixture.World, TEXT("Mirrored slab"), Cube, FVector::ZeroVector);
	if (!TestNotNull(TEXT("the slab"), Slab))
	{
		return false;
	}
	Slab->SetActorScale3D(Mirror);
	Slab->GetStaticMeshComponent()->SetHiddenInGame(true);
	Slab->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);

	const auto GatheredCopy = [Copy](const FMantlePlaceDependencies& Dependencies)
	{
		return Dependencies.Meshes.FindByPredicate([Copy](const FMantleGatheredMesh& Mesh) { return Mesh.Mesh.Get() == Copy; });
	};
	FMantlePlaceDependencies Only = FMantlePlaceDependencies::Gather(Fixture.World);
	const FMantleGatheredMesh* Hidden = GatheredCopy(Only);
	if (TestNotNull(TEXT("a collision-only mesh still uploads"), Hidden))
	{
		TestTrue(TEXT("marked collision only: no Lumen or Nanite bake"), Hidden->bCollisionOnly);
	}
	const int32 MaterialsWhenHidden = Only.Materials.Num();

	// The hidden use came first; a drawn use through AddMesh (a Script Asset, an item or weapon mesh, a water body) comes later and
	// still gives the mesh its render data, and its materials.
	Only.AddMesh(Copy, TEXT("Crate script asset"));
	const FMantleGatheredMesh* Spawned = GatheredCopy(Only);
	if (TestNotNull(TEXT("still one entry for the mesh"), Spawned))
	{
		TestFalse(TEXT("a later drawn AddMesh use clears collision only"), Spawned->bCollisionOnly);
	}
	TestTrue(TEXT("and gathers the mesh's materials"), Only.Materials.Num() >= MaterialsWhenHidden);
	// And a collision-only AddMesh (a hidden spline bake) never makes a drawn mesh collision only.
	Only.AddMesh(Copy, TEXT("Hidden road collision"), /*bCollisionOnly*/ true);
	const FMantleGatheredMesh* Again = GatheredCopy(Only);
	TestTrue(TEXT("a later collision-only use leaves it drawn"), Again && !Again->bCollisionOnly);

	// The same mesh drawn somewhere else needs its render data after all.
	AStaticMeshActor* Shown = SpawnMeshActor(Fixture.World, TEXT("Mirrored wall"), Cube, FVector(500.0, 0.0, 0.0));
	if (!TestNotNull(TEXT("the wall"), Shown))
	{
		return false;
	}
	Shown->SetActorScale3D(Mirror);
	const FMantlePlaceDependencies Both = FMantlePlaceDependencies::Gather(Fixture.World);
	const FMantleGatheredMesh* Drawn = GatheredCopy(Both);
	if (TestNotNull(TEXT("the mesh is gathered once"), Drawn))
	{
		TestFalse(TEXT("and is no longer collision only"), Drawn->bCollisionOnly);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
