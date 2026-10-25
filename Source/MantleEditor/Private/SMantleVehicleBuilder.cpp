// Copyright Joshua Eger. All Rights Reserved.
#include "SMantleVehicleBuilder.h"

#include "MantleVehicleBuilder.h"
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehicleChassisSkin.h"
#include "MantleVehiclePreset.h"
#include "MantleVehicleShapeKind.h"
#include "MantleVehicleSource.h"
#include "MantleVehicleSpawnPoint.h"
#include "SMantleBuilderViewport.h"

#include "AssetRegistry/AssetData.h"
#include "DesktopPlatformModule.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/StreamableRenderAsset.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "IDesktopPlatform.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PropertyCustomizationHelpers.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "MantleModelThumbnail.h"
#include "MantleThumbnailStore.h"
#include "MantleVehicleThumbnail.h"
#include "SMantleThumbnailSection.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MantleVehicleBuilder"

const FName SMantleVehicleBuilder::TabName(TEXT("MantleVehicleBuilder"));

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleBuilderPanelPrivate
{
	TSharedRef<SDockTab> SpawnTab(const FSpawnTabArgs&)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			.Label(LOCTEXT("TabLabel", "Build a vehicle"))
			[
				SNew(SMantleVehicleBuilder)
			];
	}

	const FLinearColor Good(0.55f, 0.80f, 0.55f);
	const FLinearColor Bad(0.95f, 0.35f, 0.30f);
	const FLinearColor Warn(0.95f, 0.65f, 0.25f);

	/** One sentence from a builder's list of problems. */
	FText JoinSentences(const TArray<FString>& Lines)
	{
		return FText::FromString(FString::Join(Lines, TEXT(" ")));
	}
}

void SMantleVehicleBuilder::RegisterTab()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName, FOnSpawnTab::CreateStatic(&MantleVehicleBuilderPanelPrivate::SpawnTab))
		.SetDisplayName(LOCTEXT("TabTitle", "Build a vehicle"))
		.SetTooltipText(LOCTEXT("TabTooltip", "Shape a vehicle players can drive in your place, or put your own model on one."))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
}

void SMantleVehicleBuilder::UnregisterTab()
{
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
	}
}

void SMantleVehicleBuilder::OpenTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(TabName);
}

// ---------------------------------------------------------------------------------------------------------------------
// The window.

void SMantleVehicleBuilder::Construct(const FArguments& InArgs)
{
	SAssignNew(Flow, SMantleBuilderFlow)
		.Steps(MakeSteps())
		.FinishLabel(LOCTEXT("Done", "Done"))
		.OnFinish(FSimpleDelegate::CreateSP(this, &SMantleVehicleBuilder::FinishWindow));

	ChildSlot
	[
		Flow.ToSharedRef()
	];
}

TArray<FMantleBuilderStep> SMantleVehicleBuilder::MakeSteps()
{
	TArray<FMantleBuilderStep> Steps;

	{
		FMantleBuilderStep& Step = Steps.AddDefaulted_GetRef();
		Step.Title = LOCTEXT("StepType", "What are you building?");
		Step.Help = LOCTEXT("StepTypeHelp", "Pick the closest match. Next you can change its size and shape, or use your own 3D model for its body.");
		Step.MakeContent = [this] { return MakeTypeStep(); };
		Step.CanContinue = [this](FText& OutWhyNot)
		{
			if (SelectedPresetId.IsEmpty())
			{
				OutWhyNot = LOCTEXT("NeedType", "Pick what you are building to continue.");
				return false;
			}
			return true;
		};
	}
	{
		FMantleBuilderStep& Step = Steps.AddDefaulted_GetRef();
		Step.Title = LOCTEXT("StepShape", "Shape it");
		Step.Help = LOCTEXT("StepShapeHelp", "Change the size and shape and watch the grey picture change. Mantle works out how heavy it is and how it handles from them. If you will use your own 3D model, you can skip this.");
		Step.MakeContent = [this] { return MakeShapeStep(); };
	}
	{
		FMantleBuilderStep& Step = Steps.AddDefaulted_GetRef();
		Step.Title = LOCTEXT("StepDrive", "Drive it");
		Step.Help = LOCTEXT("StepDriveHelp", "Put your vehicle in your level, then try it out. No model needed: the grey vehicle drives as it is. You can go back and change the shape any time.");
		Step.MakeContent = [this] { return MakeDriveStep(); };
	}
	{
		FMantleBuilderStep& Step = Steps.AddDefaulted_GetRef();
		Step.Title = LOCTEXT("StepModel", "Use your own model (optional)");
		Step.Help = LOCTEXT("StepModelHelp", "Have a 3D model of your own? Pick it here and Mantle puts it on this type of vehicle. No model? Press Done: your grey vehicle is all set.");
		Step.MakeContent = [this] { return MakeModelStep(); };
	}
	return Steps;
}

SMantleVehicleBuilder::~SMantleVehicleBuilder()
{
	// Closed any way but Done is a cancel: nothing of this window should be left in the level. (An editor that is shutting down is left alone.)
	if (!bFinished && !IsEngineExitRequested())
	{
		RemoveGreySpawns();
	}
}

void SMantleVehicleBuilder::FinishWindow()
{
	bFinished = true;
	// With no model, the grey vehicle in the level is the result and stays. With a model, the model's vehicle is the one to drive and publish once it
	// is in the level, and the grey one it replaced would be a second, unused vehicle left behind.
	const UMantleVehicleSource* Built = BuiltSource.Get();
	if (Built && FMantleVehicleBuilder::FindSpawn(*Built))
	{
		RemoveGreySpawns();
	}
	CloseWindow();
}

void SMantleVehicleBuilder::RemoveGreySpawns()
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	TArray<AMantleVehicleSpawnPoint*> Alive;
	for (const TWeakObjectPtr<AMantleVehicleSpawnPoint>& Weak : GreySpawns)
	{
		// One the creator already deleted is gone from its level (an undo may bring it back; that is theirs to decide).
		AMantleVehicleSpawnPoint* Spawn = Weak.Get();
		if (IsValid(Spawn) && Spawn->GetLevel() && Spawn->GetWorld() == World)
		{
			Alive.Add(Spawn);
		}
	}
	GreySpawns.Reset();
	if (Alive.Num() == 0 || !World)
	{
		return;
	}
	const FScopedTransaction Transaction(LOCTEXT("RemoveGreyVehicles", "Remove grey vehicle from level"));
	for (AMantleVehicleSpawnPoint* Spawn : Alive)
	{
		World->EditorDestroyActor(Spawn, /*bShouldModifyLevel*/ true);
	}
}

void SMantleVehicleBuilder::CloseWindow()
{
	if (const TSharedPtr<SDockTab> Tab = FGlobalTabmanager::Get()->FindExistingLiveTab(TabName))
	{
		Tab->RequestCloseTab();
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// 1. What are you building?

TSharedRef<SWidget> SMantleVehicleBuilder::MakeTypeStep()
{
	using namespace MantleVehicleBuilderPanelPrivate;

	TSharedRef<SVerticalBox> Cards = SNew(SVerticalBox);
	// A card for every preset whose file says it is ready: the window does not decide what a type is, the preset does.
	const TArray<const FMantleVehiclePreset*> Publishable = FMantleVehiclePresets::Publishable();
	for (const FMantleVehiclePreset* Preset : Publishable)
	{
		Cards->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
		[
			MakePresetCard(Preset->Id, FText::FromString(Preset->Name), FText::FromString(FMantleVehiclePresets::CardSentence(*Preset)))
		];
	}
	if (Publishable.Num() == 0)
	{
		Cards->AddSlot().AutoHeight()
		[
			SNew(STextBlock)
				.ColorAndOpacity(FSlateColor(Bad))
				.AutoWrapText(true)
				.Text(LOCTEXT("NoTypes", "No vehicle types were found. Reinstall the Mantle plugin, then open this window again."))
		];
	}

	return SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				Cards
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(STextBlock)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor(Bad))
					.Visibility_Lambda([this]() { return TypeProblem.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
					.Text_Lambda([this]() { return TypeProblem; })
			]
		];
}

TSharedRef<SWidget> SMantleVehicleBuilder::MakePresetCard(const FString& PresetId, const FText& Name, const FText& Description)
{
	return SNew(SButton)
		.HAlign(HAlign_Fill)
		.ContentPadding(FMargin(12, 10))
		.ButtonColorAndOpacity_Lambda([this, PresetId]()
		{
			return SelectedPresetId == PresetId ? FLinearColor(0.25f, 0.55f, 1.0f) : FLinearColor(1.f, 1.f, 1.f);
		})
		.OnClicked_Lambda([this, PresetId, Name]()
		{
			PickPreset(PresetId, Name);
			return FReply::Handled();
		})
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("BoldFont"))
					.Text(Name)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
			[
				SNew(STextBlock)
					.AutoWrapText(true)
					.Text(Description)
			]
		];
}

