// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

class SBox;

/** One step of a builder's guided window, in plain words. */
struct FMantleBuilderStep
{
	/** Short, plain, e.g. "Choose the 3D model". */
	FText Title;

	/** One short paragraph telling the creator what to do here. */
	FText Help;

	/** Builds the step's content each time the step opens. */
	TFunction<TSharedRef<SWidget>()> MakeContent;

	/** Null means always. When it returns false, OutWhyNot says in plain words what to do first; Next stays off and shows it. */
	TFunction<bool(FText& OutWhyNot)> CanContinue;

	/** Runs when the step opens, before its content is built. May be null. */
	TFunction<void()> OnEnter;
};

/**
 * The guided, one-decision-per-screen window the weapon builder and the vehicle builder share: a numbered bar of steps across
 * the top (done steps ticked and clickable to go back), the step's help, its content, and Back and Next. The last step's Next
 * is the FinishLabel. It knows nothing about weapons or vehicles.
 */
class MANTLEEDITOR_API SMantleBuilderFlow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMantleBuilderFlow) {}
		SLATE_ARGUMENT(TArray<FMantleBuilderStep>, Steps)
		SLATE_ARGUMENT(FText, FinishLabel)
		/** The last step's button was pressed (it is only enabled when that step's CanContinue passes). */
		SLATE_EVENT(FSimpleDelegate, OnFinish)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Opens a step (clamped); runs its OnEnter and builds its content. */
	void GoTo(int32 Index);

	int32 GetCurrent() const { return Current; }

	/** Builds the current step's content again, e.g. after something it shows has changed. */
	void Refresh();

private:
	TSharedRef<SWidget> MakeStepBar();
	TSharedRef<SWidget> MakeStepButton(int32 Index);
	void BuildContent();
	bool CanGoNext(FText* OutWhyNot = nullptr) const;
	FReply OnNext();

	TArray<FMantleBuilderStep> Steps;
	FText FinishLabel;
	FSimpleDelegate OnFinish;
	int32 Current = 0;
	TSharedPtr<SBox> ContentBox;
};
