// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWheelDocument.h"

#include "MantleVehicleRimFinder.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

TArray<int32> FMantleWheelDocument::ChooseWheelSlots(const TConstArrayView<FMantleWheelSlotFacts> Slots)
{
	TArray<int32> Out;
	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		const FMantleWheelSlotFacts& Slot = Slots[Index];
		if (Slot.bPaint || FMantleVehicleRims::NameIsRim(Slot.SlotName) || FMantleVehicleRims::NameIsRim(Slot.MaterialName))
		{
			Out.Add(Index);
		}
	}
	return Out;
}

bool FMantleWheelDocument::Build(const FString& Name, const FString& MeshId, const TArray<FString>& MaterialIds, const double ReferenceRadius,
	const double ReferenceWidth, const TArray<int32>& WheelSlots, FString& OutJson, FString& OutError)
{
	OutJson.Reset();
	const FString Trimmed = Name.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		OutError = TEXT("Give the wheel a name.");
		return false;
	}
	if (Trimmed.Len() > MaxNameLength)
	{
		OutError = FString::Printf(TEXT("The wheel's name is %d characters long; a wheel name can be at most %d."), Trimmed.Len(), MaxNameLength);
		return false;
	}
	if (MeshId.IsEmpty())
	{
		OutError = TEXT("The wheel's 3D model was not uploaded.");
		return false;
	}
	if (MaterialIds.Num() > MaxMaterials)
	{
		OutError = FString::Printf(TEXT("The wheel's model has %d materials; a wheel can have at most %d. Merge some of its materials, then publish again."),
			MaterialIds.Num(), MaxMaterials);
		return false;
	}
	for (int32 Index = 0; Index < MaterialIds.Num(); ++Index)
	{
		if (MaterialIds[Index].IsEmpty())
		{
			OutError = FString::Printf(TEXT("The wheel's material %d was not uploaded."), Index + 1);
			return false;
		}
	}
	if (!(ReferenceRadius >= MinRadius && ReferenceRadius <= MaxRadius))
	{
		OutError = FString::Printf(TEXT("The wheel measures %.1f cm from its axle to its outer edge; a wheel must be between %.0f and %.0f cm. Scale the model, then publish again."),
			ReferenceRadius, MinRadius, MaxRadius);
		return false;
	}
	if (!(ReferenceWidth >= MinWidth && ReferenceWidth <= MaxWidth))
	{
		OutError = FString::Printf(TEXT("The wheel measures %.1f cm along its axle; a wheel must be between %.0f and %.0f cm wide. Scale the model, then publish again."),
			ReferenceWidth, MinWidth, MaxWidth);
		return false;
	}
	TSet<int32> Seen;
	for (const int32 Slot : WheelSlots)
	{
		if (Slot < 0 || Slot >= MaterialIds.Num())
		{
			OutError = FString::Printf(TEXT("The wheel names material slot %d for its paint, but the model has %d material slot(s)."), Slot, MaterialIds.Num());
			return false;
		}
		bool bAlready = false;
		Seen.Add(Slot, &bAlready);
		if (bAlready)
		{
			OutError = FString::Printf(TEXT("The wheel names material slot %d for its paint twice."), Slot);
			return false;
		}
	}

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schemaVersion"), 1);
	Root->SetStringField(TEXT("name"), Trimmed);
	Root->SetStringField(TEXT("mesh"), MeshId);
	if (MaterialIds.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Materials;
		for (const FString& Id : MaterialIds)
		{
			Materials.Add(MakeShared<FJsonValueString>(Id));
		}
		Root->SetArrayField(TEXT("materials"), Materials);
	}
	Root->SetNumberField(TEXT("referenceRadius"), ReferenceRadius);
	Root->SetNumberField(TEXT("referenceWidth"), ReferenceWidth);
	if (WheelSlots.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Slots;
		for (const int32 Slot : WheelSlots)
		{
			Slots.Add(MakeShared<FJsonValueNumber>(Slot));
		}
		Root->SetArrayField(TEXT("wheelSlots"), Slots);
	}
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		OutError = TEXT("The wheel's settings could not be written.");
		OutJson.Reset();
		return false;
	}
	return true;
}
