// Copyright Joshua Eger. All Rights Reserved.
#include "MantleItemSpawnDetails.h"
#include "MantleItemSpawnModel.h"
#include "MantleItemSpawnPoint.h"

#include "AssetRegistry/AssetData.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Engine/StaticMesh.h"
#include "Modules/ModuleManager.h"
#include "PropertyCustomizationHelpers.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MantleItemSpawnDetails"

TSharedRef<IDetailCustomization> FMantleItemSpawnDetails::MakeInstance()
{
	return MakeShared<FMantleItemSpawnDetails>();
}

void FMantleItemSpawnDetails::Register()
{
	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	PropertyEditor.RegisterCustomClassLayout(AMantleItemSpawnPoint::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FMantleItemSpawnDetails::MakeInstance));
	PropertyEditor.NotifyCustomizationModuleChanged();
}

void FMantleItemSpawnDetails::Unregister()
{
	if (FModuleManager::Get().IsModuleLoaded(TEXT("PropertyEditor")) && UObjectInitialized())
	{
		FPropertyEditorModule& PropertyEditor = FModuleManager::GetModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
		PropertyEditor.UnregisterCustomClassLayout(AMantleItemSpawnPoint::StaticClass()->GetFName());
		PropertyEditor.NotifyCustomizationModuleChanged();
	}
}

void FMantleItemSpawnDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	const TSharedRef<IPropertyHandle> ModelHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(AMantleItemSpawnPoint, ModelId));
	DetailBuilder.HideProperty(ModelHandle);
	IDetailCategoryBuilder& Category = DetailBuilder.EditCategory(TEXT("Item"));

	Category.AddCustomRow(LOCTEXT("ModelFilter", "Model"))
		.NameContent()
		[
			SNew(STextBlock)
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.Text(LOCTEXT("Model", "Model"))
			.ToolTipText(LOCTEXT("ModelTip", "The Static Mesh the item lies as in the world. Leave empty for the model the item's script gave it. The mesh is sent with your place when you publish."))
		]
		.ValueContent()
		.MinDesiredWidth(260.0f)
		.MaxDesiredWidth(480.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SObjectPropertyEntryBox)
				.AllowedClass(UStaticMesh::StaticClass())
				.AllowClear(true)
				.DisplayThumbnail(false)
				.ObjectPath_Lambda([ModelHandle]() -> FString
				{
					FString Current;
					if (ModelHandle->GetValue(Current) == FPropertyAccess::Success && FMantleItemSpawnModel::IsMeshPath(Current))
					{
						return Current;
					}
					return FString();
				})
				.OnObjectChanged_Lambda([ModelHandle](const FAssetData& Asset)
				{
					ModelHandle->SetValue(Asset.IsValid() ? Asset.GetObjectPathString() : FString());
				})
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
			[
				SNew(STextBlock)
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Visibility_Lambda([ModelHandle]() -> EVisibility
				{
					FString Current;
					const bool bAssetId = ModelHandle->GetValue(Current) == FPropertyAccess::Success && !Current.IsEmpty() && !FMantleItemSpawnModel::IsMeshPath(Current);
					return bAssetId ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.Text_Lambda([ModelHandle]() -> FText
				{
					FString Current;
					ModelHandle->GetValue(Current);
					return FText::Format(LOCTEXT("ModelFromPlaceFile", "Model from a place file: {0}. Pick a mesh to replace it."), FText::FromString(Current));
				})
			]
		];
}

#undef LOCTEXT_NAMESPACE
