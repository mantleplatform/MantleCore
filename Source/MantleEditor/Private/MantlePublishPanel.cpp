// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePublishPanel.h"
#include "MantleApiClient.h"
#include "MantleCosmeticUpload.h"
#include "MantleCreatorSignIn.h"
#include "MantleRuntime.h"

#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Misc/MessageDialog.h"
#include "Styling/AppStyle.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MantlePublish"

const FName SMantlePublishPanel::TabName(TEXT("MantlePublish"));

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePublishPanelPrivate
{
	/** The publish running now (or the last one), its ticker, and what "Publish again" repeats. */
	TSharedPtr<FMantlePlacePublishSession> CurrentSession;
	FTSTicker::FDelegateHandle TickerHandle;
	TWeakObjectPtr<UWorld> LastWorld;
	bool bLastGoLive = true;
	bool bLastThin = false;
	FString LastNotes;

	/**
	 * Who can use the vehicles this publish turns into library items. Public is preselected and needs the
	 * rights declaration, which is asked again for every publish (SMantlePublishPanel::Start clears it); Public or Private
	 * carries over to the next publish.
	 */
	FMantleVehicleLibraryChoice VehicleChoice;

	/** Whether the running publish has vehicles to choose for (shown only then). */
	bool ShowsVehicleChoice()
	{
		return CurrentSession.IsValid() && CurrentSession->NeedsVehicleChoice();
	}

	void ApplyVehicleChoice()
	{
		if (CurrentSession.IsValid())
		{
			CurrentSession->VehicleChoice = VehicleChoice;
		}
	}

	TSharedRef<SDockTab> SpawnPublishTab(const FSpawnTabArgs&)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			.Label(LOCTEXT("TabLabel", "Mantle Publish"))
			[
				SNew(SMantlePublishPanel)
			];
	}

	bool TickSession(float)
	{
		if (!CurrentSession.IsValid())
		{
			TickerHandle.Reset();
			return false;
		}
		CurrentSession->Tick();
		if (CurrentSession->IsFinished())
		{
			const FMantlePublishResult& Result = CurrentSession->GetResult();
			UE_LOG(LogMantle, Log, TEXT("MANTLE_F6 panel: %s (%s)"), Result.bSuccess ? TEXT("done") : TEXT("stopped"), *Result.Message);
			for (const FMantleValidationIssue& Issue : Result.Attention)
			{
				UE_LOG(LogMantle, Log, TEXT("MANTLE_F6 attention: %s"), *Issue.ToString());
			}
			TickerHandle.Reset();
			return false;
		}
		return true;
	}

	const FSlateBrush* StepIcon(EMantlePublishStepState State)
	{
		switch (State)
		{
		case EMantlePublishStepState::Done: return FAppStyle::GetBrush("Icons.SuccessWithColor");
		case EMantlePublishStepState::DoneWithNotes: return FAppStyle::GetBrush("Icons.SuccessWithColor");
		case EMantlePublishStepState::Running: return FAppStyle::GetBrush("Icons.CircleArrowRight");
		case EMantlePublishStepState::Failed: return FAppStyle::GetBrush("Icons.ErrorWithColor");
		case EMantlePublishStepState::Skipped: return FAppStyle::GetBrush("Icons.InfoWithColor");
		default: return FAppStyle::GetNoBrush();
		}
	}

	FSlateColor StepColour(EMantlePublishStepState State)
	{
		return State == EMantlePublishStepState::NotStarted ? FSlateColor::UseSubduedForeground() : FSlateColor::UseForeground();
	}
}

void SMantlePublishPanel::RegisterTab()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName, FOnSpawnTab::CreateStatic(&MantlePublishPanelPrivate::SpawnPublishTab))
		.SetDisplayName(LOCTEXT("TabTitle", "Mantle Publish"))
		.SetTooltipText(LOCTEXT("TabTooltip", "What Publish is doing: checking, preparing, uploading and publishing this level."))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
}

void SMantlePublishPanel::UnregisterTab()
{
	using namespace MantlePublishPanelPrivate;
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	// Shutting down mid-publish: stop sending. Everything uploaded stays; the next Publish resumes.
	if (CurrentSession.IsValid() && CurrentSession->CanCancel())
	{
		CurrentSession->Cancel();
	}
	CurrentSession.Reset();
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
	}
}

TSharedPtr<FMantlePlacePublishSession> SMantlePublishPanel::GetSession()
{
	return MantlePublishPanelPrivate::CurrentSession;
}

