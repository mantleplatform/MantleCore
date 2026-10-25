// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCosmeticPanel.h"
#include "MantleAccessorySettings.h"
#include "MantleAssetStore.h"
#include "MantleAvatarContract.h"
#include "MantleAvatarStripFixer.h"
#include "MantleBuiltInMannequin.h"
#include "MantleCharacterSplit.h"
#include "MantleCosmeticItem.h"
#include "MantleCosmeticPreview.h"
#include "MantleCosmeticPreviewRetarget.h"
#include "MantleCosmeticPreviewViewport.h"
#include "MantleCosmeticThumbnail.h"
#include "MantleThumbnailStore.h"
#include "MantleUnriggedAsIs.h"
#include "SMantleThumbnailSection.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "MantleCosmeticUpload.h"
#include "MantleCosmeticValidation.h"
#include "MantleClothingColorPreview.h"
#include "MantleCreatorSignIn.h"
#include "MantleEditorActions.h"
#include "MantleGarmentOutfit.h"
#include "MantleHairColor.h"
#include "MantleHairFitPictures.h"
#include "MantleHairGeneration.h"
#include "MantleMetaHumanBuild.h"
#include "MantleMetaHumanPose.h"
#include "MantleParametricOutfit.h"
#include "MantleOverlayChecks.h"
#include "MantleOverlayWorn.h"
#include "SMantleOverlayPlaceViewport.h"
#include "MantleReferenceBodies.h"
#include "MantleOverlayGuide.h"
#include "MantleParametricOutfitAuthoring.h"
#include "MantleReferenceBodies.h"
#include "MantleRuntimeTextureBuilder.h"
#include "MantleRuntime.h"
#include "MantleSettings.h"
#include "MantleTestLocally.h"
#include "SMantlePreviewPanel.h"
#include "MetaHumanCharacter.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "ChaosClothAsset/ClothAssetBase.h"
#include "ChaosOutfitAsset/OutfitAsset.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "DesktopPlatformModule.h"
#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "IDesktopPlatform.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/Paths.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "MantlePng.h"
#include "Misc/FileHelper.h"
#include "Slate/WidgetRenderer.h"
#include "TextureResource.h"
#include "Framework/Docking/TabManager.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "Modules/ModuleManager.h"
#include "PropertyCustomizationHelpers.h"
#include "Styling/AppStyle.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "Widgets/Images/SImage.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Colors/SColorPicker.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MantleCosmetics"

const FName SMantleCosmeticPanel::TabName(TEXT("MantleCosmetics"));

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCosmeticPanelPrivate
{
	TSharedRef<SDockTab> SpawnCosmeticsTab(const FSpawnTabArgs&)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			.Label(LOCTEXT("TabLabel", "Mantle Cosmetics"))
			[
				SNew(SMantleCosmeticPanel)
			];
	}

	FText ClassHelp(EMantleCosmeticClass Class)
	{
		switch (Class)
		{
		case EMantleCosmeticClass::Hair:
			return LOCTEXT("HairHelp", "Hair, eyebrows, eyelashes or facial hair, each its own slot and worn together: a groom (its cards are converted), a hair cards static mesh, or one section of a mesh such as a MetaHuman face's eyelashes. It binds to the head of the body it was made on.");
		case EMantleCosmeticClass::Garment:
			return LOCTEXT("GarmentHelp", "Choose a skeletal garment or a parametric Outfit. Create an Outfit from the garment and its original body/head, then publish one clothing item. Sizes are made for you from a MetaHuman Outfit, a garment made on a MetaHuman, or a garment with its character's bare body beside it.");
		case EMantleCosmeticClass::Accessory:
			return LOCTEXT("AccessoryHelp", "A static mesh with one material. Its pivot is where it attaches to the socket.");
		case EMantleCosmeticClass::Avatar:
			return LOCTEXT("AvatarHelp", "Pick your character below: a skeletal mesh, a static mesh, a MetaHuman Creator character or a MetaHuman Blueprint, and choose Convert. Mantle builds it as a MetaHuman at Mantle's default height and checks whether the body and face are ready. Choose Use, then Upload to Mantle to add it to the catalog. The upload carries supported body and face geometry, required rig data, and a presentation thumbnail; the game supplies appearance materials and textures.");
		case EMantleCosmeticClass::Emote:
			return LOCTEXT("EmoteHelp", "An animation on the UEFN mannequin, MetaHuman, UE5 Mannequin or UE4 Mannequin skeleton, at most 10 seconds. MetaHuman, UE5 Mannequin and UE4 Mannequin clips are retargeted onto the UEFN mannequin when they upload.");
		case EMantleCosmeticClass::Overlay:
			return LOCTEXT("OverlayHelp", "A scar or tattoo players can wear on a converted MetaHuman avatar. Paint a square mask (a power of two, at most 1024 x 1024) to the MetaHuman UV layout of the body or the face, in white on black, as a greyscale image or in the red channel only. Scars may add a normal map. Pick the slot above: its name says whether it is drawn on the body or the face.");
		default:
			return FText::GetEmpty();
		}
	}

	FSlateColor SeverityColour(EMantleCosmeticSeverity Severity)
	{
		switch (Severity)
		{
		case EMantleCosmeticSeverity::Error: return FSlateColor(FLinearColor(0.95f, 0.35f, 0.30f));
		case EMantleCosmeticSeverity::Warning: return FSlateColor(FLinearColor(0.95f, 0.75f, 0.25f));
		default: return FSlateColor(FLinearColor(0.55f, 0.80f, 0.55f));
		}
	}

	const TCHAR* StateText(EMantleUploadAssetState State)
	{
		switch (State)
		{
		case EMantleUploadAssetState::Waiting: return TEXT("waiting");
		case EMantleUploadAssetState::Processing: return TEXT("processing");
		case EMantleUploadAssetState::Done: return TEXT("ready");
		case EMantleUploadAssetState::Failed: return TEXT("failed");
		default: return TEXT("");
		}
	}

	/** An animation sequence named like an idle on Skeleton (or a compatible one), the shortest name first; null when none. */
	UAnimSequence* FindIdleAnimation(const USkeleton* Skeleton)
	{
		if (!Skeleton)
		{
			return nullptr;
		}
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByClass(UAnimSequence::StaticClass()->GetClassPathName(), Assets);
		Assets.Sort([](const FAssetData& A, const FAssetData& B)
		{
			const int32 LengthA = A.AssetName.GetStringLength();
			const int32 LengthB = B.AssetName.GetStringLength();
			return LengthA != LengthB ? LengthA < LengthB : A.PackageName.LexicalLess(B.PackageName);
		});
		for (const FAssetData& Data : Assets)
		{
			if (Data.AssetName.ToString().Contains(TEXT("Idle")) && Skeleton->IsCompatibleForEditor(Data))
			{
				if (UAnimSequence* Sequence = Cast<UAnimSequence>(Data.GetAsset()))
				{
					return Sequence;
				}
			}
		}
		return nullptr;
	}

	/** #283: the fit preview's pick, kept for the editor session (the panel reopens on it). */
	TOptional<FMantlePreviewPick>& RememberedPreviewPick()
	{
		static TOptional<FMantlePreviewPick> Pick;
		return Pick;
	}
}

void SMantleCosmeticPanel::RegisterTab()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName, FOnSpawnTab::CreateStatic(&MantleCosmeticPanelPrivate::SpawnCosmeticsTab))
		.SetDisplayName(LOCTEXT("TabTitle", "Mantle Cosmetics"))
		.SetTooltipText(LOCTEXT("TabTooltip", "Publish hair, clothing, accessories, avatars and emotes to Mantle."))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
}

void SMantleCosmeticPanel::UnregisterTab()
{
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
	}
}

void SMantleCosmeticPanel::OpenTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(TabName);
}

SMantleCosmeticPanel::~SMantleCosmeticPanel()
{
	SaveAccessoryPlacement();
	// Closing the panel stops sending; what was uploaded stays and the next run resumes.
	if (Session.IsValid() && !Session->IsFinished())
	{
		Session->Cancel();
	}
	// The preview's downloads nobody waits on any more are cancelled; its fits still running finish and are dropped.
	if (PreviewSession.IsValid())
	{
		PreviewSession->OnChanged = nullptr;
		PreviewSession->Cancel();
		PreviewSession.Reset();
	}
}

void SMantleCosmeticPanel::Construct(const FArguments& InArgs)
{
	using namespace MantleCosmeticPanelPrivate;

	FMantleAvatarContract::Get(&ContractError);

	// The fit preview: catalog lookups through the creator's sign-in, downloads through the process's asset store.
	PreviewSession = FMantleCosmeticPreviewSession::Create(FMantleCosmeticPreviewSettings(), MantleCosmetic::CreateApiResolver(),
		FMantleAssetStore::Get().AsShared());
	PreviewSession->OnChanged = [this](const FMantlePreviewView& View)
	{
		if (PreviewViewport.IsValid())
		{
			PreviewViewport->Show(View);
			ApplyPreviewAnimation();
			RefreshAccessoryPreview();
		}
	};
	// #283: preview on the source body by default, or on what this editor session picked last.
	for (const FMantlePreviewPick& Option : FMantlePreviewPick::Options())
	{
		PreviewPickOptions.Add(MakeShared<FMantlePreviewPick>(Option));
	}
	PreviewPick = FMantlePreviewPick::Initial(RememberedPreviewPick().GetPtrOrNull());

	for (const EMantleCosmeticClass Class : { EMantleCosmeticClass::Hair, EMantleCosmeticClass::Garment, EMantleCosmeticClass::Accessory,
		EMantleCosmeticClass::Avatar, EMantleCosmeticClass::Emote, EMantleCosmeticClass::Overlay })
	{
		ClassOptions.Add(MakeShared<EMantleCosmeticClass>(Class));
	}
	for (const TCHAR* Role : { TEXT("skin"), TEXT("eyes"), TEXT("other") })
	{
		RoleOptions.Add(MakeShared<FString>(Role));
	}
	// This panel creates new catalog items; existing items are updated through their explicit API path.
	PublishAsOptions.Add(MakeShared<FString>());
	const auto PublishAsText = [](const FString&)
	{
		return FText::FromString(FMantleCosmeticChecks::PublishAsNewItemLabel);
	};

	const TAttribute<EVisibility> AvatarOnly = TAttribute<EVisibility>::CreateLambda([this]
	{
		return Draft.Class == EMantleCosmeticClass::Avatar ? EVisibility::Visible : EVisibility::Collapsed;
	});
	const TAttribute<EVisibility> AccessoryOnly = TAttribute<EVisibility>::CreateLambda([this]
	{
		return Draft.Class == EMantleCosmeticClass::Accessory ? EVisibility::Visible : EVisibility::Collapsed;
	});
	const TAttribute<EVisibility> EmoteOnly = TAttribute<EVisibility>::CreateLambda([this]
	{
		return Draft.Class == EMantleCosmeticClass::Emote ? EVisibility::Visible : EVisibility::Collapsed;
	});
	const TAttribute<EVisibility> OverlayOnly = TAttribute<EVisibility>::CreateLambda([this]
	{
		return Draft.Class == EMantleCosmeticClass::Overlay ? EVisibility::Visible : EVisibility::Collapsed;
	});
	const TAttribute<EVisibility> Previewable = TAttribute<EVisibility>::CreateLambda([this]
	{
		return Draft.Class == EMantleCosmeticClass::Garment || Draft.Class == EMantleCosmeticClass::Hair || Draft.Class == EMantleCosmeticClass::Avatar || Draft.Class == EMantleCosmeticClass::Accessory || Draft.Class == EMantleCosmeticClass::Overlay ? EVisibility::Visible : EVisibility::Collapsed;
	});

	// #124: every class takes the shared Thumbnail section.
	const TAttribute<EVisibility> Thumbnailable = TAttribute<EVisibility>::CreateLambda([this]
	{
		return EVisibility::Visible;
	});

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

				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
				[
					SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
					.Text(LOCTEXT("Heading", "Publish to the Mantle catalog"))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 10)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(this, &SMantleCosmeticPanel::GetSignInText)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Visibility_Lambda([this] { return ContractError.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
					.ColorAndOpacity(SeverityColour(EMantleCosmeticSeverity::Error))
					.Text_Lambda([this] { return FText::FromString(ContractError); })
				]

				// What it is, and the slot it goes in.
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeSection(LOCTEXT("WhatTitle", "What are you publishing?"),
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 0, 8, 0)
							[
								SNew(SComboBox<TSharedPtr<EMantleCosmeticClass>>)
								.OptionsSource(&ClassOptions)
								.InitiallySelectedItem(ClassOptions[1])
								.OnGenerateWidget_Lambda([](TSharedPtr<EMantleCosmeticClass> Item)
								{
									return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? LexToString(*Item) : TEXT("")));
								})
								.OnSelectionChanged_Lambda([this](TSharedPtr<EMantleCosmeticClass> Item, ESelectInfo::Type)
								{
									if (Item.IsValid())
									{
										SetClass(*Item);
									}
								})
								[
									SNew(STextBlock).Text_Lambda([this] { return FText::FromString(LexToString(Draft.Class)); })
								]
							]
							+ SHorizontalBox::Slot().FillWidth(1.0f)
							[
								SAssignNew(SlotCombo, SComboBox<TSharedPtr<FString>>)
								.OptionsSource(&SlotOptions)
								.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
								{
									return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString()));
								})
								.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type)
								{
									if (Item.IsValid())
									{
										SetSlot(*Item);
									}
								})
								[
									SNew(STextBlock).Text_Lambda([this]
									{
										return Draft.Slot.IsEmpty() ? LOCTEXT("ChooseSlot", "Slot") : FText::FromString(Draft.Slot);
									})
								]
							]
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
						[
							SNew(STextBlock)
							.AutoWrapText(true)
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
							.Text_Lambda([this] { return ClassHelp(Draft.Class); })
						]
						// #483: which sections of the garment mesh this item takes (RefreshOutfitSections).
						+ SVerticalBox::Slot().AutoHeight()
						[
							SAssignNew(OutfitBox, SVerticalBox)
						])
				]

				// The asset.
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeSection(LOCTEXT("SourceTitle", "Asset"),
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SAssignNew(SourcePickerBox, SBox)
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
						[
							SNew(SVerticalBox)
							.Visibility_Lambda([this] { return Draft.Class == EMantleCosmeticClass::Hair && FMantleCosmeticExport::IsGroomAsset(Draft.Source.Get()) ? EVisibility::Visible : EVisibility::Collapsed; })
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(SButton).Text(LOCTEXT("GenerateHairCards", "Generate cards from strands"))
								.IsEnabled_Lambda([this] { return bCanGenerateHairCards; })
								.OnClicked_Lambda([this]
								{
									FString Error;
									if (FMantleHairGeneration::Generate(Draft.Source.Get(), Error, Draft.Slot))
									{
										Draft.CardsEntry = INDEX_NONE;
										Package.Reset();
										RefreshDetails();
										HairGenerationStatus = TEXT("Cards generated. Check the item and preview their placement before uploading.");
										return OnCheck();
									}
									HairGenerationStatus = Error;
									return FReply::Handled();
									})
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
							[
								SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground())
								.Text_Lambda([this] { return HairGenerationStatus.IsEmpty() ? LOCTEXT("GenerateHairCardsHelp", "Opens the installed native generator's settings. Its starting targets are 500 clumps and the selected slot triangle limit, with a 2,048-pixel atlas; review the generated output before publishing.") : FText::FromString(HairGenerationStatus); })
							]
							]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
						[
							SNew(SVerticalBox)
							.Visibility_Lambda([this] { return Draft.Class == EMantleCosmeticClass::Garment && FMantleParametricOutfit::IsSource(Draft.Source.Get()) ? EVisibility::Visible : EVisibility::Collapsed; })
							+ SVerticalBox::Slot().AutoHeight()
							[
								SAssignNew(OutfitSizeCombo, SComboBox<TSharedPtr<FString>>)
								.OptionsSource(&OutfitSizeOptions)
								.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) { return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString())); })
								.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type)
								{
									Draft.OutfitSourceSize = Item.IsValid() ? *Item : FString();
									Package.Reset();
									if (PreviewSession.IsValid()) PreviewSession->Cancel();
									OutfitPreviewKeepAlive.Reset();
								})
								[
									SNew(STextBlock).Text_Lambda([this] { return Draft.OutfitSourceSize.IsEmpty() ? LOCTEXT("ChooseOutfitSize", "Automatic source size (preview)") : FText::FromString(Draft.OutfitSourceSize); })
								]
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
							[
								SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground())
								.Text_Lambda([this] { return OutfitSizeError.IsEmpty() ? LOCTEXT("OutfitSizeHelp", "The source-size choice is for preview diagnostics. Uploads automatically select from every authored size for each fit source.") : FText::FromString(OutfitSizeError); })
							]
							]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
						[
							SAssignNew(CardsCombo, SComboBox<TSharedPtr<FString>>)
							.Visibility_Lambda([this] { return CardsOptions.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed; })
							.OptionsSource(&CardsOptions)
							.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
							{
								return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString()));
							})
							.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type)
							{
								Draft.CardsEntry = CardsOptions.IndexOfByKey(Item);
								Package.Reset();
							})
							[
								SNew(STextBlock).Text_Lambda([this]
								{
									return CardsOptions.IsValidIndex(Draft.CardsEntry) ? FText::FromString(*CardsOptions[Draft.CardsEntry])
										: LOCTEXT("DefaultCards", "Cards: automatic (finest LOD that fits the budget)");
								})
							]
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
						[
							// #325, #326: a groom's cards upload with the colour they are drawn in, read from the groom's
							// material unless set here.
							SNew(SHorizontalBox)
							.Visibility_Lambda([this]
							{
								return Draft.Class == EMantleCosmeticClass::Hair && Draft.Source.IsValid() && !Draft.Source->IsA<USkeletalMesh>()
									? EVisibility::Visible : EVisibility::Collapsed;
							})
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
							[
								SNew(STextBlock).Text(LOCTEXT("HairColorLabel", "Hair colour"))
							]
							+ SHorizontalBox::Slot().FillWidth(1.0f)
							[
								SNew(SEditableTextBox)
								.HintText(LOCTEXT("HairColorHint", "#RRGGBB; empty reads the groom's material"))
								.Text_Lambda([this] { return FText::FromString(HairColorText); })
								.OnTextCommitted_Lambda([this](const FText& Text, ETextCommit::Type)
								{
									HairColorText = Text.ToString().TrimStartAndEnd();
									FLinearColor Parsed = FLinearColor::White;
									if (!HairColorText.IsEmpty() && FMantleHairColor::ParseHex(HairColorText, Parsed))
									{
										Draft.HairColor = Parsed;
										HairColorText = FMantleHairColor::ToHex(Parsed);
									}
									else
									{
										Draft.HairColor.Reset();
									}
									Package.Reset();
								})
							]
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
							[
								SNew(STextBlock)
								.Visibility_Lambda([this] { return !HairColorText.IsEmpty() && !Draft.HairColor.IsSet() ? EVisibility::Visible : EVisibility::Collapsed; })
								.Text(LOCTEXT("HairColorInvalid", "Not a colour: the groom's is used"))
							]
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
						[
							SAssignNew(SectionCombo, SComboBox<TSharedPtr<FString>>)
							.Visibility_Lambda([this] { return SectionOptions.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed; })
							.OptionsSource(&SectionOptions)
							.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
							{
								return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString()));
							})
							.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type)
							{
								const int32 Picked = SectionOptions.IndexOfByKey(Item);
								Draft.SourceSection = Picked == SectionOptions.Num() - 1 ? INDEX_NONE : Picked;
								Package.Reset();
							})
							[
								SNew(STextBlock).Text_Lambda([this]
								{
									return SectionOptions.IsValidIndex(Draft.SourceSection)
										? FText::Format(LOCTEXT("SectionChosen", "Section: {0}"), FText::FromString(*SectionOptions[Draft.SourceSection]))
										: LOCTEXT("ChooseSection", "Whole hair mesh (all sections)");
								})
							]
						])
				]

				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeSection(LOCTEXT("ParametricTitle", "Parametric clothing"), MakeParametricOutfitSection(),
						TAttribute<EVisibility>::CreateLambda([this] { return Draft.Class == EMantleCosmeticClass::Garment ? EVisibility::Visible : EVisibility::Collapsed; }))
				]

				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeSection(LOCTEXT("ClothingColorsTitle", "Clothing colors"), SAssignNew(ClothingColorsBox, SVerticalBox),
						TAttribute<EVisibility>::CreateLambda([this] { return Draft.Class == EMantleCosmeticClass::Garment ? EVisibility::Visible : EVisibility::Collapsed; }))
				]

				// #610: make a character upload-ready, then take its parts from here. Under Avatar only (the one door): clothing
				// and hair pick their own meshes, as before.
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeSection(LOCTEXT("ConvertTitle", "Character"), MakeConvertSection(),
						TAttribute<EVisibility>::CreateLambda([this]
						{
							return Draft.Class == EMantleCosmeticClass::Avatar ? EVisibility::Visible : EVisibility::Collapsed;
						}))
				]

				// The body it was authored on (garments, hair).
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeSection(LOCTEXT("BodyTitle", "Authored on"),
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SVerticalBox)
							.Visibility_Lambda([this] { return Draft.Class == EMantleCosmeticClass::Garment && FMantleParametricOutfit::IsSource(Draft.Source.Get()) ? EVisibility::Visible : EVisibility::Collapsed; })
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(STextBlock).Text(LOCTEXT("OutfitAuthoringCharacter", "Authoring MetaHuman Creator character"))
							]
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(SObjectPropertyEntryBox)
								.AllowedClass(UMetaHumanCharacter::StaticClass())
								.AllowClear(true).DisplayThumbnail(false)
								.ObjectPath_Lambda([this] { return Draft.OutfitAuthoringCharacter.IsValid() ? Draft.OutfitAuthoringCharacter->GetPathName() : FString(); })
								.OnObjectChanged_Lambda([this](const FAssetData& Asset)
								{
									Draft.OutfitAuthoringCharacter = Cast<UMetaHumanCharacter>(Asset.GetAsset());
									Draft.AuthoringBody.Reset(); Draft.AuthoringFace.Reset();
									Package.Reset();
									if (PreviewSession.IsValid()) PreviewSession->Cancel();
									OutfitPreviewKeepAlive.Reset();
									RefreshOutfitAuthoringDefaults();
								})
							]
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground())
								.Text(LOCTEXT("OutfitAuthoringCharacterHelp", "Choose the Creator character that produced the converted body and face below. A unique matching converted pair is selected automatically; choose the body explicitly when several builds exist."))
							]
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SObjectPropertyEntryBox)
							.AllowedClass(USkeletalMesh::StaticClass())
							.OnShouldFilterAsset_Lambda([this](const FAssetData& Asset) { return Draft.Class == EMantleCosmeticClass::Garment && FMantleParametricOutfit::IsSource(Draft.Source.Get()) && !IsOutfitAuthoringCandidate(Asset, false); })
							.AllowClear(true)
							.DisplayThumbnail(false)
							.ObjectPath_Lambda([this] { return Draft.AuthoringBody.IsValid() ? Draft.AuthoringBody->GetPathName() : FString(); })
							.OnObjectChanged_Lambda([this](const FAssetData& Asset)
							{
								SetAuthoringBody(Cast<USkeletalMesh>(Asset.GetAsset()));
							})
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
						[
							SNew(STextBlock)
							.AutoWrapText(true)
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
							.Text_Lambda([this]
							{
								if (Draft.Class == EMantleCosmeticClass::Garment && FMantleParametricOutfit::IsSource(Draft.Source.Get()))
									return LOCTEXT("OutfitBodyHelp", "Choose a full converted body from this Creator character's build. The face must come from the same build; check, preview and upload use these same authoring inputs.");
								if (Draft.Class == EMantleCosmeticClass::Hair)
									return LOCTEXT("HairBodyHelp", "Choose the hair's actual authoring body or head rig and its matching source face. The source face guides fitting to supported avatars; a body shape is not uploaded for head-fitted hair.");
								return Draft.Class == EMantleCosmeticClass::Accessory
									? LOCTEXT("AccessoryBodyHelp", "A local MetaHuman body for preview and its skeleton for catalog previews. Its body shape is not uploaded with a rigid accessory.")
									: LOCTEXT("BodyHelp", "The MetaHuman body this was made on. Found automatically when the project has one; it uploads as the item's body shape for fitting to supported avatars.");
							})
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
						[
							SNew(SVerticalBox)
							.Visibility_Lambda([this] { return Draft.Class == EMantleCosmeticClass::Hair || (Draft.Class == EMantleCosmeticClass::Garment && FMantleParametricOutfit::IsSource(Draft.Source.Get())) ? EVisibility::Visible : EVisibility::Collapsed; })
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(STextBlock).Text(LOCTEXT("HairSourceHead", "Source MetaHuman face"))
							]
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(SObjectPropertyEntryBox)
								.AllowedClass(USkeletalMesh::StaticClass())
								.OnShouldFilterAsset_Lambda([this](const FAssetData& Asset) { return Draft.Class == EMantleCosmeticClass::Garment && FMantleParametricOutfit::IsSource(Draft.Source.Get()) && !IsOutfitAuthoringCandidate(Asset, true); })
								.AllowClear(true)
								.DisplayThumbnail(false)
								.ObjectPath_Lambda([this] { return Draft.AuthoringFace.IsValid() ? Draft.AuthoringFace->GetPathName() : FString(); })
								.OnObjectChanged_Lambda([this](const FAssetData& Asset) { Draft.AuthoringFace = Cast<USkeletalMesh>(Asset.GetAsset()); Package.Reset(); if (PreviewSession.IsValid()) PreviewSession->Cancel(); OutfitPreviewKeepAlive.Reset(); })
							]
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground())
								.Text_Lambda([this] { return Draft.Class == EMantleCosmeticClass::Hair ? LOCTEXT("HairSourceHeadHelp", "The head the hair was authored on. Leave empty to find the face beside the source body; select it explicitly for standalone cards.") : LOCTEXT("OutfitSourceHeadHelp", "The face from the same full converted build as the authoring body above."); })
							]
						],
						TAttribute<EVisibility>::CreateLambda([this] { return Draft.Class == EMantleCosmeticClass::Garment || Draft.Class == EMantleCosmeticClass::Hair || Draft.Class == EMantleCosmeticClass::Accessory ? EVisibility::Visible : EVisibility::Collapsed; }))
				]

				// #736: an overlay's images beyond the asset picker, and what it previews on.
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeSection(LOCTEXT("OverlayTitle", "Overlay images"), MakeOverlaySection(), OverlayOnly)
				]

				// #584: the body an emote was animated on (MetaHuman, UE5 or UE4 Mannequin clips), which it plays on while
				// it is retargeted onto the UEFN mannequin.
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeSection(LOCTEXT("EmoteBodyTitle", "Animated on"),
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SObjectPropertyEntryBox)
							.AllowedClass(USkeletalMesh::StaticClass())
							.AllowClear(true)
							.DisplayThumbnail(false)
							.OnShouldFilterAsset_Lambda([this](const FAssetData& Asset) { return !IsEmoteBodyCandidate(Asset); })
							.ObjectPath_Lambda([this] { return Draft.EmoteBody.IsValid() ? Draft.EmoteBody->GetPathName() : FString(); })
							.OnObjectChanged_Lambda([this](const FAssetData& Asset)
							{
								Draft.EmoteBody = Cast<USkeletalMesh>(Asset.GetAsset());
								Package.Reset();
							})
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
						[
							SNew(STextBlock)
							.AutoWrapText(true)
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
							.Text(LOCTEXT("EmoteBodyHelp", "The character this animation was made on, from the meshes on its skeleton (optional). A MetaHuman, UE5 Mannequin or UE4 Mannequin clip is retargeted onto Mantle's mannequin from this body's proportions. The animation's own preview mesh is used first when it has one; with neither, a mesh on the same skeleton is found."))
						],
						EmoteOnly)
				]

				// Whether the emote loops until the player moves, or plays once.
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeSection(LOCTEXT("EmotePlaybackTitle", "Playback"),
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SCheckBox)
							.IsChecked_Lambda([this] { return Draft.bEmoteLoop ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
							.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Draft.bEmoteLoop = State == ECheckBoxState::Checked; Package.Reset(); })
							[
								SNew(STextBlock).Text(LOCTEXT("EmoteLoopToggle", "Loop until the player moves"))
							]
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
						[
							SNew(STextBlock)
							.AutoWrapText(true)
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
							.Text(LOCTEXT("EmoteLoopHelp", "On: the emote keeps playing until the player moves, jumps or is hit (a dance, a wave that keeps going). Off: it plays once and ends (a bow). To loop only part of it, add an anim notify named loop_start where the loop begins; with Loop on and no such notify, the whole clip loops."))
						],
						EmoteOnly)
				]

				// Avatar details.
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeSection(LOCTEXT("AvatarTitle", "Face and sections"),
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
							[
								SNew(STextBlock).Text(LOCTEXT("PublishAsLabel", "Publish as"))
							]
							+ SHorizontalBox::Slot().FillWidth(1.0f)
							[
								SAssignNew(PublishAsCombo, SComboBox<TSharedPtr<FString>>)
								.OptionsSource(&PublishAsOptions)
								.InitiallySelectedItem(PublishAsOptions[0])
								.OnGenerateWidget_Lambda([PublishAsText](TSharedPtr<FString> Item)
								{
									return SNew(STextBlock).Text(PublishAsText(Item.IsValid() ? *Item : FString()));
								})
								.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type)
								{
									Draft.PublishIntoItemId = Item.IsValid() ? *Item : FString();
									Package.Reset();
								})
								[
									SNew(STextBlock).Text_Lambda([this, PublishAsText] { return PublishAsText(Draft.PublishIntoItemId); })
								]
							]
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SObjectPropertyEntryBox)
							.AllowedClass(USkeletalMesh::StaticClass())
							.AllowClear(true)
							.DisplayThumbnail(false)
							.ObjectPath_Lambda([this] { return Draft.Face.IsValid() ? Draft.Face->GetPathName() : FString(); })
							.OnObjectChanged_Lambda([this](const FAssetData& Asset)
							{
								Draft.Face = Cast<USkeletalMesh>(Asset.GetAsset());
								Package.Reset();
							})
						]
						// #594: a UE5, UE4 or UEFN Mannequin body's jaw, picked from the bones below its head; hidden for a MetaHuman
						// body, whose face brings its jaw (FACIAL_C_Jaw).
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
						[
							SNew(SVerticalBox)
							.Visibility_Lambda([this] { return bJawSlotOffered ? EVisibility::Visible : EVisibility::Collapsed; })
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(SHorizontalBox)
								+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
								[
									SNew(STextBlock).Text(LOCTEXT("JawLabel", "Jaw bone (optional)"))
								]
								+ SHorizontalBox::Slot().FillWidth(1.0f)
								[
									SAssignNew(JawCombo, SComboBox<TSharedPtr<FString>>)
									.OptionsSource(&JawOptions)
									.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
									{
										return SNew(STextBlock).Text(Item.IsValid() && !Item->IsEmpty() ? FText::FromString(*Item) : LOCTEXT("JawNone", "None"));
									})
									.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type)
									{
										Draft.JawBone = Item.IsValid() && !Item->IsEmpty() ? FName(**Item) : NAME_None;
										Package.Reset();
									})
									[
										SNew(STextBlock).Text_Lambda([this]
										{
											return Draft.JawBone.IsNone() ? LOCTEXT("JawNone", "None") : FText::FromName(Draft.JawBone);
										})
									]
								]
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
							[
								SNew(STextBlock)
								.AutoWrapText(true)
								.ColorAndOpacity(FSlateColor::UseSubduedForeground())
								.Text(LOCTEXT("JawHelp", "The bone that opens the mouth, from the bones below head. It uploads as jaw, and the avatar's mouth moves with voice chat. None: the mouth stays still."))
							]
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
						[
							SAssignNew(RolesBox, SVerticalBox)
						],
						AvatarOnly)
				]

				// #595: a UE5, UE4 or UEFN Mannequin body's bone chains mapped onto the secondary motion slots, with a preset each.
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeSection(LOCTEXT("SecondaryTitle", "Secondary motion"),
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
						[
							SNew(STextBlock)
							.AutoWrapText(true)
							.ColorAndOpacity(FSlateColor::UseSubduedForeground())
							.Text(LOCTEXT("SecondaryHelp", "Hair, chest, cloth and tail bones that swing as the avatar moves. Pick the first bone of each chain for a slot; its bones fill the slot in order, and bones past the slot's end move with its last bone. Bones you leave unmapped move rigidly with their parent. The preset sets how the chain swings."))
						]
						+ SVerticalBox::Slot().AutoHeight()
						[
							SAssignNew(SecondaryBox, SVerticalBox)
						],
						TAttribute<EVisibility>::CreateLambda([this]
						{
							return Draft.Class == EMantleCosmeticClass::Avatar && bSecondaryOffered ? EVisibility::Visible : EVisibility::Collapsed;
						}))
				]

				// Avatar socket offsets (the item's "sockets"): optional, per accessory socket and tool socket (#585).
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeSection(LOCTEXT("SocketsTitle", "Socket offsets"),
						SNew(SExpandableArea)
						.InitiallyCollapsed(true)
						.AreaTitle(LOCTEXT("SocketsArea", "Move where accessories and held tools attach (optional)"))
						.BodyContent()
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
							[
								SNew(STextBlock)
								.AutoWrapText(true)
								.ColorAndOpacity(FSlateColor::UseSubduedForeground())
								.Text(LOCTEXT("SocketsHelp", "Only for bodies whose proportions put a hat, glasses or backpack in the wrong place. Tick a part to replace the socket's own; the values start at the body's socket. Location is in cm from the bone (at most 50 on each axis), rotation in degrees (pitch, yaw, roll; at most 180), scale 0.5 to 2. Values outside these are refused by the check, not clamped."))
							]
							+ SVerticalBox::Slot().AutoHeight()
							[
								SAssignNew(SocketsBox, SVerticalBox)
							]
						],
						AvatarOnly)
				]

				// Accessory offset.
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeSection(LOCTEXT("AttachTitle", "Attachment"),
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SCheckBox)
							.IsChecked_Lambda([this] { return Draft.bAttach ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
							.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Draft.bAttach = State == ECheckBoxState::Checked; bAccessoryPlacementInitialized = true; Package.Reset(); SaveAccessoryPlacement(); RefreshAccessoryPreview(); })
							[
								SNew(STextBlock).Text(LOCTEXT("AttachToggle", "Offset it from the socket"))
							]
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
						[
							MakeAttachRow(LOCTEXT("AttachLocation", "Location (cm)"), Draft.AttachLocation, -50.0f, 50.0f)
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
						[
							MakeAttachRow(LOCTEXT("AttachRotation", "Rotation (degrees)"), Draft.AttachRotation, -180.0f, 180.0f)
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
						[
							MakeAttachRow(LOCTEXT("AttachScale", "Scale"), Draft.AttachScale, 0.5f, 2.0f)
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
						[
							SNew(SButton)
							.IsEnabled_Lambda([this] { return Draft.Source.IsValid() && (Draft.AuthoringBody.IsValid()
								|| (PreviewViewport.IsValid() && PreviewViewport->GetShownBody())); })
							.OnClicked(this, &SMantleCosmeticPanel::OnAlignAccessoryAxes)
							[ SNew(STextBlock).Text(LOCTEXT("AlignAccessoryAxes", "Align to avatar axes")) ]
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
						[
							SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground())
							.Text(LOCTEXT("AccessoryAxesNote", "Unreal units are centimetres; standard mesh axes use Z up. A new accessory aligns to the chosen avatar's neutral anchor. Keep the mesh pivot at its intended attachment point, then adjust placement and preview. Saved corrections are preserved."))
						],
						AccessoryOnly)
				]

				// Listing: name and description. There is no price field; nothing about prices is shown or sent.
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeSection(LOCTEXT("ListingTitle", "Listing"),
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SEditableTextBox)
							.HintText(LOCTEXT("NameHint", "Name (players see it in the catalog)"))
							.Text_Lambda([this] { return FText::FromString(Draft.Name); })
							.OnTextChanged_Lambda([this](const FText& Text) { Draft.Name = Text.ToString(); Package.Reset(); })
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
						[
							SNew(SBox).MinDesiredHeight(60.0f)
							[
								SNew(SMultiLineEditableTextBox)
								.HintText(LOCTEXT("DescriptionHint", "Description (optional)"))
								.AutoWrapText(true)
								.Text_Lambda([this] { return FText::FromString(Draft.Description); })
								.OnTextChanged_Lambda([this](const FText& Text) { Draft.Description = Text.ToString(); Package.Reset(); })
							]
						])
				]

				// Who may use the item (#614). Public is preselected and needs the rights declaration;
				// Private is the quieter alternative and needs none.
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeSection(LOCTEXT("VisibilityTitle", "Who can use it"),
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(SCheckBox)
							.Style(&FAppStyle::Get().GetWidgetStyle<FCheckBoxStyle>("RadioButton"))
							.IsChecked_Lambda([this] { return Draft.bPublic ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
							.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { if (State == ECheckBoxState::Checked) { Draft.bPublic = true; Package.Reset(); } })
							[
								SNew(STextBlock).Text(LOCTEXT("VisibilityPublic", "Public: listed in the catalog for every player"))
							]
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
						[
							SNew(SCheckBox)
							.Style(&FAppStyle::Get().GetWidgetStyle<FCheckBoxStyle>("RadioButton"))
							.IsChecked_Lambda([this] { return Draft.bPublic ? ECheckBoxState::Unchecked : ECheckBoxState::Checked; })
							.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { if (State == ECheckBoxState::Checked) { Draft.bPublic = false; Package.Reset(); } })
							[
								SNew(STextBlock)
								.Font(FAppStyle::GetFontStyle("SmallFont"))
								.ColorAndOpacity(FSlateColor::UseSubduedForeground())
								.Text(LOCTEXT("VisibilityPrivate", "Private: only you can use it"))
							]
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
						[
							SNew(SCheckBox)
							.Visibility_Lambda([this] { return Draft.bPublic ? EVisibility::Visible : EVisibility::Collapsed; })
							.IsChecked_Lambda([this] { return Draft.bRightsDeclared ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
							.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Draft.bRightsDeclared = State == ECheckBoxState::Checked; Package.Reset(); })
							[
								SNew(STextBlock)
								.AutoWrapText(true)
								.Text(LOCTEXT("RightsDeclaration", "I own this content or its licence permits public redistribution on Mantle"))
							]
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
						[
							SNew(STextBlock)
							.AutoWrapText(true)
							.Visibility_Lambda([this] { return Draft.bPublic && !Draft.bRightsDeclared ? EVisibility::Visible : EVisibility::Collapsed; })
							.ColorAndOpacity(MantleCosmeticPanelPrivate::SeverityColour(EMantleCosmeticSeverity::Warning))
							.Text(LOCTEXT("RightsDeclarationNeeded", "Tick the declaration to publish publicly, or choose Private."))
						])
				]

				// The preview: the fit (3D clothing), or the avatar posed by its retargeter (#283).
				+ SVerticalBox::Slot().AutoHeight()
				[
					MakeSection(LOCTEXT("PreviewTitle", "Preview"), MakePreviewSection(), Previewable)
				]

				// #124: the thumbnail, the same section in every uploader.
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
				[
					// The section carries its own heading, so it is not wrapped in MakeSection's.
					SNew(SBox).Visibility(Thumbnailable)
					[
						MakeThumbnailSection()
					]
				]

				// The one button.
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 6)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
					[
						SNew(SButton)
						.ButtonStyle(&FAppStyle::Get(), "PrimaryButton")
						.IsEnabled_Lambda([this] { return !IsRunning() && ContractError.IsEmpty() && (!Draft.bPublic || Draft.bRightsDeclared); })
						.Text(LOCTEXT("Upload", "Upload to Mantle"))
						.ToolTipText(LOCTEXT("UploadTooltip", "Check, export, upload, and publish. Uploading again after a failure continues where it stopped."))
						.OnClicked(this, &SMantleCosmeticPanel::OnUpload)
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
					[
						SNew(SButton)
						.IsEnabled_Lambda([this] { return !IsRunning() && ContractError.IsEmpty(); })
						.Text(LOCTEXT("Check", "Check only"))
						.ToolTipText(LOCTEXT("CheckTooltip", "Run every check and export without uploading anything."))
						.OnClicked(this, &SMantleCosmeticPanel::OnCheck)
					]
					// Chunk 9: walk around as the avatar in the Mantle app before uploading it.
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
					[
						SNew(SButton)
						.Visibility_Lambda([this] { return Draft.Class == EMantleCosmeticClass::Avatar ? EVisibility::Visible : EVisibility::Collapsed; })
						.IsEnabled_Lambda([this] { return !IsRunning() && ContractError.IsEmpty() && !FMantleTestLocally::Get().IsBusy(); })
						.Text(LOCTEXT("TestLocally", "Test locally"))
						.ToolTipText(LOCTEXT("TestLocallyTooltip", "Check, then open the Mantle app offline with you playing as this avatar. Nothing is uploaded: the files are prepared on this computer."))
						.OnClicked(this, &SMantleCosmeticPanel::OnTestLocally)
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Visibility_Lambda([this] { return IsRunning() ? EVisibility::Visible : EVisibility::Collapsed; })
						.Text(LOCTEXT("Cancel", "Cancel"))
						.OnClicked(this, &SMantleCosmeticPanel::OnCancel)
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
				[
					SNew(SProgressBar)
					.Visibility_Lambda([this] { return Session.IsValid() ? EVisibility::Visible : EVisibility::Collapsed; })
					.Percent_Lambda([this] { return TOptional<float>(Session.IsValid() ? Session->GetProgress() : 0.0f); })
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text(this, &SMantleCosmeticPanel::GetStatusText)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Visibility_Lambda([this]
					{
						return Draft.Class == EMantleCosmeticClass::Avatar && !TestLocallyResult.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.ColorAndOpacity_Lambda([this]
					{
						return bTestLocallyError ? FSlateColor(MantleCosmeticPanelPrivate::SeverityColour(EMantleCosmeticSeverity::Error)) : FSlateColor::UseForeground();
					})
					.Text_Lambda([this] { return TestLocallyResult; })
				]
				// #581: an avatar's size in play, once a check has measured it (the cook's standing height).
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Visibility_Lambda([this]
					{
						return Package.IsSet() && Package->AvatarStandingHeightCm.IsSet() ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.Text_Lambda([this]
					{
						const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
						if (!Contract || !Package.IsSet() || !Package->AvatarStandingHeightCm.IsSet())
						{
							return FText::GetEmpty();
						}
						return FText::FromString(FMantleCosmeticChecks::DescribeAvatarPlayingHeight(Package->AvatarStandingHeightCm.GetValue(), *Contract));
					})
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SAssignNew(IssuesBox, SVerticalBox)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
				[
					SNew(SExpandableArea)
					.InitiallyCollapsed(true)
					.AreaTitle(LOCTEXT("FilesTitle", "Files and details"))
					.BodyContent()
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.Text(this, &SMantleCosmeticPanel::GetFilesText)
					]
				]
			]
		]
	];

	SetClass(EMantleCosmeticClass::Garment);
}

TSharedRef<SWidget> SMantleCosmeticPanel::MakeSection(const FText& Title, const TSharedRef<SWidget>& Content, TAttribute<EVisibility> SectionVisibility)
{
	return SNew(SBox)
		.Visibility(SectionVisibility)
		.Padding(FMargin(0, 0, 0, 10))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("NormalFontBold"))
				.Text(Title)
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				Content
			]
		];
}

