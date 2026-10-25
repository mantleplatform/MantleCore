// Copyright Joshua Eger. All Rights Reserved.
#include "Graph/MantleScriptGraphEditorToolkit.h"

#include "Graph/MantleScriptGraphAsset.h"
#include "Graph/MantleScriptEdGraph.h"
#include "Graph/MantleScriptGraphNode.h"
#include "Graph/MantleScriptGraphSchema.h"
#include "Graph/MantleScriptGraphSerializer.h"
#include "MantleRuntime.h"

#include "DetailsViewArgs.h"
#include "Editor.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "IDetailsView.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Styling/AppStyle.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "MantleScriptGraphEditor"

const FName FMantleScriptGraphEditorToolkit::GraphTabId(TEXT("MantleScriptGraphEditor_Graph"));
const FName FMantleScriptGraphEditorToolkit::DetailsTabId(TEXT("MantleScriptGraphEditor_Details"));
const FName FMantleScriptGraphEditorToolkit::MyBlueprintTabId(TEXT("MantleScriptGraphEditor_MyBlueprint"));
const FName FMantleScriptGraphEditorToolkit::CompilerResultsTabId(TEXT("MantleScriptGraphEditor_CompilerResults"));

void FMantleScriptGraphEditorToolkit::OpenGraphAsset(UMantleScriptGraph& Graph)
{
	TSharedRef<FMantleScriptGraphEditorToolkit> Toolkit = MakeShared<FMantleScriptGraphEditorToolkit>();
	Toolkit->InitEditor({&Graph});
}

void FMantleScriptGraphEditorToolkit::InitEditor(const TArray<UObject*>& ObjectsToEdit)
{
	GraphAsset = ObjectsToEdit.Num() > 0 ? Cast<UMantleScriptGraph>(ObjectsToEdit[0]) : nullptr;
	check(GraphAsset);
	GraphAsset->EnsureEdGraph();
	EdGraph = CastChecked<UMantleScriptEdGraph>(GraphAsset->EdGraph);
	if (!EdGraph->Schema)
	{
		EdGraph->Schema = UMantleScriptGraphSchema::StaticClass();
	}

	const TSharedRef<FTabManager::FLayout> Layout = FTabManager::NewLayout("MantleScriptGraphEditor_Layout_v1")
		->AddArea(
			FTabManager::NewPrimaryArea()->SetOrientation(Orient_Horizontal)
			->Split(
				FTabManager::NewSplitter()->SetOrientation(Orient_Vertical)->SetSizeCoefficient(0.22f)
				->Split(FTabManager::NewStack()->AddTab(MyBlueprintTabId, ETabState::OpenedTab)->SetSizeCoefficient(0.5f))
				->Split(FTabManager::NewStack()->AddTab(DetailsTabId, ETabState::OpenedTab)->SetSizeCoefficient(0.5f))
			)
			->Split(
				FTabManager::NewSplitter()->SetOrientation(Orient_Vertical)->SetSizeCoefficient(0.78f)
				->Split(FTabManager::NewStack()->AddTab(GraphTabId, ETabState::OpenedTab)->SetSizeCoefficient(0.75f))
				->Split(FTabManager::NewStack()->AddTab(CompilerResultsTabId, ETabState::OpenedTab)->SetSizeCoefficient(0.25f))
			)
		);

	InitAssetEditor(EToolkitMode::Standalone, TSharedPtr<IToolkitHost>(), TEXT("MantleScriptGraphEditor"), Layout, true, true, ObjectsToEdit);

	// A freshly-created (never-compiled) graph shows a first compile result right away, matching
	// "compile on save" -- there's no reason the very first open should look blank.
	Compile();
}

