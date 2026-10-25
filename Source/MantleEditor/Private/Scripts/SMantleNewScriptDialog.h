// Copyright Joshua Eger. All Rights Reserved.
// The New Script dialog of the Scripts panel.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SComboBox.h"
#include "MantleScriptTemplates.h"

class SMantleScriptsPanel;
class SEditableTextBox;
class STextBlock;
class SWindow;

/**
 * "New Script..." -- a small standalone Slate dialog (SWindow), not UMG: pick Side/Language/
 * Template/Name and it writes Scripts/<Side>/<Name>.<ext> from FMantleScriptTemplates. Sharing
 * FMantleScriptTemplates::CreateScriptFile with Mantle.NewScript keeps the dialog's Create button
 * and the console command doing exactly the same thing.
 */
class SMantleNewScriptDialog : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SMantleNewScriptDialog) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const FString& InScriptsRoot, TWeakPtr<SMantleScriptsPanel> InPanel);

	/** Builds the dialog inside a new top-level window and shows it. */
	static void OpenDialog(const FString& ScriptsRoot, TSharedPtr<SMantleScriptsPanel> Panel);

	void SetOwningWindow(TSharedPtr<SWindow> InWindow) { OwningWindow = InWindow; }

private:
	using FOptionPtr = TSharedPtr<FString>;

	TSharedRef<SWidget> BuildComboRow(const FText& Label, TArray<FOptionPtr>& Options, FOptionPtr& Selected, TSharedPtr<SComboBox<FOptionPtr>>& OutCombo);
	TSharedRef<SWidget> MakeOptionWidget(FOptionPtr Option);
	FText GetComboText(FOptionPtr Option) const { return Option.IsValid() ? FText::FromString(*Option) : FText::GetEmpty(); }

	FReply OnCreateClicked();
	FReply OnCancelClicked();

	FString ScriptsRoot;
	TWeakPtr<SMantleScriptsPanel> Panel;
	TWeakPtr<SWindow> OwningWindow;

	TArray<FOptionPtr> SideOptions, LanguageOptions, TemplateOptions;
	FOptionPtr SelectedSide, SelectedLanguage, SelectedTemplate;
	TSharedPtr<SComboBox<FOptionPtr>> SideCombo, LanguageCombo, TemplateCombo;
	TSharedPtr<SEditableTextBox> NameBox;
	TSharedPtr<STextBlock> ErrorText;
};