TSharedRef<SWidget> SMantleCosmeticPanel::MakeAttachRow(const FText& Label, FVector3f& Value, float Min, float Max)
{
	FVector3f* Target = &Value;
	return MakeVectorRow(SNew(STextBlock).Text(Label), [Target] { return Target; }, Min, Max,
		TAttribute<bool>::CreateLambda([this] { return Draft.bAttach; }), true);
}

TSharedRef<SWidget> SMantleCosmeticPanel::MakeVectorRow(const TSharedRef<SWidget>& LabelWidget, TFunction<FVector3f*()> Target, float Min, float Max,
	TAttribute<bool> Enabled, bool bClamp)
{
	const TOptional<float> HardMin = bClamp ? TOptional<float>(Min) : TOptional<float>();
	const TOptional<float> HardMax = bClamp ? TOptional<float>(Max) : TOptional<float>();
	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(0.34f).VAlign(VAlign_Center)
		[
			LabelWidget
		];
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		Row->AddSlot().FillWidth(0.22f).Padding(4, 0, 0, 0)
		[
			SNew(SNumericEntryBox<float>)
			.IsEnabled(Enabled)
			.MinValue(HardMin)
			.MaxValue(HardMax)
			.MinSliderValue(Min)
			.MaxSliderValue(Max)
			.Value_Lambda([Target, Axis]
			{
				const FVector3f* Value = Target();
				return Value ? TOptional<float>((*Value)[Axis]) : TOptional<float>();
			})
			.OnValueChanged_Lambda([this, Target, Axis, Min, Max, bClamp](float NewValue)
			{
				if (FVector3f* Value = Target())
				{
					(*Value)[Axis] = bClamp ? FMath::Clamp(NewValue, Min, Max) : NewValue;
					if (Draft.Class == EMantleCosmeticClass::Accessory) bAccessoryPlacementInitialized = true;
					Package.Reset();
					RefreshAccessoryPreview();
				}
			})
			.OnValueCommitted_Lambda([this, Target, Axis, Min, Max, bClamp](float NewValue, ETextCommit::Type)
			{
				if (FVector3f* Value = Target())
				{
					(*Value)[Axis] = bClamp ? FMath::Clamp(NewValue, Min, Max) : NewValue;
					if (Draft.Class == EMantleCosmeticClass::Accessory) bAccessoryPlacementInitialized = true;
					Package.Reset();
					SaveAccessoryPlacement();
					RefreshAccessoryPreview();
				}
			})
		];
	}
	return Row;
}

TSharedRef<SWidget> SMantleCosmeticPanel::MakeSourcePicker()
{
	const UClass* Allowed = nullptr;
	switch (Draft.Class)
	{
	case EMantleCosmeticClass::Garment:
		Allowed = UObject::StaticClass(); break;
	// The one door: any character (IsAllowedSource), which Convert takes (PickAvatar).
	case EMantleCosmeticClass::Avatar: Allowed = UObject::StaticClass(); break;
	case EMantleCosmeticClass::Accessory: Allowed = UStaticMesh::StaticClass(); break;
	case EMantleCosmeticClass::Emote: Allowed = UAnimSequence::StaticClass(); break;
	case EMantleCosmeticClass::Overlay: Allowed = UTexture2D::StaticClass(); break;
	case EMantleCosmeticClass::Hair: Allowed = UObject::StaticClass(); break;
	}
	return SNew(SObjectPropertyEntryBox)
		.AllowedClass(Allowed)
		.AllowClear(true)
		.DisplayThumbnail(false)
		.OnShouldFilterAsset_Lambda([this](const FAssetData& Asset) { return !IsAllowedSource(Asset); })
		.ObjectPath_Lambda([this]
		{
			// Avatar shows the character picked, not the converted body Use put in the draft.
			const UObject* Shown = Draft.Class == EMantleCosmeticClass::Avatar ? GetConvertSelection() : Draft.Source.Get();
			return Shown ? Shown->GetPathName() : FString();
		})
		.OnObjectChanged_Lambda([this](const FAssetData& Asset)
		{
			if (Draft.Class == EMantleCosmeticClass::Avatar)
			{
				PickAvatar(Asset.GetAsset());
			}
			else
			{
				SetSource(Asset.GetAsset());
			}
		});
}

bool SMantleCosmeticPanel::IsAllowedSource(const FAssetData& Asset) const
{
	if (Draft.Class == EMantleCosmeticClass::Avatar)
	{
		// #625: a MetaHuman Creator character (an Epic preset or the creator's own), built as it is. #776: a static mesh, a
		// model with no skeleton (fitted to a MetaHuman body). A Blueprint: a MetaHuman Creator assembly's.
		const FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();
		return ClassName.Equals(TEXT("SkeletalMesh"), ESearchCase::CaseSensitive) || ClassName.Equals(TEXT("StaticMesh"), ESearchCase::CaseSensitive)
			|| ClassName.Equals(TEXT("MetaHumanCharacter"), ESearchCase::CaseSensitive) || ClassName.Equals(TEXT("Blueprint"), ESearchCase::CaseSensitive);
	}
	if (Draft.Class == EMantleCosmeticClass::Garment)
	{
		// Chunk 8: a Chaos cloth asset too, as the agent toolset takes it (wrapped as an Outfit for its sizes).
		const UClass* AssetClass = FindObject<UClass>(Asset.AssetClassPath);
		return Asset.AssetClassPath == USkeletalMesh::StaticClass()->GetClassPathName()
			|| Asset.AssetClassPath == FTopLevelAssetPath(TEXT("/Script/ChaosOutfitAssetEngine"), TEXT("ChaosOutfitAsset"))
			|| (AssetClass && AssetClass->IsChildOf(UChaosClothAssetBase::StaticClass()));
	}
	if (Draft.Class != EMantleCosmeticClass::Hair)
	{
		return true;
	}
	const FString ClassName = Asset.AssetClassPath.GetAssetName().ToString();
	return ClassName == TEXT("GroomAsset") || ClassName == TEXT("StaticMesh") || ClassName == TEXT("SkeletalMesh");
}

bool SMantleCosmeticPanel::IsEmoteBodyCandidate(const FAssetData& Asset) const
{
	const UAnimSequence* Sequence = Cast<UAnimSequence>(Draft.Source.Get());
	const USkeleton* Skeleton = Sequence ? Sequence->GetSkeleton() : nullptr;
	if (!Skeleton)
	{
		return true;
	}
	// The mesh's Skeleton tag, as FMantleEmoteRetarget::FindSourceMesh reads it: no mesh needs loading to be listed.
	FString SkeletonTag;
	Asset.GetTagValue(TEXT("Skeleton"), SkeletonTag);
	return FPackageName::ExportTextPathToObjectPath(SkeletonTag).Equals(Skeleton->GetPathName(), ESearchCase::CaseSensitive);
}

void SMantleCosmeticPanel::SetClass(EMantleCosmeticClass Class)
{
	SaveAccessoryPlacement();
	Draft.Class = Class;
	Draft.ClothingColors.Reset(); ClothingPreviewColors.Reset(); ClothingMaterialNames.Reset();
	Draft.ClothingColorChoices.Reset(); DetectedClothingColors.Reset(); bClothingAreasKnown = false; RefreshClothingColors();
	bAccessoryPlacementInitialized = false;
	// Reset any previous explicit target when changing the item class.
	Draft.PublishIntoItemId.Reset();
	if (PublishAsCombo.IsValid() && PublishAsOptions.Num() > 0)
	{
		PublishAsCombo->SetSelectedItem(PublishAsOptions[0]);
	}
	Draft.Source.Reset();
	Draft.OverlayMaskFile.Reset();
	Draft.OverlayNormal.Reset();
	Draft.OverlayNormalFile.Reset();
	Draft.bOverlayPlaced = false;
	Draft.OverlayPicture.Reset();
	Draft.OverlayPictureFile.Reset();
	Draft.OverlayPlaceMesh.Reset();
	Draft.OverlayPlacement = FMantleOverlayPlacement();
	OverlayPlaceStatus.Reset();
	bOverlayPlaceStarted = false;
	OverlayPreviewStatus.Reset();
	bOverlayPreviewFailed = false;
	Draft.OutfitSourceSize.Reset();
	Draft.OutfitAuthoringCharacter.Reset();
	Draft.OutfitTargets.Reset();
	Draft.FitBodies.Reset();
	Draft.FitBodiesStatus.Reset();
	ParametricSourceBodyHead.Reset();
	AuthoredOutfitKeepAlive.Reset();
	OutfitAuthoringStatus.Reset();
	RefreshOutfitTargets();
	OutfitPreviewKeepAlive.Reset();
	Draft.CardsEntry = INDEX_NONE;
	Draft.SourceSection = INDEX_NONE;
	Draft.SectionRoles.Reset();
	Draft.HairColor.Reset();
	Draft.bPlaceHairByHand = false;
	HairColorText.Reset();
	Package.Reset();
	RefreshJaw();
	RefreshSecondary();
	// #283: a garment's fit and an avatar's pose share the viewport: another class starts it empty.
	PreviewInputError.Reset();
	AvatarPreviewStatus.Reset();
	bAvatarPreviewFailed = false;
	if (PreviewSession.IsValid())
	{
		PreviewSession->Cancel();
	}
	RefreshSlots();
	RebuildSourcePicker();
	RefreshDetails();
}

void SMantleCosmeticPanel::RefreshSlots()
{
	SlotOptions.Reset();
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	if (Contract)
	{
		for (const FString& Slot : CosmeticSlotOptions(*Contract, Draft.Class))
		{
			SlotOptions.Add(MakeShared<FString>(Slot));
		}
	}
	Draft.Slot = SlotOptions.Num() > 0 ? *SlotOptions[0] : FString();
	if (SlotCombo.IsValid())
	{
		SlotCombo->RefreshOptions();
		SlotCombo->SetSelectedItem(SlotOptions.Num() > 0 ? SlotOptions[0] : nullptr);
	}
}

void SMantleCosmeticPanel::RebuildSourcePicker()
{
	if (SourcePickerBox.IsValid())
	{
		SourcePickerBox->SetContent(MakeSourcePicker());
	}
}

void SMantleCosmeticPanel::SetSlot(const FString& Slot)
{
	SaveAccessoryPlacement();
	Draft.Slot = Slot;
	if (Draft.Class == EMantleCosmeticClass::Overlay && !FMantleOverlayChecks::IsScarSlot(Slot))
	{
		// Tattoos take no normal map; a scar's stays picked only while a scar slot is.
		Draft.OverlayNormal.Reset();
		Draft.OverlayNormalFile.Reset();
	}
	bAccessoryPlacementInitialized = FMantleAccessorySettings::Load(Draft, AccessorySettingsFile());
	LoadThumbnailChoice();
	RefreshAccessoryPreview();
	// #483: the sections the new slot takes.
	RefreshOutfitSections();
	Package.Reset();
}

void SMantleCosmeticPanel::SetAuthoringBody(USkeletalMesh* Body)
{
	Draft.AuthoringBody = Body;
	Package.Reset();
	if (Draft.Class == EMantleCosmeticClass::Accessory)
	{
		// A ready stage belongs to its old wearer. Keep explicit/saved placement, but stop using
		// that wearer's body for Align/Check until the replacement body has been previewed.
		PreviewInputError.Reset();
		if (PreviewSession.IsValid()) PreviewSession->Cancel();
	}
	if (Draft.Class == EMantleCosmeticClass::Garment && FMantleParametricOutfit::IsSource(Draft.Source.Get()))
	{
		Draft.AuthoringFace.Reset();
		if (PreviewSession.IsValid()) PreviewSession->Cancel();
		OutfitPreviewKeepAlive.Reset();
		RefreshOutfitAuthoringDefaults();
	}
}

void SMantleCosmeticPanel::SetSource(UObject* Source)
{
	if (PreviewSession.IsValid()) PreviewSession->Cancel();
	OutfitPreviewKeepAlive.Reset();
	Draft.OutfitSourceSize.Reset();
	SaveAccessoryPlacement();
	Draft.Source = Source;
	Draft.bPlaceHairByHand = false;
	// The rights declaration is about this content; a new source needs its own tick (#614).
	Draft.bRightsDeclared = false;
	Draft.ClothingColors.Reset(); ClothingPreviewColors.Reset();
	Draft.ClothingColorChoices.Reset(); DetectedClothingColors.Reset(); bClothingAreasKnown = false;
	ClothingMaterialNames = FMantleCosmeticExport::SectionMaterialNames(Cast<USkeletalMesh>(Source), 0);
	RefreshClothingColors();
	bAccessoryPlacementInitialized = FMantleAccessorySettings::Load(Draft, AccessorySettingsFile());
	LoadThumbnailChoice();
	Draft.CardsEntry = INDEX_NONE;
	Draft.SourceSection = INDEX_NONE;
	Draft.SectionRoles.Reset();
	Draft.OutfitTargets.Reset();
	Draft.FitBodies.Reset();
	// "Made for" is about this piece; a new source starts on Automatic.
	Draft.FitSex = EMantleFitSex::Auto;
	// Clothing from an Outfit starts with Mantle's standard fit bodies, so it uploads in sizes without a creator step.
	FMantleEditorActions::ApplyCosmeticFitTargetDefaults(Draft, FMantleAvatarContract::Get());
	RefreshOutfitTargets();
	if (FMantleParametricOutfit::IsSource(Source)) Draft.KeepSlots.Reset();
	Package.Reset();
	// #584: an "Animated on" body stays only while it is on the chosen animation's skeleton.
	const UAnimSequence* Sequence = Cast<UAnimSequence>(Source);
	if (Draft.EmoteBody.IsValid() && (!Sequence || Draft.EmoteBody->GetSkeleton() != Sequence->GetSkeleton()))
	{
		Draft.EmoteBody.Reset();
	}

	// The authoring body and the name, shared with the agent toolset (FMantleEditorActions::MakeCosmeticDraft).
	FMantleEditorActions::ApplyCosmeticSourceDefaults(Draft, FMantleAvatarContract::Get());
	if (FMantleParametricOutfit::IsSource(Source)) RefreshOutfitAuthoringDefaults();
	RefreshJaw();
	RefreshSecondary();
	RefreshDetails();
}

FMantleSecondaryMapping* SMantleCosmeticPanel::FindSecondaryMapping(const FString& ChainId)
{
	return Draft.SecondaryMotion.FindByPredicate([&ChainId](const FMantleSecondaryMapping& Mapping)
	{
		return Mapping.ChainId.Equals(ChainId, ESearchCase::CaseSensitive);
	});
}

void SMantleCosmeticPanel::RefreshSecondary()
{
	// The old rows' combo boxes read the option lists: gone before the lists change.
	if (SecondaryBox.IsValid())
	{
		SecondaryBox->ClearChildren();
	}
	TArray<FName> Candidates;
	bSecondaryOffered = FMantleEditorActions::ApplyCosmeticSecondaryDefaults(Draft, FMantleAvatarContract::Get(), Candidates);
	SecondaryBoneOptions.Reset();
	SecondaryBoneOptions.Add(MakeShared<FString>());
	for (const FName Candidate : Candidates)
	{
		SecondaryBoneOptions.Add(MakeShared<FString>(Candidate.ToString()));
	}
	SecondaryPresetOptions.Reset();
	for (const EMantleSecondaryPreset Preset :
		{ EMantleSecondaryPreset::Hair, EMantleSecondaryPreset::Chest, EMantleSecondaryPreset::Cloth, EMantleSecondaryPreset::Tail })
	{
		SecondaryPresetOptions.Add(MakeShared<FString>(MantleSecondaryMotion::PresetId(Preset)));
	}
	if (!SecondaryBox.IsValid() || !bSecondaryOffered)
	{
		return;
	}
	for (const FMantleSecondaryChainDef& Chain : MantleSecondaryMotion::Chains())
	{
		const FString ChainId = Chain.Id;
		const int32 Slots = Chain.Bones.Num();
		const EMantleSecondaryPreset DefaultPreset = Chain.DefaultPreset;
		auto PresetText = [](const TSharedPtr<FString>& Item)
		{
			EMantleSecondaryPreset Preset = EMantleSecondaryPreset::Hair;
			return Item.IsValid() && MantleSecondaryMotion::PresetFromId(*Item, Preset)
				? MantleSecondaryMotion::PresetDisplayName(Preset) : FText::GetEmpty();
		};
		SecondaryBox->AddSlot().AutoHeight().Padding(0, 2)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(SBox).WidthOverride(90.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(ChainId))
					.ToolTipText(FText::Format(LOCTEXT("SecondarySlotTip", "{0}: up to {1} bones"), FText::FromString(ChainId), FText::AsNumber(Slots)))
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 0, 8, 0)
			[
				SNew(SComboBox<TSharedPtr<FString>>)
				.OptionsSource(&SecondaryBoneOptions)
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
				{
					return SNew(STextBlock).Text(Item.IsValid() && !Item->IsEmpty() ? FText::FromString(*Item) : LOCTEXT("SecondaryNone", "None"));
				})
				.OnSelectionChanged_Lambda([this, ChainId, DefaultPreset](TSharedPtr<FString> Item, ESelectInfo::Type)
				{
					const FName Bone = Item.IsValid() && !Item->IsEmpty() ? FName(**Item) : NAME_None;
					if (Bone.IsNone())
					{
						Draft.SecondaryMotion.RemoveAll([&ChainId](const FMantleSecondaryMapping& Mapping)
						{
							return Mapping.ChainId.Equals(ChainId, ESearchCase::CaseSensitive);
						});
					}
					else if (FMantleSecondaryMapping* Mapping = FindSecondaryMapping(ChainId))
					{
						Mapping->StartBone = Bone;
					}
					else
					{
						FMantleSecondaryMapping& Added = Draft.SecondaryMotion.AddDefaulted_GetRef();
						Added.ChainId = ChainId;
						Added.StartBone = Bone;
						Added.Preset = DefaultPreset;
					}
					Package.Reset();
				})
				[
					SNew(STextBlock).Text_Lambda([this, ChainId]
					{
						const FMantleSecondaryMapping* Mapping = FindSecondaryMapping(ChainId);
						return Mapping ? FText::FromName(Mapping->StartBone) : LOCTEXT("SecondaryNone", "None");
					})
				]
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SBox).WidthOverride(90.0f)
				[
					SNew(SComboBox<TSharedPtr<FString>>)
					.OptionsSource(&SecondaryPresetOptions)
					.IsEnabled_Lambda([this, ChainId] { return FindSecondaryMapping(ChainId) != nullptr; })
					.OnGenerateWidget_Lambda([PresetText](TSharedPtr<FString> Item)
					{
						return SNew(STextBlock).Text(PresetText(Item));
					})
					.OnSelectionChanged_Lambda([this, ChainId](TSharedPtr<FString> Item, ESelectInfo::Type)
					{
						EMantleSecondaryPreset Preset = EMantleSecondaryPreset::Hair;
						FMantleSecondaryMapping* Mapping = FindSecondaryMapping(ChainId);
						if (Mapping && Item.IsValid() && MantleSecondaryMotion::PresetFromId(*Item, Preset))
						{
							Mapping->Preset = Preset;
							Package.Reset();
						}
					})
					[
						SNew(STextBlock).Text_Lambda([this, ChainId, DefaultPreset]
						{
							const FMantleSecondaryMapping* Mapping = FindSecondaryMapping(ChainId);
							return MantleSecondaryMotion::PresetDisplayName(Mapping ? Mapping->Preset : DefaultPreset);
						})
					]
				]
			]
		];
	}
}

void SMantleCosmeticPanel::RefreshJaw()
{
	TArray<FName> Candidates;
	bJawSlotOffered = FMantleEditorActions::ApplyCosmeticJawDefaults(Draft, FMantleAvatarContract::Get(), Candidates);
	JawOptions.Reset();
	JawOptions.Add(MakeShared<FString>());
	TSharedPtr<FString> Selected = JawOptions[0];
	for (const FName Candidate : Candidates)
	{
		TSharedPtr<FString> Option = MakeShared<FString>(Candidate.ToString());
		if (!Draft.JawBone.IsNone() && Candidate == Draft.JawBone)
		{
			Selected = Option;
		}
		JawOptions.Add(Option);
	}
	if (JawCombo.IsValid())
	{
		JawCombo->RefreshOptions();
		JawCombo->SetSelectedItem(Selected);
	}
}

