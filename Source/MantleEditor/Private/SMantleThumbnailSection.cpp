// Copyright Joshua Eger. All Rights Reserved.
#include "SMantleThumbnailSection.h"

#include "DesktopPlatformModule.h"
#include "HAL/IConsoleManager.h"
#include "MantleModelThumbnail.h"
#include "MantlePng.h"
#include "MantleRuntime.h"
#include "MantleVehicleBuilder.h"
#include "MantleVehicleChassisBuilder.h"
#include "MantleVehicleSource.h"
#include "MantleVehicleThumbnail.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "RenderingThread.h"
#include "Slate/WidgetRenderer.h"
#include "TextureResource.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "IDesktopPlatform.h"
#include "Misc/Paths.h"
#include "PropertyCustomizationHelpers.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateBrush.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MantleThumbnailSection"

namespace
{
	/** How big the preview is drawn: a square one is 256, a wide one is 480 across. */
	FVector2f PreviewSize(const FMantleThumbnailTarget& Target)
	{
		return Target.Width == Target.Height ? FVector2f(256.0f, 256.0f) : FVector2f(480.0f, 480.0f * Target.Height / Target.Width);
	}
}

void SMantleThumbnailSection::Construct(const FArguments& InArgs)
{
	ChoiceAttribute = InArgs._Choice;
	Target = InArgs._Target;
	OnChoiceChanged = InArgs._OnChoiceChanged;
	OnUseView = InArgs._OnUseView;
	Render = InArgs._Render;
	UseViewLabel = InArgs._UseViewLabel.IsEmpty() ? FMantleThumbnailText::UseThisView() : InArgs._UseViewLabel;
	bAngleSliders = InArgs._AngleSliders;
	DefaultAngle = InArgs._DefaultAngle;

	const FVector2f Size = PreviewSize(Target);
	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			SNew(STextBlock).Font(FAppStyle::GetFontStyle("BoldFont")).Text(FMantleThumbnailText::Heading())
		]
		+ SVerticalBox::Slot().AutoHeight() [ MakeModeRow(EMantleThumbnailMode::Automatic) ]
		+ SVerticalBox::Slot().AutoHeight() [ MakeModeRow(EMantleThumbnailMode::Angle) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(24.0f, 0.0f, 0.0f, 4.0f)
		[
			SNew(SVerticalBox)
			.Visibility_Lambda([this] { return Current().Mode == EMantleThumbnailMode::Angle ? EVisibility::Visible : EVisibility::Collapsed; })
			+ SVerticalBox::Slot().AutoHeight() [ InArgs._AngleContent.Widget ]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
			[
				MakeAngleSliders()
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f)
			[
				SNew(SHorizontalBox)
				.Visibility(bAngleSliders ? EVisibility::Collapsed : EVisibility::Visible)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(UseViewLabel)
					.IsEnabled_Lambda([this] { return OnUseView.IsBound(); })
					.OnClicked_Lambda([this]
					{
						ViewNote = OnUseView.IsBound() ? OnUseView.Execute() : FText::GetEmpty();
						Refresh();
						return FReply::Handled();
					})
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FLinearColor(1.0f, 0.4f, 0.3f))
				.Visibility_Lambda([this] { return ViewNote.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				.Text_Lambda([this] { return ViewNote; })
			]
		]
		+ SVerticalBox::Slot().AutoHeight() [ MakeModeRow(EMantleThumbnailMode::Custom) ]
		+ SVerticalBox::Slot().AutoHeight().Padding(24.0f, 0.0f, 0.0f, 4.0f)
		[
			MakeCustomControls()
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 8.0f, 0.0f, 4.0f)
		[
			SNew(SBox).WidthOverride(Size.X).HeightOverride(Size.Y)
			[
				SNew(SImage)
				.Image_Lambda([this]() -> const FSlateBrush* { return Brush.IsValid() ? Brush.Get() : FAppStyle::GetBrush("WhiteBrush"); })
				.ColorAndOpacity_Lambda([this] { return Brush.IsValid() ? FSlateColor(FLinearColor::White) : FSlateColor(FLinearColor(0.02f, 0.02f, 0.02f)); })
			]
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
		[
			SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(FMantleThumbnailText::PreviewCaption(Target))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock).AutoWrapText(true)
			.Visibility_Lambda([this] { return Result.Warning.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			.ColorAndOpacity(FLinearColor(1.0f, 0.75f, 0.2f))
			.Text_Lambda([this] { return FText::FromString(Result.Warning); })
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock).AutoWrapText(true)
			.Visibility_Lambda([this] { return Result.Error.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			.ColorAndOpacity(FLinearColor(1.0f, 0.4f, 0.3f))
			.Text_Lambda([this] { return FText::FromString(Result.Error); })
		]
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0.0f, 4.0f, 0.0f, 0.0f)
		[
			SNew(SButton)
			.Text(LOCTEXT("RefreshPreview", "Refresh picture"))
			.ToolTipText(LOCTEXT("RefreshPreviewTip", "Draw the picture again from the current choice."))
			.OnClicked_Lambda([this] { Refresh(); return FReply::Handled(); })
		]
	];
	Refresh();
}

FMantleThumbnailChoice SMantleThumbnailSection::Current() const
{
	return ChoiceAttribute.Get(FMantleThumbnailChoice());
}

void SMantleThumbnailSection::SetChoice(const FMantleThumbnailChoice& NewChoice)
{
	ViewNote = FText::GetEmpty();
	OnChoiceChanged.ExecuteIfBound(NewChoice);
	Refresh();
}

void SMantleThumbnailSection::SetMode(EMantleThumbnailMode Mode)
{
	FMantleThumbnailChoice Next = Current();
	if (Next.Mode != Mode)
	{
		Next.Mode = Mode;
		// Choose the angle with sliders starts from the kind's own view, not from nothing.
		if (Mode == EMantleThumbnailMode::Angle && bAngleSliders && Next.Angle.IsNearlyZero())
		{
			Next.Angle = DefaultAngle.Get(FRotator::ZeroRotator);
		}
		SetChoice(Next);
	}
}

TSharedRef<SWidget> SMantleThumbnailSection::MakeModeRow(EMantleThumbnailMode Mode)
{
	return SNew(SCheckBox)
		.Style(FAppStyle::Get(), "RadioButton")
		.IsChecked_Lambda([this, Mode] { return Current().Mode == Mode ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([this, Mode](ECheckBoxState State)
		{
			if (State == ECheckBoxState::Checked)
			{
				SetMode(Mode);
			}
		})
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock).Text(FMantleThumbnailText::ModeName(Mode))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(4.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(STextBlock).WrapTextAt(500.0f).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(FMantleThumbnailText::ModeHelp(Mode))
			]
		];
}

TSharedRef<SWidget> SMantleThumbnailSection::MakeAngleSliders()
{
	// One row: the label, then a spin box that edits one part of the angle.
	auto Row = [this](const FText& Label, const FText& Tip, const float Min, const float Max, TFunction<float(const FMantleThumbnailChoice&)> Get,
		TFunction<void(FMantleThumbnailChoice&, float)> Set) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(SBox).WidthOverride(48.0f)[ SNew(STextBlock).Text(Label) ]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SSpinBox<float>)
				.MinValue(Min).MaxValue(Max).MinSliderValue(Min).MaxSliderValue(Max)
				.ToolTipText(Tip)
				.Value_Lambda([this, Get] { return Get(Current()); })
				.OnValueChanged_Lambda([this, Set](float Value)
				{
					FMantleThumbnailChoice Next = Current();
					Set(Next, Value);
					SetChoice(Next);
				})
			];
	};
	return SNew(SVerticalBox)
		.Visibility(bAngleSliders ? EVisibility::Visible : EVisibility::Collapsed)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 1.0f)
		[
			Row(LOCTEXT("AngleTurn", "Turn"), LOCTEXT("AngleTurnTip", "Turn the model left or right, in degrees."), -180.0f, 180.0f,
				[](const FMantleThumbnailChoice& C) { return static_cast<float>(C.Angle.Yaw); },
				[](FMantleThumbnailChoice& C, float V) { C.Angle.Yaw = V; })
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 1.0f)
		[
			Row(LOCTEXT("AngleTilt", "Tilt"), LOCTEXT("AngleTiltTip", "Look from above or below, in degrees."), -89.0f, 89.0f,
				[](const FMantleThumbnailChoice& C) { return static_cast<float>(C.Angle.Pitch); },
				[](FMantleThumbnailChoice& C, float V) { C.Angle.Pitch = V; })
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 1.0f)
		[
			Row(LOCTEXT("AngleZoom", "Zoom"), LOCTEXT("AngleZoomTip", "1 is the whole model; more is closer."), 0.25f, 4.0f,
				[](const FMantleThumbnailChoice& C) { return C.Zoom; },
				[](FMantleThumbnailChoice& C, float V) { C.Zoom = V; })
		];
}

