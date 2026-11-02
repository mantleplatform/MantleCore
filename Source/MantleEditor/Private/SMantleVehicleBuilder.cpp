// Copyright Joshua Eger. All Rights Reserved.
#include "SMantleVehicleBuilder.h"

#include "MantleVehicleBlueprint.h"
#include "MantleVehicleBuilder.h"
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehicleChassisSkin.h"
#include "MantleVehicleConverter.h"
#include "MantleVehiclePreset.h"
#include "MantleVehicleShapeKind.h"
#include "MantleVehicleSource.h"
#include "SMantleBuilderViewport.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SComboButton.h"

#include "AssetRegistry/AssetData.h"
#include "DesktopPlatformModule.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StreamableRenderAsset.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "IDesktopPlatform.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PropertyCustomizationHelpers.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
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
#include "Widgets/SOverlay.h"
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
		.SetTooltipText(LOCTEXT("TabTooltip", "Make a vehicle players can drive from your own 3D model: pick its type, pick your model, check how it fits, and try it in your level."))
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
	PreviewKind = MakeShared<FMantleVehicleShapeKind>();

	SAssignNew(Flow, SMantleBuilderFlow)
		.Steps(MakeSteps())
		.FinishLabel(LOCTEXT("Done", "Done"))
		.OnFinish(FSimpleDelegate::CreateSP(this, &SMantleVehicleBuilder::CloseWindow));

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
		Step.Title = LOCTEXT("StepType", "What kind of vehicle?");
		Step.Help = LOCTEXT("StepTypeHelp", "Pick the closest match. This decides how your vehicle drives or flies. Next you choose your own 3D model for it.");
		Step.MakeContent = [this] { return MakeTypeStep(); };
		Step.CanContinue = [this](FText& OutWhyNot)
		{
			if (SelectedPresetId.IsEmpty())
			{
				OutWhyNot = LOCTEXT("NeedType", "Pick what kind of vehicle it is to continue.");
				return false;
			}
			return true;
		};
	}
	{
		FMantleBuilderStep& Step = Steps.AddDefaulted_GetRef();
		Step.Title = LOCTEXT("StepModel", "Choose your vehicle's model");
		Step.Help = LOCTEXT("StepModelHelp", "Every vehicle needs a 3D model. Pick one already in your project (a mesh or a vehicle Blueprint), or import a file from your computer. You will see it in the picture.");
		Step.MakeContent = [this] { return MakeModelStep(); };
		Step.CanContinue = [this](FText& OutWhyNot)
		{
			if (!Mesh.IsValid() || !MeshProblem.IsEmpty() || !bHaveModelInfo)
			{
				OutWhyNot = MeshProblem.IsEmpty() ? LOCTEXT("NeedModel", "Choose your vehicle's model to continue. A vehicle can't be made without one.") : MeshProblem;
				return false;
			}
			return true;
		};
	}
	{
		FMantleBuilderStep& Step = Steps.AddDefaulted_GetRef();
		Step.Title = LOCTEXT("StepFit", "Check the fit");
		Step.Help = LOCTEXT("StepFitHelp", "Check that your model faces the right way, is the right size, and that its wheels and other moving parts are the right ones. Mantle filled in what it could find. The green dots in the picture are the wheels.");
		Step.MakeContent = [this] { return MakeFitStep(); };
	}
	{
		FMantleBuilderStep& Step = Steps.AddDefaulted_GetRef();
		Step.Title = LOCTEXT("StepSeats", "Seats and paint");
		Step.Help = LOCTEXT("StepSeatsHelp", "Check what Mantle found on your model: where the driver sits, and which parts of it are painted so players can change their color.");
		Step.MakeContent = [this] { return MakeSeatsAndPaintStep(); };
	}
	{
		FMantleBuilderStep& Step = Steps.AddDefaulted_GetRef();
		Step.Title = FMantleThumbnailText::Heading();
		Step.Help = LOCTEXT("StepThumbnailHelp", "The picture of this item in the library. Mantle takes one for you; you can choose the angle or use your own picture.");
		Step.MakeContent = [this] { return MakeThumbnailStep(); };
	}
	{
		FMantleBuilderStep& Step = Steps.AddDefaulted_GetRef();
		Step.Title = LOCTEXT("StepBuild", "Build and drive");
		Step.Help = LOCTEXT("StepBuildHelp", "Build your vehicle, put it in your level, then press Test Locally to drive it. You can go back to any step first and build again.");
		Step.MakeContent = [this] { return MakeBuildStep(); };
		Step.CanContinue = [this](FText& OutWhyNot)
		{
			if (!BuiltSource.IsValid())
			{
				OutWhyNot = LOCTEXT("NeedBuild", "Press Build my vehicle first.");
				return false;
			}
			return true;
		};
	}
	return Steps;
}