void SMantleCosmeticPanel::RefreshDetails()
{
	RefreshOutfitSizes();
	bCanGenerateHairCards = FMantleHairGeneration::CanGenerate(Draft.Source.Get(), HairGenerationStatus);
	CardsOptions.Reset();
	SectionOptions.Reset();
	UObject* Source = Draft.Source.Get();

	// Hair's cards entry and lash section; the lists below only show the choices.
	FMantleEditorActions::ApplyCosmeticSectionDefaults(Draft);
	if (Draft.Class == EMantleCosmeticClass::Hair && FMantleCosmeticExport::IsGroomAsset(Source))
	{
		for (const FMantleGroomCardsEntry& Entry : FMantleCosmeticExport::ReadGroomCards(Source))
		{
			CardsOptions.Add(MakeShared<FString>(Entry.Describe()));
		}
	}
	if (Draft.Class == EMantleCosmeticClass::Hair)
	{
		if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Source))
		{
			// Render sections of LOD 0, named by their material slot.
			for (const FString& SectionName : FMantleCosmeticExport::RenderSectionNames(Mesh, 0))
			{
				SectionOptions.Add(MakeShared<FString>(SectionName));
			}
			SectionOptions.Add(MakeShared<FString>(TEXT("Whole hair mesh (all sections)")));
		}
	}
	if (CardsCombo.IsValid())
	{
		CardsCombo->RefreshOptions();
	}
	if (SectionCombo.IsValid())
	{
		SectionCombo->RefreshOptions();
	}

	RefreshSockets();
	RefreshOutfitSections();

	// Avatar section roles, pre-filled from MetaHuman's slot names.
	if (RolesBox.IsValid())
	{
		RolesBox->ClearChildren();
		const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
		FMantleEditorActions::ApplyCosmeticRoleDefaults(Draft, Contract);
		USkeletalMesh* Body = Cast<USkeletalMesh>(Source);
		if (Draft.Class == EMantleCosmeticClass::Avatar && Body && Contract)
		{
			const TArray<FString> Names = FMantleCosmeticExport::SectionMaterialNames(Body, FMantleCosmeticExport::AvatarBodyLOD(Body, *Contract));
			for (int32 Index = 0; Index < Names.Num(); ++Index)
			{
				RolesBox->AddSlot().AutoHeight().Padding(0, 2)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
					[
						SNew(STextBlock).Text(FText::FromString(Names[Index]))
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SComboBox<TSharedPtr<FString>>)
						.OptionsSource(&RoleOptions)
						.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item)
						{
							return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString()));
						})
						.OnSelectionChanged_Lambda([this, Index](TSharedPtr<FString> Item, ESelectInfo::Type)
						{
							if (Item.IsValid() && Draft.SectionRoles.IsValidIndex(Index))
							{
								Draft.SectionRoles[Index] = *Item;
								Package.Reset();
							}
						})
						[
							SNew(STextBlock).Text_Lambda([this, Index]
							{
								return FText::FromString(Draft.SectionRoles.IsValidIndex(Index) ? Draft.SectionRoles[Index] : FString(TEXT("other")));
							})
						]
					]
				];
			}
		}
	}
}

void SMantleCosmeticPanel::RefreshSockets()
{
	FMantleEditorActions::ApplyCosmeticSocketDefaults(Draft, FMantleAvatarContract::Get());
	if (!SocketsBox.IsValid())
	{
		return;
	}
	SocketsBox->ClearChildren();

	struct FPart
	{
		FText Label;
		bool FMantleCosmeticSocketOverride::* Flag;
		FVector3f FMantleCosmeticSocketOverride::* Value;
		float Min;
		float Max;
	};
	const FPart Parts[] = {
		{ LOCTEXT("SocketLocation", "Location (cm)"), &FMantleCosmeticSocketOverride::bLocation, &FMantleCosmeticSocketOverride::Location,
			-FMantleCosmeticChecks::MaxTransformOffsetCm, FMantleCosmeticChecks::MaxTransformOffsetCm },
		{ LOCTEXT("SocketRotation", "Rotation (degrees)"), &FMantleCosmeticSocketOverride::bRotation, &FMantleCosmeticSocketOverride::Rotation,
			-FMantleCosmeticChecks::MaxTransformDegrees, FMantleCosmeticChecks::MaxTransformDegrees },
		{ LOCTEXT("SocketScale", "Scale"), &FMantleCosmeticSocketOverride::bScale, &FMantleCosmeticSocketOverride::Scale,
			FMantleCosmeticChecks::MinTransformScale, FMantleCosmeticChecks::MaxTransformScale },
	};

	for (int32 Index = 0; Index < Draft.SocketOverrides.Num(); ++Index)
	{
		const FMantleCosmeticSocketOverride& Override = Draft.SocketOverrides[Index];
		SocketsBox->AddSlot().AutoHeight().Padding(0, 6, 0, 2)
		[
			SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("NormalFontBold"))
			.Text(Override.Bone.IsEmpty() ? FText::FromString(Override.Socket)
				: FText::Format(LOCTEXT("SocketOnBone", "{0} (on {1})"), FText::FromString(Override.Socket), FText::FromString(Override.Bone)))
		];
		// #585: a unit-scale tool socket (hand_rSocket) offers no scale row; the API refuses any scale but 1.
		const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
		const FMantleContractToolSocket* Tool = Contract ? Contract->FindToolSocket(Override.Socket) : nullptr;
		for (const FPart& Part : Parts)
		{
			if (Tool && Tool->bUnitScale && Part.Flag == &FMantleCosmeticSocketOverride::bScale)
			{
				continue;
			}
			bool FMantleCosmeticSocketOverride::* const Flag = Part.Flag;
			FVector3f FMantleCosmeticSocketOverride::* const Member = Part.Value;
			SocketsBox->AddSlot().AutoHeight().Padding(0, 2)
			[
				MakeVectorRow(
					SNew(SCheckBox)
					.IsChecked_Lambda([this, Index, Flag]
					{
						return Draft.SocketOverrides.IsValidIndex(Index) && Draft.SocketOverrides[Index].*Flag ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([this, Index, Flag](ECheckBoxState State)
					{
						if (Draft.SocketOverrides.IsValidIndex(Index))
						{
							Draft.SocketOverrides[Index].*Flag = State == ECheckBoxState::Checked;
							Package.Reset();
						}
					})
					[
						SNew(STextBlock).Text(Part.Label)
					],
					[this, Index, Member]() -> FVector3f*
					{
						return Draft.SocketOverrides.IsValidIndex(Index) ? &(Draft.SocketOverrides[Index].*Member) : nullptr;
					},
					Part.Min, Part.Max,
					TAttribute<bool>::CreateLambda([this, Index, Flag]
					{
						return Draft.SocketOverrides.IsValidIndex(Index) && Draft.SocketOverrides[Index].*Flag;
					}),
					// Not clamped: a value past the API's bounds is refused by Check with the number.
					false)
			];
		}
	}
}

void SMantleCosmeticPanel::RefreshOutfitSections()
{
	// The rows and their starting ticks, and Draft.KeepSlots from them (shared with the agent toolset).
	FMantleCosmeticOutfitRows Rows = FMantleEditorActions::ApplyCosmeticOutfitDefaults(Draft);
	OutfitSections = MoveTemp(Rows.Sections);
	OutfitSlots = MoveTemp(Rows.Slots);
	OutfitKeep = MoveTemp(Rows.Keep);
	OutfitPieces.Reset();
	for (const FMantleOutfitSection& Section : OutfitSections)
	{
		OutfitPieces.Add(FMantleOutfitSections::Classify(Section));
	}
	if (!OutfitBox.IsValid())
	{
		return;
	}
	OutfitBox->ClearChildren();
	if (OutfitSections.Num() == 0)
	{
		return;
	}
	OutfitBox->AddSlot().AutoHeight().Padding(0, 8, 0, 2)
	[
		SNew(STextBlock)
		.Font(FAppStyle::GetFontStyle("NormalFontBold"))
		.Text(LOCTEXT("OutfitTitle", "Sections"))
	];
	OutfitBox->AddSlot().AutoHeight().Padding(0, 0, 0, 4)
	[
		SNew(STextBlock)
		.AutoWrapText(true)
		.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		.Text(LOCTEXT("OutfitHelp", "The mesh's sections this item takes. A one-piece outfit uploads once per slot, each time with that piece's sections ticked."))
	];
	const FText SkinReason = LOCTEXT("OutfitSkin", "the wearer's own body shows through; garments never carry skin");
	for (int32 Index = 0; Index < OutfitSections.Num(); ++Index)
	{
		const bool bSkin = OutfitPieces.IsValidIndex(Index) && OutfitPieces[Index] == EMantleOutfitPiece::Skin;
		const FText Label = FText::Format(LOCTEXT("OutfitRow", "{0} ({1})"), FText::FromString(OutfitSections[Index].SlotName),
			FText::FromString(OutfitPieces.IsValidIndex(Index) ? FString(FMantleOutfitSections::PieceName(OutfitPieces[Index])) : FString()));
		OutfitBox->AddSlot().AutoHeight().Padding(0, 1)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SCheckBox)
				.IsEnabled(!bSkin)
				.ToolTipText(bSkin ? SkinReason : FText::GetEmpty())
				.IsChecked_Lambda([this, Index]
				{
					return OutfitKeep.IsValidIndex(Index) && OutfitKeep[Index] ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this, Index](ECheckBoxState State)
				{
					if (OutfitKeep.IsValidIndex(Index))
					{
						OutfitKeep[Index] = State == ECheckBoxState::Checked;
						ApplyOutfitKeep();
						Package.Reset();
					}
				})
				[
					SNew(STextBlock).Text(Label)
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(8, 0, 0, 0)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Visibility(bSkin ? EVisibility::Visible : EVisibility::Collapsed)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(SkinReason)
			]
		];
	}
}

void SMantleCosmeticPanel::PickClothingColor(int32 DeclarationIndex, int32 RegionIndex, bool bPreview)
{
	if (!Draft.ClothingColors.IsValidIndex(DeclarationIndex) || !Draft.ClothingColors[DeclarationIndex].Regions.IsValidIndex(RegionIndex)) return;
	const FString MaterialName = Draft.ClothingColors[DeclarationIndex].MaterialName;
	const FMantleClothingColorRegion Region = Draft.ClothingColors[DeclarationIndex].Regions[RegionIndex];
	const FMantleClothingColor* Existing = ClothingPreviewColors.FindByPredicate([&Region](const FMantleClothingColor& Color) { return Color.RegionId == Region.Id; });
	FColorPickerArgs Args; Args.ParentWidget = SharedThis(this); Args.bUseAlpha = false; Args.bClampValue = true; Args.bOnlyRefreshOnOk = true; Args.sRGBOverride = true;
	Args.InitialColor = FLinearColor::FromSRGBColor(bPreview && Existing ? Existing->Color : Region.DefaultColor);
	const TWeakPtr<SMantleCosmeticPanel> WeakPanel = SharedThis(this);
	Args.OnColorCommitted = FOnLinearColorValueChanged::CreateLambda([WeakPanel, MaterialName, Region, bPreview](FLinearColor Chosen)
	{
		const TSharedPtr<SMantleCosmeticPanel> Panel = WeakPanel.Pin();
		if (!Panel) return;
		FMantleClothingColorAuthoring* Declaration = Panel->Draft.ClothingColors.FindByPredicate([&MaterialName](const FMantleClothingColorAuthoring& Entry) { return Entry.MaterialName == MaterialName; });
		FMantleClothingColorRegion* Target = Declaration ? Declaration->Regions.FindByPredicate([&Region](const FMantleClothingColorRegion& Entry) { return Entry.Id == Region.Id; }) : nullptr;
		if (!Target) return;
		FColor Quantized = Chosen.ToFColor(true); Quantized.A = 255;
		if (bPreview)
		{
			FMantleClothingColor* Override = Panel->ClothingPreviewColors.FindByPredicate([&Region](const FMantleClothingColor& Entry) { return Entry.RegionId == Region.Id; });
			if (!Override) { Override = &Panel->ClothingPreviewColors.AddDefaulted_GetRef(); Override->RegionId = Region.Id; }
			Override->Color = Quantized;
		}
		else { Target->DefaultColor = Quantized; Panel->Package.Reset(); }
		Panel->RefreshClothingColors();
		if (bPreview) Panel->OnPreview();
	});
	OpenColorPicker(Args);
}

void SMantleCosmeticPanel::RefreshClothingColors()
{
	if (!ClothingColorsBox.IsValid()) return;
	ClothingColorsBox->ClearChildren();
	AddFoundClothingAreas();
	TSharedRef<SVerticalBox> Advanced = SNew(SVerticalBox);
	Advanced->AddSlot().AutoHeight().Padding(0, 4)
	[SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("DyeAuthoringHelp", "For experts: pick your own black-and-white area texture (red, green and blue each mark one area) and name each area. Anything you set up here replaces the areas Mantle found. Texture detail stays intact; very dark areas need a lighter version of the item's colour texture. Preview colours don't change what uploads."))];
	if (!ClothingMaterialNames.IsEmpty()) Advanced->AddSlot().AutoHeight().Padding(0, 4)
	[SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(TEXT("Material names: ") + FString::Join(ClothingMaterialNames, TEXT(", "))))];
	else Advanced->AddSlot().AutoHeight().Padding(0, 4)
	[SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("DyeMaterialHelp", "For a MetaHuman outfit, preview once to list its material names, then type one exactly below."))];
	for (int32 DeclarationIndex = 0; DeclarationIndex < Draft.ClothingColors.Num(); ++DeclarationIndex)
	{
		TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
		Rows->AddSlot().AutoHeight().Padding(0, 3)
		[SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1)[SNew(SEditableTextBox).HintText(LOCTEXT("DyeMaterialName", "Material name (from the list above)"))
				.Text(FText::FromString(Draft.ClothingColors[DeclarationIndex].MaterialName))
				.OnTextCommitted_Lambda([this, DeclarationIndex](const FText& Text, ETextCommit::Type)
				{ if (Draft.ClothingColors.IsValidIndex(DeclarationIndex)) { Draft.ClothingColors[DeclarationIndex].MaterialName = Text.ToString().TrimStartAndEnd(); Package.Reset(); } })]
			+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0)[SNew(SButton).Text(LOCTEXT("RemoveDyeMaterial", "Remove material"))
				.OnClicked_Lambda([this, DeclarationIndex] { if (Draft.ClothingColors.IsValidIndex(DeclarationIndex)) { Draft.ClothingColors.RemoveAt(DeclarationIndex); Package.Reset(); RefreshClothingColors(); } return FReply::Handled(); })]];
		auto TextureRow = [this, DeclarationIndex](bool bNeutral) -> TSharedRef<SWidget>
		{
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[SNew(STextBlock).Text(bNeutral ? LOCTEXT("DyeNeutral", "Lighter colour texture (optional)") : LOCTEXT("DyeMask", "Area texture"))]
				+ SHorizontalBox::Slot().FillWidth(1)[SNew(SObjectPropertyEntryBox).AllowedClass(UTexture2D::StaticClass())
					.ObjectPath_Lambda([this, DeclarationIndex, bNeutral]
					{ if (!Draft.ClothingColors.IsValidIndex(DeclarationIndex)) return FString(); const UTexture2D* Texture = bNeutral ? Draft.ClothingColors[DeclarationIndex].NeutralAlbedo.Get() : Draft.ClothingColors[DeclarationIndex].Mask.Get(); return Texture ? Texture->GetPathName() : FString(); })
					.OnObjectChanged_Lambda([this, DeclarationIndex, bNeutral](const FAssetData& Asset)
					{ if (Draft.ClothingColors.IsValidIndex(DeclarationIndex)) { auto& Target = bNeutral ? Draft.ClothingColors[DeclarationIndex].NeutralAlbedo : Draft.ClothingColors[DeclarationIndex].Mask; Target = Cast<UTexture2D>(Asset.GetAsset()); Package.Reset(); } })];
		};
		Rows->AddSlot().AutoHeight().Padding(0, 3)[TextureRow(false)];
		Rows->AddSlot().AutoHeight().Padding(0, 3)[TextureRow(true)];
		for (int32 RegionIndex = 0; RegionIndex < Draft.ClothingColors[DeclarationIndex].Regions.Num(); ++RegionIndex)
		{
			const FMantleClothingColorRegion& Region = Draft.ClothingColors[DeclarationIndex].Regions[RegionIndex];
			Rows->AddSlot().AutoHeight().Padding(0, 4)
			[SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1)[SNew(SEditableTextBox).HintText(LOCTEXT("DyeRegionId", "Short id (lowercase, never changes)"))
					.Text(FText::FromString(Region.Id)).OnTextCommitted_Lambda([this, DeclarationIndex, RegionIndex](const FText& Text, ETextCommit::Type)
					{ if (Draft.ClothingColors.IsValidIndex(DeclarationIndex) && Draft.ClothingColors[DeclarationIndex].Regions.IsValidIndex(RegionIndex)) { Draft.ClothingColors[DeclarationIndex].Regions[RegionIndex].Id = Text.ToString().TrimStartAndEnd(); Package.Reset(); ClothingPreviewColors.Reset(); } })]
				+ SHorizontalBox::Slot().FillWidth(1).Padding(6, 0)[SNew(SEditableTextBox).HintText(LOCTEXT("DyeRegionLabel", "Name players see"))
					.Text(FText::FromString(Region.Label)).OnTextCommitted_Lambda([this, DeclarationIndex, RegionIndex](const FText& Text, ETextCommit::Type)
					{ if (Draft.ClothingColors.IsValidIndex(DeclarationIndex) && Draft.ClothingColors[DeclarationIndex].Regions.IsValidIndex(RegionIndex)) { Draft.ClothingColors[DeclarationIndex].Regions[RegionIndex].Label = Text.ToString().TrimStartAndEnd(); Package.Reset(); } })]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0)[SNew(SButton).Text(FText::FromString(FString::Printf(TEXT("Marked in %s"), Region.Channel == 0 ? TEXT("red") : Region.Channel == 1 ? TEXT("green") : TEXT("blue"))))
					.OnClicked_Lambda([this, DeclarationIndex, RegionIndex] { if (Draft.ClothingColors.IsValidIndex(DeclarationIndex) && Draft.ClothingColors[DeclarationIndex].Regions.IsValidIndex(RegionIndex)) { auto& Target = Draft.ClothingColors[DeclarationIndex].Regions[RegionIndex]; Target.Channel = (Target.Channel + 1) % 3; Package.Reset(); RefreshClothingColors(); } return FReply::Handled(); })]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0)[SNew(SButton).Text(LOCTEXT("DyeSwatch", "Default swatch")).OnClicked_Lambda([this, DeclarationIndex, RegionIndex] { PickClothingColor(DeclarationIndex, RegionIndex, false); return FReply::Handled(); })]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0)[SNew(SButton).Text(LOCTEXT("DyePreview", "Preview color")).OnClicked_Lambda([this, DeclarationIndex, RegionIndex] { PickClothingColor(DeclarationIndex, RegionIndex, true); return FReply::Handled(); })]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0)[SNew(SButton).Text(LOCTEXT("DyeOriginal", "Original")).OnClicked_Lambda([this, DeclarationIndex, RegionIndex]
					{ if (Draft.ClothingColors.IsValidIndex(DeclarationIndex) && Draft.ClothingColors[DeclarationIndex].Regions.IsValidIndex(RegionIndex)) { const FString Id = Draft.ClothingColors[DeclarationIndex].Regions[RegionIndex].Id; ClothingPreviewColors.RemoveAll([&Id](const FMantleClothingColor& Entry) { return Entry.RegionId == Id; }); OnPreview(); } return FReply::Handled(); })]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0)[SNew(SButton).Text(LOCTEXT("DyeRemoveRegion", "Remove")).OnClicked_Lambda([this, DeclarationIndex, RegionIndex]
					{ if (Draft.ClothingColors.IsValidIndex(DeclarationIndex) && Draft.ClothingColors[DeclarationIndex].Regions.IsValidIndex(RegionIndex)) { Draft.ClothingColors[DeclarationIndex].Regions.RemoveAt(RegionIndex); Package.Reset(); ClothingPreviewColors.Reset(); RefreshClothingColors(); } return FReply::Handled(); })]];
		}
		Rows->AddSlot().AutoHeight().Padding(0, 4)[SNew(SButton).Text(LOCTEXT("DyeAddRegion", "Add area"))
			.IsEnabled(Draft.ClothingColors[DeclarationIndex].Regions.Num() < MantleClothingColor::MaxRegionsPerSection)
			.OnClicked_Lambda([this, DeclarationIndex]
			{
				if (Draft.ClothingColors.IsValidIndex(DeclarationIndex) && Draft.ClothingColors[DeclarationIndex].Regions.Num() < MantleClothingColor::MaxRegionsPerSection)
				{
					auto& Regions = Draft.ClothingColors[DeclarationIndex].Regions; FMantleClothingColorRegion NewRegion;
					int32 Number = 1; do { NewRegion.Id = FString::Printf(TEXT("region_%d"), Number++); } while (Regions.ContainsByPredicate([&NewRegion](const FMantleClothingColorRegion& Entry) { return Entry.Id == NewRegion.Id; }));
					NewRegion.Label = TEXT("Region"); NewRegion.Channel = 0;
					while (NewRegion.Channel < 2 && Regions.ContainsByPredicate([&NewRegion](const FMantleClothingColorRegion& Entry) { return Entry.Channel == NewRegion.Channel; })) ++NewRegion.Channel;
					Regions.Add(NewRegion); Package.Reset(); RefreshClothingColors();
				}
				return FReply::Handled();
			})];
		Advanced->AddSlot().AutoHeight().Padding(0, 8)[SNew(SBorder).Padding(8)[Rows]];
	}
	Advanced->AddSlot().AutoHeight().Padding(0, 5)[SNew(SButton).Text(LOCTEXT("DyeAddMaterial", "Set up areas on a material"))
		.IsEnabled(Draft.ClothingColors.Num() < 4).OnClicked_Lambda([this]
		{ if (Draft.ClothingColors.Num() < 4) { FMantleClothingColorAuthoring Declaration; FMantleClothingColorRegion Region; Region.Id = TEXT("region_1"); Region.Label = TEXT("Region"); Declaration.Regions.Add(Region); Draft.ClothingColors.Add(Declaration); Package.Reset(); RefreshClothingColors(); } return FReply::Handled(); })];
	ClothingColorsBox->AddSlot().AutoHeight().Padding(0, 8)
	[
		SNew(SExpandableArea)
		.InitiallyCollapsed(!bClothingColorsAdvancedOpen && Draft.ClothingColors.IsEmpty())
		.AreaTitle(LOCTEXT("DyeAdvancedTitle", "Advanced: mark the areas yourself"))
		.OnAreaExpansionChanged_Lambda([this](bool bExpanded) { bClothingColorsAdvancedOpen = bExpanded; })
		.BodyContent()[Advanced]
	];
}

FMantleClothingColorChoice& SMantleCosmeticPanel::ClothingChoice(const FString& MaterialName, const FString& RegionId)
{
	if (FMantleClothingColorChoice* Existing = Draft.ClothingColorChoices.FindByPredicate([&MaterialName, &RegionId](const FMantleClothingColorChoice& Entry)
		{ return Entry.MaterialName == MaterialName && Entry.RegionId == RegionId; }))
	{
		return *Existing;
	}
	FMantleClothingColorChoice& Choice = Draft.ClothingColorChoices.AddDefaulted_GetRef();
	Choice.MaterialName = MaterialName;
	Choice.RegionId = RegionId;
	return Choice;
}

TArray<FMantleClothingColor> SMantleCosmeticPanel::ClothingPreviewColorsWithHighlight() const
{
	TArray<FMantleClothingColor> Colors;
	if (bHighlightClothingAreas && Draft.ClothingColors.IsEmpty())
	{
		int32 AreaIndex = 0;
		for (const FMantleClothingColorDetection& Detection : DetectedClothingColors)
		{
			for (const FMantleClothingColorRegion& Area : Detection.Areas)
			{
				if (Colors.ContainsByPredicate([&Area](const FMantleClothingColor& Entry) { return Entry.RegionId == Area.Id; })) continue;
				FMantleClothingColor& Color = Colors.AddDefaulted_GetRef();
				Color.RegionId = Area.Id;
				Color.Color = FMantleClothingColorDetect::HighlightColor(AreaIndex++);
			}
		}
	}
	for (const FMantleClothingColor& Picked : ClothingPreviewColors)
	{
		if (!Colors.ContainsByPredicate([&Picked](const FMantleClothingColor& Entry) { return Entry.RegionId == Picked.RegionId; })) Colors.Add(Picked);
	}
	return Colors;
}

void SMantleCosmeticPanel::AddFoundClothingAreas()
{
	const auto Line = [this](const FText& Text)
	{
		ClothingColorsBox->AddSlot().AutoHeight().Padding(0, 4)[SNew(STextBlock).AutoWrapText(true).Text(Text)];
	};
	if (!Draft.ClothingColors.IsEmpty())
	{
		Line(LOCTEXT("DyeHandMade", "You've marked the colour areas yourself (under Advanced), so Mantle doesn't look for them."));
		return;
	}
	if (!bClothingAreasKnown)
	{
		Line(LOCTEXT("DyeNotYet", "Preview the item and Mantle will point out the parts players can recolour."));
		return;
	}
	int32 Count = 0, MaterialsWithAreas = 0;
	TSet<FString> Counted;
	for (const FMantleClothingColorDetection& Detection : DetectedClothingColors)
	{
		for (const FMantleClothingColorRegion& Area : Detection.Areas) { if (!Counted.Contains(Area.Id)) { Counted.Add(Area.Id); ++Count; } }
		MaterialsWithAreas += Detection.Areas.IsEmpty() ? 0 : 1;
	}
	if (Count == 0)
	{
		Line(LOCTEXT("DyeNone", "This item uploads in its original colours."));
		return;
	}
	Line(FText::Format(LOCTEXT("DyeFound", "We found {0} {0}|plural(one=area,other=areas) players can recolour. Untick any you'd like to keep fixed."), Count));
	ClothingColorsBox->AddSlot().AutoHeight().Padding(0, 2, 0, 6)
	[
		SNew(SCheckBox)
		.IsChecked(bHighlightClothingAreas ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
		.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bHighlightClothingAreas = State == ECheckBoxState::Checked; OnPreview(); })
		[SNew(STextBlock).Text(LOCTEXT("DyeHighlight", "Show each area in its own colour in the preview"))]
	];
	int32 AreaIndex = 0;
	TSet<FString> Listed;
	for (const FMantleClothingColorDetection& Detection : DetectedClothingColors)
	{
		for (const FMantleClothingColorRegion& Area : Detection.Areas)
		{
			// One row per control: an area two sections share (a left and right shoe) is listed once.
			if (Listed.Contains(Area.Id)) continue;
			Listed.Add(Area.Id);
			const FString MaterialName = Detection.MaterialName;
			const FString RegionId = Area.Id;
			const FMantleClothingColorChoice* Choice = Draft.ClothingColorChoices.FindByPredicate([&MaterialName, &RegionId](const FMantleClothingColorChoice& Entry)
				{ return Entry.MaterialName == MaterialName && Entry.RegionId == RegionId; });
			const bool bOn = !Choice || Choice->bEnabled;
			const FString Label = Choice && !Choice->Label.TrimStartAndEnd().IsEmpty() ? Choice->Label : Area.Label;
			const FString DefaultLabel = Area.Label;
			TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[
					SNew(SBox).WidthOverride(18).HeightOverride(18)
					[SNew(SColorBlock).Color(FLinearColor::FromSRGBColor(FMantleClothingColorDetect::HighlightColor(AreaIndex)))
						.ToolTipText(LOCTEXT("DyeSwatchTip", "This area's colour in the preview"))]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[
					SNew(SCheckBox)
					.IsChecked(bOn ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
					.ToolTipText(LOCTEXT("DyeTickTip", "Ticked: players can recolour this area. Unticked: it keeps its original colours."))
					.OnCheckStateChanged_Lambda([this, MaterialName, RegionId](ECheckBoxState State)
					{
						ClothingChoice(MaterialName, RegionId).bEnabled = State == ECheckBoxState::Checked;
						Package.Reset();
						OnPreview();
					})
				]
				+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
				[
					SNew(SEditableTextBox)
					.Text(FText::FromString(Label))
					.HintText(FText::FromString(DefaultLabel))
					.ToolTipText(LOCTEXT("DyeNameTip", "The name players see in the colour menu."))
					.OnTextCommitted_Lambda([this, MaterialName, RegionId, DefaultLabel](const FText& Text, ETextCommit::Type)
					{
						const FString Name = Text.ToString().TrimStartAndEnd().Left(64);
						FMantleClothingColorChoice& Target = ClothingChoice(MaterialName, RegionId);
						Target.Label = Name == DefaultLabel ? FString() : Name;
						Package.Reset();
					})
				];
			if (MaterialsWithAreas > 1)
			{
				Row->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
				[SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(FText::Format(LOCTEXT("DyeOnMaterial", "on {0}"), FText::FromString(MaterialName)))];
			}
			ClothingColorsBox->AddSlot().AutoHeight().Padding(0, 2)[Row];
			++AreaIndex;
		}
	}
}

void SMantleCosmeticPanel::ApplyOutfitKeep()
{
	Draft.KeepSlots = FMantleEditorActions::CosmeticKeepSlots(OutfitKeep, OutfitSlots);
}

bool SMantleCosmeticPanel::IsRunning() const
{
	return Session.IsValid() && !Session->IsFinished();
}

bool SMantleCosmeticPanel::BuildPackage()
{
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!Contract)
	{
		return false;
	}
	// Chunk 8: the same preparation the agent toolset runs (publication mode, converted face, the fit-size refusal).
	TArray<FMantleCosmeticIssue> Blockers;
	if (!FMantleEditorActions::PrepareCosmeticBuild(Draft, DoorRoute(), Blockers))
	{
		Package.Reset();
		ShowIssues(Blockers, {});
		return false;
	}
	if (Draft.Class == EMantleCosmeticClass::Accessory && !bAccessoryPlacementInitialized)
	{
		const USkeletalMesh* Body = PreviewSession.IsValid() && PreviewSession->GetView().State == EMantlePreviewState::Ready
			&& PreviewViewport.IsValid() ? PreviewViewport->GetShownBody() : Draft.AuthoringBody.Get();
		if (!FMantleAccessorySettings::AlignToAvatarAxes(Draft, Body, PreviewInputError))
		{
			ShowIssues({ FMantleCosmeticChecks::Error(PreviewInputError, TEXT("Choose the current MetaHuman under Authored on, or preview its catalog avatar first.")) }, {});
			return false;
		}
		bAccessoryPlacementInitialized = true;
		SaveAccessoryPlacement();
	}
	FScopedSlowTask Progress(1.0f, LOCTEXT("Preparing", "Checking and exporting..."));
	Progress.MakeDialogDelayed(0.5f);
	Progress.EnterProgressFrame(1.0f);
	Package = FMantleCosmeticExport::Build(Draft, *Contract);
	for (const FString& Note : Package->Notes)
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_EXPORT %s"), *Note);
	}
	for (const FMantleCosmeticIssue& Issue : Package->Issues)
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_CHECK %s"), *Issue.ToString());
	}
	ShowIssues(Package->Issues, Package->Notes);
	ShowHairHeadFit();
	return !Package->HasErrors();
}

void SMantleCosmeticPanel::ShowIssues(const TArray<FMantleCosmeticIssue>& Issues, const TArray<FString>& Notes)
{
	ShownIssues = Issues;
	if (!IssuesBox.IsValid())
	{
		return;
	}
	IssuesBox->ClearChildren();
	for (const FMantleCosmeticIssue& Issue : Issues)
	{
		IssuesBox->AddSlot().AutoHeight().Padding(0, 2)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(MantleCosmeticPanelPrivate::SeverityColour(Issue.Severity))
			.Text(FText::FromString(Issue.ToString()))
		];
	}
	if (Issues.Num() == 0)
	{
		IssuesBox->AddSlot().AutoHeight()
		[
			SNew(STextBlock)
			.ColorAndOpacity(MantleCosmeticPanelPrivate::SeverityColour(EMantleCosmeticSeverity::Info))
			.Text(LOCTEXT("AllGood", "Every check passed."))
		];
	}
}

