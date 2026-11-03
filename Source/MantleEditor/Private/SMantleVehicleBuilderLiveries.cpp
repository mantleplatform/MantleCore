// Copyright Joshua Eger. All Rights Reserved.
// The Liveries step of the Build a vehicle window (SMantleVehicleBuilder): images laid over the paint, a name for each, the one the car starts in,
// a live preview on the model and the UV guide to paint them on.
#include "SMantleVehicleBuilder.h"

#include "MantleVehicleBuilder.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleLiveryTools.h"
#include "MantleVehicleAsset.h"
#include "MantleVehicleSource.h"
#include "SMantleBuilderViewport.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "DesktopPlatformModule.h"
#include "Engine/Texture2D.h"
#include "Factories/Factory.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "IAssetTools.h"
#include "IDesktopPlatform.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "PropertyCustomizationHelpers.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MantleVehicleBuilder"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleVehicleBuilderLiveriesPrivate
{
	const FLinearColor Bad(0.95f, 0.35f, 0.30f);
	const FLinearColor Warn(0.95f, 0.65f, 0.25f);
}

TSharedRef<SWidget> SMantleVehicleBuilder::MakeLiveriesStep()
{
	using namespace MantleVehicleBuilderLiveriesPrivate;

	SAssignNew(LiveryRowsBox, SVerticalBox);
	RebuildLiveryRows();

	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
	Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
	[
		SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("LiveriesHelp", "A livery is drawn over the painted parts of the body and under the clear coat. Where the image is clear (its alpha), the car's paint color shows. The image has to be laid out like your model's first UV set; Export UV guide writes a picture of that layout to paint on."))
	];
	Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
		[
			SNew(SButton)
				.ToolTipText(LOCTEXT("AddImageTip", "Pick a PNG, TGA or DDS file. Mantle imports it into your project and adds it as a livery."))
				.IsEnabled_Lambda([this] { return LiveryChoices.Num() < FMantleVehicleLimits::MaxLiveries; })
				.OnClicked(this, &SMantleVehicleBuilder::OnAddLiveryImage)
				[
					SNew(STextBlock).Text(LOCTEXT("AddImage", "Add image..."))
				]
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SButton)
				.ToolTipText(LOCTEXT("GuideTip", "Saves a picture of your model's UV layout with the painted parts outlined, to draw a livery on in your paint program."))
				.IsEnabled_Lambda([this] { return Preview.bOk; })
				.OnClicked(this, &SMantleVehicleBuilder::OnExportUvGuide)
				[
					SNew(STextBlock).Text(LOCTEXT("ExportGuide", "Export UV guide..."))
				]
		]
	];
	Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 2)
	[
		SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("PickTextureHelp", "Or pick a texture that is already in your project:"))
	];
	Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
	[
		SNew(SObjectPropertyEntryBox)
			.AllowedClass(UTexture2D::StaticClass())
			.AllowClear(false)
			.DisplayThumbnail(false)
			.ObjectPath_Lambda([] { return FString(); })
			.OnObjectChanged_Lambda([this](const FAssetData& Asset)
			{
				AddLivery(Cast<UTexture2D>(Asset.GetAsset()));
			})
	];
	Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
	[
		SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity_Lambda([this] { return bLiveryStatusFailed ? FSlateColor(Bad) : FSlateColor::UseSubduedForeground(); })
			.Visibility_Lambda([this] { return LiveryStatus.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			.Text_Lambda([this] { return LiveryStatus; })
	];
	Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
	[
		SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor(Warn))
			.Visibility_Lambda([this]
			{
				bool bAny = false;
				for (const FPaintChoice& Choice : PaintChoices)
				{
					bAny = bAny || Choice.bOn;
				}
				return !LiveryChoices.IsEmpty() && !bAny ? EVisibility::Visible : EVisibility::Collapsed;
			})
			.Text(LOCTEXT("NoPaintForLivery", "No part of your model is painted, so a livery has nothing to cover. Tick the parts to paint in the Seats and paint step."))
	];
	Rows->AddSlot().AutoHeight()
	[
		LiveryRowsBox.ToSharedRef()
	];
	return WithPicture(SNew(SScrollBox) + SScrollBox::Slot()[Rows], /*bLights*/ false, /*bLiveries*/ true);
}

