// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssets.h"
#include "MantleSplineMeshBake.h"

#include "Components/SplineMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AutomationTest.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// Spline meshes become baked static meshes at export: the deformed geometry of USplineMeshComponent, merged by
// owner and material set, in world space relative to a pivot, within the place mesh limits.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleSplineMeshBakeTestPrivate
{
	const TCHAR* const CubePath = TEXT("/Engine/BasicShapes/Cube.Cube");

	/** A transient world with one actor to own spline meshes built over an engine cube. Cleans the bake cache up. */
	struct FSplineFixture
	{
		UWorld* World = nullptr;
		AActor* Owner = nullptr;
		UStaticMesh* Cube = nullptr;

		FSplineFixture()
		{
			FMantleSplineMeshBake::ClearCache();
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false);
			if (World)
			{
				FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
				Context.SetCurrentWorld(World);
				Owner = World->SpawnActor<AActor>();
			}
			Cube = LoadObject<UStaticMesh>(nullptr, CubePath);
		}

		~FSplineFixture()
		{
			FMantleSplineMeshBake::ClearCache();
			if (World)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(false);
			}
		}

		bool IsReady() const { return World && Owner && Cube && Cube->GetRenderData() && Cube->GetRenderData()->LODResources.Num() > 0; }

		/** A straight spline mesh over the cube from Location to Location + Span (the cube's X axis runs along the spline), not registered. */
		USplineMeshComponent* AddSpline(const FVector& Location, const FVector& Span, UMaterialInterface* Material = nullptr) const
		{
			USplineMeshComponent* Component = NewObject<USplineMeshComponent>(Owner);
			Component->SetStaticMesh(Cube);
			Component->SetForwardAxis(ESplineMeshAxis::X, false);
			Component->SetStartAndEnd(FVector::ZeroVector, Span, Span, Span, false);
			Component->SetStartScale(FVector2D(1.0, 1.0), false);
			Component->SetEndScale(FVector2D(1.0, 1.0), false);
			Component->SetWorldLocation(Location);
			// A component made in code has no collision until it is given some (a landscape's pieces set theirs); a road you walk on blocks.
			Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			if (Material)
			{
				Component->SetMaterial(0, Material);
			}
			return Component;
		}

		int32 CubeVertices() const { return Cube->GetRenderData()->LODResources[0].GetNumVertices(); }
		int32 CubeTriangles() const { return Cube->GetRenderData()->LODResources[0].GetNumTriangles(); }
	};

	/** Bakes just these components, as Export bakes a level's. */
	FMantleSplineBakeResult BakeAll(const TArray<USplineMeshComponent*>& Components, const FMantlePlaceAssetLimits& Limits)
	{
		return FMantleSplineMeshBake::BakeComponents(Components, Limits, nullptr);
	}

	FBox WorldBoundsOf(const FMantleBakedSplineMesh& Baked)
	{
		return Baked.Mesh->GetBounds().GetBox().ShiftBy(Baked.Transform.GetLocation());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSplineMeshBakeBoundsTest,
	"Mantle.Editor.SplineMeshBake.BoundsMatchTheComponent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSplineMeshBakeBoundsTest::RunTest(const FString& Parameters)
{
	using namespace MantleSplineMeshBakeTestPrivate;
	FSplineFixture Fixture;
	if (!TestTrue(TEXT("the fixture builds (a world, an actor and the engine cube)"), Fixture.IsReady()))
	{
		return false;
	}

	// A 4 m straight spline from (1000, 2000, 300), over a 1 m cube: it comes out 4 m long and 1 m square.
	USplineMeshComponent* Component = Fixture.AddSpline(FVector(1000.0, 2000.0, 300.0), FVector(400.0, 0.0, 0.0));
	TestTrue(TEXT("a visible spline mesh with a mesh is a bake source"), FMantleSplineMeshBake::IsBakeSource(Component));

	const FMantleSplineBakeResult Result = BakeAll({ Component }, FMantlePlaceAssetLimits());
	for (const FMantleSplineBakeProblem& Problem : Result.Problems)
	{
		AddError(FString::Printf(TEXT("'%s' %s %s"), *Problem.OwnerLabel, *Problem.What, *Problem.Remedy));
	}
	if (!TestEqual(TEXT("one spline mesh bakes into one mesh"), Result.Meshes.Num(), 1))
	{
		return false;
	}
	const FMantleBakedSplineMesh& Baked = Result.Meshes[0];
	if (!TestNotNull(TEXT("the baked mesh"), Baked.Mesh))
	{
		return false;
	}

	// Not in the creator's Content.
	TestTrue(TEXT("the baked mesh is transient"), Baked.Mesh->GetOutermost() == GetTransientPackage());

	const FBox Source = Component->CalcBounds(Component->GetComponentTransform()).GetBox();
	const FBox Merged = WorldBoundsOf(Baked);
	const FVector Slack = Source.GetSize() * 0.02 + FVector(1.0);
	TestTrue(FString::Printf(TEXT("the baked bounds lie inside the component's (baked %s, component %s)"), *Merged.ToString(), *Source.ToString()),
		Source.ExpandBy(1.0).IsInsideOrOn(Merged));
	TestTrue(FString::Printf(TEXT("and fill them (baked %s, component %s)"), *Merged.ToString(), *Source.ToString()),
		Merged.ExpandBy(Slack).IsInsideOrOn(Source));

	// The straight spline's own geometry: x from 1000 to 1400, 1 m square about (2000, 300).
	const FBox Expected(FVector(1000.0, 1950.0, 250.0), FVector(1400.0, 2050.0, 350.0));
	TestTrue(FString::Printf(TEXT("the baked bounds are the deformed cube's (baked %s, expected %s)"), *Merged.ToString(), *Expected.ToString()),
		Merged.Min.Equals(Expected.Min, 1.0) && Merged.Max.Equals(Expected.Max, 1.0));

	// Welded, deformation moves vertices but doesn't add or remove any.
	TestEqual(TEXT("the baked mesh has the cube's vertices"), Baked.Vertices, Fixture.CubeVertices());
	TestEqual(TEXT("and the cube's triangles"), Baked.Triangles, Fixture.CubeTriangles());
	TestEqual(TEXT("it counts its source component"), Baked.SourceComponents, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSplineMeshBakeMergeTest,
	"Mantle.Editor.SplineMeshBake.MergesByOwnerAndMaterialSet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSplineMeshBakeMergeTest::RunTest(const FString& Parameters)
{
	using namespace MantleSplineMeshBakeTestPrivate;
	FSplineFixture Fixture;
	if (!TestTrue(TEXT("the fixture builds"), Fixture.IsReady()))
	{
		return false;
	}
	UMaterialInterface* Grid = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial"));
	UMaterialInterface* Basic = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (!TestTrue(TEXT("two engine materials load"), Grid && Basic && Grid != Basic))
	{
		return false;
	}

	// Two pieces with one material merge into one mesh holding both; a third with another material is its own.
	USplineMeshComponent* A = Fixture.AddSpline(FVector(0.0, 0.0, 0.0), FVector(400.0, 0.0, 0.0), Grid);
	USplineMeshComponent* B = Fixture.AddSpline(FVector(500.0, 0.0, 0.0), FVector(0.0, 400.0, 0.0), Grid);
	USplineMeshComponent* C = Fixture.AddSpline(FVector(0.0, 500.0, 0.0), FVector(400.0, 0.0, 0.0), Basic);
	const FMantleSplineBakeResult Result = BakeAll({ A, B, C }, FMantlePlaceAssetLimits());
	for (const FMantleSplineBakeProblem& Problem : Result.Problems)
	{
		AddError(FString::Printf(TEXT("'%s' %s %s"), *Problem.OwnerLabel, *Problem.What, *Problem.Remedy));
	}
	if (!TestEqual(TEXT("two material sets bake into two meshes"), Result.Meshes.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("one report for the one owner"), Result.Reports.Num(), 1);
	if (Result.Reports.Num() == 1)
	{
		TestEqual(TEXT("it counts all three components"), Result.Reports[0].Components, 3);
		TestEqual(TEXT("and both meshes"), Result.Reports[0].Meshes, 2);
		TestTrue(TEXT("and reads as a sentence"), Result.Reports[0].ToString().Contains(TEXT("3 spline meshes on")));
	}

	const FMantleBakedSplineMesh* Merged = Result.Meshes.FindByPredicate([](const FMantleBakedSplineMesh& Mesh) { return Mesh.SourceComponents == 2; });
	const FMantleBakedSplineMesh* Alone = Result.Meshes.FindByPredicate([](const FMantleBakedSplineMesh& Mesh) { return Mesh.SourceComponents == 1; });
	if (!TestTrue(TEXT("one mesh holds two pieces and one holds one"), Merged && Alone))
	{
		return false;
	}
	TestEqual(TEXT("the merged mesh has both pieces' vertices"), Merged->Vertices, 2 * Fixture.CubeVertices());
	TestEqual(TEXT("and both pieces' triangles"), Merged->Triangles, 2 * Fixture.CubeTriangles());

	// Each baked mesh draws with the material its pieces drew with, and collides with its triangles.
	TestTrue(TEXT("the merged mesh keeps its material"), FMantlePlaceAssets::MeshSlotMaterial(Merged->Mesh, FMantlePlaceAssets::UploadSlots(Merged->Mesh)[0]) == Grid);
	TestTrue(TEXT("the other keeps its own"), FMantlePlaceAssets::MeshSlotMaterial(Alone->Mesh, FMantlePlaceAssets::UploadSlots(Alone->Mesh)[0]) == Basic);
	TestTrue(TEXT("a colliding spline mesh bakes to a blocking mesh"), Merged->Collision == EMantleCollision::Blocking);
	if (const UBodySetup* Body = Merged->Mesh->GetBodySetup())
	{
		TestTrue(TEXT("collision is the triangles"), Body->GetCollisionTraceFlag() == CTF_UseComplexAsSimple);
	}
	else
	{
		AddError(TEXT("the baked mesh has a body setup"));
	}

	// The pivot is where the merged mesh sits: its bounds relative to it are centred on it (to the cm it is rounded to).
	const FBox Box = WorldBoundsOf(*Merged);
	TestTrue(TEXT("the pivot is the merged bounds' centre"), Box.GetCenter().Equals(Merged->Transform.GetLocation(), 1.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSplineMeshBakeLimitsTest,
	"Mantle.Editor.SplineMeshBake.StaysWithinTheMeshLimits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSplineMeshBakeLimitsTest::RunTest(const FString& Parameters)
{
	using namespace MantleSplineMeshBakeTestPrivate;
	FSplineFixture Fixture;
	if (!TestTrue(TEXT("the fixture builds"), Fixture.IsReady()))
	{
		return false;
	}

	// A triangle cap that fits one cube piece (12) but not two: three pieces become three meshes.
	FMantlePlaceAssetLimits Limits;
	Limits.MaxTriangles = Fixture.CubeTriangles() + 2;
	USplineMeshComponent* A = Fixture.AddSpline(FVector(0.0, 0.0, 0.0), FVector(400.0, 0.0, 0.0));
	USplineMeshComponent* B = Fixture.AddSpline(FVector(0.0, 500.0, 0.0), FVector(400.0, 0.0, 0.0));
	USplineMeshComponent* C = Fixture.AddSpline(FVector(0.0, 1000.0, 0.0), FVector(400.0, 0.0, 0.0));
	const FMantleSplineBakeResult Split = BakeAll({ A, B, C }, Limits);
	TestEqual(TEXT("no problem"), Split.Problems.Num(), 0);
	TestEqual(TEXT("the cap cuts the group into meshes of one piece each"), Split.Meshes.Num(), 3);
	for (const FMantleBakedSplineMesh& Baked : Split.Meshes)
	{
		TestTrue(TEXT("each mesh is within the cap"), Baked.Triangles <= Limits.MaxTriangles);
	}

	// A piece that is over the cap by itself can't be baked, and says why.
	FMantlePlaceAssetLimits Tiny;
	Tiny.MaxTriangles = 4;
	const FMantleSplineBakeResult Refused = BakeAll({ A }, Tiny);
	TestEqual(TEXT("nothing bakes"), Refused.Meshes.Num(), 0);
	if (TestEqual(TEXT("one problem"), Refused.Problems.Num(), 1))
	{
		TestTrue(TEXT("it names the triangle count"), Refused.Problems[0].What.Contains(FString::FromInt(Fixture.CubeTriangles())));
		TestFalse(TEXT("and has a fix"), Refused.Problems[0].Remedy.IsEmpty());
	}

	// A group using more materials together than a mesh has slots can't be baked either.
	FMantlePlaceAssetLimits NoSlots;
	NoSlots.MaxSections = 0;
	const FMantleSplineBakeResult TooMany = BakeAll({ A }, NoSlots);
	TestEqual(TEXT("past the slot cap nothing bakes"), TooMany.Meshes.Num(), 0);
	TestEqual(TEXT("and it is one problem"), TooMany.Problems.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSplineMeshBakeCacheTest,
	"Mantle.Editor.SplineMeshBake.IsCachedWhileTheSourcesAreUnchanged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSplineMeshBakeCacheTest::RunTest(const FString& Parameters)
{
	using namespace MantleSplineMeshBakeTestPrivate;
	FSplineFixture Fixture;
	if (!TestTrue(TEXT("the fixture builds"), Fixture.IsReady()))
	{
		return false;
	}

	USplineMeshComponent* Component = Fixture.AddSpline(FVector::ZeroVector, FVector(400.0, 0.0, 0.0));
	const FMantleSplineBakeResult First = BakeAll({ Component }, FMantlePlaceAssetLimits());
	const FMantleSplineBakeResult Second = BakeAll({ Component }, FMantlePlaceAssetLimits());
	if (!TestEqual(TEXT("both bake one mesh"), First.Meshes.Num() + Second.Meshes.Num(), 2))
	{
		return false;
	}
	// Gather, Check and Export all bake the level: they must see one mesh, so one mesh key.
	TestTrue(TEXT("a second bake of unchanged sources returns the same mesh"), First.Meshes[0].Mesh == Second.Meshes[0].Mesh);
	TestEqual(TEXT("and so the same mesh key"), FMantlePlaceAssets::MeshKey(First.Meshes[0].Mesh), FMantlePlaceAssets::MeshKey(Second.Meshes[0].Mesh));
	TestEqual(TEXT("and the same entity id seed"), First.Meshes[0].IdSeed, Second.Meshes[0].IdSeed);

	// Moving the spline's end changes what is baked, so it is baked again.
	Component->SetStartAndEnd(FVector::ZeroVector, FVector(400.0, 0.0, 0.0), FVector(0.0, 400.0, 0.0), FVector(0.0, 400.0, 0.0), false);
	const FMantleSplineBakeResult Third = BakeAll({ Component }, FMantlePlaceAssetLimits());
	if (TestEqual(TEXT("the changed spline bakes one mesh"), Third.Meshes.Num(), 1))
	{
		TestTrue(TEXT("a different mesh"), Third.Meshes[0].Mesh != First.Meshes[0].Mesh);
		TestEqual(TEXT("with the same entity id seed, so the entity keeps its id"), Third.Meshes[0].IdSeed, First.Meshes[0].IdSeed);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSplineMeshBakeSourcesTest,
	"Mantle.Editor.SplineMeshBake.OnlyContentAPlayerCouldSeeIsBaked",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSplineMeshBakeSourcesTest::RunTest(const FString& Parameters)
{
	using namespace MantleSplineMeshBakeTestPrivate;
	FSplineFixture Fixture;
	if (!TestTrue(TEXT("the fixture builds"), Fixture.IsReady()))
	{
		return false;
	}

	USplineMeshComponent* Visible = Fixture.AddSpline(FVector::ZeroVector, FVector(400.0, 0.0, 0.0));
	TestTrue(TEXT("a visible spline mesh is a source"), FMantleSplineMeshBake::IsBakeSource(Visible));

	USplineMeshComponent* Hidden = Fixture.AddSpline(FVector::ZeroVector, FVector(400.0, 0.0, 0.0));
	Hidden->SetHiddenInGame(true);
	Hidden->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	TestFalse(TEXT("one hidden in game with no collision is not"), FMantleSplineMeshBake::IsBakeSource(Hidden));

	USplineMeshComponent* HiddenButSolid = Fixture.AddSpline(FVector::ZeroVector, FVector(400.0, 0.0, 0.0));
	HiddenButSolid->SetHiddenInGame(true);
	HiddenButSolid->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	TestTrue(TEXT("one hidden in game that collides is (a player could walk on it)"), FMantleSplineMeshBake::IsBakeSource(HiddenButSolid));

	USplineMeshComponent* NoMesh = NewObject<USplineMeshComponent>(Fixture.Owner);
	TestFalse(TEXT("one with no mesh is not"), FMantleSplineMeshBake::IsBakeSource(NoMesh));

	// Collision follows the source: a non-colliding road bakes to a mesh with no collision.
	USplineMeshComponent* Painted = Fixture.AddSpline(FVector(0.0, 1000.0, 0.0), FVector(400.0, 0.0, 0.0));
	Painted->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	const FMantleSplineBakeResult Result = BakeAll({ Visible, Painted }, FMantlePlaceAssetLimits());
	if (TestEqual(TEXT("colliding and non-colliding pieces bake apart"), Result.Meshes.Num(), 2))
	{
		const int32 Colliding = Result.Meshes.FindByPredicate([](const FMantleBakedSplineMesh& Mesh) { return Mesh.Collision == EMantleCollision::Blocking; }) ? 1 : 0;
		const int32 Walkthrough = Result.Meshes.FindByPredicate([](const FMantleBakedSplineMesh& Mesh) { return Mesh.Collision == EMantleCollision::None; }) ? 1 : 0;
		TestEqual(TEXT("one blocks"), Colliding, 1);
		TestEqual(TEXT("one doesn't"), Walkthrough, 1);
	}
	return true;
}

#endif
