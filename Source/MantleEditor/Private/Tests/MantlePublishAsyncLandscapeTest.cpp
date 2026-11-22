// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHeadlessPublish.h"
#include "MantleTerrainExport.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeProxy.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Mantle.PublishAsync unregisters the level's components before conversion (City Sample's Big City ran out rendering them), but a
// landscape's heightfield is read through its ULandscapeInfo, which forgets an unregistered landscape component: OrbitalBase's async
// publish failed with "Landscape 'Landscape' has no landscape components loaded". Landscapes keep their components; the rest go.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePublishAsyncLandscapeTestPrivate
{
	constexpr int32 SubsectionQuads = 7;
	constexpr int32 SizeVerts = SubsectionQuads + 1;

	/** An empty editor world for one test, destroyed with it. */
	struct FTestWorld
	{
		UWorld* World = nullptr;

		FTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantlePublishAsyncLandscapeTestWorld"));
		}

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

	/** One 8 x 8 sample landscape component whose heights rise along X, as the engine's own tests make one (PCGTestsCommon). */
	ALandscape* SpawnLandscape(UWorld* World, TArray<uint16>& OutHeights)
	{
		FActorSpawnParameters Spawn;
		Spawn.ObjectFlags = RF_Transient;
		ALandscape* Landscape = World->SpawnActor<ALandscape>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
		if (!Landscape)
		{
			return nullptr;
		}
		OutHeights.SetNumUninitialized(SizeVerts * SizeVerts);
		for (int32 Y = 0; Y < SizeVerts; ++Y)
		{
			for (int32 X = 0; X < SizeVerts; ++X)
			{
				OutHeights[Y * SizeVerts + X] = static_cast<uint16>(32768 + X * 100 + Y);
			}
		}
		TMap<FGuid, TArray<uint16>> Heights;
		Heights.Add(FGuid(), OutHeights);
		TMap<FGuid, TArray<FLandscapeImportLayerInfo>> Layers;
		Layers.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());
		Landscape->Import(FGuid::NewGuid(), 0, 0, SizeVerts - 1, SizeVerts - 1, /*InNumSubsections*/ 1, SubsectionQuads, Heights, nullptr, Layers,
			ELandscapeImportAlphamapType::Additive, MakeArrayView<const FLandscapeLayer>({}));
		Landscape->CreateLandscapeInfo();
		return Landscape;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePublishAsyncKeepsLandscapeRegisteredTest,
	"Mantle.Editor.PublishAsync.KeepsLandscapeRegisteredForTerrain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePublishAsyncKeepsLandscapeRegisteredTest::RunTest(const FString& Parameters)
{
	using namespace MantlePublishAsyncLandscapeTestPrivate;

	const FTestWorld Test;
	if (!TestNotNull(TEXT("a test world"), Test.World))
	{
		return false;
	}
	TArray<uint16> Heights;
	ALandscape* Landscape = SpawnLandscape(Test.World, Heights);
	AStaticMeshActor* Prop = Test.World->SpawnActor<AStaticMeshActor>();
	if (!TestNotNull(TEXT("a landscape"), Landscape) || !TestNotNull(TEXT("a static mesh actor"), Prop))
	{
		return false;
	}
	Prop->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));

	FMantleTerrainSource Before;
	FString Problem;
	if (!TestTrue(TEXT("the landscape reads before anything is unregistered"), FMantleTerrainExport::ReadLandscape(Landscape, Before, Problem)))
	{
		AddError(Problem);
		return false;
	}
	TestEqual(TEXT("its width"), Before.Width, SizeVerts);
	TestEqual(TEXT("its height"), Before.Height, SizeVerts);

	// What Mantle.PublishAsync does before conversion.
	TestTrue(TEXT("a landscape keeps its components"), FMantleHeadlessPublish::KeepsComponentsRegistered(Landscape));
	TestFalse(TEXT("a static mesh actor does not"), FMantleHeadlessPublish::KeepsComponentsRegistered(Prop));
	TestFalse(TEXT("nor does no actor"), FMantleHeadlessPublish::KeepsComponentsRegistered(nullptr));
	const TArray<TWeakObjectPtr<AActor>> ToUnregister = FMantleHeadlessPublish::ActorsToUnregister(Test.World);
	TestFalse(TEXT("the landscape is not unregistered"), ToUnregister.Contains(TWeakObjectPtr<AActor>(Landscape)));
	TestTrue(TEXT("the static mesh actor is (City Sample's protection)"), ToUnregister.Contains(TWeakObjectPtr<AActor>(Prop)));
	for (const TWeakObjectPtr<AActor>& Actor : ToUnregister)
	{
		if (Actor.IsValid())
		{
			Actor->UnregisterAllComponents();
		}
	}
	TestFalse(TEXT("the static mesh's component is unregistered"), Prop->GetStaticMeshComponent()->IsRegistered());

	FMantleTerrainSource After;
	if (TestTrue(TEXT("the landscape still reads after the async unregister"), FMantleTerrainExport::ReadLandscape(Landscape, After, Problem)))
	{
		TestEqual(TEXT("the same width"), After.Width, Before.Width);
		TestEqual(TEXT("the same height"), After.Height, Before.Height);
		TestTrue(TEXT("the same heights"), After.Heights == Before.Heights);
	}
	else
	{
		AddError(Problem);
	}

	// Why: with its own components unregistered, the landscape has nothing left to read (OrbitalBase's failure).
	Landscape->UnregisterAllComponents();
	FMantleTerrainSource Unregistered;
	TestFalse(TEXT("an unregistered landscape does not read"), FMantleTerrainExport::ReadLandscape(Landscape, Unregistered, Problem));
	TestTrue(TEXT("for want of loaded components"), Problem.Contains(TEXT("no landscape components loaded")));
	return true;
}

#endif