bool SMantlePublishPanel::IsRunning()
{
	return MantlePublishPanelPrivate::CurrentSession.IsValid() && !MantlePublishPanelPrivate::CurrentSession->IsFinished();
}

void SMantlePublishPanel::Start(UWorld* World, bool bGoLive, const FString& Notes, const bool bThinInstances)
{
	using namespace MantlePublishPanelPrivate;
	if (IsRunning())
	{
		FGlobalTabmanager::Get()->TryInvokeTab(TabName);
		return;
	}
	if (!World)
	{
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NoLevel", "There is no level open to publish."));
		return;
	}
	FString WhyNot;
	if (!FMantleApiClient::IsConfigured(WhyNot))
	{
		FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(WhyNot));
		return;
	}
	LastWorld = World;
	bLastGoLive = bGoLive;
	bLastThin = bThinInstances;
	LastNotes = Notes;
	// Public or Private is remembered between publishes, the declaration is not: each go-live with vehicles asks for a fresh tick.
	VehicleChoice.bRightsDeclared = false;
	const TSharedRef<FMantleLevelPublishSource> Source = MakeShared<FMantleLevelPublishSource>(World, Notes);
	Source->SetThinInstances(bThinInstances);
	CurrentSession = MakeShared<FMantlePlacePublishSession>(Source, MakeShared<FMantleApiUploadTransport>(), bGoLive, Notes);
	ApplyVehicleChoice();
	FGlobalTabmanager::Get()->TryInvokeTab(TabName);
	CurrentSession->Start();
	if (!TickerHandle.IsValid())
	{
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&MantlePublishPanelPrivate::TickSession), 0.0f);
	}
}

