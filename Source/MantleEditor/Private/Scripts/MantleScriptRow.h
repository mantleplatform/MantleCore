// Copyright Joshua Eger. All Rights Reserved.
// One row of the Scripts panel: a file discovered beside the current level's Scripts/
// folder, plus whatever Check All has told us about it.
//
// Language classification goes through MantleScriptLanguageFromExtension (MantleScriptCatalog.h)
// so the panel agrees with what the game's disk catalogue source (FMantleDiskScriptCatalogSource) would
// classify the same file as.

#pragma once

#include "CoreMinimal.h"
#include "MantleScriptCatalogFiles.h"
#include "MantleScriptSide.h"

/** Superset of EMantleScriptCatalogLanguage that also covers script graphs, for editor-only purposes
 *  (New Script templates, Check All). */
enum class EMantleScriptEditorLanguage : uint8
{
	Unknown,
	Luau,
	JavaScript,
	TypeScript,
	Python,
	/** A UMantleScriptGraph asset (MantleEditor/Public/Graph/MantleScriptGraphAsset.h).
	 *  Never produced by MantleScriptEditorLanguageFromExtension -- a graph is a .uasset, not a
	 *  plain text file, and is discovered separately, by asset class, in
	 *  FMantleScriptDiscovery::DiscoverAt. */
	Graph
};

/** Classifies RelativePath's extension: MantleScriptLanguageFromExtension first (Luau/JS/TS,
 *  case-sensitive extensions, same as the runtime), then a local ".py" check for Python. */
MANTLEEDITOR_API EMantleScriptEditorLanguage MantleScriptEditorLanguageFromExtension(const FString& Extension);

/** Which Scripts/ subfolder a script lives in on disk. A file lives in exactly one of these,
 *  unlike EMantleSide's "Both", which is a script's ability to run on either side at runtime. A
 *  script folder's Side is EMantleSide::Both for Shared (see MantleScriptCatalog.cpp's
 *  ClassifyPath) -- rows reuse that convention directly rather than inventing a fourth value. */
using EMantleScriptRowSide = EMantleSide;

/** One discovered file under the current level's Scripts/ root. */
struct FMantleScriptRow
{
	/** Path relative to the Scripts/ root, forward slashes, exact on-disk case:
	 *  "Server/Modules/Util.luau". */
	FString RelativePath;
	/** Absolute path on disk. */
	FString FullPath;
	EMantleScriptRowSide Side = EMantleSide::Server;
	EMantleScriptEditorLanguage Language = EMantleScriptEditorLanguage::Unknown;
	/** Scripts anywhere under Shared/, and any folder named exactly "Modules" at any depth --
	 *  mirrors MantleScriptCatalog.cpp's ClassifyPath. */
	bool bModule = false;
	FDateTime Timestamp;

	/** Language == Graph only: the UMantleScriptGraph asset's package name ("/Game/.../Scripts/
	 *  Server/MyGraph"), so Open and Check All can load it without re-deriving the path from
	 *  FullPath. Empty for every other language. */
	FString AssetPackageName;

	// Check All -------------------------------------------------------------------------------
	bool bChecked = false;
	bool bCheckOk = false;
	/** True when the language cannot be checked (unknown extension). Distinct from a real
	 *  failure: shown neutrally, not counted as an error. */
	bool bCheckSkipped = false;
	FString CheckMessage;
};

using FMantleScriptRowPtr = TSharedPtr<FMantleScriptRow>;
