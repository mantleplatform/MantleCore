// Copyright Joshua Eger. All Rights Reserved.
#include "MantleScriptOptionRows.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceSettingsActor.h"
#include "MantlePlaceStorage.h"
#include "MantleScriptPackages.h"

#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "IDetailChildrenBuilder.h"
#include "PropertyHandle.h"
#include "ScopedTransaction.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MantleScriptOptionRows"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleScriptOptionRowsPrivate
{
	/** The red of a row's note. */
	const FLinearColor NoteColor(0.95f, 0.3f, 0.3f);

	/** The stored value of exactly Key (TMap's keys ignore case), or null. */
	const FString* FindExactSetting(const TMap<FString, FString>& Settings, const FString& Key)
	{
		for (const TPair<FString, FString>& Pair : Settings)
		{
			if (Pair.Key.Equals(Key, ESearchCase::CaseSensitive))
			{
				return &Pair.Value;
			}
		}
		return nullptr;
	}

	/** The first entry a storage row's handle edits, or null. */
	FMantleStorageSetting* FirstStorageEntry(const TSharedRef<IPropertyHandle>& Handle)
	{
		TArray<void*> RawData;
		Handle->AccessRawData(RawData);
		for (void* Data : RawData)
		{
			if (Data)
			{
				return static_cast<FMantleStorageSetting*>(Data);
			}
		}
		return nullptr;
	}
}

FMantleScriptOptionRow FMantleScriptOptionRow::Make(const FString& Package, const FMantleScriptPackageParameter& Option, const TOptional<FString>& Value)
{
	FMantleScriptOptionRow Row;
	Row.Key = Option.Name;
	Row.Help = Option.Help;
	const FString TypeName = FMantleScriptPackages::ParameterTypeName(Option.Type);

	TArray<FString> Details;
	Details.Add(TypeName);
	if (Option.Type == EMantleScriptPackageParameterType::Number)
	{
		if (Option.bHasMin && Option.bHasMax)
		{
			Details.Add(FString::Printf(TEXT("%s to %s"), *FMantleScriptPackages::FormatOptionNumber(Option.Min), *FMantleScriptPackages::FormatOptionNumber(Option.Max)));
		}
		else if (Option.bHasMin)
		{
			Details.Add(FString::Printf(TEXT("at least %s"), *FMantleScriptPackages::FormatOptionNumber(Option.Min)));
		}
		else if (Option.bHasMax)
		{
			Details.Add(FString::Printf(TEXT("at most %s"), *FMantleScriptPackages::FormatOptionNumber(Option.Max)));
		}
	}
	if (Option.bRequired)
	{
		Details.Add(TEXT("required"));
	}
	Row.Label = FString::Printf(TEXT("%s (%s)"), *Option.Name, *FString::Join(Details, TEXT(", ")));
	Row.Tooltip = FString::Printf(TEXT("%s (%s)%s%s"), *Option.Name, *TypeName, Option.Help.IsEmpty() ? TEXT("") : TEXT(": "), *Option.Help);
	if (Option.bHasDefault && !Option.Default.IsEmpty())
	{
		Row.Hint = FString::Printf(TEXT("%s (default)"), *Option.Default);
	}
	Row.bIsSet = Value.IsSet();
	Row.Shown = Value.IsSet() ? Value.GetValue() : Option.bHasDefault ? Option.Default : FString();

	switch (Option.Type)
	{
	case EMantleScriptPackageParameterType::Boolean:
		Row.Widget = EMantleScriptOptionWidget::CheckBox;
		break;
	case EMantleScriptPackageParameterType::Choice:
		Row.Widget = EMantleScriptOptionWidget::Dropdown;
		Row.Choices = Option.Options;
		break;
	case EMantleScriptPackageParameterType::Number:
		Row.Widget = Option.bHasMin && Option.bHasMax ? EMantleScriptOptionWidget::Slider : EMantleScriptOptionWidget::Number;
		Row.SliderMin = Option.Min;
		Row.SliderMax = Option.Max;
		break;
	case EMantleScriptPackageParameterType::String:
		Row.Widget = EMantleScriptOptionWidget::Text;
		break;
	}

	if (Option.NeedsValue() && (!Value.IsSet() || Value.GetValue().IsEmpty()))
	{
		Row.Note = FString::Printf(TEXT("%s needs %s."), *Package, *Option.Name);
	}
	else if (Value.IsSet())
	{
		Row.Note = FMantleScriptPackages::OptionValueProblem(Package, Option, Value.GetValue());
	}
	return Row;
}

