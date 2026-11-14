// Cells in the exporter, from a level: what only a level can show (the actors' own flags and guids). The planner's rules are in
// MantleCellsTest.cpp.

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "MantleCells.h"
#include "MantlePlaceExporter.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCellsExportTestPrivate
{
	struct FCellsWorld
	{
		UWorld* World = nullptr;

		FCellsWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleCellsExportTestWorld"));
		}

		~FCellsWorld()
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

		/** A static mesh actor of the engine cube, named Name (the object name, which orders actors that share a guid) and labeled Label. */
		AStaticMeshActor* SpawnCube(const TCHAR* Name, const TCHAR* Label, const FVector& Location)
		{
			UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
			if (!World || !Cube)
			{
				return nullptr;
			}
			FActorSpawnParameters Params;
			Params.Name = FName(Name);
			AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator, Params);
			if (Actor)
			{
				Actor->SetActorLabel(Label);
				Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
			}
			return Actor;
		}
	};

	/** An actor's guid, set the way a level instance loaded twice would leave two of them: the property, since the engine hides the setter. */
	bool SetGuid(AActor* Actor, const FGuid& Guid)
	{
		const FStructProperty* Property = CastField<FStructProperty>(AActor::StaticClass()->FindPropertyByName(TEXT("ActorGuid")));
		if (!Actor || !Property)
		{
			return false;
		}
		*Property->ContainerPtrToValuePtr<FGuid>(Actor) = Guid;
		return Actor->GetActorGuid() == Guid;
	}

	/** The entity named Name, in the root or in any cell. */
	const FMantleEntity* FindByName(const FMantleExportResult& Result, const FString& Name)
	{
		for (const FMantleEntity& Entity : Result.Place.Entities)
		{
			if (Entity.Name == Name)
			{
				return &Entity;
			}
		}
		for (const FMantlePlannedCell& Cell : Result.Cells)
		{
			for (const FMantleEntity& Entity : Cell.Entities)
			{
				if (Entity.Name == Name)
				{
					return &Entity;
				}
			}
		}
		return nullptr;
	}

	bool InRoot(const FMantleExportResult& Result, const FString& Name)
	{
		return Result.Place.Entities.ContainsByPredicate([&Name](const FMantleEntity& Entity) { return Entity.Name == Name; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellsNotPartitionedTest,
	"Mantle.Editor.Cells.AnyActorIsSpatiallyLoadedInALevelWithoutWorldPartition",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellsNotPartitionedTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellsExportTestPrivate;
	FCellsWorld Level;
	AStaticMeshActor* Plain = Level.SpawnCube(TEXT("CellsPlain"), TEXT("Plain"), FVector(100.0, 100.0, 0.0));
	// Info, light and PCG actors report Is Spatially Loaded off by default; a creator can also turn it off on any actor. Rule 6 of
	// contract 5.2 reads the flag only where World Partition exists, so in this level it changes nothing.
	AStaticMeshActor* Off = Level.SpawnCube(TEXT("CellsOff"), TEXT("FlagOff"), FVector(300.0, 300.0, 0.0));
	if (!TestNotNull(TEXT("the actors spawned"), Plain) || !TestNotNull(TEXT("both"), Off))
	{
		return false;
	}
	Off->SetIsSpatiallyLoaded(false);
	TestFalse(TEXT("the flag is off on one of them"), Off->GetIsSpatiallyLoaded());
	TestFalse(TEXT("the level has no World Partition"), UWorld::IsPartitionedWorld(Level.World));

	const FMantleExportResult Result = FMantlePlaceExporter::Export(Level.World, FMantleExportOptions());
	const FMantleEntity* PlainEntity = FindByName(Result, TEXT("Plain"));
	const FMantleEntity* OffEntity = FindByName(Result, TEXT("FlagOff"));
	if (!TestNotNull(TEXT("the plain actor is published"), PlainEntity) || !TestNotNull(TEXT("and so is the other"), OffEntity))
	{
		return false;
	}
	TestFalse(TEXT("the plain one left the root"), InRoot(Result, TEXT("Plain")));
	TestFalse(TEXT("so did the one whose flag is off, which means nothing here"), InRoot(Result, TEXT("FlagOff")));
	TestEqual(TEXT("nothing was kept for the flag"), Result.CellReport.StayCounts[static_cast<int32>(EMantleCellStay::NotSpatiallyLoaded)], static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellsSharedGuidTest,
	"Mantle.Editor.Cells.ActorsSharingAGuidGetTheSameIdsWhateverTheLoadOrder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellsSharedGuidTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellsExportTestPrivate;
	const FGuid Shared(0x11111111, 0x22222222, 0x33333333, 0x44444444);

	// The same two actors, spawned in either order (the order the engine iterates them in).
	TMap<FString, FString> IdsByLabel[2];
	for (int32 Order = 0; Order < 2; ++Order)
	{
		FCellsWorld Level;
		const TCHAR* Names[2] = { TEXT("CellsAlpha"), TEXT("CellsBeta") };
		const TCHAR* Labels[2] = { TEXT("Alpha"), TEXT("Beta") };
		const FVector Places[2] = { FVector(100.0, 100.0, 0.0), FVector(900.0, 100.0, 0.0) };
		for (int32 Step = 0; Step < 2; ++Step)
		{
			const int32 Which = Order == 0 ? Step : 1 - Step;
			AStaticMeshActor* Actor = Level.SpawnCube(Names[Which], Labels[Which], Places[Which]);
			if (!TestNotNull(TEXT("an actor spawned"), Actor) || !TestTrue(TEXT("its guid was set"), SetGuid(Actor, Shared)))
			{
				return false;
			}
		}
		const FMantleExportResult Result = FMantlePlaceExporter::Export(Level.World, FMantleExportOptions());
		for (const TCHAR* Label : Labels)
		{
			const FMantleEntity* Entity = FindByName(Result, Label);
			if (!TestNotNull(TEXT("the actor is published"), Entity))
			{
				return false;
			}
			IdsByLabel[Order].Add(Label, Entity->Id);
		}
		TestNotEqual(TEXT("the two have different ids"), IdsByLabel[Order].FindRef(TEXT("Alpha")), IdsByLabel[Order].FindRef(TEXT("Beta")));
	}
	TestEqual(TEXT("Alpha has the same id either way"), IdsByLabel[0].FindRef(TEXT("Alpha")), IdsByLabel[1].FindRef(TEXT("Alpha")));
	TestEqual(TEXT("and Beta"), IdsByLabel[0].FindRef(TEXT("Beta")), IdsByLabel[1].FindRef(TEXT("Beta")));
	// By full path name: the actor named first keeps the guid's own id, the other takes the suffix.
	TestTrue(TEXT("the second by name is the suffixed one"), IdsByLabel[0].FindRef(TEXT("Beta")).EndsWith(TEXT("_2")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