TSharedRef<SWidget> SMantleThumbnailSection::MakeCustomControls()
{
	return SNew(SVerticalBox)
		.Visibility_Lambda([this] { return Current().Mode == EMantleThumbnailMode::Custom ? EVisibility::Visible : EVisibility::Collapsed; })
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(STextBlock).Text(LOCTEXT("CustomTexture", "Texture"))
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SObjectPropertyEntryBox)
				.AllowedClass(UTexture2D::StaticClass())
				.AllowClear(true)
				.DisplayThumbnail(false)
				.ObjectPath_Lambda([this] { return Current().Texture.ToString(); })
				.OnObjectChanged_Lambda([this](const FAssetData& Asset)
				{
					FMantleThumbnailChoice Next = Current();
					Next.Texture = TSoftObjectPtr<UTexture2D>(Asset.GetSoftObjectPath());
					if (!Next.Texture.IsNull())
					{
						// One source at a time, so the picture shown is the picture named.
						Next.ImageFile.FilePath.Reset();
					}
					SetChoice(Next);
				})
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
			[
				SNew(STextBlock).Text(LOCTEXT("CustomFile", "PNG file"))
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(SEditableTextBox)
				.Text_Lambda([this] { return FText::FromString(Current().ImageFile.FilePath); })
				.HintText(LOCTEXT("CustomFileHint", "Path to a .png file"))
				.OnTextCommitted_Lambda([this](const FText& Text, ETextCommit::Type)
				{
					FMantleThumbnailChoice Next = Current();
					if (Next.ImageFile.FilePath != Text.ToString())
					{
						Next.ImageFile.FilePath = Text.ToString();
						Next.Texture.Reset();
						SetChoice(Next);
					}
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("CustomBrowse", "Browse..."))
				.OnClicked_Lambda([this]
				{
					IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
					if (!DesktopPlatform)
					{
						return FReply::Handled();
					}
					const void* ParentWindow = FSlateApplication::IsInitialized() ? FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr) : nullptr;
					const FString Existing = Current().ImageFile.FilePath;
					TArray<FString> Files;
					if (DesktopPlatform->OpenFileDialog(ParentWindow, TEXT("Choose the thumbnail picture"), Existing.IsEmpty() ? FString() : FPaths::GetPath(Existing),
						FString(), TEXT("PNG image (*.png)|*.png"), EFileDialogFlags::None, Files) && !Files.IsEmpty())
					{
						FMantleThumbnailChoice Next = Current();
						Next.ImageFile.FilePath = FPaths::ConvertRelativePathToFull(Files[0]);
						Next.Texture.Reset();
						SetChoice(Next);
					}
					return FReply::Handled();
				})
			]
		];
}

