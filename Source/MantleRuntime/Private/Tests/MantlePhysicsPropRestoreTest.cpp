// Copyright Joshua Eger. All Rights Reserved.
//
// Stage 6 of large places, physics objects by distance (S6-B): a client whose server stops sending a prop is left a plain stand-in for it (AMantlePhysicsProp::RestorePlaceholder, run by
// the engine's PreDestroyFromReplication). A scratch world has no NetDriver, so the engine's destroy is called by hand; what is checked is what the prop owns. Expected values by hand.

#include "MantlePhysicsProp.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MantlePhysicsPropRestoreTestPrivate
{
	/** The place of a client's world, as far as a prop needs it: which actor each entity has, and what it was told when that changed. */
	class FStubPlace : public IMantlePhysicsPropPlace
	{
	public:
		FMantlePlace Place;
		TMap<FString, AActor*> Actors;
		TArray<FString> Replaced;
		FSimpleMulticastDelegate Ready;

		virtual bool HasPlace() const override { return true; }
		virtual const FMantlePlace& GetPlace() const override { return Place; }
		virtual AActor* FindActor(const FString& EntityId) const override { AActor* const* Found = Actors.Find(EntityId); return Found ? *Found : nullptr; }
		virtual void ReplaceEntityActor(const FString& EntityId, AActor* NewActor) override { Actors.Add(EntityId, NewActor); Replaced.Add(EntityId); }
		virtual FSimpleMulticastDelegate& OnPlaceReady() override { return Ready; }
	};

	FStubPlace* GStub = nullptr;
	IMantlePhysicsPropPlace* StubResolver(UWorld*) { return GStub; }

	UWorld* MakeWorld()
	{
		if (!GEngine)
		{
			return nullptr;
		}
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false);
		if (World)
		{
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
		}
		return World;
	}

	void DestroyWorld(UWorld* World)
	{
		if (World)
		{
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
	}

	/** A prop with the engine's cube, as a client's adopted copy: not the authority, adopted, the server's body simulating at Pose. */
	AMantlePhysicsProp* SpawnClientProp(UWorld* World, const FVector& Pose, const bool bServerSimulates)
	{
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		AMantlePhysicsProp* Prop = Cube ? World->SpawnActor<AMantlePhysicsProp>(Pose, FRotator(0, 30, 0)) : nullptr;
		if (!Prop)
		{
			return nullptr;
		}
		Prop->GetStaticMeshComponent()->SetStaticMesh(Cube);
		FMantlePhysicsProps Physics;
		Physics.bSimulate = bServerSimulates;
		Physics.Material = EMantlePhysicsMaterial::Rubber;
		Prop->InitFromPlace(TEXT("ent_crate"), Physics);
		Prop->SetRole(ROLE_SimulatedProxy);
		Prop->MarkAdoptedForTest();
		Prop->GetReplicatedMovement_Mutable().bRepPhysics = bServerSimulates;
		return Prop;
	}
}

