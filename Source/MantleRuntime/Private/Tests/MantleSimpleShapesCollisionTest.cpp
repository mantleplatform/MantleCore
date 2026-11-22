// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssets.h"
#include "MantleRuntimeStaticMeshBuilder.h"
#include "MantleStaticMeshReader.h"

#include "Async/TaskGraphInterfaces.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "PhysicsEngine/BodySetup.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

// #189: an uploaded mesh whose collision is simple-as-complex boxes, spheres or capsules (no convex hull) has nothing for the
// physics cook to do, and the engine reports that async cook as not succeeded. The place loader read that as "its collision could
// not be cooked" (574 of City Sample's meshes, on client and server) while the shapes were there and blocked. This builds such a
// mesh as the loader does (the async cook, MRM2 simple collision), shows what the engine reports, that the loader's rule
// (FMantlePlaceAssets::CollidesAfterCook) calls it solid, and that a trace really stops on it.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleSimpleShapesCollisionTestPrivate
{
	/** A 100 cm box as a one-section MRM2 mesh, with the given simple collision. */
	FMantleStaticMeshData MakeBox(const FMantleStaticMeshCollision& Collision)
	{
		FMantleStaticMeshData Mesh;
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const float X = (Corner & 1) ? 50.0f : -50.0f;
			const float Y = (Corner & 2) ? 50.0f : -50.0f;
			const float Z = (Corner & 4) ? 50.0f : -50.0f;
			Mesh.Positions.Add(FVector3f(X, Y, Z));
			Mesh.Normals.Add(FVector3f(0.0f, 0.0f, 1.0f));
			Mesh.Tangents.Add(FVector4f(1.0f, 0.0f, 0.0f, 1.0f));
			Mesh.UVs.Add(FVector2f((X + 50.0f) / 100.0f, (Y + 50.0f) / 100.0f));
		}
		const uint32 Triangles[12][3] = {
			{ 0, 2, 1 }, { 1, 2, 3 }, { 4, 5, 6 }, { 5, 7, 6 },
			{ 0, 1, 4 }, { 1, 5, 4 }, { 2, 6, 3 }, { 3, 6, 7 },
			{ 0, 4, 2 }, { 2, 4, 6 }, { 1, 3, 5 }, { 3, 7, 5 } };
		for (const uint32 (&Triangle)[3] : Triangles)
		{
			Mesh.Indices.Add(Triangle[0]);
			Mesh.Indices.Add(Triangle[1]);
			Mesh.Indices.Add(Triangle[2]);
		}
		Mesh.Bounds = FBox3f(FVector3f(-50.0f), FVector3f(50.0f));
		FMantleSkinnedMeshSection Section;
		Section.MaterialIndex = 0;
		Section.FirstIndex = 0;
		Section.IndexCount = 36;
		Section.MaterialName = TEXT("A");
		Mesh.Sections = { Section };
		Mesh.Collision = Collision;
		return Mesh;
	}

	/** One box covering the mesh, answering complex queries too: the most common shape of the failing City Sample meshes. */
	FMantleStaticMeshCollision BoxesOnly()
	{
		FMantleStaticMeshCollision Collision;
		Collision.bSimpleAsComplex = true;
		FMantleStaticMeshBox Box;
		Box.Size = FVector3f(100.0f);
		Collision.Boxes.Add(Box);
		FMantleStaticMeshSphere Sphere;
		Sphere.Center = FVector3f(0.0f, 0.0f, 50.0f);
		Sphere.Radius = 20.0f;
		Collision.Spheres.Add(Sphere);
		return Collision;
	}

	/** The same as one convex hull: there is something to cook. */
	FMantleStaticMeshCollision OneHull()
	{
		FMantleStaticMeshCollision Collision;
		Collision.bSimpleAsComplex = true;
		FMantleStaticMeshHull Hull;
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			Hull.Vertices.Add(FVector3f((Corner & 1) ? 50.0f : -50.0f, (Corner & 2) ? 50.0f : -50.0f, (Corner & 4) ? 50.0f : -50.0f));
		}
		Collision.Hulls.Add(Hull);
		return Collision;
	}

	/** Built as the place loader builds it for the async cook: collision on, not cooked in the build, no render resources. */
	UStaticMesh* BuildForAsyncCook(const FMantleStaticMeshCollision& Collision, FString& OutError)
	{
		FMantleStaticMeshBuildParams Params;
		Params.bComplexCollision = true;
		Params.bCookCollisionNow = false;
		Params.bInitRenderResources = false;
		return FMantleRuntimeStaticMeshBuilder::Build(MakeBox(Collision), Params, &OutError);
	}

	/** Runs the body's async cook as the loader does and pumps the game thread until it reports; false when it never did. */
	bool CookAsync(UBodySetup& Body, bool& bOutSucceeded)
	{
		bool bDone = false;
		Body.CreatePhysicsMeshesAsync(FOnAsyncPhysicsCookFinished::CreateLambda([&bDone, &bOutSucceeded](const bool bSucceeded)
		{
			bOutSucceeded = bSucceeded;
			bDone = true;
		}));
		const double Deadline = FPlatformTime::Seconds() + 20.0;
		while (!bDone && FPlatformTime::Seconds() < Deadline)
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::Sleep(0.001f);
		}
		return bDone;
	}

	struct FTestWorld
	{
		UWorld* World = nullptr;
		FTestWorld() { World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(*FString::Printf(TEXT("MantleSimpleShapesTestWorld_%s"), *FGuid::NewGuid().ToString()))); }
		~FTestWorld()
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSimpleShapesCollisionTest,
	"Mantle.Runtime.StaticMesh.SimpleShapesCollision",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSimpleShapesCollisionTest::RunTest(const FString& Parameters)
{
	using namespace MantleSimpleShapesCollisionTestPrivate;

	TestFalse(TEXT("no body: does not collide"), FMantlePlaceAssets::CollidesAfterCook(nullptr));
	{
		TStrongObjectPtr<UBodySetup> Empty(NewObject<UBodySetup>());
		TestFalse(TEXT("a body with no shapes and no triangles does not collide"), FMantlePlaceAssets::CollidesAfterCook(Empty.Get()));
	}

	// The City Sample case: a box and a sphere, simple as complex, nothing to cook.
	FString Error;
	TStrongObjectPtr<UStaticMesh> Boxes(BuildForAsyncCook(BoxesOnly(), Error));
	if (!TestNotNull(TEXT("the boxes-only mesh builds"), Boxes.Get()))
	{
		AddError(Error);
		return false;
	}
	UBodySetup* BoxesBody = Boxes->GetBodySetup();
	if (!TestNotNull(TEXT("it has a body"), BoxesBody))
	{
		return false;
	}
	TestEqual(TEXT("simple as complex"), static_cast<int32>(BoxesBody->CollisionTraceFlag), static_cast<int32>(CTF_UseSimpleAsComplex));
	bool bSucceeded = true;
	if (!TestTrue(TEXT("the async cook reports"), CookAsync(*BoxesBody, bSucceeded)))
	{
		return false;
	}
	// What the engine says (UBodySetup::FinishCreatePhysicsMeshesAsync with no helper). Should this ever change, the rule still holds.
	if (bSucceeded)
	{
		AddInfo(TEXT("this engine reports a cook with nothing to cook as succeeded: the false alarm of #189 cannot happen here"));
	}
	else
	{
		AddInfo(TEXT("the engine reports the cook with nothing to cook as not succeeded (the false alarm of #189)"));
	}
	TestEqual(TEXT("the box is kept"), BoxesBody->AggGeom.BoxElems.Num(), 1);
	TestEqual(TEXT("the sphere is kept"), BoxesBody->AggGeom.SphereElems.Num(), 1);
	TestTrue(TEXT("the loader calls it solid"), FMantlePlaceAssets::CollidesAfterCook(BoxesBody));

	// A hull: something to cook, cooked, solid.
	TStrongObjectPtr<UStaticMesh> Hull(BuildForAsyncCook(OneHull(), Error));
	if (!TestNotNull(TEXT("the hull mesh builds"), Hull.Get()) || !TestNotNull(TEXT("with a body"), Hull->GetBodySetup()))
	{
		AddError(Error);
		return false;
	}
	bool bHullSucceeded = false;
	if (TestTrue(TEXT("the hull's async cook reports"), CookAsync(*Hull->GetBodySetup(), bHullSucceeded)))
	{
		TestTrue(TEXT("the hull cook succeeded"), bHullSucceeded);
		TestTrue(TEXT("the hull was cooked"), Hull->GetBodySetup()->AggGeom.ConvexElems.Num() == 1
			&& Hull->GetBodySetup()->AggGeom.ConvexElems[0].GetChaosConvexMesh().IsValid());
		TestTrue(TEXT("the loader calls it solid"), FMantlePlaceAssets::CollidesAfterCook(Hull->GetBodySetup()));
	}

	// And it blocks: placed in a world, simple and complex traces (a walking player, a shot) stop on the box's face.
	FTestWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	AStaticMeshActor* Actor = Fixture.World->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the actor"), Actor))
	{
		return false;
	}
	UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
	Component->SetMobility(EComponentMobility::Movable);
	Component->SetStaticMesh(Boxes.Get());
	Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Component->SetCollisionResponseToAllChannels(ECR_Block);
	Component->RecreatePhysicsState();
	for (const bool bComplex : { false, true })
	{
		const FString What = bComplex ? TEXT("a complex trace (a shot)") : TEXT("a simple trace (a walking player)");
		FHitResult Hit;
		const FCollisionQueryParams Query(SCENE_QUERY_STAT(MantleSimpleShapesTest), bComplex);
		const bool bHit = Fixture.World->LineTraceSingleByChannel(Hit, FVector(-500.0, 0.0, 0.0), FVector(500.0, 0.0, 0.0), ECC_Visibility, Query);
		TestTrue(What + TEXT(" hits the mesh"), bHit && Hit.GetComponent() == Component);
		TestTrue(*FString::Printf(TEXT("%s stops at the box's face, x = -50 (%.2f)"), *What, Hit.ImpactPoint.X), bHit && FMath::Abs(Hit.ImpactPoint.X + 50.0) < 0.5);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