void SMantleThumbnailSection::Refresh()
{
	Result = FMantleThumbnailResolver::Resolve(Current(), Target, Render);
	Brush.Reset();
	if (Result.Succeeded())
	{
		// FColor is BGRA in memory, the order the dynamic brush takes. A new name each time, so Slate never shows the old picture.
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Result.Pixels.GetData()), Result.Pixels.Num() * sizeof(FColor));
		static int32 Serial = 0;
		Brush = FSlateDynamicImageBrush::CreateWithImageData(*FString::Printf(TEXT("MantleThumbnailSection_%d"), ++Serial),
			FVector2f(Target.Width, Target.Height), Bytes);
	}
	Invalidate(EInvalidateWidgetReason::Layout | EInvalidateWidgetReason::Paint);
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleThumbnailSectionDevShot
{
	/** Widget rendered offscreen at Width over the editor's panel grey, as a PNG (the same way as the Cosmetics panel's dev shots). */
	bool Save(const TSharedRef<SWidget>& Widget, const FString& File, const float Width, const float MaxHeight)
	{
		FWidgetRenderer Renderer(true, false);
		UTextureRenderTarget2D* Target = FWidgetRenderer::CreateTargetFor(FVector2D(Width, MaxHeight), TF_Bilinear, true);
		Target->AddToRoot();
		Renderer.DrawWidget(Target, Widget, FVector2D(Width, MaxHeight), 0.0f);
		Widget->SlatePrepass(1.0f);
		const float Height = FMath::Clamp(FMath::CeilToFloat(Widget->GetDesiredSize().Y) + 16.0f, 64.0f, MaxHeight);
		Target->ResizeTarget(FMath::CeilToInt(Width), FMath::CeilToInt(Height));
		Renderer.DrawWidget(Target, Widget, FVector2D(Width, Height), 0.0f);
		FlushRenderingCommands();
		TArray<FColor> Pixels;
		FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
		const bool bRead = Resource && Resource->ReadPixels(Pixels);
		Target->RemoveFromRoot();
		if (!bRead)
		{
			return false;
		}
		const FColor Back(26, 26, 26);
		for (FColor& Pixel : Pixels)
		{
			const float Alpha = Pixel.A / 255.0f;
			Pixel = FColor(FMath::RoundToInt(Pixel.R * Alpha + Back.R * (1 - Alpha)), FMath::RoundToInt(Pixel.G * Alpha + Back.G * (1 - Alpha)),
				FMath::RoundToInt(Pixel.B * Alpha + Back.B * (1 - Alpha)), 255);
		}
		const bool bSaved = FFileHelper::SaveArrayToFile(FMantlePng::EncodeRGBA8(Target->SizeX, Target->SizeY, Pixels), *File);
		UE_LOG(LogMantle, Display, TEXT("MANTLE_THUMBNAIL_SHOT %s %dx%d %s"), *File, Target->SizeX, Target->SizeY, bSaved ? TEXT("saved") : TEXT("not saved"));
		return bSaved;
	}

	void Run(const TArray<FString>& Args)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_THUMBNAIL_SHOT usage: Mantle.Dev.ThumbnailSectionShot <Directory> <Sample.png> [Exit]"));
			return;
		}
		// The sample stands in for the kind's own render in Automatic and Choose the angle; the catalog thumbnail capture draws the real ones.
		FMantleThumbnailChoice Sample;
		Sample.Mode = EMantleThumbnailMode::Custom;
		Sample.ImageFile.FilePath = Args[1];
		TArray<FColor> SamplePixels;
		int32 SampleWidth = 0, SampleHeight = 0;
		FString Error;
		if (!FMantleThumbnailResolver::LoadCustom(Sample, SamplePixels, SampleWidth, SampleHeight, Error))
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_THUMBNAIL_SHOT sample: %s"), *Error);
			return;
		}
		const FMantleThumbnailResolver::FRender Render = [&SamplePixels, SampleWidth, SampleHeight](const FMantleThumbnailChoice&, TArray<FColor>& Out, int32& W, int32& H, FString&)
		{
			Out = SamplePixels;
			W = SampleWidth;
			H = SampleHeight;
			return true;
		};
		struct FShot { const TCHAR* Name; FMantleThumbnailTarget Target; EMantleThumbnailMode Mode; };
		const FShot Shots[] = {
			{ TEXT("square_automatic"), FMantleThumbnailTarget::Square(), EMantleThumbnailMode::Automatic },
			{ TEXT("square_angle"), FMantleThumbnailTarget::Square(), EMantleThumbnailMode::Angle },
			{ TEXT("square_custom"), FMantleThumbnailTarget::Square(), EMantleThumbnailMode::Custom },
			{ TEXT("wide_custom"), FMantleThumbnailTarget::Wide(), EMantleThumbnailMode::Custom },
		};
		for (const FShot& Shot : Shots)
		{
			FMantleThumbnailChoice Choice = Sample;
			Choice.Mode = Shot.Mode;
			const TSharedRef<SMantleThumbnailSection> Section = SNew(SMantleThumbnailSection)
				.Target(Shot.Target)
				.Choice(Choice)
				.Render(Render)
				.OnUseView_Lambda([]() { return FText::GetEmpty(); });
			Save(SNew(SBox).Padding(12.0f)[Section], FPaths::Combine(Args[0], FString::Printf(TEXT("thumbnail_section_%s.png"), Shot.Name)), 560.0f, 1400.0f);
		}
		// A maintainer run draws and goes: an editor left open would hold the plugin's binaries.
		if (Args.Num() > 2 && Args[2].Equals(TEXT("Exit"), ESearchCase::IgnoreCase))
		{
			FPlatformMisc::RequestExit(false);
		}
	}

	/**
	 * A real model through each choice, as a vehicle, weapon or consumable gets it (FMantleModelThumbnail): the catalog thumbnail capture of the model
	 * (Automatic and Choose the angle) and a custom image, and the section with its Turn / Tilt / Zoom sliders on the chosen angle. Uploads nothing.
	 */
	void RunModelSamples(const TArray<FString>& Args)
	{
		if (Args.Num() < 3)
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_THUMBNAIL_SHOT usage: Mantle.Dev.ThumbnailModelSamples <Model> <Directory> <Custom.png> [Exit]"));
			return;
		}
		FString Path = Args[0];
		if (!FPackageName::GetShortName(Path).Contains(TEXT(".")))
		{
			Path += TEXT(".") + FPackageName::GetShortName(Path);
		}
		UObject* Model = LoadObject<UObject>(nullptr, *Path);
		if (!Model)
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_THUMBNAIL_SHOT model %s did not load"), *Path);
			return;
		}
		IFileManager::Get().MakeDirectory(*Args[1], true);
		const FMantleThumbnailResolver::FRender Render = FMantleModelThumbnail::RenderFor([Model]() -> UObject* { return Model; });
		struct FSample { const TCHAR* Name; EMantleThumbnailMode Mode; };
		for (const FSample& Sample : { FSample{ TEXT("automatic"), EMantleThumbnailMode::Automatic }, FSample{ TEXT("angle"), EMantleThumbnailMode::Angle },
			FSample{ TEXT("custom"), EMantleThumbnailMode::Custom } })
		{
			FMantleThumbnailChoice Choice;
			Choice.Mode = Sample.Mode;
			Choice.Angle = FRotator(-12.0, 150.0, 0.0);
			Choice.Zoom = 1.3f;
			Choice.ImageFile.FilePath = Args[2];
			const FMantleThumbnailResult Picture = FMantleThumbnailResolver::Resolve(Choice, FMantleThumbnailTarget::Square(), Render);
			const FString File = FPaths::Combine(Args[1], FString::Printf(TEXT("model_thumbnail_%s.png"), Sample.Name));
			const bool bSaved = Picture.Succeeded() && FFileHelper::SaveArrayToFile(FMantleThumbnailResolver::ToPng(Picture, FMantleThumbnailTarget::Square()), *File);
			UE_LOG(LogMantle, Display, TEXT("MANTLE_THUMBNAIL_SHOT %s %s %s"), *File, bSaved ? TEXT("saved") : TEXT("NOT saved"), *(Picture.Succeeded() ? Picture.Source : Picture.Error));
			if (Sample.Mode == EMantleThumbnailMode::Angle)
			{
				const TSharedRef<SMantleThumbnailSection> Section = SNew(SMantleThumbnailSection)
					.Target(FMantleThumbnailTarget::Square())
					.AngleSliders(true)
					.DefaultAngle(FMantleModelThumbnail::DefaultView())
					.Choice(Choice)
					.Render(Render);
				Save(SNew(SBox).Padding(12.0f)[Section], FPaths::Combine(Args[1], TEXT("model_section_angle.png")), 560.0f, 1400.0f);
			}
		}
		if (Args.Num() > 3 && Args[3].Equals(TEXT("Exit"), ESearchCase::IgnoreCase))
		{
			FPlatformMisc::RequestExit(false);
		}
	}

	/**
	 * The vehicle builder's section on Automatic after a build: builds the sedan chassis in a scratch package, then draws the section with the builder's own
	 * render (FMantleVehicleThumbnail::PreviewFor over the built vehicle). Before the build it draws the same section too. Uploads nothing.
	 */
	void RunVehicleSamples(const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_THUMBNAIL_SHOT usage: Mantle.Dev.VehicleThumbnailShot <Directory> [Exit]"));
			return;
		}
		IFileManager::Get().MakeDirectory(*Args[0], true);
		FMantleChassisSpec Spec;
		FString Error;
		const bool bSpec = FMantleVehicleChassisBuilder::LoadSpec(TEXT("sedan"), Spec, Error);
		const UMantleVehicleSource* Built = nullptr;
		FMantleVehicleBuildResult Result;
		const FMantleThumbnailResolver::FRender Render = FMantleVehicleThumbnail::PreviewFor([&Built] { return Built; }, []() -> UObject* { return nullptr; });
		const auto Draw = [&](const TCHAR* Name)
		{
			const TSharedRef<SMantleThumbnailSection> Section = SNew(SMantleThumbnailSection)
				.Target(FMantleThumbnailTarget::Square())
				.AngleSliders(true)
				.DefaultAngle(FMantleModelThumbnail::DefaultView())
				.Choice(FMantleThumbnailChoice())
				.Render(Render);
			Save(SNew(SBox).Padding(12.0f)[Section], FPaths::Combine(Args[0], FString::Printf(TEXT("vehicle_section_%s.png"), Name)), 560.0f, 1400.0f);
		};
		Draw(TEXT("before_build"));
		if (bSpec)
		{
			Result = FMantleVehicleBuilder::BuildChassis(Spec, TEXT("/Temp/MantleVehicleThumbnailShot"), TEXT("ShotSedan"));
			Built = Result.Source;
			UE_LOG(LogMantle, Display, TEXT("MANTLE_THUMBNAIL_SHOT build %s, picture %d bytes"), Result.bOk ? TEXT("ok") : TEXT("FAILED"), Built ? Built->ThumbnailPng.Num() : 0);
			Draw(TEXT("after_build"));
		}
		else
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_THUMBNAIL_SHOT sedan spec: %s"), *Error);
		}
		if (Args.Num() > 1 && Args[1].Equals(TEXT("Exit"), ESearchCase::IgnoreCase))
		{
			FPlatformMisc::RequestExit(false);
		}
	}

	FAutoConsoleCommand VehicleCommand(
		TEXT("Mantle.Dev.VehicleThumbnailShot"),
		TEXT("Mantle.Dev.VehicleThumbnailShot <Directory> [Exit]: the vehicle builder's Thumbnail section on Automatic before and after a build of the sedan; with Exit the editor closes afterwards. Uploads nothing."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunVehicleSamples));

	FAutoConsoleCommand ModelCommand(
		TEXT("Mantle.Dev.ThumbnailModelSamples"),
		TEXT("Mantle.Dev.ThumbnailModelSamples <Model> <Directory> <Custom.png> [Exit]: a model through each thumbnail choice (the catalog capture), and the section with its angle sliders. Uploads nothing."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&RunModelSamples));

	FAutoConsoleCommand Command(
		TEXT("Mantle.Dev.ThumbnailSectionShot"),
		TEXT("Mantle.Dev.ThumbnailSectionShot <Directory> <Sample.png> [Exit]: renders the shared Thumbnail section offscreen in each choice, square and wide, using the sample picture; with Exit the editor closes afterwards. Uploads nothing."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&Run));
}

#undef LOCTEXT_NAMESPACE
