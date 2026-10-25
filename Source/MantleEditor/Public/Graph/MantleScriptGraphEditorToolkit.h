// Copyright Joshua Eger. All Rights Reserved.
// The graph editor itself -- an SGraphEditor (the same widget the Material and Niagara
// editors use) plus a My Blueprint-style panel (variables/custom events/functions, backed
// directly by UMantleScriptEdGraph's own UPROPERTY arrays so add/remove/rename come for free from
// IDetailsView), a details panel for the selected node, and a compiler results panel. Save
// compiles first (SaveAsset_Execute override): success writes the Luau next to the asset so the
// non-Shipping disk script catalogue picks it up on its next poll (no separate step -- see
// FMantleScriptGraphSerializer::CompiledLuauPathForAsset); failure marks the offending node and
// leaves the previously-compiled Luau on disk alone, then still saves the uasset itself, so a
// creator's in-progress graph is never lost to a compile error.
#pragma once

#include "CoreMinimal.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "EdGraph/EdGraph.h"
#include "GraphEditor.h"
#include "Widgets/Views/SListView.h"
#include "Graph/MantleGraphCompiler.h"

class IDetailsView;
class UMantleScriptGraph;
class UMantleScriptEdGraph;

/** One row of the compiler results list. */
struct FMantleGraphCompilerResultRow
{
	FString Text;
	/** Empty for a success row, or a whole-graph structural error with no single owning node. */
	FString NodeId;
	bool bError = false;
};

class FMantleScriptGraphEditorToolkit : public FAssetEditorToolkit
{
public:
	/** Opens (or fronts) the graph editor for Graph. Used by the Content Browser double-click
	 *  (FMantleScriptGraphActions::OpenAssetEditor) and the Scripts panel's Open button. */
	static void OpenGraphAsset(UMantleScriptGraph& Graph);
	void InitEditor(const TArray<UObject*>& ObjectsToEdit);

	//~ Begin IToolkit interface
	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
	virtual FString GetWorldCentricTabPrefix() const override;
	virtual FLinearColor GetWorldCentricTabColorScale() const override;
	virtual void RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual void UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	//~ End IToolkit interface

	//~ Begin FAssetEditorToolkit interface
	virtual void SaveAsset_Execute() override;
	//~ End FAssetEditorToolkit interface

	/** Compiles the current graph with FMantleGraphCompiler::Compile, applies the result to the
	 *  nodes and the compiler results list, and, on success, writes the Luau to disk. Returns the
	 *  compile result so callers (e.g. the Scripts panel's Check All) can report ok/fail without
	 *  duplicating this logic. */
	FMantleGraphCompileResult Compile();

private:
	TSharedRef<SDockTab> SpawnGraphTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnDetailsTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnMyBlueprintTab(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnCompilerResultsTab(const FSpawnTabArgs& Args);

	TSharedRef<SGraphEditor> CreateGraphEditorWidget();
	void OnSelectionChanged(const FGraphPanelSelectionSet& NewSelection);

	void RebuildCompilerResultsList(const FMantleGraphCompileResult& Result, const FString& ErrorNodeId);
	void OnCompilerResultDoubleClicked(TSharedPtr<FMantleGraphCompilerResultRow> Row);
	TSharedRef<class ITableRow> OnGenerateCompilerResultRow(TSharedPtr<FMantleGraphCompilerResultRow> Row, const TSharedRef<class STableViewBase>& Owner);

	void FocusNodeById(const FString& NodeId);

	UMantleScriptGraph* GraphAsset = nullptr;
	UMantleScriptEdGraph* EdGraph = nullptr;

	TSharedPtr<SGraphEditor> GraphEditorWidget;
	TSharedPtr<IDetailsView> NodeDetailsView;
	TSharedPtr<IDetailsView> MyBlueprintView;
	TSharedPtr<SListView<TSharedPtr<FMantleGraphCompilerResultRow>>> CompilerResultsList;
	TArray<TSharedPtr<FMantleGraphCompilerResultRow>> CompilerResultRows;

	static const FName GraphTabId;
	static const FName DetailsTabId;
	static const FName MyBlueprintTabId;
	static const FName CompilerResultsTabId;
};
