// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceSecretsDetails.h"
#include "MantleCreatorSignIn.h"
#include "MantlePlaceSecrets.h"
#include "MantlePlaceSettingsActor.h"
#include "MantleSettings.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Engine/World.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "MantleApiClient.h"
#include "Misc/MessageDialog.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MantlePlaceSecretsDetails"

namespace
{
	/** Everything the section remembers. The value is here and nowhere else; it is emptied after a save and with the section. */
	struct FSecretsState
	{
		TWeakObjectPtr<UWorld> World;
		FString PlaceId;
		MantlePlaceSecrets::FList List;
		bool bLoaded = false;
		bool bBusy = false;
		bool bStoreOff = false;
		FString Status;
		bool bStatusError = false;
		/** Shown once the next list loads, so a save or delete confirmation survives the refresh. */
		FString Note;
		FString NameText;
		FString Value;
		bool bPasted = false;
		TSharedPtr<SEditableTextBox> ValueBox;
		TSharedPtr<SEditableTextBox> NameBox;

		~FSecretsState() { Value.Empty(); }

		void SetStatus(const FString& Text, bool bError)
		{
			Status = Text;
			bStatusError = bError;
		}

		void ClearValue()
		{
			Value.Empty();
			bPasted = false;
			if (ValueBox.IsValid())
			{
				ValueBox->SetText(FText::GetEmpty());
			}
		}

		bool NameExists(const FString& Name) const
		{
			return List.Entries.ContainsByPredicate([&Name](const MantlePlaceSecrets::FEntry& Entry) { return Entry.Name == Name; });
		}

		/** The place this level publishes to for the signed-in account, or empty with Status explaining. */
		bool ResolvePlace()
		{
			UWorld* Level = World.Get();
			if (!Level)
			{
				SetStatus(TEXT("Open a saved level first."), true);
				return false;
			}
			FString WhyNot;
			FString Account;
			if (!FMantleCreatorSignIn::ResolveAccountId(Account, WhyNot))
			{
				SetStatus(WhyNot, true);
				return false;
			}
			PlaceId = UMantleSettings::Get()->FindPlaceForLevel(FMantleApiClient::BaseUrlResolver(), Account, UMantleSettings::LevelKey(Level));
			if (PlaceId.IsEmpty())
			{
				SetStatus(MantlePlaceSecrets::NoPlaceMessage(), false);
				return false;
			}
			return true;
		}

	};

	void Refresh(const TSharedRef<FSecretsState>& State)
	{
		if (State->bBusy || State->bStoreOff || !State->ResolvePlace())
		{
			return;
		}
		State->bBusy = true;
		State->SetStatus(TEXT("Loading..."), false);
		const TWeakPtr<FSecretsState> Weak = State;
		MantlePlaceSecrets::List(State->PlaceId, [Weak](const FMantleApiResult& Result)
		{
			const TSharedPtr<FSecretsState> Live = Weak.Pin();
			if (!Live)
			{
				return;
			}
			Live->bBusy = false;
			MantlePlaceSecrets::FList Parsed;
			if (Result.bSuccess && MantlePlaceSecrets::ParseList(Result.Body, Parsed))
			{
				Live->List = MoveTemp(Parsed);
				Live->bLoaded = true;
				Live->SetStatus(Live->Note, false);
				Live->Note.Empty();
			}
			else
			{
				Live->bStoreOff = MantlePlaceSecrets::IsStoreOff(Result);
				Live->SetStatus(Result.bSuccess ? FString(TEXT("Mantle sent a list this plugin could not read.")) : MantlePlaceSecrets::DescribeFailure(Result), true);
			}
		});
	}

	void Save(const TSharedRef<FSecretsState>& State)
	{
		if (State->bBusy)
		{
			return;
		}
		const FString Name = MantlePlaceSecrets::NormalizeName(State->NameText);
		FString Why;
		if (!MantlePlaceSecrets::CheckName(Name, Why)
			|| !MantlePlaceSecrets::CheckValue(State->Value, State->List.MaxValueBytes, Why)
			|| !MantlePlaceSecrets::CheckCount(State->List.Entries.Num(), State->List.MaxSecrets, State->NameExists(Name), Why))
		{
			State->SetStatus(Why, true);
			return;
		}
		if (!State->ResolvePlace())
		{
			return;
		}
		State->bBusy = true;
		State->SetStatus(FString::Printf(TEXT("Saving %s..."), *Name), false);
		const TWeakPtr<FSecretsState> Weak = State;
		MantlePlaceSecrets::Set(State->PlaceId, Name, State->Value, [Weak, Name](const FMantleApiResult& Result)
		{
			const TSharedPtr<FSecretsState> Live = Weak.Pin();
			if (!Live)
			{
				return;
			}
			Live->bBusy = false;
			if (Result.bSuccess)
			{
				Live->ClearValue();
				Live->NameText.Empty();
				if (Live->NameBox.IsValid())
				{
					Live->NameBox->SetText(FText::GetEmpty());
				}
				Live->Note = FString::Printf(TEXT("%s saved. Servers that start from now on get it; running servers keep what they loaded."), *Name);
				Live->bLoaded = false;
				Refresh(Live.ToSharedRef());
				return;
			}
			Live->bStoreOff = MantlePlaceSecrets::IsStoreOff(Result);
			Live->SetStatus(MantlePlaceSecrets::DescribeFailure(Result), true);
		});
	}

