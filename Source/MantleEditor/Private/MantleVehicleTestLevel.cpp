// Copyright Joshua Eger. All Rights Reserved.
// Mantle.Vehicle.MakeTestLevel: a developer harness for the Test Locally check of a vehicle (#821 merge requirement 3), so a headless editor can make the level a
// vehicle is tried in without anyone clicking Put it in my level. It makes a new level with a long flat floor, a sun and sky, a player start and one Mantle Vehicle
// Spawn parked on the floor with the vehicle on it, saves it and leaves it open as the editor's level, ready for Mantle.Preview.TestLocally.

#include "MantleVehicleSource.h"
#include "MantleVehicleSpawnPoint.h"

#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/IConsoleManager.h"
#include "Misc/PackageName.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleTestLevelPrivate
{
	FAutoConsoleCommand MakeTestLevelCommand(
		TEXT("Mantle.Vehicle.MakeTestLevel"),
		TEXT("Mantle.Vehicle.MakeTestLevel <vehicle asset path> <LevelName> [floor length m, default 800] [floor width m, default 200] -- makes /Game/MantleVehicles/TestLocally/<LevelName> with a flat floor "
			 "(its top at height 0, from 100 m behind the origin), a player start 8 m behind the vehicle's spawn and a Mantle Vehicle Spawn at the origin facing +X with the vehicle, saves it and "
			 "leaves it open. Logs MANTLE_VEHICLE_TEST_LEVEL ok=<0|1>."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogTemp, Error, TEXT("MANTLE_VEHICLE_TEST_LEVEL ok=0 usage: Mantle.Vehicle.MakeTestLevel <vehicle asset path> <LevelName> [floor length m] [floor width m]"));
				return;
			}
			UMantleVehicleSource* Source = LoadObject<UMantleVehicleSource>(nullptr, *Args[0], nullptr, LOAD_NoWarn | LOAD_Quiet);
			UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
			if (!Source || !Cube || !GEditor)
			{
				UE_LOG(LogTemp, Error, TEXT("MANTLE_VEHICLE_TEST_LEVEL ok=0 %s"), !Source ? TEXT("the vehicle asset was not found") : !Cube ? TEXT("the engine cube was not found") : TEXT("no editor"));
				return;
			}
			const double LengthM = Args.Num() > 2 ? FCString::Atod(*Args[2]) : 800.0;
			const double WidthM = Args.Num() > 3 ? FCString::Atod(*Args[3]) : 200.0;
			const FString Package = FString::Printf(TEXT("/Game/MantleVehicles/TestLocally/%s"), *Args[1]);

			UWorld* World = UEditorLoadingAndSavingUtils::NewBlankMap(/*bSaveExistingMap*/ false);
			if (!World)
			{
				UE_LOG(LogTemp, Error, TEXT("MANTLE_VEHICLE_TEST_LEVEL ok=0 a blank level could not be made"));
				return;
			}
			// The engine cube is 100 cm a side. The floor is 1 m thick, its top at height 0, running from 100 m behind the origin along +X.
			AStaticMeshActor* Floor = World->SpawnActor<AStaticMeshActor>(FVector(-10000.0 + LengthM * 50.0, 0.0, -50.0), FRotator::ZeroRotator);
			if (Floor)
			{
				UStaticMeshComponent* Component = Floor->GetStaticMeshComponent();
				// A spawned static actor refuses a mesh until it is movable; it goes back to static after.
				Component->SetMobility(EComponentMobility::Movable);
				Component->SetStaticMesh(Cube);
				Component->SetMobility(EComponentMobility::Static);
				Floor->SetActorScale3D(FVector(LengthM, WidthM, 1.0));
				Floor->SetActorLabel(TEXT("Floor"));
			}
			if (ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector(0.0, 0.0, 500.0), FRotator(-50.0, 30.0, 0.0)))
			{
				Sun->SetActorLabel(TEXT("Sun"));
			}
			if (ASkyLight* Sky = World->SpawnActor<ASkyLight>(FVector(0.0, 0.0, 600.0), FRotator::ZeroRotator))
			{
				Sky->SetActorLabel(TEXT("Sky"));
			}
			if (APlayerStart* Start = World->SpawnActor<APlayerStart>(FVector(-800.0, 0.0, 100.0), FRotator::ZeroRotator))
			{
				Start->SetActorLabel(TEXT("Player Start"));
			}
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AMantleVehicleSpawnPoint* Spawn = World->SpawnActor<AMantleVehicleSpawnPoint>(FVector(0.0, 0.0, 50.0), FRotator::ZeroRotator, Params);
			if (!Spawn)
			{
				UE_LOG(LogTemp, Error, TEXT("MANTLE_VEHICLE_TEST_LEVEL ok=0 the vehicle spawn could not be added"));
				return;
			}
			Spawn->Vehicle = Source;
			Spawn->SetActorLabel(FString::Printf(TEXT("%s spawn"), *Source->GetName()));
			const bool bSaved = UEditorLoadingAndSavingUtils::SaveMap(World, Package);
			UE_LOG(LogTemp, Display, TEXT("MANTLE_VEHICLE_TEST_LEVEL ok=%d level=%s vehicle=%s floor_m=%.0fx%.0f"), bSaved ? 1 : 0, *Package, *Source->GetPathName(), LengthM, WidthM);
		}));
}
