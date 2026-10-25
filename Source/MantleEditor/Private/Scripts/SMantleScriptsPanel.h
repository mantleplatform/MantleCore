// Copyright Joshua Eger. All Rights Reserved.
// The Scripts panel. It only shows what's on disk and what Check All found; there is no live host
// to ask for running status.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "MantleScriptRow.h"

class SHeaderRow;
class ITableRow;
class STableViewBase;

/**
 * "Mantle Scripts": the dockable panel showing the current level's Scripts/ folder. Server/
 * Client/Shared sections, New Script..., Check All, and buttons to open a script (or the whole
 * project) in VS Code.
 */
class SMantleScriptsPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMantleScriptsPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SMantleScriptsPanel() override;

	/** Re-scans disk immediately (used right after New Script creates a file, and on map change). */
	void RefreshNow();
	/** Re-scans disk, then selects/highlights the row for RelativePath once found. */
	void RefreshAndSelect(const FString& RelativePath);
	/** Runs Check All synchronously and logs a one-line-per-script summary (used by
	 *  Mantle.CheckScripts too, when the panel is open). */
	void CheckAll();

private:
	TSharedRef<SWidget> BuildToolbar();
	TSharedRef<SWidget> BuildSection(const FText& Title, TArray<FMantleScriptRowPtr>* RowsArray, TSharedPtr<SListView<FMantleScriptRowPtr>>& OutListView);
	TSharedRef<SHeaderRow> MakeHeaderRow() const;
	TSharedRef<ITableRow> OnGenerateRow(FMantleScriptRowPtr Row, const TSharedRef<STableViewBase>& OwnerTable);

	void RescanDisk();
	void RebuildSections();
	void OpenPath(const FString& FullPath);
	/** Language == Graph: opens the graph editor toolkit. Everything else: OpenPath, unchanged. */
	void OpenRow(const FMantleScriptRowPtr& Row);
	void OnRowDoubleClicked(FMantleScriptRowPtr Row);

	FReply OnNewScriptClicked();
	FReply OnOpenSelectedClicked();
	FReply OnOpenScriptsFolderClicked();
	FReply OnOpenVsCodeWholeProjectClicked();
	FReply OnCheckAllClicked();
	FReply OnRegenerateTypesClicked();

	FText GetScriptsRootText() const;
	EActiveTimerReturnType OnPollTimer(double InCurrentTime, float InDeltaTime);
	void OnMapChanged(uint32 MapChangeFlags);
	void OnMapOpened(const FString& Filename, bool bAsTemplate);

	FString ScriptsRoot;
	TArray<FMantleScriptRowPtr> AllRows; // every discovered row; section arrays below are filtered views
	TArray<FMantleScriptRowPtr> ServerRows, ClientRows, SharedRows;
	TSharedPtr<SListView<FMantleScriptRowPtr>> ServerList, ClientList, SharedList;

	TMap<FString, FDateTime> LastSeenTimestamps; // disk-change detection for the 1s poll

	FDelegateHandle MapChangeHandle;
	FDelegateHandle MapOpenedHandle;
};