void SMantleVehicleBuilder::CloseWindow()
{
	if (const TSharedPtr<SDockTab> Tab = FGlobalTabmanager::Get()->FindExistingLiveTab(TabName))
	{
		Tab->RequestCloseTab();
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// The picture.

TSharedRef<SWidget> SMantleVehicleBuilder::WithPicture(const TSharedRef<SWidget>& Left, const bool bLights)
{
	bPictureLights = bLights;
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 12.f, 0.f)
		[
			SNew(SBox).WidthOverride(420.f)
			[
				Left
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f)
		[
			MakePicture()
		];
}

TSharedRef<SWidget> SMantleVehicleBuilder::MakePicture()
{
	using namespace MantleVehicleBuilderPanelPrivate;

	const TSharedRef<SMantleBuilderViewport> Viewport = SNew(SMantleBuilderViewport).Kind(PreviewKind)
		.OnPointsChanged(FSimpleDelegate::CreateSP(this, &SMantleVehicleBuilder::OnPicturePointsChanged));
	PreviewViewport = Viewport;
	if (Preview.bOk && !bPreviewDirty)
	{
		// Opened again with nothing changed: the same picture, in a new viewport.
		Viewport->SetMesh(Preview.Mesh.Get(), true);
		PushLightsToPicture();
	}
	else
	{
		RebuildPreview(/*bFrame*/ true);
	}

	return SNew(SBox).MinDesiredHeight(320.f)
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			[
				SNew(SBox)
					.Visibility_Lambda([this] { return Preview.bOk ? EVisibility::Visible : EVisibility::Collapsed; })
					[
						Viewport
					]
			]
			+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(16.f)
			[
				SNew(STextBlock)
					.AutoWrapText(true)
					.Justification(ETextJustify::Center)
					.Visibility_Lambda([this] { return Preview.bOk ? EVisibility::Collapsed : EVisibility::Visible; })
					.ColorAndOpacity_Lambda([this] { return PreviewProblem.IsEmpty() ? FSlateColor::UseSubduedForeground() : FSlateColor(Bad); })
					.Text_Lambda([this]
					{
						if (!PreviewProblem.IsEmpty())
						{
							return PreviewProblem;
						}
						if (SelectedPresetId.IsEmpty())
						{
							return LOCTEXT("PictureNoType", "Pick what kind of vehicle it is.");
						}
						return LOCTEXT("PictureNoModel", "Choose your vehicle's model next.");
					})
			]
		];
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
	bPreviewDirty = false;
	PreviewProblem = FText::GetEmpty();
	const TSharedPtr<SMantleBuilderViewport> Viewport = PreviewViewport.Pin();
	// The viewport lets go of the old picture before it is discarded.
	if (Viewport.IsValid())
	{
		Viewport->SetMesh(nullptr, false);
	}
	Preview.Discard();
	PreviewKind->SetWheelDots(TArray<FVector>());

	UObject* Model = Mesh.Get();
	if (Model && !SelectedPresetId.IsEmpty() && bHaveModelInfo)
	{
		if (FMantleVehicleBuilder::MakeModelPreview(Model, SelectedPresetId, MakeBuildOptions(), Preview))
		{
			PreviewKind->SetSpec(Preview.Spec);
			PreviewKind->SetWheelDots(Preview.WheelDots);
			if (Viewport.IsValid())
			{
				Viewport->SetMesh(Preview.Mesh.Get(), bFrame);
			}
		}
		else
		{
			PreviewProblem = MantleVehicleBuilderPanelPrivate::JoinSentences(Preview.Problems);
		}
	}
	RefreshLights();
	RefreshPaintChoices();
	PushLightsToPicture();
}

// ---------------------------------------------------------------------------------------------------------------------
// 1. What kind of vehicle?

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

	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 12.f, 0.f)
		[
			SNew(SBox).WidthOverride(420.f)
			[
				SNew(SScrollBox)
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
				]
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f)
		[
			MakePicture()
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
	// The type's frame at its own size: the starting point of its sizes and feel.
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
	// A plane, helicopter or hovercraft is sized by one Size held to the preset's own fit range.
	const FMantleVehiclePreset* Picked = FMantleVehiclePresets::Find(PresetId);
	Shape.Start(Loaded, Picked ? Picked->FitMinScale : 0.0, Picked ? Picked->FitMaxScale : 0.0);
	bPreviewDirty = false;

	// A different type is a different vehicle: what was made for the last one stays in the project, and this window stops pointing at it.
	ForgetBuild();

	// A model already picked is read again for the new type: its moving parts, its facing, its size and whether it fits are the new type's.
	ReadModel();
}

// ---------------------------------------------------------------------------------------------------------------------
// Size and feel.

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
	// A vehicle built before the change is not this one's: "Put it in my level", "Show its settings" and Done would hand on its old numbers.
	if (BuiltSource.IsValid() || bHaveLastBuild)
	{
		ForgetBuild();
	}
	MarkPreviewDirty();
}

