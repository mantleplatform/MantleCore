// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleTestLocally.h"
#include "Widgets/SCompoundWidget.h"

class ITableRow;
class STableViewBase;
template <typename ItemType> class SListView;

/**
 * The Mantle Preview panel: what Test Locally is doing and what the preview client reports. The state
 * (finding client, exporting, starting, ready, failed, stopped, closed), the three ready flags, which client runs it,
 * the client's script messages (level-coloured, newest last, copyable), and how many requests the offline preview
 * refused, and the export's coverage report (what was kept, approximated and dropped) as a collapsible section; plus Test Locally, Stop, Reset local data, Choose client and Open bundle folder. It only shows
 * FMantleTestLocally's state, which it keeps current by polling preview-status.json while the owned process runs.
 */
class SMantlePreviewPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMantlePreviewPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** The nomad tab: registered at module startup, opened by Test Locally. */
	static const FName TabName;
	static void RegisterTab();
	static void UnregisterTab();
	static void OpenTab();

	/** The toolbar and menu action: shows the panel and runs Test Locally on the level open in the editor. */
	static void TestLocally();

	/** Stops the preview process this editor started (the toolbar and menu action). */
	static void StopPreview();

private:
	using FRow = FMantleTestLocally::FMessage;

	EActiveTimerReturnType Refresh(double InCurrentTime, float InDeltaTime);
	void SyncRows();
	TSharedRef<ITableRow> MakeRow(TSharedPtr<FRow> Row, const TSharedRef<STableViewBase>& Owner);

	FText GetStateText() const;
	FText GetDetailText() const;
	FText GetClientText() const;
	FText GetBlockedText() const;

	FReply OnTestLocally();
	FReply OnStop();
	FReply OnResetLocalData();
	FReply OnChooseClient();
	FReply OnOpenBundleFolder();
	FReply OnCopyMessages();

	TArray<TSharedPtr<FRow>> Rows;
	TSharedPtr<SListView<TSharedPtr<FRow>>> MessageList;
	int32 ShownRevision = INDEX_NONE;

	/** The export's coverage report, the collapsible section's title and body, rebuilt when the state's revision changes. */
	FText CoverageTitle;
	FText CoverageBody;
	bool bHasCoverage = false;
};