FName FMantleScriptGraphEditorToolkit::GetToolkitFName() const { return FName(TEXT("MantleScriptGraphEditor")); }
FText FMantleScriptGraphEditorToolkit::GetBaseToolkitName() const { return LOCTEXT("ToolkitName", "Mantle Script Graph Editor"); }
FString FMantleScriptGraphEditorToolkit::GetWorldCentricTabPrefix() const { return TEXT("MantleScriptGraph "); }
FLinearColor FMantleScriptGraphEditorToolkit::GetWorldCentricTabColorScale() const { return FLinearColor(0.7f, 0.07f, 0.1f); }

void FMantleScriptGraphEditorToolkit::RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);

	const TSharedRef<FWorkspaceItem> Category = InTabManager->AddLocalWorkspaceMenuCategory(LOCTEXT("WorkspaceMenu", "Mantle Script Graph"));

	InTabManager->RegisterTabSpawner(GraphTabId, FOnSpawnTab::CreateSP(this, &FMantleScriptGraphEditorToolkit::SpawnGraphTab))
		.SetDisplayName(LOCTEXT("GraphTab", "Graph")).SetGroup(Category);
	InTabManager->RegisterTabSpawner(DetailsTabId, FOnSpawnTab::CreateSP(this, &FMantleScriptGraphEditorToolkit::SpawnDetailsTab))
		.SetDisplayName(LOCTEXT("DetailsTab", "Details")).SetGroup(Category);
	InTabManager->RegisterTabSpawner(MyBlueprintTabId, FOnSpawnTab::CreateSP(this, &FMantleScriptGraphEditorToolkit::SpawnMyBlueprintTab))
		.SetDisplayName(LOCTEXT("MyBlueprintTab", "My Blueprint")).SetGroup(Category);
	InTabManager->RegisterTabSpawner(CompilerResultsTabId, FOnSpawnTab::CreateSP(this, &FMantleScriptGraphEditorToolkit::SpawnCompilerResultsTab))
		.SetDisplayName(LOCTEXT("CompilerResultsTab", "Compiler Results")).SetGroup(Category);
}

void FMantleScriptGraphEditorToolkit::UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);
	InTabManager->UnregisterTabSpawner(GraphTabId);
	InTabManager->UnregisterTabSpawner(DetailsTabId);
	InTabManager->UnregisterTabSpawner(MyBlueprintTabId);
	InTabManager->UnregisterTabSpawner(CompilerResultsTabId);
}

TSharedRef<SGraphEditor> FMantleScriptGraphEditorToolkit::CreateGraphEditorWidget()
{
	SGraphEditor::FGraphEditorEvents Events;
	Events.OnSelectionChanged = SGraphEditor::FOnSelectionChanged::CreateSP(this, &FMantleScriptGraphEditorToolkit::OnSelectionChanged);

	FGraphAppearanceInfo Appearance;
	Appearance.CornerText = LOCTEXT("AppearanceCorner", "MANTLE SCRIPT GRAPH");

	GraphEditorWidget = SNew(SGraphEditor)
		.AdditionalCommands(GetToolkitCommands())
		.IsEditable(true)
		.Appearance(Appearance)
		.GraphToEdit(EdGraph)
		.GraphEvents(Events)
		.AutoExpandActionMenu(true);
	return GraphEditorWidget.ToSharedRef();
}

TSharedRef<SDockTab> FMantleScriptGraphEditorToolkit::SpawnGraphTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("GraphTabLabel", "Graph"))
		[
			CreateGraphEditorWidget()
		];
}

TSharedRef<SDockTab> FMantleScriptGraphEditorToolkit::SpawnDetailsTab(const FSpawnTabArgs& Args)
{
	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	FDetailsViewArgs DetailsArgs;
	DetailsArgs.bAllowSearch = false;
	DetailsArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	NodeDetailsView = PropertyModule.CreateDetailView(DetailsArgs);

	return SNew(SDockTab)
		.Label(LOCTEXT("DetailsTabLabel", "Details"))
		[
			NodeDetailsView.ToSharedRef()
		];
}

