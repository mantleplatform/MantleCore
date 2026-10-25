// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAssetCleanup.h"
#include "MantleVehicleSource.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

// The shared cleanup used in place of the engine's delete and unload, which ensure on the typed element registry in -nullrhi runs.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleAssetCleanupTestPrivate
{
	/** A small saved package with one asset in it; the file goes away with the object. */
	struct FSavedFixture
	{
		FString PackageName = TEXT("/Game/MantleAutomation/_AssetCleanupTest/Saved");
		FString File = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());

		~FSavedFixture()
		{
			IFileManager::Get().Delete(*File, false, true, true);
			IFileManager::Get().DeleteDirectory(*FPaths::GetPath(File), false, true);
		}

		UMantleVehicleSource* Save(const FName Name)
		{
			UPackage* Package = CreatePackage(*PackageName);
			UMantleVehicleSource* Asset = NewObject<UMantleVehicleSource>(Package, Name, RF_Public | RF_Standalone);
			Package->MarkPackageDirty();
			FAssetRegistryModule::AssetCreated(Asset);
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			Args.SaveFlags = SAVE_NoError;
			return UPackage::SavePackage(Package, Asset, *File, Args) ? Asset : nullptr;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleAssetCleanupTest,
	"Mantle.Editor.AssetCleanup.UnloadReadsTheFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FMantleAssetCleanupTest::RunTest(const FString& Parameters)
{
	using namespace MantleAssetCleanupTestPrivate;

	FSavedFixture Fixture;
	UMantleVehicleSource* Saved = Fixture.Save(TEXT("CleanupAsset"));
	if (!TestNotNull(TEXT("the fixture saves"), Saved))
	{
		return false;
	}
	UPackage* Package = Saved->GetOutermost();

	FText Error;
	TestTrue(TEXT("the saved package unloads"), FMantleAssetCleanup::UnloadPackagesClean({ Package }, Error));
	TestTrue(TEXT("the file is kept"), IFileManager::Get().FileExists(*Fixture.File));
	TestNull(TEXT("nothing of it is left in memory"), FindObject<UMantleVehicleSource>(nullptr, *(Fixture.PackageName + TEXT(".CleanupAsset"))));

	UPackage* Reloaded = LoadPackage(nullptr, *Fixture.PackageName, LOAD_None);
	UMantleVehicleSource* ReloadedAsset = Reloaded ? FindObject<UMantleVehicleSource>(Reloaded, TEXT("CleanupAsset")) : nullptr;
	if (TestNotNull(TEXT("a reload reads the file"), ReloadedAsset))
	{
		// MoveAside takes the asset out of its package and removes the file, as a delete does.
		TestEqual(TEXT("one asset is moved aside"), FMantleAssetCleanup::MoveAside({ ReloadedAsset, nullptr }), 1);
		TestFalse(TEXT("and its file is gone"), IFileManager::Get().FileExists(*Fixture.File));
		TestEqual(TEXT("the asset left its package"), ReloadedAsset->GetOutermost(), GetTransientPackage());
	}
	return true;
}

#endif
