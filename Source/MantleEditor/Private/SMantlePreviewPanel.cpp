// Copyright Joshua Eger. All Rights Reserved.
#include "SMantlePreviewPanel.h"

#include "MantlePreviewClient.h"
#include "MantleSettings.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/MessageDialog.h"
#include "Styling/AppStyle.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"

#define LOCTEXT_NAMESPACE "MantlePreviewPanel"

const FName SMantlePreviewPanel::TabName(TEXT("MantlePreview"));

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePreviewPanelPrivate
{
	TSharedRef<SDockTab> SpawnPreviewTab(const FSpawnTabArgs&)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			.Label(LOCTEXT("TabLabel", "Mantle Preview"))
			[
				SNew(SMantlePreviewPanel)
			];
	}

	UWorld* PanelEditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	FSlateColor LevelColour(const FString& Level)
	{
		if (Level.Equals(TEXT("error"), ESearchCase::CaseSensitive))
		{
			return FSlateColor(FLinearColor(0.95f, 0.35f, 0.30f));
		}
		if (Level.Equals(TEXT("warn"), ESearchCase::CaseSensitive))
		{
			return FSlateColor(FLinearColor(0.95f, 0.75f, 0.25f));
		}
		return FSlateColor::UseForeground();
	}

	FSlateColor FlagColour(bool bReady)
	{
		return bReady ? FSlateColor(FLinearColor(0.55f, 0.80f, 0.55f)) : FSlateColor::UseSubduedForeground();
	}

	FText FlagText(const FText& Name, bool bReady)
	{
		return FText::Format(LOCTEXT("FlagFormat", "{0}: {1}"), Name, bReady ? LOCTEXT("FlagReady", "ready") : LOCTEXT("FlagWaiting", "waiting"));
	}

	/** One message as a line of text, for the clipboard. */
	FString MessageLine(const FMantleTestLocally::FMessage& Message)
	{
		return FString::Printf(TEXT("[%.2f] %s %s: %s"), Message.Seconds, *Message.Level, *Message.Source, *Message.Text);
	}
}

void SMantlePreviewPanel::RegisterTab()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName, FOnSpawnTab::CreateStatic(&MantlePreviewPanelPrivate::SpawnPreviewTab))
		.SetDisplayName(LOCTEXT("TabTitle", "Mantle Preview"))
		.SetTooltipText(LOCTEXT("TabTooltip", "What Test Locally is doing, and what the Mantle client running your level reports."))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
}

void SMantlePreviewPanel::UnregisterTab()
{
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
	}
}

void SMantlePreviewPanel::OpenTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(TabName);
}

void SMantlePreviewPanel::TestLocally()
{
	OpenTab();
	FMantleTestLocally& TestLocallyFlow = FMantleTestLocally::Get();
	if (TestLocallyFlow.IsBusy())
	{
		// A click while it works only shows the panel.
		return;
	}
	// Every outcome, errors included, is the panel's state; nothing is reported only in the log.
	FString Error;
	TestLocallyFlow.Run(MantlePreviewPanelPrivate::PanelEditorWorld(), Error);
}

void SMantlePreviewPanel::StopPreview()
{
	FMantleTestLocally::Get().Stop();
}

