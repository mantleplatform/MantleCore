// Copyright Joshua Eger. All Rights Reserved.
// The Doors step of the Build a vehicle window (SMantleVehicleBuilder): which bone of the model is each door, the hood and the trunk, so players can open them,
// and a Flip for a part that swings the wrong way.
#include "SMantleVehicleBuilder.h"

#include "MantleVehicleBuilder.h"
#include "MantleVehicleChassisSkin.h"
#include "SMantleBuilderViewport.h"

#include "Styling/AppStyle.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MantleVehicleBuilder"

TSharedRef<SWidget> SMantleVehicleBuilder::MakeDoorsStep()
{
	SAssignNew(DoorRowsBox, SVerticalBox);
	RebuildDoorRows();
	return WithPicture(SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			DoorRowsBox.ToSharedRef()
		]);
}

void SMantleVehicleBuilder::SetHingeBone(const EMantleVehicleHingePart Part, const FName Bone)
{
	if (Bone.IsNone())
	{
		HingeChoice.Bones.Remove(Part);
		HingeChoice.Flipped.Remove(Part);
	}
	else
	{
		HingeChoice.Bones.Add(Part, Bone);
	}
	// What was built is of the old choice; the picture is made again so it shows the bones the build will have.
	ForgetBuild();
	RebuildDoorRows();
	RebuildPreview(/*bFrame*/ false);
}

void SMantleVehicleBuilder::SetHingeFlip(const EMantleVehicleHingePart Part, const bool bFlipped)
{
	if (bFlipped)
	{
		HingeChoice.Flipped.Add(Part);
	}
	else
	{
		HingeChoice.Flipped.Remove(Part);
	}
	ForgetBuild();
	RebuildPreview(/*bFrame*/ false);
}

void SMantleVehicleBuilder::RebuildDoorRows()
{
	if (!DoorRowsBox.IsValid())
	{
		return;
	}
	DoorRowsBox->ClearChildren();
	DoorRowsBox->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
	[
		SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("DoorsHelp", "Pick which part of your model is each door, the hood and the trunk, so players can open them. Mantle filled in what it could find. Leave a part on None if your model doesn't have it."))
	];
	if (!bHaveModelInfo)
	{
		return;
	}
	if (!ModelInfo.bSkeletal || ModelInfo.Bones.Num() < 2)
	{
		DoorRowsBox->AddSlot().AutoHeight().Padding(0, 0, 0, 6)
		[
			SNew(STextBlock)
				.AutoWrapText(true)
				.Text(LOCTEXT("DoorsNeedBones", "Your model's doors can't open because it has no bones. To make them open, rig each door, the hood and the trunk to a bone of its own in your 3D tool and import the model again."))
		];
		return;
	}

	// The model's bones to pick from, None first.
	HingeChoices.Reset();
	HingeValues.Reset();
	HingeChoices.Add(MakeShared<FString>(TEXT("None")));
	HingeValues.Add(FName());
	for (const FMantleVehicleModelPiece& Piece : FMantleVehicleChassisSkin::Pieces(ModelInfo))
	{
		if (Piece.Kind == FMantleVehicleModelPiece::EKind::Bone)
		{
			HingeChoices.Add(MakeShared<FString>(FMantleVehicleChassisSkin::DescribePiece(ModelInfo, Piece)));
			HingeValues.Add(Piece.Bone);
		}
	}
	for (const EMantleVehicleHingePart Part : FMantleVehicleChassisSkin::HingeParts())
	{
		const FName Assigned = HingeChoice.BoneOf(Part);
		const int32 Selected = FMath::Max(0, HingeValues.IndexOfByKey(Assigned));
		DoorRowsBox->AddSlot().AutoHeight().Padding(0, 1)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 2.f, 8.f, 2.f)
			[
				SNew(SBox).WidthOverride(120.f)
				[
					SNew(STextBlock).Text(FMantleVehicleChassisSkin::HingeLabel(Part)).AutoWrapText(true)
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(SComboBox<TSharedPtr<FString>>)
					.OptionsSource(&HingeChoices)
					.InitiallySelectedItem(HingeChoices[Selected])
					.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) { return SNew(STextBlock).Text(FText::FromString(*Item)); })
					.OnSelectionChanged_Lambda([this, Part](TSharedPtr<FString> Item, ESelectInfo::Type Info)
					{
						// Opening the list and setting its first choice is not a pick.
						const int32 At = HingeChoices.IndexOfByKey(Item);
						if (At != INDEX_NONE && Info != ESelectInfo::Direct)
						{
							SetHingeBone(Part, HingeValues[At]);
						}
					})
					[
						SNew(STextBlock).Text_Lambda([this, Part]
						{
							const int32 At = HingeValues.IndexOfByKey(HingeChoice.BoneOf(Part));
							return FText::FromString(HingeChoices.IsValidIndex(At) ? *HingeChoices[At] : FString(TEXT("None")));
						})
					]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
			[
				SNew(SCheckBox)
					.ToolTipText(LOCTEXT("FlipTip", "Turn this on if the part swings the wrong way when it opens. Build, then try it in the game: a door should swing out, the hood and trunk up."))
					.IsEnabled_Lambda([this, Part] { return !HingeChoice.BoneOf(Part).IsNone(); })
					.IsChecked_Lambda([this, Part] { return HingeChoice.IsFlipped(Part) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this, Part](ECheckBoxState State) { SetHingeFlip(Part, State == ECheckBoxState::Checked); })
					[
						SNew(STextBlock).Text(LOCTEXT("Flip", "Flip"))
					]
			]
		];
	}
}

#undef LOCTEXT_NAMESPACE
