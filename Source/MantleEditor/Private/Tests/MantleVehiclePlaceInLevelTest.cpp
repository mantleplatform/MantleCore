// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleBuilder.h"
#include "MantleVehicleSource.h"
#include "MantleVehicleSpawnPoint.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehiclePlaceInLevelTest,
	"Mantle.Vehicle.Builder.PlaceInLevel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehiclePlaceInLevelTest::RunTest(const FString& Parameters)
{
	UMantleVehicleSource* Source = NewObject<UMantleVehicleSource>(GetTransientPackage(), NAME_None, RF_Transient);
	if (!TestNotNull(TEXT("the test vehicle exists"), Source))
	{
		return false;
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World || !World->PersistentLevel)
	{
		// A headless runner may have no editor world; the creator is then told to open a level.
		AddWarning(TEXT("PARTLY SKIPPED: there is no editor world in this runner; only the no-level message is checked, not placing in a level."));
		FString Problem;
		TestFalse(TEXT("placing with no level open fails"), FMantleVehicleBuilder::PlaceInLevel(*Source, Problem));
		TestEqual(TEXT("it says to open a level"), Problem, FString(TEXT("Open the level you want to drive in first.")));
		return true;
	}

	int32 Before = 0;
	for (TActorIterator<AMantleVehicleSpawnPoint> It(World); It; ++It)
	{
		++Before;
	}

	FString Problem;
	AMantleVehicleSpawnPoint* Spawn = nullptr;
	const bool bPlaced = FMantleVehicleBuilder::PlaceInLevel(*Source, Problem, &Spawn);
	TestTrue(TEXT("placing in an open level works"), bPlaced);
	TestTrue(TEXT("a working placement has no problem to report"), Problem.IsEmpty());

	int32 After = 0;
	for (TActorIterator<AMantleVehicleSpawnPoint> It(World); It; ++It)
	{
		++After;
	}
	TestEqual(TEXT("exactly one spawn was added"), After, Before + 1);
	if (TestNotNull(TEXT("the spawn is returned"), Spawn))
	{
		TestEqual(TEXT("the spawn holds the vehicle"), Spawn->Vehicle.Get(), Source);
		TestTrue(TEXT("the spawn is in the editor world"), Spawn->GetWorld() == World);

		// Leave the level as it was: the test's spawn goes away again.
		if (GEditor)
		{
			GEditor->SelectActor(Spawn, /*bInSelected*/ false, /*bNotify*/ false);
		}
		World->EditorDestroyActor(Spawn, /*bShouldModifyLevel*/ true);
	}
	return true;
}

#endif