void SMantleCosmeticPanel::ShowHairHeadFit()
{
	if (!IssuesBox.IsValid() || !Package.IsSet())
	{
		return;
	}
	// The hair card lives in its own box so confirming it leaves every check above it alone.
	const TSharedRef<SVerticalBox> Card = SNew(SVerticalBox);
	IssuesBox->AddSlot().AutoHeight()[Card];
	// #52: which route this hair takes, in words, before anything it did to the hair.
	if (Package->bHasHairRoute)
	{
		const FMantleHairRoute::FResult& Route = Package->HairRoute;
		Card->AddSlot().AutoHeight().Padding(0, 8, 0, 2)
		[
			SNew(STextBlock).Font(FAppStyle::GetFontStyle("BoldFont")).Text(FText::FromString(Route.Title))
		];
		Card->AddSlot().AutoHeight().Padding(0, 2)
		[
			SNew(STextBlock).AutoWrapText(true)
			.ColorAndOpacity(Route.bSupported ? FSlateColor::UseForeground() : FSlateColor(MantleCosmeticPanelPrivate::SeverityColour(EMantleCosmeticSeverity::Warning)))
			.Text(FText::FromString(Route.Words))
		];
	}
	if (!Package->bHairHeadFitted)
	{
		if (Draft.bPlaceHairByHand && Draft.Class == EMantleCosmeticClass::Hair)
		{
			Card->AddSlot().AutoHeight().Padding(0, 8, 0, 2)
			[
				SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("HairFitByHand", "This hair uploads where you made it; it was not sized to the standard head."))
			];
			Card->AddSlot().AutoHeight().Padding(0, 4)
			[
				SNew(SButton).Text(LOCTEXT("HairFitForMe", "Fit it for me"))
				.OnClicked_Lambda([this]
				{
					Draft.bPlaceHairByHand = false;
					Package.Reset();
					if (PreviewSession.IsValid()) PreviewSession->Cancel();
					BuildPackage();
					return FReply::Handled();
				})
			];
		}
		return;
	}
	const FMantleHairFitReport::FResult& Fit = Package->HairHeadFit;
	Card->AddSlot().AutoHeight().Padding(0, 8, 0, 2)
	[
		SNew(STextBlock).AutoWrapText(true).Text(FText::FromString(Fit.Headline))
	];
	// The hair as authored and as fitted, on the head it was fitted to (the same pictures as Mantle.Dev.RenderHairFit's angled view).
	{
		constexpr int32 PictureSize = 220;
		const auto MakeBrush = [this](bool bAfter) -> TSharedPtr<FSlateDynamicImageBrush>
		{
			TArray<FColor> Pixels;
			FString Ignored;
			if (!FMantleHairFitPictures::Render(Package->HairHeadFitMeshes, bAfter, FMantleHairFitPictures::AngledYaw, PictureSize, Pixels, Ignored)) return nullptr;
			// FColor is BGRA in memory, the order the dynamic brush takes. A new name each time, so Slate never shows the old picture.
			TArray<uint8> Bytes;
			Bytes.Append(reinterpret_cast<const uint8*>(Pixels.GetData()), Pixels.Num() * sizeof(FColor));
			static int32 Serial = 0;
			return FSlateDynamicImageBrush::CreateWithImageData(*FString::Printf(TEXT("MantleHairFit_%d"), ++Serial), FVector2f(PictureSize, PictureSize), Bytes);
		};
		HairFitBeforeBrush = MakeBrush(false);
		HairFitAfterBrush = MakeBrush(true);
		if (HairFitBeforeBrush.IsValid() && HairFitAfterBrush.IsValid())
		{
			Card->AddSlot().AutoHeight().Padding(0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(LOCTEXT("HairFitBefore", "As you made it"))]
					+ SVerticalBox::Slot().AutoHeight()[SNew(SImage).Image(HairFitBeforeBrush.Get())]
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(LOCTEXT("HairFitAfter", "Fitted to the standard head"))]
					+ SVerticalBox::Slot().AutoHeight()[SNew(SImage).Image(HairFitAfterBrush.Get())]
				]
			];
		}
	}
	for (const FString& Message : Fit.Messages)
	{
		Card->AddSlot().AutoHeight().Padding(0, 2)
		[
			SNew(STextBlock).AutoWrapText(true)
			.ColorAndOpacity(MantleCosmeticPanelPrivate::SeverityColour(EMantleCosmeticSeverity::Warning))
			.Text(FText::FromString(Message))
		];
	}
	Card->AddSlot().AutoHeight().Padding(0, 4)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
		[
			SNew(SButton).Text(FText::FromString(FMantleHairFitReport::ConfirmLabel()))
			.OnClicked_Lambda([WeakCard = TWeakPtr<SVerticalBox>(Card)]
			{
				// Confirming keeps the fitted hair; the preview and the upload already carry it.
				if (const TSharedPtr<SVerticalBox> Pinned = WeakCard.Pin())
				{
					Pinned->ClearChildren();
					Pinned->AddSlot().AutoHeight()
					[
						SNew(STextBlock).Text(LOCTEXT("HairFitConfirmed", "Hair fit confirmed."))
					];
				}
				return FReply::Handled();
			})
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SButton).Text(FText::FromString(FMantleHairFitReport::AdjustLabel()))
			.ToolTipText(LOCTEXT("HairFitAdjustTip", "Upload this hair where you made it instead, so you can place it by hand."))
			.OnClicked_Lambda([this]
			{
				Draft.bPlaceHairByHand = true;
				Package.Reset();
				if (PreviewSession.IsValid()) PreviewSession->Cancel();
				BuildPackage();
				return FReply::Handled();
			})
		]
	];
}

FReply SMantleCosmeticPanel::OnCheck()
{
	BuildPackage();
	if (ThumbnailSection.IsValid()) ThumbnailSection->Refresh();
	return FReply::Handled();
}

FReply SMantleCosmeticPanel::OnTestLocally()
{
	TestLocallyResult = FText::GetEmpty();
	bTestLocallyError = false;
	const bool bChecked = BuildPackage();
	if (ThumbnailSection.IsValid()) ThumbnailSection->Refresh();
	if (!bChecked || !Package.IsSet())
	{
		TestLocallyResult = LOCTEXT("TestLocallyFixFirst", "Fix the problems below first, then choose Test locally again.");
		bTestLocallyError = true;
		return FReply::Handled();
	}
	// The Mantle Preview panel follows the run (finding the app, preparing, starting, ready) as for a level.
	SMantlePreviewPanel::OpenTab();
	FString Error;
	FString Notice;
	const TSharedRef<const FMantleCosmeticPackage> Avatar = MakeShared<FMantleCosmeticPackage>(Package.GetValue());
	if (FMantleTestLocally::Get().RunWithDraftAvatar(Avatar, Error, Notice))
	{
		FString Said = TEXT("The Mantle app is opening with you as this avatar. Nothing was uploaded.");
		if (!Notice.IsEmpty())
		{
			Said += TEXT(" ") + Notice;
		}
		TestLocallyResult = FText::FromString(Said);
	}
	else
	{
		TestLocallyResult = FText::FromString(Error.IsEmpty() ? FString(TEXT("The test was cancelled.")) : Error);
		bTestLocallyError = !Error.IsEmpty();
	}
	return FReply::Handled();
}

FReply SMantleCosmeticPanel::OnUpload()
{
	if (!BuildPackage())
	{
		if (ThumbnailSection.IsValid()) ThumbnailSection->Refresh();
		return FReply::Handled();
	}
	if (ThumbnailSection.IsValid()) ThumbnailSection->Refresh();
	if (!FMantleCreatorSignIn::EnsureSignedIn(LOCTEXT("UploadSignInReason", "Sign in to publish this item to Mantle.")))
	{
		return FReply::Handled();
	}
	const FString ResumeFile = FMantleCosmeticUploadSession::DefaultResumeFile(
		Draft.bOverlayPlaced ? (Draft.OverlayPicture.IsValid() ? Draft.OverlayPicture->GetPathName() : Draft.OverlayPictureFile)
			: Draft.Source.IsValid() ? Draft.Source->GetPathName() : Draft.OverlayMaskFile, Draft.Slot);
	Session = MakeShared<FMantleCosmeticUploadSession>(Package.GetValue(), MakeShared<FMantleApiUploadTransport>(), ResumeFile);
	Session->Start();
	RegisterActiveTimer(0.1f, FWidgetActiveTimerDelegate::CreateSP(this, &SMantleCosmeticPanel::TickUpload));
	return FReply::Handled();
}

FReply SMantleCosmeticPanel::OnCancel()
{
	if (Session.IsValid())
	{
		Session->Cancel();
	}
	return FReply::Handled();
}

EActiveTimerReturnType SMantleCosmeticPanel::TickUpload(double InCurrentTime, float InDeltaTime)
{
	if (!Session.IsValid())
	{
		return EActiveTimerReturnType::Stop;
	}
	Session->Tick();
	if (Session->IsFinished())
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_UPLOAD %s item=%s status=%s"),
			Session->Succeeded() ? TEXT("DONE") : TEXT("STOPPED"), *Session->GetItemId(), *Session->GetItemStatus());
		// Chunk 6: the publish list's open row is uploaded (while the draft is still what it opened); walking, the next ticked row opens.
		if (PublishRows.IsValidIndex(PublishOpen) && PublishOpenSource.IsValid() && Draft.Source == PublishOpenSource)
		{
			if (Session->Succeeded())
			{
				PublishRows[PublishOpen].bUploaded = true;
				const int32 Next = bPublishWalk ? FMantlePublishList::Next(PublishRows, PublishOpen) : INDEX_NONE;
				if (Next != INDEX_NONE)
				{
					OpenPublishRow(Next);
				}
				else
				{
					bPublishWalk = false;
					RefreshConvertRows();
				}
			}
			else
			{
				bPublishWalk = false;
			}
		}
		return EActiveTimerReturnType::Stop;
	}
	return EActiveTimerReturnType::Continue;
}

FText SMantleCosmeticPanel::GetStatusText() const
{
	if (Session.IsValid())
	{
		return FText::FromString(Session->GetStatusText());
	}
	if (Package.IsSet())
	{
		return Package->HasErrors()
			? LOCTEXT("FixFirst", "Fix the problems below, then choose Upload to Mantle.")
			: LOCTEXT("ReadyToUpload", "Ready: choose Upload to Mantle.");
	}
	return LOCTEXT("Idle", "Choose what you are publishing and the asset, then Upload to Mantle. The plugin checks, exports and uploads everything.");
}

FText SMantleCosmeticPanel::GetSignInText() const
{
	return FMantleCreatorSignIn::IsSignedIn()
		? FMantleCreatorSignIn::DescribeSignedIn()
		: LOCTEXT("NotSignedIn", "Not signed in: Upload asks for a creator token (Tools > Mantle > Sign in with creator token).");
}

FText SMantleCosmeticPanel::GetFilesText() const
{
	TArray<FString> Lines;
	if (Session.IsValid())
	{
		for (const FMantleUploadAssetProgress& Asset : Session->GetAssets())
		{
			Lines.Add(FString::Printf(TEXT("%s: %s%s%s"), *Asset.Label, MantleCosmeticPanelPrivate::StateText(Asset.State),
				Asset.Message.IsEmpty() ? TEXT("") : TEXT(" - "), *Asset.Message));
		}
		if (!Session->GetItemId().IsEmpty())
		{
			Lines.Add(FString::Printf(TEXT("Item %s: %s (screening %s)%s"), *Session->GetItemId(), *Session->GetItemStatus(), *Session->GetItemModerationState(),
				Session->GetItemMayClipInMotion() ? TEXT(", may clip in motion") : TEXT("")));
		}
		Lines.Add(FString());
		Lines.Append(Session->GetLog());
	}
	else if (Package.IsSet())
	{
		for (const FMantleCosmeticUploadAsset& Asset : Package->Assets)
		{
			Lines.Add(FString::Printf(TEXT("%s: %.1f KB"), *Asset.Label, Asset.Bytes.Num() / 1024.0));
		}
		Lines.Add(FString());
		Lines.Append(Package->Notes);
	}
	return FText::FromString(FString::Join(Lines, TEXT("\n")));
}

// ---- The fit preview ---------------------------------------------------------------------------------

TSharedRef<SWidget> SMantleCosmeticPanel::MakePreviewSection()
{
	using namespace MantleCosmeticPanelPrivate;
	auto AnimationSkeleton = [this]() -> const USkeleton*
	{
		const USkeletalMesh* Body = PreviewViewport.IsValid() && PreviewViewport->GetShownBody() ? PreviewViewport->GetShownBody() : Draft.AuthoringBody.Get();
		return Body ? Body->GetSkeleton() : nullptr;
	};
	const TAttribute<EVisibility> GarmentRows = TAttribute<EVisibility>::CreateLambda([this]
	{
		return Draft.Class == EMantleCosmeticClass::Garment || Draft.Class == EMantleCosmeticClass::Hair || Draft.Class == EMantleCosmeticClass::Accessory ? EVisibility::Visible : EVisibility::Collapsed;
	});
	// #283: the picker opens on the row this session picked last (the source body the first time).
	TSharedPtr<FMantlePreviewPick> InitialRow;
	for (const TSharedPtr<FMantlePreviewPick>& Option : PreviewPickOptions)
	{
		if (Option.IsValid() && Option->IsSameRow(PreviewPick))
		{
			InitialRow = Option;
			break;
		}
	}
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text_Lambda([this]
			{
				if (Draft.Class == EMantleCosmeticClass::Accessory) return LOCTEXT("AccessoryPreviewHelp", "Preview the rigid item on the source or a catalog MetaHuman. Attachment edits update the preview and are saved per mesh and slot. Check hair and face clearance; this does not reshape the accessory.");
				if (Draft.Class == EMantleCosmeticClass::Overlay) return LOCTEXT("OverlayPreviewHelp", "See the overlay on a converted MetaHuman body or face (the slot decides which), drawn with the same skin material players' clients use. This shows exactly the image that would upload. Nothing is uploaded.");
				if (Draft.Class == EMantleCosmeticClass::Hair) return LOCTEXT("HairPreviewHelp", "Preview the published cards fitted to a source or catalog MetaHuman head. Scalp cards follow the body; brows, lashes and facial hair follow the face. Choose the source head the cards were authored on before checking fit.");
				return Draft.Class == EMantleCosmeticClass::Avatar
					? LOCTEXT("AvatarPreviewHelp", "See the avatar the way players will: posed by the same retargeter their game runs, copying the UEFN mannequin's pose. Nothing needs adding to the project: Mantle brings its own mannequin, and uses yours (with its standing idle) when the project has SKM_UEFN_Mannequin. Nothing is uploaded. Move around it in the viewport as in the garment preview.")
					: LOCTEXT("PreviewHelp", "See the garment the way players will: fitted by the same code their game runs, on the body you made it on or any catalog avatar by its id (itm_u...). The body under the garment is hidden as in the game. Nothing is uploaded; catalog avatars are downloaded once and kept while the panel is open.");
			})
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			.Visibility(GarmentRows)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(STextBlock).Text(LOCTEXT("PreviewOnLabel", "Preview on"))
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(SComboBox<TSharedPtr<FMantlePreviewPick>>)
				.OptionsSource(&PreviewPickOptions)
				.InitiallySelectedItem(InitialRow)
				.OnGenerateWidget_Lambda([](TSharedPtr<FMantlePreviewPick> Item)
				{
					return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? Item->Label() : FString()));
				})
				.OnSelectionChanged_Lambda([this](TSharedPtr<FMantlePreviewPick> Item, ESelectInfo::Type)
				{
					if (Item.IsValid())
					{
						// The row only: what was typed for an id stays for when that row is picked again.
						PreviewPick.Kind = Item->Kind;
						RememberPreviewPick();
					}
				})
				[
					SNew(STextBlock).Text_Lambda([this] { return FText::FromString(PreviewPick.Label()); })
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(SEditableTextBox)
				.Visibility_Lambda([this] { return PreviewPick.Kind == EMantlePreviewPickKind::TypedId ? EVisibility::Visible : EVisibility::Collapsed; })
				.HintText(LOCTEXT("PreviewAvatarHint", "itm_u followed by 32 hex digits"))
				.Text_Lambda([this] { return FText::FromString(PreviewPick.TypedId); })
				.OnTextChanged_Lambda([this](const FText& Text)
				{
					PreviewPick.TypedId = Text.ToString().TrimStartAndEnd();
					RememberPreviewPick();
				})
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
		[
			SNew(SHorizontalBox)
			.Visibility(GarmentRows)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this] { return bPreviewAnimate ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this, AnimationSkeleton](ECheckBoxState State)
				{
					bPreviewAnimate = State == ECheckBoxState::Checked;
					if (bPreviewAnimate && !PreviewAnimation.IsValid())
					{
						// An idle on the body's skeleton when the project has one; the creator can pick any other.
						PreviewAnimation = FindIdleAnimation(AnimationSkeleton());
					}
					ApplyPreviewAnimation();
				})
				[
					SNew(STextBlock).Text(LOCTEXT("PreviewAnimate", "Play an animation (else the reference pose)"))
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SObjectPropertyEntryBox)
				.AllowedClass(UAnimSequence::StaticClass())
				.AllowClear(true)
				.DisplayThumbnail(false)
				.IsEnabled_Lambda([this] { return bPreviewAnimate; })
				.OnShouldFilterAsset_Lambda([AnimationSkeleton](const FAssetData& Asset)
				{
					const USkeleton* Skeleton = AnimationSkeleton();
					return Skeleton && !Skeleton->IsCompatibleForEditor(Asset);
				})
				.ObjectPath_Lambda([this] { return PreviewAnimation.IsValid() ? PreviewAnimation->GetPathName() : FString(); })
				.OnObjectChanged_Lambda([this](const FAssetData& Asset)
				{
					PreviewAnimation = Cast<UAnimSequence>(Asset.GetAsset());
					ApplyPreviewAnimation();
				})
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
			[
				SNew(SButton)
				.IsEnabled_Lambda([this] { return ContractError.IsEmpty(); })
				.Text(LOCTEXT("PreviewButton", "Preview"))
				.ToolTipText_Lambda([this]
				{
					if (Draft.Class == EMantleCosmeticClass::Accessory) return LOCTEXT("AccessoryPreviewTooltip", "Show this rigid accessory on the chosen MetaHuman, with its published attachment correction.");
					return Draft.Class == EMantleCosmeticClass::Avatar
						? LOCTEXT("AvatarPreviewTooltip", "Show the avatar posed by its retargeter from the UEFN mannequin, as players' clients pose it. Works without a mannequin in the project; with SKM_UEFN_Mannequin and its idle it also plays the idle.")
						: LOCTEXT("PreviewTooltip", "Fit the garment to the chosen body and show it. The fit runs in the background; the editor stays responsive.");
				})
				.OnClicked(this, &SMantleCosmeticPanel::OnPreview)
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity_Lambda([this]
				{
					const bool bFailed = Draft.Class == EMantleCosmeticClass::Overlay ? bOverlayPreviewFailed
						: Draft.Class == EMantleCosmeticClass::Avatar ? bAvatarPreviewFailed
						: (!PreviewInputError.IsEmpty() || (PreviewSession.IsValid() && PreviewSession->GetView().State == EMantlePreviewState::Failed));
					return bFailed ? SeverityColour(EMantleCosmeticSeverity::Error) : FSlateColor::UseForeground();
				})
				.Text(this, &SMantleCosmeticPanel::GetPreviewStatusText)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
		[
			SNew(SBox)
			.HeightOverride(420.0f)
			[
				SAssignNew(PreviewViewport, SMantleCosmeticPreviewViewport)
			]
		];
}

TSharedRef<SWidget> SMantleCosmeticPanel::MakeThumbnailSection()
{
	return SAssignNew(ThumbnailSection, SMantleThumbnailSection)
		.Target(FMantleThumbnailTarget::Square())
		.Choice_Lambda([this] { return Draft.Thumbnail; })
		.OnChoiceChanged_Raw(this, &SMantleCosmeticPanel::ThumbnailChoiceChanged)
		.AngleSliders(true)
		.DefaultAngle_Lambda([this] { return FMantleCosmeticThumbnail::DefaultView(Draft.Class == EMantleCosmeticClass::Accessory); })
		.Render(FMantleThumbnailResolver::FRender([this](const FMantleThumbnailChoice&, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError)
		{
			// The picture the upload carries is the one the check just exported, so it is shown from the package.
			const FMantleCosmeticUploadAsset* Asset = Package.IsSet() && !Package->ThumbnailKey.IsEmpty() ? Package->FindAsset(Package->ThumbnailKey) : nullptr;
			if (!Asset)
			{
				OutError = TEXT("Choose Check only to draw this picture.");
				return false;
			}
			IImageWrapperModule& Wrappers = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
			const TSharedPtr<IImageWrapper> Wrapper = Wrappers.CreateImageWrapper(EImageFormat::PNG);
			TArray64<uint8> Raw;
			if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Asset->Bytes.GetData(), Asset->Bytes.Num()) || !Wrapper->GetRaw(ERGBFormat::BGRA, 8, Raw))
			{
				OutError = TEXT("The exported thumbnail could not be shown.");
				return false;
			}
			OutWidth = Wrapper->GetWidth();
			OutHeight = Wrapper->GetHeight();
			OutPixels.SetNumUninitialized(static_cast<int32>(Raw.Num() / sizeof(FColor)));
			FMemory::Memcpy(OutPixels.GetData(), Raw.GetData(), OutPixels.Num() * sizeof(FColor));
			return true;
		}))
		[
			SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("ThumbnailAngleHint", "Choose Check only to see the picture from this angle."))
		];
}

void SMantleCosmeticPanel::LoadThumbnailChoice()
{
	const UObject* Source = Draft.Source.Get();
	FMantleThumbnailStore::Load(Source ? Source->GetPathName() : FString(), FString::FromInt(static_cast<int32>(Draft.Class)), Draft.Thumbnail, FMantleThumbnailStore::DefaultConfigFile());
	if (ThumbnailSection.IsValid()) ThumbnailSection->Refresh();
}

void SMantleCosmeticPanel::ThumbnailChoiceChanged(const FMantleThumbnailChoice& Choice)
{
	Draft.Thumbnail = Choice;
	Package.Reset();
	const UObject* Source = Draft.Source.Get();
	FMantleThumbnailStore::Save(Source ? Source->GetPathName() : FString(), FString::FromInt(static_cast<int32>(Draft.Class)), Choice, FMantleThumbnailStore::DefaultConfigFile());
}

bool SMantleCosmeticPanel::IsOutfitAuthoringCandidate(const FAssetData& Asset, bool bFace) const
{
	const UMetaHumanCharacter* Character = Draft.OutfitAuthoringCharacter.Get();
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	if (!Character || !Contract || Asset.AssetClassPath != USkeletalMesh::StaticClass()->GetClassPathName()) return false;
	const FString Root = FPackageName::GetLongPackagePath(Character->GetOutermost()->GetName()) + TEXT("/Unpacked/");
	if (!Asset.PackageName.ToString().StartsWith(Root)) return false;
	USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset.GetAsset());
	if (!FMantleAvatarConverter::IsConvertOutput(Mesh)) return false;
	const FString* Built = Mesh->GetPackage()->GetMetaData().FindValue(Mesh, FMantleAvatarConverter::BuiltKey);
	if (!Built || !Built->StartsWith(Root)) return false;
	if (!bFace) return FMantleCosmeticExport::FindReferenceLOD(Mesh, *Contract) != INDEX_NONE;
	const USkeletalMesh* Body = Draft.AuthoringBody.Get();
	if (!Body || Mesh->GetRefSkeleton().FindBoneIndex(TEXT("FACIAL_C_FacialRoot")) == INDEX_NONE) return false;
	const FString* BodyBuilt = Body->GetPackage()->GetMetaData().FindValue(Body, FMantleAvatarConverter::BuiltKey);
	return BodyBuilt && *Built == *BodyBuilt;
}

void SMantleCosmeticPanel::RefreshOutfitAuthoringDefaults()
{
	if (Draft.Class != EMantleCosmeticClass::Garment || !FMantleParametricOutfit::IsSource(Draft.Source.Get())) return;
	if (Draft.AuthoringBody.IsValid() && !IsOutfitAuthoringCandidate(FAssetData(Draft.AuthoringBody.Get()), false)) Draft.AuthoringBody.Reset();
	if (Draft.AuthoringFace.IsValid() && !IsOutfitAuthoringCandidate(FAssetData(Draft.AuthoringFace.Get()), true)) Draft.AuthoringFace.Reset();
	const UMetaHumanCharacter* Character = Draft.OutfitAuthoringCharacter.Get();
	if (!Character) return;
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*(FPackageName::GetLongPackagePath(Character->GetOutermost()->GetName()) + TEXT("/Unpacked"))));
	Filter.ClassPaths.Add(USkeletalMesh::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> Assets;
	Registry.GetAssets(Filter, Assets);
	if (!Draft.AuthoringBody.IsValid())
	{
		TArray<USkeletalMesh*> Bodies;
		for (const FAssetData& Asset : Assets)
		{
			if (IsOutfitAuthoringCandidate(Asset, false)) Bodies.AddUnique(CastChecked<USkeletalMesh>(Asset.GetAsset()));
		}
		if (Bodies.Num() == 1) Draft.AuthoringBody = Bodies[0];
	}
	if (!Draft.AuthoringFace.IsValid() && Draft.AuthoringBody.IsValid())
	{
		TArray<USkeletalMesh*> Faces;
		for (const FAssetData& Asset : Assets)
		{
			if (IsOutfitAuthoringCandidate(Asset, true)) Faces.AddUnique(CastChecked<USkeletalMesh>(Asset.GetAsset()));
		}
		if (Faces.Num() == 1) Draft.AuthoringFace = Faces[0];
	}
}

void SMantleCosmeticPanel::RefreshOutfitSizes()
{
	OutfitSizeOptions.Reset();
	OutfitSizeOptions.Add(MakeShared<FString>());
	OutfitSizeError.Reset();
	TArray<FMantleOutfitSourceSize> Sizes;
	if (Draft.Class == EMantleCosmeticClass::Garment && FMantleParametricOutfit::IsSource(Draft.Source.Get())
		&& FMantleParametricOutfit::ListSizes(Draft.Source.Get(), Sizes, OutfitSizeError))
	{
		for (const auto& Size : Sizes) OutfitSizeOptions.Add(MakeShared<FString>(Size.Name));
		// Production authoring lets Epic select among all source sizes for each representative body.
	}
	if (OutfitSizeCombo.IsValid()) OutfitSizeCombo->RefreshOptions();
}

TSharedRef<SWidget> SMantleCosmeticPanel::MakeParametricOutfitSection()
{
	// "Made for": the body the creator made the clothing on picks Epic's women's or men's standard bodies for its sizes.
	auto FitSexButton = [this](const EMantleFitSex Sex, const FText& Label)
	{
		return SNew(SCheckBox)
			.Style(&FAppStyle::Get(), "RadioButton")
			.IsEnabled_Lambda([this] { return !IsRunning(); })
			.IsChecked_Lambda([this, Sex] { return Draft.FitSex == Sex ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this, Sex](const ECheckBoxState State)
			{
				if (State != ECheckBoxState::Checked || Draft.FitSex == Sex) return;
				FMantleEditorActions::SetCosmeticFitSex(Draft, Sex, FMantleAvatarContract::Get());
				Package.Reset();
				if (PreviewSession.IsValid()) PreviewSession->Cancel();
				OutfitPreviewKeepAlive.Reset();
				RefreshOutfitTargets();
			})
			[
				SNew(STextBlock).Text(Label)
			];
	};
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SVerticalBox)
			.Visibility_Lambda([this] { return Cast<USkeletalMesh>(Draft.Source.Get()) ? EVisibility::Visible : EVisibility::Collapsed; })
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("OriginalBodyHeadHelp", "Select the original combined body/head the garment was made on. Its shape, pose and coordinates must match the garment; a converted body alone is not this source."))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[
				SNew(SObjectPropertyEntryBox)
				.AllowedClass(USkeletalMesh::StaticClass()).AllowClear(true).DisplayThumbnail(false)
				.ObjectPath_Lambda([this] { return ParametricSourceBodyHead.IsValid() ? ParametricSourceBodyHead->GetPathName() : FString(); })
				.OnObjectChanged_Lambda([this](const FAssetData& Asset) { ParametricSourceBodyHead = Cast<USkeletalMesh>(Asset.GetAsset()); OutfitAuthoringStatus.Reset(); })
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SButton).Text(LOCTEXT("CreateOutfit", "Create parametric Outfit"))
				.IsEnabled_Lambda([this] { return !IsRunning() && Cast<USkeletalMesh>(Draft.Source.Get()) && ParametricSourceBodyHead.IsValid(); })
				.OnClicked(this, &SMantleCosmeticPanel::OnCreateParametricOutfit)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 6)
		[
			SNew(SVerticalBox)
			.Visibility_Lambda([this] { return FMantleParametricOutfit::IsSource(Draft.Source.Get()) ? EVisibility::Visible : EVisibility::Collapsed; })
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("OutfitTargetsHelp", "Mantle fits the clothing to Epic's standard body types automatically (listed below), so nothing needs converting. To add a body of your own, choose a Creator character and its converted body/face, then add it as a fit source. All sources upload as one clothing item; Epic chooses its authored size for each source."))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[
					SNew(STextBlock).Text(LOCTEXT("FitSexLabel", "Made for:"))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
				[
					FitSexButton(EMantleFitSex::Auto, LOCTEXT("FitSexAuto", "Automatic"))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
				[
					FitSexButton(EMantleFitSex::Female, LOCTEXT("FitSexFemale", "Women's body"))
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					FitSexButton(EMantleFitSex::Male, LOCTEXT("FitSexMale", "Men's body"))
				]
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(LOCTEXT("FitSexHelp", "The style of body you made this clothing on. It picks whether its sizes are made on Epic's standard women's or men's bodies; Automatic guesses from the Outfit's own sizes."))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4)
			[
				SNew(SButton).Text(LOCTEXT("AddOutfitTarget", "Add selected converted fit source"))
				.IsEnabled_Lambda([this] { return !IsRunning() && Draft.OutfitTargets.Num() < 8 && Draft.OutfitAuthoringCharacter.IsValid() && Draft.AuthoringBody.IsValid() && Draft.AuthoringFace.IsValid(); })
				.OnClicked_Lambda([this]
				{
					if (!IsOutfitAuthoringCandidate(FAssetData(Draft.AuthoringBody.Get()), false) || !IsOutfitAuthoringCandidate(FAssetData(Draft.AuthoringFace.Get()), true)) return FReply::Handled();
					if (Draft.OutfitTargets.ContainsByPredicate([this](const FMantleOutfitAuthoringTarget& Target) { return Target.Body == Draft.AuthoringBody; })) return FReply::Handled();
					int32 Number = 1;
					FString Id;
					do { Id = FString::Printf(TEXT("fit%d"), Number++); } while (Draft.OutfitTargets.ContainsByPredicate([&Id](const FMantleOutfitAuthoringTarget& Target) { return Target.Id == Id; }));
					Draft.OutfitTargets.Add({Id, Draft.OutfitAuthoringCharacter, Draft.AuthoringBody, Draft.AuthoringFace});
					Package.Reset(); RefreshOutfitTargets();
					return FReply::Handled();
				})
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SAssignNew(OutfitTargetsBox, SVerticalBox)
			]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] { return FText::FromString(OutfitAuthoringStatus); })
		];
}

void SMantleCosmeticPanel::RefreshOutfitTargets()
{
	if (!OutfitTargetsBox.IsValid()) return;
	OutfitTargetsBox->ClearChildren();
	for (const FMantleOutfitAuthoringTarget& Target : Draft.OutfitTargets)
	{
		const FString Id = Target.Id;
		const FString Label = Id + TEXT(": ") + (Target.Character.IsValid() ? Target.Character->GetName() : TEXT("Missing character"));
		OutfitTargetsBox->AddSlot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1)
			[SNew(STextBlock).Text(FText::FromString(Label))]
			+ SHorizontalBox::Slot().AutoWidth()
			[SNew(SButton).Text(LOCTEXT("RemoveOutfitTarget", "Remove")).IsEnabled_Lambda([this] { return !IsRunning(); }).OnClicked_Lambda([this, Id]
			{
				Draft.OutfitTargets.RemoveAll([&Id](const FMantleOutfitAuthoringTarget& Row) { return Row.Id == Id; });
				Package.Reset(); RefreshOutfitTargets(); return FReply::Handled();
			})]
		];
	}
}

FReply SMantleCosmeticPanel::OnCreateParametricOutfit()
{
	USkeletalMesh* Garment = Cast<USkeletalMesh>(Draft.Source.Get());
	if (!Garment || !ParametricSourceBodyHead.IsValid()) return FReply::Handled();
	FScopedSlowTask Progress(1, LOCTEXT("AuthoringOutfit", "Creating parametric Outfit resizing data..."));
	Progress.MakeDialog();
	FString PackageName, AssetName;
	IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	Tools.CreateUniqueAssetName(FPackageName::GetLongPackagePath(Garment->GetOutermost()->GetName()) / (TEXT("OA_") + Garment->GetName()), FString(), PackageName, AssetName);
	UPackage* AssetPackage = CreatePackage(*PackageName);
	const FMantleOutfitAuthoredSource Sources[] = { { TEXT("authored"), Garment, ParametricSourceBodyHead } };
	TArray<TStrongObjectPtr<UObject>> KeepAlive;
	FString Error;
	UChaosOutfitAsset* Outfit = FMantleParametricOutfitAuthoring::Create(Sources, AssetPackage, FName(*AssetName), 1500, KeepAlive, Error);
	if (!Outfit) { OutfitAuthoringStatus = Error; return FReply::Handled(); }
	FSavePackageArgs Save;
	Save.TopLevelFlags = RF_Public | RF_Standalone;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(AssetPackage, Outfit, *Filename, Save))
	{ OutfitAuthoringStatus = TEXT("The native Outfit was created but could not be saved. Check the output folder permissions."); return FReply::Handled(); }
	FAssetRegistryModule::AssetCreated(Outfit);
	AuthoredOutfitKeepAlive = MoveTemp(KeepAlive);
	SetSource(Outfit);
	OutfitAuthoringStatus = TEXT("Saved ") + Outfit->GetPathName() + TEXT(". Mantle fits it to Epic's standard bodies automatically; choose what it was made for, then check and upload it.");
	return FReply::Handled();
}