TSharedRef<SDockTab> FMantleScriptGraphEditorToolkit::SpawnMyBlueprintTab(const FSpawnTabArgs& Args)
{
	// Variables/custom events/functions are plain UPROPERTY arrays on UMantleScriptEdGraph, so
	// pointing an ordinary details view at it gives add/remove/rename for free -- a My Blueprint
	// panel without a second, hand-rolled editing UI to keep in sync with the schema's palette
	// (MantleScriptGraphSchema.cpp reads these same arrays to build the "Get X"/"Set X"/etc actions).
	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	FDetailsViewArgs DetailsArgs;
	DetailsArgs.bAllowSearch = false;
	DetailsArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	MyBlueprintView = PropertyModule.CreateDetailView(DetailsArgs);
	MyBlueprintView->SetObject(EdGraph);

	return SNew(SDockTab)
		.Label(LOCTEXT("MyBlueprintTabLabel", "My Blueprint"))
		[
			MyBlueprintView.ToSharedRef()
		];
}

TSharedRef<SDockTab> FMantleScriptGraphEditorToolkit::SpawnCompilerResultsTab(const FSpawnTabArgs& Args)
{
	SAssignNew(CompilerResultsList, SListView<TSharedPtr<FMantleGraphCompilerResultRow>>)
		.ListItemsSource(&CompilerResultRows)
		.OnGenerateRow(this, &FMantleScriptGraphEditorToolkit::OnGenerateCompilerResultRow)
		.OnMouseButtonDoubleClick(this, &FMantleScriptGraphEditorToolkit::OnCompilerResultDoubleClicked)
		.SelectionMode(ESelectionMode::Single);

	return SNew(SDockTab)
		.Label(LOCTEXT("CompilerResultsTabLabel", "Compiler Results"))
		[
			CompilerResultsList.ToSharedRef()
		];
}

TSharedRef<ITableRow> FMantleScriptGraphEditorToolkit::OnGenerateCompilerResultRow(TSharedPtr<FMantleGraphCompilerResultRow> Row, const TSharedRef<STableViewBase>& Owner)
{
	const TSharedRef<SWidget> Content = !Row->NodeId.IsEmpty()
		? StaticCastSharedRef<SWidget>(SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.OnClicked_Lambda([this, NodeId = Row->NodeId]()
			{
				FocusNodeById(NodeId);
				return FReply::Handled();
			})
			[
				SNew(STextBlock)
				.Text(FText::FromString(Row->Text))
				.ColorAndOpacity(Row->bError ? FLinearColor(1.0f, 0.35f, 0.3f) : FLinearColor(0.4f, 0.9f, 0.4f))
			])
		: StaticCastSharedRef<SWidget>(SNew(STextBlock)
			.Text(FText::FromString(Row->Text))
			.ColorAndOpacity(Row->bError ? FLinearColor(1.0f, 0.35f, 0.3f) : FLinearColor(0.4f, 0.9f, 0.4f)));
	return SNew(STableRow<TSharedPtr<FMantleGraphCompilerResultRow>>, Owner)
		[
			Content
		];
}

void FMantleScriptGraphEditorToolkit::OnCompilerResultDoubleClicked(TSharedPtr<FMantleGraphCompilerResultRow> Row)
{
	if (Row.IsValid() && !Row->NodeId.IsEmpty())
	{
		FocusNodeById(Row->NodeId);
	}
}

void FMantleScriptGraphEditorToolkit::FocusNodeById(const FString& NodeId)
{
	if (!EdGraph || !GraphEditorWidget.IsValid())
	{
		return;
	}
	for (UEdGraphNode* N : EdGraph->Nodes)
	{
		if (const UMantleScriptGraphNode* MN = Cast<UMantleScriptGraphNode>(N))
		{
			if (MN->NodeGuid.ToString(EGuidFormats::Digits) == NodeId)
			{
				GraphEditorWidget->ClearSelectionSet();
				GraphEditorWidget->SetNodeSelection(N, true);
				GraphEditorWidget->JumpToNode(N, false, true);
				return;
			}
		}
	}
}

