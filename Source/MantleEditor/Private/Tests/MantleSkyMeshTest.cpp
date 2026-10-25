// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCoverage.h"
#include "MantlePlaceExporter.h"
#include "MantleSkyMesh.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// A level's own sky dome (#63) is left out of the place, as SkyLight and SkyAtmosphere are: Mantle supplies its own sky.
// The engine's sky sphere and a Blueprint with two dome meshes lose the dome parts and keep the rest of the actor; a
// Static Mesh Actor with a sky material is left out whole; and large geometry that is not a sky (a mountain, a backdrop
// that does not enclose the level) stays. Built in a transient editor world from the engine cube.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleSkyMeshTestPrivate
{
	const TCHAR* const CubePath = TEXT("/Engine/BasicShapes/Cube.Cube");

	/** An empty editor world for one test, destroyed with it. */
	struct FSkyWorld
	{
		UWorld* World = nullptr;

		FSkyWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleSkyMeshTestWorld"));
		}

		~FSkyWorld()
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

	/** A material flagged Is Sky, and/or two-sided: what a painted dome's material says about itself. */
	UMaterial* NewSkyMaterial(bool bIsSky, bool bTwoSided)
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
		Material->bIsSky = bIsSky;
		Material->TwoSided = bTwoSided;
		return Material;
	}

	/** A Static Mesh Actor of the cube, scaled so the cube (100 units) is Size units across, at Location. */
	AStaticMeshActor* SpawnCube(UWorld* World, UStaticMesh* Cube, const TCHAR* Label, const FVector& Location, double Size, UMaterialInterface* Material = nullptr)
	{
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
		if (Actor)
		{
			Actor->SetActorLabel(Label);
			Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
			Actor->SetActorScale3D(FVector(Size / 100.0));
			if (Material)
			{
				Actor->GetStaticMeshComponent()->SetMaterial(0, Material);
			}
		}
		return Actor;
	}

	/** A plain actor with a scene root, which ClassifyActor makes a Group, labelled as a sky Blueprint is. */
	AActor* SpawnGroupActor(UWorld* World, const TCHAR* Label)
	{
		AActor* Actor = World->SpawnActor<AActor>();
		if (Actor)
		{
			USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("DefaultSceneRoot"));
			Actor->SetRootComponent(Root);
			Root->RegisterComponent();
			Actor->SetActorLabel(Label);
		}
		return Actor;
	}

	UStaticMeshComponent* AddMesh(AActor* Actor, const TCHAR* Name, UStaticMesh* Mesh, double Size, UMaterialInterface* Material)
	{
		UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Actor, Name);
		Component->SetStaticMesh(Mesh);
		Component->SetupAttachment(Actor->GetRootComponent());
		Component->SetRelativeScale3D(FVector(Size / 100.0));
		if (Material)
		{
			Component->SetMaterial(0, Material);
		}
		Component->RegisterComponent();
		return Component;
	}

	int32 CountMeshParts(const TArray<FMantlePrefabPart>& Parts)
	{
		int32 Count = 0;
		for (const FMantlePrefabPart& Part : Parts)
		{
			Count += Part.Type == EMantleEntityType::StaticMesh ? 1 : 0;
		}
		return Count;
	}

	int32 CountSkyNotes(const TArray<FMantleUnpublishedPart>& Unsupported)
	{
		int32 Count = 0;
		for (const FMantleUnpublishedPart& Part : Unsupported)
		{
			Count += (Part.Category == MantleCoverageCategory::Sky && !Part.bBlocksPublish && Part.What.Contains(TEXT("Mantle supplies its own sky"))) ? 1 : 0;
		}
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSkyMeshEngineSkySphereTest,
	"Mantle.Editor.SkyMesh.EngineSkySphereMeshIsLeftOut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSkyMeshEngineSkySphereTest::RunTest(const FString& Parameters)
{
	using namespace MantleSkyMeshTestPrivate;
	UClass* SkySphere = LoadClass<AActor>(nullptr, TEXT("/Engine/EngineSky/BP_Sky_Sphere.BP_Sky_Sphere_C"));
	if (!SkySphere)
	{
		AddWarning(TEXT("the engine's BP_Sky_Sphere did not load; skipped"));
		return true;
	}
	FSkyWorld Fixture;
	AActor* Actor = Fixture.World->SpawnActor<AActor>(SkySphere);
	if (!TestNotNull(TEXT("the sky sphere spawned"), Actor))
	{
		return false;
	}
	TArray<UStaticMeshComponent*> Meshes;
	Actor->GetComponents(Meshes);
	if (Meshes.Num() == 0)
	{
		AddWarning(TEXT("this engine's BP_Sky_Sphere has no static mesh component; skipped"));
		return true;
	}
	for (const UStaticMeshComponent* Mesh : Meshes)
	{
		TestTrue(FString::Printf(TEXT("'%s' is a sky mesh by its class, whatever its size or material"), *Mesh->GetName()), FMantleSkyMesh::IsSkyMesh(Mesh));
	}

	// Left out as a part, with a note in the Sky category; nothing of the sphere is a mesh part.
	TArray<FMantlePrefabPart> Parts;
	TArray<FMantleUnpublishedPart> Unsupported;
	FMantlePlaceExporter::CollectPrefabParts(Actor, Parts, &Unsupported);
	TestEqual(TEXT("no sky mesh is a part"), CountMeshParts(Parts), 0);
	TestTrue(TEXT("the mesh is named in the Sky category"), CountSkyNotes(Unsupported) >= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSkyMeshBlueprintDomesTest,
	"Mantle.Editor.SkyMesh.BlueprintWithTwoDomesKeepsItsOtherMeshes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSkyMeshBlueprintDomesTest::RunTest(const FString& Parameters)
{
	using namespace MantleSkyMeshTestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
	if (!Cube)
	{
		AddWarning(TEXT("the engine cube did not load; skipped"));
		return true;
	}
	FSkyWorld Fixture;
	// The level: a ground slab at the origin, well inside both domes.
	SpawnCube(Fixture.World, Cube, TEXT("Ground"), FVector::ZeroVector, 5000.0);

	// BP_Embersky: a sky and a cloud dome (40000 and 25000 units across, two-sided, named like a sky), and one small prop.
	UMaterial* Painted = NewSkyMaterial(/*bIsSky*/ false, /*bTwoSided*/ true);
	AActor* Blueprint = SpawnGroupActor(Fixture.World, TEXT("BP_Embersky"));
	if (!TestNotNull(TEXT("the sky Blueprint spawned"), Blueprint))
	{
		return false;
	}
	UStaticMeshComponent* Sky = AddMesh(Blueprint, TEXT("Sky"), Cube, 40000.0, Painted);
	UStaticMeshComponent* Clouds = AddMesh(Blueprint, TEXT("Clouds"), Cube, 25000.0, Painted);
	UStaticMeshComponent* Prop = AddMesh(Blueprint, TEXT("Lantern"), Cube, 50.0, nullptr);
	TestTrue(TEXT("the sky dome is a sky mesh"), FMantleSkyMesh::IsSkyMesh(Sky));
	TestTrue(TEXT("so is the cloud dome"), FMantleSkyMesh::IsSkyMesh(Clouds));
	TestFalse(TEXT("the lantern is not"), FMantleSkyMesh::IsSkyMesh(Prop));

	TArray<FMantlePrefabPart> Parts;
	TArray<FMantleUnpublishedPart> Unsupported;
	FMantlePlaceExporter::CollectPrefabParts(Blueprint, Parts, &Unsupported);
	TestEqual(TEXT("only the lantern is a mesh part"), CountMeshParts(Parts), 1);
	TestEqual(TEXT("each dome is named in the Sky category, not as an error"), CountSkyNotes(Unsupported), 2);
	TestEqual(TEXT("and nothing else is reported"), Unsupported.Num(), 2);

	// Exported: the Group and its lantern are written; the domes are not.
	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	const FMantleEntity* Group = Result.Place.Entities.FindByPredicate([](const FMantleEntity& Entity) { return Entity.Name == TEXT("BP_Embersky"); });
	if (TestNotNull(TEXT("the Blueprint's Group is written"), Group))
	{
		int32 Children = 0;
		for (const FMantleEntity& Entity : Result.Place.Entities)
		{
			Children += Entity.Parent == Group->Id ? 1 : 0;
		}
		TestEqual(TEXT("with the lantern as its only child"), Children, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSkyMeshKeepsLargeGeometryTest,
	"Mantle.Editor.SkyMesh.LargeNonSkyMeshStaysAndSkyMaterialActorGoes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSkyMeshKeepsLargeGeometryTest::RunTest(const FString& Parameters)
{
	using namespace MantleSkyMeshTestPrivate;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
	if (!Cube)
	{
		AddWarning(TEXT("the engine cube did not load; skipped"));
		return true;
	}
	FSkyWorld Fixture;
	SpawnCube(Fixture.World, Cube, TEXT("Ground"), FVector::ZeroVector, 5000.0);

	// Large geometry that is not a sky: a mountain as big as a dome (lit, default material) around the level, and a
	// two-sided backdrop named like a sky that is off to one side and encloses nothing.
	AStaticMeshActor* Mountain = SpawnCube(Fixture.World, Cube, TEXT("Mountain"), FVector::ZeroVector, 40000.0);
	AStaticMeshActor* Backdrop = SpawnCube(Fixture.World, Cube, TEXT("Skyline Backdrop"), FVector(60000.0, 0.0, 0.0), 40000.0, NewSkyMaterial(false, /*bTwoSided*/ true));
	// A small dome with the sky flag still goes: the engine's own marker is enough.
	AStaticMeshActor* Dome = SpawnCube(Fixture.World, Cube, TEXT("PaintedDome"), FVector::ZeroVector, 45000.0, NewSkyMaterial(/*bIsSky*/ true, /*bTwoSided*/ false));
	if (!TestNotNull(TEXT("the actors spawned"), Mountain) || !TestNotNull(TEXT("all of them"), Backdrop) || !TestNotNull(TEXT("really"), Dome))
	{
		return false;
	}

	TestFalse(TEXT("a large lit mesh is not a sky"), FMantleSkyMesh::IsSkyMeshActor(Mountain));
	TestFalse(TEXT("a backdrop that does not enclose the level is not a sky"), FMantleSkyMesh::IsSkyMeshActor(Backdrop));
	TestTrue(TEXT("a mesh with an Is Sky material is"), FMantleSkyMesh::IsSkyMeshActor(Dome));
	TestFalse(TEXT("the mountain is published"), FMantlePlaceExporter::IsProvidedByMantle(Mountain));
	TestTrue(TEXT("the sky mesh is Mantle's to supply"), FMantlePlaceExporter::IsProvidedByMantle(Dome));

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	auto Has = [&Result](const TCHAR* Name)
	{
		return Result.Place.Entities.ContainsByPredicate([Name](const FMantleEntity& Entity) { return Entity.Name == Name; });
	};
	TestTrue(TEXT("the ground stays"), Has(TEXT("Ground")));
	TestTrue(TEXT("the mountain stays"), Has(TEXT("Mountain")));
	TestTrue(TEXT("the backdrop stays"), Has(TEXT("Skyline Backdrop")));
	TestFalse(TEXT("the sky dome is left out"), Has(TEXT("PaintedDome")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