void SMantlePreviewPanel::Construct(const FArguments& InArgs)
{
	using namespace MantlePreviewPanelPrivate;

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(10.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("BoldFont"))
				.Text(this, &SMantlePreviewPanel::GetStateText)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity_Lambda([]()
				{
					return FMantleTestLocally::Get().GetState() == EMantleTestLocallyState::Failed
						? FSlateColor(FLinearColor(0.95f, 0.35f, 0.30f))
						: FSlateColor::UseForeground();
				})
				.Visibility_Lambda([]()
				{
					return FMantleTestLocally::Get().GetErrorText().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
				})
				.Text(this, &SMantlePreviewPanel::GetDetailText)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity_Lambda([]()
				{
					return FMantleTestLocally::Get().IsNoticeError()
						? FSlateColor(FLinearColor(0.95f, 0.35f, 0.30f))
						: FSlateColor::UseSubduedForeground();
				})
				.Visibility_Lambda([]()
				{
					return FMantleTestLocally::Get().GetNotice().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
				})
				.Text_Lambda([]() { return FText::FromString(FMantleTestLocally::Get().GetNotice()); })
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 8.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 16.0f, 0.0f)
				[
					SNew(STextBlock)
					.ColorAndOpacity_Lambda([]() { return FlagColour(FMantleTestLocally::Get().IsGeometryReady()); })
					.Text_Lambda([]() { return FlagText(LOCTEXT("FlagGeometry", "Geometry"), FMantleTestLocally::Get().IsGeometryReady()); })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 16.0f, 0.0f)
				[
					SNew(STextBlock)
					.ColorAndOpacity_Lambda([]() { return FlagColour(FMantleTestLocally::Get().IsScriptsReady()); })
					.Text_Lambda([]() { return FlagText(LOCTEXT("FlagScripts", "Scripts"), FMantleTestLocally::Get().IsScriptsReady()); })
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(STextBlock)
					.ColorAndOpacity_Lambda([]() { return FlagColour(FMantleTestLocally::Get().IsSpawnReady()); })
					.Text_Lambda([]() { return FlagText(LOCTEXT("FlagSpawn", "Spawn"), FMantleTestLocally::Get().IsSpawnReady()); })
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(this, &SMantlePreviewPanel::GetClientText)
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Visibility_Lambda([]()
				{
					return FMantleTestLocally::Get().GetBlockedRequests() > 0 ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.Text(this, &SMantlePreviewPanel::GetBlockedText)
			]
			// What the export kept, approximated and dropped; collapsed until the creator asks.
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 4.0f, 0.0f, 0.0f)
			[
				SNew(SExpandableArea)
				.InitiallyCollapsed(true)
				.Visibility_Lambda([this]() { return bHasCoverage ? EVisibility::Visible : EVisibility::Collapsed; })
				.HeaderContent()
				[
					SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("BoldFont"))
					.Text_Lambda([this]() { return CoverageTitle; })
				]
				.BodyContent()
				[
					SNew(SBox)
					.MaxDesiredHeight(260.0f)
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot()
						[
							SNew(STextBlock)
							.AutoWrapText(true)
							.Text_Lambda([this]() { return CoverageBody; })
						]
					]
				]
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			.Padding(0.0f, 8.0f, 0.0f, 8.0f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.DarkGroupBorder"))
				.Padding(4.0f)
				[
					SAssignNew(MessageList, SListView<TSharedPtr<FRow>>)
					.ListItemsSource(&Rows)
					.SelectionMode(ESelectionMode::None)
					.OnGenerateRow(this, &SMantlePreviewPanel::MakeRow)
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("TestLocally", "Test Locally"))
					.ToolTipText(LOCTEXT("TestLocallyTip", "Export this level and run it in the Mantle client on this PC. Nothing is uploaded; running it again replaces the preview."))
					.IsEnabled_Lambda([]() { return !FMantleTestLocally::Get().IsBusy(); })
					.OnClicked(this, &SMantlePreviewPanel::OnTestLocally)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("Stop", "Stop"))
					.ToolTipText(LOCTEXT("StopTip", "Close the preview this editor started. A Mantle you started yourself is never touched."))
					.IsEnabled_Lambda([]() { return MantlePreviewClient::IsRunning(); })
					.OnClicked(this, &SMantlePreviewPanel::OnStop)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("ResetLocalData", "Reset local data"))
					.ToolTipText(LOCTEXT("ResetLocalDataTip", "Delete what the preview saved for this level (Test Locally keeps it between runs), so the next run starts fresh."))
					.IsEnabled_Lambda([]() { return !FMantleTestLocally::Get().IsBusy() && !MantlePreviewClient::IsRunning(); })
					.OnClicked(this, &SMantlePreviewPanel::OnResetLocalData)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("ChooseClient", "Choose client..."))
					.ToolTipText(LOCTEXT("ChooseClientTip", "Pick the Mantle client program to run previews in, when Test Locally cannot find the one you installed."))
					.IsEnabled_Lambda([]() { return !FMantleTestLocally::Get().IsBusy(); })
					.OnClicked(this, &SMantlePreviewPanel::OnChooseClient)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(LOCTEXT("OpenBundleFolder", "Open bundle folder"))
					.ToolTipText(LOCTEXT("OpenBundleFolderTip", "Show the folder Test Locally exported, with the preview's status file."))
					.IsEnabled_Lambda([]() { return !FMantleTestLocally::Get().GetBundleDirectory().IsEmpty(); })
					.OnClicked(this, &SMantlePreviewPanel::OnOpenBundleFolder)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("CopyMessages", "Copy messages"))
					.ToolTipText(LOCTEXT("CopyMessagesTip", "Copy every message below to the clipboard."))
					.IsEnabled_Lambda([]() { return FMantleTestLocally::Get().GetMessages().Num() > 0; })
					.OnClicked(this, &SMantlePreviewPanel::OnCopyMessages)
				]
			]
		]
	];

	SyncRows();
	RegisterActiveTimer(0.33f, FWidgetActiveTimerDelegate::CreateSP(this, &SMantlePreviewPanel::Refresh));
}

