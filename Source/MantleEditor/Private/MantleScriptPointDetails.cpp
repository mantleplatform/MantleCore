// Copyright Joshua Eger. All Rights Reserved.
#include "MantleScriptPointDetails.h"
#include "MantlePlaceSettingsActor.h"
#include "MantlePlaceStorage.h"
#include "MantleScriptOptionRows.h"
#include "MantleScriptPackages.h"
#include "MantleScriptPoint.h"

#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Engine/World.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "ScopedTransaction.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MantleScriptPointDetails"

FString FMantleScriptPointChoice::Label() const
{
	FString Shown = Name;
	if (bHasManifest && !Manifest.Name.IsEmpty() && !Manifest.Name.Equals(Name, ESearchCase::CaseSensitive))
	{
		Shown += FString::Printf(TEXT("  -  %s"), *Manifest.Name);
	}
	if (bLocal && !bStored)
	{
		Shown += TEXT("  (in Packages/, stored when you publish)");
	}
	else if (bLocal)
	{
		Shown += TEXT("  (in Packages/)");
	}
	else if (Kind.IsEmpty())
	{
		Shown += TEXT("  (typed in by hand: Mantle checks it is a script package when you publish)");
	}
	return Shown;
}

TSharedRef<IDetailCustomization> FMantleScriptPointDetails::MakeInstance()
{
	return MakeShared<FMantleScriptPointDetails>();
}

void FMantleScriptPointDetails::Register()
{
	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	PropertyEditor.RegisterCustomClassLayout(AMantleScriptPoint::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FMantleScriptPointDetails::MakeInstance));
	// Place Settings > Storage rows draw a script package's settings with the same rows as a point's parameters.
	PropertyEditor.RegisterCustomPropertyTypeLayout(FMantleStorageSetting::StaticStruct()->GetFName(),
		FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FMantleStorageSettingCustomization::MakeInstance));
	PropertyEditor.NotifyCustomizationModuleChanged();
}

void FMantleScriptPointDetails::Unregister()
{
	if (FModuleManager::Get().IsModuleLoaded(TEXT("PropertyEditor")) && UObjectInitialized())
	{
		FPropertyEditorModule& PropertyEditor = FModuleManager::GetModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
		PropertyEditor.UnregisterCustomClassLayout(AMantleScriptPoint::StaticClass()->GetFName());
		PropertyEditor.UnregisterCustomPropertyTypeLayout(FMantleStorageSetting::StaticStruct()->GetFName());
		PropertyEditor.NotifyCustomizationModuleChanged();
	}
}

TArray<FMantleScriptPointChoice> FMantleScriptPointDetails::MakeChoices(const TArray<FMantleStorageSetting>& Storage, const TArray<FMantleLocalScriptPackage>& Local)
{
	TArray<FMantleScriptPointChoice> ScriptEntries;
	TArray<FMantleScriptPointChoice> UnknownEntries;
	TArray<FMantleScriptPointChoice> LocalOnly;
	auto FindLocal = [&Local](const FString& Name) -> const FMantleLocalScriptPackage*
	{
		return Local.FindByPredicate([&Name](const FMantleLocalScriptPackage& Package) { return Package.Name.Equals(Name, ESearchCase::CaseSensitive); });
	};
	for (const FMantleStorageSetting& Entry : Storage)
	{
		const bool bScript = Entry.Kind.Equals(FMantleStorage::KindScript, ESearchCase::CaseSensitive);
		if (!Entry.Kind.IsEmpty() && !bScript)
		{
			continue; // a vehicle or a weapon: never a Script Point's
		}
		FMantleScriptPointChoice Choice;
		Choice.Name = Entry.Name;
		Choice.Kind = Entry.Kind;
		Choice.bStored = true;
		if (const FMantleLocalScriptPackage* Package = FindLocal(Entry.Name))
		{
			Choice.bLocal = true;
			Choice.bHasManifest = Package->bHasManifest;
			Choice.Manifest = Package->Manifest;
		}
		if (!Choice.bHasManifest && !Entry.Manifest.IsEmpty())
		{
			Choice.Manifest = FMantleScriptPackages::ReadManifestForHints(Entry.Manifest);
			Choice.bHasManifest = true;
		}
		(bScript || Choice.bLocal ? ScriptEntries : UnknownEntries).Add(MoveTemp(Choice));
	}
	for (const FMantleLocalScriptPackage& Package : Local)
	{
		const bool bTaken = Storage.ContainsByPredicate([&Package](const FMantleStorageSetting& Entry)
		{
			return Entry.Name.Equals(Package.Name, ESearchCase::CaseSensitive);
		});
		if (bTaken)
		{
			continue; // one choice with its entry above, or an entry of another kind holds the name
		}
		FMantleScriptPointChoice& Choice = LocalOnly.AddDefaulted_GetRef();
		Choice.Name = Package.Name;
		Choice.bLocal = true;
		Choice.bHasManifest = Package.bHasManifest;
		Choice.Manifest = Package.Manifest;
	}
	TArray<FMantleScriptPointChoice> Out = MoveTemp(ScriptEntries);
	Out.Append(MoveTemp(LocalOnly));
	Out.Append(MoveTemp(UnknownEntries));
	return Out;
}