bool FMantleScriptOptionRow::Commit(const FMantleScriptPackageParameter& Option, const FString& Text, TOptional<FString>& OutValue)
{
	OutValue.Reset();
	if (Text.IsEmpty())
	{
		return true; // left out: the default applies
	}
	if (FMantlePlaceParser::CodePointLength(Text) > FMantlePlaceLimits::MaxStorageSettingValueLength)
	{
		return false;
	}
	switch (Option.Type)
	{
	case EMantleScriptPackageParameterType::Number:
	{
		double Number = 0.0;
		if (!FMantleScriptPackages::ParseOptionNumber(Text, Number))
		{
			return false;
		}
		break;
	}
	case EMantleScriptPackageParameterType::Boolean:
		if (!Text.Equals(TEXT("true"), ESearchCase::CaseSensitive) && !Text.Equals(TEXT("false"), ESearchCase::CaseSensitive))
		{
			return false;
		}
		break;
	case EMantleScriptPackageParameterType::Choice:
		if (!Option.HasOption(Text))
		{
			return false;
		}
		break;
	case EMantleScriptPackageParameterType::String:
		break;
	}
	OutValue = Text;
	return true;
}

void FMantleScriptOptionRows::FillRow(FDetailWidgetRow& Row, const FString& Package, const FMantleScriptPackageParameter& Option, FGetter Get, FSetter Set)
{
	using namespace MantleScriptOptionRowsPrivate;
	const FMantleScriptPackageParameter Declared = Option;
	const FMantleScriptOptionRow Initial = FMantleScriptOptionRow::Make(Package, Declared, Get());
	// Recomputed whenever Slate asks, so the row follows undo, redo and edits made elsewhere.
	const TFunction<FMantleScriptOptionRow()> Current = [Package, Declared, Get]()
	{
		return FMantleScriptOptionRow::Make(Package, Declared, Get());
	};
	const FText Tooltip = FText::FromString(Initial.Tooltip);

	TSharedRef<SWidget> Editor = SNullWidget::NullWidget;
	switch (Initial.Widget)
	{
	case EMantleScriptOptionWidget::CheckBox:
	{
		const TSharedRef<SCheckBox> CheckBox = SNew(SCheckBox)
			.ToolTipText(Tooltip)
			.IsChecked_Lambda([Current]
			{
				return Current().Shown.Equals(TEXT("true"), ESearchCase::CaseSensitive) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
			})
			.OnCheckStateChanged_Lambda([Set](ECheckBoxState State)
			{
				Set(FString(State == ECheckBoxState::Checked ? TEXT("true") : TEXT("false")));
			});
		Editor = CheckBox;
		break;
	}
	case EMantleScriptOptionWidget::Dropdown:
	{
		// The combo keeps a pointer to its options: the lambdas below hold them for as long as the widget lives.
		const TSharedRef<TArray<TSharedPtr<FString>>> Items = MakeShared<TArray<TSharedPtr<FString>>>();
		TSharedPtr<FString> Selected;
		for (const FString& Choice : Initial.Choices)
		{
			const TSharedPtr<FString> Item = MakeShared<FString>(Choice);
			if (Choice.Equals(Initial.Shown, ESearchCase::CaseSensitive))
			{
				Selected = Item;
			}
			Items->Add(Item);
		}
		const TSharedRef<SComboBox<TSharedPtr<FString>>> Combo = SNew(SComboBox<TSharedPtr<FString>>)
			.ToolTipText(Tooltip)
			.OptionsSource(&Items.Get())
			.InitiallySelectedItem(Selected)
			.OnGenerateWidget_Lambda([Items](TSharedPtr<FString> Item) -> TSharedRef<SWidget>
			{
				(void)Items; // held, not read: the combo's OptionsSource points into it
				return SNew(STextBlock).Text(FText::FromString(Item.IsValid() ? *Item : FString()));
			})
			.OnSelectionChanged_Lambda([Set](TSharedPtr<FString> Item, ESelectInfo::Type SelectInfo)
			{
				if (Item.IsValid() && SelectInfo != ESelectInfo::Direct)
				{
					Set(*Item);
				}
			})
			[
				SNew(STextBlock)
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.Text_Lambda([Current]
				{
					const FMantleScriptOptionRow Now = Current();
					return Now.Shown.IsEmpty() ? LOCTEXT("PickOne", "Pick one") : FText::FromString(Now.Shown);
				})
			];
		Editor = Combo;
		break;
	}
	case EMantleScriptOptionWidget::Slider:
	{
		const double Min = Initial.SliderMin;
		const double Max = Initial.SliderMax;
		const TSharedRef<SSpinBox<double>> Slider = SNew(SSpinBox<double>)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.ToolTipText(Tooltip)
			.MinValue(Min)
			.MaxValue(Max)
			.MinSliderValue(Min)
			.MaxSliderValue(Max)
			.Value_Lambda([Current, Min]
			{
				double Number = Min;
				return FMantleScriptPackages::ParseOptionNumber(Current().Shown, Number) ? Number : Min;
			})
			.OnValueCommitted_Lambda([Set](double Value, ETextCommit::Type)
			{
				Set(FMantleScriptPackages::FormatOptionNumber(Value));
			});
		Editor = Slider;
		break;
	}
	case EMantleScriptOptionWidget::Number:
	case EMantleScriptOptionWidget::Text:
	{
		const TSharedRef<SEditableTextBox> TextBox = SNew(SEditableTextBox)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.ToolTipText(Tooltip)
			.HintText(FText::FromString(Initial.Hint))
			.Text_Lambda([Get]
			{
				const TOptional<FString> Value = Get();
				return FText::FromString(Value.IsSet() ? Value.GetValue() : FString());
			})
			.OnTextCommitted_Lambda([Declared, Get, Set](const FText& Text, ETextCommit::Type)
			{
				TOptional<FString> Value;
				if (!FMantleScriptOptionRow::Commit(Declared, Text.ToString(), Value))
				{
					return; // not kept: the row shows the value it had (the tooltip says what it takes)
				}
				const TOptional<FString> Before = Get();
				const bool bSame = Before.IsSet() == Value.IsSet() && (!Value.IsSet() || Before.GetValue().Equals(Value.GetValue(), ESearchCase::CaseSensitive));
				if (!bSame)
				{
					Set(Value);
				}
			});
		Editor = TextBox;
		break;
	}
	}

	Row.NameContent()
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.ToolTipText(Tooltip)
			.Text(FText::FromString(Initial.Label))
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Visibility(Initial.Help.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
			.Text(FText::FromString(Initial.Help))
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor(NoteColor))
			.Visibility_Lambda([Current] { return Current().Note.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			.Text_Lambda([Current] { return FText::FromString(Current().Note); })
		]
	]
	.ValueContent()
	.MinDesiredWidth(160.0f)
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
		[
			Editor
		]
		+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0).VAlign(VAlign_Center)
		[
			SNew(SButton)
			.Text(LOCTEXT("UseDefault", "Default"))
			.ToolTipText(LOCTEXT("UseDefaultTip", "Clear this value, so the package's default applies."))
			.IsEnabled_Lambda([Get] { return Get().IsSet(); })
			.OnClicked_Lambda([Set]
			{
				Set(TOptional<FString>());
				return FReply::Handled();
			})
		]
	];
}

