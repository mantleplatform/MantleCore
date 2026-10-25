// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceWorldPartition.h"

#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// Check and Publish load the Level Instances the editor has not loaded before they export, so what is inside them publishes, and unload
// them again afterwards (GhzGarage/mantle#818). A real Level Instance needs a world asset on disk, so the load itself is exercised by
// the headless check of a level that has one; these tests hold what needs no asset: nothing is held for a level without any, a Level
// Instance load counts as held (so the publish session unloads it), and the note says how many loaded and how many would not.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLevelInstanceLoadEmptyTest,
	"Mantle.Editor.LevelInstanceLoad.LevelWithoutOnesHoldsNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLevelInstanceLoadEmptyTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleLevelInstanceLoadTestWorld"));
	if (!TestNotNull(TEXT("the world was made"), World))
	{
		return false;
	}
	FMantlePlaceWorldPartition::FLoadResult Loaded = FMantlePlaceWorldPartition::LoadAll(World);
	TestFalse(TEXT("a level with no Level Instances loads none"), Loaded.bLoaded());
	TestEqual(TEXT("and none failed"), Loaded.LevelInstancesFailed, 0);
	TestTrue(TEXT("and there is no note"), FMantlePlaceWorldPartition::DescribeLoad(Loaded).IsEmpty());
	FMantlePlaceWorldPartition::ReleaseLoaded(World, Loaded);

	World->DestroyWorld(/*bInformEngineOfWorld*/ false);
	if (World->IsRooted())
	{
		World->RemoveFromRoot();
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleLevelInstanceLoadNoteTest,
	"Mantle.Editor.LevelInstanceLoad.NoteCountsLoadedAndFailed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleLevelInstanceLoadNoteTest::RunTest(const FString& Parameters)
{
	FMantlePlaceWorldPartition::FLoadResult Loaded;
	Loaded.LevelInstances.Add(GetTransientPackage());
	TestTrue(TEXT("a loaded Level Instance counts as held, so the session unloads it"), Loaded.bLoaded());
	const FString Note = FMantlePlaceWorldPartition::DescribeLoad(Loaded);
	TestFalse(TEXT("and there is a note for it"), Note.IsEmpty());

	// One that would not load adds to the note without being held.
	FMantlePlaceWorldPartition::FLoadResult Failed;
	Failed.LevelInstancesFailed = 2;
	TestFalse(TEXT("one that would not load is not held"), Failed.bLoaded());
	TestNotEqual(TEXT("but it is said"), FMantlePlaceWorldPartition::DescribeLoad(Failed), FString());

	// With World Partition cells loaded too, one note carries both.
	Loaded.UnloadedBefore = 10;
	Loaded.CommitBeforeMB = 1000;
	Loaded.CommitAfterMB = 2000;
	TestTrue(TEXT("both parts are in one note"), FMantlePlaceWorldPartition::DescribeLoad(Loaded).Len() > Note.Len());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