void FMantleScriptGraphEditorToolkit::OnSelectionChanged(const FGraphPanelSelectionSet& NewSelection)
{
	if (!NodeDetailsView.IsValid())
	{
		return;
	}
	TArray<UObject*> Selected;
	for (UObject* Obj : NewSelection)
	{
		Selected.Add(Obj);
	}
	NodeDetailsView->SetObjects(Selected);
}

void FMantleScriptGraphEditorToolkit::RebuildCompilerResultsList(const FMantleGraphCompileResult& Result, const FString& ErrorNodeId)
{
	CompilerResultRows.Reset();
	if (Result.bSuccess)
	{
		TSharedPtr<FMantleGraphCompilerResultRow> Row = MakeShared<FMantleGraphCompilerResultRow>();
		Row->Text = FString::Printf(TEXT("Compiled to Luau (%d line%s)."), Result.LineMap.Num(), Result.LineMap.Num() == 1 ? TEXT("") : TEXT("s"));
		Row->bError = false;
		CompilerResultRows.Add(Row);
	}
	else
	{
		TSharedPtr<FMantleGraphCompilerResultRow> Row = MakeShared<FMantleGraphCompilerResultRow>();
		Row->Text = Result.Error;
		Row->bError = true;
		Row->NodeId = ErrorNodeId;
		CompilerResultRows.Add(Row);
	}
	if (CompilerResultsList.IsValid())
	{
		CompilerResultsList->RequestListRefresh();
	}
}

FMantleGraphCompileResult FMantleScriptGraphEditorToolkit::Compile()
{
	FMantleGraphCompileResult Result;
	if (!EdGraph || !GraphAsset)
	{
		Result.Error = TEXT("no graph to compile");
		return Result;
	}

	EMantleSide Side;
	FString FolderWarning;
	const bool bClassified = FMantleScriptGraphSerializer::ClassifySide(GraphAsset->GetOutermost()->GetName(), Side, FolderWarning);

	FMantleGraph Graph;
	FString BuildError;
	const FString GraphName = GraphAsset->GetName();
	if (!FMantleScriptGraphSerializer::BuildGraph(*EdGraph, GraphName, Side, Graph, BuildError))
	{
		Result.bSuccess = false;
		Result.Error = BuildError;
	}
	else
	{
		Result = FMantleGraphCompiler::Compile(Graph);
	}

	const FString ErrorNodeId = FMantleScriptGraphSerializer::ApplyCompileResult(*EdGraph, Result);
	if (GraphEditorWidget.IsValid())
	{
		GraphEditorWidget->NotifyGraphChanged();
	}

	if (!bClassified)
	{
		// The folder problem always wins over whatever the compiler itself says -- Side was only a
		// best-effort fallback (Server) so BuildGraph/Compile had something to run against.
		Result.bSuccess = false;
		Result.Error = FolderWarning;
		RebuildCompilerResultsList(Result, FString());
		return Result;
	}

	RebuildCompilerResultsList(Result, ErrorNodeId);

	if (Result.bSuccess)
	{
		const FString LuauPath = FMantleScriptGraphSerializer::CompiledLuauPathForAsset(GraphAsset->GetOutermost()->GetName());
		if (!LuauPath.IsEmpty())
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(LuauPath), true);
			if (!FFileHelper::SaveStringToFile(Result.Luau, *LuauPath))
			{
				UE_LOG(LogMantle, Warning, TEXT("Mantle Script Graph: compiled '%s' but could not write '%s'"), *GraphName, *LuauPath);
			}
		}
	}

	return Result;
}

void FMantleScriptGraphEditorToolkit::SaveAsset_Execute()
{
	Compile();
	FAssetEditorToolkit::SaveAsset_Execute();
}

#undef LOCTEXT_NAMESPACE