TSharedRef<IPropertyTypeCustomization> FMantleStorageSettingCustomization::MakeInstance()
{
	return MakeShared<FMantleStorageSettingCustomization>();
}

bool FMantleStorageSettingCustomization::ManifestFor(const FMantleStorageSetting& Entry, const TArray<FMantleLocalScriptPackage>& Local, FMantleScriptPackageManifest& OutManifest)
{
	OutManifest = FMantleScriptPackageManifest();
	const bool bScript = Entry.Kind.Equals(FMantleStorage::KindScript, ESearchCase::CaseSensitive);
	if (!Entry.Kind.IsEmpty() && !bScript)
	{
		return false; // a vehicle or a weapon takes no settings
	}
	const FString& Name = Entry.Name;
	const bool bLocal = Local.ContainsByPredicate([&Name](const FMantleLocalScriptPackage& Package) { return Package.Name.Equals(Name, ESearchCase::CaseSensitive); });
	if (!bScript && !bLocal)
	{
		return false; // typed in by hand: not known to be a script package
	}
	return FMantleScriptPackages::KnownManifest(Entry.Name, Local, Entry.Manifest, OutManifest);
}

void FMantleStorageSettingCustomization::CustomizeHeader(TSharedRef<IPropertyHandle> PropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& Utils)
{
	using namespace MantleScriptOptionRowsPrivate;
	HeaderRow
		.NameContent()
		[
			PropertyHandle->CreatePropertyNameWidget()
		]
		.ValueContent()
		.MinDesiredWidth(200.0f)
		[
			SNew(STextBlock)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.Text_Lambda([PropertyHandle]
			{
				const FMantleStorageSetting* Entry = FirstStorageEntry(PropertyHandle);
				if (!Entry)
				{
					return FText::GetEmpty();
				}
				return FText::FromString(Entry->Kind.IsEmpty()
					? FString::Printf(TEXT("%s  v%d"), *Entry->Name, Entry->Version)
					: FString::Printf(TEXT("%s  (%s, v%d)"), *Entry->Name, *Entry->Kind, Entry->Version));
			})
		];
}