FReply SMantleCosmeticPanel::OnPreview()
{
	if (Draft.Class == EMantleCosmeticClass::Avatar)
	{
		return OnPreviewAvatar();
	}
	if (Draft.Class == EMantleCosmeticClass::Accessory) return OnPreviewAccessory();
	if (Draft.Class == EMantleCosmeticClass::Overlay) return OnPreviewOverlay();
	if (!PreviewSession.IsValid())
	{
		return FReply::Handled();
	}
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!Contract)
	{
		return FReply::Handled();
	}
	// #283: who it is fitted to: the source body or the explicitly typed catalog id.
	FMantlePreviewTarget Target;
	FString PickError;
	if (!PreviewPick.ToTarget(Target, PickError))
	{
		PreviewInputError = PickError;
		PreviewSession->Cancel();
		return FReply::Handled();
	}
	FMantleCosmeticDraft ReadingDraft = Draft;
	if (Draft.Class == EMantleCosmeticClass::Garment && FMantleParametricOutfit::IsSource(Draft.Source.Get()))
	{
		PreviewSession->Cancel();
		OutfitPreviewKeepAlive.Reset();
		if (!FMantleParametricOutfit::Prepare(Draft, *Contract, ReadingDraft, OutfitPreviewKeepAlive, PreviewInputError)) return FReply::Handled();
	}
	USkeletalMesh* Garment = Cast<USkeletalMesh>(ReadingDraft.Source.Get());
	if (Draft.Class == EMantleCosmeticClass::Garment)
	{
		ClothingMaterialNames = FMantleCosmeticExport::SectionMaterialNames(Garment, 0);
		RefreshClothingColors();
	}
	USkeletalMesh* Body = ReadingDraft.AuthoringBody.Get();
	if (Draft.Class == EMantleCosmeticClass::Hair && !Body)
	{
		Body = FMantleCosmeticExport::FindAuthoringBody(Draft.Source.Get(), *Contract);
		Draft.AuthoringBody = Body;
	}
	// #430: the face it was authored with, as the upload finds it: the preview fits the collar the way players' clients will.
	USkeletalMesh* Face = FMantleCosmeticExport::FindAuthoringFace(ReadingDraft, Body);
	FMantlePreviewGarment Inputs;
	FString Error;
	{
		FScopedSlowTask Progress(1.0f, LOCTEXT("PreviewReading", "Reading the garment and its body..."));
		Progress.MakeDialogDelayed(0.5f);
		Progress.EnterProgressFrame(1.0f);
		const bool bRead = Draft.Class == EMantleCosmeticClass::Hair
			? FMantlePreviewGarment::FromHairAssets(Draft, *Contract, Inputs, Error)
			: FMantlePreviewGarment::FromAssets(Garment, Body, ReadingDraft.Slot, *Contract, Inputs, Error, Face, ReadingDraft.KeepSlots.GetPtrOrNull());
		if (!bRead)
		{
			PreviewInputError = Error;
			PreviewSession->Cancel();
			UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_PREVIEW inputs refused: %s"), *Error);
			return FReply::Handled();
		}
	}
	PreviewInputError.Reset();
	const bool bHandMadeColors = !ReadingDraft.ClothingColors.IsEmpty();
	if (Draft.Class == EMantleCosmeticClass::Garment && (bHandMadeColors || ReadingDraft.bDetectClothingColors))
	{
		ReadingDraft.OutfitTargets.Reset(); ReadingDraft.bGenerateLODs = false; ReadingDraft.bRenderThumbnail = false;
		// #756: the areas are found the way the upload finds them; a missing name or rights tick must not hide them.
		if (ReadingDraft.Name.TrimStartAndEnd().IsEmpty()) ReadingDraft.Name = TEXT("Preview");
		ReadingDraft.bPublic = false;
		FMantleCosmeticPackage ColorPackage;
		{
			FScopedSlowTask Finding(1.0f, LOCTEXT("PreviewFindingAreas", "Looking for the parts players can recolour..."));
			Finding.MakeDialogDelayed(0.5f);
			Finding.EnterProgressFrame(1.0f);
			ColorPackage = FMantleCosmeticExport::Build(ReadingDraft, *Contract);
		}
		for (const FString& Note : ColorPackage.Notes)
		{
			if (Note.Contains(TEXT("colour area"))) UE_LOG(LogMantle, Log, TEXT("MANTLE_CLOTHING_AREAS %s"), *Note);
		}
		if (ColorPackage.HasErrors())
		{
			if (bHandMadeColors)
			{
				PreviewInputError = TEXT("The declared clothing colors failed export validation. Check the item for details.");
				ShowIssues(ColorPackage.Issues, ColorPackage.Notes); PreviewSession->Cancel(); return FReply::Handled();
			}
			DetectedClothingColors.Reset(); bClothingAreasKnown = false;
		}
		else
		{
			DetectedClothingColors = ColorPackage.DetectedClothingColors; bClothingAreasKnown = !bHandMadeColors;
			FString ColorError;
			if (!MakeClothingColorPreviewMaterials(ColorPackage, ColorPackage.MaterialNames, ClothingPreviewColorsWithHighlight(), Inputs.Materials, ColorError))
			{
				if (bHandMadeColors) { PreviewInputError = ColorError; PreviewSession->Cancel(); return FReply::Handled(); }
				UE_LOG(LogMantle, Log, TEXT("MANTLE_CLOTHING_AREAS preview highlight unavailable: %s"), *ColorError);
			}
		}
		RefreshClothingColors();
	}
	PreviewSession->SetSourceMeshes(Body, Face);
	PreviewSession->SetGarment(Inputs);
	if (Target.Kind == EMantlePreviewTargetKind::CatalogAvatar)
	{
		if (!MantleCosmetic::IsItemId(Target.AvatarId))
		{
			// Said by the session with the id's expected shape; no sign-in needed to hear it.
			PreviewSession->Request(Target);
			return FReply::Handled();
		}
		if (!FMantleCreatorSignIn::EnsureSignedIn(LOCTEXT("PreviewSignInReason", "Sign in to preview on avatars from the Mantle catalog.")))
		{
			PreviewInputError = TEXT("Sign in to preview on catalog avatars, or pick the body it was made on.");
			return FReply::Handled();
		}
	}
	PreviewSession->Request(Target);
	return FReply::Handled();
}

FString SMantleCosmeticPanel::AccessorySettingsFile() const
{
	return FMantleAccessorySettings::DefaultConfigFile();
}

void SMantleCosmeticPanel::SaveAccessoryPlacement()
{
	if (!bAccessoryPlacementInitialized) return;
	FMantleAccessorySettings::Save(Draft, AccessorySettingsFile());
}

FReply SMantleCosmeticPanel::OnAlignAccessoryAxes()
{
	const USkeletalMesh* Body = PreviewSession.IsValid() && PreviewSession->GetView().State == EMantlePreviewState::Ready
		&& PreviewViewport.IsValid() ? PreviewViewport->GetShownBody() : Draft.AuthoringBody.Get();
	if (FMantleAccessorySettings::AlignToAvatarAxes(Draft, Body, PreviewInputError))
	{
		bAccessoryPlacementInitialized = true;
		Package.Reset();
		SaveAccessoryPlacement();
		RefreshAccessoryPreview();
	}
	return FReply::Handled();
}

void SMantleCosmeticPanel::RefreshAccessoryPreview()
{
	if (Draft.Class != EMantleCosmeticClass::Accessory || !PreviewViewport.IsValid() || !PreviewSession.IsValid()
		|| PreviewSession->GetView().State != EMantlePreviewState::Ready) return;
	if (!bAccessoryPlacementInitialized)
	{
		if (!FMantleAccessorySettings::AlignToAvatarAxes(Draft, PreviewViewport->GetShownBody(), PreviewInputError)) return;
		bAccessoryPlacementInitialized = true;
		Package.Reset();
		SaveAccessoryPlacement();
	}
	FMantleCosmeticTransform Attach;
	if (Draft.bAttach)
	{
		Attach.Location = FVector(Draft.AttachLocation);
		Attach.Rotation = FRotator(Draft.AttachRotation.X, Draft.AttachRotation.Y, Draft.AttachRotation.Z);
		Attach.Scale = FVector(Draft.AttachScale);
	}
	PreviewViewport->ShowAccessory(Cast<UStaticMesh>(Draft.Source.Get()), Draft.Slot, Attach, PreviewSession->GetView().Sockets, PreviewInputError);
}

FReply SMantleCosmeticPanel::OnPreviewAccessory()
{
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!Contract || !PreviewSession.IsValid()) return FReply::Handled();
	PreviewInputError.Reset();
	USkeletalMesh* Body = Draft.AuthoringBody.Get();
	if (!Body)
	{
		Body = FMantleCosmeticExport::FindAuthoringBody(Draft.Source.Get(), *Contract);
		Draft.AuthoringBody = Body;
	}
	if (!Cast<UStaticMesh>(Draft.Source.Get()) || !Body)
	{
		PreviewInputError = TEXT("Choose the accessory static mesh and a local MetaHuman body under Authored on.");
		PreviewSession->Cancel();
		return FReply::Handled();
	}
	FString SkeletonId;
	if (!FMantleCosmeticExport::AvatarSkeletonId(Body, *Contract, SkeletonId, PreviewInputError) || SkeletonId != TEXT("metahuman"))
	{
		if (PreviewInputError.IsEmpty()) PreviewInputError = TEXT("Accessory preview requires a MetaHuman body.");
		PreviewSession->Cancel();
		return FReply::Handled();
	}
	FMantlePreviewTarget Target;
	if (!PreviewPick.ToTarget(Target, PreviewInputError))
	{
		PreviewSession->Cancel();
		return FReply::Handled();
	}
	if (Target.Kind == EMantlePreviewTargetKind::CatalogAvatar && MantleCosmetic::IsItemId(Target.AvatarId)
		&& !FMantleCreatorSignIn::EnsureSignedIn(LOCTEXT("AccessorySignIn", "Sign in to preview accessories on catalog avatars."))) return FReply::Handled();
	SaveAccessoryPlacement();
	PreviewSession->SetSourceMeshes(Body, FMantleCosmeticExport::FindAuthoringFace(Draft, Body));
	PreviewSession->RequestAvatar(Target);
	return FReply::Handled();
}


void SMantleCosmeticPanel::RememberPreviewPick() const
{
	MantleCosmeticPanelPrivate::RememberedPreviewPick() = PreviewPick;
}

FReply SMantleCosmeticPanel::OnPreviewAvatar()
{
	using namespace MantleCosmeticPanelPrivate;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!Contract || !PreviewViewport.IsValid())
	{
		return FReply::Handled();
	}
	auto Refuse = [this](const FString& Why)
	{
		AvatarPreviewStatus = Why;
		bAvatarPreviewFailed = true;
		if (PreviewSession.IsValid())
		{
			// Clears the viewport (its Idle view).
			PreviewSession->Cancel();
		}
		UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_PREVIEW avatar refused: %s"), *Why);
	};
	USkeletalMesh* Body = Cast<USkeletalMesh>(Draft.Source.Get());
	if (!Body)
	{
		Refuse(TEXT("Choose the avatar's body skeletal mesh (Asset) to preview it."));
		return FReply::Handled();
	}

	// Its skeleton as the upload identifies it, which picks the retargeter the client poses it through.
	FString SkeletonId;
	FString Error;
	{
		FScopedSlowTask Progress(1.0f, LOCTEXT("AvatarPreviewReading", "Reading the avatar's skeleton..."));
		Progress.MakeDialogDelayed(0.5f);
		Progress.EnterProgressFrame(1.0f);
		if (!FMantleCosmeticExport::AvatarSkeletonId(Body, *Contract, SkeletonId, Error))
		{
			Refuse(FString::Printf(TEXT("%s cannot be previewed: %s"), *Body->GetName(), *Error));
			return FReply::Handled();
		}
	}
	FMantleAvatarPreviewPlan Plan;
	if (!FMantleAvatarPreviewPlan::ForSkeleton(SkeletonId, Draft.Face.IsValid(), Plan, Error))
	{
		Refuse(Error);
		return FReply::Handled();
	}

	// #535: the plugin's retargeter, the UEFN mannequin as the driver (the project's, else the plugin's built-in one), and an idle.
	FMantleAvatarPreviewInputs Inputs;
	{
		FScopedSlowTask Progress(1.0f, LOCTEXT("AvatarPreviewDriver", "Preparing the UEFN mannequin that poses the avatar..."));
		Progress.MakeDialogDelayed(0.5f);
		Progress.EnterProgressFrame(1.0f);
		FString DriverError;
		USkeletalMesh* Driver = FMantleBuiltInMannequin::UEFNMannequin(*Contract, &DriverError);
		Inputs = FMantleAvatarPreviewInputs::Gather(Plan, Driver, FMantleAvatarPreviewInputs::DefaultIdleClipPath(), DriverError);
	}
	FMantleAvatarPreviewScene Stage;
	Stage.Body = Body;
	Stage.Face = Plan.bShowFace ? Draft.Face.Get() : nullptr;
	Stage.Key = Body->GetPathName();
	Stage.Retargeter = Inputs.Retargeter;
	Stage.Driver = Inputs.Driver;
	Stage.IdleClip = Inputs.IdleClip;
	Stage.Idle = Inputs.IdleAsset;

	// The garment preview gives the viewport way (its Idle view clears the stage first).
	if (PreviewSession.IsValid())
	{
		PreviewSession->Cancel();
	}
	PreviewInputError.Reset();
	const bool bPosed = PreviewViewport->ShowAvatar(Stage);
	AvatarPreviewStatus = Inputs.Status(Plan, bPosed, UMantlePreviewRetargetAnimInstance::IsRetargetNodeAvailable());
	bAvatarPreviewFailed = false;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_PREVIEW avatar=%s skeleton=%s posed=%d face=%d driver=%s idle=%s missing=%s"), *Body->GetPathName(),
		*SkeletonId, bPosed ? 1 : 0, Stage.Face ? 1 : 0, Inputs.Driver ? *Inputs.Driver->GetPathName() : TEXT("none"),
		Inputs.IdleClip.IsValid() ? TEXT("plugin") : (Inputs.IdleAsset ? *Inputs.IdleAsset->GetPathName() : TEXT("none")), *FString::Join(Inputs.Missing, TEXT(",")));
	return FReply::Handled();
}

TSharedRef<SWidget> SMantleCosmeticPanel::MakeOverlaySection()
{
	const TAttribute<EVisibility> ScarOnly = TAttribute<EVisibility>::CreateLambda([this]
	{
		return FMantleOverlayChecks::IsScarSlot(Draft.Slot) ? EVisibility::Visible : EVisibility::Collapsed;
	});
	const auto FileRow = [this](const FText& Label, const bool bNormal) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(STextBlock).Text(Label)
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 0, 6, 0)
			[
				SNew(SEditableTextBox)
				.HintText(LOCTEXT("OverlayFileHint", "Path to a .png file"))
				.Text_Lambda([this, bNormal] { return FText::FromString(bNormal ? Draft.OverlayNormalFile : Draft.OverlayMaskFile); })
				.OnTextCommitted_Lambda([this, bNormal](const FText& Text, ETextCommit::Type)
				{
					(bNormal ? Draft.OverlayNormalFile : Draft.OverlayMaskFile) = Text.ToString().TrimStartAndEnd().TrimQuotes();
					Draft.bRightsDeclared = false;
					Package.Reset();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("OverlayBrowse", "Browse..."))
				.OnClicked_Lambda([this, bNormal] { BrowseOverlayPng(bNormal); return FReply::Handled(); })
			];
	};
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
		[
			MakeOverlayPlaceSection()
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
		[
			SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Visibility_Lambda([this] { return Draft.bOverlayPlaced ? EVisibility::Collapsed : EVisibility::Visible; })
			.Text(LOCTEXT("OverlayMaskHelp", "The mask is the Asset above (a Texture 2D), or a PNG file chosen here when the Asset is empty. It is read as stored: white where the scar or tattoo shows, black where it does not."))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
		[
			SNew(SBox)
			.Visibility_Lambda([this] { return Draft.bOverlayPlaced ? EVisibility::Collapsed : EVisibility::Visible; })
			[
				FileRow(LOCTEXT("OverlayMaskFile", "Mask PNG"), false)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
		[
			SNew(SVerticalBox)
			.Visibility_Lambda([this, ScarOnly] { return Draft.bOverlayPlaced ? EVisibility::Collapsed : ScarOnly.Get(); })
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock).Text(LOCTEXT("OverlayNormalAsset", "Scar normal map (optional)"))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SObjectPropertyEntryBox)
				.AllowedClass(UTexture2D::StaticClass())
				.AllowClear(true)
				.DisplayThumbnail(false)
				.ObjectPath_Lambda([this] { return Draft.OverlayNormal.IsValid() ? Draft.OverlayNormal->GetPathName() : FString(); })
				.OnObjectChanged_Lambda([this](const FAssetData& Asset)
				{
					Draft.OverlayNormal = Cast<UTexture2D>(Asset.GetAsset());
					Draft.bRightsDeclared = false;
					Package.Reset();
				})
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				FileRow(LOCTEXT("OverlayNormalFile", "or a normal map PNG"), true)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
		[
			SNew(STextBlock).Text(LOCTEXT("OverlayPreviewBody", "Preview body (optional)"))
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SObjectPropertyEntryBox)
			.AllowedClass(USkeletalMesh::StaticClass())
			.AllowClear(true)
			.DisplayThumbnail(false)
			.ObjectPath_Lambda([this] { return OverlayPreviewBody.IsValid() ? OverlayPreviewBody->GetPathName() : FString(); })
			.OnObjectChanged_Lambda([this](const FAssetData& Asset) { OverlayPreviewBody = Cast<USkeletalMesh>(Asset.GetAsset()); RefreshOverlayPlacement(); })
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
		[
			SNew(STextBlock).Text(LOCTEXT("OverlayPreviewFace", "Preview face (optional)"))
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SObjectPropertyEntryBox)
			.AllowedClass(USkeletalMesh::StaticClass())
			.AllowClear(true)
			.DisplayThumbnail(false)
			.ObjectPath_Lambda([this] { return OverlayPreviewFace.IsValid() ? OverlayPreviewFace->GetPathName() : FString(); })
			.OnObjectChanged_Lambda([this](const FAssetData& Asset) { OverlayPreviewFace = Cast<USkeletalMesh>(Asset.GetAsset()); RefreshOverlayPlacement(); })
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
		[
			SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("OverlayPreviewBodyHelp", "A converted MetaHuman body and face to preview on. Left empty, Mantle's reference body and face are used when this project has built them."))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("OverlaySaveGuide", "Save painting guide"))
				.OnClicked_Lambda([this] { SaveOverlayGuide(); return FReply::Handled(); })
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
		[
			SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("OverlayGuideHint", "Open this under your painting as a guide layer; paint on the area you want, then hide the guide before saving."))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
		[
			SNew(STextBlock).AutoWrapText(true)
			.Visibility_Lambda([this] { return OverlayGuideStatus.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			.ColorAndOpacity_Lambda([this] { return bOverlayGuideFailed ? FSlateColor(FLinearColor::Red) : FSlateColor::UseSubduedForeground(); })
			.Text_Lambda([this] { return FText::FromString(OverlayGuideStatus); })
		];
}

TSharedRef<SWidget> SMantleCosmeticPanel::MakeOverlayPlaceSection()
{
	const TAttribute<EVisibility> PlacedOnly = TAttribute<EVisibility>::CreateLambda([this]
	{
		return Draft.bOverlayPlaced ? EVisibility::Visible : EVisibility::Collapsed;
	});
	const TAttribute<EVisibility> ScarPlacedOnly = TAttribute<EVisibility>::CreateLambda([this]
	{
		return Draft.bOverlayPlaced && FMantleOverlayChecks::IsScarSlot(Draft.Slot) ? EVisibility::Visible : EVisibility::Collapsed;
	});
	const auto Spin = [this](const FText& Label, const float Min, const float Max, const float Delta, float FMantleOverlayPlacement::* Field,
		const TAttribute<EVisibility>& Visible) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			.Visibility(Visible)
			+ SHorizontalBox::Slot().FillWidth(0.45f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(Label)
			]
			+ SHorizontalBox::Slot().FillWidth(0.55f).Padding(0, 1)
			[
				SNew(SSpinBox<float>)
				.MinValue(Min).MaxValue(Max).Delta(Delta)
				.Value_Lambda([this, Field] { return Draft.OverlayPlacement.*Field; })
				.OnValueChanged_Lambda([this, Field](const float Value)
				{
					Draft.OverlayPlacement.*Field = Value;
					if (OverlayPlaceViewport.IsValid())
					{
						OverlayPlaceViewport->SetPlacement(Draft.OverlayPlacement, Draft.OverlayPlaceAspect);
					}
				})
				.OnValueCommitted_Lambda([this](const float, ETextCommit::Type)
				{
					Draft.bRightsDeclared = false;
					Package.Reset();
				})
			];
	};
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SCheckBox)
			.IsChecked_Lambda([this] { return Draft.bOverlayPlaced ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](const ECheckBoxState State)
			{
				Draft.bOverlayPlaced = State == ECheckBoxState::Checked;
				Draft.bRightsDeclared = false;
				Package.Reset();
				RefreshOverlayPlacement();
			})
			[
				SNew(STextBlock).Text(LOCTEXT("OverlayPlaceToggle", "Place a picture on the body or face (instead of painting a mask)"))
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
		[
			SNew(SVerticalBox)
			.Visibility(PlacedOnly)
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
			[
				SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(LOCTEXT("OverlayPlaceHelp", "Choose a flat picture, then click the skin where it goes. Drag the arrows to move it (W), turn it (E) or size it (R). White (or the opaque part) of the picture shows; black (or the clear part) does not. Preview shows the result; upload bakes it into the mask players receive."))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock).Text(LOCTEXT("OverlayPictureAsset", "Picture"))
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SObjectPropertyEntryBox)
				.AllowedClass(UTexture2D::StaticClass())
				.AllowClear(true)
				.DisplayThumbnail(false)
				.ObjectPath_Lambda([this] { return Draft.OverlayPicture.IsValid() ? Draft.OverlayPicture->GetPathName() : FString(); })
				.OnObjectChanged_Lambda([this](const FAssetData& Asset)
				{
					Draft.OverlayPicture = Cast<UTexture2D>(Asset.GetAsset());
					Draft.bRightsDeclared = false;
					Package.Reset();
					RefreshOverlayPlacement();
				})
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[
					SNew(STextBlock).Text(LOCTEXT("OverlayPictureFile", "or a picture PNG"))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(0, 0, 6, 0)
				[
					SNew(SEditableTextBox)
					.HintText(LOCTEXT("OverlayFileHint", "Path to a .png file"))
					.Text_Lambda([this] { return FText::FromString(Draft.OverlayPictureFile); })
					.OnTextCommitted_Lambda([this](const FText& Text, ETextCommit::Type)
					{
						Draft.OverlayPictureFile = Text.ToString().TrimStartAndEnd().TrimQuotes();
						Draft.bRightsDeclared = false;
						Package.Reset();
						RefreshOverlayPlacement();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("OverlayBrowse", "Browse..."))
					.OnClicked_Lambda([this] { BrowseOverlayPicture(); return FReply::Handled(); })
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(SBox).HeightOverride(360.f)
				[
					SAssignNew(OverlayPlaceViewport, SMantleOverlayPlaceViewport)
					.OnPlacementChanged_Lambda([this]
					{
						if (OverlayPlaceViewport.IsValid())
						{
							Draft.OverlayPlacement = OverlayPlaceViewport->GetPlacement();
						}
						Draft.bRightsDeclared = false;
						Package.Reset();
					})
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				Spin(LOCTEXT("OverlayPlaceWidth", "Width on the skin (cm)"), 0.5f, 100.f, 0.5f, &FMantleOverlayPlacement::WidthCm, PlacedOnly)
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				Spin(LOCTEXT("OverlayPlaceDepth", "How far it reaches in and out (cm)"), 0.5f, 50.f, 0.5f, &FMantleOverlayPlacement::DepthCm, PlacedOnly)
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				Spin(LOCTEXT("OverlayPlaceFalloff", "Fade at the reach's edge"), 0.f, 1.f, 0.05f, &FMantleOverlayPlacement::Falloff, PlacedOnly)
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				Spin(LOCTEXT("OverlayPlaceRelief", "Scar relief"), 0.f, 6.f, 0.1f, &FMantleOverlayPlacement::Relief, ScarPlacedOnly)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("OverlayPlaceReset", "Put it on the chest / face"))
					.OnClicked_Lambda([this] { if (OverlayPlaceViewport.IsValid()) { OverlayPlaceViewport->ResetPlacement(); } return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("OverlayPlaceFrame", "Frame"))
					.OnClicked_Lambda([this] { if (OverlayPlaceViewport.IsValid()) { OverlayPlaceViewport->Frame(); } return FReply::Handled(); })
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor(FLinearColor::Red))
				.Visibility_Lambda([this] { return OverlayPlaceStatus.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				.Text_Lambda([this] { return FText::FromString(OverlayPlaceStatus); })
			]
		];
}

void SMantleCosmeticPanel::BrowseOverlayPicture()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		return;
	}
	const void* ParentWindow = FSlateApplication::IsInitialized()
		? FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr)
		: nullptr;
	TArray<FString> Files;
	if (!DesktopPlatform->OpenFileDialog(ParentWindow, TEXT("Choose the picture to place"),
		Draft.OverlayPictureFile.IsEmpty() ? FString() : FPaths::GetPath(Draft.OverlayPictureFile), FString(), TEXT("PNG image (*.png)|*.png"),
		EFileDialogFlags::None, Files) || Files.IsEmpty())
	{
		return;
	}
	Draft.OverlayPictureFile = FPaths::ConvertRelativePathToFull(Files[0]);
	if (Draft.Name.IsEmpty())
	{
		Draft.Name = FPaths::GetBaseFilename(Draft.OverlayPictureFile);
	}
	Draft.bRightsDeclared = false;
	Package.Reset();
	RefreshOverlayPlacement();
}

void SMantleCosmeticPanel::RefreshOverlayPlacement()
{
	OverlayPlaceStatus.Reset();
	if (!Draft.bOverlayPlaced || !OverlayPlaceViewport.IsValid())
	{
		return;
	}
	// The skin to place on: the picked body or face, else Mantle's reference ones (as the export and the Preview choose).
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	USkeletalMesh* Mesh = FMantleOverlayChecks::IsFaceSlot(Draft.Slot) ? OverlayPreviewFace.Get() : OverlayPreviewBody.Get();
	if (!Mesh)
	{
		Mesh = FMantleOverlayBake::DefaultMesh(FMantleOverlayChecks::IsFaceSlot(Draft.Slot), Contract);
	}
	Draft.OverlayPlaceMesh = Mesh;
	FString Error;
	const bool bHadMesh = OverlayPlaceViewport->SetMesh(Mesh, Error);
	if (!bHadMesh)
	{
		OverlayPlaceStatus = Error;
	}
	// The picture's shape, for the outline.
	Draft.OverlayPlaceAspect = 1.f;
	if (UTexture2D* Picture = Draft.OverlayPicture.Get())
	{
		if (Picture->GetSizeX() > 0)
		{
			Draft.OverlayPlaceAspect = static_cast<float>(Picture->GetSizeY()) / static_cast<float>(Picture->GetSizeX());
		}
	}
	else if (!Draft.OverlayPictureFile.IsEmpty())
	{
		TArray<FColor> Pixels;
		FIntPoint Size = FIntPoint::ZeroValue;
		FString ReadError;
		if (FMantleOverlayChecks::ReadPngFile(Draft.OverlayPictureFile, Pixels, Size, ReadError) && Size.X > 0)
		{
			Draft.OverlayPlaceAspect = static_cast<float>(Size.Y) / static_cast<float>(Size.X);
		}
		else
		{
			OverlayPlaceStatus = ReadError;
		}
	}
	OverlayPlaceViewport->SetPlacement(Draft.OverlayPlacement, Draft.OverlayPlaceAspect);
	if (bHadMesh && !bOverlayPlaceStarted)
	{
		// First time on this skin: start on the chest (or the face), so there is something to see.
		bOverlayPlaceStarted = true;
		OverlayPlaceViewport->ResetPlacement();
	}
}

void SMantleCosmeticPanel::SaveOverlayGuide()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		return;
	}
	const bool bFace = FMantleOverlayChecks::IsFaceSlot(Draft.Slot);
	const void* ParentWindow = FSlateApplication::IsInitialized()
		? FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr)
		: nullptr;
	TArray<FString> Files;
	if (!DesktopPlatform->SaveFileDialog(ParentWindow, TEXT("Save the painting guide"), FString(), FMantleOverlayGuide::DefaultFileName(Draft.Slot),
		TEXT("PNG image (*.png)|*.png"), EFileDialogFlags::None, Files) || Files.IsEmpty())
	{
		return;
	}
	FString Error;
	bOverlayGuideFailed = !FMantleOverlayGuide::Write(bFace, Files[0], Error);
	OverlayGuideStatus = bOverlayGuideFailed ? Error : FString::Printf(TEXT("Saved %s."), *FPaths::ConvertRelativePathToFull(Files[0]));
}

void SMantleCosmeticPanel::BrowseOverlayPng(const bool bNormal)
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		return;
	}
	const void* ParentWindow = FSlateApplication::IsInitialized()
		? FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr)
		: nullptr;
	const FString& Current = bNormal ? Draft.OverlayNormalFile : Draft.OverlayMaskFile;
	TArray<FString> Files;
	if (!DesktopPlatform->OpenFileDialog(ParentWindow, bNormal ? TEXT("Choose the scar's normal map") : TEXT("Choose the overlay's mask"),
		Current.IsEmpty() ? FString() : FPaths::GetPath(Current), FString(), TEXT("PNG image (*.png)|*.png"), EFileDialogFlags::None, Files)
		|| Files.IsEmpty())
	{
		return;
	}
	const FString File = FPaths::ConvertRelativePathToFull(Files[0]);
	if (bNormal)
	{
		Draft.OverlayNormalFile = File;
	}
	else
	{
		Draft.OverlayMaskFile = File;
		if (Draft.Name.IsEmpty())
		{
			Draft.Name = FPaths::GetBaseFilename(File);
		}
	}
	// The rights declaration is about this content; a new image needs its own tick (#614).
	Draft.bRightsDeclared = false;
	Package.Reset();
}

FReply SMantleCosmeticPanel::OnPreviewOverlay()
{
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!Contract || !PreviewViewport.IsValid())
	{
		return FReply::Handled();
	}
	const auto Refuse = [this](const FString& Why)
	{
		OverlayPreviewStatus = Why;
		bOverlayPreviewFailed = true;
		UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_PREVIEW overlay refused: %s"), *Why);
		return FReply::Handled();
	};
	if (PreviewSession.IsValid())
	{
		// The garment preview gives the viewport way (its Idle view clears the stage first).
		PreviewSession->Cancel();
	}

	// The preview shows what would upload: the export's own checks, and its exact mask and normal bytes.
	const FMantleCosmeticPackage Built = FMantleCosmeticExport::BuildCore(Draft, *Contract);
	if (Built.HasErrors())
	{
		ShowIssues(Built.Issues, Built.Notes);
		return Refuse(TEXT("The overlay does not pass the checks yet; fix the problems listed under the checks, then Preview."));
	}
	FMantleOverlayWorn Worn;
	FString Refusal;
	if (!FMantleOverlayWorn::Make(Built, Draft.Slot, *Contract, OverlayPreviewBody.Get(), OverlayPreviewFace.Get(), Worn, Refusal))
	{
		return Refuse(Refusal);
	}
	const bool bFace = Worn.bFace;
	USkeletalMesh* Body = Worn.Body;
	USkeletalMesh* Face = Worn.Face;
	USkeletalMesh* Target = bFace ? Face : Body;
	const TMap<int32, UMaterialInterface*>& BodyMaterials = Worn.BodyMaterials;
	const TMap<int32, UMaterialInterface*>& FaceMaterials = Worn.FaceMaterials;
	const FString Note = Worn.Note;
	// The preview viewport holds the instance through its components; the worn set's own references go with this function.

	PreviewInputError.Reset();
	AvatarPreviewStatus.Reset();
	bAvatarPreviewFailed = false;
	PreviewViewport->ShowOverlay(Body, Face, BodyMaterials, FaceMaterials, bFace, Body->GetPathName() + (Face ? Face->GetPathName() : FString()) + Draft.Slot);
	const bool bDrawn = Worn.bDrawn;
	OverlayPreviewStatus = FString::Printf(TEXT("%s on %s.%s"), *Draft.Slot, *Target->GetName(), *Note);
	bOverlayPreviewFailed = !bDrawn;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_COSMETIC_PREVIEW overlay slot=%s target=%s drawn=%d"), *Draft.Slot, *Target->GetPathName(), bDrawn ? 1 : 0);
	return FReply::Handled();
}

