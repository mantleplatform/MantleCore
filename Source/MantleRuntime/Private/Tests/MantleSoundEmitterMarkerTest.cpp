// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceBuilder.h"
#include "MantlePlaceTypes.h"
#include "MantleSoundEmitterMarker.h"

#include "Engine/Engine.h"
#include "Engine/DemoNetDriver.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Sound/AmbientSound.h"
#include "UObject/Package.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSoundEmitterMarkerTest,
	"Mantle.Runtime.PlaceBuild.SoundEmitterServerMarker",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSoundEmitterMarkerTest::RunTest(const FString& Parameters)
{
	// The engine will not spawn an AmbientSound on a dedicated server (BaseEngine.ini ClassesExcludedOnDedicatedServer), so a server builds a SoundEmitter entity as this marker.
	TestTrue(TEXT("a dedicated server spawns the marker"), AMantleSoundEmitterMarker::ClassFor(true) == AMantleSoundEmitterMarker::StaticClass());
	TestTrue(TEXT("a client, a standalone game and the editor spawn the engine's AmbientSound"), AMantleSoundEmitterMarker::ClassFor(false) == AAmbientSound::StaticClass());
	TestFalse(TEXT("the marker is not an AmbientSound (script code that wants the audio must not find one on a server)"), AMantleSoundEmitterMarker::StaticClass()->IsChildOf(AAmbientSound::StaticClass()));

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("MantleSoundEmitterMarkerTestWorld"));
	if (!TestNotNull(TEXT("a world"), World))
	{
		return false;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AMantleSoundEmitterMarker* Marker = World->SpawnActor<AMantleSoundEmitterMarker>(AMantleSoundEmitterMarker::StaticClass(), FTransform(FRotator::ZeroRotator, FVector(100.0, 200.0, 300.0)), Params);
	if (TestNotNull(TEXT("the marker spawns where the entity is"), Marker))
	{
		TestEqual(TEXT("at (100, 200, 300) cm"), Marker->GetActorLocation(), FVector(100.0, 200.0, 300.0));
		TestNotNull(TEXT("it has a root component (the entity can be moved and parented)"), Marker->GetRootComponent());
		TestTrue(TEXT("hidden in game"), Marker->IsHidden());
		TestFalse(TEXT("not replicated"), Marker->GetIsReplicated());
		TestEqual(TEXT("it draws and plays nothing: the root is its only component"), Marker->GetComponents().Num(), 1);
		Marker->Destroy();
	}
	World->DestroyWorld(/*bInformEngineOfWorld*/ false);
	return true;
}

#if WITH_EDITOR

// The builder's own path: a SoundEmitter entity built by FMantlePlaceBuilder::Build in a world that is a dedicated server. The editor's tests have no server, so the world is made
// one the way the engine does it in an editor program (UNetDriver::GetNetMode: a game world with a server net driver and a world context that "runs as dedicated").
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSoundEmitterBuilderServerTest,
	"Mantle.Runtime.PlaceBuild.SoundEmitterBuilderServer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSoundEmitterBuilderServerTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("an engine"), GEngine))
	{
		return false;
	}
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("MantleSoundEmitterBuilderServerTestWorld"));
	if (!TestNotNull(TEXT("a world"), World))
	{
		return false;
	}
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	Context.RunAsDedicated = true;
	// A concrete driver (UNetDriver itself is abstract): with no server connection its IsServer() is true, which is all the engine's editor branch asks.
	UDemoNetDriver* Driver = NewObject<UDemoNetDriver>(GetTransientPackage());
	Driver->SetWorld(World);
	World->SetNetDriver(Driver);
	TestEqual(TEXT("the test world is a dedicated server"), static_cast<int32>(World->GetNetMode()), static_cast<int32>(NM_DedicatedServer));

	// An uploaded sound's id: a server resolves nothing it cannot hear, so this builds with no asset set. "nothing_here" is neither a library sound nor an uploaded id.
	FMantlePlace Place;
	Place.SchemaVersion = 1;
	Place.PlaceId = TEXT("plc_marker001");
	FMantleEntity Emitter;
	Emitter.Id = TEXT("ent_emitter");
	Emitter.Name = TEXT("Emitter");
	Emitter.Type = EMantleEntityType::SoundEmitter;
	Emitter.Transform = FTransform(FRotator::ZeroRotator, FVector(100.0, 200.0, 300.0));
	FMantleSoundEmitterProps Props;
	Props.Sound = TEXT("ast_u0123456789abcdef0123456789abcdef");
	Emitter.Props.Set<FMantleSoundEmitterProps>(Props);
	Place.Entities.Add(Emitter);
	FMantleEntity Unknown = Emitter;
	Unknown.Id = TEXT("ent_unknown");
	Unknown.Name = TEXT("Unknown");
	FMantleSoundEmitterProps UnknownProps;
	UnknownProps.Sound = TEXT("nothing_here");
	Unknown.Props.Set<FMantleSoundEmitterProps>(UnknownProps);
	Place.Entities.Add(Unknown);

	FMantleBuildOptions Options;
	Options.bRuntimeEnvironment = false;
	FMantleBuildResult Result = FMantlePlaceBuilder::Build(World, Place, Options);

	AActor* const* Built = Result.Actors.Find(TEXT("ent_emitter"));
	if (TestTrue(TEXT("the SoundEmitter entity is built on a server"), Built && *Built))
	{
		AActor* Actor = *Built;
		TestTrue(TEXT("as the marker, not an AmbientSound"), Actor->GetClass() == AMantleSoundEmitterMarker::StaticClass());
		TestEqual(TEXT("at the entity's place (100, 200, 300) cm"), Actor->GetActorLocation(), FVector(100.0, 200.0, 300.0));
	}
	TestFalse(TEXT("a sound that is neither in the library nor an uploaded id still fails on a server, as on a client"), Result.Actors.Contains(TEXT("ent_unknown")));
	TestTrue(TEXT("and says why"), Result.ErrorSummary().Contains(TEXT("not a known sound")));

	FMantlePlaceBuilder::Teardown(Result);
	World->SetNetDriver(nullptr);
	Driver->SetWorld(nullptr);
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(/*bInformEngineOfWorld*/ false);
	return true;
}

#endif // WITH_EDITOR

#endif // WITH_DEV_AUTOMATION_TESTS