void SMantleVehicleBuilder::RebuildLiveryRows()
{
	using namespace MantleVehicleBuilderLiveriesPrivate;
	if (!LiveryRowsBox.IsValid())
	{
		return;
	}
	LiveryRowsBox->ClearChildren();
	LiveryBrushes.Reset();

	LiveryRowsBox->AddSlot().AutoHeight().Padding(0, 2)
	[
		SNew(SCheckBox)
			.Style(&FAppStyle::Get().GetWidgetStyle<FCheckBoxStyle>("RadioButton"))
			.IsChecked_Lambda([this] { return StartLivery == INDEX_NONE ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState) { SetStartLivery(INDEX_NONE); })
			[
				SNew(STextBlock).Text(LOCTEXT("StartPlain", "The car starts with plain paint (no livery)"))
			]
	];

	for (int32 Index = 0; Index < LiveryChoices.Num(); ++Index)
	{
		UTexture2D* Texture = LiveryChoices[Index].Texture.LoadSynchronous();
		const TSharedRef<FSlateBrush> Brush = MakeShared<FSlateBrush>();
		Brush->SetResourceObject(Texture);
		Brush->ImageSize = FVector2D(64.f, 64.f);
		Brush->DrawAs = Texture ? ESlateBrushDrawType::Image : ESlateBrushDrawType::NoDrawType;
		LiveryBrushes.Add(Brush);

		// The same lines the vehicle check gives (a size past the rule, no alpha).
		FString Problems;
		if (!Texture)
		{
			Problems = TEXT("This image can't be found.");
		}
		else
		{
			TArray<FMantleValidationIssue> Issues;
			FMantleVehicleConverter::LiveryImageIssues(TEXT("Your vehicle"), *Texture, Issues);
			TArray<FString> Lines;
			for (const FMantleValidationIssue& Issue : Issues)
			{
				Lines.Add(Issue.Problem);
			}
			Problems = FString::Join(Lines, TEXT(" "));
		}

		LiveryRowsBox->AddSlot().AutoHeight().Padding(0, 6, 0, 0)
		[
			SNew(SBorder)
				.Padding(6)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
						[
							SNew(SButton)
								.ToolTipText(LOCTEXT("ShowOnModel", "Show this livery on the model in the picture."))
								.OnClicked_Lambda([this, Index] { ShowLivery(Index); return FReply::Handled(); })
								[
									SNew(SBox).WidthOverride(64.f).HeightOverride(64.f)
									[
										SNew(SImage).Image(&Brush.Get())
									]
								]
						]
						+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(SEditableTextBox)
									.HintText(LOCTEXT("LiveryNameHint", "Name (shown in menus)"))
									.Text_Lambda([this, Index] { return LiveryChoices.IsValidIndex(Index) ? FText::FromString(LiveryChoices[Index].Name) : FText::GetEmpty(); })
									.OnTextCommitted_Lambda([this, Index](const FText& Text, ETextCommit::Type) { SetLiveryName(Index, Text.ToString()); })
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
							[
								SNew(SCheckBox)
									.Style(&FAppStyle::Get().GetWidgetStyle<FCheckBoxStyle>("RadioButton"))
									.IsChecked_Lambda([this, Index] { return StartLivery == Index ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
									.OnCheckStateChanged_Lambda([this, Index](ECheckBoxState) { SetStartLivery(Index); })
									[
										SNew(STextBlock).Text(LOCTEXT("StartsWith", "The car starts with this one"))
									]
							]
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(6, 0, 0, 0)
						[
							SNew(SButton)
								.ToolTipText(LOCTEXT("RemoveLiveryTip", "Remove this livery."))
								.OnClicked_Lambda([this, Index] { RemoveLivery(Index); return FReply::Handled(); })
								[
									SNew(STextBlock).Text(LOCTEXT("RemoveLivery", "Remove"))
								]
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
					[
						SNew(STextBlock)
							.AutoWrapText(true)
							.ColorAndOpacity(FSlateColor(Warn))
							.Visibility(Problems.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
							.Text(FText::FromString(Problems))
					]
				]
		];
	}
}

void SMantleVehicleBuilder::AddLivery(UTexture2D* Texture)
{
	LiveryStatus = FText::GetEmpty();
	bLiveryStatusFailed = false;
	if (!Texture)
	{
		return;
	}
	if (LiveryChoices.Num() >= FMantleVehicleLimits::MaxLiveries)
	{
		LiveryStatus = FText::Format(LOCTEXT("TooManyLiveries", "A vehicle has at most {0} liveries. Remove one first."), FMantleVehicleLimits::MaxLiveries);
		bLiveryStatusFailed = true;
		return;
	}
	const TSoftObjectPtr<UTexture2D> Soft(Texture);
	for (const FLiveryChoice& Choice : LiveryChoices)
	{
		if (Choice.Texture.ToSoftObjectPath() == Soft.ToSoftObjectPath())
		{
			LiveryStatus = LOCTEXT("LiveryAlreadyAdded", "That image is already one of the liveries.");
			bLiveryStatusFailed = true;
			return;
		}
	}
	FLiveryChoice& Choice = LiveryChoices.AddDefaulted_GetRef();
	Choice.Texture = Soft;
	Choice.Name = FMantleVehicleLiveryTools::NameFromImage(Texture->GetName());
	bLiveriesEdited = true;
	// The image just added is the one shown on the model; the first one added is the one the car starts in.
	ShownLivery = LiveryChoices.Num() - 1;
	if (LiveryChoices.Num() == 1)
	{
		StartLivery = 0;
	}
	RebuildLiveryRows();
	LiveriesChanged();
}

FReply SMantleVehicleBuilder::OnAddLiveryImage()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		return FReply::Handled();
	}
	const void* ParentWindow = FSlateApplication::IsInitialized() ? FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr) : nullptr;
	TArray<FString> Files;
	if (!DesktopPlatform->OpenFileDialog(ParentWindow, TEXT("Pick a livery image"), LastImportFolder, FString(),
			TEXT("Image (*.png;*.tga;*.dds;*.jpg;*.jpeg;*.bmp)|*.png;*.tga;*.dds;*.jpg;*.jpeg;*.bmp|PNG (*.png)|*.png|TGA (*.tga)|*.tga|DDS (*.dds)|*.dds"), EFileDialogFlags::Multiple, Files)
		|| Files.IsEmpty())
	{
		return FReply::Handled();
	}
	LastImportFolder = FPaths::GetPath(Files[0]);
	LiveryStatus = FText::GetEmpty();
	bLiveryStatusFailed = false;
	for (const FString& File : Files)
	{
		if (LiveryChoices.Num() >= FMantleVehicleLimits::MaxLiveries)
		{
			break;
		}
		if (FPaths::GetExtension(File).Equals(TEXT("ytd"), ESearchCase::IgnoreCase))
		{
			LiveryStatus = LOCTEXT("YtdNo", "Mantle can't read GTA .ytd files. Open it in your usual tool, save the picture as a PNG or DDS, and add that.");
			bLiveryStatusFailed = true;
			continue;
		}
		UAssetImportTask* Task = NewObject<UAssetImportTask>(GetTransientPackage());
		Task->Filename = FPaths::ConvertRelativePathToFull(File);
		Task->DestinationPath = FMantleVehicleBuilder::ImportFolder() / TEXT("Liveries");
		Task->bAutomated = true;
		Task->bReplaceExisting = true;
		Task->bSave = true;
		Task->bAsync = false;
		FAssetToolsModule::GetModule().Get().ImportAssetTasks({ Task });
		UTexture2D* Made = nullptr;
		for (UObject* Object : Task->GetObjects())
		{
			Made = Made ? Made : Cast<UTexture2D>(Object);
		}
		if (!Made)
		{
			LiveryStatus = FText::Format(LOCTEXT("ImageNotImported", "Mantle could not import {0}. Use a PNG, TGA or DDS file."), FText::FromString(FPaths::GetCleanFilename(File)));
			bLiveryStatusFailed = true;
			continue;
		}
		AddLivery(Made);
	}
	return FReply::Handled();
}