FText SMantleCosmeticPanel::GetPreviewStatusText() const
{
	if (Draft.Class == EMantleCosmeticClass::Avatar)
	{
		return AvatarPreviewStatus.IsEmpty()
			? LOCTEXT("AvatarPreviewIdle", "Choose the avatar's body, then Preview to see it posed the way players' clients pose it.")
			: FText::FromString(AvatarPreviewStatus);
	}
	if (Draft.Class == EMantleCosmeticClass::Overlay)
	{
		return OverlayPreviewStatus.IsEmpty()
			? LOCTEXT("OverlayPreviewIdle", "Choose the mask, then Preview to see it on a converted MetaHuman body or face.")
			: FText::FromString(OverlayPreviewStatus);
	}
	if (!PreviewInputError.IsEmpty())
	{
		return FText::FromString(PreviewInputError);
	}
	if (PreviewSession.IsValid() && PreviewSession->GetView().State != EMantlePreviewState::Idle)
	{
		return FText::FromString(PreviewSession->GetView().Status);
	}
	if (Draft.Class == EMantleCosmeticClass::Accessory) return LOCTEXT("AccessoryPreviewIdle", "Choose the accessory mesh, then Preview it on a MetaHuman. Saved placement follows this mesh and slot.");
	return LOCTEXT("PreviewIdle", "Choose the garment and the body it was made on, then Preview.");
}

void SMantleCosmeticPanel::ApplyPreviewAnimation()
{
	if (PreviewViewport.IsValid())
	{
		PreviewViewport->SetAnimation(bPreviewAnimate ? PreviewAnimation.Get() : nullptr);
	}
}

// ---- Convert (#610) -----------------------------------------------------------------------------------

TArray<EMantleAvatarRoute> FMantleConvertSection::Routes()
{
	TArray<EMantleAvatarRoute> Offered = { EMantleAvatarRoute::Convert };
	if (UMantleSettings::AllowsAsIsAvatars())
	{
		Offered.Add(EMantleAvatarRoute::AsIs);
	}
	return Offered;
}

bool FMantleConvertSection::ShowsRoutePicker()
{
	return Routes().Num() > 1;
}

EMantleAvatarRoute FMantleConvertSection::EffectiveRoute(const EMantleAvatarRoute Picked)
{
	return Routes().Contains(Picked) ? Picked : EMantleAvatarRoute::Convert;
}

FString FMantleConvertSection::CloudNote(TConstArrayView<FMantleConvertFix> Fixes)
{
	const bool bCloud = Fixes.ContainsByPredicate([](const FMantleConvertFix& Fix)
	{
		return IsCloudStep(Fix.FixerId) && Fix.bCanFix && Fix.bEnabled;
	});
	return bCloud ? FString(FMantleMetaHumanBuild::ConsentLine) : FString();
}

bool FMantleConvertSection::IsCloudStep(const FName FixerId)
{
	return FixerId == MantleAvatarFixerIds::MHBuild || FixerId == MantleAvatarFixerIds::MHSource;
}

FString FMantleConvertSection::ActionText(const FMantleConvertFix& Fix)
{
	if (!IsCloudStep(Fix.FixerId))
	{
		return Fix.Action;
	}
	FString Text = Fix.Action.Replace(FMantleMetaHumanBuild::ConsentLine, TEXT(""), ESearchCase::CaseSensitive);
	Text.ReplaceInline(TEXT("  "), TEXT(" "), ESearchCase::CaseSensitive);
	return Text.TrimStartAndEnd();
}

bool FMantleConvertSection::CanConvert(TConstArrayView<FMantleConvertFix> Fixes)
{
	// The converter's own rule (a bare-skin line counts only while the body is kept).
	if (Fixes.ContainsByPredicate([Fixes](const FMantleConvertFix& Fix) { return Fix.bCanFix && FMantleAvatarStripFixer::IsUnconfirmed(Fixes, Fix); }))
	{
		return false;
	}
	// #770: with the face on a MetaHuman body, one body (Women's or Men's) must be picked.
	return !Fixes.ContainsByPredicate([Fixes](const FMantleConvertFix& Fix)
	{
		return Fix.Kind == FMantleAvatarStripFixer::KindFaceOnly && !FMantleAvatarStripFixer::MissingBodyChoice(Fixes, Fix.PartIndex).IsEmpty();
	});
}

void FMantleConvertSection::PickBody(TArray<FMantleConvertFix>& Fixes, const int32 PartIndex, const bool bFaceOnBody, const TOptional<bool> Feminine)
{
	for (FMantleConvertFix& Fix : Fixes)
	{
		if (Fix.FixerId != MantleAvatarFixerIds::Strip || Fix.PartIndex != PartIndex)
		{
			continue;
		}
		if (Fix.Kind == FMantleAvatarStripFixer::KindFaceOnly)
		{
			Fix.bEnabled = bFaceOnBody;
		}
		else if (Feminine.IsSet() && (Fix.Kind == FMantleAvatarStripFixer::KindFeminineBody || Fix.Kind == FMantleAvatarStripFixer::KindMasculineBody))
		{
			Fix.bEnabled = (Fix.Kind == FMantleAvatarStripFixer::KindFeminineBody) == Feminine.GetValue();
		}
	}
}

void FMantleConvertSection::KeptPartsChanged(TArray<FMantleConvertFix>& Fixes, const int32 PartIndex)
{
	for (FMantleConvertFix& Fix : Fixes)
	{
		if (Fix.FixerId == MantleAvatarFixerIds::Strip && Fix.PartIndex == PartIndex && Fix.Kind == FMantleAvatarStripFixer::KindBareSkin)
		{
			Fix.bEnabled = false;
		}
	}
}

FMantleAvatarSight FMantleConvertSection::SeeKind(const UObject* Selection)
{
	FMantleAvatarSight Sight;
	if (!Selection)
	{
		return Sight;
	}
	Sight.Name = Selection->GetName();
	if (const USkeletalMesh* Mesh = Cast<USkeletalMesh>(Selection))
	{
		Sight.Kind = FMantleAvatarSight::EKind::SkeletalMesh;
		Sight.Bones = Mesh->GetRefSkeleton().GetRawBoneNum();
		// One bone, or no usable weights (FMantleUnriggedBody::HasNoUsableWeights), as MakeContext reads it.
		Sight.bUnrigged = FMantleUnriggedAsIs::HasNoRig(Mesh);
	}
	else if (Selection->IsA<UStaticMesh>())
	{
		Sight.Kind = FMantleAvatarSight::EKind::StaticMesh;
	}
	else if (Selection->IsA<UMetaHumanCharacter>())
	{
		Sight.Kind = FMantleAvatarSight::EKind::CreatorCharacter;
	}
	else if (Selection->IsA<UBlueprint>())
	{
		Sight.Kind = FMantleAvatarSight::EKind::Blueprint;
	}
	else
	{
		Sight.Name.Reset();
	}
	// Chunk 7: a model with no rig can go As is, skinned to Epic's MetaHuman body skeleton (not in an editor without a renderer).
	Sight.bAutoRigs = FMantleUnriggedAsIs::CanAutoRig(Selection, Sight.AutoRigNote);
	return Sight;
}

void FMantleConvertSection::SeeContext(const FMantleConvertContext& Context, TConstArrayView<FMantleConvertFix> Fixes, FMantleAvatarSight& Sight)
{
	Sight.bLooked = true;
	// The As-is route never marks a part unrigged: keep what the pick (or an earlier Convert look) saw.
	if (Context.Route == EMantleAvatarRoute::Convert && Context.Parts.IsValidIndex(0) && Sight.Kind == FMantleAvatarSight::EKind::SkeletalMesh)
	{
		Sight.bUnrigged = Context.Parts[0].bUnrigged;
	}
	// Strip ticks the face-only line by default when the body reads as covered (IsCovered). An assembled MetaHuman's line
	// means something else (MetaHuman Creator can't fit its body), so a Blueprint never reads as covered here.
	Sight.bCovered = Sight.Kind != FMantleAvatarSight::EKind::Blueprint && Fixes.ContainsByPredicate([](const FMantleConvertFix& Fix)
	{
		return Fix.FixerId == MantleAvatarFixerIds::Strip && Fix.PartIndex == 0 && Fix.Kind == FMantleAvatarStripFixer::KindFaceOnly && Fix.bEnabled;
	});
}

void FMantleConvertSection::SeeAssembly(const FMantleAssemblyScan& Scan, FMantleAvatarSight& Sight)
{
	TSet<const USkeletalMesh*> Meshes;
	for (const FMantleAssemblyMesh& Garment : Scan.Garments)
	{
		Meshes.Add(Garment.Mesh.Get());
	}
	Sight.Clothing = Meshes.Num();
	Sight.Hair = Scan.Hair.Num();
}

FString FMantleConvertSection::WhatISee(const FMantleAvatarSight& Sight)
{
	FString Seen;
	switch (Sight.Kind)
	{
	case FMantleAvatarSight::EKind::SkeletalMesh:
		if (!Sight.bUnrigged)
		{
			Seen = FString::Printf(TEXT("Skeletal mesh, %d bones%s."), Sight.Bones, Sight.bLooked ? TEXT(", rigged") : TEXT(""));
		}
		else if (Sight.Bones <= 1)
		{
			Seen = TEXT("Skeletal mesh with one bone, no rig: we'll fit it to a MetaHuman body.");
		}
		else
		{
			// Chunk 4's (#161) case, in its words (FMantleUnriggedBody::NoUsableWeightsMessage).
			Seen = FString::Printf(TEXT("Skeletal mesh, %d bones. Skeleton found, but no usable weights: we'll fit it to a MetaHuman body."), Sight.Bones);
		}
		break;
	case FMantleAvatarSight::EKind::StaticMesh:
		Seen = TEXT("Static mesh, no skeleton.");
		break;
	case FMantleAvatarSight::EKind::CreatorCharacter:
		Seen = TEXT("MetaHuman Creator character.");
		break;
	case FMantleAvatarSight::EKind::Blueprint:
		if (!Sight.bLooked)
		{
			Seen = TEXT("MetaHuman Blueprint.");
			break;
		}
		{
			TArray<FString> Pieces = { TEXT("body") };
			if (Sight.Clothing > 0)
			{
				Pieces.Add(FString::Printf(TEXT("%d clothing piece%s"), Sight.Clothing, Sight.Clothing == 1 ? TEXT("") : TEXT("s")));
			}
			if (Sight.Hair > 0)
			{
				Pieces.Add(TEXT("hair"));
			}
			Seen = FString::Printf(TEXT("MetaHuman Blueprint: %s."), *FString::Join(Pieces, TEXT(", ")));
		}
		break;
	default:
		return FString();
	}
	if (Sight.bCovered)
	{
		Seen += TEXT(" The body is covered by clothing.");
	}
	return Sight.Name.IsEmpty() ? Seen : FString::Printf(TEXT("%s: %s"), *Sight.Name, *Seen);
}

FString FMantleConvertSection::AsIsGreyedWhy(const FMantleAvatarSight& Sight)
{
	if (Sight.Kind == FMantleAvatarSight::EKind::CreatorCharacter)
	{
		return TEXT("it's already a MetaHuman");
	}
	const bool bNoRig = Sight.Kind == FMantleAvatarSight::EKind::StaticMesh || (Sight.Kind == FMantleAvatarSight::EKind::SkeletalMesh && Sight.bUnrigged);
	if (!bNoRig || Sight.bAutoRigs)
	{
		return FString();
	}
	return Sight.AutoRigNote.IsEmpty() ? FString(TEXT("no rig to keep")) : Sight.AutoRigNote;
}

EMantleAvatarRoute FMantleConvertSection::DoorRoute(const EMantleAvatarRoute Picked, const FMantleAvatarSight& Sight)
{
	const EMantleAvatarRoute Route = EffectiveRoute(Picked);
	return Route == EMantleAvatarRoute::AsIs && !AsIsGreyedWhy(Sight).IsEmpty() ? EMantleAvatarRoute::Convert : Route;
}

TArray<FMantlePublishRow> FMantlePublishList::Make(const FMantleConvertContext& Context, const FMantlePackReview* Review, TConstArrayView<FString> HairSlots)
{
	TArray<FMantlePublishRow> Rows;
	auto AddPart = [&Rows](const FMantlePublishRow::EKind Kind, const int32 Index, const FMantleConvertPart& Part)
	{
		FMantlePublishRow& Row = Rows.AddDefaulted_GetRef();
		Row.Kind = Kind;
		Row.Index = Index;
		Row.Slot = Part.Slot;
		Row.Name = Part.Name;
		Row.bTicked = Part.bIncluded;
		if (!Part.Mesh.IsValid())
		{
			Row.Why = TEXT("Convert didn't make it");
			Row.bTicked = false;
		}
	};
	for (int32 Index = 0; Index < Context.Parts.Num(); ++Index)
	{
		AddPart(FMantlePublishRow::EKind::Part, Index, Context.Parts[Index]);
	}
	for (int32 Index = 0; Index < Context.Kept.Num(); ++Index)
	{
		AddPart(FMantlePublishRow::EKind::Kept, Index, Context.Kept[Index]);
	}
	// The pack's hair as its rows left it: a ticked hair row of its own (one folded into another piece goes with that one).
	for (int32 Piece = 0; Review && Piece < Review->Pieces.Num(); ++Piece)
	{
		const FMantlePackPiece& Each = Review->Pieces[Piece];
		if (!Each.bTicked || Each.PartOf != INDEX_NONE || !HairSlots.Contains(Each.Slot))
		{
			continue;
		}
		FMantlePublishRow& Row = Rows.AddDefaulted_GetRef();
		Row.Kind = FMantlePublishRow::EKind::PackHair;
		Row.Index = Piece;
		Row.Slot = Each.Slot;
		Row.Name = Each.Label;
		if (!Review->Meshes.IsValidIndex(Each.MeshIndex) || !Review->Meshes[Each.MeshIndex].IsValid())
		{
			Row.Why = TEXT("its mesh didn't load");
			Row.bTicked = false;
		}
	}
	for (int32 Index = 0; Index < Context.Garments.Num(); ++Index)
	{
		const FMantleConvertGarment& Garment = Context.Garments[Index];
		FMantlePublishRow& Row = Rows.AddDefaulted_GetRef();
		Row.Kind = FMantlePublishRow::EKind::Garment;
		Row.Index = Index;
		Row.Slot = Garment.Slot;
		Row.Name = Garment.Name;
		Row.bTicked = true;
	}
	Sync(Context, Rows);
	return Rows;
}

void FMantlePublishList::Sync(const FMantleConvertContext& Context, TArray<FMantlePublishRow>& Rows)
{
	for (FMantlePublishRow& Row : Rows)
	{
		const FMantleConvertGarment* Garment = Row.Kind == FMantlePublishRow::EKind::Garment && Context.Garments.IsValidIndex(Row.Index) ? &Context.Garments[Row.Index] : nullptr;
		if (!Garment || !Row.Why.IsEmpty())
		{
			continue;
		}
		// A piece that can't be an Outfit, or whose Outfit couldn't be made (high heels are found then), is greyed and unticked.
		Row.Why = !Garment->Why.IsEmpty() ? Garment->Why : Garment->Error.IsEmpty() ? FString() : FString::Printf(TEXT("Couldn't make the Outfit: %s"), *Garment->Error);
		Row.bTicked &= Row.Why.IsEmpty();
	}
}

int32 FMantlePublishList::Next(TConstArrayView<FMantlePublishRow> Rows, const int32 After)
{
	for (int32 Index = FMath::Max(After + 1, 0); Index < Rows.Num(); ++Index)
	{
		if (Rows[Index].bTicked && Rows[Index].Why.IsEmpty() && !Rows[Index].bUploaded)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

FString FMantlePublishList::SlotWord(const FString& Slot)
{
	if (Slot.Equals(TEXT("avatar"), ESearchCase::CaseSensitive))
	{
		return TEXT("Body");
	}
	FString Word = Slot.Replace(TEXT("_"), TEXT(" "));
	if (!Word.IsEmpty())
	{
		Word[0] = FChar::ToUpper(Word[0]);
	}
	return Word;
}

UObject* SMantleCosmeticPanel::GetConvertSelection() const
{
	return ConvertSource.IsValid() ? ConvertSource.Get() : Draft.Source.Get();
}

EMantleAvatarRoute SMantleCosmeticPanel::DoorRoute() const
{
	return FMantleConvertSection::DoorRoute(ConvertRoute, AvatarSight);
}

void SMantleCosmeticPanel::PickAvatar(UObject* Character)
{
	ConvertSource = Character;
	AvatarSight = FMantleConvertSection::SeeKind(Character);
	// Convert is the pre-selected choice for every character.
	ConvertRoute = EMantleAvatarRoute::Convert;
	SetSource(Cast<USkeletalMesh>(Character));
	ResetConvert();
}

void SMantleCosmeticPanel::ResetConvert()
{
	ConvertReview.Reset();
	ConvertContext.Reset();
	ConvertFixes.Reset();
	ConvertResult.Reset();
	ConvertStatus.Reset();
	bConvertFailed = false;
	PublishRows.Reset();
	PublishThumbs.Reset();
	bPublishWalk = false;
	RefreshConvertRows();
}

TSharedRef<SWidget> SMantleCosmeticPanel::MakeConvertSection()
{
	using namespace MantleCosmeticPanelPrivate;
	// #625: a route button; As is shows only while As-is uploads are allowed, greyed with why for a character it can't take.
	// Picking another route forgets the fix list.
	auto RouteButton = [this](const EMantleAvatarRoute Route, const FText& Label, const FText& Tooltip)
	{
		return SNew(SCheckBox)
			.Style(&FAppStyle::Get(), "RadioButton")
			.ToolTipText(Tooltip)
			.IsEnabled_Lambda([this, Route] { return Route != EMantleAvatarRoute::AsIs || FMantleConvertSection::AsIsGreyedWhy(AvatarSight).IsEmpty(); })
			.IsChecked_Lambda([this, Route] { return DoorRoute() == Route ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this, Route](const ECheckBoxState State)
			{
				if (State == ECheckBoxState::Checked && ConvertRoute != Route)
				{
					ConvertRoute = Route;
					ResetConvert();
				}
			})
			[
				SNew(STextBlock).Text(Label)
			];
	};
	return SAssignNew(ConvertSectionBox, SVerticalBox)
		// What the plugin sees in the character picked under Avatar (PickAvatar); the pack's rows come under the question.
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Visibility_Lambda([this] { return AvatarSight.Kind == FMantleAvatarSight::EKind::None ? EVisibility::Collapsed : EVisibility::Visible; })
			.Text_Lambda([this] { return FText::FromString(FMantleConvertSection::WhatISee(AvatarSight)); })
		]
		// After Use: the converted body the upload takes, which the picker above doesn't show.
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Visibility_Lambda([this] { return Draft.Source.IsValid() && Draft.Source.Get() != GetConvertSelection() ? EVisibility::Visible : EVisibility::Collapsed; })
			.Text_Lambda([this]
			{
				return FText::Format(LOCTEXT("ConvertUploads", "Upload takes {0}."), FText::FromString(Draft.Source.IsValid() ? Draft.Source->GetName() : FString()));
			})
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
		[
			SNew(STextBlock).Text(LOCTEXT("ConvertQuestion", "How should it go up?"))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
			[
				RouteButton(EMantleAvatarRoute::Convert, LOCTEXT("ConvertRouteConvert", "Convert to MetaHuman"),
					LOCTEXT("ConvertRouteConvertTooltip", "The avatar becomes a MetaHuman: it wears clothing and takes the avatar options."))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
			[
				SNew(SBox)
				.Visibility_Lambda([] { return FMantleConvertSection::ShowsRoutePicker() ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					RouteButton(EMantleAvatarRoute::AsIs, LOCTEXT("ConvertRouteAsIs", "As is"),
						LOCTEXT("ConvertRouteAsIsTooltip", "What you see is what you get: a supported skeleton and Mantle's limits, no customisation, no clothing."))
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Visibility_Lambda([this]
				{
					return FMantleConvertSection::ShowsRoutePicker() && !FMantleConvertSection::AsIsGreyedWhy(AvatarSight).IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.Text_Lambda([this] { return FText::Format(LOCTEXT("ConvertAsIsGreyed", "({0})"), FText::FromString(FMantleConvertSection::AsIsGreyedWhy(AvatarSight))); })
			]
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(LOCTEXT("ConvertHelp", "Nothing of yours is changed: the plugin lists what it would do first, then writes new assets to a Converted folder next to it, and each part comes back here, ready to preview and upload."))
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.IsEnabled_Lambda([this] { return GetConvertSelection() != nullptr && ContractError.IsEmpty() && !IsRunning(); })
				.Text_Lambda([this] { return DoorRoute() == EMantleAvatarRoute::AsIs ? LOCTEXT("ConvertAnalyzeAsIs", "As is...") : LOCTEXT("ConvertAnalyze", "Convert..."); })
				.ToolTipText(LOCTEXT("ConvertAnalyzeTooltip", "List what the plugin would fix to make this character upload-ready. Nothing is written until you choose Convert."))
				.OnClicked(this, &SMantleCosmeticPanel::OnAnalyzeConvert)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Visibility_Lambda([this] { return ConvertStatus.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			.ColorAndOpacity_Lambda([this] { return bConvertFailed ? SeverityColour(EMantleCosmeticSeverity::Error) : FSlateColor::UseForeground(); })
			.Text_Lambda([this] { return FText::FromString(ConvertStatus); })
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
		[
			SAssignNew(ConvertBox, SVerticalBox)
		];
}

FReply SMantleCosmeticPanel::OnAnalyzeConvert()
{
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	UObject* Selection = GetConvertSelection();
	if (!Contract || !Selection)
	{
		return FReply::Handled();
	}
	if (AvatarSight.Kind == FMantleAvatarSight::EKind::None)
	{
		AvatarSight = FMantleConvertSection::SeeKind(Selection);
	}
	// #55: what the pack holds beside a character's skeletal mesh converted as an avatar, as rows the creator changes.
	ConvertReview.Reset();
	USkeletalMesh* Picked = Cast<USkeletalMesh>(Selection);
	// Not a MetaHuman Creator assembly (its folder scans as one, as MakeContext checks).
	const FString AssemblyFolder = Picked ? FMantleAvatarAssemblyFixer::AssemblyFolderOf(Picked) : FString();
	FMantleAssemblyScan AssemblyScan;
	FString ScanError;
	const bool bAssembly = !AssemblyFolder.IsEmpty() && FMantleAvatarAssemblyFixer::Scan(AssemblyFolder, AssemblyScan, ScanError);
	if (Picked && DoorRoute() == EMantleAvatarRoute::Convert && Picked->GetRefSkeleton().GetRawBoneNum() > 1 && !bAssembly)
	{
		FScopedSlowTask Progress(1.0f, LOCTEXT("ConvertScanning", "Looking at what's in the pack..."));
		Progress.MakeDialogDelayed(0.5f);
		Progress.EnterProgressFrame(1.0f);
		const TSharedRef<FMantlePackReview> Review = MakeShared<FMantlePackReview>(FMantleCharacterSplit::ReviewFor(*Picked));
		if (Review->HasRows() && Review->BodyMesh() != INDEX_NONE)
		{
			ConvertReview = Review;
		}
		else
		{
			UE_LOG(LogMantle, Log, TEXT("MANTLE_PACK %s: no rows (%s)"), *Picked->GetName(), Review->HasRows() ? TEXT("no bare body to offer") : TEXT("nothing beside it"));
		}
	}
	AnalyzeConvertBody();
	return FReply::Handled();
}

void SMantleCosmeticPanel::AnalyzeConvertBody()
{
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	UObject* Selection = GetConvertSelection();
	if (!Contract || !Selection)
	{
		return;
	}
	// The one door is under Avatar: whatever was picked converts for the avatar slot (a Blueprint as its whole assembly).
	FMantleConvertOptions Options;
	Options.Slot = TEXT("avatar");
	// #625: Convert unless As-is is allowed, picked and not greyed. The fix list never asks Epic (MHBuild's line says the
	// sign-in is checked when Convert runs).
	Options.Route = DoorRoute();
	// #55: the body the review's row chose, with the clothing its rows take.
	const int32 BodyMesh = ConvertReview.IsValid() ? ConvertReview->BodyMesh() : INDEX_NONE;
	if (ConvertReview.IsValid() && ConvertReview->Meshes.IsValidIndex(BodyMesh) && ConvertReview->Meshes[BodyMesh].IsValid())
	{
		Selection = ConvertReview->Meshes[BodyMesh].Get();
		Options.Review = ConvertReview;
	}
	ConvertResult.Reset();
	ConvertFixes.Reset();
	PublishRows.Reset();
	PublishThumbs.Reset();
	bPublishWalk = false;
	ConvertContext = MakeShared<FMantleConvertContext>();
	FString Error;
	{
		FScopedSlowTask Progress(1.0f, LOCTEXT("ConvertAnalyzing", "Looking at the character..."));
		Progress.MakeDialogDelayed(0.5f);
		Progress.EnterProgressFrame(1.0f);
		if (!FMantleAvatarConverter::MakeContext(Selection, Options, *Contract, *ConvertContext, Error))
		{
			ConvertContext.Reset();
			ConvertStatus = Error;
			bConvertFailed = true;
			RefreshConvertRows();
			UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT refused %s: %s"), *Selection->GetPathName(), *Error);
			return;
		}
		ConvertFixes = FMantleAvatarConverter::Analyze(*ConvertContext);
	}
	// The "what I see" line: what the converter found, before the creator changes a tick. With the pack's rows it looked at
	// the body row's mesh, not the one picked; the rows say which body that is.
	if (Selection == GetConvertSelection())
	{
		FMantleConvertSection::SeeContext(*ConvertContext, ConvertFixes, AvatarSight);
	}
	else
	{
		AvatarSight.bLooked = true;
	}
	if (AvatarSight.Kind == FMantleAvatarSight::EKind::Blueprint)
	{
		FMantleAssemblyScan Scan;
		FString ScanError;
		const FString Folder = FMantleAvatarAssemblyFixer::AssemblyFolderOf(GetConvertSelection());
		if (!Folder.IsEmpty() && FMantleAvatarAssemblyFixer::Scan(Folder, Scan, ScanError))
		{
			FMantleConvertSection::SeeAssembly(Scan, AvatarSight);
		}
	}
	bConvertFailed = false;
	int32 Fixable = 0;
	for (const FMantleConvertFix& Fix : ConvertFixes)
	{
		Fixable += Fix.bCanFix ? 1 : 0;
	}
	ConvertStatus = ConvertFixes.Num() == 0
		? FString::Printf(TEXT("Nothing to fix in %d part(s). Choose Convert to take them to the panel."), ConvertContext->Parts.Num())
		: FString::Printf(TEXT("%d thing(s) to fix across %d part(s), %d of them here. Untick a fix to leave its problem for the checks. Nothing is written until you choose Convert; it writes to %s."),
			ConvertFixes.Num(), ConvertContext->Parts.Num(), Fixable, *ConvertContext->OutputFolder);
	for (const FString& Key : FMantleAvatarConverter::FixKeys(ConvertFixes))
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT fix %s"), *Key);
	}
	RefreshConvertRows();
}

FReply SMantleCosmeticPanel::OnRunConvert()
{
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!Contract || !ConvertContext.IsValid())
	{
		return FReply::Handled();
	}
	FMantleConvertOptions Options;
	{
		// Convert blocks until it is done (the MetaHuman steps wait on Epic's cloud), so no
		// Cancel: the dialog says which step runs (the converter's own progress frames, one per step).
		FScopedSlowTask Progress(1.0f, LOCTEXT("ConvertRunning", "Converting. Making a MetaHuman can take several minutes; the editor waits until it is done."));
		Progress.MakeDialog();
		Progress.EnterProgressFrame(1.0f);
		// #55: the clothing as the rows stand now (the creator may have changed them since the fixes were listed).
		if (ConvertReview.IsValid() && ConvertContext->Parts.IsValidIndex(0) && ConvertContext->Parts[0].Original.IsValid())
		{
			FMantleGarmentOutfit::GarmentsFromReview(*ConvertContext->Parts[0].Original.Get(), *ConvertReview, ConvertContext->Garments);
		}
		ConvertResult = FMantleAvatarConverter::Convert(*ConvertContext, ConvertFixes, Options, *Contract);
	}
	int32 Ready = 0;
	for (const FMantleConvertPartCheck& Check : ConvertResult->Checks)
	{
		Ready += Check.bCanUpload ? 1 : 0;
	}
	bConvertFailed = ConvertResult->Failed.Num() > 0;
	ConvertStatus = FString::Printf(TEXT("Converted into %s: %d fix(es) applied, %d failed. %d of %d part(s) are ready to upload. Below is what to publish: untick what you don't want, rename anything, then publish the ticked items."),
		*ConvertContext->OutputFolder, ConvertResult->Applied.Num(), ConvertResult->Failed.Num(), Ready, ConvertResult->Checks.Num());
	MakePublishRows();
	RefreshConvertRows();
	return FReply::Handled();
}

void SMantleCosmeticPanel::RefreshConvertRows()
{
	using namespace MantleCosmeticPanelPrivate;
	if (!ConvertBox.IsValid())
	{
		return;
	}
	ConvertBox->ClearChildren();
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	if (!ConvertContext.IsValid() || !Contract)
	{
		// #55: a body that can't convert keeps the rows, so another can be picked.
		AddPackReviewRows();
		return;
	}
	auto PartLabel = [this, Contract](int32 PartIndex)
	{
		return ConvertContext->Parts.IsValidIndex(PartIndex)
			? FMantleAvatarConverter::DescribePart(ConvertContext->Parts[PartIndex], *Contract) : FString(TEXT("The character"));
	};
	auto AddLine = [this](const FString& Text, const FSlateColor& Colour)
	{
		ConvertBox->AddSlot().AutoHeight().Padding(0, 2)
		[
			SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(Colour).Text(FText::FromString(Text))
		];
	};

	if (!ConvertResult.IsSet())
	{
		// #55: what the pack holds, as rows the creator changes; then the parts, then the fix list: one checkbox per fix, what is
		// wrong and what it does; a line it can't fix greyed with why.
		AddPackReviewRows();
		TArray<FString> Parts;
		for (int32 Index = 0; Index < ConvertContext->Parts.Num(); ++Index)
		{
			Parts.Add(PartLabel(Index) + (ConvertContext->Parts[Index].bIncluded ? TEXT("") : TEXT(" (left out unless you tick it below)")));
		}
		AddLine(FString::Printf(TEXT("Parts: %s."), *FString::Join(Parts, TEXT("; "))), FSlateColor::UseSubduedForeground());
		// A third-party avatar's separate clothing: offered after Convert, as Outfits on the MetaHuman it makes.
		TArray<FString> Clothing;
		for (const FMantleConvertGarment& Garment : ConvertContext->Garments)
		{
			Clothing.Add(Garment.Why.IsEmpty() ? FString::Printf(TEXT("%s (%s)"), *Garment.Name, *Garment.Slot) : FString::Printf(TEXT("%s (can't: %s)"), *Garment.Name, *Garment.Why));
		}
		if (Clothing.Num() > 0 && !ConvertReview.IsValid())
		{
			AddLine(FString::Printf(TEXT("Clothing: %s. After Convert, each piece is listed to publish, as a MetaHuman Outfit on this character's MetaHuman."),
				*FString::Join(Clothing, TEXT("; "))), FSlateColor::UseSubduedForeground());
		}
		AddBodyRows();
		for (int32 Index = 0; Index < ConvertFixes.Num(); ++Index)
		{
			const FMantleConvertFix& Fix = ConvertFixes[Index];
			if (FMantleAvatarStripFixer::IsBodyRowLine(Fix))
			{
				continue; // In the body row above.
			}
			const bool bCanFix = Fix.bCanFix;
			ConvertBox->AddSlot().AutoHeight().Padding(0, 4)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0, 0, 6, 0)
				[
					SNew(SCheckBox)
					.IsEnabled(bCanFix)
					.ToolTipText(!bCanFix ? LOCTEXT("ConvertFixCant", "The plugin can't fix this here; the line says why.")
						: Fix.bNeedsConfirm ? LOCTEXT("ConvertFixConfirm", "Tick to confirm. Convert stays off until you do.")
						: LOCTEXT("ConvertFixTip", "Untick to leave this problem for the checks."))
					.IsChecked_Lambda([this, Index]
					{
						return ConvertFixes.IsValidIndex(Index) && ConvertFixes[Index].bEnabled && ConvertFixes[Index].bCanFix
							? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([this, Index](ECheckBoxState State)
					{
						if (ConvertFixes.IsValidIndex(Index))
						{
							ConvertFixes[Index].bEnabled = State == ECheckBoxState::Checked;
							// Chunk 5: a changed slot or piece line changes the kept parts, so the bare-skin tick is the creator's again.
							if (ConvertFixes[Index].FixerId == MantleAvatarFixerIds::Strip)
							{
								FMantleConvertSection::KeptPartsChanged(ConvertFixes, ConvertFixes[Index].PartIndex);
							}
						}
					})
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.ColorAndOpacity(bCanFix ? FSlateColor::UseForeground() : FSlateColor::UseSubduedForeground())
						.Text(FText::FromString(FString::Printf(TEXT("%s: %s"), *PartLabel(Fix.PartIndex), *Fix.Problem)))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Text(FText::FromString(FMantleConvertSection::ActionText(Fix)))
					]
				]
			];
		}
		// #625: the cloud note, once, by Convert, while a ticked line builds with Epic's cloud (a note: no tick box).
		ConvertBox->AddSlot().AutoHeight().Padding(0, 8, 0, 0)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Visibility_Lambda([this] { return FMantleConvertSection::CloudNote(ConvertFixes).IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			.Text_Lambda([this] { return FText::FromString(FMantleConvertSection::CloudNote(ConvertFixes)); })
		];
		ConvertBox->AddSlot().AutoHeight().Padding(0, 8, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.ButtonStyle(&FAppStyle::Get(), "PrimaryButton")
				// #625: a line that needs confirming (the kept parts are bare skin) keeps Convert off until it is ticked.
				.IsEnabled_Lambda([this]
				{
					return ConvertContext.IsValid() && !IsRunning() && FMantleConvertSection::CanConvert(ConvertFixes);
				})
				.Text(LOCTEXT("ConvertRun", "Convert"))
				.ToolTipText(LOCTEXT("ConvertRunTooltip", "Apply the ticked fixes into new assets in the Converted folder (an earlier conversion of this character there is replaced), then check each part. Lines you must confirm keep it off until you tick them."))
				.OnClicked(this, &SMantleCosmeticPanel::OnRunConvert)
			]
		];
		return;
	}

	// After Convert: what happened, then each part with its checks and a button that takes it to the panel.
	for (const FMantleConvertOutcome& Failed : ConvertResult->Failed)
	{
		AddLine(FString::Printf(TEXT("Not fixed: %s: %s (%s)"), *PartLabel(Failed.Fix.PartIndex), *Failed.Fix.Problem, *Failed.Reason),
			SeverityColour(EMantleCosmeticSeverity::Error));
	}
	for (const FString& Note : ConvertContext->Notes)
	{
		AddLine(Note, SeverityColour(EMantleCosmeticSeverity::Info));
	}
	// Chunk 6: one "What to publish" list (the body, each piece of clothing, the hair), each row ticked, named and drawn.
	AddPublishRows();
	ConvertBox->AddSlot().AutoHeight().Padding(0, 8, 0, 0)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
		[
			SNew(SButton)
			.ButtonStyle(&FAppStyle::Get(), "PrimaryButton")
			.IsEnabled_Lambda([this] { return !IsRunning() && FMantlePublishList::Next(PublishRows, INDEX_NONE) != INDEX_NONE; })
			.Text(LOCTEXT("PublishTicked", "Publish the ticked items"))
			.ToolTipText(LOCTEXT("PublishTickedTooltip", "Opens the first ticked item in the panel. Check and upload it as usual; when its upload is done, the next ticked item opens. Unticked items are never uploaded."))
			.OnClicked_Lambda([this]
			{
				bPublishWalk = true;
				OpenPublishRow(FMantlePublishList::Next(PublishRows, INDEX_NONE));
				return FReply::Handled();
			})
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SButton)
			.IsEnabled_Lambda([this] { return GetConvertSelection() != nullptr && !IsRunning(); })
			.Text(LOCTEXT("ConvertAgain", "Look again"))
			.ToolTipText(LOCTEXT("ConvertAgainTooltip", "List the fixes for the character again, for example to convert it with other fixes ticked."))
			.OnClicked(this, &SMantleCosmeticPanel::OnAnalyzeConvert)
		]
	];
}

void SMantleCosmeticPanel::RefreshConvertRowsLater(const bool bAnalyze)
{
	RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSPLambda(this, [this, bAnalyze](double, float)
	{
		if (bAnalyze)
		{
			AnalyzeConvertBody();
		}
		else
		{
			RefreshConvertRows();
		}
		return EActiveTimerReturnType::Stop;
	}));
}

void SMantleCosmeticPanel::AddBodyRows()
{
	using namespace MantleCosmeticPanelPrivate;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	auto Find = [this](const int32 PartIndex, const FName Kind)
	{
		return ConvertFixes.IndexOfByPredicate([PartIndex, Kind](const FMantleConvertFix& Fix)
		{
			return Fix.FixerId == MantleAvatarFixerIds::Strip && Fix.PartIndex == PartIndex && Fix.Kind == Kind;
		});
	};
	for (int32 PartIndex = 0; ConvertContext.IsValid() && PartIndex < ConvertContext->Parts.Num(); ++PartIndex)
	{
		const int32 FaceIndex = Find(PartIndex, FMantleAvatarStripFixer::KindFaceOnly);
		const int32 BareIndex = Find(PartIndex, FMantleAvatarStripFixer::KindBareSkin);
		if (FaceIndex == INDEX_NONE || BareIndex == INDEX_NONE)
		{
			continue;
		}
		// The row's lines by their kind each time (the list is rebuilt on every look).
		auto Line = [this, Find, PartIndex](const FName Kind) -> FMantleConvertFix*
		{
			const int32 Index = Find(PartIndex, Kind);
			return ConvertFixes.IsValidIndex(Index) ? &ConvertFixes[Index] : nullptr;
		};
		auto IsFace = [Line] { const FMantleConvertFix* Face = Line(FMantleAvatarStripFixer::KindFaceOnly); return Face && Face->bEnabled; };
		const bool bCanKeep = ConvertFixes[BareIndex].bCanFix;
		const FString Label = ConvertContext->Parts.Num() > 1 && Contract
			? FMantleAvatarConverter::DescribePart(ConvertContext->Parts[PartIndex], *Contract) + TEXT(", body:") : FString(TEXT("Body:"));
		auto Radio = [](const FText& Text, TFunction<bool()> IsOn, TFunction<void()> Pick, const bool bEnabled)
		{
			return SNew(SCheckBox)
				.Style(&FAppStyle::Get(), "RadioButton")
				.IsEnabled(bEnabled)
				.IsChecked_Lambda([IsOn] { return IsOn() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([Pick](const ECheckBoxState State) { if (State == ECheckBoxState::Checked) { Pick(); } })
				[
					SNew(STextBlock).Text(Text)
				];
		};
		auto IsBody = [Line](const FName Kind) { const FMantleConvertFix* Body = Line(Kind); return Body && Body->bEnabled; };
		ConvertBox->AddSlot().AutoHeight().Padding(0, 4, 0, 6)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SWrapBox)
				.UseAllottedSize(true)
				.InnerSlotPadding(FVector2D(12, 4))
				+ SWrapBox::Slot().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Font(FAppStyle::GetFontStyle("NormalFontBold")).Text(FText::FromString(Label))
				]
				+ SWrapBox::Slot().VAlign(VAlign_Center)
				[
					Radio(LOCTEXT("BodyRowKeep", "Keep this character's body"), [IsFace] { return !IsFace(); },
						[this, PartIndex] { FMantleConvertSection::PickBody(ConvertFixes, PartIndex, false); }, bCanKeep)
				]
				+ SWrapBox::Slot().VAlign(VAlign_Center)
				[
					Radio(LOCTEXT("BodyRowFace", "Put the face on a MetaHuman body"), IsFace,
						[this, PartIndex] { FMantleConvertSection::PickBody(ConvertFixes, PartIndex, true); }, true)
				]
				+ SWrapBox::Slot().VAlign(VAlign_Center)
				[
					SNew(SHorizontalBox)
					.IsEnabled_Lambda([IsFace] { return IsFace(); })
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
					[
						Radio(LOCTEXT("BodyRowWomens", "Women's"), [IsBody] { return IsBody(FMantleAvatarStripFixer::KindFeminineBody); },
							[this, PartIndex] { FMantleConvertSection::PickBody(ConvertFixes, PartIndex, true, true); }, true)
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						Radio(LOCTEXT("BodyRowMens", "Men's"), [IsBody] { return IsBody(FMantleAvatarStripFixer::KindMasculineBody); },
							[this, PartIndex] { FMantleConvertSection::PickBody(ConvertFixes, PartIndex, true, false); }, true)
					]
				]
			]
			// Why it is pre-picked.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(FText::Format(LOCTEXT("BodyRowWhy", "Picked because {0}."), FText::FromString(ConvertFixes[FaceIndex].Problem)))
			]
			// With the face on a MetaHuman body: the pick still to make.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(SeverityColour(EMantleCosmeticSeverity::Warning))
				.Visibility_Lambda([this, PartIndex] { return FMantleAvatarStripFixer::MissingBodyChoice(ConvertFixes, PartIndex).IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
				.Text_Lambda([this, PartIndex] { return FText::FromString(FMantleAvatarStripFixer::MissingBodyChoice(ConvertFixes, PartIndex) + TEXT(".")); })
			]
			// Keeping the body: the one bare-skin tick, ticked only when a bare body was clearly found.
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(SHorizontalBox)
				.Visibility_Lambda([IsFace, bCanKeep] { return bCanKeep && !IsFace() ? EVisibility::Visible : EVisibility::Collapsed; })
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(0, 0, 6, 0)
				[
					SNew(SCheckBox)
					.ToolTipText(LOCTEXT("ConvertFixConfirm", "Tick to confirm. Convert stays off until you do."))
					.IsChecked_Lambda([Line]
					{
						const FMantleConvertFix* Bare = Line(FMantleAvatarStripFixer::KindBareSkin);
						return Bare && Bare->bEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([Line](const ECheckBoxState State)
					{
						if (FMantleConvertFix* Bare = Line(FMantleAvatarStripFixer::KindBareSkin))
						{
							Bare->bEnabled = State == ECheckBoxState::Checked;
						}
					})
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock).AutoWrapText(true).Text(LOCTEXT("BodyRowBare", "The kept parts are bare skin"))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Text_Lambda([Line]
						{
							const FMantleConvertFix* Bare = Line(FMantleAvatarStripFixer::KindBareSkin);
							return Bare ? FText::FromString(Bare->Problem + TEXT(". ") + Bare->Action) : FText::GetEmpty();
						})
					]
				]
			]
		];
	}
}