using namespace MantlePhysicsPropRestoreTestPrivate;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePhysicsPropRestoreTest,
	"Mantle.Runtime.PhysicsProp.RelevancyRestoresAPlaceholder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePhysicsPropRestoreTest::RunTest(const FString&)
{
	UWorld* World = MakeWorld();
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	FStubPlace Stub;
	GStub = &Stub;
	AMantlePhysicsProp::PlaceForWorld = &StubResolver;

	const FVector Pose(4000.0, -300.0, 120.0);
	AMantlePhysicsProp* Prop = SpawnClientProp(World, Pose, /*bServerSimulates*/ true);
	if (!TestNotNull(TEXT("the prop"), Prop)) { AMantlePhysicsProp::PlaceForWorld = nullptr; GStub = nullptr; DestroyWorld(World); return false; }
	Prop->SetActorHiddenInGame(true);
	Prop->SetActorScale3D(FVector(2.0, 2.0, 2.0));
	AStaticMeshActor* Child = World->SpawnActor<AStaticMeshActor>(Pose + FVector(0, 0, 100), FRotator::ZeroRotator);
	Child->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable); // the builder makes every place actor Movable: a Static one cannot be attached to a Movable parent
	Child->AttachToActor(Prop, FAttachmentTransformRules::KeepWorldTransform);
	Stub.Actors.Add(TEXT("ent_crate"), Prop);

	// The engine's replicated destroy runs this just before it destroys the copy (the server stopped sending it).
	Prop->PreDestroyFromReplication();

	AStaticMeshActor* Placeholder = Cast<AStaticMeshActor>(Stub.FindActor(TEXT("ent_crate")));
	if (TestNotNull(TEXT("the entity's actor is a stand-in now"), Placeholder) && TestTrue(TEXT("a plain mesh actor, not a prop"), !Cast<AMantlePhysicsProp>(Placeholder)))
	{
		TestEqual(TEXT("the place was told once"), Stub.Replaced.Num(), 1);
		TestTrue(TEXT("at the prop's pose"), Placeholder->GetActorLocation().Equals(Pose, 0.01));
		TestTrue(TEXT("with its turn"), FMath::IsNearlyEqual(Placeholder->GetActorRotation().Yaw, 30.0, 0.01));
		TestTrue(TEXT("and its scale"), Placeholder->GetActorScale3D().Equals(FVector(2.0), 0.001));
		TestTrue(TEXT("hidden as the prop was"), Placeholder->IsHidden());
		TestTrue(TEXT("with the prop's mesh"), Placeholder->GetStaticMeshComponent()->GetStaticMesh() == Prop->GetStaticMeshComponent()->GetStaticMesh());
		TestFalse(TEXT("not simulating (a client holds no body for a far prop)"), Placeholder->GetStaticMeshComponent()->IsSimulatingPhysics());
		TestTrue(TEXT("what was attached to the prop follows the stand-in"), Child->GetAttachParentActor() == Placeholder);
		const UMantleFarPropFacts* Facts = Placeholder->FindComponentByClass<UMantleFarPropFacts>();
		if (TestNotNull(TEXT("with what the prop was last seen as"), Facts))
		{
			TestEqual(TEXT("its surface"), static_cast<int32>(Facts->Material), static_cast<int32>(EMantlePhysicsMaterial::Rubber));
			TestFalse(TEXT("the server's body simulated: not anchored"), Facts->bAnchored);
			TestTrue(TEXT("grabbable"), Facts->bGrabbable);
		}
	}

	// A stand-in is not made for what is not a stand-in for anything: a copy that never took its placeholder (not adopted), the authority, and a prop whose entity already has another actor.
	Stub.Replaced.Reset();
	AMantlePhysicsProp* Loose = SpawnClientProp(World, Pose, true);
	Stub.Actors.Add(TEXT("ent_crate"), Loose);
	Loose->RestorePlaceholder(); // adopted
	TestEqual(TEXT("an adopted copy leaves one"), Stub.Replaced.Num(), 1);
	Stub.Replaced.Reset();
	AMantlePhysicsProp* Server = World->SpawnActor<AMantlePhysicsProp>(Pose, FRotator::ZeroRotator);
	Stub.Actors.Add(TEXT("ent_crate"), Server);
	TestNull(TEXT("the authority makes none"), Server->RestorePlaceholder());
	AMantlePhysicsProp* Other = SpawnClientProp(World, Pose, true);
	Stub.Actors.Add(TEXT("ent_crate"), Server); // the entity's actor is not this copy
	TestNull(TEXT("a copy that is not the entity's actor makes none"), Other->RestorePlaceholder());
	AMantlePhysicsProp* Waiting = World->SpawnActor<AMantlePhysicsProp>(Pose, FRotator::ZeroRotator);
	Waiting->InitFromPlace(TEXT("ent_crate"), FMantlePhysicsProps());
	Waiting->SetRole(ROLE_SimulatedProxy);
	Stub.Actors.Add(TEXT("ent_crate"), Waiting);
	TestFalse(TEXT("a copy that has not taken its placeholder over is not adopted"), Waiting->IsAdopted());
	TestNull(TEXT("and makes none"), Waiting->RestorePlaceholder());
	TestEqual(TEXT("and the place was told of none of them"), Stub.Replaced.Num(), 0);

	AMantlePhysicsProp::PlaceForWorld = nullptr;
	GStub = nullptr;
	DestroyWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePhysicsPropRegistryTest,
	"Mantle.Runtime.PhysicsProp.ServerPropsAreListedWhileTheyPlay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePhysicsPropRegistryTest::RunTest(const FString&)
{
	UWorld* World = MakeWorld();
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("the engine's cube"), Cube)) { DestroyWorld(World); return false; }
	AMantlePhysicsProp* Prop = World->SpawnActor<AMantlePhysicsProp>(FVector(0, 0, 500), FRotator::ZeroRotator);
	Prop->GetStaticMeshComponent()->SetStaticMesh(Cube);
	FMantlePhysicsProps Physics;
	Physics.bSimulate = true;
	Prop->InitFromPlace(TEXT("ent_listed"), Physics);
	Prop->BeginSimulation();

	auto Count = [&]()
	{
		int32 N = 0;
		AMantlePhysicsProp::ForEachServerProp(World, [&N](AMantlePhysicsProp&) { ++N; });
		return N;
	};
	TestEqual(TEXT("not listed before it begins play"), Count(), 0);
	Prop->DispatchBeginPlay();
	TestEqual(TEXT("listed once it plays (the server's, with authority)"), Count(), 1);
	TestTrue(TEXT("a body that simulates is awake the moment it starts"), Prop->IsBodyAwake());
	Prop->GetStaticMeshComponent()->PutRigidBodyToSleep();
	TestFalse(TEXT("and asleep when put to sleep"), Prop->IsBodyAwake());
	Prop->SetAnchored(true);
	TestFalse(TEXT("an anchored one is never awake"), Prop->IsBodyAwake());
	Prop->Destroy();
	TestEqual(TEXT("not listed once it ends"), Count(), 0);
	DestroyWorld(World);
	return true;
}

// A client's copy follows the server by predictive interpolation, not by simulating freely between updates (the joints
// play test saw the default mode pull motor-driven props back thirty times a second). The engine reads the mode from the
// actor (FBodyInstance::GetPhysicsReplicationMode), which AStaticMeshActor sets when its components are initialized.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePhysicsPropReplicationModeTest,
	"Mantle.Runtime.PhysicsProp.FollowsTheServerByPredictiveInterpolation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePhysicsPropReplicationModeTest::RunTest(const FString&)
{
	UWorld* World = MakeWorld();
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	AMantlePhysicsProp* Prop = World->SpawnActor<AMantlePhysicsProp>(FVector(0, 0, 500), FRotator::ZeroRotator);
	if (TestNotNull(TEXT("a prop"), Prop))
	{
		TestTrue(TEXT("its actor replicates physics by predictive interpolation"),
			Prop->GetPhysicsReplicationMode() == EPhysicsReplicationMode::PredictiveInterpolation);
		TestTrue(TEXT("and so does its body"),
			Prop->GetStaticMeshComponent()->GetBodyInstance()->GetPhysicsReplicationMode() == EPhysicsReplicationMode::PredictiveInterpolation);
		TestTrue(TEXT("at 30 updates a second"), FMath::IsNearlyEqual(Prop->GetNetUpdateFrequency(), 30.0f));
		Prop->Destroy();
	}
	DestroyWorld(World);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