void SMantlePublishPanel::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(12.0f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
				[
					SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
					.Text(this, &SMantlePublishPanel::GetTitleText)
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeStepRow(EMantlePublishStep::Check, LOCTEXT("StepCheck", "Check this place"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeStepRow(EMantlePublishStep::Prepare, LOCTEXT("StepPrepare", "Prepare meshes and materials"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeStepRow(EMantlePublishStep::Upload, LOCTEXT("StepUpload", "Upload to Mantle"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeStepRow(EMantlePublishStep::Process, LOCTEXT("StepProcess", "Mantle processes and screens them"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeStepRow(EMantlePublishStep::GoLive, LOCTEXT("StepGoLive", "Publish"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 6)
				[
					SNew(SBox)
					.HeightOverride(14.0f)
					[
						SNew(SProgressBar)
						.Percent_Lambda([] { return TOptional<float>(SMantlePublishPanel::GetSession().IsValid() ? SMantlePublishPanel::GetSession()->GetProgress() : 0.0f); })
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text(this, &SMantlePublishPanel::GetStatusText)
				]
				// Only a go-live with vehicles: who can use them once they are library items. Going live
				// waits here until Public has its declaration or Private is chosen.
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
				[
					MakeVehicleChoice()
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
					[
						SNew(SButton)
						.Visibility_Lambda([] { return SMantlePublishPanel::GetSession().IsValid() && SMantlePublishPanel::GetSession()->CanCancel() ? EVisibility::Visible : EVisibility::Collapsed; })
						.Text(LOCTEXT("Cancel", "Cancel"))
						.ToolTipText(LOCTEXT("CancelTooltip", "Stop here. Nothing is published; what was uploaded stays on Mantle, so the next Publish continues from it."))
						.OnClicked(this, &SMantlePublishPanel::OnCancel)
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.ButtonStyle(&FAppStyle::Get(), "PrimaryButton")
						.Visibility_Lambda([]
						{
							const TSharedPtr<FMantlePlacePublishSession> Session = SMantlePublishPanel::GetSession();
							return Session.IsValid() && Session->IsFinished() && !Session->Succeeded() ? EVisibility::Visible : EVisibility::Collapsed;
						})
						.Text_Lambda([]
						{
							return MantlePublishPanelPrivate::bLastGoLive ? LOCTEXT("PublishAgain", "Publish again") : LOCTEXT("SaveAgain", "Save again");
						})
						.ToolTipText(LOCTEXT("PublishAgainTooltip", "Run it again with the same notes. Nothing already uploaded is sent twice."))
						.OnClicked(this, &SMantlePublishPanel::OnPublishAgain)
					]
					// GhzGarage/mantle#818: the place is too big and even thinning would fix it; the numbers are in the list below.
					+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
					[
						SNew(SButton)
						.Visibility_Lambda([]
						{
							const TSharedPtr<FMantlePlacePublishSession> Session = SMantlePublishPanel::GetSession();
							return Session.IsValid() && Session->IsFinished() && !Session->Succeeded() && !Session->GetResult().ThinOffer.IsEmpty()
								? EVisibility::Visible : EVisibility::Collapsed;
						})
						.Text(LOCTEXT("ThinAndPublish", "Thin and publish"))
						.ToolTipText(LOCTEXT("ThinAndPublishTooltip", "Remove foliage and instances evenly across the level until the place fits, then publish. The numbers are listed below; the coverage report says how many were removed. Your level is not changed."))
						.OnClicked(this, &SMantlePublishPanel::OnThinAndPublish)
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
				[
					SAssignNew(AttentionBox, SVerticalBox)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[
					SNew(SExpandableArea)
					.InitiallyCollapsed(true)
					.AreaTitle_Lambda([]
					{
						const TSharedPtr<FMantlePlacePublishSession> Session = SMantlePublishPanel::GetSession();
						const int32 Count = Session.IsValid() ? Session->GetResult().Validation.Issues.Num() : 0;
						return FText::Format(LOCTEXT("NotesTitle", "Notes from Check this place ({0})"), FText::AsNumber(Count));
					})
					.BodyContent()
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.Text(this, &SMantlePublishPanel::GetNotesText)
					]
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SExpandableArea)
					.InitiallyCollapsed(true)
					.AreaTitle(LOCTEXT("DetailsTitle", "Details"))
					.BodyContent()
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.Text(this, &SMantlePublishPanel::GetDetailsText)
					]
				]
			]
		]
	];

	RebuildAttention();
	RegisterActiveTimer(0.25f, FWidgetActiveTimerDelegate::CreateSP(this, &SMantlePublishPanel::Refresh));
}

TSharedRef<SWidget> SMantlePublishPanel::MakeVehicleChoice()
{
	using namespace MantlePublishPanelPrivate;
	return SNew(SVerticalBox)
		.Visibility_Lambda([] { return ShowsVehicleChoice() ? EVisibility::Visible : EVisibility::Collapsed; })
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
		[
			SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("NormalFontBold"))
			.Text(LOCTEXT("VehiclesTitle", "Vehicles, weapons and script packages in this place: who can use them"))
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SCheckBox)
			.Style(&FAppStyle::Get().GetWidgetStyle<FCheckBoxStyle>("RadioButton"))
			.IsChecked_Lambda([] { return VehicleChoice.bPublic ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([](ECheckBoxState State)
			{
				if (State == ECheckBoxState::Checked)
				{
					VehicleChoice.bPublic = true;
					ApplyVehicleChoice();
				}
			})
			[
				SNew(STextBlock).Text(LOCTEXT("VehiclesPublic", "Public: listed in the library for every creator"))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
		[
			SNew(SCheckBox)
			.Style(&FAppStyle::Get().GetWidgetStyle<FCheckBoxStyle>("RadioButton"))
			.IsChecked_Lambda([] { return VehicleChoice.bPublic ? ECheckBoxState::Unchecked : ECheckBoxState::Checked; })
			.OnCheckStateChanged_Lambda([](ECheckBoxState State)
			{
				if (State == ECheckBoxState::Checked)
				{
					VehicleChoice.bPublic = false;
					ApplyVehicleChoice();
				}
			})
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(LOCTEXT("VehiclesPrivate", "Private: only you can use them"))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
		[
			SNew(SCheckBox)
			.Visibility_Lambda([] { return VehicleChoice.bPublic ? EVisibility::Visible : EVisibility::Collapsed; })
			.IsChecked_Lambda([] { return VehicleChoice.bRightsDeclared ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([](ECheckBoxState State)
			{
				VehicleChoice.bRightsDeclared = State == ECheckBoxState::Checked;
				ApplyVehicleChoice();
			})
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(LOCTEXT("VehiclesRights", "I own this content or its licence permits public redistribution on Mantle"))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Visibility_Lambda([] { return VehicleChoice.bPublic && !VehicleChoice.bRightsDeclared ? EVisibility::Visible : EVisibility::Collapsed; })
			.ColorAndOpacity(FLinearColor(1.0f, 0.75f, 0.2f))
			.Text(LOCTEXT("VehiclesRightsNeeded", "Tick the declaration to go live with public items, or choose Private."))
		];
}

TSharedRef<SWidget> SMantlePublishPanel::MakeStepRow(EMantlePublishStep Step, const FText& Label)
{
	auto State = [Step]
	{
		const TSharedPtr<FMantlePlacePublishSession> Session = SMantlePublishPanel::GetSession();
		return Session.IsValid() ? Session->GetStepState(Step) : EMantlePublishStepState::NotStarted;
	};
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 3, 8, 3)
		[
			SNew(SBox)
			.WidthOverride(16.0f)
			.HeightOverride(16.0f)
			[
				SNew(SImage)
				.Image_Lambda([State] { return MantlePublishPanelPrivate::StepIcon(State()); })
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 3, 12, 3)
		[
			SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("NormalFontBold"))
			.ColorAndOpacity_Lambda([State] { return MantlePublishPanelPrivate::StepColour(State()); })
			.Text(Label)
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0, 3)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text_Lambda([Step]
			{
				const TSharedPtr<FMantlePlacePublishSession> Session = SMantlePublishPanel::GetSession();
				return Session.IsValid() ? FText::FromString(Session->GetStepDetail(Step)) : FText::GetEmpty();
			})
		];
}

EActiveTimerReturnType SMantlePublishPanel::Refresh(double InCurrentTime, float InDeltaTime)
{
	const TSharedPtr<FMantlePlacePublishSession> Session = SMantlePublishPanel::GetSession();
	const int32 Count = Session.IsValid() ? Session->GetAttention().Num() : 0;
	if (Session != ShownSession.Pin() || Count != ShownAttention)
	{
		RebuildAttention();
	}
	return EActiveTimerReturnType::Continue;
}

void SMantlePublishPanel::RebuildAttention()
{
	const TSharedPtr<FMantlePlacePublishSession> Session = SMantlePublishPanel::GetSession();
	ShownSession = Session;
	ShownAttention = Session.IsValid() ? Session->GetAttention().Num() : 0;
	if (!AttentionBox.IsValid())
	{
		return;
	}
	AttentionBox->ClearChildren();
	if (!Session.IsValid())
	{
		return;
	}
	// Check this place's errors stop a publish before anything is uploaded: they are rows too.
	TArray<FMantleValidationIssue> Rows;
	for (const FMantleValidationIssue& Issue : Session->GetResult().Validation.Issues)
	{
		if (Issue.Severity == EMantleIssueSeverity::Error)
		{
			Rows.Add(Issue);
		}
	}
	Rows.Append(Session->GetAttention());
	// GhzGarage/mantle#818: the fix on offer, with its numbers, so choosing "Thin and publish" is an informed choice.
	if (Session->IsFinished() && !Session->GetResult().ThinOffer.IsEmpty())
	{
		Rows.Add(FMantleValidationIssue{ EMantleIssueSeverity::Warning, FString(), TEXT("Thinning is on offer."), Session->GetResult().ThinOffer });
	}
	if (Rows.Num() == 0)
	{
		return;
	}
	AttentionBox->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
	[
		SNew(STextBlock)
		.Font(FAppStyle::GetFontStyle("NormalFontBold"))
		.Text(LOCTEXT("AttentionTitle", "Needs your attention"))
	];
	for (const FMantleValidationIssue& Row : Rows)
	{
		const bool bError = Row.Severity == EMantleIssueSeverity::Error;
		const FString Problem = Row.Actor.IsEmpty() ? Row.Problem : FString::Printf(TEXT("'%s' %s"), *Row.Actor, *Row.Problem);
		AttentionBox->AddSlot().AutoHeight().Padding(0, 2, 0, 6)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0, 1, 8, 0)
			[
				SNew(SBox)
				.WidthOverride(16.0f)
				.HeightOverride(16.0f)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush(bError ? "Icons.ErrorWithColor" : "Icons.WarningWithColor"))
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text(FText::FromString(Problem))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Visibility(Row.Remedy.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(FText::FromString(Row.Remedy))
				]
			]
		];
	}
}

FText SMantlePublishPanel::GetTitleText() const
{
	const TSharedPtr<FMantlePlacePublishSession> Session = SMantlePublishPanel::GetSession();
	if (!Session.IsValid())
	{
		return LOCTEXT("TitleIdle", "Choose Tools > Mantle > Publish to publish the open level.");
	}
	const FText Level = FText::FromString(Session->GetLevelName());
	if (Session->Succeeded())
	{
		return Session->IsGoLive() ? FText::Format(LOCTEXT("TitlePublished", "'{0}' is published"), Level)
			: FText::Format(LOCTEXT("TitleSaved", "'{0}' is saved to Mantle"), Level);
	}
	if (Session->IsFinished())
	{
		return Session->GetResult().bCancelled ? FText::Format(LOCTEXT("TitleCancelled", "Publishing '{0}' was cancelled"), Level)
			: Session->GetResult().bWaitingForReview ? FText::Format(LOCTEXT("TitleReview", "'{0}' is waiting for Mantle's moderators"), Level)
			: FText::Format(LOCTEXT("TitleStopped", "'{0}' was not published"), Level);
	}
	return Session->IsGoLive() ? FText::Format(LOCTEXT("TitlePublishing", "Publishing '{0}'"), Level)
		: FText::Format(LOCTEXT("TitleSaving", "Saving '{0}' to Mantle"), Level);
}

FText SMantlePublishPanel::GetStatusText() const
{
	const TSharedPtr<FMantlePlacePublishSession> Session = SMantlePublishPanel::GetSession();
	if (!Session.IsValid())
	{
		return LOCTEXT("StatusIdle", "Publish checks the level, prepares its meshes and materials, uploads what Mantle does not have yet, waits for Mantle to process it, and makes it live. You can keep working while it runs.");
	}
	FString Text = Session->GetStatusText();
	if (Session->Succeeded() && !Session->GetResult().PlaceId.IsEmpty())
	{
		Text += FString::Printf(TEXT("\nPlace id: %s"), *Session->GetResult().PlaceId);
	}
	return FText::FromString(Text);
}

FText SMantlePublishPanel::GetNotesText() const
{
	const TSharedPtr<FMantlePlacePublishSession> Session = SMantlePublishPanel::GetSession();
	if (!Session.IsValid())
	{
		return FText::GetEmpty();
	}
	TArray<FString> Lines;
	for (const FMantleValidationIssue& Issue : Session->GetResult().Validation.Issues)
	{
		Lines.Add(Issue.ToString());
	}
	return FText::FromString(FString::Join(Lines, TEXT("\n\n")));
}

FText SMantlePublishPanel::GetDetailsText() const
{
	const TSharedPtr<FMantlePlacePublishSession> Session = SMantlePublishPanel::GetSession();
	if (!Session.IsValid())
	{
		return FText::GetEmpty();
	}
	TArray<FString> Lines = Session->GetLog();
	if (const FMantleAssetUploadQueue* Queue = Session->GetQueue())
	{
		Lines.Add(FString());
		Lines.Append(Queue->GetLog());
	}
	// The tail: a Fab level's log is hundreds of lines.
	constexpr int32 MaxLines = 300;
	if (Lines.Num() > MaxLines)
	{
		Lines.RemoveAt(0, Lines.Num() - MaxLines);
	}
	return FText::FromString(FString::Join(Lines, TEXT("\n")));
}

FReply SMantlePublishPanel::OnCancel()
{
	if (const TSharedPtr<FMantlePlacePublishSession> Session = SMantlePublishPanel::GetSession())
	{
		Session->Cancel();
	}
	return FReply::Handled();
}

FReply SMantlePublishPanel::OnPublishAgain()
{
	using namespace MantlePublishPanelPrivate;
	UWorld* World = LastWorld.Get();
	if (!World)
	{
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("LevelGone", "The level that was being published is no longer open. Open it and choose Tools > Mantle > Publish."));
		return FReply::Handled();
	}
	if (!FMantleCreatorSignIn::EnsureSignedIn(LOCTEXT("AgainSignInReason", "Sign in to publish this place to Mantle.")))
	{
		return FReply::Handled();
	}
	Start(World, bLastGoLive, LastNotes, bLastThin);
	return FReply::Handled();
}

FReply SMantlePublishPanel::OnThinAndPublish()
{
	using namespace MantlePublishPanelPrivate;
	UWorld* World = LastWorld.Get();
	if (!World)
	{
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("ThinLevelGone", "The level that was being published is no longer open. Open it and choose Tools > Mantle > Publish."));
		return FReply::Handled();
	}
	if (!FMantleCreatorSignIn::EnsureSignedIn(LOCTEXT("ThinSignInReason", "Sign in to publish this place to Mantle.")))
	{
		return FReply::Handled();
	}
	Start(World, bLastGoLive, LastNotes, /*bThinInstances*/ true);
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
