// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCableBake.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceValidator.h"

#include "CableActor.h"
#include "CableComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// Cables become baked static meshes at export: the tube the Cable Component draws, along the points it has in the
// editor, merged by material, in world space relative to a pivot, with no collision (the component has none in a game).

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCableBakeTestPrivate
{
	/** A transient editor world for one test, destroyed with it. Cleans the bake cache up. */
	struct FCableWorld
	{
		UWorld* World = nullptr;

		FCableWorld()
		{
			FMantleCableBake::ClearCache();
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleCableBakeTestWorld"));
		}

		~FCableWorld()
		{
			FMantleCableBake::ClearCache();
			if (World)
			{
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
				if (World->IsRooted())
				{
					World->RemoveFromRoot();
				}
			}
		}

		/** A Cable Actor at Location whose cable runs Span from there (its points are straight: nothing ticks in a test), with Material if given. */
		ACableActor* AddCable(const FVector& Location, const FVector& Span, UMaterialInterface* Material = nullptr) const
		{
			FActorSpawnParameters Parameters;
			Parameters.bDeferConstruction = true;
			ACableActor* Actor = World->SpawnActor<ACableActor>(ACableActor::StaticClass(), FTransform(Location), Parameters);
			if (!Actor)
			{
				return nullptr;
			}
			Actor->CableComponent->EndLocation = Span;
			Actor->CableComponent->NumSegments = 4;
			if (Material)
			{
				Actor->CableComponent->SetMaterial(0, Material);
			}
			Actor->FinishSpawning(FTransform(Location));
			return Actor;
		}
	};

	TArray<UCableComponent*> ComponentsOf(const TArray<ACableActor*>& Actors)
	{
		TArray<UCableComponent*> Components;
		for (const ACableActor* Actor : Actors)
		{
			Components.Add(Actor->CableComponent);
		}
		return Components;
	}

	FBox WorldBoundsOf(const FMantleBakedSplineMesh& Baked)
	{
		return Baked.Mesh->GetBounds().GetBox().ShiftBy(Baked.Transform.GetLocation());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCableBakeBoundsTest,
	"Mantle.Editor.CableBake.BoundsMatchTheCable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCableBakeBoundsTest::RunTest(const FString& Parameters)
{
	using namespace MantleCableBakeTestPrivate;
	FCableWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}

	// A 4 m cable from (1000, 2000, 300), 10 units wide: a tube 4 m long and 10 units across.
	ACableActor* Actor = Fixture.AddCable(FVector(1000.0, 2000.0, 300.0), FVector(400.0, 0.0, 0.0));
	if (!TestNotNull(TEXT("the cable actor"), Actor))
	{
		return false;
	}
	UCableComponent* Cable = Actor->CableComponent;
	Cable->CableWidth = 10.0f;
	TestTrue(TEXT("a visible cable is a bake source"), FMantleCableBake::IsBakeSource(Cable));

	const FMantleSplineBakeResult Result = FMantleCableBake::BakeComponents({ Cable }, FMantlePlaceAssetLimits(), nullptr);
	for (const FMantleSplineBakeProblem& Problem : Result.Problems)
	{
		AddError(FString::Printf(TEXT("'%s' %s %s"), *Problem.OwnerLabel, *Problem.What, *Problem.Remedy));
	}
	if (!TestEqual(TEXT("one cable bakes into one mesh"), Result.Meshes.Num(), 1))
	{
		return false;
	}
	const FMantleBakedSplineMesh& Baked = Result.Meshes[0];
	if (!TestNotNull(TEXT("the baked mesh"), Baked.Mesh))
	{
		return false;
	}

	// Not in the creator's Content, and nothing a player collides with.
	TestTrue(TEXT("the baked mesh is transient"), Baked.Mesh->GetOutermost() == GetTransientPackage());
	TestEqual(TEXT("it has no collision"), Baked.Collision, EMantleCollision::None);

	const FBox Merged = WorldBoundsOf(Baked);
	const FBox Expected(FVector(1000.0, 1995.0, 295.0), FVector(1400.0, 2005.0, 305.0));
	TestTrue(FString::Printf(TEXT("the baked bounds are the tube's (baked %s, expected %s)"), *Merged.ToString(), *Expected.ToString()),
		Merged.Min.Equals(Expected.Min, 1.0) && Merged.Max.Equals(Expected.Max, 1.0));

	// Five points (four segments) of a ring of NumSides + 1 vertices, and two triangles a side a segment.
	const int32 Sides = Cable->NumSides;
	TestEqual(TEXT("the tube's vertices"), Baked.Vertices, 5 * (Sides + 1));
	TestEqual(TEXT("and its triangles"), Baked.Triangles, 4 * Sides * 2);
	TestEqual(TEXT("it counts its source cable"), Baked.SourceComponents, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCableBakeMergeTest,
	"Mantle.Editor.CableBake.MergesByMaterial",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCableBakeMergeTest::RunTest(const FString& Parameters)
{
	using namespace MantleCableBakeTestPrivate;
	FCableWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}

	UMaterial* Black = NewObject<UMaterial>(GetTransientPackage());
	TArray<ACableActor*> Actors;
	Actors.Add(Fixture.AddCable(FVector(0.0, 0.0, 300.0), FVector(300.0, 0.0, 0.0), Black));
	Actors.Add(Fixture.AddCable(FVector(0.0, 100.0, 300.0), FVector(300.0, 0.0, 0.0), Black));
	Actors.Add(Fixture.AddCable(FVector(0.0, 200.0, 300.0), FVector(300.0, 0.0, 0.0)));
	if (!TestFalse(TEXT("the cables spawned"), Actors.Contains(nullptr)))
	{
		return false;
	}

	const FMantleSplineBakeResult Result = FMantleCableBake::BakeComponents(ComponentsOf(Actors), FMantlePlaceAssetLimits(), nullptr);
	if (!TestEqual(TEXT("two materials bake into two meshes"), Result.Meshes.Num(), 2))
	{
		return false;
	}
	int32 Components = 0;
	for (const FMantleBakedSplineMesh& Baked : Result.Meshes)
	{
		Components += Baked.SourceComponents;
		TestEqual(TEXT("each mesh has the one material slot"), FMantlePlaceAssets::UploadSlots(Baked.Mesh).Num(), 1);
	}
	TestEqual(TEXT("every cable is in a mesh"), Components, 3);
	const FMantleBakedSplineMesh* Merged = Result.Meshes.FindByPredicate([](const FMantleBakedSplineMesh& Baked) { return Baked.SourceComponents == 2; });
	if (TestNotNull(TEXT("the two cables of one material share a mesh"), Merged))
	{
		TestTrue(TEXT("drawn with that material"), FMantlePlaceAssets::MeshSlotMaterial(Merged->Mesh, 0) == Black);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCableBakeCacheTest,
	"Mantle.Editor.CableBake.IsCachedWhileTheCablesAreUnchanged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCableBakeCacheTest::RunTest(const FString& Parameters)
{
	using namespace MantleCableBakeTestPrivate;
	FCableWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	if (!TestNotNull(TEXT("the cable"), Fixture.AddCable(FVector(0.0, 0.0, 300.0), FVector(300.0, 0.0, 0.0))))
	{
		return false;
	}

	// Gather, Check and Export all bake the level: they must see one mesh, so one mesh key.
	const FMantleSplineBakeResult First = FMantleCableBake::Bake(Fixture.World, FMantlePlaceAssetLimits());
	const FMantleSplineBakeResult Second = FMantleCableBake::Bake(Fixture.World, FMantlePlaceAssetLimits());
	if (!TestEqual(TEXT("both bake one mesh"), First.Meshes.Num() + Second.Meshes.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("a second bake of unchanged cables returns the same mesh"), First.Meshes[0].Mesh == Second.Meshes[0].Mesh);
	TestEqual(TEXT("and so the same mesh key"), FMantlePlaceAssets::MeshKey(First.Meshes[0].Mesh), FMantlePlaceAssets::MeshKey(Second.Meshes[0].Mesh));
	TestEqual(TEXT("and the same entity id seed"), First.Meshes[0].IdSeed, Second.Meshes[0].IdSeed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCableBakeHiddenTest,
	"Mantle.Editor.CableBake.OnlyACablePlayersCanSeeIsBaked",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCableBakeHiddenTest::RunTest(const FString& Parameters)
{
	using namespace MantleCableBakeTestPrivate;
	FCableWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	ACableActor* Actor = Fixture.AddCable(FVector(0.0, 0.0, 300.0), FVector(300.0, 0.0, 0.0));
	if (!TestNotNull(TEXT("the cable"), Actor))
	{
		return false;
	}
	Actor->CableComponent->SetHiddenInGame(true);
	TestFalse(TEXT("a cable hidden in game is not a source"), FMantleCableBake::IsBakeSource(Actor->CableComponent));
	TestEqual(TEXT("and bakes nothing"), FMantleCableBake::Bake(Fixture.World, FMantlePlaceAssetLimits()).Meshes.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCableBakeExportTest,
	"Mantle.Editor.CableBake.ACableActorPublishesAsAStaticMesh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCableBakeExportTest::RunTest(const FString& Parameters)
{
	using namespace MantleCableBakeTestPrivate;
	FCableWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	ACableActor* Actor = Fixture.AddCable(FVector(0.0, 0.0, 300.0), FVector(300.0, 0.0, 0.0));
	if (!TestNotNull(TEXT("the cable"), Actor))
	{
		return false;
	}
	Actor->SetActorLabel(TEXT("Hanging wire"));

	// Not refused as an unsupported actor, by Export or by Check.
	TestTrue(TEXT("it holds only what the cable bake publishes"), FMantlePlaceExporter::IsInstanceHost(Actor));
	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	const bool bRefused = Result.Errors.ContainsByPredicate([&Actor](const FMantleParseError& Error)
	{
		return Error.Path.Contains(Actor->GetName()) && Error.Message.Contains(TEXT("cannot publish"));
	});
	TestFalse(TEXT("export does not refuse it"), bRefused);
	int32 StaticMeshes = 0;
	for (const FMantleEntity& Entity : Result.Place.Entities)
	{
		StaticMeshes += Entity.Type == EMantleEntityType::StaticMesh ? 1 : 0;
	}
	TestEqual(TEXT("it is written as one static mesh"), StaticMeshes, 1);

	const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(Fixture.World);
	TestFalse(TEXT("Check raises no error for it"), Validation.Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue)
	{
		return Issue.Actor == TEXT("Hanging wire") && Issue.Severity == EMantleIssueSeverity::Error;
	}));

	// Gather uploads the baked mesh.
	const FMantlePlaceDependencies Dependencies = FMantlePlaceDependencies::Gather(Fixture.World);
	TestTrue(TEXT("the baked mesh is gathered"), Dependencies.Meshes.ContainsByPredicate([](const FMantleGatheredMesh& Gathered)
	{
		return Gathered.UsedBy.Contains(TEXT("Cables"));
	}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
