// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceBuildRunner.h"
#include "MantlePlaceBuilder.h"
#include "MantleHeightfieldReader.h"
#include "MantlePlaceParser.h"
#include "MantleTerrainBuilder.h"

#include "Containers/Ticker.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

// The staged build: the same place built in steps (FMantlePlaceBuildSession, FMantlePlaceBuildRunner) stands exactly as the
// one-call Build makes it, a step does at least one unit and stops after the unit that crosses its budget, and stopping a build
// takes away what it had made.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantlePlaceBuildSessionTestPrivate
{
	constexpr int32 TextCount = 12;

	/**
	 * A place of one Group at x = 1000 holding one Text (the child, relative to the group), and TextCount loose Texts at x = 100 * n.
	 * The child proves attach and the relative transform; the others are enough entities to take several steps.
	 */
	FMantleParseResult ParsePlace()
	{
		FString Entities = TEXT("{\"id\":\"ent_group\",\"type\":\"Group\",\"parent\":null,"
			"\"transform\":{\"location\":[1000,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{}},"
			"{\"id\":\"ent_child\",\"type\":\"Text\",\"parent\":\"ent_group\","
			"\"transform\":{\"location\":[100,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{\"text\":\"child\"}}");
		for (int32 Number = 0; Number < TextCount; ++Number)
		{
			Entities += FString::Printf(
				TEXT(",{\"id\":\"ent_text_%d\",\"type\":\"Text\",\"parent\":null,"
					 "\"transform\":{\"location\":[%d,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{\"text\":\"t%d\"}}"),
				Number, Number * 100, Number);
		}
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_session001\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Steps\",\"authorId\":\"usr_4b1d\"},\"settings\":{},\"assets\":[],\"entities\":[%s]}"),
			*Entities);
		return FMantlePlaceParser::ParseFromString(Json);
	}

	FMantleBuildOptions TestOptions()
	{
		FMantleBuildOptions Options;
		Options.bRuntimeEnvironment = false;
		return Options;
	}

	int32 CountActors(UWorld* World)
	{
		int32 Count = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			++Count;
		}
		return Count;
	}

	/** A 257 x 257 heightfield (2 x 2 chunks of 128 quads), rising along X. */
	FMantleHeightfield MakeTerrainField()
	{
		FMantleHeightfield Field;
		Field.Width = 257;
		Field.Height = 257;
		Field.SpacingX = 100.0f;
		Field.SpacingY = 100.0f;
		Field.HeightScale = 1.0f;
		Field.HeightOffset = 0.0f;
		Field.Heights.SetNumUninitialized(257 * 257);
		for (int32 Y = 0; Y < 257; ++Y)
		{
			for (int32 X = 0; X < 257; ++X)
			{
				Field.Heights[Y * 257 + X] = static_cast<uint16>(X * 20 + (Y % 7));
			}
		}
		return Field;
	}

	int32 CountChunkComponents(const AActor* Actor)
	{
		TInlineComponentArray<UStaticMeshComponent*> Chunks(Actor);
		return Chunks.Num();
	}

	/** An empty game world for one test, destroyed with it. */
	struct FSessionWorld
	{
		UWorld* World = nullptr;

		FSessionWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("MantlePlaceBuildSessionTestWorld"));
		}

		~FSessionWorld()
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
	FMantlePlaceBuildSessionStandsLikeBuildTest,
	"Mantle.Runtime.PlaceBuild.Session.StandsLikeBuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceBuildSessionStandsLikeBuildTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceBuildSessionTestPrivate;
	FSessionWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	const FMantleParseResult Parsed = ParsePlace();
	if (!TestTrue(TEXT("the place parses"), Parsed.bSuccess))
	{
		AddError(Parsed.ErrorSummary());
		return false;
	}
	const int32 Entities = Parsed.Place.Entities.Num();
	TestEqual(TEXT("the group, its child and the loose texts"), Entities, TextCount + 2);

	// The one-call build is the reference.
	FMantleBuildResult Whole = FMantlePlaceBuilder::Build(Fixture.World, Parsed.Place, TestOptions());
	TestTrue(TEXT("the one-call build succeeds"), Whole.bSuccess);
	TestEqual(TEXT("an actor for every entity"), Whole.Actors.Num(), Entities);

	// A zero budget is the smallest step: exactly one unit, so every pass takes one step per entity.
	TSharedRef<FMantlePlaceBuildSession> Session = FMantlePlaceBuilder::Begin(Fixture.World, Parsed.Place, TestOptions());
	TestEqual(TEXT("nothing spawned yet"), Session->EntitiesLeftToSpawn(), Entities);
	TestFalse(TEXT("not finished before the first step"), Session->IsFinished());
	TestFalse(TEXT("one step does not finish a place of several entities"), Session->Step(0.0));
	TestEqual(TEXT("and spawned exactly one entity"), Session->EntitiesLeftToSpawn(), Entities - 1);
	int32 Steps = 1;
	int32 LastLeft = Session->EntitiesLeftToSpawn();
	while (!Session->Step(0.0) && Steps < 10000)
	{
		++Steps;
		const int32 Left = Session->EntitiesLeftToSpawn();
		TestTrue(TEXT("the spawn pass only goes forward"), Left <= LastLeft);
		LastLeft = Left;
	}
	++Steps;
	TestTrue(TEXT("finished"), Session->IsFinished());
	// Spawn, attach, transform: a unit each per entity, at least; the later passes add more.
	TestTrue(TEXT("a zero budget takes at least one step per entity per pass"), Steps >= Entities * 3);
	TestTrue(TEXT("and stepping a finished session stays finished"), Session->Step(0.0));

	FMantleBuildResult& Staged = Session->GetResult();
	TestTrue(TEXT("the staged build succeeds"), Staged.bSuccess);
	TestEqual(TEXT("the same number of actors"), Staged.Actors.Num(), Whole.Actors.Num());
	for (const TPair<FString, AActor*>& Pair : Whole.Actors)
	{
		AActor* const* Other = Staged.Actors.Find(Pair.Key);
		if (!TestNotNull(*FString::Printf(TEXT("%s was built by the staged build"), *Pair.Key), Other))
		{
			continue;
		}
		TestTrue(*FString::Printf(TEXT("%s stands in the same place"), *Pair.Key),
			Pair.Value->GetActorLocation().Equals((*Other)->GetActorLocation()));
		TestEqual(*FString::Printf(TEXT("%s has the same parent"), *Pair.Key),
			Pair.Value->GetAttachParentActor() != nullptr, (*Other)->GetAttachParentActor() != nullptr);
	}
	if (AActor* const* Child = Staged.Actors.Find(TEXT("ent_child")))
	{
		TestTrue(TEXT("the child is attached to the group"),
			(*Child)->GetAttachParentActor() == Staged.Actors.FindRef(TEXT("ent_group")));
		TestTrue(TEXT("and stands at the group's place plus its own"), (*Child)->GetActorLocation().Equals(FVector(1100.0, 0.0, 0.0)));
	}

	FMantlePlaceBuilder::Teardown(Staged);
	FMantlePlaceBuilder::Teardown(Whole);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceBuildSessionBudgetTest,
	"Mantle.Runtime.PlaceBuild.Session.BudgetIsSpentOnUnits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceBuildSessionBudgetTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceBuildSessionTestPrivate;
	FSessionWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	const FMantleParseResult Parsed = ParsePlace();
	if (!TestTrue(TEXT("the place parses"), Parsed.bSuccess))
	{
		return false;
	}

	// A budget longer than the whole build is one step, like Build.
	TSharedRef<FMantlePlaceBuildSession> Roomy = FMantlePlaceBuilder::Begin(Fixture.World, Parsed.Place, TestOptions());
	TestTrue(TEXT("a long budget finishes in one step"), Roomy->Step(60.0));
	TestTrue(TEXT("with every actor"), Roomy->GetResult().Actors.Num() == Parsed.Place.Entities.Num());
	FMantlePlaceBuilder::Teardown(Roomy->GetResult());

	// The unlimited budget as well.
	TSharedRef<FMantlePlaceBuildSession> Whole = FMantlePlaceBuilder::Begin(Fixture.World, Parsed.Place, TestOptions());
	TestTrue(TEXT("no budget finishes in one step"), Whole->Step(FMantlePlaceBuildSession::NoBudget));
	FMantlePlaceBuilder::Teardown(Whole->GetResult());

	// No world: finished at once, with the error and no crash.
	TSharedRef<FMantlePlaceBuildSession> NoWorld = FMantlePlaceBuilder::Begin(nullptr, Parsed.Place, TestOptions());
	TestTrue(TEXT("a build with no world is finished at once"), NoWorld->Step(0.0));
	TestFalse(TEXT("unsuccessful"), NoWorld->GetResult().bSuccess);
	TestEqual(TEXT("with one error"), NoWorld->GetResult().Errors.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceBuildRunnerTest,
	"Mantle.Runtime.PlaceBuild.Runner.BuildsOnTicksAndCancels",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceBuildRunnerTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceBuildSessionTestPrivate;
	FSessionWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	const FMantleParseResult Parsed = ParsePlace();
	if (!TestTrue(TEXT("the place parses"), Parsed.bSuccess))
	{
		return false;
	}
	const TSharedRef<const FMantlePlace> Place = MakeShared<FMantlePlace>(Parsed.Place);
	const int32 ActorsBefore = CountActors(Fixture.World);
	Fixture.World->bShouldSimulatePhysics = true;

	// It builds on the engine's ticks and calls back once, with the whole place.
	int32 Calls = 0;
	int32 BuiltActors = 0;
	FMantleBuildResult Delivered;
	TSharedPtr<FMantlePlaceBuildRunner> Runner = FMantlePlaceBuildRunner::Start(Fixture.World, Place, TestOptions(),
		[&Calls, &BuiltActors, &Delivered](FMantleBuildResult&& Built)
		{
			++Calls;
			BuiltActors = Built.Actors.Num();
			Delivered = MoveTemp(Built);
		},
		/*FrameBudgetSeconds*/ 0.0);
	TestEqual(TEXT("nothing is built before the first tick"), CountActors(Fixture.World), ActorsBefore);
	TestTrue(TEXT("it is running"), Runner->IsRunning());
	TestTrue(TEXT("the world's physics steps stay on while it builds (bodies are made final, nothing needs them off)"), Fixture.World->bShouldSimulatePhysics);
	for (int32 Tick = 0; Tick < 1000 && Calls == 0; ++Tick)
	{
		FTSTicker::GetCoreTicker().Tick(0.016f);
	}
	TestEqual(TEXT("the callback ran once"), Calls, 1);
	TestEqual(TEXT("with every actor"), BuiltActors, Place->Entities.Num());
	TestFalse(TEXT("and the runner is done"), Runner->IsRunning());
	TestTrue(TEXT("the physics steps are still on"), Fixture.World->bShouldSimulatePhysics);
	FMantlePlaceBuilder::Teardown(Delivered);
	TestEqual(TEXT("tearing the delivered result down leaves the world as it was"), CountActors(Fixture.World), ActorsBefore);
	Runner.Reset();
	FTSTicker::GetCoreTicker().Tick(0.016f);
	TestEqual(TEXT("a finished runner never calls back again"), Calls, 1);

	// Dropped before it finishes: what it made is gone and the callback never runs.
	int32 DroppedCalls = 0;
	Runner = FMantlePlaceBuildRunner::Start(Fixture.World, Place, TestOptions(),
		[&DroppedCalls](FMantleBuildResult&&) { ++DroppedCalls; },
		/*FrameBudgetSeconds*/ 0.0);
	for (int32 Tick = 0; Tick < 3; ++Tick)
	{
		FTSTicker::GetCoreTicker().Tick(0.016f);
	}
	TestTrue(TEXT("still building after a few ticks"), Runner->IsRunning());
	TestTrue(TEXT("with some of the place standing"), CountActors(Fixture.World) > ActorsBefore);
	TestTrue(TEXT("physics steps still on"), Fixture.World->bShouldSimulatePhysics);
	Runner.Reset();
	TestTrue(TEXT("and dropping the runner leaves them on"), Fixture.World->bShouldSimulatePhysics);
	TestEqual(TEXT("dropping it takes away what it had built"), CountActors(Fixture.World), ActorsBefore);
	for (int32 Tick = 0; Tick < 10; ++Tick)
	{
		FTSTicker::GetCoreTicker().Tick(0.016f);
	}
	TestEqual(TEXT("a dropped runner never calls back"), DroppedCalls, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainSpawnStepsTest,
	"Mantle.Runtime.PlaceBuild.Terrain.SpawnInChunkSteps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainSpawnStepsTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceBuildSessionTestPrivate;
	FSessionWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	const FMantleHeightfield Field = MakeTerrainField();
	FMantleTerrainBuildParams Params;
	Params.bRenderLODs = false;
	Params.bCollision = true;
	Params.bInitRenderResources = false;
	const int32 ActorsBefore = CountActors(Fixture.World);

	// Spawn is the staged spawn stepped to its end: 2 x 2 chunks.
	FString Error;
	AActor* Whole = FMantleTerrainBuilder::Spawn(Fixture.World, TEXT("Whole"), Field, nullptr, true, Params, &Error);
	if (!TestNotNull(TEXT("the one-call terrain"), Whole))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("four chunks"), CountChunkComponents(Whole), 4);

	// The same terrain a chunk a step: four steps, a chunk more on the actor after each, the same actor at the end.
	TSharedPtr<FMantleTerrainSpawn> Staged = FMantleTerrainSpawn::Begin(Fixture.World, TEXT("Staged"), Field, nullptr, true, Params, &Error);
	if (!TestTrue(TEXT("the staged terrain begins"), Staged.IsValid()))
	{
		AddError(Error);
		return false;
	}
	int32 Steps = 0;
	while (!Staged->Step() && Steps < 100)
	{
		++Steps;
	}
	++Steps;
	TestEqual(TEXT("one step a chunk"), Steps, 4);
	TestFalse(TEXT("nothing failed"), Staged->HasFailed());
	AActor* Stepped = Staged->TakeActor();
	if (TestNotNull(TEXT("the staged terrain's actor"), Stepped))
	{
		TestEqual(TEXT("with the same chunks"), CountChunkComponents(Stepped), CountChunkComponents(Whole));
		TestTrue(TEXT("tagged as a built terrain"), Stepped->ActorHasTag(FName(FMantleTerrainBuilder::TerrainActorTag)));
		Stepped->Destroy();
	}
	Staged.Reset();
	Whole->Destroy();

	// Dropped half way: what it had built is destroyed with it.
	TSharedPtr<FMantleTerrainSpawn> Dropped = FMantleTerrainSpawn::Begin(Fixture.World, TEXT("Dropped"), Field, nullptr, true, Params, &Error);
	if (TestTrue(TEXT("a third begins"), Dropped.IsValid()))
	{
		TestFalse(TEXT("not finished after one chunk"), Dropped->Step());
		TestEqual(TEXT("its actor stands"), CountActors(Fixture.World), ActorsBefore + 1);
		Dropped.Reset();
		TestEqual(TEXT("dropping it takes the actor away"), CountActors(Fixture.World), ActorsBefore);
	}

	// A heightfield that cannot be built is refused at once, with the reason.
	FMantleHeightfield Bad = MakeTerrainField();
	Bad.SpacingX = 0.0f;
	FString BadError;
	TestFalse(TEXT("an unusable heightfield does not begin"), FMantleTerrainSpawn::Begin(Fixture.World, TEXT("Bad"), Bad, nullptr, true, Params, &BadError).IsValid());
	TestFalse(TEXT("and says why"), BadError.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceBuildRunnerWorldEndsTest,
	"Mantle.Runtime.PlaceBuild.Runner.WorldEndsMidBuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceBuildRunnerWorldEndsTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceBuildSessionTestPrivate;
	const FMantleParseResult Parsed = ParsePlace();
	if (!TestTrue(TEXT("the place parses"), Parsed.bSuccess))
	{
		return false;
	}
	const TSharedRef<const FMantlePlace> Place = MakeShared<FMantlePlace>(Parsed.Place);

	// The world is destroyed and collected under a build that is part way: once with the runner still ticking (it notices and
	// stops), once with the runner dropped (it must not touch the actors the world took with it). No crash, no callback.
	int32 Calls = 0;
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("MantlePlaceBuildWorldEndsTestWorld"));
		if (!TestNotNull(TEXT("a world"), World))
		{
			return false;
		}
		TSharedPtr<FMantlePlaceBuildRunner> Runner = FMantlePlaceBuildRunner::Start(World, Place, TestOptions(),
			[&Calls](FMantleBuildResult&&) { ++Calls; },
			/*FrameBudgetSeconds*/ 0.0);
		for (int32 Tick = 0; Tick < 3; ++Tick)
		{
			FTSTicker::GetCoreTicker().Tick(0.016f);
		}
		TestTrue(TEXT("still building after a few ticks"), Runner->IsRunning());

		World->DestroyWorld(/*bInformEngineOfWorld*/ false);
		if (World->IsRooted())
		{
			World->RemoveFromRoot();
		}
		World = nullptr;
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

		if (Pass == 0)
		{
			for (int32 Tick = 0; Tick < 3; ++Tick)
			{
				FTSTicker::GetCoreTicker().Tick(0.016f);
			}
			TestFalse(TEXT("a runner whose world ended stops itself"), Runner->IsRunning());
		}
		Runner.Reset();
		FTSTicker::GetCoreTicker().Tick(0.016f);
	}
	TestEqual(TEXT("the callback never ran"), Calls, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainSpawnStaticTest,
	"Mantle.Runtime.PlaceBuild.Terrain.SpawnsStaticInPlace",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainSpawnStaticTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceBuildSessionTestPrivate;
	FSessionWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	const FMantleHeightfield Field = MakeTerrainField();
	FMantleTerrainBuildParams Params;
	Params.bRenderLODs = false;
	Params.bCollision = true;
	Params.bInitRenderResources = false;

	// Given where it ends, the terrain is made there and Static, root and every chunk, before anything registers: after every step it is Static and in place.
	const FTransform Where(FRotator(0.0, 90.0, 0.0), FVector(10000.0, -2000.0, 300.0), FVector(1.0));
	FString Error;
	TSet<const AActor*> Before; // the world's own actors (settings, brush, volumes) are not the terrain
	for (TActorIterator<AActor> It(Fixture.World); It; ++It)
	{
		Before.Add(*It);
	}
	TSharedPtr<FMantleTerrainSpawn> Staged = FMantleTerrainSpawn::Begin(Fixture.World, TEXT("Static"), Field, nullptr, true, Params, &Error, &Where);
	if (!TestTrue(TEXT("the staged terrain begins"), Staged.IsValid()))
	{
		AddError(Error);
		return false;
	}
	int32 Steps = 0;
	int32 Looked = 0;
	bool bDone = false;
	while (!bDone && Steps < 100)
	{
		bDone = Staged->Step();
		++Steps;
		// After every step, not only at the end: every registered component of the terrain being built is Static and stands where it ends.
		for (TActorIterator<AActor> It(Fixture.World); It; ++It)
		{
			if (Before.Contains(*It))
			{
				continue;
			}
			if (!It->GetActorLocation().Equals(Where.GetLocation(), 0.01))
			{
				AddError(FString::Printf(TEXT("step %d: %s is at %s, not where it ends"), Steps, *It->GetName(), *It->GetActorLocation().ToString()));
			}
			TInlineComponentArray<USceneComponent*> Registered(*It);
			for (const USceneComponent* Part : Registered)
			{
				if (Part->IsRegistered())
				{
					++Looked;
					if (Part->Mobility != EComponentMobility::Static)
					{
						AddError(FString::Printf(TEXT("step %d: %s of %s is registered and not Static"), Steps, *Part->GetName(), *It->GetName()));
					}
				}
			}
		}
	}
	TestTrue(TEXT("registered components were looked at on several steps"), Looked >= 4 && Steps >= 2);
	AActor* Actor = Staged->TakeActor();
	if (TestNotNull(TEXT("the terrain actor"), Actor))
	{
		TestEqual(TEXT("four chunks"), CountChunkComponents(Actor), 4);
		TestEqual(TEXT("the root is Static"), static_cast<int32>(Actor->GetRootComponent()->Mobility), static_cast<int32>(EComponentMobility::Static));
		TestTrue(TEXT("and stands where it ends"), Actor->GetActorLocation().Equals(Where.GetLocation(), 0.01));
		TestTrue(TEXT("turned as it ends"), FMath::IsNearlyEqual(Actor->GetActorRotation().Yaw, 90.0, 0.01));
		TInlineComponentArray<USceneComponent*> Parts(Actor);
		for (const USceneComponent* Part : Parts)
		{
			TestEqual(*FString::Printf(TEXT("%s is Static"), *Part->GetName()), static_cast<int32>(Part->Mobility), static_cast<int32>(EComponentMobility::Static));
		}
		Actor->Destroy();
	}

	// A terrain that ends Movable (a dynamic one) is made where it ends too, Movable: after every step it stands there, root and every chunk, none at the origin.
	{
		TSet<const AActor*> BeforeMovable;
		for (TActorIterator<AActor> It(Fixture.World); It; ++It)
		{
			BeforeMovable.Add(*It);
		}
		const FTransform MovableWhere(FRotator(0.0, 45.0, 0.0), FVector(-7000.0, 3000.0, 150.0), FVector(1.0));
		TSharedPtr<FMantleTerrainSpawn> Moving = FMantleTerrainSpawn::Begin(Fixture.World, TEXT("Movable"), Field, nullptr, true, Params, &Error, nullptr, &MovableWhere);
		if (TestTrue(TEXT("a movable terrain begins"), Moving.IsValid()))
		{
			int32 MovableSteps = 0;
			int32 MovableLooked = 0;
			bool bMovableDone = false;
			while (!bMovableDone && MovableSteps < 100)
			{
				bMovableDone = Moving->Step();
				++MovableSteps;
				for (TActorIterator<AActor> It(Fixture.World); It; ++It)
				{
					if (BeforeMovable.Contains(*It) || !It->GetActorLocation().Equals(MovableWhere.GetLocation(), 0.01))
					{
						if (!BeforeMovable.Contains(*It))
						{
							AddError(FString::Printf(TEXT("movable terrain step %d: %s is at %s, not where it ends"), MovableSteps, *It->GetName(), *It->GetActorLocation().ToString()));
						}
						continue;
					}
					TInlineComponentArray<USceneComponent*> Registered(*It);
					for (const USceneComponent* Part : Registered)
					{
						if (Part->IsRegistered())
						{
							++MovableLooked;
							if (Part->Mobility != EComponentMobility::Movable)
							{
								AddError(FString::Printf(TEXT("movable terrain step %d: %s is registered and not Movable"), MovableSteps, *Part->GetName()));
							}
							if (!Part->GetComponentLocation().Equals(MovableWhere.GetLocation(), 0.01) && Part->GetAttachParent() == nullptr)
							{
								AddError(FString::Printf(TEXT("movable terrain step %d: the root %s is at %s"), MovableSteps, *Part->GetName(), *Part->GetComponentLocation().ToString()));
							}
						}
					}
				}
			}
			TestTrue(TEXT("registered components of the movable terrain were looked at on several steps"), MovableLooked >= 4 && MovableSteps >= 2);
			if (AActor* MovableActor = Moving->TakeActor())
			{
				TestTrue(TEXT("and it is where it ends when it is finished"), MovableActor->GetActorLocation().Equals(MovableWhere.GetLocation(), 0.01));
				MovableActor->Destroy();
			}
		}
	}

	// Without a place to end, it is the Movable terrain at the origin it always was.
	TSharedPtr<FMantleTerrainSpawn> Plain = FMantleTerrainSpawn::Begin(Fixture.World, TEXT("Plain"), Field, nullptr, true, Params, &Error);
	if (TestTrue(TEXT("a plain terrain begins"), Plain.IsValid()))
	{
		while (!Plain->Step())
		{
		}
		AActor* PlainActor = Plain->TakeActor();
		if (TestNotNull(TEXT("its actor"), PlainActor))
		{
			TestEqual(TEXT("the root is Movable"), static_cast<int32>(PlainActor->GetRootComponent()->Mobility), static_cast<int32>(EComponentMobility::Movable));
			PlainActor->Destroy();
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
