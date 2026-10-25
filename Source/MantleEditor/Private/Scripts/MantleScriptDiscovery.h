// Copyright Joshua Eger. All Rights Reserved.
// Editor-side "what scripts exist" for the Scripts panel and the
// Mantle.ScriptsPanel/Mantle.NewScript/Mantle.CheckScripts console commands.

#pragma once

#include "CoreMinimal.h"
#include "MantleScriptRow.h"

/**
 * Discovers script files under a level's Scripts/ folder. The root is resolved straight from
 * the currently open editor level's package path, the same computation the game's disk catalogue source (FMantleDiskScriptCatalogSource) would
 * do for a `-MantlePlace=<PlaceDir>/place.json` run one level up (Scripts/ is a sibling of the
 * place file there; here it's a sibling of the level asset).
 */
class FMantleScriptDiscovery
{
public:
	/** <folder of the currently open level>/Scripts, or an empty string if no level under /Game
	 *  is open, or its folder can't be mapped to disk. */
	static FString CurrentLevelScriptsRoot();

	/** The package-path twin of CurrentLevelScriptsRoot: "/Game/.../Scripts" for the currently
	 *  open level, or empty. UMantleScriptGraph rows are found by asset class under this
	 *  path (DiscoverAt) rather than by file extension, since a graph is a .uasset. */
	static FString CurrentLevelScriptsPackageRoot();

	/**
	 * Recursively lists every recognised script file under ScriptsRoot into OutRows (cleared
	 * first, sorted by path). Classification mirrors MantleScriptCatalog.cpp's ClassifyPath
	 * exactly (case-sensitive Server/Client/Shared top folder, a "Modules" folder at any depth,
	 * dotfiles and dead languages skipped) so the panel never lists a file the runtime's own
	 * catalogue would refuse.
	 */
	static void DiscoverAt(const FString& ScriptsRoot, TArray<FMantleScriptRowPtr>& OutRows);
};
