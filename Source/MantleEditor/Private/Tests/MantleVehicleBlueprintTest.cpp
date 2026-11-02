// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAssetCleanup.h"
#include "MantleVehicleBlueprint.h"
#include "MantleVehicleBuilder.h"
#include "MantleVehicleSource.h"

#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

// A vehicle Blueprint as the builder's source (BuildAny): a Chaos vehicle from a Fab pack, and a Blueprint made of several static pieces. The Blueprints are the
// ones in the MantleVehicles scratch project; a project without them skips, and says so.

namespace MantleVehicleBlueprintTestPrivate
{
	const TCHAR* const ChaosBlueprint = TEXT("/Game/VehicleVarietyPack/Blueprints/Hatchback/BP_Hatchback.BP_Hatchback");
	const TCHAR* const ShipBlueprint = TEXT("/Game/StarSparrow/Blueprints/BP_ModularStarSparrow01.BP_ModularStarSparrow01");
	const TCHAR* const TestFolder = TEXT("/Game/MantleVehicles/_AutomationTest_Blueprint");

	UBlueprint* Load(FAutomationTestBase& Test, const TCHAR* Path)
	{
		if (!FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(FString(Path))))
		{
			Test.AddWarning(FString::Printf(TEXT("SKIPPED, not passed: %s is not in this project."), Path));
			return nullptr;
		}
		return LoadObject<UBlueprint>(nullptr, Path);
	}

	void Say(FAutomationTestBase& Test, const FMantleVehicleBuildResult& Result)
	{
		for (const FString& Problem : Result.Problems)
		{
			Test.AddInfo(TEXT("Problem: ") + Problem);
		}
		for (const FString& Note : Result.Notes)
		{
			Test.AddInfo(TEXT("Note: ") + Note);
		}
	}

	/** Takes away what a build made: the vehicle, and any mesh it saved beside it. */
	void Clean(const FMantleVehicleBuildResult& Result, const TArray<UObject*>& Extra)
	{
		TArray<UObject*> Cleanup = Extra;
		if (Result.Source)
		{
			Cleanup.Add(Result.Source);
		}
		Cleanup.RemoveAll([](UObject* Object) { return Object == nullptr; });
		if (Cleanup.Num() > 0)
		{
			FMantleAssetCleanup::MoveAside(Cleanup);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBlueprintChaosTest,
	"Mantle.Vehicle.Blueprint.ChaosVehicleBuildsThroughBuildAny",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBlueprintChaosTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBlueprintTestPrivate;
	UBlueprint* Blueprint = Load(*this, ChaosBlueprint);
	if (!Blueprint)
	{
		return true;
	}

	// The Blueprint gives its skeletal body, not a merge.
	FMantleVehicleBlueprintModel Read = FMantleVehicleBlueprint::Resolve(Blueprint, FString(), /*bSave*/ false);
	TestTrue(TEXT("a Chaos vehicle Blueprint gives its skeletal mesh"), Read.Model && Read.Model->IsA<USkeletalMesh>());
	TestFalse(TEXT("and is not merged"), Read.bMerged);
	TestTrue(TEXT("and carries the wheel setups read off the Blueprint"), Read.Around.IsSet() && Read.Around->WheelSetups.Num() >= 2);

	FMantleVehicleModelOptions Options;
	Options.Folder = TestFolder;
	const FMantleVehicleBuildResult Result = FMantleVehicleBuilder::BuildAny(Blueprint, TEXT("sedan"), Options);
	Say(*this, Result);
	TestTrue(TEXT("the Blueprint builds through BuildAny"), Result.bOk && Result.Source != nullptr);
	if (Result.Source)
	{
		TestTrue(TEXT("the vehicle has wheels"), Result.Source->Wheels.Num() >= 4);
		TestFalse(TEXT("and a body"), Result.Source->Body.IsNull());
		TestEqual(TEXT("of the type picked"), Result.Source->Preset, FName(TEXT("sedan")));
	}
	Clean(Result, TArray<UObject*>());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleVehicleBlueprintMergeTest,
	"Mantle.Vehicle.Blueprint.StaticPiecesMergeThroughBuildAny",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleVehicleBlueprintMergeTest::RunTest(const FString& Parameters)
{
	using namespace MantleVehicleBlueprintTestPrivate;
	UBlueprint* Blueprint = Load(*this, ShipBlueprint);
	if (!Blueprint)
	{
		return true;
	}

	// The pieces become one Static Mesh that keeps the pieces' materials.
	FMantleVehicleBlueprintModel Read = FMantleVehicleBlueprint::Resolve(Blueprint, FString(TestFolder) / TEXT("Merged"), /*bSave*/ false);
	UStaticMesh* Merged = Cast<UStaticMesh>(Read.Model);
	TestTrue(TEXT("a Blueprint of static pieces gives one Static Mesh"), Merged != nullptr && Read.bMerged);
	if (Merged)
	{
		TestTrue(TEXT("with triangles in it"), Merged->GetNumTriangles(0) > 0);
		TestTrue(TEXT("and at least one material"), Merged->GetStaticMaterials().Num() >= 1);
		TestFalse(TEXT("and no empty material slot"), Merged->GetStaticMaterials().ContainsByPredicate([](const FStaticMaterial& Slot) { return Slot.MaterialInterface == nullptr; }));
	}
	TestTrue(TEXT("the creator is told that moving pieces become one solid piece"), Read.Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("solid piece")); }));
	FMantleAssetCleanup::MoveAside(Read.MadeAssets);

	// And through BuildAny to a built vehicle.
	FMantleVehicleModelOptions Options;
	Options.Folder = TestFolder;
	const FMantleVehicleBuildResult Result = FMantleVehicleBuilder::BuildAny(Blueprint, TEXT("jet"), Options);
	Say(*this, Result);
	TestTrue(TEXT("the ship builds through BuildAny"), Result.bOk && Result.Source != nullptr);
	if (Result.Source)
	{
		TestFalse(TEXT("with a body"), Result.Source->Body.IsNull());
		TestEqual(TEXT("of the type picked"), Result.Source->Preset, FName(TEXT("jet")));
	}
	TestTrue(TEXT("and says the pieces were merged"), Result.Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("merged them into one model")); }));

	// Leave the project as it was: the merged mesh BuildAny saved goes away too.
	TArray<UObject*> Extra;
	Extra.Add(FindObject<UStaticMesh>(nullptr, *(FMantleVehicleBlueprint::MergeFolder() / TEXT("BP_ModularStarSparrow01/SM_BP_ModularStarSparrow01.SM_BP_ModularStarSparrow01"))));
	Clean(Result, Extra);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