void FMantleStorageSettingCustomization::CustomizeChildren(TSharedRef<IPropertyHandle> PropertyHandle, IDetailChildrenBuilder& ChildBuilder, IPropertyTypeCustomizationUtils& Utils)
{
	using namespace MantleScriptOptionRowsPrivate;
	const FMantleStorageSetting* Entry = FirstStorageEntry(PropertyHandle);

	// The level's Packages/ folders: a folder's package.json is what publishing stores under its name.
	TArray<FMantleLocalScriptPackage> Local;
	{
		TArray<UObject*> Outers;
		PropertyHandle->GetOuterObjects(Outers);
		const AActor* Owner = Outers.Num() > 0 ? Cast<AActor>(Outers[0]) : nullptr;
		UWorld* World = Owner ? Owner->GetWorld() : nullptr;
		if (World)
		{
			TArray<FMantleScriptPackageProblem> Ignored;
			FMantleScriptPackages::Collect(FMantleScriptPackages::GetPackagesFolder(World), Local, Ignored);
		}
	}
	FMantleScriptPackageManifest Manifest;
	const bool bTyped = Entry && ManifestFor(*Entry, Local, Manifest);
	// The plain map stays when it holds a key the manifest does not declare, so the creator can still see and remove it.
	bool bHideMap = bTyped;
	if (bTyped)
	{
		for (const TPair<FString, FString>& Pair : Entry->Settings)
		{
			bHideMap = bHideMap && Manifest.FindSetting(Pair.Key) != nullptr;
		}
	}

	const FName SettingsName = GET_MEMBER_NAME_CHECKED(FMantleStorageSetting, Settings);
	TSharedPtr<IPropertyHandle> SettingsHandle;
	uint32 Count = 0;
	PropertyHandle->GetNumChildren(Count);
	for (uint32 Index = 0; Index < Count; ++Index)
	{
		const TSharedPtr<IPropertyHandle> Child = PropertyHandle->GetChildHandle(Index);
		if (!Child.IsValid())
		{
			continue;
		}
		if (Child->GetProperty() && Child->GetProperty()->GetFName() == SettingsName)
		{
			SettingsHandle = Child;
			if (bHideMap)
			{
				continue;
			}
		}
		ChildBuilder.AddProperty(Child.ToSharedRef());
	}
	if (!bTyped || !SettingsHandle.IsValid() || Manifest.Settings.Num() == 0)
	{
		return;
	}

	const FString Package = Entry->Name;
	for (const FMantleScriptPackageParameter& Setting : Manifest.Settings)
	{
		const FString Key = Setting.Name;
		FMantleScriptOptionRows::FGetter Get = [PropertyHandle, Key]() -> TOptional<FString>
		{
			const FMantleStorageSetting* Current = FirstStorageEntry(PropertyHandle);
			const FString* Value = Current ? FindExactSetting(Current->Settings, Key) : nullptr;
			return Value ? TOptional<FString>(*Value) : TOptional<FString>();
		};
		FMantleScriptOptionRows::FSetter Set = [PropertyHandle, SettingsHandle, Key](const TOptional<FString>& Value)
		{
			const FScopedTransaction Transaction(LOCTEXT("SetStorageSettingUndo", "Set script package setting"));
			SettingsHandle->NotifyPreChange();
			TArray<void*> RawData;
			PropertyHandle->AccessRawData(RawData);
			for (void* Data : RawData)
			{
				if (!Data)
				{
					continue;
				}
				FMantleStorageSetting* Edited = static_cast<FMantleStorageSetting*>(Data);
				// Removed first, so a key spelt in another case gives way to the manifest's spelling.
				Edited->Settings.Remove(Key);
				if (Value.IsSet())
				{
					Edited->Settings.Add(Key, Value.GetValue());
				}
			}
			SettingsHandle->NotifyPostChange(EPropertyChangeType::ValueSet);
			SettingsHandle->NotifyFinishedChangingProperties();
		};
		FMantleScriptOptionRows::FillRow(ChildBuilder.AddCustomRow(FText::FromString(Key)), Package, Setting, MoveTemp(Get), MoveTemp(Set));
	}
}

#undef LOCTEXT_NAMESPACE