EActiveTimerReturnType SMantlePreviewPanel::Refresh(double InCurrentTime, float InDeltaTime)
{
	FMantleTestLocally& TestLocallyFlow = FMantleTestLocally::Get();
	TestLocallyFlow.Poll();
	if (TestLocallyFlow.GetRevision() != ShownRevision)
	{
		SyncRows();
	}
	return EActiveTimerReturnType::Continue;
}

void SMantlePreviewPanel::SyncRows()
{
	const FMantleTestLocally& TestLocallyFlow = FMantleTestLocally::Get();
	ShownRevision = TestLocallyFlow.GetRevision();

	const FMantleCoverage& Coverage = TestLocallyFlow.GetCoverage();
	bHasCoverage = !Coverage.IsEmpty();
	CoverageTitle = FText::Format(LOCTEXT("CoverageTitle", "Export coverage: {0}"), FText::FromString(Coverage.ToSummary()));
	CoverageBody = FText::FromString(FString::Join(Coverage.ToDisplayLines(), TEXT("\n")));

	const TArray<FRow>& Messages = TestLocallyFlow.GetMessages();
	const bool bGrew = Messages.Num() > Rows.Num();
	Rows.Reset(Messages.Num());
	for (const FRow& Message : Messages)
	{
		Rows.Add(MakeShared<FRow>(Message));
	}
	if (MessageList.IsValid())
	{
		MessageList->RequestListRefresh();
		// Newest last, and the list follows it as it grows.
		if (bGrew && Rows.Num() > 0)
		{
			MessageList->RequestScrollIntoView(Rows.Last());
		}
	}
}

TSharedRef<ITableRow> SMantlePreviewPanel::MakeRow(TSharedPtr<FRow> Row, const TSharedRef<STableViewBase>& Owner)
{
	using namespace MantlePreviewPanelPrivate;

	const FRow Message = Row.IsValid() ? *Row : FRow();
	return SNew(STableRow<TSharedPtr<FRow>>, Owner)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.0f, 1.0f, 8.0f, 1.0f)
			[
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(FText::FromString(FString::Printf(TEXT("%.2f"), Message.Seconds)))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0.0f, 1.0f, 8.0f, 1.0f)
			[
				SNew(STextBlock)
				.ColorAndOpacity(LevelColour(Message.Level))
				.Text(FText::FromString(Message.Source))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.Padding(0.0f, 1.0f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(LevelColour(Message.Level))
				.Text(FText::FromString(Message.Text))
			]
		];
}

FText SMantlePreviewPanel::GetStateText() const
{
	switch (FMantleTestLocally::Get().GetState())
	{
	case EMantleTestLocallyState::FindingClient: return LOCTEXT("StateFinding", "Checking the Mantle client...");
	case EMantleTestLocallyState::Exporting: return LOCTEXT("StateExporting", "Exporting this level...");
	case EMantleTestLocallyState::Starting: return LOCTEXT("StateStarting", "Starting the preview...");
	case EMantleTestLocallyState::Ready: return LOCTEXT("StateReady", "Ready: the preview is running.");
	case EMantleTestLocallyState::Failed: return LOCTEXT("StateFailed", "Failed");
	case EMantleTestLocallyState::Stopped: return LOCTEXT("StateStopped", "Stopped");
	case EMantleTestLocallyState::Closed: return LOCTEXT("StateClosed", "The preview closed.");
	default: return LOCTEXT("StateIdle", "Press Test Locally to run this level in the Mantle client.");
	}
}

FText SMantlePreviewPanel::GetDetailText() const
{
	return FText::FromString(FMantleTestLocally::Get().GetErrorText());
}