void SMantleCosmeticPanel::AddPackReviewRows()
{
	PackMoveOptions.Reset();
	PackBodyOptions.Reset();
	if (!ConvertReview.IsValid() || !ConvertBox.IsValid())
	{
		return;
	}
	const FMantlePackReview& Review = *ConvertReview;
	auto Subdued = [this](const FString& Text, const float Indent)
	{
		ConvertBox->AddSlot().AutoHeight().Padding(Indent, 0, 0, 2)
		[
			SNew(STextBlock).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(FText::FromString(Text))
		];
	};
	ConvertBox->AddSlot().AutoHeight().Padding(0, 2, 0, 2)
	[
		SNew(STextBlock).Font(FAppStyle::GetFontStyle("NormalFontBold")).Text(LOCTEXT("PackHeading", "Found in this pack"))
	];
	Subdued(TEXT("Each piece is placed by where it sits on the body. Change any row: Convert uses them as you leave them."), 0.0f);

	// The body Convert makes the MetaHuman of: the bare bodies of the pack, best first.
	TSharedPtr<int32> ChosenBody;
	TArray<FString> NotOffered;
	for (int32 Index = 0; Index < Review.Bodies.Num(); ++Index)
	{
		if (Review.Bodies[Index].Rank < 0)
		{
			NotOffered.Add(FString::Printf(TEXT("%s (%s)"), *Review.Bodies[Index].Label, *Review.Bodies[Index].Why));
			continue;
		}
		PackBodyOptions.Add(MakeShared<int32>(Index));
		if (Index == Review.Body)
		{
			ChosenBody = PackBodyOptions.Last();
		}
	}
	auto BodyText = [this](const int32 Index)
	{
		const FMantlePackBody* Body = ConvertReview.IsValid() && ConvertReview->Bodies.IsValidIndex(Index) ? &ConvertReview->Bodies[Index] : nullptr;
		return Body ? FText::FromString(FString::Printf(TEXT("%s (%s)"), *Body->Label, *Body->Why)) : FText::GetEmpty();
	};
	ConvertBox->AddSlot().AutoHeight().Padding(0, 4, 0, 2)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
		[
			SNew(STextBlock).Text(LOCTEXT("PackBody", "Body:"))
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[
			SNew(SComboBox<TSharedPtr<int32>>)
			.OptionsSource(&PackBodyOptions)
			.InitiallySelectedItem(ChosenBody)
			.IsEnabled_Lambda([this] { return !IsRunning(); })
			.ToolTipText(LOCTEXT("PackBodyTooltip", "The body Convert makes the MetaHuman of. The pack's other bare bodies are listed; a body that wears clothing isn't one."))
			.OnGenerateWidget_Lambda([BodyText](TSharedPtr<int32> Item)
			{
				return SNew(STextBlock).Text(Item.IsValid() ? BodyText(*Item) : FText::GetEmpty());
			})
			.OnSelectionChanged_Lambda([this](TSharedPtr<int32> Item, ESelectInfo::Type Info)
			{
				if (Info != ESelectInfo::Direct && Item.IsValid() && ConvertReview.IsValid() && ConvertReview->Body != *Item)
				{
					ConvertReview->Body = *Item;
					UE_LOG(LogMantle, Log, TEXT("MANTLE_PACK body changed to %s"), *ConvertReview->Bodies[*Item].Label);
					RefreshConvertRowsLater(/*bAnalyze*/ true);
				}
			})
			[
				SNew(STextBlock).Text_Lambda([this, BodyText] { return ConvertReview.IsValid() ? BodyText(ConvertReview->Body) : FText::GetEmpty(); })
			]
		]
	];
	if (NotOffered.Num() > 0)
	{
		Subdued(FString::Printf(TEXT("Other bodies in the pack, not used: %s."), *FString::Join(NotOffered, TEXT("; "))), 0.0f);
	}

	// One row per piece, grouped by where it goes; the pieces that go into one shown under it.
	auto Order = [](const FString& Slot)
	{
		return Slot == TEXT("top") ? 0 : Slot == TEXT("bottom") ? 1 : Slot == TEXT("footwear") ? 2 : Slot == TEXT("accessory") ? 3 : Slot.IsEmpty() ? 5 : 4;
	};
	auto IsGarment = [](const FString& Slot) { return Slot == TEXT("top") || Slot == TEXT("bottom") || Slot == TEXT("footwear"); };
	TArray<int32> Heads;
	for (int32 Index = 0; Index < Review.Pieces.Num(); ++Index)
	{
		if (Review.Pieces[Index].PartOf == INDEX_NONE || !Review.Pieces.IsValidIndex(Review.Pieces[Index].PartOf))
		{
			Heads.Add(Index);
		}
	}
	Heads.StableSort([&Review, &Order](const int32 A, const int32 B) { return Order(Review.Pieces[A].Slot) < Order(Review.Pieces[B].Slot); });
	auto PieceRow = [this, &Review, &Subdued, &IsGarment](const int32 Index, const float Indent)
	{
		const FMantlePackPiece& Piece = Review.Pieces[Index];
		const bool bUsed = !Piece.Slot.IsEmpty();
		// "Make it": a slot, into another piece's item, or not used.
		const TSharedRef<TArray<TSharedPtr<FMantlePackMove>>> Moves = MakeShared<TArray<TSharedPtr<FMantlePackMove>>>();
		TSharedPtr<FMantlePackMove> Current;
		auto AddMove = [&Moves, &Current, &Piece](const FString& Label, const FString& Slot, const int32 PartOf)
		{
			const TSharedPtr<FMantlePackMove> Move = MakeShared<FMantlePackMove>(FMantlePackMove{ Label, Slot, PartOf });
			Moves->Add(Move);
			if (PartOf != INDEX_NONE ? Piece.PartOf == PartOf : (Piece.PartOf == INDEX_NONE && Piece.Slot == Slot))
			{
				Current = Move;
			}
		};
		AddMove(TEXT("Top"), TEXT("top"), INDEX_NONE);
		AddMove(TEXT("Bottom"), TEXT("bottom"), INDEX_NONE);
		AddMove(TEXT("Footwear"), TEXT("footwear"), INDEX_NONE);
		AddMove(TEXT("Accessory"), TEXT("accessory"), INDEX_NONE);
		AddMove(TEXT("Hair"), TEXT("hair"), INDEX_NONE);
		if (bUsed && Piece.PartOf == INDEX_NONE && !Current.IsValid())
		{
			AddMove(Review.SlotHeading(Piece), Piece.Slot, INDEX_NONE);
		}
		for (int32 Other = 0; Other < Review.Pieces.Num(); ++Other)
		{
			if (Other != Index && Review.Pieces[Other].PartOf == INDEX_NONE && Review.Pieces[Other].bTicked && IsGarment(Review.Pieces[Other].Slot))
			{
				AddMove(FString::Printf(TEXT("Part of %s"), *Review.Pieces[Other].Label), FString(), Other);
			}
		}
		AddMove(TEXT("Not used"), FString(), INDEX_NONE);
		PackMoveOptions.Add(Moves);
		ConvertBox->AddSlot().AutoHeight().Padding(Indent, 2, 0, 0)
		[
			SNew(SHorizontalBox)
			.ToolTipText(FText::FromString(Piece.Why))
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
			[
				SNew(SCheckBox)
				.Visibility(bUsed ? EVisibility::Visible : EVisibility::Hidden)
				.IsEnabled_Lambda([this] { return !IsRunning(); })
				.ToolTipText(LOCTEXT("PackTickTooltip", "Untick to leave this piece out of this conversion; tick it again to bring it back."))
				.IsChecked_Lambda([this, Index]
				{
					return ConvertReview.IsValid() && ConvertReview->Pieces.IsValidIndex(Index) && ConvertReview->Pieces[Index].bTicked ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this, Index](const ECheckBoxState State)
				{
					if (ConvertReview.IsValid() && ConvertReview->Pieces.IsValidIndex(Index))
					{
						ConvertReview->Pieces[Index].bTicked = State == ECheckBoxState::Checked;
						UE_LOG(LogMantle, Log, TEXT("MANTLE_PACK %s %s"), *ConvertReview->Pieces[Index].Label, ConvertReview->Pieces[Index].bTicked ? TEXT("ticked") : TEXT("unticked"));
					}
				})
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity_Lambda([this, Index]
				{
					return ConvertReview.IsValid() && ConvertReview->Pieces.IsValidIndex(Index) && ConvertReview->Pieces[Index].bTicked ? FSlateColor::UseForeground() : FSlateColor::UseSubduedForeground();
				})
				.Text_Lambda([this, Index] { return ConvertReview.IsValid() ? FText::FromString(ConvertReview->RowText(Index)) : FText::GetEmpty(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
			[
				SNew(SComboBox<TSharedPtr<FMantlePackMove>>)
				.OptionsSource(&Moves.Get())
				.InitiallySelectedItem(Current)
				.IsEnabled_Lambda([this] { return !IsRunning(); })
				.ToolTipText(LOCTEXT("PackMoveTooltip", "Where this piece goes: a slot, into another piece's item (one item), or not used."))
				.OnGenerateWidget_Lambda([](TSharedPtr<FMantlePackMove> Item)
				{
					return SNew(STextBlock).Text(Item.IsValid() ? FText::FromString(Item->Label) : FText::GetEmpty());
				})
				.OnSelectionChanged_Lambda([this, Index](TSharedPtr<FMantlePackMove> Item, ESelectInfo::Type Info)
				{
					if (Info != ESelectInfo::Direct && Item.IsValid() && ConvertReview.IsValid())
					{
						ConvertReview->Move(Index, Item->Slot, Item->PartOf);
						UE_LOG(LogMantle, Log, TEXT("MANTLE_PACK %s made %s"), *ConvertReview->Pieces[Index].Label, *Item->Label);
						RefreshConvertRowsLater(/*bAnalyze*/ false);
					}
				})
				[
					SNew(STextBlock).Text(FText::FromString(Current.IsValid() ? Current->Label : FString(TEXT("Not used"))))
				]
			]
		];
		if (!bUsed || Piece.Slot == TEXT("accessory") || (!IsGarment(Piece.Slot) && Piece.PartOf == INDEX_NONE))
		{
			Subdued(Piece.Why, Indent + 22.0f);
		}
	};
	FString Group;
	bool bNotMade = false;
	for (const int32 Head : Heads)
	{
		const FMantlePackPiece& Piece = Review.Pieces[Head];
		const FString Heading = Review.SlotHeading(Piece);
		if (Heading != Group)
		{
			Group = Heading;
			const FString Slot = Piece.Slot;
			ConvertBox->AddSlot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock).Font(FAppStyle::GetFontStyle("NormalFontBold")).Text(FText::FromString(Heading + TEXT(":")))
				]
				// Several pieces in one slot: one item (the default), or one item each.
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12, 0, 0, 0)
				[
					SNew(SCheckBox)
					.Visibility_Lambda([this, Slot, IsGarment]
					{
						int32 Ticked = 0;
						if (ConvertReview.IsValid())
						{
							for (const FMantlePackPiece& Other : ConvertReview->Pieces)
							{
								Ticked += Other.bTicked && Other.Slot == Slot && Other.PartOf == INDEX_NONE ? 1 : 0;
							}
						}
						return IsGarment(Slot) && Ticked > 1 ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.ToolTipText(LOCTEXT("PackOneItemTooltip", "Ticked: the ticked pieces here make one item (a corset with its sleeves). Unticked: one item each."))
					.IsChecked_Lambda([this, Slot] { return ConvertReview.IsValid() && ConvertReview->IsOneItem(Slot) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this, Slot](const ECheckBoxState State)
					{
						if (ConvertReview.IsValid())
						{
							if (State == ECheckBoxState::Checked)
							{
								ConvertReview->Separate.Remove(Slot);
							}
							else
							{
								ConvertReview->Separate.Add(Slot);
							}
							UE_LOG(LogMantle, Log, TEXT("MANTLE_PACK %s: %s"), *Slot, State == ECheckBoxState::Checked ? TEXT("one item") : TEXT("one item each"));
						}
					})
					[
						SNew(STextBlock).Text(LOCTEXT("PackOneItem", "One item"))
					]
				]
			];
		}
		bNotMade |= Piece.bTicked && !Piece.Slot.IsEmpty() && !IsGarment(Piece.Slot);
		PieceRow(Head, 0.0f);
		for (int32 Index = 0; Index < Review.Pieces.Num(); ++Index)
		{
			if (Review.Pieces[Index].PartOf == Head)
			{
				PieceRow(Index, 24.0f);
			}
		}
	}
	if (bNotMade)
	{
		Subdued(TEXT("Convert makes the body and the clothing (top, bottom, footwear); ticked hair is listed to publish after Convert, as it is. Accessories aren't made by it yet: upload them under Accessory."), 0.0f);
	}
}

void SMantleCosmeticPanel::LoadConvertedPart(const int32 PartIndex)
{
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!Contract || !ConvertContext.IsValid() || !ConvertContext->Parts.IsValidIndex(PartIndex))
	{
		return;
	}
	LoadPart(ConvertContext->Parts[PartIndex], ConvertContext->Parts[PartIndex].Name, PartIndex);
}

void SMantleCosmeticPanel::LoadPart(const FMantleConvertPart& ConvertedPart, const FString& Name, const int32 PartIndex)
{
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError);
	if (!Contract)
	{
		return;
	}
	// A copy: SetClass and SetSource below never touch the converter's state, but the part must outlive them unchanged.
	FMantleConvertPart Part = ConvertedPart;
	Part.Name = Name;
	EMantleCosmeticClass Class = EMantleCosmeticClass::Garment;
	if (!FMantleAvatarConverter::ClassOfSlot(Part.Slot, *Contract, Class) || !Part.Mesh.IsValid())
	{
		return;
	}
	SetClass(Class);
	for (const TSharedPtr<FString>& Option : SlotOptions)
	{
		if (Option.IsValid() && Option->Equals(Part.Slot, ESearchCase::CaseSensitive))
		{
			Draft.Slot = *Option;
			if (SlotCombo.IsValid())
			{
				SlotCombo->SetSelectedItem(Option);
			}
			break;
		}
	}
	// Set before the source, so its defaults keep them (FMantleEditorActions::ApplyCosmeticSourceDefaults).
	Draft.AuthoringBody = Part.AuthoringBody;
	Draft.AuthoringFace = Part.AuthoringFace;
	Draft.Name = Part.Name;
	SetSource(Part.Mesh.Get());
	if (Class == EMantleCosmeticClass::Avatar)
	{
		Draft.Face = Part.Face;
	}
	if (Class == EMantleCosmeticClass::Hair && Part.Section != INDEX_NONE)
	{
		Draft.SourceSection = Part.Section;
	}
	Package.Reset();
	if (ConvertResult.IsSet())
	{
		for (const FMantleConvertPartCheck& Check : ConvertResult->Checks)
		{
			if (Check.PartIndex == PartIndex)
			{
				ShowIssues(Check.Issues, TArray<FString>());
			}
		}
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT use part=%d %s mesh=%s face=%s section=%d body=%s"), PartIndex, *FMantleAvatarConverter::DescribePart(Part, *Contract),
		*Part.Mesh->GetPathName(), Part.Face.IsValid() ? *Part.Face->GetPathName() : TEXT("none"), Part.Section,
		Part.AuthoringBody.IsValid() ? *Part.AuthoringBody->GetPathName() : TEXT("found by the upload"));
}

void SMantleCosmeticPanel::UseConvertGarment(const int32 GarmentIndex)
{
	if (!ConvertContext.IsValid() || !ConvertContext->Garments.IsValidIndex(GarmentIndex) || !ConvertContext->Parts.IsValidIndex(0))
	{
		return;
	}
	const FMantleConvertMetaHumanWork* Work = ConvertContext->MetaHuman.Find(0);
	if (ConvertContext->Garments[GarmentIndex].Outfit.IsEmpty() && Work && FMantleGarmentOutfit::WhyNoOutfits(*ConvertContext, 0, ConvertContext->Garments[GarmentIndex].Sections.Num() > 0).IsEmpty())
	{
		// The character's own clothing sections are cut into meshes first (they have none until now).
		FMantleGarmentOutfit::MakeSectionMeshes(*ConvertContext);
		// Every piece that can be an Outfit and isn't yet, at once: they share one copy of the MetaHuman as their authored body.
		// Chunk 6: a piece unticked in the publish list isn't made (ticking it later makes it then).
		TArray<int32> Indices;
		TArray<USkeletalMesh*> Meshes;
		TArray<TArray<USkeletalMesh*>> Trims;
		for (int32 Index = 0; Index < ConvertContext->Garments.Num(); ++Index)
		{
			const FMantleConvertGarment& Garment = ConvertContext->Garments[Index];
			const bool bUnticked = PublishRows.ContainsByPredicate([Index](const FMantlePublishRow& Row)
			{
				return Row.Kind == FMantlePublishRow::EKind::Garment && Row.Index == Index && !Row.bTicked;
			});
			if (Garment.Why.IsEmpty() && Garment.Outfit.IsEmpty() && Garment.Error.IsEmpty() && Garment.Mesh.IsValid() && (!bUnticked || Index == GarmentIndex))
			{
				Indices.Add(Index);
				Meshes.Add(Garment.Mesh.Get());
				TArray<USkeletalMesh*>& Its = Trims.AddDefaulted_GetRef();
				for (const TWeakObjectPtr<USkeletalMesh>& Trim : Garment.Trims)
				{
					Its.Add(Trim.Get());
				}
			}
		}
		const FMantleConvertPart& Avatar = ConvertContext->Parts[0];
		const FString BodyName = Avatar.Original.IsValid() ? Avatar.Original->GetName() : Avatar.Name;
		const FString Folder = ConvertContext->OutputFolder / (FMantleMetaHumanSteps::AssetTag(BodyName) + TEXT("_Outfit"));
		FMantleGarmentOutfitResult Result;
		FString RefusalCode;
		FString Refusal;
		bool bMade = false;
		{
			FScopedSlowTask Progress(1.0f, LOCTEXT("ConvertGarmentRunning", "Making the clothing into MetaHuman Outfits on this character's MetaHuman..."));
			Progress.MakeDialog();
			Progress.EnterProgressFrame(1.0f);
			const bool bCut = Indices.ContainsByPredicate([this](const int32 Index) { return ConvertContext->Garments[Index].Sections.Num() > 0; });
			bMade = FMantleGarmentOutfit::MakeFromWork(*Work, BodyName, Folder, Meshes, Result, RefusalCode, Refusal, bCut, Trims);
		}
		// Its own mesh can't be split cleanly: Convert's note says its clothing isn't offered.
		if (RefusalCode == TEXT("not_splittable"))
		{
			ConvertContext->Notes.AddUnique(Refusal);
		}
		for (int32 Made = 0; Made < Indices.Num(); ++Made)
		{
			FMantleConvertGarment& Garment = ConvertContext->Garments[Indices[Made]];
			const FMantleGarmentOutfitPiece* Piece = bMade && Result.Pieces.IsValidIndex(Made) ? &Result.Pieces[Made] : nullptr;
			Garment.Outfit = Piece ? Piece->Outfit : FString();
			Garment.Error = !bMade ? Refusal : Piece ? Piece->Error : FString(TEXT("no Outfit came back"));
			UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT garment %s outfit=%s error=%s"), Garment.Mesh.IsValid() ? *Garment.Mesh->GetPathName() : TEXT("?"),
				*Garment.Outfit, *Garment.Error);
		}
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT garment outfits on %s: body gap %.2f cm, %s"), *Result.Character, Result.BodyGapCm, bMade ? TEXT("made") : *Refusal);
		FMantlePublishList::Sync(*ConvertContext, PublishRows);
	}
	const FMantleConvertGarment Garment = ConvertContext->Garments[GarmentIndex];
	UChaosOutfitAsset* Outfit = Garment.Outfit.IsEmpty() ? nullptr : LoadObject<UChaosOutfitAsset>(nullptr, *Garment.Outfit);
	if (!Outfit)
	{
		RefreshConvertRows();
		return;
	}
	SetClass(EMantleCosmeticClass::Garment);
	for (const TSharedPtr<FString>& Option : SlotOptions)
	{
		if (Option.IsValid() && Option->Equals(Garment.Slot, ESearchCase::CaseSensitive))
		{
			Draft.Slot = *Option;
			if (SlotCombo.IsValid())
			{
				SlotCombo->SetSelectedItem(Option);
			}
			break;
		}
	}
	// The Outfit carries its own authored body (the MetaHuman); the panel finds it (RefreshOutfitAuthoringDefaults).
	Draft.AuthoringBody.Reset();
	Draft.AuthoringFace.Reset();
	Draft.Name = Garment.Name;
	SetSource(Outfit);
	Package.Reset();
	RefreshConvertRows();
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT use garment=%d %s slot=%s outfit=%s"), GarmentIndex, *Garment.Name, *Garment.Slot, *Garment.Outfit);
}

