// Copyright Joshua Eger. All Rights Reserved.
#include "SMantleNewScriptDialog.h"

#include "MantleScriptDiscovery.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "SMantleScriptsPanel.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MantleEditor"

void SMantleNewScriptDialog::OpenDialog(const FString& ScriptsRoot, TSharedPtr<SMantleScriptsPanel> Panel)
{
	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("NewScriptTitle", "New Script"))
		.ClientSize(FVector2D(420, 260))
		.SupportsMaximize(false)
		.SupportsMinimize(false)
		.SizingRule(ESizingRule::FixedSize);

	TSharedRef<SMantleNewScriptDialog> Content = SNew(SMantleNewScriptDialog, ScriptsRoot, Panel);
	Content->SetOwningWindow(Window);
	Window->SetContent(Content);

	if (TSharedPtr<SWindow> RootWindow = FGlobalTabmanager::Get()->GetRootWindow())
	{
		FSlateApplication::Get().AddWindowAsNativeChild(Window, RootWindow.ToSharedRef());
	}
	else
	{
		FSlateApplication::Get().AddWindow(Window);
	}
}

void SMantleNewScriptDialog::Construct(const FArguments& InArgs, const FString& InScriptsRoot, TWeakPtr<SMantleScriptsPanel> InPanel)
{
	ScriptsRoot = InScriptsRoot;
	Panel = InPanel;

	for (EMantleScriptRowSide Side : FMantleScriptTemplates::AllSides())
	{
		SideOptions.Add(MakeShared<FString>(FMantleScriptTemplates::SideFolderName(Side)));
	}
	for (EMantleScriptEditorLanguage Language : FMantleScriptTemplates::NewScriptLanguages())
	{
		LanguageOptions.Add(MakeShared<FString>(FMantleScriptTemplates::LanguageDisplayName(Language)));
	}
	for (EMantleScriptTemplateKind Template : FMantleScriptTemplates::AllTemplates())
	{
		TemplateOptions.Add(MakeShared<FString>(FMantleScriptTemplates::TemplateDisplayName(Template)));
	}
	SelectedSide = SideOptions[0];
	SelectedLanguage = LanguageOptions[0];
	SelectedTemplate = TemplateOptions[0];

	ChildSlot
	[
		SNew(SBorder).Padding(12)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(2)
			[
				BuildComboRow(LOCTEXT("Side", "Side"), SideOptions, SelectedSide, SideCombo)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(2)
			[
				BuildComboRow(LOCTEXT("Language", "Language"), LanguageOptions, SelectedLanguage, LanguageCombo)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(2)
			[
				BuildComboRow(LOCTEXT("Template", "Template"), TemplateOptions, SelectedTemplate, TemplateCombo)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(2, 8)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
				[
					SNew(STextBlock).Text(LOCTEXT("Name", "Name"))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					SAssignNew(NameBox, SEditableTextBox).HintText(LOCTEXT("NameHint", "MyScript"))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(2, 4)
			[
				SAssignNew(ErrorText, STextBlock)
				.Text(FText::GetEmpty())
				.ColorAndOpacity(FLinearColor(1.0f, 0.35f, 0.3f))
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SNew(SSpacer)
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
				[
					SNew(SButton).Text(LOCTEXT("Cancel", "Cancel")).OnClicked(this, &SMantleNewScriptDialog::OnCancelClicked)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0)
				[
					SNew(SButton).Text(LOCTEXT("Create", "Create")).OnClicked(this, &SMantleNewScriptDialog::OnCreateClicked)
				]
			]
		]
	];
}

TSharedRef<SWidget> SMantleNewScriptDialog::BuildComboRow(const FText& Label, TArray<FOptionPtr>& Options, FOptionPtr& Selected, TSharedPtr<SComboBox<FOptionPtr>>& OutCombo)
{
	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
		[
			SNew(SBox).WidthOverride(70.0f)
			[
				SNew(STextBlock).Text(Label)
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f)
		[
			SAssignNew(OutCombo, SComboBox<FOptionPtr>)
			.OptionsSource(&Options)
			.InitiallySelectedItem(Selected)
			.OnGenerateWidget(this, &SMantleNewScriptDialog::MakeOptionWidget)
			.OnSelectionChanged_Lambda([&Selected](FOptionPtr NewSelection, ESelectInfo::Type) { Selected = NewSelection; })
			[
				SNew(STextBlock).Text_Lambda([&Selected]() { return Selected.IsValid() ? FText::FromString(*Selected) : FText::GetEmpty(); })
			]
		];
}

TSharedRef<SWidget> SMantleNewScriptDialog::MakeOptionWidget(FOptionPtr Option)
{
	return SNew(STextBlock).Text(GetComboText(Option));
}

FReply SMantleNewScriptDialog::OnCreateClicked()
{
	FMantleNewScriptRequest Request;
	FMantleScriptTemplates::ParseSide(SelectedSide.IsValid() ? *SelectedSide : FString(), Request.Side);
	FMantleScriptTemplates::ParseLanguage(SelectedLanguage.IsValid() ? *SelectedLanguage : FString(), Request.Language);
	FMantleScriptTemplates::ParseTemplate(SelectedTemplate.IsValid() ? *SelectedTemplate : FString(), Request.Template);
	Request.Name = NameBox.IsValid() ? NameBox->GetText().ToString() : FString();

	FString Error;
	FString RelativePath;
	if (Request.Language == EMantleScriptEditorLanguage::Graph)
	{
		const FString ScriptsPackageRoot = FMantleScriptDiscovery::CurrentLevelScriptsPackageRoot();
		const FString PackagePath = FMantleScriptTemplates::CreateGraphAsset(ScriptsPackageRoot, Request, Error);
		if (PackagePath.IsEmpty())
		{
			if (ErrorText.IsValid())
			{
				ErrorText->SetText(FText::FromString(Error));
			}
			return FReply::Handled();
		}
		RelativePath = FMantleScriptTemplates::SideFolderName(Request.Side) / Request.Name;
	}
	else
	{
		const FString FullPath = FMantleScriptTemplates::CreateScriptFile(ScriptsRoot, Request, Error);
		if (FullPath.IsEmpty())
		{
			if (ErrorText.IsValid())
			{
				ErrorText->SetText(FText::FromString(Error));
			}
			return FReply::Handled();
		}
		RelativePath = FMantleScriptTemplates::SideFolderName(Request.Side) / (Request.Name + TEXT(".") + FMantleScriptTemplates::FileExtension(Request.Language));
	}

	if (TSharedPtr<SMantleScriptsPanel> PanelPtr = Panel.Pin())
	{
		PanelPtr->RefreshAndSelect(RelativePath);
	}
	if (TSharedPtr<SWindow> Window = OwningWindow.Pin())
	{
		Window->RequestDestroyWindow();
	}
	return FReply::Handled();
}

FReply SMantleNewScriptDialog::OnCancelClicked()
{
	if (TSharedPtr<SWindow> Window = OwningWindow.Pin())
	{
		Window->RequestDestroyWindow();
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