TSharedRef<SWidget> SMantleVehicleBuilder::MakeSizeRow()
{
	// The Size is set through the session, which holds it to the preset's own fit range and says so; the slider only covers that range.
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
	for (const EMantleVehicleFeel Feel : MantleVehicleFeels)
	{
		if (Shape.HasFeel(Feel))
		{
			Rows->AddSlot().AutoHeight()
			[
				MakeFeelRow(Feel)
			];
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// 2. Choose your vehicle's model.

TSharedRef<SWidget> SMantleVehicleBuilder::MakeModelStep()
{
	using namespace MantleVehicleBuilderPanelPrivate;

	TSharedRef<SWidget> Left = SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(SObjectPropertyEntryBox)
					.AllowedClass(UObject::StaticClass())
					.OnShouldFilterAsset_Lambda([](const FAssetData& Asset)
					{
						// Meshes and vehicle Blueprints are listed; a solid mesh is accepted so the window can say why it won't do.
						const UClass* AssetClass = Asset.GetClass();
						return !AssetClass || !(AssetClass->IsChildOf(USkeletalMesh::StaticClass()) || AssetClass->IsChildOf(UStaticMesh::StaticClass())
							|| AssetClass->IsChildOf(UBlueprint::StaticClass()));
					})
					.AllowClear(true)
					.DisplayThumbnail(true)
					.ObjectPath_Lambda([this] { return BuildSource.IsValid() ? BuildSource->GetPathName() : (Mesh.IsValid() ? Mesh->GetPathName() : FString()); })
					.OnObjectChanged_Lambda([this](const FAssetData& Asset) { SetMesh(Asset); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SButton)
						.ContentPadding(FMargin(12, 4))
						.OnClicked(this, &SMantleVehicleBuilder::OnPickFile)
						.ToolTipText(LOCTEXT("PickFileTip", "Choose a model file (.fbx, .obj, .gltf or .glb) on your computer. Mantle imports it into your project (under MantleVehicles > Imported) and picks it here."))
						.Text(LOCTEXT("PickFile", "Import a file..."))
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
			// The model does not look like the type picked in step 1: say so, and offer the type it looks like.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor(Warn))
					.Visibility_Lambda([this]() { return bHaveModelInfo && TypeMatch.Verdict != EMantleVehicleTypeVerdict::Fits && !TypeMatch.Message.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
					.Text_Lambda([this]() { return FText::FromString(TypeMatch.Message); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(SButton)
					.HAlign(HAlign_Left)
					.ContentPadding(FMargin(12, 4))
					.Visibility_Lambda([this]() { return bHaveModelInfo && TypeMatch.Suggests() ? EVisibility::Visible : EVisibility::Collapsed; })
					.OnClicked(this, &SMantleVehicleBuilder::OnSwitchType)
					.ToolTipText(LOCTEXT("SwitchTip", "Change step 1's answer to the kind of vehicle your model looks like."))
					.Text_Lambda([this]() { return FText::FromString(TypeMatch.ButtonText); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Visibility_Lambda([this]() { return BlueprintNotes.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
					.Text_Lambda([this]() { return JoinSentences(BlueprintNotes); })
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(STextBlock)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(LOCTEXT("ModelHelp", "You can pick a Static Mesh, a Skeletal Mesh, or a vehicle Blueprint (such as a Chaos vehicle from a marketplace pack). A Blueprint made of several pieces is merged into one model for you. Have an FBX, OBJ, glTF or GLB file? Press Import a file and Mantle imports it and picks it for you."))
			]
		];

	return WithPicture(Left);
}

FReply SMantleVehicleBuilder::OnPickFile()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		ImportStatus = LOCTEXT("NoFileDialog", "This computer can't open a file picker here. Drag your model file into the Content Browser to import it, then pick it from the list.");
		bImportFailed = true;
		return FReply::Handled();
	}
	const void* ParentWindow = FSlateApplication::IsInitialized() ? FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr) : nullptr;
	TArray<FString> Files;
	if (!DesktopPlatform->OpenFileDialog(ParentWindow, TEXT("Pick your model"), LastImportFolder, FString(), TEXT("3D model (*.fbx;*.obj;*.gltf;*.glb)|*.fbx;*.obj;*.gltf;*.glb|FBX (*.fbx)|*.fbx|OBJ (*.obj)|*.obj|glTF (*.gltf;*.glb)|*.gltf;*.glb"), EFileDialogFlags::None, Files)
		|| Files.IsEmpty())
	{
		// Canceled: nothing changes and there is nothing to say.
		return FReply::Handled();
	}
	LastImportFolder = FPaths::GetPath(Files[0]);
	ImportFile(Files[0]);
	return FReply::Handled();
}

void SMantleVehicleBuilder::ImportFile(const FString& File)
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
	ImportStatus = FText::Format(LOCTEXT("Imported", "Imported {0} into {1}. It is picked above."), FText::FromString(Model->GetName()),
		FText::FromString(FPackageName::GetLongPackagePath(Model->GetOutermost()->GetName())));
	SetMesh(FAssetData(Model));
}

FReply SMantleVehicleBuilder::OnSwitchType()
{
	if (TypeMatch.Suggests() && !TypeMatch.SuggestedType.IsEmpty())
	{
		PickPreset(TypeMatch.SuggestedType, FText::FromString(TypeMatch.SuggestedName));
	}
	return FReply::Handled();
}

bool SMantleVehicleBuilder::CanBuild() const
{
	return !SelectedPresetId.IsEmpty() && Mesh.IsValid() && BuildSource.IsValid() && MeshProblem.IsEmpty();
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
	UObject* Model = nullptr;
	UObject* Source = nullptr;
	TArray<FString> Notes;
	FText Problem;
	if (UBlueprint* Blueprint = Cast<UBlueprint>(Picked))
	{
		// A vehicle Blueprint: its skeletal body (a Chaos vehicle, the wheels and parts on it riding along), or its static pieces merged into one model.
		const FMantleVehicleBlueprintModel Resolved = FMantleVehicleBlueprint::Resolve(Blueprint);
		Notes = Resolved.Notes;
		if (Resolved.Model)
		{
			Model = Resolved.Model;
			Source = Resolved.Around.IsSet() ? static_cast<UObject*>(Blueprint) : Resolved.Model;
		}
		else
		{
			Problem = MantleVehicleBuilderPanelPrivate::JoinSentences(Resolved.Problems);
		}
	}
	else if (Cast<USkeletalMesh>(Picked) || Cast<UStaticMesh>(Picked))
	{
		Model = Picked;
		Source = Picked;
	}
	// Another model (or none) is another vehicle: the last build was of the old one, so it is forgotten and cannot be placed by mistake.
	if (!Model || Model != Mesh.Get())
	{
		ForgetBuild();
	}
	Mesh = Model;
	BuildSource = Source;
	BlueprintNotes = Notes;
	MeshProblem = Problem;
	bPaintEdited = false;
	bLightsEdited = false;
	LoadThumbnailChoice();
	ReadModel();
	if (Asset.IsValid() && !Model && MeshProblem.IsEmpty())
	{
		// The picker lets any asset through so a wrong one can be explained here, instead of being refused without a word.
		MeshProblem = LOCTEXT("NotAModel", "That isn't a model. Pick a Static Mesh, a Skeletal Mesh or a vehicle Blueprint.");
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

// ---------------------------------------------------------------------------------------------------------------------
// Reading the model.

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
	TypeMatch = FMantleVehicleTypeMatch();
	UObject* Picked = Mesh.Get();
	if (!Picked || SelectedPresetId.IsEmpty())
	{
		RebuildModelRows();
		RebuildPreview(/*bFrame*/ true);
		return;
	}
	FString Error;
	if (!FMantleVehicleModel::Read(Picked, ModelInfo, Error))
	{
		MeshProblem = FText::FromString(Error);
		RebuildModelRows();
		RebuildPreview(/*bFrame*/ true);
		return;
	}
	bHaveModelInfo = true;
	// Does the model look like the type picked? Advice only: the build is never stopped by it.
	TypeMatch = FMantleVehicleTypeMatchCheck::Check(ModelInfo, SelectedPresetId);
	bUseSkin = !FMantleVehicleBuilder::FitsOwnWheels(Picked, SelectedPresetId);
	if (bUseSkin)
	{
		// A plane with two propellers is a twin: the frame has a propeller bone for each. The sizes start again from that frame.
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
		FMantleVehicleAsset Frame;
		if (FMantleVehicleChassisBuilder::MakeAsset(Shape.GetArchetype(), Frame, Error))
		{
			PartSlots = FMantleVehicleChassisSkin::PartSlots(Shape.GetArchetype(), Frame.Body);
			ModelYaw = FMantleVehicleChassisSkin::SuggestMapAndYaw(PartSlots, ModelInfo, PartMap);
		}
		ProposeShape();
	}
	RebuildModelRows();
	RebuildPreview(/*bFrame*/ true);
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
	// The model's length and width change with the way it faces, and so does the size the frame is proposed at.
	ForgetBuild();
	ProposeShape();
	RebuildModelRows();
	RebuildPreview(/*bFrame*/ false);
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
	// Assigned wheels place the frame's wheels: the sizes are proposed again; what was built is of the old assignment.
	ForgetBuild();
	ProposeShape();
	RebuildModelRows();
	RebuildPreview(/*bFrame*/ false);
}

// ---------------------------------------------------------------------------------------------------------------------
// 3. Check the fit.

TSharedRef<SWidget> SMantleVehicleBuilder::MakeFitStep()
{
	using namespace MantleVehicleBuilderPanelPrivate;

	TSharedRef<SVerticalBox> Feel = SNew(SVerticalBox);
	if (Shape.IsStarted())
	{
		// How it feels to drive, beyond its size: grip, power, suspension and top speed, where the type has them.
		Feel->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 2.f)
		[
			SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("BoldFont"))
				.Text(LOCTEXT("HandlingTitle", "How it feels to drive"))
		];
		AddFeelRows(Feel);
		Feel->AddSlot().AutoHeight().Padding(0.f, 4.f)
		[
			SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("BoldFont"))
				.AutoWrapText(true)
				.Text_Lambda([this] { return Shape.GetSummary(); })
		];
		Feel->AddSlot().AutoHeight().Padding(0.f, 4.f)
		[
			SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(Warn)
				.Visibility_Lambda([this] { return Shape.GetWarnings().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				.Text_Lambda([this] { return Shape.GetWarnings(); })
		];
	}

	ModelRowsBox = SNew(SVerticalBox);
	RebuildModelRows();
	return WithPicture(SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				ModelRowsBox.ToSharedRef()
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				Feel
			]
		]);
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
				.Text(LOCTEXT("OwnWheels", "Its wheels are named like a car's, so Mantle fits the vehicle's frame to them and your wheels turn as they are."))
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
				.ToolTipText(LOCTEXT("TwinTip", "Turn this on if your plane has two propellers, like a P-38. The plane then has two engines and you can point out each propeller."))
				.IsChecked_Lambda([this] { return bTwinEngine ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { SetTwinEngine(State == ECheckBoxState::Checked); })
				[
					SNew(STextBlock).Text(LOCTEXT("Twin", "My plane has two engines and two propellers"))
				]
		];
	}

	// What Mantle proposed for the frame, and the Size slider of a type that has one Size.
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
				.Text(LOCTEXT("SizeFitted", "Mantle sized the vehicle's frame to fit your model, so there is no size to set."))
		];
	}

	// The parts of the model that move: one row for each moving part of the frame.
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
				.Text(LOCTEXT("PartsTitle", "Wheels and other moving parts"))
		];
		ModelRowsBox->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
		[
			SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(LOCTEXT("PartsHelp", "Mantle filled in the parts it found. Fix any it got wrong. A bone picks that bone and everything attached to it; a section picks one material of the mesh. Leave a row at (nothing) and your model still drives or flies, that part just does not move."))
		];
		if (PartMap.IsEmpty())
		{
			ModelRowsBox->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
					.AutoWrapText(true)
					.ColorAndOpacity(Warn)
					.Text(LOCTEXT("NoPartsFound", "Mantle couldn't tell which parts are the wheels, so they won't spin. If the wheels have their own material, pick it for each wheel below. Otherwise separate the wheels in your 3D tool and import the model again."))
			];
		}
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