void SMantleCosmeticPanel::MakePublishRows()
{
	PublishRows.Reset();
	PublishThumbs.Reset();
	PublishOpen = INDEX_NONE;
	PublishOpenSource.Reset();
	bPublishWalk = false;
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	if (!ConvertContext.IsValid() || !Contract)
	{
		return;
	}
	PublishRows = FMantlePublishList::Make(*ConvertContext, ConvertReview.Get(), Contract->HairSlots);
	FScopedSlowTask Progress(static_cast<float>(PublishRows.Num()), LOCTEXT("PublishThumbs", "Drawing a thumbnail for each item..."));
	Progress.MakeDialogDelayed(0.5f);
	for (const FMantlePublishRow& Row : PublishRows)
	{
		Progress.EnterProgressFrame(1.0f);
		PublishThumbs.Add(RenderPublishThumb(Row));
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT publish row %s: %s slot=%s %s%s thumbnail=%s"), *FMantlePublishList::SlotWord(Row.Slot), *Row.Name, *Row.Slot,
			Row.bTicked ? TEXT("ticked") : TEXT("unticked"), Row.Why.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (greyed: %s)"), *Row.Why),
			PublishThumbs.Last().IsValid() ? TEXT("drawn") : TEXT("none"));
	}
}

TSharedPtr<FSlateDynamicImageBrush> SMantleCosmeticPanel::RenderPublishThumb(const FMantlePublishRow& Row) const
{
	if (!ConvertContext.IsValid() || !FMantleCosmeticThumbnail::CanCapture())
	{
		return nullptr;
	}
	const FMantleConvertContext& Context = *ConvertContext;
	// The pack's body the clothing and hair were made on (the converted part's source), else the converted body.
	USkeletalMesh* PackBody = Context.Parts.IsValidIndex(0) ? (Context.Parts[0].Original.IsValid() ? Context.Parts[0].Original.Get() : Context.Parts[0].Mesh.Get()) : nullptr;
	FMantleThumbnailSubject Subject;
	switch (Row.Kind)
	{
	case FMantlePublishRow::EKind::Part:
	case FMantlePublishRow::EKind::Kept:
	{
		const FMantleConvertPart* Part = Row.Kind == FMantlePublishRow::EKind::Part ? (Context.Parts.IsValidIndex(Row.Index) ? &Context.Parts[Row.Index] : nullptr)
			: (Context.Kept.IsValidIndex(Row.Index) ? &Context.Kept[Row.Index] : nullptr);
		if (!Part || !Part->Mesh.IsValid())
		{
			return nullptr;
		}
		if (Row.Slot.Equals(TEXT("avatar"), ESearchCase::CaseSensitive))
		{
			Subject = FMantleCosmeticThumbnail::AvatarSubject(Part->Mesh.Get(), Part->Face.Get());
		}
		else if (Row.Kind == FMantlePublishRow::EKind::Kept)
		{
			// On the Blueprint's own body and face, which it was made on.
			Subject = FMantleCosmeticThumbnail::GarmentSubject(Part->Mesh.Get(), Context.AssemblyBodyToFit.Get(), Context.AssemblyFaceToFit.Get());
		}
		else
		{
			const FMantleConvertPart* Avatar = Context.Parts.IsValidIndex(0) ? &Context.Parts[0] : nullptr;
			Subject = FMantleCosmeticThumbnail::GarmentSubject(Part->Mesh.Get(), Avatar ? Avatar->Mesh.Get() : nullptr, Avatar ? Avatar->Face.Get() : nullptr);
		}
		break;
	}
	case FMantlePublishRow::EKind::PackHair:
	{
		const int32 Mesh = ConvertReview.IsValid() && ConvertReview->Pieces.IsValidIndex(Row.Index) ? ConvertReview->Pieces[Row.Index].MeshIndex : INDEX_NONE;
		USkeletalMesh* Hair = ConvertReview.IsValid() && ConvertReview->Meshes.IsValidIndex(Mesh) ? ConvertReview->Meshes[Mesh].Get() : nullptr;
		if (!Hair)
		{
			return nullptr;
		}
		Subject = FMantleCosmeticThumbnail::GarmentSubject(Hair, PackBody, nullptr);
		break;
	}
	case FMantlePublishRow::EKind::Garment:
	{
		const FMantleConvertGarment* Garment = Context.Garments.IsValidIndex(Row.Index) ? &Context.Garments[Row.Index] : nullptr;
		if (!Garment)
		{
			return nullptr;
		}
		if (Garment->Mesh.IsValid())
		{
			// On the body it came with, its trims drawn with it (the item uploads as one).
			Subject = FMantleCosmeticThumbnail::GarmentSubject(Garment->Mesh.Get(), PackBody, nullptr);
			for (const TWeakObjectPtr<USkeletalMesh>& Trim : Garment->Trims)
			{
				if (Trim.IsValid())
				{
					Subject.SkeletalMeshes.Add(Trim.Get());
				}
			}
		}
		else if (Garment->SourceMesh.IsValid())
		{
			// A piece of the character's own mesh, before it is cut: only its sections.
			Subject = FMantleCosmeticThumbnail::GarmentSubject(Garment->SourceMesh.Get(), nullptr, nullptr, &Garment->Sections);
		}
		break;
	}
	}
	if (Subject.IsEmpty())
	{
		return nullptr;
	}
	constexpr int32 Size = 128;
	TArray<FColor> Pixels;
	FString Error;
	if (!FMantleCosmeticThumbnail::Capture(Subject, Size, Pixels, Error))
	{
		UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT publish row %s: no thumbnail (%s)"), *Row.Name, *Error);
		return nullptr;
	}
	// FColor is BGRA in memory, the order the dynamic brush takes. A new name each time, so Slate never shows an old picture.
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Pixels.GetData()), Pixels.Num() * sizeof(FColor));
	static int32 Serial = 0;
	return FSlateDynamicImageBrush::CreateWithImageData(*FString::Printf(TEXT("MantlePublishRow_%d"), ++Serial), FVector2f(Size, Size), Bytes);
}

void SMantleCosmeticPanel::AddPublishRows()
{
	using namespace MantleCosmeticPanelPrivate;
	if (PublishRows.IsEmpty() || !ConvertContext.IsValid())
	{
		return;
	}
	ConvertBox->AddSlot().AutoHeight().Padding(0, 10, 0, 2)
	[
		SNew(STextBlock).Font(FAppStyle::GetFontStyle("NormalFontBold")).Text(LOCTEXT("PublishHeading", "What to publish"))
	];
	// The clothing's Outfits are made on the MetaHuman Convert made: when they can't be, the clothing rows say why.
	const FString WhyNone = FMantleGarmentOutfit::WhyNoOutfits(*ConvertContext, 0);
	const FString WhyNoneSection = FMantleGarmentOutfit::WhyNoOutfits(*ConvertContext, 0, /*bSectionPieces*/ true);
	for (int32 RowIndex = 0; RowIndex < PublishRows.Num(); ++RowIndex)
	{
		const FMantlePublishRow& Row = PublishRows[RowIndex];
		// What stands under the name: greyed with why, uploaded, the part's check, the Outfit made or why it wasn't.
		FString Status;
		FSlateColor StatusColour = FSlateColor::UseSubduedForeground();
		const FMantleConvertGarment* Garment = Row.Kind == FMantlePublishRow::EKind::Garment && ConvertContext->Garments.IsValidIndex(Row.Index)
			? &ConvertContext->Garments[Row.Index] : nullptr;
		FString Why = Row.Why;
		if (Garment && Why.IsEmpty())
		{
			Why = Garment->Sections.Num() > 0 ? WhyNoneSection : WhyNone;
		}
		if (!Why.IsEmpty())
		{
			Status = Why;
		}
		else if (Row.bUploaded)
		{
			Status = TEXT("Uploaded.");
			StatusColour = SeverityColour(EMantleCosmeticSeverity::Info);
		}
		else if (Row.Kind == FMantlePublishRow::EKind::Part && ConvertResult.IsSet())
		{
			const FMantleConvertPartCheck* Check = ConvertResult->Checks.FindByPredicate([&Row](const FMantleConvertPartCheck& Each) { return Each.PartIndex == Row.Index; });
			TArray<FString> Errors;
			for (int32 Issue = 0; Check && Issue < Check->Issues.Num(); ++Issue)
			{
				if (Check->Issues[Issue].Severity == EMantleCosmeticSeverity::Error)
				{
					Errors.Add(Check->Issues[Issue].ToString());
				}
			}
			Status = !Check ? FString() : Errors.IsEmpty() ? FString(TEXT("Ready to upload."))
				: FString::Printf(TEXT("%d problem(s) still stop the upload: %s"), Errors.Num(), *FString::Join(Errors, TEXT(" ")));
			StatusColour = SeverityColour(Errors.IsEmpty() ? EMantleCosmeticSeverity::Info : EMantleCosmeticSeverity::Error);
		}
		else if (Garment)
		{
			Status = !Garment->Error.IsEmpty() ? FString::Printf(TEXT("Couldn't make the Outfit: %s"), *Garment->Error)
				: !Garment->Outfit.IsEmpty() ? FString(TEXT("A MetaHuman Outfit on this character's MetaHuman, made."))
				: FString(TEXT("Becomes a MetaHuman Outfit on this character's MetaHuman when you open it."));
			StatusColour = Garment->Error.IsEmpty() ? FSlateColor::UseSubduedForeground() : SeverityColour(EMantleCosmeticSeverity::Error);
		}
		else if (Row.Kind == FMantlePublishRow::EKind::Kept)
		{
			Status = TEXT("As it is in the Blueprint.");
		}
		else if (Row.Kind == FMantlePublishRow::EKind::PackHair)
		{
			Status = TEXT("As it is in the pack; the Hair checks fit it to the head.");
		}
		const bool bCan = Why.IsEmpty();
		const TSharedPtr<FSlateDynamicImageBrush> Thumb = PublishThumbs.IsValidIndex(RowIndex) ? PublishThumbs[RowIndex] : nullptr;
		ConvertBox->AddSlot().AutoHeight().Padding(0, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
			[
				SNew(SCheckBox)
				.IsEnabled(bCan)
				.ToolTipText(LOCTEXT("PublishTickTip", "Ticked items are published; an unticked one is never uploaded."))
				.IsChecked_Lambda([this, RowIndex]
				{
					return PublishRows.IsValidIndex(RowIndex) && PublishRows[RowIndex].bTicked ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this, RowIndex](const ECheckBoxState State)
				{
					if (PublishRows.IsValidIndex(RowIndex))
					{
						PublishRows[RowIndex].bTicked = State == ECheckBoxState::Checked;
					}
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
			[
				SNew(SBox).WidthOverride(64.0f).HeightOverride(64.0f)
				[
					// No thumbnail without a renderer (a headless editor): the space stays, so the rows line up.
					SNew(SImage).Image(Thumb.IsValid() ? Thumb.Get() : FAppStyle::GetNoBrush())
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
					[
						SNew(STextBlock)
						.Font(FAppStyle::GetFontStyle("NormalFontBold"))
						.ColorAndOpacity(bCan ? FSlateColor::UseForeground() : FSlateColor::UseSubduedForeground())
						.Text(FText::FromString(FMantlePublishList::SlotWord(Row.Slot) + TEXT(":")))
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f)
					[
						SNew(SEditableTextBox)
						.IsEnabled(bCan)
						.ToolTipText(LOCTEXT("PublishNameTip", "The item's name in the catalog."))
						.Text_Lambda([this, RowIndex] { return FText::FromString(PublishRows.IsValidIndex(RowIndex) ? PublishRows[RowIndex].Name : FString()); })
						.OnTextCommitted_Lambda([this, RowIndex](const FText& Text, ETextCommit::Type)
						{
							if (PublishRows.IsValidIndex(RowIndex) && !Text.ToString().TrimStartAndEnd().IsEmpty())
							{
								PublishRows[RowIndex].Name = Text.ToString().TrimStartAndEnd();
							}
						})
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Visibility(Status.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
					.ColorAndOpacity(StatusColour)
					.Text(FText::FromString(Status))
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
			[
				SNew(SButton)
				.IsEnabled_Lambda([this, bCan] { return bCan && !IsRunning(); })
				.Text(LOCTEXT("PublishOpen", "Open"))
				.ToolTipText(LOCTEXT("PublishOpenTip", "Fill the panel with this item to preview, check and upload it. Clothing becomes a MetaHuman Outfit on this character's MetaHuman first; the first one makes every ticked piece and takes a minute."))
				.OnClicked_Lambda([this, RowIndex]
				{
					bPublishWalk = false;
					OpenPublishRow(RowIndex);
					return FReply::Handled();
				})
			]
		];
	}
}

void SMantleCosmeticPanel::OpenPublishRow(const int32 RowIndex)
{
	if (!PublishRows.IsValidIndex(RowIndex) || !ConvertContext.IsValid())
	{
		bPublishWalk = false;
		return;
	}
	const FMantlePublishRow Row = PublishRows[RowIndex];
	PublishOpen = RowIndex;
	switch (Row.Kind)
	{
	case FMantlePublishRow::EKind::Part:
		if (ConvertContext->Parts.IsValidIndex(Row.Index))
		{
			LoadPart(ConvertContext->Parts[Row.Index], Row.Name, Row.Index);
		}
		break;
	case FMantlePublishRow::EKind::Kept:
		if (ConvertContext->Kept.IsValidIndex(Row.Index))
		{
			LoadPart(ConvertContext->Kept[Row.Index], Row.Name, INDEX_NONE);
		}
		break;
	case FMantlePublishRow::EKind::PackHair:
	{
		const int32 Mesh = ConvertReview.IsValid() && ConvertReview->Pieces.IsValidIndex(Row.Index) ? ConvertReview->Pieces[Row.Index].MeshIndex : INDEX_NONE;
		FMantleConvertPart Hair;
		Hair.Slot = Row.Slot;
		Hair.Mesh = ConvertReview.IsValid() && ConvertReview->Meshes.IsValidIndex(Mesh) ? ConvertReview->Meshes[Mesh].Get() : nullptr;
		Hair.Original = Hair.Mesh;
		// Made on the pack's body (the converted part's source).
		if (ConvertContext->Parts.IsValidIndex(0))
		{
			Hair.AuthoringBody = ConvertContext->Parts[0].Original;
		}
		LoadPart(Hair, Row.Name, INDEX_NONE);
		break;
	}
	case FMantlePublishRow::EKind::Garment:
		if (ConvertContext->Garments.IsValidIndex(Row.Index))
		{
			ConvertContext->Garments[Row.Index].Name = Row.Name;
			UseConvertGarment(Row.Index);
		}
		break;
	}
	PublishOpenSource = Draft.Source;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_CONVERT publish open row %d %s: %s source=%s%s"), RowIndex, *FMantlePublishList::SlotWord(Row.Slot), *Row.Name,
		Draft.Source.IsValid() ? *Draft.Source->GetPathName() : TEXT("none"), bPublishWalk ? TEXT(" (publishing the ticked items)") : TEXT(""));
	// The row's own Open button may have called this: the rows are rebuilt after its callback.
	RefreshConvertRowsLater(false);
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCosmeticPanelDevShot
{
	/** Widget rendered offscreen at Width (its own height, wrapped text laid out at that width) over the editor's panel grey, as a PNG. */
	bool Save(const TSharedRef<SWidget>& Widget, const FString& File, const float Width, const float MaxHeight)
	{
		FWidgetRenderer Renderer(true, false);
		// The first pass lays the wrapped text out at Width; the second draws it at the height it then wants.
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
		UE_LOG(LogMantle, Display, TEXT("MANTLE_CONVERT_SHOT %s %dx%d %s"), *File, Target->SizeX, Target->SizeY, bSaved ? TEXT("saved") : TEXT("not saved"));
		return bSaved;
	}

	FAutoConsoleCommand Command(
		TEXT("Mantle.Dev.ConvertPanelShot"),
		TEXT("Mantle.Dev.ConvertPanelShot <Character> <Directory> [Confirm keys] [body=<Mesh> untick=<Mesh> tick=<Mesh> move=<Mesh>:<slot|none> separate=<slot> ...] [list-only]: converts a character in the Cosmetics panel (the pack's rows changed step by step first, a picture after each), uses its first clothing piece, and renders the panel after each step. Nothing uploads."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&SMantleCosmeticPanel::DevConvertShot));
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCosmeticPanelDevShot
{
	FAutoConsoleCommand PanelShotCommand(
		TEXT("Mantle.Dev.CosmeticPanelShot"),
		TEXT("Mantle.Dev.CosmeticPanelShot <File.png> [Exit] [Source= Class= Body= Face=]: renders the Cosmetics panel offscreen, to show its Thumbnail section. Uploads nothing."),
		FConsoleCommandWithArgsDelegate::CreateStatic(&SMantleCosmeticPanel::DevPanelShot));
}

void SMantleCosmeticPanel::DevPanelShot(const TArray<FString>& Args)
{
	using namespace MantleCosmeticPanelDevShot;
	if (Args.IsEmpty())
	{
		UE_LOG(LogMantle, Error, TEXT("MANTLE_PANEL_SHOT usage: Mantle.Dev.CosmeticPanelShot <File.png> [Exit]"));
		return;
	}
	const TSharedPtr<SDockTab> Tab = FGlobalTabmanager::Get()->TryInvokeTab(TabName);
	if (Tab.IsValid())
	{
		const TSharedRef<SMantleCosmeticPanel> Panel = StaticCastSharedRef<SMantleCosmeticPanel>(Tab->GetContent());
		FString SourcePath;
		FString CustomPng;
		// #52: Body= and Face= (asset paths) and Build check the draft as the Check button does, so the hair fit card shows.
		FString BodyPath, FacePath;
		bool bAngle = false;
		int32 ClassNumber = static_cast<int32>(EMantleCosmeticClass::Garment);
		for (int32 Index = 1; Index < Args.Num(); ++Index)
		{
			if (Args[Index].StartsWith(TEXT("Source=")))
			{
				SourcePath = Args[Index].Mid(7);
			}
			else if (Args[Index].StartsWith(TEXT("Class=")))
			{
				ClassNumber = FCString::Atoi(*Args[Index].Mid(6));
			}
			else if (Args[Index].StartsWith(TEXT("Body=")))
			{
				BodyPath = Args[Index].Mid(5);
			}
			else if (Args[Index].StartsWith(TEXT("Face=")))
			{
				FacePath = Args[Index].Mid(5);
			}
			else if (Args[Index].StartsWith(TEXT("SaveCustom=")))
			{
				CustomPng = Args[Index].Mid(11);
			}
			else if (Args[Index].Equals(TEXT("Angle"), ESearchCase::IgnoreCase))
			{
				bAngle = true;
			}
		}
		if (!SourcePath.IsEmpty())
		{
			if (!FPackageName::GetShortName(SourcePath).Contains(TEXT(".")))
			{
				SourcePath += TEXT(".") + FPackageName::GetShortName(SourcePath);
			}
			if (UObject* Item = LoadObject<UObject>(nullptr, *SourcePath))
			{
				Panel->Draft.Class = static_cast<EMantleCosmeticClass>(ClassNumber);
				Panel->SetSource(Item);
				if (!CustomPng.IsEmpty())
				{
					FMantleThumbnailChoice Choice;
					Choice.Mode = EMantleThumbnailMode::Custom;
					Choice.ImageFile.FilePath = CustomPng;
					Panel->ThumbnailChoiceChanged(Choice);
					Panel->ThumbnailSection->Refresh();
				}
			}
			else
			{
				UE_LOG(LogMantle, Error, TEXT("MANTLE_PANEL_SHOT failed: %s did not load"), *SourcePath);
			}
		}
		const auto LoadPath = [](FString Path) -> UObject*
		{
			if (Path.IsEmpty()) return nullptr;
			if (!FPackageName::GetShortName(Path).Contains(TEXT("."))) Path += TEXT(".") + FPackageName::GetShortName(Path);
			return LoadObject<UObject>(nullptr, *Path);
		};
		if (!BodyPath.IsEmpty() || !FacePath.IsEmpty())
		{
			Panel->Draft.AuthoringBody = Cast<USkeletalMesh>(LoadPath(BodyPath));
			Panel->Draft.AuthoringFace = Cast<USkeletalMesh>(LoadPath(FacePath));
			if (Panel->Draft.Class == EMantleCosmeticClass::Hair) Panel->Draft.Slot = TEXT("hair");
			Panel->Draft.bRenderThumbnail = false;
			Panel->BuildPackage();
		}
		if (bAngle)
		{
			// Choose the angle, from the class's default view: the same sliders the builders show.
			FMantleThumbnailChoice Choice = Panel->Draft.Thumbnail;
			Choice.Mode = EMantleThumbnailMode::Angle;
			Choice.Angle = FMantleCosmeticThumbnail::DefaultView(Panel->Draft.Class == EMantleCosmeticClass::Accessory);
			Panel->Draft.Thumbnail = Choice;
			Panel->ThumbnailSection->Refresh();
		}
		Save(Panel, Args[0], 640.0f, 6000.0f);
	}
	else
	{
		UE_LOG(LogMantle, Error, TEXT("MANTLE_PANEL_SHOT failed: the panel did not open"));
	}
	if (Args.ContainsByPredicate([](const FString& Arg) { return Arg.Equals(TEXT("Exit"), ESearchCase::IgnoreCase); }))
	{
		FPlatformMisc::RequestExit(false);
	}
}

void SMantleCosmeticPanel::DevConvertShot(const TArray<FString>& Args)
{
	using namespace MantleCosmeticPanelDevShot;
	if (Args.Num() < 2)
	{
		UE_LOG(LogMantle, Error, TEXT("MANTLE_CONVERT_SHOT usage: Mantle.Dev.ConvertPanelShot <Character> <Directory> [Confirm keys] [body=<Mesh> untick=<Mesh> ...] [list-only]"));
		return;
	}
	FString Path = Args[0];
	if (!FPackageName::GetShortName(Path).Contains(TEXT(".")))
	{
		Path += TEXT(".") + FPackageName::GetShortName(Path);
	}
	UObject* Character = LoadObject<UObject>(nullptr, *Path);
	const TSharedPtr<SDockTab> Tab = FGlobalTabmanager::Get()->TryInvokeTab(TabName);
	if (!Character || !Tab.IsValid())
	{
		UE_LOG(LogMantle, Error, TEXT("MANTLE_CONVERT_SHOT failed: %s did not load, or the panel did not open"), *Path);
		return;
	}
	const TSharedRef<SMantleCosmeticPanel> Panel = StaticCastSharedRef<SMantleCosmeticPanel>(Tab->GetContent());
	const FString Stem = FPaths::Combine(Args[1], FPackageName::GetShortName(Character->GetOutermost()->GetName()));
	// #55: the pack review's rows changed one step at a time (body=<Mesh>, untick=<Mesh>, tick=<Mesh>, move=<Mesh>:<slot>,
	// separate=<slot>), a picture after each; list-only stops before Convert.
	TArray<FString> Confirm;
	TArray<FString> Edits;
	bool bListOnly = false;
	for (int32 Arg = 2; Arg < Args.Num(); ++Arg)
	{
		if (Args[Arg] == TEXT("list-only"))
		{
			bListOnly = true;
		}
		else if (Args[Arg].Contains(TEXT("=")))
		{
			Edits.Add(Args[Arg]);
		}
		else
		{
			Args[Arg].ParseIntoArray(Confirm, TEXT("+"));
		}
	}
	auto ConfirmAndShoot = [&Panel, &Confirm](const FString& File)
	{
		const TArray<FString> Keys = FMantleAvatarConverter::FixKeys(Panel->ConvertFixes);
		for (int32 Index = 0; Index < Keys.Num(); ++Index)
		{
			if (Confirm.Contains(Keys[Index]))
			{
				Panel->ConvertFixes[Index].bEnabled = true;
			}
		}
		Panel->RefreshConvertRows();
		// Chunk 5: the body row as it stands (the picture shows the same).
		for (const FMantleConvertFix& Fix : Panel->ConvertFixes)
		{
			if (FMantleAvatarStripFixer::IsBodyRowLine(Fix))
			{
				UE_LOG(LogMantle, Display, TEXT("MANTLE_CONVERT_SHOT body row part %d %s: %s%s (%s)"), Fix.PartIndex, *Fix.Kind.ToString(),
					Fix.bEnabled ? TEXT("ticked") : TEXT("unticked"), Fix.bCanFix ? TEXT("") : TEXT(", greyed"), *Fix.Problem);
			}
		}
		Save(Panel->ConvertSectionBox.ToSharedRef(), File, 820.0f, 4000.0f);
		// What Convert would take from the rows as they stand (OnRunConvert's own call).
		if (Panel->ConvertReview.IsValid() && Panel->ConvertContext.IsValid() && Panel->ConvertContext->Parts.IsValidIndex(0) && Panel->ConvertContext->Parts[0].Original.IsValid())
		{
			TArray<FMantleConvertGarment> Garments;
			FMantleGarmentOutfit::GarmentsFromReview(*Panel->ConvertContext->Parts[0].Original.Get(), *Panel->ConvertReview, Garments);
			UE_LOG(LogMantle, Display, TEXT("MANTLE_CONVERT_SHOT %s: body %s, %d clothing item(s)"), *FPaths::GetCleanFilename(File), *Panel->ConvertContext->Parts[0].Original->GetName(), Garments.Num());
		}
	};

	// Pick it at the Avatar door, list its fixes, tick the ones to confirm.
	Panel->SetClass(EMantleCosmeticClass::Avatar);
	Panel->PickAvatar(Character);
	UE_LOG(LogMantle, Display, TEXT("MANTLE_CONVERT_SHOT sees: %s As is: %s"), *FMantleConvertSection::WhatISee(Panel->AvatarSight),
		!FMantleConvertSection::ShowsRoutePicker() ? TEXT("hidden") : FMantleConvertSection::AsIsGreyedWhy(Panel->AvatarSight).IsEmpty() ? TEXT("offered")
		: *FMantleConvertSection::AsIsGreyedWhy(Panel->AvatarSight));
	Panel->OnAnalyzeConvert();
	if (!Panel->ConvertContext.IsValid() || !Panel->ConvertSectionBox.IsValid())
	{
		UE_LOG(LogMantle, Error, TEXT("MANTLE_CONVERT_SHOT %s: %s"), *Path, *Panel->ConvertStatus);
		return;
	}
	ConfirmAndShoot(Stem + TEXT("_list.png"));
	for (int32 Step = 0; Step < Edits.Num(); ++Step)
	{
		FString Key;
		FString Value;
		Edits[Step].Split(TEXT("="), &Key, &Value);
		FMantlePackReview* Review = Panel->ConvertReview.Get();
		const int32 Piece = Review ? Review->Pieces.IndexOfByPredicate([Review, &Value](const FMantlePackPiece& Each)
		{
			return Review->MeshNames.IsValidIndex(Each.MeshIndex) && Value.StartsWith(Review->MeshNames[Each.MeshIndex]) && (Value.Len() == Review->MeshNames[Each.MeshIndex].Len() || Value[Review->MeshNames[Each.MeshIndex].Len()] == TEXT(':'));
		}) : INDEX_NONE;
		if (!Review)
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_CONVERT_SHOT %s: no pack rows to change"), *Edits[Step]);
			break;
		}
		if (Key == TEXT("body"))
		{
			const int32 Body = Review->Bodies.IndexOfByPredicate([Review, &Value](const FMantlePackBody& Each) { return Review->MeshNames.IsValidIndex(Each.MeshIndex) && Review->MeshNames[Each.MeshIndex] == Value; });
			if (Body != INDEX_NONE)
			{
				Review->Body = Body;
				Panel->AnalyzeConvertBody();
			}
		}
		else if ((Key == TEXT("untick") || Key == TEXT("tick")) && Piece != INDEX_NONE)
		{
			Review->Pieces[Piece].bTicked = Key == TEXT("tick");
		}
		else if (Key == TEXT("move") && Piece != INDEX_NONE)
		{
			FString Slot;
			Value.Split(TEXT(":"), nullptr, &Slot);
			Review->Move(Piece, Slot == TEXT("none") ? FString() : Slot);
		}
		else if (Key == TEXT("separate"))
		{
			Review->Separate.Add(Value);
		}
		else
		{
			UE_LOG(LogMantle, Error, TEXT("MANTLE_CONVERT_SHOT %s: no such change or piece"), *Edits[Step]);
			continue;
		}
		UE_LOG(LogMantle, Display, TEXT("MANTLE_CONVERT_SHOT changed %s"), *Edits[Step]);
		for (const FString& Line : Review->Describe(Character->GetName()))
		{
			UE_LOG(LogMantle, Display, TEXT("%s"), *Line);
		}
		ConfirmAndShoot(FString::Printf(TEXT("%s_list_%d.png"), *Stem, Step + 1));
	}
	if (bListOnly)
	{
		return;
	}

	Panel->OnRunConvert();
	UE_LOG(LogMantle, Display, TEXT("MANTLE_CONVERT_SHOT converted: %s"), *Panel->ConvertStatus);
	// Chunk 6: the "What to publish" list, and the order "Publish the ticked items" would open its rows in.
	TArray<FString> Order;
	for (int32 Row = FMantlePublishList::Next(Panel->PublishRows, INDEX_NONE); Row != INDEX_NONE; Row = FMantlePublishList::Next(Panel->PublishRows, Row))
	{
		Order.Add(FMantlePublishList::SlotWord(Panel->PublishRows[Row].Slot) + TEXT(": ") + Panel->PublishRows[Row].Name);
	}
	UE_LOG(LogMantle, Display, TEXT("MANTLE_CONVERT_SHOT publish list: %d row(s), %d drawn; publishes in order: %s"), Panel->PublishRows.Num(),
		Panel->PublishThumbs.FilterByPredicate([](const TSharedPtr<FSlateDynamicImageBrush>& Thumb) { return Thumb.IsValid(); }).Num(), *FString::Join(Order, TEXT("; ")));
	Save(Panel->ConvertSectionBox.ToSharedRef(), Stem + TEXT("_converted.png"), 820.0f, 4000.0f);

	const int32 First = Panel->ConvertContext->Garments.IndexOfByPredicate([](const FMantleConvertGarment& Garment) { return Garment.Why.IsEmpty(); });
	if (First == INDEX_NONE || !FMantleGarmentOutfit::WhyNoOutfits(*Panel->ConvertContext, 0).IsEmpty())
	{
		UE_LOG(LogMantle, Display, TEXT("MANTLE_CONVERT_SHOT no clothing piece to use: %s"), *FMantleGarmentOutfit::WhyNoOutfits(*Panel->ConvertContext, 0));
		return;
	}
	Panel->UseConvertGarment(First);
	Save(Panel->ConvertSectionBox.ToSharedRef(), Stem + TEXT("_used.png"), 820.0f, 4000.0f);
	Save(Panel, Stem + TEXT("_panel.png"), 820.0f, 2400.0f);
	for (const FMantleConvertGarment& Garment : Panel->ConvertContext->Garments)
	{
		UE_LOG(LogMantle, Display, TEXT("MANTLE_CONVERT_SHOT garment %s slot=%s outfit=%s why=%s error=%s"), *Garment.Name, *Garment.Slot, *Garment.Outfit, *Garment.Why, *Garment.Error);
	}
	UE_LOG(LogMantle, Display, TEXT("MANTLE_CONVERT_SHOT draft source=%s slot=%s name=%s"), Panel->Draft.Source.IsValid() ? *Panel->Draft.Source->GetPathName() : TEXT("none"),
		*Panel->Draft.Slot, *Panel->Draft.Name);
}

bool SMantleCosmeticPanel::DevCheck(const EMantleCosmeticClass Class, UObject* Source, const FString& Slot, TArray<FMantleCosmeticIssue>& OutIssues)
{
	const TSharedRef<SMantleCosmeticPanel> Panel = SNew(SMantleCosmeticPanel);
	Panel->SetClass(Class);
	Panel->SetSlot(Slot);
	Panel->SetSource(Source);
	// The slot again once the source is in: a garment's section ticks follow it, as when the creator picks the slot last.
	Panel->SetSlot(Slot);
	const bool bPassed = Panel->BuildPackage();
	OutIssues = Panel->ShownIssues;
	return bPassed;
}

#undef LOCTEXT_NAMESPACE
