// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UObject;
class UPackage;

/**
 * Removes assets and unloads packages without ObjectTools::ForceDeleteObjects, ObjectTools::DeleteObjectsUnchecked or
 * UPackageTools::UnloadPackages. Those unload packages through the editor selection, which ensures "Element type ID '0' has not
 * been registered" (TypedElementRegistry.h) the first time it runs in a process whose typed element registry is not set up
 * (-nullrhi runs, CI), and that fails whichever test hits it first. Every editor path that deletes or unloads assets uses this.
 */
class MANTLEEDITOR_API FMantleAssetCleanup
{
public:
	/**
	 * Removes the assets from the registry, moves them into the transient package and marks them garbage, so a later load or
	 * create at their names starts clean. With bDeleteFiles the package files they were saved to are removed too, as a delete
	 * does. Returns how many assets were moved aside. Null entries are skipped.
	 */
	static int32 MoveAside(const TArray<UObject*>& Objects, bool bDeleteFiles = true);

	/**
	 * Unloads packages: every object in them is moved aside and collected, so a reload reads the file. Their files are kept.
	 * Returns false (with OutError) only if a package could not be unloaded.
	 */
	static bool UnloadPackagesClean(const TArray<UPackage*>& Packages, FText& OutError);
};
