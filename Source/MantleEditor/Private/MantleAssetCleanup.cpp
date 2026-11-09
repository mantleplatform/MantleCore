// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAssetCleanup.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	/** Out of the package, into the transient package, and garbage; with bTellRegistry, out of the registry as a deleted asset too. */
	void MoveOne(UObject* Object, const bool bTellRegistry)
	{
		if (bTellRegistry)
		{
			FAssetRegistryModule::AssetDeleted(Object);
		}
		Object->ClearFlags(RF_Public | RF_Standalone);
		Object->Rename(*MakeUniqueObjectName(GetTransientPackage(), Object->GetClass(), Object->GetFName()).ToString(),
			GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
		Object->MarkAsGarbage();
	}
}

int32 FMantleAssetCleanup::MoveAside(const TArray<UObject*>& Objects, const bool bDeleteFiles)
{
	TArray<UPackage*> Packages;
	int32 Moved = 0;
	for (UObject* Object : Objects)
	{
		if (!Object)
		{
			continue;
		}
		if (UPackage* Package = Object->GetOutermost(); Package && Package != GetTransientPackage())
		{
			Packages.AddUnique(Package);
		}
		// With the files going too, the registry hears of it once per package below. AssetDeleted would mark the package empty
		// ("deleted in editor"), and the change a save just before made reaches the registry a frame later, after that mark: it
		// then warns "package was marked as deleted in editor, but has been modified on disk" (AssetRegistry.cpp) and returns
		// the package from queries again until the file's removal arrives too.
		MoveOne(Object, /*bTellRegistry*/ !bDeleteFiles);
		++Moved;
	}
	for (UPackage* Package : Packages)
	{
		ResetLoaders(Package);
		if (bDeleteFiles)
		{
			const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
			if (IFileManager::Get().FileExists(*File))
			{
				IFileManager::Get().Delete(*File, false, true, true);
			}
			// The package is gone from disk and memory: its registry data goes, with no empty-package mark for a late change to trip.
			FAssetRegistryModule::PackageDeleted(Package);
		}
	}
	return Moved;
}

bool FMantleAssetCleanup::UnloadPackagesClean(const TArray<UPackage*>& Packages, FText& OutError)
{
	for (UPackage* Package : Packages)
	{
		if (!Package)
		{
			continue;
		}
		TArray<UObject*> Contained;
		GetObjectsWithPackage(Package, Contained, EGetObjectsFlags::None);
		for (UObject* Object : Contained)
		{
			// The file stays and a reload reads it, so the registry keeps what it found on disk: nothing is deleted.
			MoveOne(Object, /*bTellRegistry*/ false);
		}
		ResetLoaders(Package);
		Package->ClearFlags(RF_Public | RF_Standalone);
		Package->MarkAsGarbage();
	}
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	OutError = FText::GetEmpty();
	return true;
}
