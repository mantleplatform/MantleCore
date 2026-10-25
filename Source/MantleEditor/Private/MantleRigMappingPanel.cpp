// Copyright Joshua Eger. All Rights Reserved.
#include "MantleRigMappingPanel.h"
#include "MantleAvatarContract.h"

#include "Engine/SkeletalMesh.h"
#include "Misc/Paths.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MantleRigMappingPanel"

namespace MantleRigMappingPanelPrivate
{
	FText RigMappingOptionText(const FName Source, const bool bMustMap)
	{
		if (!Source.IsNone())
		{
			return FText::FromName(Source);
		}
		return bMustMap ? LOCTEXT("ChooseBone", "(choose a bone)") : LOCTEXT("InsertBone", "(none: added with no weights)");
	}
}

void SMantleRigMappingPanel::Construct(const FArguments& InArgs)
{
	Mesh = InArgs._Mesh;
	OnMappingChanged = InArgs._OnMappingChanged;

	const TArray<FMantleRigBone> Bones = FMantleAvatarRigMap::ReadBones(Mesh.Get());
	SourceOptions.Add(MakeShared<FName>(NAME_None));
	for (const FMantleRigBone& Bone : Bones)
	{
		SourceOptions.Add(MakeShared<FName>(Bone.Name));
	}
	if (const FMantleRigMapping* Session = FMantleAvatarRigMap::FindSessionMapping(Mesh.Get()))
	{
		Mapping = *Session;
	}
	else if (const FMantleAvatarContract* Contract = FMantleAvatarContract::Get())
	{
		bool bKnown = false;
		Mapping = FMantleAvatarRigMap::Detect(Bones, *Contract, FMantleRigProfiles::All(), bKnown);
	}
	ProfileName = Mapping.ProfileName.IsEmpty() && Mesh.IsValid() ? Mesh->GetName() : Mapping.ProfileName;

	TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);
	TArray<FName> Targets;
	for (const FMantleRigMappingEntry& Entry : Mapping.Entries)
	{
		if (Entry.bRequired)
		{
			Targets.Add(Entry.Target);
		}
	}
	for (const FName Target : Targets)
	{
		Rows->AddSlot().AutoHeight().Padding(0.0f, 1.0f)[MakeRow(Target)];
	}

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text_Lambda([this] { return StatusText(); })
		]
		+ SVerticalBox::Slot().AutoHeight().MaxHeight(360.0f)
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				Rows
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.0f)
			[
				SNew(SEditableTextBox)
				.HintText(LOCTEXT("ProfileNameHint", "Profile name"))
				.Text_Lambda([this] { return FText::FromString(ProfileName); })
				.OnTextChanged_Lambda([this](const FText& Text) { ProfileName = Text.ToString(); })
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(SButton)
				.Text(LOCTEXT("SaveProfile", "Save as profile"))
				.ToolTipText(LOCTEXT("SaveProfileTip", "Remember this mapping, so characters with this rig convert without mapping again."))
				.OnClicked(this, &SMantleRigMappingPanel::SaveProfile)
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text_Lambda([this] { return SaveResult; })
		]
	];
}

TSharedRef<SWidget> SMantleRigMappingPanel::MakeRow(const FName Target)
{
	using namespace MantleRigMappingPanelPrivate;
	const bool bMustMap = FMantleAvatarRigMap::MustMapBones().Contains(Target);
	const FName Current = Mapping.SourceFor(Target);
	TSharedPtr<FName> Initial = SourceOptions[0];
	for (const TSharedPtr<FName>& Option : SourceOptions)
	{
		if (Option.IsValid() && !Current.IsNone() && *Option == Current)
		{
			Initial = Option;
			break;
		}
	}
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(0.4f).VAlign(VAlign_Center)
		[
			SNew(STextBlock).Text(FText::FromName(Target))
		]
		+ SHorizontalBox::Slot().FillWidth(0.6f)
		[
			SNew(SComboBox<TSharedPtr<FName>>)
			.OptionsSource(&SourceOptions)
			.InitiallySelectedItem(Initial)
			.OnGenerateWidget_Lambda([bMustMap](TSharedPtr<FName> Item)
			{
				return SNew(STextBlock).Text(RigMappingOptionText(Item.IsValid() ? *Item : NAME_None, bMustMap));
			})
			.OnSelectionChanged_Lambda([this, Target](TSharedPtr<FName> Item, ESelectInfo::Type)
			{
				if (Item.IsValid())
				{
					Choose(Target, *Item);
				}
			})
			[
				SNew(STextBlock).Text_Lambda([this, Target, bMustMap] { return RigMappingOptionText(Mapping.SourceFor(Target), bMustMap); })
			]
		];
}

void SMantleRigMappingPanel::Choose(const FName Target, const FName Source)
{
	Mapping.Set(Target, Source, true);
	FMantleAvatarRigMap::SetSessionMapping(Mesh.Get(), Mapping);
	SaveResult = FText::GetEmpty();
	OnMappingChanged.ExecuteIfBound(Mapping);
}

FText SMantleRigMappingPanel::StatusText() const
{
	const TArray<FName> Missing = Mapping.MissingBones();
	const FText Rig = Mapping.ProfileName.IsEmpty() ? LOCTEXT("UnknownRig", "This rig is not one Mantle knows.")
		: FText::Format(LOCTEXT("KnownRig", "{0} rig."), FText::FromString(Mapping.ProfileName));
	if (Missing.Num() > 0)
	{
		TArray<FString> Names;
		for (const FName Bone : Missing)
		{
			Names.Add(Bone.ToString());
		}
		return FText::Format(LOCTEXT("MapMore", "{0} Map {1} more bones to continue: {2}."), Rig, FText::AsNumber(Missing.Num()),
			FText::FromString(FString::Join(Names, TEXT(", "))));
	}
	return FText::Format(LOCTEXT("Mapped", "{0} Every bone the conversion needs is mapped onto the {1} skeleton; bones left empty are added with no weights."),
		Rig, FText::FromString(FMantleAvatarRigMap::SkeletonDisplayName(Mapping.TargetSkeletonId)));
}

FReply SMantleRigMappingPanel::SaveProfile()
{
	FString Path;
	FString Error;
	if (!FMantleRigProfiles::Save(Mapping, ProfileName, Path, Error))
	{
		SaveResult = FText::Format(LOCTEXT("SaveFailed", "Not saved: {0}."), FText::FromString(Error));
		return FReply::Handled();
	}
	Mapping.ProfileId = FPaths::GetBaseFilename(Path);
	Mapping.ProfileName = ProfileName.TrimStartAndEnd();
	FMantleAvatarRigMap::SetSessionMapping(Mesh.Get(), Mapping);
	SaveResult = FText::Format(LOCTEXT("Saved", "Saved to {0}. Characters with this rig now convert without mapping."), FText::FromString(Path));
	OnMappingChanged.ExecuteIfBound(Mapping);
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