TArray<FMantleScriptPointChoice> FMantleScriptPointDetails::GatherChoices(UWorld* World)
{
	TArray<FMantleStorageSetting> Storage;
	if (const AMantlePlaceSettingsActor* Settings = AMantlePlaceSettingsActor::Find(World))
	{
		Storage = Settings->Storage;
	}
	TArray<FMantleLocalScriptPackage> Local;
	TArray<FMantleScriptPackageProblem> Ignored;
	FMantleScriptPackages::Collect(FMantleScriptPackages::GetPackagesFolder(World), Local, Ignored);
	return MakeChoices(Storage, Local);
}

void FMantleScriptPointDetails::CustomizeDetails(const TSharedPtr<IDetailLayoutBuilder>& DetailBuilder)
{
	Layout = DetailBuilder;
	if (DetailBuilder.IsValid())
	{
		CustomizeDetails(*DetailBuilder);
	}
}

TOptional<FString> FMantleScriptPointDetails::GetParameter(const FString& Key) const
{
	for (const TWeakObjectPtr<AMantleScriptPoint>& Weak : Points)
	{
		if (const AMantleScriptPoint* Point = Weak.Get())
		{
			if (const FString* Value = Point->Parameters.Find(Key))
			{
				return *Value;
			}
			return TOptional<FString>();
		}
	}
	return TOptional<FString>();
}

void FMantleScriptPointDetails::SetParameter(const FString& Key, const TOptional<FString>& Value)
{
	const FScopedTransaction Transaction(LOCTEXT("SetParameterUndo", "Set Script Point parameter"));
	FProperty* Property = FindFProperty<FProperty>(AMantleScriptPoint::StaticClass(), GET_MEMBER_NAME_CHECKED(AMantleScriptPoint, Parameters));
	for (const TWeakObjectPtr<AMantleScriptPoint>& Weak : Points)
	{
		AMantleScriptPoint* Point = Weak.Get();
		if (!Point)
		{
			continue;
		}
		Point->Modify();
		Point->PreEditChange(Property);
		if (Value.IsSet())
		{
			Point->Parameters.Add(Key, Value.GetValue());
		}
		else
		{
			Point->Parameters.Remove(Key);
		}
		FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
		// Protected on the point; UObject's is public and virtual.
		static_cast<UObject*>(Point)->PostEditChangeProperty(Event);
	}
}

void FMantleScriptPointDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	Points.Reset();
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	for (const TWeakObjectPtr<UObject>& Object : Objects)
	{
		if (AMantleScriptPoint* Point = Cast<AMantleScriptPoint>(Object.Get()))
		{
			Points.Add(Point);
		}
	}
	if (Points.Num() == 0)
	{
		return;
	}
	AMantleScriptPoint* First = Points[0].Get();
	UWorld* World = First ? First->GetWorld() : nullptr;

	Choices.Reset();
	for (FMantleScriptPointChoice& Choice : GatherChoices(World))
	{
		Choices.Add(MakeShared<FMantleScriptPointChoice>(MoveTemp(Choice)));
	}

	StorageHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(AMantleScriptPoint, StorageItem));
	DetailBuilder.HideProperty(StorageHandle);
	IDetailCategoryBuilder& Category = DetailBuilder.EditCategory(TEXT("Script"));

	// The current value, which may be none of the choices (typed earlier, or its entry since removed).
	FString Current;
	const bool bSingleValue = StorageHandle.IsValid() && StorageHandle->GetValue(Current) == FPropertyAccess::Success;
	TSharedPtr<FMantleScriptPointChoice> Selected;
	for (const TSharedPtr<FMantleScriptPointChoice>& Choice : Choices)
	{
		if (bSingleValue && Choice->Name.Equals(Current, ESearchCase::CaseSensitive))
		{
			Selected = Choice;
		}
	}

	const TWeakPtr<IDetailLayoutBuilder> WeakLayout = Layout;
	TSharedPtr<IPropertyHandle> Handle = StorageHandle;
	const FText CurrentText = !bSingleValue ? LOCTEXT("MultipleValues", "Multiple values")
		: Selected.IsValid() ? FText::FromString(Selected->Label())
		: Current.IsEmpty() ? LOCTEXT("NoPackage", "Pick a script package")
		: FText::Format(LOCTEXT("NotAChoice", "{0}  (not a script package of this place)"), FText::FromString(Current));

	TSharedRef<SWidget> NameWidget = SNullWidget::NullWidget;
	if (StorageHandle.IsValid())
	{
		NameWidget = StorageHandle->CreatePropertyNameWidget();
	}
	else
	{
		const TSharedRef<STextBlock> NameText = SNew(STextBlock).Text(LOCTEXT("StorageItem", "Storage Item"));
		NameWidget = NameText;
	}
	Category.AddCustomRow(LOCTEXT("StorageItemFilter", "Storage Item"))
		.NameContent()
		[
			NameWidget
		]
		.ValueContent()
		.MinDesiredWidth(260.0f)
		.MaxDesiredWidth(480.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SComboBox<TSharedPtr<FMantleScriptPointChoice>>)
				.OptionsSource(&Choices)
				.InitiallySelectedItem(Selected)
				.OnGenerateWidget_Lambda([](TSharedPtr<FMantleScriptPointChoice> Choice) -> TSharedRef<SWidget>
				{
					return SNew(STextBlock).Text(FText::FromString(Choice.IsValid() ? Choice->Label() : FString()));
				})
				.OnSelectionChanged_Lambda([Handle, WeakLayout](TSharedPtr<FMantleScriptPointChoice> Choice, ESelectInfo::Type SelectInfo)
				{
					if (!Choice.IsValid() || SelectInfo == ESelectInfo::Direct || !Handle.IsValid())
					{
						return;
					}
					Handle->SetValue(Choice->Name);
					// The parameter rows below are the new package's.
					if (const TSharedPtr<IDetailLayoutBuilder> Builder = WeakLayout.Pin())
					{
						Builder->ForceRefreshDetails();
					}
				})
				[
					SNew(STextBlock)
					.Font(IDetailLayoutBuilder::GetDetailFont())
					.Text(CurrentText)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
			[
				SNew(STextBlock)
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Visibility(Choices.Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed)
				.Text(LOCTEXT("NoChoices", "This place has no script package. Add one with Tools > Mantle > Library (Scripts), or make one in a Packages folder beside the level's Scripts folder."))
			]
		];

	// The chosen package's declared parameters, each with its type, default and help, writing into Parameters.
	if (!Selected.IsValid() || !Selected->bHasManifest || Selected->Manifest.Parameters.Num() == 0)
	{
		return;
	}
	const FMantleScriptPackageManifest Manifest = Selected->Manifest;
	if (!Manifest.Description.IsEmpty())
	{
		Category.AddCustomRow(LOCTEXT("PackageDescriptionFilter", "Package"))
			.WholeRowContent()
			[
				SNew(STextBlock)
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(FText::FromString(Manifest.Description))
			];
	}
	const TWeakPtr<FMantleScriptPointDetails> WeakThis = StaticCastSharedRef<FMantleScriptPointDetails>(AsShared());
	// The same rows a storage entry's settings draw (a dropdown for a choice, a slider for a number with min and max,
	// the required marker and a red note), writing into the point's Parameters.
	const FString Package = Selected->Name;
	for (const FMantleScriptPackageParameter& Parameter : Manifest.Parameters)
	{
		const FString Key = Parameter.Name;
		FMantleScriptOptionRows::FGetter Get = [WeakThis, Key]() -> TOptional<FString>
		{
			const TSharedPtr<FMantleScriptPointDetails> Details = WeakThis.Pin();
			return Details.IsValid() ? Details->GetParameter(Key) : TOptional<FString>();
		};
		FMantleScriptOptionRows::FSetter Set = [WeakThis, Key](const TOptional<FString>& Value)
		{
			if (const TSharedPtr<FMantleScriptPointDetails> Details = WeakThis.Pin())
			{
				Details->SetParameter(Key, Value);
			}
		};
		FMantleScriptOptionRows::FillRow(Category.AddCustomRow(FText::FromString(Key)), Package, Parameter, MoveTemp(Get), MoveTemp(Set));
	}
}

#undef LOCTEXT_NAMESPACE
