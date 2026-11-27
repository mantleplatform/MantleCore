// Copyright Joshua Eger. All Rights Reserved.
#include "MantleVehicleSourceDetails.h"
#include "MantleVehicleSource.h"

#include "DetailLayoutBuilder.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"

TSharedRef<IDetailCustomization> FMantleVehicleSourceDetails::MakeInstance()
{
	return MakeShared<FMantleVehicleSourceDetails>();
}

void FMantleVehicleSourceDetails::Register()
{
	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	PropertyEditor.RegisterCustomClassLayout(UMantleVehicleSource::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FMantleVehicleSourceDetails::MakeInstance));
	PropertyEditor.NotifyCustomizationModuleChanged();
}

void FMantleVehicleSourceDetails::Unregister()
{
	if (FModuleManager::Get().IsModuleLoaded(TEXT("PropertyEditor")) && UObjectInitialized())
	{
		FPropertyEditorModule& PropertyEditor = FModuleManager::GetModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
		PropertyEditor.UnregisterCustomClassLayout(UMantleVehicleSource::StaticClass()->GetFName());
		PropertyEditor.NotifyCustomizationModuleChanged();
	}
}

void FMantleVehicleSourceDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	bool bAnyCanPick = false;
	for (const TWeakObjectPtr<UObject>& Object : Objects)
	{
		const UMantleVehicleSource* Source = Cast<UMantleVehicleSource>(Object.Get());
		bAnyCanPick = bAnyCanPick || (Source && Source->CanPickEngineSound());
	}
	if (!bAnyCanPick)
	{
		const TSharedRef<IPropertyHandle> Tuning = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UMantleVehicleSource, Tuning));
		if (const TSharedPtr<IPropertyHandle> Sound = Tuning->GetChildHandle(GET_MEMBER_NAME_CHECKED(FMantleVehicleSourceTuning, EngineSound)))
		{
			DetailBuilder.HideProperty(Sound);
		}
	}
	// Whether it can pick one follows the movement type and pedalling: show or hide it again when either changes.
	IDetailLayoutBuilder* Builder = &DetailBuilder;
	const FSimpleDelegate Refresh = FSimpleDelegate::CreateLambda([Builder]() { Builder->ForceRefreshDetails(); });
	DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UMantleVehicleSource, MovementType))->SetOnPropertyValueChanged(Refresh);
	const TSharedRef<IPropertyHandle> TuningHandle = DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UMantleVehicleSource, Tuning));
	if (const TSharedPtr<IPropertyHandle> Pedal = TuningHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FMantleVehicleSourceTuning, bPedal)))
	{
		Pedal->SetOnPropertyValueChanged(Refresh);
	}
}
