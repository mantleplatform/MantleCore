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

	/** Points every prop at Stub for one test and gives the resolver that was there back after: the place module sets the game's at startup, and
	    other tests' props (a script host's, in a world with a retained place) adopt through it. */
	struct FStubResolverScope
	{
		FMantlePhysicsPropPlaceResolver Previous = AMantlePhysicsProp::PlaceForWorld;
		explicit FStubResolverScope(FStubPlace& Stub)
		{
			GStub = &Stub;
			AMantlePhysicsProp::PlaceForWorld = &StubResolver;
		}
		~FStubResolverScope()
		{
			AMantlePhysicsProp::PlaceForWorld = Previous;
			GStub = nullptr;
		}
	};

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
	FStubResolverScope Resolver(Stub);

	const FVector Pose(4000.0, -300.0, 120.0);
	AMantlePhysicsProp* Prop = SpawnClientProp(World, Pose, /*bServerSimulates*/ true);
	if (!TestNotNull(TEXT("the prop"), Prop)) { DestroyWorld(World); return false; }
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
	// As a game world is: an actor's components are initialized as it spawns only once the world's actors are
	// (AActor::PostActorConstruction), and that is where AStaticMeshActor applies the mode.
	World->InitializeActorsForPlay(FURL());
	AMantlePhysicsProp* Prop = World->SpawnActor<AMantlePhysicsProp>(FVector(0, 0, 500), FRotator::ZeroRotator);
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (TestNotNull(TEXT("a prop"), Prop) && TestNotNull(TEXT("the engine's cube"), Cube))
	{
		// A body (its owner is what the engine asks for the mode) only once the prop has a mesh.
		Prop->GetStaticMeshComponent()->SetStaticMesh(Cube);
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

// Script API 1.95: a server script's physics object (Assets:Spawn with Anchored = false, Entity:Clone of a physics entity) has no place entity. A client's copy
// takes over the stand-in its script host built from the spawn item, whichever came first, with the settings it was sent (none are looked up in the place,
// for a placed prop either). The stub place has no entities at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePhysicsPropRuntimeAdoptTest,
	"Mantle.Runtime.PhysicsProp.AdoptsAScriptStandInInEitherOrder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePhysicsPropRuntimeAdoptTest::RunTest(const FString&)
{
	UWorld* World = MakeWorld();
	if (!TestNotNull(TEXT("a world"), World)) { return false; }
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("the engine's cube"), Cube)) { DestroyWorld(World); return false; }
	FStubPlace Stub;
	FStubResolverScope Resolver(Stub);

	FMantlePhysicsProps Sent;
	Sent.Mass = 12.0;
	Sent.LinearDamping = 0.5;
	Sent.Material = EMantlePhysicsMaterial::Ice;

	/** A plain stand-in with the cube, as a script host builds one (twice the size, so taking its scale over shows). */
	auto StandIn = [&](const FVector& At)
	{
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(At, FRotator::ZeroRotator);
		Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
		Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
		Actor->SetActorScale3D(FVector(2.0));
		return Actor;
	};
	/** A client's copy of the prop for Id, as it arrives: its id and settings, no mesh, not the authority; then it begins play (where it first tries to adopt). */
	auto Arrive = [&](const TCHAR* Id)
	{
		AMantlePhysicsProp* Prop = World->SpawnActor<AMantlePhysicsProp>(FVector(0, 0, 300), FRotator::ZeroRotator);
		Prop->SetReplicatedStateForTest(Id, Sent);
		Prop->SetRole(ROLE_SimulatedProxy);
		Prop->DispatchBeginPlay();
		return Prop;
	};
	auto CheckAdopted = [&](const TCHAR* What, AMantlePhysicsProp* Prop, AStaticMeshActor* Was, const TCHAR* Id)
	{
		TestTrue(FString::Printf(TEXT("%s: adopted"), What), Prop->IsAdopted());
		TestTrue(FString::Printf(TEXT("%s: the entity's actor is the prop"), What), Stub.FindActor(Id) == Prop);
		TestTrue(FString::Printf(TEXT("%s: the stand-in is gone"), What), !IsValid(Was) || Was->IsActorBeingDestroyed());
		TestTrue(FString::Printf(TEXT("%s: with its mesh"), What), Prop->GetStaticMeshComponent()->GetStaticMesh() == Cube);
		TestTrue(FString::Printf(TEXT("%s: and its scale"), What), Prop->GetActorScale3D().Equals(FVector(2.0), 0.001));
		TestEqual(FString::Printf(TEXT("%s: the mass it was sent"), What), Prop->GetMassKg(), 12.0, 0.01);
		TestEqual(FString::Printf(TEXT("%s: the damping it was sent"), What), static_cast<double>(Prop->GetStaticMeshComponent()->GetLinearDamping()), 0.5, 0.001);
		TestEqual(FString::Printf(TEXT("%s: the surface it was sent"), What), static_cast<int32>(Prop->GetPhysicsMaterial()), static_cast<int32>(EMantlePhysicsMaterial::Ice));
	};

	// The stand-in first: the copy takes it over as it begins play.
	AStaticMeshActor* First = StandIn(FVector(0, 0, 300));
	Stub.Actors.Add(TEXT("mesh_1"), First);
	AMantlePhysicsProp* Early = Arrive(TEXT("mesh_1"));
	CheckAdopted(TEXT("stand-in first"), Early, First, TEXT("mesh_1"));

	// The copy first: it waits (nothing to take over, and no place entity to complain about), then takes the stand-in over when it is registered.
	AMantlePhysicsProp* Waiting = Arrive(TEXT("clone_1"));
	TestFalse(TEXT("copy first: waits"), Waiting->IsAdopted());
	AStaticMeshActor* Second = StandIn(FVector(0, 0, 300));
	Stub.Actors.Add(TEXT("clone_1"), Second);
	AMantlePhysicsProp::AdoptWaiting(World, TEXT("mesh_1")); // another entity's stand-in: not this copy's
	TestFalse(TEXT("copy first: not another entity's stand-in"), Waiting->IsAdopted());
	AMantlePhysicsProp::AdoptWaiting(World, TEXT("clone_1"));
	CheckAdopted(TEXT("copy first"), Waiting, Second, TEXT("clone_1"));

	// The entity goes here while the server still sends the copy (its spawn's area went away): the copy waits again, and takes the next stand-in over
	// rather than being drawn beside it.
	Waiting->ReturnToWaiting();
	TestFalse(TEXT("returned: waits"), Waiting->IsAdopted());
	AStaticMeshActor* Third = StandIn(FVector(0, 0, 300));
	Stub.Actors.Add(TEXT("clone_1"), Third);
	AMantlePhysicsProp::AdoptWaiting(World, TEXT("clone_1"));
	TestTrue(TEXT("returned: takes the new stand-in over"), Waiting->IsAdopted() && Stub.FindActor(TEXT("clone_1")) == Waiting);
	TestTrue(TEXT("returned: and the new stand-in is gone"), !IsValid(Third) || Third->IsActorBeingDestroyed());

	// A copy whose entity this machine has forgotten (destroyed here) leaves no stand-in when the engine takes it away.
	Stub.Actors.Remove(TEXT("mesh_1"));
	Stub.Replaced.Reset();
	TestNull(TEXT("a forgotten entity's copy leaves no stand-in"), Early->RestorePlaceholder());
	TestEqual(TEXT("and the place is told of none"), Stub.Replaced.Num(), 0);

	DestroyWorld(World);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
