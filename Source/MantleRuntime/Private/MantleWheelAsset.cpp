// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWheelAsset.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceParser.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleWheelAssetPrivate
{
	constexpr int32 MaxNameLength = 64;
	constexpr int32 MaxMaterials = 64;
	constexpr double MinRadius = 5.0;
	constexpr double MaxRadius = 150.0;
	constexpr double MinWidth = 2.0;
	constexpr double MaxWidth = 100.0;

	void AddError(TArray<FMantleWheelDocError>& Errors, const FString& Pointer, const FString& Message)
	{
		Errors.Add(FMantleWheelDocError{ Pointer, Message });
	}

	bool ReadUploadedId(const TSharedPtr<FJsonValue>& Value, const FString& At, TArray<FMantleWheelDocError>& Errors, FString& Out)
	{
		FString Text;
		if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Text))
		{
			AddError(Errors, At, TEXT("must be text: an uploaded asset id (ast_u and 32 lowercase hex digits)"));
			return false;
		}
		if (!FMantlePlaceAssets::IsUploadedId(Text))
		{
			AddError(Errors, At, FString::Printf(TEXT("is '%s', which is not an uploaded asset id (ast_u and 32 lowercase hex digits)"), *Text));
			return false;
		}
		Out = Text;
		return true;
	}

	bool ReadRanged(const TSharedPtr<FJsonObject>& Root, const TCHAR* Key, const double Min, const double Max,
		TArray<FMantleWheelDocError>& Errors, double& Out)
	{
		const FString At = FString(TEXT("/")) + Key;
		const TSharedPtr<FJsonValue> Value = Root->TryGetField(Key);
		if (!Value.IsValid() || Value->IsNull())
		{
			AddError(Errors, At, TEXT("is required"));
			return false;
		}
		double Number = 0.0;
		if (Value->Type != EJson::Number || !Value->TryGetNumber(Number) || !FMath::IsFinite(Number))
		{
			AddError(Errors, At, TEXT("must be a number (cm)"));
			return false;
		}
		if (Number < Min || Number > Max)
		{
			AddError(Errors, At, FString::Printf(TEXT("is %g; it must be from %g to %g (cm)"), Number, Min, Max));
			return false;
		}
		Out = Number;
		return true;
	}
}

FString FMantleWheelDocError::ToString() const
{
	return FString::Printf(TEXT("%s: %s"), Pointer.IsEmpty() ? TEXT("(document)") : *Pointer, *Message);
}