// ---------------------------------------------------------------------------------------------------------------------
// 4. Seats and paint.

TSharedRef<SWidget> SMantleVehicleBuilder::MakeSeatsAndPaintStep()
{
	using namespace MantleVehicleBuilderPanelPrivate;

	// The person in the seat is a pilot in a plane or a helicopter, a rider on a bike and a driver otherwise.
	const FMantleChassisSpec& Seated = Shape.GetArchetype();
	const FText SeatedWord = Seated.Aircraft.bSet || Seated.Helicopter.bSet ? LOCTEXT("SeatPilot", "pilot") : Seated.bCenterline ? LOCTEXT("SeatRider", "rider") : LOCTEXT("SeatDriver", "driver");

	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
	Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
	[
		SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("BoldFont"))
			.Text(LOCTEXT("SeatsTitle", "Seats"))
	];
	Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
	[
		SNew(STextBlock)
			.AutoWrapText(true)
			.Text(FText::Format(LOCTEXT("SeatFound", "Mantle put the {0}'s seat where the orange dot is in the picture. It is fitted to your model, and you can move it later in the vehicle's settings (Show its settings)."), SeatedWord))
	];
	if (Shape.HasPassengerOption())
	{
		Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
		[
			MakePassengerRow()
		];
	}
	Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 12)
	[
		SNew(SCheckBox)
			.ToolTipText(LOCTEXT("NoInsideTip", "Turn this on if your model is just the outside shell, with no seats or steering wheel inside. People in it won't be drawn while they ride."))
			.IsChecked_Lambda([this]() { return bHideRiders ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { SetHideRiders(State == ECheckBoxState::Checked); })
			[
				SNew(STextBlock).Text(LOCTEXT("NoInside", "This model has no inside: hide the driver and passenger"))
			]
	];

	Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
	[
		SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("BoldFont"))
			.Text(LOCTEXT("PaintTitle", "Paint"))
	];
	Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
	[
		SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("PaintHelp", "Ticked parts take the color and liveries players choose. Mantle ticked the parts of your model that look like car paint. Untick any it got wrong, or tick others."))
	];
	if (PaintChoices.IsEmpty())
	{
		Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
		[
			SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(LOCTEXT("NoPaintSections", "Your model has no parts to list yet. Go back and choose your vehicle's model."))
		];
	}
	for (int32 Index = 0; Index < PaintChoices.Num(); ++Index)
	{
		Rows->AddSlot().AutoHeight().Padding(0, 1)
		[
			SNew(SCheckBox)
				.IsChecked_Lambda([this, Index]() { return PaintChoices.IsValidIndex(Index) && PaintChoices[Index].bOn ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this, Index](ECheckBoxState State) { SetPaintSection(Index, State == ECheckBoxState::Checked); })
				[
					SNew(STextBlock).Text(FText::FromString(PaintChoices[Index].Section))
				]
		];
	}

	// Lights: which parts of the model glow as which light, and the headlight beams, dragged in the picture.
	Rows->AddSlot().AutoHeight().Padding(0, 12, 0, 4)
	[
		SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("BoldFont"))
			.Text(LOCTEXT("LightsTitle", "Lights"))
	];
	Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
	[
		SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("LightsHelp", "Mantle found the parts of your model that glow and sorted them into lights, shown as colored dots in the picture. In the game each kind glows in one color: white headlights and reverse lights, red rear lights, amber signals. Pick the parts for each light, or none."))
	];
	Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
	[
		SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor(Warn))
			.Visibility_Lambda([this]
			{
				bool bAny = false;
				for (int32 Kind = 0; Kind < FMantleVehicleLights::NumKinds; ++Kind)
				{
					bAny = bAny || FMantleVehicleLights::Sections(Lights, static_cast<EMantleLightKind>(Kind)).Num() > 0;
				}
				return Preview.bOk && !bAny ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.Text(LOCTEXT("NoLightsFound", "Mantle found no lights on your model. If its lights are parts of their own, pick them below. The headlight beams still light the road."))
	];
	for (const EMantleLightKind Kind : { EMantleLightKind::Head, EMantleLightKind::Rear, EMantleLightKind::Reverse,
		EMantleLightKind::Left, EMantleLightKind::Right })
	{
		Rows->AddSlot().AutoHeight().Padding(0, 1)
		[
			MakeLightRow(Kind)
		];
	}
	Rows->AddSlot().AutoHeight().Padding(0, 10, 0, 4)
	[
		SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("BoldFont"))
			.Text(LOCTEXT("BeamsTitle", "Headlight beams"))
	];
	Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
	[
		SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text_Lambda([this]
			{
				return FText::Format(LOCTEXT("BeamsHelp", "{0}|plural(one=One beam lights,other=# beams light) the road while the headlights are on. Each is a yellow arrow in the picture: click it, then drag to move it or turn it to aim."),
					FText::AsNumber(Lights.Beams.Num()));
			})
	];
	Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
		[
			SNew(SButton)
				.Text(LOCTEXT("AddBeam", "Add a beam"))
				.IsEnabled_Lambda([this] { return Preview.bOk; })
				.OnClicked(this, &SMantleVehicleBuilder::OnAddBeam)
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
		[
			SNew(SButton)
				.Text(LOCTEXT("RemoveBeam", "Remove the last beam"))
				.IsEnabled_Lambda([this] { return Lights.Beams.Num() > 0; })
				.OnClicked(this, &SMantleVehicleBuilder::OnRemoveBeam)
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SButton)
				.Text(LOCTEXT("ResetLights", "Reset to Mantle's guess"))
				.ToolTipText(LOCTEXT("ResetLightsTip", "Puts every light row and beam back to what Mantle found on your model."))
				.IsEnabled_Lambda([this] { return bLightsEdited && Preview.bOk; })
				.OnClicked(this, &SMantleVehicleBuilder::OnResetLights)
		]
	];

	return WithPicture(SNew(SScrollBox) + SScrollBox::Slot()[Rows], /*bLights*/ true);
}

