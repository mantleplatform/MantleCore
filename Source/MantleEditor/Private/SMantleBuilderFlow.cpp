// Copyright Joshua Eger. All Rights Reserved.
#include "SMantleBuilderFlow.h"

#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MantleBuilderFlow"

void SMantleBuilderFlow::Construct(const FArguments& InArgs)
{
	Steps = InArgs._Steps;
	FinishLabel = InArgs._FinishLabel.IsEmpty() ? LOCTEXT("Finish", "Finish") : InArgs._FinishLabel;
	OnFinish = InArgs._OnFinish;

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(12.f, 10.f, 12.f, 6.f)
		[
			MakeStepBar()
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(16.f, 6.f, 16.f, 2.f)
		[
			SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("DetailsView.CategoryFontStyle"))
			.Text_Lambda([this] { return Steps.IsValidIndex(Current) ? Steps[Current].Title : FText::GetEmpty(); })
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(16.f, 2.f, 16.f, 8.f)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text_Lambda([this] { return Steps.IsValidIndex(Current) ? Steps[Current].Help : FText::GetEmpty(); })
		]
		+ SVerticalBox::Slot().FillHeight(1.f).Padding(16.f, 0.f)
		[
			SAssignNew(ContentBox, SBox)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(16.f, 8.f, 16.f, 12.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SButton)
				.Text(LOCTEXT("Back", "Back"))
				.IsEnabled_Lambda([this] { return Current > 0; })
				.OnClicked_Lambda([this]
				{
					GoTo(Current - 1);
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center).Padding(12.f, 0.f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FLinearColor(0.95f, 0.65f, 0.25f))
				.Text_Lambda([this]
				{
					FText WhyNot;
					return CanGoNext(&WhyNot) ? FText::GetEmpty() : WhyNot;
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
				.Text_Lambda([this] { return Current >= Steps.Num() - 1 ? FinishLabel : LOCTEXT("Next", "Next"); })
				.IsEnabled_Lambda([this] { return CanGoNext(); })
				.OnClicked(this, &SMantleBuilderFlow::OnNext)
			]
		]
	];

	GoTo(0);
}

TSharedRef<SWidget> SMantleBuilderFlow::MakeStepBar()
{
	TSharedRef<SHorizontalBox> Bar = SNew(SHorizontalBox);
	for (int32 Index = 0; Index < Steps.Num(); ++Index)
	{
		Bar->AddSlot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)
		[
			MakeStepButton(Index)
		];
	}
	return Bar;
}

TSharedRef<SWidget> SMantleBuilderFlow::MakeStepButton(int32 Index)
{
	// A done step shows a tick and goes back when clicked, the current one is lit, the ones ahead are greyed.
	return SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "SimpleButton")
		.IsEnabled_Lambda([this, Index] { return Index < Current; })
		.ToolTipText_Lambda([this, Index] { return Steps.IsValidIndex(Index) ? Steps[Index].Title : FText::GetEmpty(); })
		.OnClicked_Lambda([this, Index]
		{
			if (Index < Current)
			{
				GoTo(Index);
			}
			return FReply::Handled();
		})
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 4.f, 0.f)
			[
				SNew(SBox).WidthOverride(16.f).HeightOverride(16.f)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.Check"))
					.ColorAndOpacity(FLinearColor(0.55f, 0.80f, 0.55f))
					.Visibility_Lambda([this, Index] { return Index < Current ? EVisibility::Visible : EVisibility::Collapsed; })
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text_Lambda([this, Index]
				{
					return FText::Format(LOCTEXT("StepLabel", "{0}. {1}"), FText::AsNumber(Index + 1), Steps.IsValidIndex(Index) ? Steps[Index].Title : FText::GetEmpty());
				})
				.ColorAndOpacity_Lambda([this, Index]
				{
					if (Index == Current)
					{
						return FSlateColor(FLinearColor(1.f, 1.f, 1.f));
					}
					return Index < Current ? FSlateColor(FLinearColor(0.55f, 0.80f, 0.55f)) : FSlateColor(FLinearColor(0.45f, 0.45f, 0.45f));
				})
				.Font_Lambda([this, Index]
				{
					return Index == Current ? FAppStyle::GetFontStyle("NormalFontBold") : FAppStyle::GetFontStyle("NormalFont");
				})
			]
		];
}

void SMantleBuilderFlow::GoTo(int32 Index)
{
	if (Steps.Num() == 0)
	{
		return;
	}
	Current = FMath::Clamp(Index, 0, Steps.Num() - 1);
	if (Steps[Current].OnEnter)
	{
		Steps[Current].OnEnter();
	}
	BuildContent();
}

void SMantleBuilderFlow::Refresh()
{
	BuildContent();
}

void SMantleBuilderFlow::BuildContent()
{
	if (!ContentBox.IsValid())
	{
		return;
	}
	if (Steps.IsValidIndex(Current) && Steps[Current].MakeContent)
	{
		ContentBox->SetContent(Steps[Current].MakeContent());
	}
	else
	{
		ContentBox->SetContent(SNullWidget::NullWidget);
	}
}

bool SMantleBuilderFlow::CanGoNext(FText* OutWhyNot) const
{
	if (!Steps.IsValidIndex(Current) || !Steps[Current].CanContinue)
	{
		return true;
	}
	FText WhyNot;
	const bool bOk = Steps[Current].CanContinue(WhyNot);
	if (OutWhyNot)
	{
		*OutWhyNot = WhyNot;
	}
	return bOk;
}

FReply SMantleBuilderFlow::OnNext()
{
	if (!CanGoNext())
	{
		return FReply::Handled();
	}
	if (Current >= Steps.Num() - 1)
	{
		OnFinish.ExecuteIfBound();
	}
	else
	{
		GoTo(Current + 1);
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