void SMantleVehicleBuilder::RemoveLivery(const int32 Index)
{
	if (!LiveryChoices.IsValidIndex(Index))
	{
		return;
	}
	LiveryChoices.RemoveAt(Index);
	bLiveriesEdited = true;
	auto Shift = [Index](int32& Which)
	{
		if (Which == Index)
		{
			Which = INDEX_NONE;
		}
		else if (Which > Index)
		{
			--Which;
		}
	};
	Shift(StartLivery);
	Shift(ShownLivery);
	RebuildLiveryRows();
	LiveriesChanged();
}

void SMantleVehicleBuilder::SetLiveryName(const int32 Index, const FString& Name)
{
	if (!LiveryChoices.IsValidIndex(Index))
	{
		return;
	}
	const FString Cut = Name.TrimStartAndEnd().Left(FMantleVehicleConverter::MaxLiveryNameLength);
	if (LiveryChoices[Index].Name == Cut)
	{
		return;
	}
	LiveryChoices[Index].Name = Cut;
	bLiveriesEdited = true;
	LiveriesChanged();
}

void SMantleVehicleBuilder::SetStartLivery(const int32 Index)
{
	if (StartLivery == Index || (Index != INDEX_NONE && !LiveryChoices.IsValidIndex(Index)))
	{
		return;
	}
	StartLivery = Index;
	bLiveriesEdited = true;
	ShownLivery = Index;
	LiveriesChanged();
}