	void Remove(const TSharedRef<FSecretsState>& State, const FString& Name)
	{
		if (State->bBusy || !State->ResolvePlace())
		{
			return;
		}
		const FText Question = FText::Format(LOCTEXT("ConfirmDelete", "Delete the secret {0}? Server scripts will no longer get it on servers that start from now on."), FText::FromString(Name));
		if (FMessageDialog::Open(EAppMsgType::YesNo, Question) != EAppReturnType::Yes)
		{
			return;
		}
		State->bBusy = true;
		State->SetStatus(FString::Printf(TEXT("Deleting %s..."), *Name), false);
		const TWeakPtr<FSecretsState> Weak = State;
		MantlePlaceSecrets::Delete(State->PlaceId, Name, [Weak, Name](const FMantleApiResult& Result)
		{
			const TSharedPtr<FSecretsState> Live = Weak.Pin();
			if (!Live)
			{
				return;
			}
			Live->bBusy = false;
			if (Result.bSuccess)
			{
				Live->Note = FString::Printf(TEXT("%s deleted."), *Name);
				Refresh(Live.ToSharedRef());
				return;
			}
			Live->bStoreOff = MantlePlaceSecrets::IsStoreOff(Result);
			Live->SetStatus(MantlePlaceSecrets::DescribeFailure(Result), true);
		});
	}

	void OpenLocalFile(const TSharedRef<FSecretsState>& State)
	{
		const UWorld* Level = State->World.Get();
		FString Path;
		FString Why;
		if (!Level || !MantlePlaceSecrets::EnsureLocalFile(Level->GetName(), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), Path, Why))
		{
			State->SetStatus(Why.IsEmpty() ? FString(TEXT("Open a saved level first.")) : Why, true);
			return;
		}
		FPlatformProcess::LaunchFileInDefaultExternalApplication(*Path);
		State->SetStatus(FString::Printf(TEXT("Opened %s. It is ignored by git and used by Test Locally only."), *Path), false);
	}
}

TSharedRef<IDetailCustomization> FMantlePlaceSecretsDetails::MakeInstance()
{
	return MakeShared<FMantlePlaceSecretsDetails>();
}

void FMantlePlaceSecretsDetails::Register()
{
	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	PropertyEditor.RegisterCustomClassLayout(AMantlePlaceSettingsActor::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FMantlePlaceSecretsDetails::MakeInstance));
	PropertyEditor.NotifyCustomizationModuleChanged();
}

void FMantlePlaceSecretsDetails::Unregister()
{
	if (FModuleManager::Get().IsModuleLoaded(TEXT("PropertyEditor")) && UObjectInitialized())
	{
		FPropertyEditorModule& PropertyEditor = FModuleManager::GetModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
		PropertyEditor.UnregisterCustomClassLayout(AMantlePlaceSettingsActor::StaticClass()->GetFName());
		PropertyEditor.NotifyCustomizationModuleChanged();
	}
}

void FMantlePlaceSecretsDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	if (Objects.Num() != 1 || !Objects[0].IsValid())
	{
		return;
	}
	const TSharedRef<FSecretsState> State = MakeShared<FSecretsState>();
	State->World = Objects[0]->GetWorld();

	IDetailCategoryBuilder& Category = DetailBuilder.EditCategory(TEXT("Mantle|Secrets"), LOCTEXT("SecretsCategory", "Mantle: Secrets"));

	Category.AddCustomRow(LOCTEXT("SecretsInfoRow", "Secrets"))
	.WholeRowContent()
	[
		SNew(STextBlock)
		.AutoWrapText(true)
		.Text(LOCTEXT("SecretsInfo", "Secrets (API keys, webhook URLs) reach this place's server scripts on Mantle-hosted servers only, through Secrets:Get. A change reaches servers that start after it; running servers keep the values they loaded. Never send a value to a client. Values are write-only: they can't be read back here."))
	];

	Category.AddCustomRow(LOCTEXT("SecretsStatusRow", "Status"))
	.WholeRowContent()
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity_Lambda([State] { return State->bStatusError ? FSlateColor(FLinearColor(1.0f, 0.4f, 0.3f)) : FSlateColor::UseSubduedForeground(); })
			.Text_Lambda([State]
			{
				if (State->bStoreOff)
				{
					return FText::FromString(MantlePlaceSecrets::StoreOffMessage());
				}
				if (!State->Status.IsEmpty())
				{
					return FText::FromString(State->Status);
				}
				return State->bLoaded ? FText::FromString(MantlePlaceSecrets::CountText(State->List.Entries.Num(), State->List.MaxSecrets)) : LOCTEXT("NotLoaded", "Press Refresh to load this place's secrets.");
			})
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0)
		[
			SNew(SButton)
			.Text(LOCTEXT("Refresh", "Refresh"))
			.IsEnabled_Lambda([State] { return !State->bBusy && !State->bStoreOff; })
			.OnClicked_Lambda([State] { Refresh(State); return FReply::Handled(); })
		]
	];

	Category.AddCustomRow(LOCTEXT("SecretsListRow", "Secrets"))
	.WholeRowContent()
	[
		SNew(SVerticalBox)
		.Visibility_Lambda([State] { return State->bStoreOff ? EVisibility::Collapsed : EVisibility::Visible; })
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text_Lambda([State]
			{
				FString Lines;
				for (const MantlePlaceSecrets::FEntry& Entry : State->List.Entries)
				{
					Lines += FString::Printf(TEXT("%s    ••••••••    set %s\n"), *Entry.Name, *Entry.UpdatedAt);
				}
				return FText::FromString(Lines.IsEmpty() ? FString(TEXT("No secrets set.")) : Lines.TrimEnd());
			})
		]
	];

	Category.AddCustomRow(LOCTEXT("SecretsEditRow", "Add or replace"))
	.WholeRowContent()
	[
		SNew(SVerticalBox)
		.IsEnabled_Lambda([State] { return !State->bStoreOff && !State->bBusy; })
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
		[
			SAssignNew(State->NameBox, SEditableTextBox)
			.HintText(LOCTEXT("NameHint", "NAME (capital letters, digits, underscores)"))
			.OnTextChanged_Lambda([State](const FText& Text)
			{
				// Upper-cased as the creator types.
				State->NameText = MantlePlaceSecrets::NormalizeName(Text.ToString());
				if (State->NameBox.IsValid() && !State->NameBox->GetText().ToString().Equals(State->NameText, ESearchCase::CaseSensitive))
				{
					State->NameBox->SetText(FText::FromString(State->NameText));
				}
			})
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
		[
			SAssignNew(State->ValueBox, SEditableTextBox)
			.IsPassword(true)
			.HintText(LOCTEXT("ValueHint", "Value (never shown again). For several lines use Paste multi-line."))
			.IsEnabled_Lambda([State] { return !State->bPasted; })
			.OnTextChanged_Lambda([State](const FText& Text)
			{
				if (!State->bPasted)
				{
					State->Value = Text.ToString();
				}
			})
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("PasteMultiline", "Paste multi-line"))
				.ToolTipText(LOCTEXT("PasteMultilineTip", "Takes the clipboard as the value, line endings exactly as copied (for a PEM key). The value is not shown."))
				.OnClicked_Lambda([State]
				{
					FString Clip;
					FPlatformApplicationMisc::ClipboardPaste(Clip);
					State->Value = MoveTemp(Clip);
					State->bPasted = !State->Value.IsEmpty();
					if (State->ValueBox.IsValid())
					{
						State->ValueBox->SetText(State->bPasted ? FText::FromString(FString::Printf(TEXT("(%d characters pasted)"), State->Value.Len())) : FText::GetEmpty());
					}
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("ClearValue", "Clear value"))
				.OnClicked_Lambda([State] { State->ClearValue(); return FReply::Handled(); })
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("Save", "Save secret"))
				.ToolTipText(LOCTEXT("SaveTip", "Adds the secret, or replaces it when the name is already set."))
				.OnClicked_Lambda([State] { Save(State); return FReply::Handled(); })
			]
		]
	];

	// Delete: one button per name, below the form.
	Category.AddCustomRow(LOCTEXT("SecretsDeleteRow", "Delete"))
	.WholeRowContent()
	[
		SNew(SHorizontalBox)
		.Visibility_Lambda([State] { return State->bStoreOff ? EVisibility::Collapsed : EVisibility::Visible; })
		+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
		[
			SNew(SButton)
			.Text(LOCTEXT("DeleteTyped", "Delete the secret named above"))
			.IsEnabled_Lambda([State] { return !State->bBusy && State->NameExists(MantlePlaceSecrets::NormalizeName(State->NameText)); })
			.OnClicked_Lambda([State] { Remove(State, MantlePlaceSecrets::NormalizeName(State->NameText)); return FReply::Handled(); })
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SButton)
			.Text(LOCTEXT("LocalFile", "Create or open secrets.local.json"))
			.ToolTipText(LOCTEXT("LocalFileTip", "Test values for Test Locally: {\"NAME\": \"value\"}. Kept out of git, copied into the preview folder while it runs, never uploaded."))
			.OnClicked_Lambda([State] { OpenLocalFile(State); return FReply::Handled(); })
		]
	];
}

#undef LOCTEXT_NAMESPACE