void SMantleVehicleBuilder::PickPreset(const FString& PresetId, const FText& Name)
{
	if (PresetId == SelectedPresetId)
	{
		return;
	}
	// The type's chassis at its own size: the starting point of the shape, and what Reset goes back to.
	FMantleChassisSpec Loaded;
	FString Error;
	if (!FMantleVehicleChassisBuilder::LoadSpec(PresetId, Loaded, Error))
	{
		TypeProblem = FText::Format(LOCTEXT("TypeNotRead", "The {0}'s shape could not be read ({1}). Reinstall the Mantle plugin, then open this window again."),
			Name, FText::FromString(Error));
		return;
	}
	TypeProblem = FText::GetEmpty();
	SelectedPresetId = PresetId;
	SelectedPresetName = Name;
	// A plane, helicopter or hovercraft is shaped by one Size held to the preset's own fit range.
	const FMantleVehiclePreset* Picked = FMantleVehiclePresets::Find(PresetId);
	Shape.Start(Loaded, Picked ? Picked->FitMinScale : 0.0, Picked ? Picked->FitMaxScale : 0.0);
	bPreviewDirty = false;

	// A different type is a different vehicle: what was made for the last one stays in the project, and this window stops pointing at it.
	GreyName.Reset();
	GreyPresetId.Reset();
	GreySource.Reset();
	GreySignature.Reset();
	GreyExtra.Reset();
	DriveStatus = FText::GetEmpty();
	GreyPlaceNote.Reset();
	BuiltSource.Reset();
	if (ThumbnailSection.IsValid())
	{
		ThumbnailSection->Refresh();
	}
	bHaveLastBuild = false;
	SuggestedSize.Reset();
	SaveStatus = FText::GetEmpty();

	// A model already picked is read again for the new type: its moving parts, its facing and its size are the new type's.
	ReadModel();
}

// ---------------------------------------------------------------------------------------------------------------------
// 2. Shape it.

void SMantleVehicleBuilder::SetDimension(const EMantleVehicleDimension Dimension, const double Value)
{
	// The session holds the size to its range and writes the note, the summary and the warnings at once; it says whether a size really changed (the
	// same size again, as a drag's last step and its commit are, changes nothing). Only the picture waits: it is redrawn once, shortly.
	if (Shape.SetDimension(Dimension, Value))
	{
		ShapeChanged();
	}
}

void SMantleVehicleBuilder::SetShapeSize(const double Value)
{
	if (Shape.SetSize(Value))
	{
		ShapeChanged();
	}
}

void SMantleVehicleBuilder::SetFeel(const EMantleVehicleFeel Feel, const double Value)
{
	if (Shape.SetFeel(Feel, Value))
	{
		ShapeChanged();
	}
}

void SMantleVehicleBuilder::SetPassengerSeat(const bool bOn)
{
	if (Shape.SetPassenger(bOn))
	{
		ShapeChanged();
	}
}

void SMantleVehicleBuilder::ShapeChanged()
{
	// What it said about the vehicle in the level was about the shape before.
	DriveStatus = FText::GetEmpty();
	// So is the build: a vehicle built from the old shape is not this shape's, and "Put it in my level", "Show its settings" and Done would hand on its old numbers.
	if (BuiltSource.IsValid() || bHaveLastBuild)
	{
		ForgetBuild();
	}
	MarkPreviewDirty();
}

FReply SMantleVehicleBuilder::OnResetShape()
{
	if (Shape.Reset())
	{
		DriveStatus = FText::GetEmpty();
		bPreviewDirty = false;
		RebuildPreview(/*bFrame*/ true);
	}
	return FReply::Handled();
}

void SMantleVehicleBuilder::MarkPreviewDirty()
{
	bPreviewDirty = true;
	if (!bPreviewTimerPending)
	{
		bPreviewTimerPending = true;
		RegisterActiveTimer(0.1f, FWidgetActiveTimerDelegate::CreateSP(this, &SMantleVehicleBuilder::OnPreviewTimer));
	}
}

void SMantleVehicleBuilder::FlushPreview()
{
	if (bPreviewDirty)
	{
		bPreviewDirty = false;
		RebuildPreview(/*bFrame*/ false);
	}
}

EActiveTimerReturnType SMantleVehicleBuilder::OnPreviewTimer(double CurrentTime, float DeltaTime)
{
	bPreviewTimerPending = false;
	FlushPreview();
	return EActiveTimerReturnType::Stop;
}

void SMantleVehicleBuilder::RebuildPreview(const bool bFrame)
{
	PreviewProblem = FText::GetEmpty();
	if (ShapeKind.IsValid())
	{
		ShapeKind->SetSpec(Shape.GetShaped());
	}
	const TSharedPtr<SMantleBuilderViewport> Viewport = ShapeViewport.Pin();
	if (!Viewport.IsValid())
	{
		return;
	}
	FMantleVehicleAsset Asset;
	FString Error;
	if (!FMantleVehicleChassisBuilder::MakeAsset(Shape.GetShaped(), Asset, Error))
	{
		PreviewProblem = FText::Format(LOCTEXT("PreviewNotMade", "The picture can't show these sizes ({0}). Change a size."), FText::FromString(FMantleVehicleShape::PlainReason(Error)));
		Viewport->RefreshFromKind();
		return;
	}
	UMantleBuiltVehicle* Built = UMantleBuiltVehicle::Build(Asset, GetTransientPackage(), &Error);
	if (!Built)
	{
		PreviewProblem = FText::Format(LOCTEXT("PreviewNotBuilt", "The picture can't show these sizes ({0}). Change a size."), FText::FromString(FMantleVehicleShape::PlainReason(Error)));
		Viewport->RefreshFromKind();
		return;
	}
	PreviewVehicle.Reset(Built);
	Viewport->SetMesh(Built->Mesh.Get(), bFrame);
	Viewport->RefreshFromKind();
}