bool FMantleWheelDocument::Parse(const FString& Json, FMantleWheelDesc& Out, TArray<FMantleWheelDocError>& OutErrors)
{
	using namespace MantleWheelAssetPrivate;
	Out = FMantleWheelDesc();
	OutErrors.Reset();
	if (Json.Len() > MaxBytes)
	{
		AddError(OutErrors, FString(), FString::Printf(TEXT("is %d characters long; a wheel document is at most %d"), Json.Len(), MaxBytes));
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(JsonReader, Root) || !Root.IsValid())
	{
		AddError(OutErrors, FString(), FString::Printf(TEXT("is not a JSON object (%s)"), *JsonReader->GetErrorMessage()));
		return false;
	}

	static const TCHAR* const Known[] = { TEXT("schemaVersion"), TEXT("name"), TEXT("mesh"), TEXT("materials"), TEXT("referenceRadius"),
		TEXT("referenceWidth"), TEXT("wheelSlots") };
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Root->Values)
	{
		bool bKnown = false;
		for (const TCHAR* Key : Known)
		{
			bKnown = bKnown || Pair.Key.Equals(Key, ESearchCase::CaseSensitive);
		}
		if (!bKnown)
		{
			AddError(OutErrors, FString(TEXT("/")) + Pair.Key, TEXT("is not a wheel field; remove it"));
		}
	}

	FMantleWheelDesc Desc;

	// schemaVersion: exactly 1.
	{
		const TSharedPtr<FJsonValue> Value = Root->TryGetField(TEXT("schemaVersion"));
		double Number = 0.0;
		if (!Value.IsValid() || Value->Type != EJson::Number || !Value->TryGetNumber(Number) || Number != 1.0)
		{
			AddError(OutErrors, TEXT("/schemaVersion"), TEXT("must be the number 1"));
		}
	}

	// name: 1 to 64 characters.
	{
		const TSharedPtr<FJsonValue> Value = Root->TryGetField(TEXT("name"));
		FString Name;
		if (!Value.IsValid() || Value->Type != EJson::String || !Value->TryGetString(Name))
		{
			AddError(OutErrors, TEXT("/name"), TEXT("is required text, 1 to 64 characters"));
		}
		else
		{
			const int32 Length = FMantlePlaceParser::CodePointLength(Name);
			if (Length < 1 || Length > MaxNameLength)
			{
				AddError(OutErrors, TEXT("/name"), FString::Printf(TEXT("is %d characters; it must be 1 to %d"), Length, MaxNameLength));
			}
			else
			{
				Desc.Name = Name;
			}
		}
	}

	// mesh: an uploaded id.
	{
		const TSharedPtr<FJsonValue> Value = Root->TryGetField(TEXT("mesh"));
		if (!Value.IsValid() || Value->IsNull())
		{
			AddError(OutErrors, TEXT("/mesh"), TEXT("is required: the uploaded static mesh's asset id"));
		}
		else
		{
			ReadUploadedId(Value, TEXT("/mesh"), OutErrors, Desc.MeshId);
		}
	}

	// materials: optional, at most 64 uploaded ids.
	{
		const TSharedPtr<FJsonValue> Value = Root->TryGetField(TEXT("materials"));
		if (Value.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
			if (Value->Type != EJson::Array || !Value->TryGetArray(List) || !List)
			{
				AddError(OutErrors, TEXT("/materials"), TEXT("must be a list of uploaded material ids"));
			}
			else if (List->Num() > MaxMaterials)
			{
				AddError(OutErrors, TEXT("/materials"), FString::Printf(TEXT("lists %d materials; at most %d"), List->Num(), MaxMaterials));
			}
			else
			{
				for (int32 Index = 0; Index < List->Num(); ++Index)
				{
					FString Id;
					if (ReadUploadedId((*List)[Index], FString::Printf(TEXT("/materials/%d"), Index), OutErrors, Id))
					{
						Desc.MaterialIds.Add(Id);
					}
				}
			}
		}
	}

	ReadRanged(Root, TEXT("referenceRadius"), MinRadius, MaxRadius, OutErrors, Desc.ReferenceRadius);
	ReadRanged(Root, TEXT("referenceWidth"), MinWidth, MaxWidth, OutErrors, Desc.ReferenceWidth);

	// wheelSlots: optional, whole numbers from 0, each once.
	{
		const TSharedPtr<FJsonValue> Value = Root->TryGetField(TEXT("wheelSlots"));
		if (Value.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
			if (Value->Type != EJson::Array || !Value->TryGetArray(List) || !List)
			{
				AddError(OutErrors, TEXT("/wheelSlots"), TEXT("must be a list of mesh material slot numbers"));
			}
			else
			{
				for (int32 Index = 0; Index < List->Num(); ++Index)
				{
					const FString At = FString::Printf(TEXT("/wheelSlots/%d"), Index);
					double Number = 0.0;
					const TSharedPtr<FJsonValue>& Item = (*List)[Index];
					if (!Item.IsValid() || Item->Type != EJson::Number || !Item->TryGetNumber(Number) || !FMath::IsFinite(Number)
						|| Number != FMath::FloorToDouble(Number) || Number < 0.0 || Number > 2147483647.0)
					{
						AddError(OutErrors, At, TEXT("must be a whole number from 0"));
						continue;
					}
					const int32 Slot = static_cast<int32>(Number);
					if (Desc.WheelSlots.Contains(Slot))
					{
						AddError(OutErrors, At, FString::Printf(TEXT("repeats slot %d; list each slot once"), Slot));
						continue;
					}
					Desc.WheelSlots.Add(Slot);
				}
			}
		}
	}

	// The rule across fields: with materials listed, every wheel slot is one of them.
	if (OutErrors.IsEmpty() && Desc.MaterialIds.Num() > 0)
	{
		for (int32 Index = 0; Index < Desc.WheelSlots.Num(); ++Index)
		{
			if (Desc.WheelSlots[Index] >= Desc.MaterialIds.Num())
			{
				AddError(OutErrors, FString::Printf(TEXT("/wheelSlots/%d"), Index),
					FString::Printf(TEXT("is slot %d, but the wheel lists %d materials; a wheel slot must be below that"),
						Desc.WheelSlots[Index], Desc.MaterialIds.Num()));
			}
		}
	}

	if (!OutErrors.IsEmpty())
	{
		return false;
	}
	Out = MoveTemp(Desc);
	return true;
}

bool FMantleWheelDocument::Parse(const FString& Json, FMantleWheelDesc& Out, FString& OutError)
{
	TArray<FMantleWheelDocError> Errors;
	const bool bParsed = Parse(Json, Out, Errors);
	TArray<FString> Lines;
	for (const FMantleWheelDocError& Error : Errors)
	{
		Lines.Add(Error.ToString());
	}
	OutError = FString::Join(Lines, TEXT("\n"));
	return bParsed;
}

TArray<FString> FMantleWheelAsset::NamedIds() const
{
	TArray<FString> Ids;
	if (!Desc.MeshId.IsEmpty())
	{
		Ids.AddUnique(Desc.MeshId);
	}
	for (const FString& Id : Desc.MaterialIds)
	{
		if (!Id.IsEmpty())
		{
			Ids.AddUnique(Id);
		}
	}
	return Ids;
}

bool FMantleWheelAsset::Read(const TConstArrayView<uint8> Bytes, FMantleWheelAsset& Out, FString& OutError)
{
	Out = FMantleWheelAsset();
	if (Bytes.IsEmpty() || Bytes.Num() > FMantleWheelDocument::MaxBytes)
	{
		OutError = FString::Printf(TEXT("is %d bytes; a wheel document is 1 to %d"), Bytes.Num(), FMantleWheelDocument::MaxBytes);
		return false;
	}
	int32 Start = 0;
	if (Bytes.Num() >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF)
	{
		Start = 3;
	}
	const auto Converted = StringCast<TCHAR>(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData() + Start), Bytes.Num() - Start);
	const FString Text = FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());

	FMantleWheelDesc Desc;
	if (!FMantleWheelDocument::Parse(Text, Desc, OutError))
	{
		return false;
	}
	Out.Text = Text;
	Out.Desc = MoveTemp(Desc);
	return true;
}