void SMantleVehicleBuilder::RefreshPaintChoices()
{
	TArray<FPaintChoice> Fresh;
	if (Preview.bOk && Preview.Mesh.IsValid())
	{
		const TArray<FSkeletalMaterial>& Materials = Preview.Mesh->GetMaterials();
		TArray<FString> Names;
		for (const FSkeletalMaterial& Material : Materials)
		{
			Names.Add(Material.MaterialSlotName.ToString());
		}
		const TArray<FString> Sections = FMantleVehicleConverter::UniqueSectionNames(Names);
		for (int32 Index = 0; Index < Sections.Num(); ++Index)
		{
			FPaintChoice& Choice = Fresh.AddDefaulted_GetRef();
			Choice.Section = Sections[Index];
			// A light glows in its own color, so it is not painted, even when it was cut from the paint.
			bool bLight = false;
			for (int32 Kind = 0; Kind < FMantleVehicleLights::NumKinds; ++Kind)
			{
				bLight = bLight || FMantleVehicleLights::Sections(Lights, static_cast<EMantleLightKind>(Kind)).Contains(Choice.Section);
			}
			Choice.bOn = !bLight && FMantleVehicleConverter::LooksLikePaint(Materials[Index].MaterialInterface);
			if (bPaintEdited)
			{
				// The creator's ticks stay across a redraw of the picture.
				for (const FPaintChoice& Old : PaintChoices)
				{
					if (Old.Section == Choice.Section)
					{
						Choice.bOn = Old.bOn;
					}
				}
			}
		}
	}
	PaintChoices = Fresh;
}