FText SMantlePreviewPanel::GetClientText() const
{
	const FMantleTestLocally& TestLocallyFlow = FMantleTestLocally::Get();
	if (!TestLocallyFlow.HasClient())
	{
		const FString Project = MantlePreviewClient::ProjectOverride(FCommandLine::Get(), UMantleSettings::Get()->PreviewClientProject.FilePath);
		if (!Project.IsEmpty())
		{
			return FText::Format(LOCTEXT("ClientNoneProject", "Client: not looked for yet; will run the project {0}"), FText::FromString(Project));
		}
		return LOCTEXT("ClientNone", "Client: not looked for yet");
	}
	const FMantlePreviewClientInfo& Client = TestLocallyFlow.GetClient();
	const FText Version = Client.ClientVersion.IsEmpty() ? LOCTEXT("ClientVersionUnknown", "unknown") : FText::FromString(Client.ClientVersion);
	if (!Client.Project.IsEmpty())
	{
		return FText::Format(LOCTEXT("ClientProjectFormat", "Client: {0}, project {1}, version {2}"),
			FText::FromString(Client.Source), FText::FromString(Client.Project), Version);
	}
	return FText::Format(LOCTEXT("ClientFormat", "Client: {0}, version {1}"), FText::FromString(Client.Source), Version);
}

FText SMantlePreviewPanel::GetBlockedText() const
{
	return FText::Format(
		LOCTEXT("BlockedFormat", "{0} requests were refused. The preview is offline: anything that tried to reach Mantle's services was blocked, not sent."),
		FText::AsNumber(FMantleTestLocally::Get().GetBlockedRequests()));
}

FReply SMantlePreviewPanel::OnTestLocally()
{
	TestLocally();
	return FReply::Handled();
}

FReply SMantlePreviewPanel::OnStop()
{
	StopPreview();
	return FReply::Handled();
}

FReply SMantlePreviewPanel::OnResetLocalData()
{
	FMantleTestLocally& TestLocallyFlow = FMantleTestLocally::Get();
	// The level the last preview ran, else the one open now.
	FString LevelName = TestLocallyFlow.GetLevelName();
	if (LevelName.IsEmpty())
	{
		LevelName = FMantleTestLocally::LevelNameOf(MantlePreviewPanelPrivate::PanelEditorWorld());
	}
	if (LevelName.IsEmpty())
	{
		TestLocallyFlow.SetNotice(TEXT("Open and save a level first: the preview's local data belongs to one level."), /*bError*/ true);
		return FReply::Handled();
	}

	const EAppReturnType::Type Answer = FMessageDialog::Open(EAppMsgType::YesNo,
		FText::Format(LOCTEXT("ResetConfirm", "Delete everything the preview saved for {0}? Its saved data starts from nothing on the next Test Locally."),
			FText::FromString(LevelName)));
	if (Answer != EAppReturnType::Yes)
	{
		return FReply::Handled();
	}

	FString Message;
	const bool bOk = TestLocallyFlow.ResetLocalData(LevelName, Message);
	TestLocallyFlow.SetNotice(Message, !bOk);
	return FReply::Handled();
}

FReply SMantlePreviewPanel::OnChooseClient()
{
	FMantleTestLocally& TestLocallyFlow = FMantleTestLocally::Get();
	FString Message;
	const bool bOk = TestLocallyFlow.ChooseClient(Message);
	if (!Message.IsEmpty())
	{
		TestLocallyFlow.SetNotice(Message, !bOk);
	}
	return FReply::Handled();
}

FReply SMantlePreviewPanel::OnOpenBundleFolder()
{
	const FString& Folder = FMantleTestLocally::Get().GetBundleDirectory();
	if (!Folder.IsEmpty())
	{
		FPlatformProcess::ExploreFolder(*Folder);
	}
	return FReply::Handled();
}

FReply SMantlePreviewPanel::OnCopyMessages()
{
	TArray<FString> Lines;
	for (const FMantleTestLocally::FMessage& Message : FMantleTestLocally::Get().GetMessages())
	{
		Lines.Add(MantlePreviewPanelPrivate::MessageLine(Message));
	}
	// And the coverage report, so a copy carries what the export dropped too.
	const FMantleCoverage& Coverage = FMantleTestLocally::Get().GetCoverage();
	if (!Coverage.IsEmpty())
	{
		Lines.Add(FString());
		Lines.Add(FString::Printf(TEXT("Export coverage: %s"), *Coverage.ToSummary()));
		Lines.Append(Coverage.ToDisplayLines());
	}
	FPlatformApplicationMisc::ClipboardCopy(*FString::Join(Lines, TEXT("\n")));
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
