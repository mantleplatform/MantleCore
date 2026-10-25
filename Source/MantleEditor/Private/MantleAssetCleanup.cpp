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
	/** Out of the registry and the package, into the transient package, and garbage. */
	void MoveOne(UObject* Object)
	{
		FAssetRegistryModule::AssetDeleted(Object);
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
		MoveOne(Object);
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
			MoveOne(Object);
		}
		ResetLoaders(Package);
		Package->ClearFlags(RF_Public | RF_Standalone);
		Package->MarkAsGarbage();
	}
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	OutError = FText::GetEmpty();
	return true;
}