void SMantleVehicleBuilder::SetPaintSection(const int32 Index, const bool bOn)
{
	if (!PaintChoices.IsValidIndex(Index) || PaintChoices[Index].bOn == bOn)
	{
		return;
	}
	PaintChoices[Index].bOn = bOn;
	bPaintEdited = true;
	// Already built: the vehicle takes the ticks at once.
	if (UMantleVehicleSource* Source = BuiltSource.Get())
	{
		ApplyChoices(*Source);
		SaveNow();
	}
}

void SMantleVehicleBuilder::SetHideRiders(const bool bOn)
{
	if (bHideRiders == bOn)
	{
		return;
	}
	bHideRiders = bOn;
	if (UMantleVehicleSource* Source = BuiltSource.Get())
	{
		FMantleVehicleBuilder::SetHiddenRiders(*Source, Cast<USkeletalMesh>(Mesh.Get()), bOn);
		SaveNow();
	}
}

void SMantleVehicleBuilder::ApplyChoices(UMantleVehicleSource& Source)
{
	if (bPaintEdited)
	{
		Source.Modify();
		Source.Paint.Sections.Reset();
		for (const FPaintChoice& Choice : PaintChoices)
		{
			if (Choice.bOn)
			{
				Source.Paint.Sections.Add(Choice.Section);
			}
		}
		Source.MarkPackageDirty();
	}
	if (bHideRiders)
	{
		FMantleVehicleBuilder::SetHiddenRiders(Source, Cast<USkeletalMesh>(Mesh.Get()), true);
	}
}