TSharedRef<SWidget> SMantleVehicleBuilder::MakeDimensionRow(const EMantleVehicleDimension Dimension)
{
	// The slider's range depends on the other sizes (wheels never touch), so it asks again each time it is drawn. Only the slider has it: the
	// spin box's own limits stay unset, because it would snap a typed or dragged number to them before the session sees it, and a size held to its
	// range is the session's to say so (FMantleVehicleShape::Edit writes the note).
	const FText Label = FText::Format(LOCTEXT("DimensionLabel", "{0} (cm)"), FMantleVehicleShape::Label(Dimension));
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 4.f, 8.f, 4.f)
		[
			SNew(SBox).WidthOverride(150.f)
			[
				SNew(STextBlock).Text(Label).ToolTipText(FMantleVehicleShape::Help(Dimension))
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(0.f, 4.f)
		[
			SNew(SSpinBox<float>)
				.MinSliderValue_Lambda([this, Dimension] { return TOptional<float>(static_cast<float>(Shape.GetHeldRange(Dimension).Min)); })
				.MaxSliderValue_Lambda([this, Dimension] { return TOptional<float>(static_cast<float>(Shape.GetHeldRange(Dimension).Max)); })
				.Delta(1.f)
				.MinFractionalDigits(0)
				.MaxFractionalDigits(1)
				.ToolTipText(FMantleVehicleShape::Help(Dimension))
				.Value_Lambda([this, Dimension] { return static_cast<float>(Shape.GetDimensions().Get(Dimension)); })
				.OnValueChanged_Lambda([this, Dimension](float Value) { SetDimension(Dimension, Value); })
				.OnEndSliderMovement_Lambda([this](float) { FlushPreview(); })
				.OnValueCommitted_Lambda([this, Dimension](float Value, ETextCommit::Type)
				{
					SetDimension(Dimension, Value);
					FlushPreview();
				})
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
		[
			SNew(SBox).WidthOverride(64.f)
			[
				SNew(STextBlock)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text_Lambda([this, Dimension] { return FMantleVehicleShape::LengthText(Shape.GetDimensions().Get(Dimension)); })
			]
		];
}

TSharedRef<SWidget> SMantleVehicleBuilder::MakeSizeRow()
{
	// The Size is shaped through the session, which holds it to the preset's own fit range and says so; the slider only covers that range.
	const FText Help = FMantleVehicleShape::SizeHelp(FMantleVehicleShape::FamilyOf(Shape.GetArchetype()));
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 4.f, 8.f, 4.f)
		[
			SNew(SBox).WidthOverride(150.f)
			[
				SNew(STextBlock).Text(FMantleVehicleShape::SizeLabel()).ToolTipText(Help)
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(0.f, 4.f)
		[
			SNew(SSpinBox<float>)
				.MinSliderValue_Lambda([this] { return TOptional<float>(static_cast<float>(Shape.GetSizeRange().Min)); })
				.MaxSliderValue_Lambda([this] { return TOptional<float>(static_cast<float>(Shape.GetSizeRange().Max)); })
				.Delta(0.01f)
				.MinFractionalDigits(2)
				.MaxFractionalDigits(2)
				.ToolTipText(Help)
				.Value_Lambda([this] { return static_cast<float>(Shape.GetSize()); })
				.OnValueChanged_Lambda([this](float Value) { SetShapeSize(Value); })
				.OnEndSliderMovement_Lambda([this](float) { FlushPreview(); })
				.OnValueCommitted_Lambda([this](float Value, ETextCommit::Type)
				{
					SetShapeSize(Value);
					FlushPreview();
				})
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
		[
			SNew(SBox).WidthOverride(64.f)
			[
				SNew(STextBlock)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text_Lambda([this] { return FMantleVehicleShape::SizeText(Shape.GetSize()); })
			]
		];
}

TSharedRef<SWidget> SMantleVehicleBuilder::MakeFeelRow(const EMantleVehicleFeel Feel)
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 4.f, 8.f, 4.f)
		[
			SNew(SBox).WidthOverride(150.f)
			[
				SNew(STextBlock).Text(FMantleVehicleShape::FeelLabel(Feel)).ToolTipText(FMantleVehicleShape::FeelHelp(Feel))
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(0.f, 4.f)
		[
			SNew(SSpinBox<float>)
				.MinSliderValue(-1.f)
				.MaxSliderValue(1.f)
				.Delta(0.05f)
				.MinFractionalDigits(2)
				.MaxFractionalDigits(2)
				.ToolTipText(FMantleVehicleShape::FeelHelp(Feel))
				.Value_Lambda([this, Feel] { return static_cast<float>(Shape.GetFeel(Feel)); })
				.OnValueChanged_Lambda([this, Feel](float Value) { SetFeel(Feel, Value); })
				.OnEndSliderMovement_Lambda([this](float) { FlushPreview(); })
				.OnValueCommitted_Lambda([this, Feel](float Value, ETextCommit::Type)
				{
					SetFeel(Feel, Value);
					FlushPreview();
				})
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
		[
			SNew(SBox).WidthOverride(64.f)
			[
				SNew(STextBlock)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text_Lambda([this, Feel]
					{
						// The slider's end is up to 30 percent (FMantleChassisFeel::Reach) more or less.
						FNumberFormattingOptions Signed;
						Signed.AlwaysSign = true;
						return FText::Format(LOCTEXT("FeelPercent", "{0}%"),
							FText::AsNumber(FMath::RoundToInt(Shape.GetFeel(Feel) * FMantleChassisFeel::Reach * 100.0), &Signed));
					})
			]
		];
}

TSharedRef<SWidget> SMantleVehicleBuilder::MakePassengerRow()
{
	return SNew(SCheckBox)
		.ToolTipText(FMantleVehicleShape::PassengerHelp())
		.IsChecked_Lambda([this] { return Shape.GetPassenger() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { SetPassengerSeat(State == ECheckBoxState::Checked); })
		[
			SNew(STextBlock).Text(FMantleVehicleShape::PassengerLabel())
		];
}

void SMantleVehicleBuilder::AddFeelRows(const TSharedRef<SVerticalBox>& Rows)
{
	bool bAnyFeel = false;
	for (const EMantleVehicleFeel Feel : MantleVehicleFeels)
	{
		if (Shape.HasFeel(Feel))
		{
			bAnyFeel = true;
			Rows->AddSlot().AutoHeight()
			[
				MakeFeelRow(Feel)
			];
		}
	}
	if (Shape.HasPassengerOption())
	{
		Rows->AddSlot().AutoHeight().Padding(0.f, bAnyFeel ? 8.f : 4.f, 0.f, 4.f)
		[
			MakePassengerRow()
		];
	}
}

TSharedRef<SWidget> SMantleVehicleBuilder::MakeShapeStep()
{
	using namespace MantleVehicleBuilderPanelPrivate;
	if (SelectedPresetId.IsEmpty())
	{
		return SNew(STextBlock).Text(LOCTEXT("PickTypeFirst", "Pick what you are building first."));
	}

	ShapeKind = MakeShared<FMantleVehicleShapeKind>();
	ShapeKind->SetSpec(Shape.GetShaped());
	const TSharedRef<SMantleBuilderViewport> Viewport = SNew(SMantleBuilderViewport).Kind(ShapeKind);
	ShapeViewport = Viewport;
	bPreviewDirty = false;
	RebuildPreview(/*bFrame*/ true);

	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
	if (Shape.UsesSize())
	{
		// A plane, helicopter or hovercraft grows as one (its wing, rotor or pads are sized for what it lifts), so it has a Size and not eight sizes;
		// and a seat or a top-speed feel where its type has one.
		Rows->AddSlot().AutoHeight()
		[
			MakeSizeRow()
		];
		AddFeelRows(Rows);
	}
	else
	{
		for (const EMantleVehicleDimension Dimension : MantleVehicleDimensions)
		{
			// A motorcycle's wheels are in a line: it has no track to change.
			if (FMantleVehicleShape::IsEditable(Shape.GetArchetype(), Dimension))
			{
				Rows->AddSlot().AutoHeight()
				[
					MakeDimensionRow(Dimension)
				];
			}
		}
		// How it handles, beyond its sizes: grip, power, suspension and top speed, and the passenger seat where it has one.
		Rows->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 2.f)
		[
			SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("BoldFont"))
				.Text(LOCTEXT("HandlingTitle", "How it feels to drive"))
		];
		AddFeelRows(Rows);
	}

	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 12.f, 0.f)
		[
			SNew(SBox).WidthOverride(400.f)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						Rows
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 8.f)
					[
						SNew(SButton)
							.ToolTipText(LOCTEXT("ResetShapeTip", "Put the shape back to what this type starts with."))
							.IsEnabled_Lambda([this] { return Shape.IsShaped(); })
							.OnClicked(this, &SMantleVehicleBuilder::OnResetShape)
							.Text_Lambda([this] { return FText::Format(LOCTEXT("ResetShape", "Reset to {0}"), SelectedPresetName); })
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
					[
						SNew(STextBlock)
							.Font(FAppStyle::GetFontStyle("BoldFont"))
							.AutoWrapText(true)
							.Text_Lambda([this] { return Shape.GetSummary(); })
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
					[
						SNew(STextBlock)
							.AutoWrapText(true)
							.ColorAndOpacity(Warn)
							.Visibility_Lambda([this] { return Shape.GetWarnings().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
							.Text_Lambda([this] { return Shape.GetWarnings(); })
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
					[
						SNew(STextBlock)
							.AutoWrapText(true)
							.ColorAndOpacity(FSlateColor(Bad))
							.Visibility_Lambda([this] { return PreviewProblem.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
							.Text_Lambda([this] { return PreviewProblem; })
					]
				]
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f)
		[
			SNew(SBox).MinDesiredHeight(320.f)
			[
				Viewport
			]
		];
}

// ---------------------------------------------------------------------------------------------------------------------
// 3. Drive it.

TSharedRef<SWidget> SMantleVehicleBuilder::MakeDriveStep()
{
	using namespace MantleVehicleBuilderPanelPrivate;
	return SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
			[
				SNew(STextBlock)
					.AutoWrapText(true)
					.Text_Lambda([this] { return FText::Format(LOCTEXT("DriveSummary", "Your {0}. {1}"), SelectedPresetName, Shape.GetSummary()); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
			[
				SNew(STextBlock)
					.AutoWrapText(true)
					.ColorAndOpacity(Warn)
					.Visibility_Lambda([this] { return FMantleVehicleShape::DriveAdvice(Shape.GetArchetype()).IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
					.Text_Lambda([this] { return FMantleVehicleShape::DriveAdvice(Shape.GetArchetype()); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 8.f)
			[
				SNew(SBox).HeightOverride(44.f)
				[
					SNew(SButton)
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
						.OnClicked(this, &SMantleVehicleBuilder::OnPutGreyInLevel)
						.ToolTipText(LOCTEXT("PutGreyTip", "Saves your vehicle in your project and adds it to the level you have open, in front of you. Then press Test Locally to drive it."))
						[
							SNew(STextBlock)
								.Font(FAppStyle::GetFontStyle("BoldFont"))
								.Text(LOCTEXT("PutGreyButton", "Put it in my level"))
						]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
			[
				SNew(STextBlock)
					.AutoWrapText(true)
					.Visibility_Lambda([this] { return DriveStatus.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
					.ColorAndOpacity_Lambda([this] { return bDriveFailed ? FSlateColor(Bad) : FSlateColor(Good); })
					.Text_Lambda([this] { return DriveStatus; })
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(LOCTEXT("DriveNext", "Next: press Test Locally (in the toolbar, next to Play) to drive it. Changed your mind about the shape? Go back, change it, and press Put it in my level again: the vehicle in your level is updated."))
			]
		];
}

FReply SMantleVehicleBuilder::OnPutGreyInLevel()
{
	using namespace MantleVehicleBuilderPanelPrivate;
	if (SelectedPresetId.IsEmpty())
	{
		return FReply::Handled();
	}
	bDriveFailed = true;

	// A new vehicle takes the first free name; the same one again (the shape changed) is remade under its own.
	if (GreyName.IsEmpty() || GreyPresetId != SelectedPresetId)
	{
		GreyName = FMantleVehicleBuilder::FirstFreeChassisName(SelectedPresetName.ToString());
		GreyPresetId = SelectedPresetId;
	}
	// The same sizes as the vehicle already made for them: nothing to remake, only to put in the level (or to find there).
	const FString Signature = GreyPresetId + TEXT("|") + GreyName + TEXT("|") + Shape.Signature();
	UMantleVehicleSource* Source = GreySource.Get();
	const bool bRemade = !Source || Signature != GreySignature;
	if (bRemade)
	{
		const FMantleVehicleBuildResult Result = FMantleVehicleBuilder::BuildChassis(Shape.GetShaped(), FMantleVehicleBuilder::ChassisFolder() / GreyName, GreyName);
		if (!Result.bOk || !Result.Source)
		{
			DriveStatus = FText::Format(LOCTEXT("GreyNotMade", "Your vehicle could not be made. {0}"), JoinSentences(Result.Problems));
			return FReply::Handled();
		}
		Source = Result.Source;
		GreySource = Source;
		GreySignature = Signature;
		GreyExtra.Reset();
		if (Result.bSaved)
		{
			GreyExtra = FString::Printf(TEXT(" Saved as %s in the MantleVehicles folder of your Content Browser."), *GreyName);
		}
		else if (!Result.SaveProblem.IsEmpty())
		{
			GreyExtra = FString::Printf(TEXT(" Not saved: %s"), *Result.SaveProblem);
		}
		for (const FString& Note : Result.Notes)
		{
			GreyExtra += TEXT(" ") + Note;
		}
		bGreyNotSaved = !Result.bSaved && !Result.SaveProblem.IsEmpty();
	}

	// One spawn for the vehicle: a second press updates it (the spawn names the asset, which was just remade) instead of adding another.
	FString Problem;
	FText Said;
	AMantleVehicleSpawnPoint* Placed = nullptr;
	if (AMantleVehicleSpawnPoint* Existing = FMantleVehicleBuilder::FindSpawn(*Source))
	{
		GreySpawns.AddUnique(Existing);
		if (GEditor)
		{
			GEditor->SelectNone(/*bNoteSelectionChange*/ false, /*bDeselectBSPSurfs*/ true);
			GEditor->SelectActor(Existing, /*bInSelected*/ true, /*bNotify*/ true);
		}
		Said = bRemade ? FText::Format(LOCTEXT("GreyUpdated", "Your {0} in your level is updated to the new shape."), SelectedPresetName)
			: FText::Format(LOCTEXT("GreyAlreadyThere", "Your {0} is already in your level, and nothing about it has changed."), SelectedPresetName);
	}
	else if (FMantleVehicleBuilder::PlaceInLevel(*Source, Problem, &Placed, &GreyPlaceNote))
	{
		// Remembered, so Cancel (or Done with a model on it instead) takes it out of the level again.
		if (Placed)
		{
			GreySpawns.AddUnique(Placed);
		}
		Said = FText::Format(LOCTEXT("GreyPlaced", "Your {0} is in your level."), SelectedPresetName);
	}
	else
	{
		DriveStatus = FText::FromString(Problem);
		return FReply::Handled();
	}
	DriveStatus = FText::Format(LOCTEXT("GreyStatus", "{0}{1}{2} Press Test Locally (in the toolbar, next to Play) to drive it."), Said, FText::FromString(GreyExtra),
		FText::FromString(GreyPlaceNote.IsEmpty() ? FString() : FString(TEXT(" ")) + GreyPlaceNote));
	bDriveFailed = bGreyNotSaved;
	return FReply::Handled();
}

// ---------------------------------------------------------------------------------------------------------------------
// 4. Use your own model (optional): the model flow.

TSharedRef<SWidget> SMantleVehicleBuilder::MakeModelStep()
{
	using namespace MantleVehicleBuilderPanelPrivate;
	ResultBox = SNew(SVerticalBox);

	TSharedRef<SWidget> Content = SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(SObjectPropertyEntryBox)
					.AllowedClass(UStreamableRenderAsset::StaticClass())
					.OnShouldFilterAsset_Lambda([](const FAssetData& Asset)
					{
						// Only models are listed; a solid one is accepted so the window can say why it won't do.
						const UClass* AssetClass = Asset.GetClass();
						return !AssetClass || !(AssetClass->IsChildOf(USkeletalMesh::StaticClass()) || AssetClass->IsChildOf(UStaticMesh::StaticClass()));
					})
					.AllowClear(true)
					.DisplayThumbnail(true)
					.ObjectPath_Lambda([this] { return Mesh.IsValid() ? Mesh->GetPathName() : FString(); })
					.OnObjectChanged_Lambda([this](const FAssetData& Asset) { SetMesh(Asset); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SButton)
						.ContentPadding(FMargin(12, 4))
						.OnClicked(this, &SMantleVehicleBuilder::OnPickFbx)
						.ToolTipText(LOCTEXT("PickFbxTip", "Choose an .fbx file on your computer. Mantle imports it into your project (under MantleVehicles > Imported) and picks it here."))
						.Text(LOCTEXT("PickFbx", "Pick an .fbx file..."))
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(8, 0, 0, 0)
				[
					SNew(STextBlock)
						.AutoWrapText(true)
						.ColorAndOpacity_Lambda([this]() { return bImportFailed ? FSlateColor(Bad) : FSlateColor::UseSubduedForeground(); })
						.Visibility_Lambda([this]() { return ImportStatus.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
						.Text_Lambda([this]() { return ImportStatus; })
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor(Bad))
					.Visibility_Lambda([this]() { return MeshProblem.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
					.Text_Lambda([this]() { return MeshProblem; })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(STextBlock)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(LOCTEXT("ModelHelp", "Pick the model you imported or bought: a Static Mesh or a Skeletal Mesh. A car whose wheels are separate parts named like Wheel_FL and Wheel_FR is fitted to its own wheels. Any other model, such as a spaceship, a plane or a bike, goes on the grey vehicle as its skin: the grey vehicle stays the physics, and you can point out which parts of your model are its wheels, propellers or rotors so they move. Have an FBX file? Press Pick an .fbx file, and Mantle imports it and picks it for you."))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 16)
			[
				SAssignNew(ModelRowsBox, SVerticalBox)
			]
			// #124: the library card's picture, the same Thumbnail section every Mantle uploader shows.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
			[
				SAssignNew(ThumbnailSection, SMantleThumbnailSection)
					.Target(FMantleThumbnailTarget::Square())
					.AngleSliders(true)
					.DefaultAngle(FMantleModelThumbnail::DefaultView())
					.Choice_Lambda([this] { return ThumbnailChoice; })
					.OnChoiceChanged_Raw(this, &SMantleVehicleBuilder::ThumbnailChoiceChanged)
					.Render(FMantleVehicleThumbnail::PreviewFor([this]() -> const UMantleVehicleSource* { return BuiltSource.Get(); }, [this]() -> UObject* { return Mesh.Get(); }))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 12)
			[
				SNew(SBox).HeightOverride(44.f)
				[
					SNew(SButton)
						.HAlign(HAlign_Center)
						.VAlign(VAlign_Center)
						.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
						.IsEnabled_Raw(this, &SMantleVehicleBuilder::CanBuild)
						.OnClicked(this, &SMantleVehicleBuilder::OnBuild)
						.ToolTipText(LOCTEXT("BuildTooltip", "Pick a vehicle type and a model first, then press this."))
						[
							SNew(STextBlock)
								.Font(FAppStyle::GetFontStyle("BoldFont"))
								.Text(LOCTEXT("BuildButton", "Build my vehicle"))
						]
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				ResultBox.ToSharedRef()
			]
		];

	// The step is built again each time it is opened: the model's rows and what the last build said are shown again.
	RebuildModelRows();
	if (bHaveLastBuild)
	{
		ShowResult(LastBuild);
	}
	return Content;
}

FReply SMantleVehicleBuilder::OnPickFbx()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		ImportStatus = LOCTEXT("NoFileDialog", "This computer can't open a file picker here. Drag your .fbx into the Content Browser to import it, then pick it from the list.");
		bImportFailed = true;
		return FReply::Handled();
	}
	const void* ParentWindow = FSlateApplication::IsInitialized() ? FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr) : nullptr;
	TArray<FString> Files;
	if (!DesktopPlatform->OpenFileDialog(ParentWindow, TEXT("Pick your model (.fbx)"), LastFbxFolder, FString(), TEXT("FBX model (*.fbx)|*.fbx"), EFileDialogFlags::None, Files)
		|| Files.IsEmpty())
	{
		// Cancelled: nothing changes and there is nothing to say.
		return FReply::Handled();
	}
	LastFbxFolder = FPaths::GetPath(Files[0]);
	ImportFbx(Files[0]);
	return FReply::Handled();
}

void SMantleVehicleBuilder::ImportFbx(const FString& File)
{
	FString Error;
	UObject* Model = FMantleVehicleBuilder::ImportModelFile(File, Error);
	if (!Model)
	{
		ImportStatus = FText::FromString(Error);
		bImportFailed = true;
		return;
	}
	bImportFailed = false;
	ImportStatus = FText::Format(LOCTEXT("Imported", "Imported {0} into {1}. It is picked below."), FText::FromString(Model->GetName()),
		FText::FromString(FPackageName::GetLongPackagePath(Model->GetOutermost()->GetName())));
	SetMesh(FAssetData(Model));
}

bool SMantleVehicleBuilder::CanBuild() const
{
	return !SelectedPresetId.IsEmpty() && Mesh.IsValid();
}

void SMantleVehicleBuilder::ForgetBuild()
{
	BuiltSource.Reset();
	if (ThumbnailSection.IsValid())
	{
		ThumbnailSection->Refresh();
	}
	LastBuild = FMantleVehicleBuildResult();
	bHaveLastBuild = false;
	SuggestedSize.Reset();
	SaveStatus = FText::GetEmpty();
	bSaveFailed = false;
	// What the page showed was about the last model; the vehicle built from it must not be put in the level for this one.
	PlaceResultBox.Reset();
	if (ResultBox.IsValid())
	{
		ResultBox->ClearChildren();
	}
}

FVector2f SMantleVehicleBuilder::ModelSizeRange() const
{
	const FMantleVehiclePreset* Preset = FMantleVehiclePresets::Find(SelectedPresetId);
	if (Preset && Preset->FitMinScale > 0.0 && Preset->FitMaxScale >= Preset->FitMinScale)
	{
		return FVector2f(static_cast<float>(Preset->FitMinScale), static_cast<float>(Preset->FitMaxScale));
	}
	return FVector2f(0.25f, 4.0f);
}

void SMantleVehicleBuilder::SetMesh(const FAssetData& Asset)
{
	UObject* Picked = Asset.GetAsset();
	UObject* Model = Cast<USkeletalMesh>(Picked) || Cast<UStaticMesh>(Picked) ? Picked : nullptr;
	// Another model (or none) is another vehicle: the last build was of the old one, so it is forgotten and cannot be placed by mistake.
	if (!Model || Model != Mesh.Get())
	{
		ForgetBuild();
	}
	Mesh = Model;
	MeshProblem = FText::GetEmpty();
	LoadThumbnailChoice();
	ReadModel();
	if (Asset.IsValid() && !Model)
	{
		// The picker lets any model through so a wrong one can be explained here, instead of being refused without a word.
		MeshProblem = LOCTEXT("NotAModel", "That isn't a model. Pick a Static Mesh or a Skeletal Mesh.");
	}
}

void SMantleVehicleBuilder::LoadThumbnailChoice()
{
	FMantleThumbnailStore::Load(Mesh.IsValid() ? Mesh->GetPathName() : FString(), TEXT("vehicle"), ThumbnailChoice, FMantleThumbnailStore::DefaultConfigFile());
	if (ThumbnailSection.IsValid())
	{
		ThumbnailSection->Refresh();
	}
}

void SMantleVehicleBuilder::ThumbnailChoiceChanged(const FMantleThumbnailChoice& Choice)
{
	ThumbnailChoice = Choice;
	FMantleThumbnailStore::Save(Mesh.IsValid() ? Mesh->GetPathName() : FString(), TEXT("vehicle"), Choice, FMantleThumbnailStore::DefaultConfigFile());
}

void SMantleVehicleBuilder::ApplyThumbnailChoice(FMantleVehicleBuildResult& Result)
{
	if (!Result.bOk || !Result.Source || ThumbnailChoice.Mode == EMantleThumbnailMode::Automatic)
	{
		return;
	}
	// The build took the automatic picture by itself; the creator's choice replaces it.
	const FMantleThumbnailResult Picture = FMantleThumbnailResolver::Resolve(ThumbnailChoice, FMantleThumbnailTarget::Square(),
		FMantleModelThumbnail::RenderFor([this]() -> UObject* { return Mesh.Get(); }));
	if (!Picture.Succeeded())
	{
		Result.Notes.Add(FString::Printf(TEXT("Your thumbnail choice could not be used (%s), so the vehicle keeps the automatic picture."), *Picture.Error));
		return;
	}
	Result.Source->Modify();
	Result.Source->ThumbnailPng = FMantleThumbnailResolver::ToPng(Picture, FMantleThumbnailTarget::Square());
	Result.Source->MarkPackageDirty();
	Result.Notes.Add(Picture.Warning.IsEmpty() ? Picture.Source : FString::Printf(TEXT("%s (%s)"), *Picture.Source, *Picture.Warning));
	if (Result.bSaved)
	{
		Result.bSaved = FMantleVehicleBuilder::SaveVehicle(*Result.Source, Result.SaveProblem);
	}
}

void SMantleVehicleBuilder::SetTwinEngine(const bool bOn)
{
	if (bOn == bTwinEngine)
	{
		return;
	}
	ForgetBuild();
	ReadModel(TOptional<bool>(bOn));
}

void SMantleVehicleBuilder::ReadModel(const TOptional<bool> Twin)
{
	bHaveModelInfo = false;
	bUseSkin = false;
	bTwinEngine = false;
	PartSlots.Reset();
	PartMap = FMantleVehicleModelMap();
	ModelYaw = 0.0;
	ProposalText.Reset();
	UObject* Picked = Mesh.Get();
	if (!Picked || SelectedPresetId.IsEmpty())
	{
		RebuildModelRows();
		return;
	}
	FString Error;
	if (!FMantleVehicleModel::Read(Picked, ModelInfo, Error))
	{
		MeshProblem = FText::FromString(Error);
		RebuildModelRows();
		return;
	}
	bHaveModelInfo = true;
	bUseSkin = !FMantleVehicleBuilder::FitsOwnWheels(Picked, SelectedPresetId);
	if (bUseSkin)
	{
		// A plane with two propellers is a twin: the grey vehicle has a propeller bone for each. The shape starts again from that chassis.
		FMantleChassisSpec Base;
		if (FMantleVehicleChassisBuilder::LoadSpec(SelectedPresetId, Base, Error))
		{
			bTwinEngine = FMantleVehicleChassisSkin::CanBeTwin(Base) && Twin.Get(FMantleVehicleChassisSkin::CountPropellers(ModelInfo) == 2);
			if ((Shape.GetArchetype().Aircraft.EngineCount == 2) != bTwinEngine)
			{
				const FMantleVehiclePreset* PresetPicked = FMantleVehiclePresets::Find(SelectedPresetId);
				Shape.Start(bTwinEngine ? FMantleVehicleChassisSkin::AsTwin(Base) : Base, PresetPicked ? PresetPicked->FitMinScale : 0.0, PresetPicked ? PresetPicked->FitMaxScale : 0.0);
				ShapeChanged();
			}
		}
		FMantleVehicleAsset Grey;
		if (FMantleVehicleChassisBuilder::MakeAsset(Shape.GetArchetype(), Grey, Error))
		{
			PartSlots = FMantleVehicleChassisSkin::PartSlots(Shape.GetArchetype(), Grey.Body);
			ModelYaw = FMantleVehicleChassisSkin::SuggestMapAndYaw(PartSlots, ModelInfo, PartMap);
		}
		ProposeShape();
	}
	RebuildModelRows();
}

void SMantleVehicleBuilder::ProposeShape()
{
	const FMantleVehiclePreset* Preset = FMantleVehiclePresets::Find(SelectedPresetId);
	if (!Preset || !bHaveModelInfo || !Shape.IsStarted())
	{
		return;
	}
	const FMantleVehicleModelProposal Proposal = FMantleVehicleChassisSkin::Propose(Shape.GetArchetype(), *Preset, ModelInfo, ModelYaw, PartMap);
	bool bChanged = false;
	if (Proposal.bUsesSize)
	{
		bChanged |= Shape.SetSize(Proposal.Size);
	}
	for (const TPair<EMantleVehicleDimension, double>& Dimension : Proposal.Dimensions)
	{
		bChanged |= Shape.SetDimension(Dimension.Key, Dimension.Value);
	}
	ProposalText = FString::Join(Proposal.Notes, TEXT(" "));
	if (bChanged)
	{
		ShapeChanged();
	}
}

void SMantleVehicleBuilder::SetModelYaw(const double Degrees)
{
	if (FMath::IsNearlyEqual(ModelYaw, Degrees))
	{
		return;
	}
	ModelYaw = Degrees;
	// The model's length and width change with the way it faces, and so does the size the chassis is proposed at.
	ForgetBuild();
	ProposeShape();
	RebuildModelRows();
}

void SMantleVehicleBuilder::SetPart(const FName ChassisBone, const FMantleVehicleModelPiece& Piece)
{
	if (Piece.IsSet())
	{
		PartMap.Parts.Add(ChassisBone, Piece);
	}
	else
	{
		PartMap.Parts.Remove(ChassisBone);
	}
	// Assigned wheels place the chassis's wheels: the sizes are proposed again; what was built is of the old assignment.
	ForgetBuild();
	ProposeShape();
	RebuildModelRows();
}

void SMantleVehicleBuilder::RebuildModelRows()
{
	using namespace MantleVehicleBuilderPanelPrivate;
	if (!ModelRowsBox.IsValid())
	{
		return;
	}
	ModelRowsBox->ClearChildren();
	if (!bHaveModelInfo)
	{
		return;
	}
	ModelRowsBox->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
	[
		SNew(STextBlock)
			.AutoWrapText(true)
			.Text(FText::Format(LOCTEXT("ModelIs", "Your model is {0}."), FText::FromString(ModelInfo.Describe())))
	];
	if (!bUseSkin)
	{
		ModelRowsBox->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
		[
			SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(LOCTEXT("OwnWheels", "Its wheels are named like a car's, so Mantle fits the grey vehicle to them and your wheels turn as they are."))
		];
		return;
	}

	// Which way the front of the model faces. Unreal's own axes, as the viewport's gizmo shows them: X red, Y green.
	YawChoices.Reset();
	YawValues.Reset();
	const TPair<const TCHAR*, double> Facings[] = {
		{ TEXT("+X (the red arrow: how Unreal models usually face)"), 0.0 }, { TEXT("+Y (the green arrow)"), 270.0 }, { TEXT("-X (against the red arrow)"), 180.0 },
		{ TEXT("-Y (against the green arrow)"), 90.0 } };
	int32 Current = 0;
	for (const TPair<const TCHAR*, double>& Facing : Facings)
	{
		if (FMath::IsNearlyEqual(FRotator::NormalizeAxis(Facing.Value), FRotator::NormalizeAxis(ModelYaw)))
		{
			Current = YawChoices.Num();
		}
		YawChoices.Add(MakeShared<FString>(Facing.Key));
		YawValues.Add(Facing.Value);
	}
	ModelRowsBox->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 2.f, 8.f, 2.f)
		[
			SNew(SBox).WidthOverride(150.f)
			[
				SNew(STextBlock)
					.Text(LOCTEXT("FacingLabel", "The front of my model faces"))
					.ToolTipText(LOCTEXT("FacingTip", "Which of Unreal's axes the nose (or the front bumper) of your model points along. Mantle turns the model so it points forward on the vehicle."))
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
		[
			SNew(SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&YawChoices)
				.InitiallySelectedItem(YawChoices[Current])
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) { return SNew(STextBlock).Text(FText::FromString(*Item)); })
				.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type)
				{
					const int32 At = YawChoices.IndexOfByKey(Item);
					if (At != INDEX_NONE)
					{
						SetModelYaw(YawValues[At]);
					}
				})
				[
					SNew(STextBlock).Text_Lambda([this]
					{
						for (int32 At = 0; At < YawChoices.Num(); ++At)
						{
							if (FMath::IsNearlyEqual(FRotator::NormalizeAxis(YawValues[At]), FRotator::NormalizeAxis(ModelYaw)))
							{
								return FText::FromString(*YawChoices[At]);
							}
						}
						return FText::GetEmpty();
					})
				]
		]
	];

	// A propeller plane may have two engines, each with a propeller of its own.
	if (Shape.GetArchetype().Aircraft.bSet && Shape.GetArchetype().Aircraft.bPropeller)
	{
		ModelRowsBox->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
		[
			SNew(SCheckBox)
				.ToolTipText(LOCTEXT("TwinTip", "Turn this on if your plane has two propellers, like a P-38. The grey plane then has two engines and you can point out each propeller."))
				.IsChecked_Lambda([this] { return bTwinEngine ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { SetTwinEngine(State == ECheckBoxState::Checked); })
				[
					SNew(STextBlock).Text(LOCTEXT("Twin", "My plane has two engines and two propellers"))
				]
		];
	}

	// What Mantle proposed for the chassis, and the Size slider of a type shaped by one Size (the sizes of a road vehicle are in the Shape step).
	if (!ProposalText.IsEmpty())
	{
		ModelRowsBox->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
		[
			SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(FText::FromString(ProposalText))
		];
	}
	if (Shape.UsesSize())
	{
		ModelRowsBox->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
		[
			MakeSizeRow()
		];
	}
	else
	{
		ModelRowsBox->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
		[
			SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(LOCTEXT("SizesInShape", "The grey vehicle's sizes now match your model. Change any of them in the Shape it step; your model is scaled to fit."))
		];
	}

	// The parts of the model that move: one row for each moving part of the chassis.
	if (PartSlots.Num() > 0)
	{
		PieceChoices.Reset();
		PieceValues.Reset();
		PieceChoices.Add(MakeShared<FString>(TEXT("(nothing: it stays still)")));
		PieceValues.Add(FMantleVehicleModelPiece());
		for (const FMantleVehicleModelPiece& Piece : FMantleVehicleChassisSkin::Pieces(ModelInfo))
		{
			PieceChoices.Add(MakeShared<FString>(FMantleVehicleChassisSkin::DescribePiece(ModelInfo, Piece)));
			PieceValues.Add(Piece);
		}
		ModelRowsBox->AddSlot().AutoHeight().Padding(0, 8, 0, 2)
		[
			SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("BoldFont"))
				.Text(LOCTEXT("PartsTitle", "Parts that move (optional)"))
		];
		ModelRowsBox->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
		[
			SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(LOCTEXT("PartsHelp", "Point out which part of your model is each of these, so it moves. A bone picks that bone and everything attached to it; a section picks one material of the mesh. Leave a row at (nothing) and your model still drives or flies, that part just does not move."))
		];
		for (const FMantleVehiclePartSlot& Slot : PartSlots)
		{
			const FMantleVehicleModelPiece* Assigned = PartMap.Find(Slot.ChassisBone);
			int32 Selected = 0;
			for (int32 At = 1; Assigned && At < PieceValues.Num(); ++At)
			{
				if (PieceValues[At] == *Assigned)
				{
					Selected = At;
				}
			}
			const FName ChassisBone = Slot.ChassisBone;
			ModelRowsBox->AddSlot().AutoHeight().Padding(0, 1)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 2.f, 8.f, 2.f)
				[
					SNew(SBox).WidthOverride(150.f)
					[
						SNew(STextBlock).Text(Slot.Label).ToolTipText(Slot.Help).AutoWrapText(true)
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(SComboBox<TSharedPtr<FString>>)
						.OptionsSource(&PieceChoices)
						.InitiallySelectedItem(PieceChoices[Selected])
						.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) { return SNew(STextBlock).Text(FText::FromString(*Item)); })
						.OnSelectionChanged_Lambda([this, ChassisBone](TSharedPtr<FString> Item, ESelectInfo::Type Info)
						{
							// Opening the list and setting its first choice is not a pick.
							const int32 At = PieceChoices.IndexOfByKey(Item);
							if (At != INDEX_NONE && Info != ESelectInfo::Direct)
							{
								SetPart(ChassisBone, PieceValues[At]);
							}
						})
						[
							SNew(STextBlock).Text_Lambda([this, ChassisBone]
							{
								const FMantleVehicleModelPiece* Piece = PartMap.Find(ChassisBone);
								return FText::FromString(Piece ? FMantleVehicleChassisSkin::DescribePiece(ModelInfo, *Piece) : FString(TEXT("(nothing: it stays still)")));
							})
						]
				]
			];
		}
	}
}

FMantleVehicleModelOptions SMantleVehicleBuilder::MakeBuildOptions() const
{
	// A model with the type's wheels is fitted to them; any other is worn on the grey vehicle as its skin, with the shape, the facing and the parts as the page has them.
	FMantleVehicleModelOptions Options;
	if (bUseSkin)
	{
		Options.Spec = Shape.GetShaped();
		Options.YawDegrees = ModelYaw;
		Options.Map = PartMap;
		// Pressing Build again for the same type and model updates the vehicle this window made, in the same assets, instead of adding a new set each time.
		if (!BuiltAssetName.IsEmpty() && BuiltAssetKey == SelectedPresetId + TEXT("|") + (Mesh.IsValid() ? Mesh->GetPathName() : FString()))
		{
			Options.Name = BuiltAssetName;
			Options.bOverwrite = true;
		}
	}
	return Options;
}

void SMantleVehicleBuilder::RememberBuilt(const FMantleVehicleBuildResult& Result)
{
	if (!Result.AssetName.IsEmpty())
	{
		BuiltAssetName = Result.AssetName;
		BuiltAssetKey = SelectedPresetId + TEXT("|") + (Mesh.IsValid() ? Mesh->GetPathName() : FString());
	}
}

FReply SMantleVehicleBuilder::OnBuild()
{
	if (!CanBuild())
	{
		return FReply::Handled();
	}
	FMantleVehicleBuildResult Result = FMantleVehicleBuilder::BuildAny(Mesh.Get(), SelectedPresetId, MakeBuildOptions());
	ApplyThumbnailChoice(Result);
	if (Result.bOk && Result.Source)
	{
		RememberBuilt(Result);
		BuiltSource = Result.Source;
		bSaveFailed = !Result.bSaved && !Result.SaveProblem.IsEmpty();
		SaveStatus = Result.bSaved ? LOCTEXT("Saved", "Saved in your Content Browser.")
			: bSaveFailed ? FText::Format(LOCTEXT("NotSavedBuild", "Not saved: {0}"), FText::FromString(Result.SaveProblem))
			: FText::GetEmpty();
		SuggestedSize = Result.Fit.IsSet() ? TOptional<float>(static_cast<float>(Result.Fit->Scale)) : TOptional<float>();
		if (ThumbnailSection.IsValid())
		{
			// Automatic shows the picture this build took.
			ThumbnailSection->Refresh();
		}
	}
	// The step shows the notes and the Size again when it is opened again; the vehicle itself it reaches through BuiltSource, which does not dangle.
	LastBuild = Result;
	LastBuild.Source = nullptr;
	bHaveLastBuild = true;
	ShowResult(Result);
	return FReply::Handled();
}

void SMantleVehicleBuilder::ShowResult(const FMantleVehicleBuildResult& Result)
{
	using namespace MantleVehicleBuilderPanelPrivate;

	ResultBox->ClearChildren();
	// The person in the seat is a pilot in a plane or a helicopter, a rider on a bike and a driver otherwise.
	const FMantleChassisSpec& Seated = Shape.GetArchetype();
	const FText SeatedWord = Seated.Aircraft.bSet || Seated.Helicopter.bSet ? LOCTEXT("SeatPilot", "pilot") : Seated.bCentreline ? LOCTEXT("SeatRider", "rider") : LOCTEXT("SeatDriver", "driver");
	const FText SizeTip = FText::Format(LOCTEXT("SizeTip", "How big the whole vehicle is in Mantle. 1 means as modelled. Mantle suggests a size so its {0} fits."), SeatedWord);
	if (Result.bOk)
	{
		ResultBox->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
		[
			SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("BoldFont"))
				.ColorAndOpacity(FSlateColor(Good))
				.AutoWrapText(true)
				.Text(FText::Format(LOCTEXT("Ready", "Your {0} is ready."), SelectedPresetName))
		];
		for (const FString& Note : Result.Notes)
		{
			ResultBox->AddSlot().AutoHeight().Padding(8, 0, 0, 4)
			[
				SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(Note))
			];
		}
		if (Result.bOnChassis)
		{
			// The Size of a vehicle worn on the grey chassis is the chassis's own: it was set above and the build made the numbers for it.
			ResultBox->AddSlot().AutoHeight().Padding(0, 8, 0, 8)
			[
				SNew(STextBlock)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(LOCTEXT("ChassisSizeNote", "Its size is the grey vehicle's. To change it, use the Size or the sizes above (or in the Shape it step) and press Build my vehicle again."))
			];
		}
		else
		{
			ResultBox->AddSlot().AutoHeight().Padding(0, 8, 0, 4)
			[
				SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("BoldFont"))
					.ToolTipText(SizeTip)
					.Text(LOCTEXT("SizeLabel", "Size"))
			];
			if (Result.Fit.IsSet())
			{
				ResultBox->AddSlot().AutoHeight().Padding(8, 0, 0, 4)
				[
					SNew(STextBlock)
						.AutoWrapText(true)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Text(FText::FromString(Result.Fit->Reason))
				];
			}
			ResultBox->AddSlot().AutoHeight().Padding(8, 0, 0, 8)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(SSpinBox<float>)
						.MinValue(ModelSizeRange().X)
						.MaxValue(ModelSizeRange().Y)
						.MinSliderValue(ModelSizeRange().X)
						.MaxSliderValue(ModelSizeRange().Y)
						.Delta(0.01f)
						.MinFractionalDigits(2)
						.MaxFractionalDigits(2)
						.ToolTipText(SizeTip)
						.Value_Lambda([this]() { const UMantleVehicleSource* Source = BuiltSource.Get(); return Source ? Source->FitScale : 1.f; })
						.OnBeginSliderMovement(this, &SMantleVehicleBuilder::BeginSizeDrag)
						.OnEndSliderMovement(this, &SMantleVehicleBuilder::EndSizeDrag)
						.OnValueChanged(this, &SMantleVehicleBuilder::SetSize)
						.OnValueCommitted_Lambda([this](float NewSize, ETextCommit::Type) { SetSize(NewSize); SaveNow(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0).VAlign(VAlign_Center)
				[
					SNew(SButton)
						.ContentPadding(FMargin(8, 2))
						.IsEnabled_Lambda([this]() { return SuggestedSize.IsSet(); })
						.OnClicked(this, &SMantleVehicleBuilder::OnResetSize)
						.ToolTipText(LOCTEXT("ResetSizeTip", "Go back to the size Mantle suggested for this vehicle."))
						.Text(LOCTEXT("ResetSize", "Reset to suggested"))
				]
			];
		}
		ResultBox->AddSlot().AutoHeight().Padding(8, 0, 0, 8)
		[
			SNew(SCheckBox)
				.ToolTipText(LOCTEXT("NoInsideTip", "Turn this on if your model is just the outside shell, with no seats or steering wheel inside. People in it won't be drawn while they ride."))
				.IsChecked_Lambda([this]()
				{
					const UMantleVehicleSource* Source = BuiltSource.Get();
					return Source && Source->Tuning.bHideDriver && Source->Tuning.bHidePassenger ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
					{
						if (UMantleVehicleSource* Source = BuiltSource.Get())
						{
							FMantleVehicleBuilder::SetHiddenRiders(*Source, Cast<USkeletalMesh>(Mesh.Get()), State == ECheckBoxState::Checked);
						}
						SaveNow();
					})
				[
					SNew(STextBlock).Text(LOCTEXT("NoInside", "This model has no inside — hide the driver and passenger"))
				]
		];
		ResultBox->AddSlot().AutoHeight().Padding(0, 8, 0, 4)
		[
			SNew(STextBlock)
				.AutoWrapText(true)
				.Visibility_Lambda([this]() { return SaveStatus.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				.ColorAndOpacity_Lambda([this]() { return bSaveFailed ? FSlateColor(Bad) : FSlateColor::UseSubduedForeground(); })
				.Text_Lambda([this]() { return SaveStatus; })
		];
		ResultBox->AddSlot().AutoHeight().Padding(0, 8, 0, 8)
		[
			SNew(SBox).HeightOverride(44.f)
			[
				SNew(SButton)
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.OnClicked(this, &SMantleVehicleBuilder::OnPlaceInLevel)
					.ToolTipText(LOCTEXT("PlaceTip", "Adds your vehicle to the level you have open, in front of you. Then press Test Locally to drive it."))
					[
						SNew(STextBlock)
							.Font(FAppStyle::GetFontStyle("BoldFont"))
							.Text(LOCTEXT("PlaceButton", "Put it in my level"))
					]
			]
		];
		PlaceResultBox = SNew(SVerticalBox);
		ResultBox->AddSlot().AutoHeight().Padding(0, 0, 0, 8)
		[
			PlaceResultBox.ToSharedRef()
		];

		ResultBox->AddSlot().AutoHeight().HAlign(HAlign_Left)
		[
			SNew(SButton)
				.ContentPadding(FMargin(12, 6))
				.OnClicked(this, &SMantleVehicleBuilder::OnShowSettings)
				.Text(LOCTEXT("ShowSettings", "Show its settings"))
		];
		ResultBox->AddSlot().AutoHeight().Padding(0, 10, 0, 0)
		[
			SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(LOCTEXT("NextStep", "Next: press Put it in my level, then press Test Locally (in the toolbar, next to Play) to drive it."))
		];
		return;
	}

	ResultBox->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
	[
		SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("BoldFont"))
			.ColorAndOpacity(FSlateColor(Bad))
			.AutoWrapText(true)
			.Text(LOCTEXT("NotReady", "Your vehicle could not be built yet."))
	];
	for (const FString& Problem : Result.Problems)
	{
		ResultBox->AddSlot().AutoHeight().Padding(8, 0, 0, 4)
		[
			SNew(STextBlock)
				.ColorAndOpacity(FSlateColor(Bad))
				.AutoWrapText(true)
				.Text(FText::FromString(Problem))
		];
	}
	ResultBox->AddSlot().AutoHeight().Padding(0, 8, 0, 0)
	[
		SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("TryAgain", "Fix what is listed above, then press Build my vehicle again."))
	];
}

void SMantleVehicleBuilder::BeginSizeDrag()
{
	SizeDragTransaction = MakeShared<FScopedTransaction>(LOCTEXT("ChangeSizeDrag", "Change vehicle size"));
}

void SMantleVehicleBuilder::EndSizeDrag(float)
{
	SizeDragTransaction.Reset();
	// The drag is over, so this is the one save for the whole drag; none happened while the slider moved.
	SaveNow();
}

void SMantleVehicleBuilder::SaveNow()
{
	UMantleVehicleSource* Source = BuiltSource.Get();
	if (!Source || !Source->GetOutermost()->IsDirty())
	{
		return;
	}
	FString Problem;
	if (FMantleVehicleBuilder::SaveVehicle(*Source, Problem))
	{
		SaveStatus = LOCTEXT("SavedAgain", "Saved.");
		bSaveFailed = false;
	}
	else if (!Problem.IsEmpty())
	{
		SaveStatus = FText::Format(LOCTEXT("NotSaved", "Not saved: {0}"), FText::FromString(Problem));
		bSaveFailed = true;
	}
}

void SMantleVehicleBuilder::SetSize(float NewSize)
{
	UMantleVehicleSource* Source = BuiltSource.Get();
	if (!Source)
	{
		return;
	}
	// The type's own fit range, not a wider one: a vehicle bigger or smaller than that is not what the type's handling was made for.
	const FVector2f Range = ModelSizeRange();
	const float Clamped = FMath::Clamp(NewSize, Range.X, Range.Y);
	if (FMath::IsNearlyEqual(Source->FitScale, Clamped))
	{
		return;
	}
	// Typing a number or pressing Reset has no drag around it, so it gets its own undo step; during a drag the drag's transaction is open.
	TOptional<FScopedTransaction> Own;
	if (!SizeDragTransaction.IsValid())
	{
		Own.Emplace(LOCTEXT("ChangeSize", "Change vehicle size"));
	}
	Source->Modify();
	Source->FitScale = Clamped;
	Source->MarkPackageDirty();
}

FReply SMantleVehicleBuilder::OnResetSize()
{
	if (SuggestedSize.IsSet())
	{
		SetSize(SuggestedSize.GetValue());
		SaveNow();
	}
	return FReply::Handled();
}

FReply SMantleVehicleBuilder::OnPlaceInLevel()
{
	using namespace MantleVehicleBuilderPanelPrivate;

	if (!PlaceResultBox.IsValid())
	{
		return FReply::Handled();
	}
	PlaceResultBox->ClearChildren();
	UMantleVehicleSource* Source = BuiltSource.Get();
	FString Problem;
	FString PlaceNote;
	if (!Source)
	{
		Problem = TEXT("Build your vehicle first, then press Put it in my level.");
	}
	else if (FMantleVehicleBuilder::PlaceInLevel(*Source, Problem, nullptr, &PlaceNote))
	{
		PlaceResultBox->AddSlot().AutoHeight()
		[
			SNew(STextBlock)
				.ColorAndOpacity(FSlateColor(Good))
				.AutoWrapText(true)
				.Text(FText::Format(LOCTEXT("Placed", "Your {0} is in your level.{1} Press Test Locally (in the toolbar, next to Play) to drive it."), SelectedPresetName,
					FText::FromString(PlaceNote.IsEmpty() ? FString() : FString(TEXT(" ")) + PlaceNote)))
		];
		return FReply::Handled();
	}
	PlaceResultBox->AddSlot().AutoHeight()
	[
		SNew(STextBlock)
			.ColorAndOpacity(FSlateColor(Bad))
			.AutoWrapText(true)
			.Text(FText::FromString(Problem))
	];
	return FReply::Handled();
}

FReply SMantleVehicleBuilder::OnShowSettings()
{
	if (UMantleVehicleSource* Source = BuiltSource.Get())
	{
		if (GEditor)
		{
			if (UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				Subsystem->OpenEditorForAsset(Source);
			}
		}
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
