// Copyright Joshua Eger. All Rights Reserved.
// Converts between the editable UMantleScriptEdGraph and the JSON graph model
// (FMantleGraph), and applies a compile result back onto the nodes. Kept free of any Slate
// dependency so it is unit-testable headlessly (see the automation tests under
// MantleEditor/Private/Graph/Tests) and reusable by the Scripts panel's Check All without
// spinning up the graph editor UI.
#pragma once

#include "CoreMinimal.h"
#include "Graph/MantleGraphCompiler.h"
#include "Graph/MantleGraphTypes.h"
#include "MantleScriptSide.h"

class UMantleScriptEdGraph;

class MANTLEEDITOR_API FMantleScriptGraphSerializer
{
public:
	/** Walks EdGraph's nodes and pin links into a parsed FMantleGraph ready for
	 *  FMantleGraphCompiler::Compile. GraphName becomes FMantleGraph::Name; Side is normally the
	 *  result of ClassifySide on the owning asset's package path. Fails (returns false, fills
	 *  OutError) only for a structural problem the compiler could never explain on its own, e.g. a
	 *  node this build doesn't recognise; per-node semantic problems (a missing input, a wrong-side
	 *  member, ...) are left for the compiler to report by design, so there is exactly one place
	 *  that explains why a graph doesn't compile. */
	static bool BuildGraph(const UMantleScriptEdGraph& EdGraph, const FString& GraphName, EMantleSide Side, FMantleGraph& OutGraph, FString& OutError);

	/** Clears every node's compiler-message state, then, on failure, parses Result.Error's leading
	 *  "node '<id>': " (the compiler always writes failures this way when they're tied to one
	 *  node -- MantleGraphCompiler.h) and marks that node's bHasCompilerMessage/ErrorType/ErrorMsg.
	 *  Returns the NodeGuid (as ExportText, i.e. UEdGraphNode::NodeGuid.ToString()) of the node the
	 *  error was applied to, or an empty string when the error has no single owning node (a
	 *  whole-graph structural error) or the graph compiled cleanly. */
	static FString ApplyCompileResult(UMantleScriptEdGraph& EdGraph, const FMantleGraphCompileResult& Result);

	/** Classifies PackagePath (an asset's full package name, e.g.
	 *  "/Game/MyPlace/Scripts/Server/KillBricks") by its Scripts/-relative top folder, exactly like
	 *  MantleScriptDiscovery.cpp's ClassifyPath and MantlePlaceValidator.cpp's own script-folder
	 *  check. True with OutSide set for Server/Client (case-exact); false with OutWarning set to
	 *  the same wording MantlePlaceValidator uses for a script outside Server/Client/Shared
	 *  otherwise (OutSide is still filled with a Server fallback so callers that must proceed --
	 *  e.g. the palette -- have something usable).
	 *
	 *  #838: a graph inside a script package ("/Game/MyPlace/Packages/KillBricks/Server/Brick")
	 *  classifies by the folder after the package's name the same way; its compiled .luau beside it
	 *  is what the package runs and publishes.
	 */
	static bool ClassifySide(const FString& PackagePath, EMantleSide& OutSide, FString& OutWarning);

	/** The on-disk .luau path compile-on-save writes to: the same folder PackageName's own .uasset
	 *  lives in (a plain script file's Scripts/<Side>/ folder, per FMantleScriptTemplates), same
	 *  base name, ".luau" extension -- so the non-Shipping disk script catalogue
	 *  (FMantleDiskScriptCatalogSource) picks it up on its next poll with no extra step. Empty if
	 *  PackageName can't be mapped to a file on disk. */
	static FString CompiledLuauPathForAsset(const FString& PackageName);
};