TSharedRef<SWidget> SMantleVehicleBuilder::MakeLightRow(const EMantleLightKind Kind)
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
		[
			SNew(SBox).WidthOverride(10.f).HeightOverride(10.f)
			[
				SNew(SImage)
					.Image(FAppStyle::GetBrush("WhiteBrush"))
					.ColorAndOpacity(FMantleVehicleLights::Color(Kind))
			]
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
		[
			SNew(SBox).WidthOverride(100.f)
			[
				SNew(STextBlock).Text(FMantleVehicleLights::Label(Kind))
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.f)
		[
			SNew(SComboButton)
				.IsEnabled_Lambda([this] { return Preview.bOk; })
				.ButtonContent()
				[
					SNew(STextBlock).Text_Lambda([this, Kind] { return LightRowText(Kind); })
				]
				.OnGetMenuContent_Lambda([this, Kind]
				{
					// Every section of the body; ticked are the ones that glow as this light. The menu stays open, so several can be picked.
					FMenuBuilder Menu(/*bInShouldCloseWindowAfterMenuSelection*/ false, nullptr);
					for (const FString& Section : Preview.Sections)
					{
						Menu.AddMenuEntry(FText::FromString(Section), FText::GetEmpty(), FSlateIcon(),
							FUIAction(FExecuteAction::CreateLambda([this, Kind, Section] { ToggleLightSection(Kind, Section); }), FCanExecuteAction(),
								FIsActionChecked::CreateLambda([this, Kind, Section] { return FMantleVehicleLights::Sections(Lights, Kind).Contains(Section); })),
							NAME_None, EUserInterfaceActionType::ToggleButton);
					}
					return Menu.MakeWidget();
				})
		];
}

FText SMantleVehicleBuilder::LightRowText(const EMantleLightKind Kind) const
{
	const TArray<FString>& Sections = FMantleVehicleLights::Sections(Lights, Kind);
	return Sections.IsEmpty() ? LOCTEXT("LightNone", "(none)") : FText::FromString(FString::Join(Sections, TEXT(", ")));
}

void SMantleVehicleBuilder::ToggleLightSection(const EMantleLightKind Kind, const FString& Section)
{
	TArray<FString>& Picked = FMantleVehicleLights::Sections(Lights, Kind);
	if (Picked.Contains(Section))
	{
		Picked.Remove(Section);
	}
	else
	{
		// A section glows as one light.
		for (int32 Other = 0; Other < FMantleVehicleLights::NumKinds; ++Other)
		{
			FMantleVehicleLights::Sections(Lights, static_cast<EMantleLightKind>(Other)).Remove(Section);
		}
		Picked.Add(Section);
		// And it is not painted: it glows in its own color.
		for (FPaintChoice& Choice : PaintChoices)
		{
			if (Choice.Section == Section && Choice.bOn)
			{
				Choice.bOn = false;
				bPaintEdited = true;
			}
		}
	}
	LightsChanged();
}

void SMantleVehicleBuilder::RefreshLights()
{
	if (!Preview.bOk)
	{
		return;
	}
	if (!bLightsEdited)
	{
		Lights = Preview.Lights;
		return;
	}
	// The creator's picks stay across a redraw of the picture, for the sections the model still has.
	for (int32 Kind = 0; Kind < FMantleVehicleLights::NumKinds; ++Kind)
	{
		FMantleVehicleLights::Sections(Lights, static_cast<EMantleLightKind>(Kind)).RemoveAll([this](const FString& Section) { return !Preview.Sections.Contains(Section); });
	}
}

void SMantleVehicleBuilder::LightsChanged()
{
	bLightsEdited = true;
	PushLightsToPicture();
	// Already built: the vehicle takes the lights at once.
	if (UMantleVehicleSource* Source = BuiltSource.Get())
	{
		if (bUseSkin)
		{
			TArray<FString> Notes;
			FMantleVehicleBuilder::ApplyLights(*Source, Lights, Notes);
		}
		else
		{
			FMantleVehicleBuildResult Again;
			Again.Source = Source;
			Again.bOk = true;
			FMantleVehicleBuilder::LightOwnBody(Again, MakeBuildOptions());
		}
		SaveNow();
	}
}

void SMantleVehicleBuilder::PushLightsToPicture()
{
	const TSharedPtr<SMantleBuilderViewport> Viewport = PreviewViewport.Pin();
	TArray<FMantleBuilderOverlayMarker> Markers;
	TArray<FMantleBuilderPoint> Points;
	if (bPictureLights && Preview.bOk && Preview.Body.LODs.Num() > 0)
	{
		// A dot in the light's color on each light section, at the middle of its triangles.
		const FMantleSkinnedMeshLOD& LOD = Preview.Body.LODs[0];
		for (int32 Kind = 0; Kind < FMantleVehicleLights::NumKinds; ++Kind)
		{
			const EMantleLightKind LightKind = static_cast<EMantleLightKind>(Kind);
			for (const FString& Section : FMantleVehicleLights::Sections(Lights, LightKind))
			{
				const int32 Slot = Preview.Sections.IndexOfByKey(Section);
				FVector Sum = FVector::ZeroVector;
				int32 Count = 0;
				for (const FMantleSkinnedMeshSection& Drawn : LOD.Sections)
				{
					if (Drawn.MaterialIndex != Slot || Drawn.FirstIndex < 0 || Drawn.FirstIndex + Drawn.IndexCount > LOD.Indices.Num())
					{
						continue;
					}
					for (int32 At = Drawn.FirstIndex; At < Drawn.FirstIndex + Drawn.IndexCount; ++At)
					{
						Sum += FVector(LOD.Positions[LOD.Indices[At]]);
						++Count;
					}
				}
				if (Count == 0)
				{
					continue;
				}
				FMantleBuilderOverlayMarker& Marker = Markers.AddDefaulted_GetRef();
				Marker.Location = Sum / Count;
				Marker.Label = FMantleVehicleLights::Label(LightKind);
				// Reverse lights are white too: gray tells them apart from the headlights in the picture.
				Marker.Color = LightKind == EMantleLightKind::Reverse ? FLinearColor(0.6f, 0.6f, 0.6f) : FMantleVehicleLights::Color(LightKind);
				Marker.RadiusCm = 3.f;
			}
		}
		for (int32 Index = 0; Index < Lights.Beams.Num(); ++Index)
		{
			const FMantleVehicleSourceLightBeam& Beam = Lights.Beams[Index];
			FMantleBuilderPoint& Point = Points.AddDefaulted_GetRef();
			Point.Role = FMantleVehicleShapeKind::BeamRole(Index);
			Point.Transform = FTransform(Beam.Direction.GetSafeNormal().Rotation(), Beam.Position);
			Point.Guess = Point.Transform;
			Point.Source = EMantleBuilderPointSource::Creator;
		}
	}
	PreviewKind->SetLights(Points.Num(), Markers);
	if (Viewport.IsValid())
	{
		Viewport->SetPoints(Points);
		Viewport->RefreshFromKind();
	}
}

void SMantleVehicleBuilder::OnPicturePointsChanged()
{
	const TSharedPtr<SMantleBuilderViewport> Viewport = PreviewViewport.Pin();
	if (!Viewport.IsValid())
	{
		return;
	}
	bool bChanged = false;
	for (const FMantleBuilderPoint& Point : Viewport->GetPoints())
	{
		for (int32 Index = 0; Index < Lights.Beams.Num(); ++Index)
		{
			if (Point.Role == FMantleVehicleShapeKind::BeamRole(Index))
			{
				Lights.Beams[Index].Position = Point.Transform.GetLocation();
				Lights.Beams[Index].Direction = Point.Transform.GetRotation().GetForwardVector();
				bChanged = true;
			}
		}
	}
	if (bChanged)
	{
		LightsChanged();
	}
}

FReply SMantleVehicleBuilder::OnAddBeam()
{
	// A new beam at the middle of the front, pointing ahead; the creator drags it into place.
	const FBox3f Bounds = Preview.Body.ComputeBounds(0);
	FMantleVehicleSourceLightBeam& Beam = Lights.Beams.AddDefaulted_GetRef();
	Beam.Position = Bounds.IsValid ? FVector(Bounds.Max.X, Bounds.GetCenter().Y, Bounds.GetCenter().Z) : FVector::ZeroVector;
	Beam.Direction = FVector(1.0, 0.0, -0.05);
	LightsChanged();
	return FReply::Handled();
}

FReply SMantleVehicleBuilder::OnRemoveBeam()
{
	if (Lights.Beams.Num() > 0)
	{
		Lights.Beams.Pop();
		LightsChanged();
	}
	return FReply::Handled();
}

FReply SMantleVehicleBuilder::OnResetLights()
{
	Lights = Preview.Lights;
	LightsChanged();
	bLightsEdited = false;
	return FReply::Handled();
}

// ---------------------------------------------------------------------------------------------------------------------
// 5. Thumbnail.

TSharedRef<SWidget> SMantleVehicleBuilder::MakeThumbnailStep()
{
	// The library card's picture, the same Thumbnail section every Mantle uploader shows.
	return SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SAssignNew(ThumbnailSection, SMantleThumbnailSection)
				.Target(FMantleThumbnailTarget::Square())
				.AngleSliders(true)
				.DefaultAngle(FMantleModelThumbnail::DefaultView())
				.Choice_Lambda([this] { return ThumbnailChoice; })
				.OnChoiceChanged_Raw(this, &SMantleVehicleBuilder::ThumbnailChoiceChanged)
				.Render(FMantleVehicleThumbnail::PreviewFor([this]() -> const UMantleVehicleSource* { return BuiltSource.Get(); }, [this]() -> UObject* { return Mesh.Get(); }))
		];
}