void SMantleVehicleBuilder::ShowLivery(const int32 Index)
{
	ShownLivery = LiveryChoices.IsValidIndex(Index) ? Index : INDEX_NONE;
	ApplyLiveryPreview();
}

void SMantleVehicleBuilder::LiveriesChanged()
{
	ApplyLiveryPreview();
	if (UMantleVehicleSource* Source = BuiltSource.Get())
	{
		ApplyLiveries(*Source);
		SaveNow();
	}
}

void SMantleVehicleBuilder::ApplyLiveries(UMantleVehicleSource& Source) const
{
	FMantleVehicleSourcePaint& Paint = Source.Paint;
	Paint.Liveries.Reset();
	Paint.LiveryNames.Reset();
	for (const FLiveryChoice& Choice : LiveryChoices)
	{
		Paint.Liveries.Add(Choice.Texture);
		Paint.LiveryNames.Add(Choice.Name);
	}
	Paint.DefaultLivery = LiveryChoices.IsValidIndex(StartLivery) ? StartLivery : INDEX_NONE;
	Source.MarkPackageDirty();
}

void SMantleVehicleBuilder::ApplyLiveryPreview()
{
	const TSharedPtr<SMantleBuilderViewport> Viewport = PreviewViewport.Pin();
	LiveryPreviewMaterials.Reset();
	if (!Viewport.IsValid())
	{
		return;
	}
	TMap<int32, UMaterialInterface*> Slots;
	if (bPictureLiveries && Preview.bOk && LiveryChoices.IsValidIndex(ShownLivery))
	{
		// The ticked sections wear the paint, and so the livery; the color is the one a new vehicle starts with.
		UMaterialInterface* Material = FMantleVehicleLiveryTools::MakePreviewMaterial(LiveryChoices[ShownLivery].Texture.LoadSynchronous(), FMantleVehicleSourcePaint().Color);
		LiveryPreviewMaterials.Emplace(Material);
		for (int32 Index = 0; Index < PaintChoices.Num(); ++Index)
		{
			if (PaintChoices[Index].bOn)
			{
				Slots.Add(Index, Material);
			}
		}
	}
	Viewport->SetSlotMaterials(Slots);
}

FReply SMantleVehicleBuilder::OnExportUvGuide()
{
	LiveryStatus = FText::GetEmpty();
	bLiveryStatusFailed = false;
	if (!Preview.bOk)
	{
		LiveryStatus = LOCTEXT("GuideNeedsModel", "Choose your vehicle's model first.");
		bLiveryStatusFailed = true;
		return FReply::Handled();
	}
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		return FReply::Handled();
	}
	const void* ParentWindow = FSlateApplication::IsInitialized() ? FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr) : nullptr;
	const FString ModelName = Mesh.IsValid() ? Mesh->GetName() : TEXT("Vehicle");
	TArray<FString> Files;
	if (!DesktopPlatform->SaveFileDialog(ParentWindow, TEXT("Save the UV guide"), LastGuideFolder.IsEmpty() ? LastImportFolder : LastGuideFolder,
			ModelName + TEXT("_UV_guide.png"), TEXT("PNG (*.png)|*.png"), EFileDialogFlags::None, Files)
		|| Files.IsEmpty())
	{
		return FReply::Handled();
	}
	LastGuideFolder = FPaths::GetPath(Files[0]);
	TSet<int32> Painted;
	for (int32 Index = 0; Index < PaintChoices.Num(); ++Index)
	{
		if (PaintChoices[Index].bOn)
		{
			Painted.Add(Index);
		}
	}
	FString Error;
	if (FMantleVehicleLiveryTools::WriteUvGuide(Preview.Body, Painted, 2048, Files[0], Error))
	{
		LiveryStatus = FText::Format(LOCTEXT("GuideSaved", "Saved the UV guide to {0}. It is 2048 by 2048; paint inside the colored shapes, which are the painted parts."), FText::FromString(Files[0]));
		FPlatformProcess::ExploreFolder(*Files[0]);
	}
	else
	{
		LiveryStatus = FText::FromString(Error);
		bLiveryStatusFailed = true;
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
