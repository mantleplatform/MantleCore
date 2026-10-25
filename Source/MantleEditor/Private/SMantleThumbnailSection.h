// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleThumbnailChoice.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"

struct FSlateDynamicImageBrush;

/**
 * The Thumbnail section every Mantle uploader shows (#124): Automatic, Choose the angle, Custom image, and the exact picture that will be uploaded.
 * The wording and layout are the same everywhere (FMantleThumbnailText); each uploader supplies where its choice is kept, how its kind renders,
 * and (for Choose the angle) an orbit preview or a "Use this view" action.
 */
class SMantleThumbnailSection : public SCompoundWidget
{
public:
	/** Called with the new choice; the host saves it with the creator's source item. */
	DECLARE_DELEGATE_OneParam(FOnChoiceChanged, const FMantleThumbnailChoice&);

	/**
	 * Called for "Use this view": the host takes the angle from its orbit preview (or, for a place, the level viewport), saves it and returns an empty
	 * text; or returns why it could not.
	 */
	DECLARE_DELEGATE_RetVal(FText, FOnUseView);

	SLATE_BEGIN_ARGS(SMantleThumbnailSection) {}
		/** The saved choice, read whenever the section redraws. */
		SLATE_ATTRIBUTE(FMantleThumbnailChoice, Choice)
		/** The picture's size: FMantleThumbnailTarget::Square() or Wide(). */
		SLATE_ARGUMENT(FMantleThumbnailTarget, Target)
		SLATE_EVENT(FOnChoiceChanged, OnChoiceChanged)
		SLATE_EVENT(FOnUseView, OnUseView)
		/** The kind's render for Automatic and Angle (see FMantleThumbnailResolver::FRender). */
		SLATE_ARGUMENT(FMantleThumbnailResolver::FRender, Render)
		/** The "Use this view" button's text; a place says "Use current view" for the level viewport. */
		SLATE_ARGUMENT(FText, UseViewLabel)
		/**
		 * Choose the angle with turn, tilt and zoom sliders (for a kind with no orbit preview of its own, a model on its own): the preview redraws
		 * as they move, and there is no "Use this view" button.
		 */
		SLATE_ARGUMENT(bool, AngleSliders)
		/** With the sliders: the angle Choose the angle starts from the first time it is picked. */
		SLATE_ATTRIBUTE(FRotator, DefaultAngle)
		/** Under Choose the angle: the orbit preview, when the kind has one. */
		SLATE_DEFAULT_SLOT(FArguments, AngleContent)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Draws the picture again from the saved choice (after the host changed the item). */
	void Refresh();

	/** The picture the upload would send for the saved choice, as the section last drew it. */
	const FMantleThumbnailResult& GetResult() const { return Result; }

private:
	FMantleThumbnailChoice Current() const;
	void SetChoice(const FMantleThumbnailChoice& NewChoice);
	void SetMode(EMantleThumbnailMode Mode);
	TSharedRef<SWidget> MakeModeRow(EMantleThumbnailMode Mode);
	TSharedRef<SWidget> MakeCustomControls();
	TSharedRef<SWidget> MakeAngleSliders();

	TAttribute<FMantleThumbnailChoice> ChoiceAttribute;
	FMantleThumbnailTarget Target;
	FOnChoiceChanged OnChoiceChanged;
	FOnUseView OnUseView;
	FMantleThumbnailResolver::FRender Render;
	FText UseViewLabel;
	bool bAngleSliders = false;
	TAttribute<FRotator> DefaultAngle;

	FMantleThumbnailResult Result;
	/** The last "Use this view" refusal, shown until the next change. */
	FText ViewNote;
	TSharedPtr<FSlateDynamicImageBrush> Brush;
};