// ---------------------------------------------------------------------------------------------------------------------
// 6. Build and drive.

TSharedRef<SWidget> SMantleVehicleBuilder::MakeBuildStep()
{
	using namespace MantleVehicleBuilderPanelPrivate;
	ResultBox = SNew(SVerticalBox);

	TSharedRef<SWidget> Content = SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(STextBlock)
					.AutoWrapText(true)
					.Text_Lambda([this]
					{
						return FText::Format(LOCTEXT("BuildSummary", "Your {0}, made from {1}. {2}"), SelectedPresetName,
							Mesh.IsValid() ? FText::FromString(Mesh->GetName()) : FText::GetEmpty(), Shape.IsStarted() ? Shape.GetSummary() : FText::GetEmpty());
					})
			]
			// What a plane, jet or helicopter needs to take off, so the creator puts it somewhere it can.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
			[
				SNew(STextBlock)
					.AutoWrapText(true)
					.ColorAndOpacity(Warn)
					.Visibility_Lambda([this] { return Shape.IsStarted() && !FMantleVehicleShape::DriveAdvice(Shape.GetArchetype()).IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
					.Text_Lambda([this] { return Shape.IsStarted() ? FMantleVehicleShape::DriveAdvice(Shape.GetArchetype()) : FText::GetEmpty(); })
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

	// The step is built again each time it is opened: what the last build said is shown again.
	if (bHaveLastBuild)
	{
		ShowResult(LastBuild);
	}
	return Content;
}

FMantleVehicleModelOptions SMantleVehicleBuilder::MakeBuildOptions() const
{
	// A model with the type's wheels is fitted to them; any other is worn on the type's frame as its skin, with the sizes, the facing and the parts as the page has them.
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
	if (bLightsEdited)
	{
		Options.Lights = Lights;
		if (Preview.bOk)
		{
			Options.LightsBounds = Preview.Body.ComputeBounds(0);
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
	FMantleVehicleBuildResult Result = FMantleVehicleBuilder::BuildAny(BuildSource.Get(), SelectedPresetId, MakeBuildOptions());
	if (Result.bOk && Result.Source)
	{
		// The seats and paint step's choices go into the vehicle before it is saved.
		ApplyChoices(*Result.Source);
		if (Result.bSaved)
		{
			Result.bSaved = FMantleVehicleBuilder::SaveVehicle(*Result.Source, Result.SaveProblem);
		}
	}
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
	const FText SeatedWord = Seated.Aircraft.bSet || Seated.Helicopter.bSet ? LOCTEXT("SeatPilot", "pilot") : Seated.bCenterline ? LOCTEXT("SeatRider", "rider") : LOCTEXT("SeatDriver", "driver");
	const FText SizeTip = FText::Format(LOCTEXT("SizeTip", "How big the whole vehicle is in Mantle. 1 means as modeled. Mantle suggests a size so its {0} fits."), SeatedWord);
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
			// The size of a vehicle worn on the type's frame is the frame's: it was set in Check the fit and the build made the numbers for it.
			ResultBox->AddSlot().AutoHeight().Padding(0, 8, 0, 8)
			[
				SNew(STextBlock)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(LOCTEXT("FrameSizeNote", "Mantle sized the vehicle to your model. To change it, go back to Check the fit and press Build my vehicle again."))
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
				.Text(LOCTEXT("NextStep", "Next: press Put it in my level, then press Test Locally (in the toolbar, next to Play) to drive it. Press Done when you are finished."))
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
