// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCosmeticItem.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MantleApiClient.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Named, not anonymous: unity builds. The readers are FMantleClientApi's (MantleClientApi.cpp), moved with the item
// types; the generic ones (ParseRoot ... OptionalBytes) stay there too for the client's other parsers.
namespace MantleCosmeticItemPrivate
{
	TSharedPtr<FJsonObject> ParseRoot(const FString& Json, const TCHAR* What, FString& OutWhyNot)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
		{
			OutWhyNot = FString::Printf(TEXT("%s is not a JSON object"), What);
			return nullptr;
		}
		return Object;
	}

	/** A required string field. */
	bool RequireString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, const FString& Where,
		FString& Out, FString& OutWhyNot)
	{
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(Name);
		if (!Value.IsValid() || Value->Type != EJson::String)
		{
			OutWhyNot = FString::Printf(TEXT("%s%s must be a string"), *Where, Name);
			return false;
		}
		Out = Value->AsString();
		return true;
	}

	/** An optional string: absent or null leaves Out empty, a string is read, anything else fails. */
	bool OptionalString(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, const FString& Where,
		FString& Out, FString& OutWhyNot)
	{
		Out.Empty();
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(Name);
		if (!Value.IsValid() || Value->Type == EJson::Null)
		{
			return true;
		}
		if (Value->Type != EJson::String)
		{
			OutWhyNot = FString::Printf(TEXT("%s%s must be a string or null"), *Where, Name);
			return false;
		}
		Out = Value->AsString();
		return true;
	}

	/** An optional boolean: absent leaves Out alone, a bool is read, anything else fails. */
	bool OptionalBool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, const FString& Where,
		bool& Out, FString& OutWhyNot)
	{
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(Name);
		if (!Value.IsValid())
		{
			return true;
		}
		if (Value->Type != EJson::Boolean)
		{
			OutWhyNot = FString::Printf(TEXT("%s%s must be true or false"), *Where, Name);
			return false;
		}
		Out = Value->AsBool();
		return true;
	}

	/** A required integer (a JSON number with no fraction). */
	bool RequireInteger(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, const FString& Where,
		int32& Out, FString& OutWhyNot)
	{
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(Name);
		double Number = 0.0;
		if (!Value.IsValid() || Value->Type != EJson::Number || !Value->TryGetNumber(Number)
			|| Number != FMath::TruncToDouble(Number) || FMath::Abs(Number) > 1.0e9)
		{
			OutWhyNot = FString::Printf(TEXT("%s%s must be an integer"), *Where, Name);
			return false;
		}
		Out = static_cast<int32>(Number);
		return true;
	}

	const TArray<TSharedPtr<FJsonValue>>* RequireArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name,
		const FString& Where, FString& OutWhyNot)
	{
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(Name);
		if (!Value.IsValid() || Value->Type != EJson::Array)
		{
			OutWhyNot = FString::Printf(TEXT("%s%s must be an array"), *Where, Name);
			return nullptr;
		}
		return &Value->AsArray();
	}

	/** An optional non-negative integer; absent or null leaves Out at 0. */
	bool OptionalBytes(const TSharedPtr<FJsonObject>& Object, const FString& Name, const FString& Where, int64& Out, FString& OutWhyNot)
	{
		Out = 0;
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(Name);
		if (!Value.IsValid() || Value->Type == EJson::Null)
		{
			return true;
		}
		double Number = 0.0;
		if (Value->Type != EJson::Number || !Value->TryGetNumber(Number) || Number < 0.0 || Number != FMath::TruncToDouble(Number) || Number > 1.0e15)
		{
			OutWhyNot = FString::Printf(TEXT("%s%s must be a byte count"), *Where, *Name);
			return false;
		}
		Out = static_cast<int64>(Number);
		return true;
	}

	/**
	 * One cooked asset reference, "<Name>": {url, cookedHash, cookedBytes} or the flat
	 * "<Name>Url" / "<Name>CookedHash" / "<Name>CookedBytes". A URL without a usable blake3 hash is
	 * named in OutUnverified (the cache never fetches it); only a wrongly typed field fails.
	 */
	bool ReadCookedAssetRef(const TSharedPtr<FJsonObject>& Object, const FString& Name, const FString& Where,
		FMantleCosmeticAssetRef& Out, TArray<FString>& OutUnverified, FString& OutWhyNot)
	{
		Out = FMantleCosmeticAssetRef();
		FString HashText;
		const TSharedPtr<FJsonValue> Nested = Object->TryGetField(Name);
		if (Nested.IsValid() && Nested->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Ref = Nested->AsObject();
			const FString NestedWhere = Where + Name + TEXT(".");
			if (!OptionalString(Ref, TEXT("url"), NestedWhere, Out.Url, OutWhyNot)
				|| !OptionalString(Ref, TEXT("cookedHash"), NestedWhere, HashText, OutWhyNot)
				|| !OptionalBytes(Ref, TEXT("cookedBytes"), NestedWhere, Out.Bytes, OutWhyNot))
			{
				return false;
			}
		}
		else if (Nested.IsValid() && Nested->Type != EJson::Null)
		{
			OutWhyNot = FString::Printf(TEXT("%s%s must be an object or null"), *Where, *Name);
			return false;
		}
		else
		{
			if (!OptionalString(Object, *(Name + TEXT("Url")), Where, Out.Url, OutWhyNot)
				|| !OptionalString(Object, *(Name + TEXT("CookedHash")), Where, HashText, OutWhyNot)
				|| !OptionalBytes(Object, Name + TEXT("CookedBytes"), Where, Out.Bytes, OutWhyNot))
			{
				return false;
			}
		}
		if (Out.Url.IsEmpty())
		{
			// No asset (null until it exists and is approved): nothing to verify either.
			Out = FMantleCosmeticAssetRef();
			return true;
		}
		const FString Label = Where + Name;
		if (HashText.IsEmpty())
		{
			OutUnverified.Add(Label);
			return true;
		}
		FString HashError;
		if (!FMantleContentHash::Parse(HashText, Out.Hash, &HashError))
		{
			Out.Hash = FMantleContentHash();
			OutUnverified.Add(FString::Printf(TEXT("%s (%s)"), *Label, *HashError));
		}
		return true;
	}

	/**
	 * A hair_cards section's "hairColor": [r, g, b], linear, each clamped to [0, 1] (the server refuses values outside).
	 * Absent or null leaves Out unset; anything but three finite numbers fails.
	 */
	bool ReadHairColor(const TSharedPtr<FJsonObject>& Object, const FString& Where, TOptional<FLinearColor>& Out, FString& OutWhyNot)
	{
		Out.Reset();
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(TEXT("hairColor"));
		if (!Value.IsValid() || Value->Type == EJson::Null)
		{
			return true;
		}
		if (Value->Type != EJson::Array || Value->AsArray().Num() != 3)
		{
			OutWhyNot = FString::Printf(TEXT("%shairColor must be an array of three numbers"), *Where);
			return false;
		}
		float Channels[3] = { 0.0f, 0.0f, 0.0f };
		for (int32 Channel = 0; Channel < 3; ++Channel)
		{
			const TSharedPtr<FJsonValue>& Component = Value->AsArray()[Channel];
			double Number = 0.0;
			if (!Component.IsValid() || Component->Type != EJson::Number || !Component->TryGetNumber(Number) || !FMath::IsFinite(Number))
			{
				OutWhyNot = FString::Printf(TEXT("%shairColor must be an array of three numbers"), *Where);
				return false;
			}
			Channels[Channel] = static_cast<float>(FMath::Clamp(Number, 0.0, 1.0));
		}
		Out = FLinearColor(Channels[0], Channels[1], Channels[2], 1.0f);
		return true;
	}

	bool ReadCookedMaterials(const TSharedPtr<FJsonObject>& Object, const TCHAR* ListName, const FString& Where,
		TArray<FMantleCosmeticMaterial>& Out, TArray<FString>& OutUnverified, FString& OutWhyNot)
	{
		Out.Reset();
		const TSharedPtr<FJsonValue> List = Object->TryGetField(ListName);
		if (!List.IsValid() || List->Type == EJson::Null)
		{
			return true;
		}
		if (List->Type != EJson::Array)
		{
			OutWhyNot = FString::Printf(TEXT("%s%s must be an array"), *Where, ListName);
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>& Entries = List->AsArray();
		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			const FString EntryWhere = FString::Printf(TEXT("%s%s[%d]."), *Where, ListName, Index);
			if (!Entries[Index].IsValid() || Entries[Index]->Type != EJson::Object)
			{
				OutWhyNot = FString::Printf(TEXT("%s%s[%d] must be an object"), *Where, ListName, Index);
				return false;
			}
			const TSharedPtr<FJsonObject> Entry = Entries[Index]->AsObject();
			FMantleCosmeticMaterial& Material = Out.AddDefaulted_GetRef();
			if (Entry->HasField(TEXT("clientMaterial")))
			{
				if (!RequireString(Entry, TEXT("clientMaterial"), EntryWhere, Material.ClientMaterial, OutWhyNot)
					|| !RequireString(Entry, TEXT("role"), EntryWhere, Material.Role, OutWhyNot)
					|| Entry->Values.Num() != 2 || !MantleCosmetic::IsClientMaterialBinding(Material.ClientMaterial, Material.Role,
						FCString::Strcmp(ListName, TEXT("faceMaterials")) == 0))
				{ OutWhyNot = EntryWhere + TEXT("clientMaterial requires a fixed part/role binding and forbids uploaded appearance fields"); return false; }
				continue;
			}
			if (!OptionalString(Entry, TEXT("role"), EntryWhere, Material.Role, OutWhyNot)
				|| !ReadCookedAssetRef(Entry, TEXT("albedo"), EntryWhere, Material.Albedo, OutUnverified, OutWhyNot)
				|| !ReadCookedAssetRef(Entry, TEXT("normal"), EntryWhere, Material.Normal, OutUnverified, OutWhyNot)
				|| !ReadCookedAssetRef(Entry, TEXT("orm"), EntryWhere, Material.ORM, OutUnverified, OutWhyNot)
				|| !ReadHairColor(Entry, EntryWhere, Material.HairColor, OutWhyNot)
				|| !OptionalString(Entry, TEXT("nativeHairProfile"), EntryWhere, Material.NativeHairProfile, OutWhyNot))
			{
				return false;
			}
			if (!Material.NativeHairProfile.IsEmpty() && (!Material.IsHairCards()
				|| MantleCosmetic::NativeHairProfileFromString(Material.NativeHairProfile) == MantleCosmetic::ENativeHairProfile::Unsupported))
			{
				OutWhyNot = EntryWhere + TEXT("nativeHairProfile must name a supported native cards profile on hair_cards only");
				return false;
			}
			const auto Profile = MantleCosmetic::NativeHairProfileFromString(Material.NativeHairProfile);
            const TSharedPtr<FJsonValue> HairValue = Entry->TryGetField(TEXT("nativeHair"));
            const bool bTypedProfile = MantleCosmetic::RequiresNativeHairSettings(Profile);
            if (bTypedProfile)
            {
                if (!Material.IsHairCards() || FCString::Strcmp(ListName, TEXT("materials")) != 0 || !Material.HairColor.IsSet()
                    || !Material.Albedo.IsLoadable() || !HairValue.IsValid() || HairValue->Type != EJson::Object)
                { OutWhyNot = EntryWhere + TEXT("nativeHair is required on a v2 native hair_cards material"); return false; }
                const TSharedPtr<FJsonObject> Hair = HairValue->AsObject();
                FMantleCosmeticNativeHair NativeHair;
                double HairVersion = 0, Melanin = 0, Redness = 0;
                const TArray<TSharedPtr<FJsonValue>>* Dye = nullptr;
                if (Hair->Values.Num() != 5 || !Hair->TryGetNumberField(TEXT("version"), HairVersion) || HairVersion != 1
                    || !Hair->TryGetNumberField(TEXT("melanin"), Melanin) || !FMath::IsFinite(Melanin) || Melanin < 0 || Melanin > 1
                    || !Hair->TryGetNumberField(TEXT("redness"), Redness) || !FMath::IsFinite(Redness) || Redness < 0 || Redness > 1
                    || !Hair->TryGetArrayField(TEXT("dyeColor"), Dye) || Dye->Num() != 3 || !Hair->HasField(TEXT("tangent")))
                { OutWhyNot = EntryWhere + TEXT("nativeHair requires version 1, bounded melanin/redness, dyeColor RGB and tangent"); return false; }
                float DyeChannels[3];
                for (int32 Channel = 0; Channel < 3; ++Channel)
                {
                    double Component = 0;
                    if (!(*Dye)[Channel].IsValid() || (*Dye)[Channel]->Type != EJson::Number
                        || !(*Dye)[Channel]->TryGetNumber(Component) || !FMath::IsFinite(Component) || Component < 0 || Component > 1)
                    { OutWhyNot = EntryWhere + TEXT("nativeHair.dyeColor must contain three finite [0,1] numbers"); return false; }
                    DyeChannels[Channel] = static_cast<float>(Component);
                }
                NativeHair.Settings.Melanin = static_cast<float>(Melanin); NativeHair.Settings.Redness = static_cast<float>(Redness);
                NativeHair.Settings.DyeColor = FLinearColor(DyeChannels[0], DyeChannels[1], DyeChannels[2], 1.f);
                if (!ReadCookedAssetRef(Hair, TEXT("tangent"), EntryWhere + TEXT("nativeHair."), NativeHair.Tangent, OutUnverified, OutWhyNot)
                    || !NativeHair.Tangent.IsLoadable())
                { OutWhyNot = EntryWhere + TEXT("nativeHair.tangent requires an approved verified cooked texture"); return false; }
                Material.NativeHair = MoveTemp(NativeHair);
            }
            else if (HairValue.IsValid())
            { OutWhyNot = EntryWhere + TEXT("nativeHair is forbidden outside v2 native profiles"); return false; }
			if (Material.Role.IsEmpty())
			{
				Material.Role = TEXT("other");
			}
			const TSharedPtr<FJsonValue> Native = Entry->TryGetField(TEXT("nativeEye"));
			if (Native.IsValid() && Native->Type != EJson::Null)
			{
				if (Material.Role != TEXT("eyes") || FCString::Strcmp(ListName, TEXT("faceMaterials")) != 0 || Native->Type != EJson::Object)
				{ OutWhyNot = EntryWhere + TEXT("nativeEye requires an avatar face eyes section"); return false; }
				FMantleCosmeticNativeEye Eye;
				const TSharedPtr<FJsonObject> EyeObject = Native->AsObject();
				const TSharedPtr<FJsonObject>* Textures = nullptr;
				if (!FMantleNativeEye::ReadSettings(EyeObject, Eye.Settings, OutWhyNot)
					|| !EyeObject->TryGetObjectField(TEXT("textures"), Textures) || (*Textures)->Values.Num() != FMantleNativeEye::TextureParameters().Num())
				{ OutWhyNot = EntryWhere + TEXT("nativeEye requires all fixed verified textures and settings"); return false; }
				for (const TCHAR* Parameter : FMantleNativeEye::TextureParameters())
				{
					if (!(*Textures)->HasField(Parameter)) { OutWhyNot = EntryWhere + TEXT("native eye texture parameter missing"); return false; }
					FMantleCosmeticAssetRef& Ref = Eye.Textures.Add(FName(Parameter));
					if (!ReadCookedAssetRef(*Textures, Parameter, EntryWhere + TEXT("nativeEye.textures."), Ref, OutUnverified, OutWhyNot)) return false;
				}
				Material.NativeEye = MoveTemp(Eye);
			}
			const TSharedPtr<FJsonValue> Colors = Entry->TryGetField(TEXT("colorCustomization"));
			if (Colors.IsValid() && Colors->Type != EJson::Null)
			{
				if (Colors->Type != EJson::Object || Material.Role != TEXT("other") || FCString::Strcmp(ListName, TEXT("materials")) != 0 || Material.HairColor.IsSet() || Material.NativeEye.IsSet())
				{ OutWhyNot = EntryWhere + TEXT("colorCustomization requires an ordinary clothing material"); return false; }
				const TSharedPtr<FJsonObject> Declaration = Colors->AsObject();
				double Version = 0; FString Mode;
				const TArray<TSharedPtr<FJsonValue>>* Regions = nullptr;
				if (Declaration->Values.Num() != 4 || !Declaration->TryGetNumberField(TEXT("version"), Version) || Version != 1 ||
					!Declaration->TryGetStringField(TEXT("mode"), Mode) || Mode != MantleClothingColor::Mode ||
					!ReadCookedAssetRef(Declaration, TEXT("mask"), EntryWhere, Material.ColorMask, OutUnverified, OutWhyNot) || !Material.ColorMask.IsLoadable() ||
					!Declaration->TryGetArrayField(TEXT("regions"), Regions) || Regions->Num() < 1 || Regions->Num() > MantleClothingColor::MaxRegionsPerSection)
				{ OutWhyNot = EntryWhere + TEXT("colorCustomization needs the fixed mode, verified mask and one to three regions"); return false; }
				for (const TSharedPtr<FJsonValue>& RegionValue : *Regions)
				{
					if (!RegionValue.IsValid() || RegionValue->Type != EJson::Object) { OutWhyNot = EntryWhere + TEXT("colour region must be an object"); return false; }
					const TSharedPtr<FJsonObject> RegionObject = RegionValue->AsObject();
					FMantleClothingColorRegion Region; FString Channel, RGB; double Luminance = 0;
					if (RegionObject->Values.Num() != 5 || !RegionObject->TryGetStringField(TEXT("id"), Region.Id) || !RegionObject->TryGetStringField(TEXT("label"), Region.Label) ||
						!RegionObject->TryGetStringField(TEXT("channel"), Channel) || !RegionObject->TryGetStringField(TEXT("defaultColor"), RGB) ||
						!RegionObject->TryGetNumberField(TEXT("referenceLuminance"), Luminance)) { OutWhyNot = EntryWhere + TEXT("colour region fields are incomplete"); return false; }
					Region.Channel = Channel == TEXT("r") ? 0 : Channel == TEXT("g") ? 1 : Channel == TEXT("b") ? 2 : 255;
					bool bRGB = RGB.Len() == 7 && RGB[0] == TEXT('#');
					for (int32 Digit = 1; bRGB && Digit < RGB.Len(); ++Digit) bRGB = (RGB[Digit] >= TEXT('0') && RGB[Digit] <= TEXT('9')) || (RGB[Digit] >= TEXT('a') && RGB[Digit] <= TEXT('f'));
					if (!bRGB || !FMath::IsFinite(Luminance) || Luminance < MantleClothingColor::MinReferenceLuminance || Luminance > 1) { OutWhyNot = EntryWhere + TEXT("colour region needs opaque RGB and nonzero reference luminance"); return false; }
					Region.DefaultColor = FColor::FromHex(RGB); Region.DefaultColor.A = 255; Region.ReferenceLuminance = static_cast<float>(Luminance);
					Material.ColorRegions.Add(MoveTemp(Region));
				}
				if (!MantleClothingColor::ValidateRegions(Material.ColorRegions, OutWhyNot)) return false;
			}
		}
		return true;
	}

	/** An optional [x, y, z] of finite numbers; absent or null leaves Out unset. */
	bool ReadTransformPart(const TSharedPtr<FJsonObject>& Object, const TCHAR* Name, const FString& Where, TOptional<FVector>& Out, FString& OutWhyNot)
	{
		Out.Reset();
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(Name);
		if (!Value.IsValid() || Value->Type == EJson::Null)
		{
			return true;
		}
		if (Value->Type != EJson::Array || Value->AsArray().Num() != 3)
		{
			OutWhyNot = FString::Printf(TEXT("%s%s must be an array of three numbers"), *Where, Name);
			return false;
		}
		FVector Parsed = FVector::ZeroVector;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const TSharedPtr<FJsonValue>& Component = Value->AsArray()[Axis];
			double Number = 0.0;
			if (!Component.IsValid() || Component->Type != EJson::Number || !Component->TryGetNumber(Number) || !FMath::IsFinite(Number))
			{
				OutWhyNot = FString::Printf(TEXT("%s%s must be an array of three numbers"), *Where, Name);
				return false;
			}
			Parsed[Axis] = Number;
		}
		Out = Parsed;
		return true;
	}

	/**
	 * A backend avatar.Transform, {location, rotation, scale}, each part optional: location in cm, rotation in
	 * degrees as (pitch, yaw, roll), scale per axis. Absent, null or {} is an empty transform. Bounds are not
	 * checked here (the server refuses them; the assembler clamps what it shows).
	 */
	bool ReadCosmeticTransform(const TSharedPtr<FJsonValue>& Value, const FString& Where, FMantleCosmeticTransform& Out, FString& OutWhyNot)
	{
		Out = FMantleCosmeticTransform();
		if (!Value.IsValid() || Value->Type == EJson::Null)
		{
			return true;
		}
		if (Value->Type != EJson::Object)
		{
			OutWhyNot = FString::Printf(TEXT("%s must be an object or null"), *Where.LeftChop(1));
			return false;
		}
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		TOptional<FVector> Rotation;
		if (!ReadTransformPart(Object, TEXT("location"), Where, Out.Location, OutWhyNot)
			|| !ReadTransformPart(Object, TEXT("rotation"), Where, Rotation, OutWhyNot)
			|| !ReadTransformPart(Object, TEXT("scale"), Where, Out.Scale, OutWhyNot))
		{
			return false;
		}
		if (Rotation.IsSet())
		{
			Out.Rotation = FRotator(Rotation->X, Rotation->Y, Rotation->Z);
		}
		return true;
	}

	/** An avatar's "sockets": {socket name: transform}; absent or null is none. */
	bool ReadSocketOverrides(const TSharedPtr<FJsonObject>& Object, const FString& Where, TMap<FString, FMantleCosmeticTransform>& Out,
		FString& OutWhyNot)
	{
		Out.Reset();
		const TSharedPtr<FJsonValue> Value = Object->TryGetField(TEXT("sockets"));
		if (!Value.IsValid() || Value->Type == EJson::Null)
		{
			return true;
		}
		if (Value->Type != EJson::Object)
		{
			OutWhyNot = FString::Printf(TEXT("%ssockets must be an object or null"), *Where);
			return false;
		}
		const TSharedPtr<FJsonObject> Sockets = Value->AsObject();
		for (const auto& Field : Sockets->Values)
		{
			const FString Socket(Field.Key.ToView());
			FMantleCosmeticTransform Transform;
			if (!ReadCosmeticTransform(Field.Value, FString::Printf(TEXT("%ssockets.%s."), *Where, *Socket), Transform, OutWhyNot))
			{
				return false;
			}
			if (!Transform.IsEmpty())
			{
				Out.Add(Socket, MoveTemp(Transform));
			}
		}
		return true;
	}

	/** POST /avatar/items/batch through FMantleApiClient. */
	class FApiCosmeticResolver final : public IMantleCosmeticResolver
	{
	public:
		virtual void Resolve(const TArray<FString>& ItemIds, TFunction<void(const FMantleCosmeticResolveResult&)> OnDone) override
		{
			FMantleApiClient::SendAsync(TEXT("POST"), TEXT("/avatar/items/batch"), MantleCosmetic::BuildItemBatchBody(ItemIds),
				FMantleApiCallback::CreateLambda([OnDone = MoveTemp(OnDone)](const FMantleApiResult& Response)
				{
					FMantleCosmeticResolveResult Result;
					Result.StatusCode = Response.StatusCode;
					if (!Response.bSuccess)
					{
						Result.Error = Response.Message.IsEmpty() ? FString::Printf(TEXT("HTTP %d"), Response.StatusCode) : Response.Message;
					}
					else
					{
						FString WhyNot;
						Result.bSuccess = MantleCosmetic::ParseItemBatch(Response.Body, Result.Items, Result.Missing, WhyNot);
						if (!Result.bSuccess)
						{
							Result.Error = FString::Printf(TEXT("the item batch could not be read: %s"), *WhyNot);
						}
					}
					OnDone(Result);
				}));
		}
	};

	/** The optional "missing" array of a batch response: strings only. */
	bool ReadMissingList(const TSharedPtr<FJsonObject>& Root, TArray<FString>& OutMissing, FString& OutWhyNot)
	{
		const TSharedPtr<FJsonValue> MissingValue = Root->TryGetField(TEXT("missing"));
		if (MissingValue.IsValid() && MissingValue->Type != EJson::Null)
		{
			if (MissingValue->Type != EJson::Array)
			{
				OutWhyNot = TEXT("missing must be an array");
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Entry : MissingValue->AsArray())
			{
				if (!Entry.IsValid() || Entry->Type != EJson::String)
				{
					OutWhyNot = TEXT("missing entries must be strings");
					return false;
				}
				OutMissing.Add(Entry->AsString());
			}
		}
		return true;
	}
}

// ---- Small types -----------------------------------------------------------------------------------

FMantleStoredAssetRef FMantleCosmeticAssetRef::ToStoreRef() const
{
	FMantleStoredAssetRef Ref;
	Ref.Hash = Hash;
	Ref.Url = Url;
	Ref.Bytes = Bytes;
	return Ref;
}

FString FMantleCosmeticItem::VersionKey() const
{
	return FString::Printf(TEXT("%s@%d"), *Id, Version);
}

bool FMantleCosmeticMaterial::IsHairCards() const
{
	return Role.Equals(MantleCosmetic::HairCardsRole, ESearchCase::CaseSensitive);
}

EMantleCosmeticKind MantleCosmetic::KindFromApi(const FString& Value)
{
	if (Value == TEXT("avatar"))
	{
		return EMantleCosmeticKind::Avatar;
	}
	if (Value == TEXT("skinned"))
	{
		return EMantleCosmeticKind::Skinned;
	}
	if (Value == TEXT("rigid"))
	{
		return EMantleCosmeticKind::Rigid;
	}
	if (Value == TEXT("emote"))
	{
		return EMantleCosmeticKind::Emote;
	}
	if (Value == TEXT("overlay"))
	{
		return EMantleCosmeticKind::Overlay;
	}
	return EMantleCosmeticKind::Unknown;
}

const TCHAR* MantleCosmetic::KindName(const EMantleCosmeticKind Kind)
{
	switch (Kind)
	{
	case EMantleCosmeticKind::Avatar: return TEXT("avatar");
	case EMantleCosmeticKind::Skinned: return TEXT("skinned");
	case EMantleCosmeticKind::Rigid: return TEXT("rigid");
	case EMantleCosmeticKind::Emote: return TEXT("emote");
	case EMantleCosmeticKind::Overlay: return TEXT("overlay");
	default: return TEXT("unknown");
	}
}

bool MantleCosmetic::IsItemId(const FString& Value)
{
	if (Value.Len() != 5 + 32 || !Value.StartsWith(TEXT("itm_u"), ESearchCase::CaseSensitive))
	{
		return false;
	}
	for (int32 Index = 5; Index < Value.Len(); ++Index)
	{
		const TCHAR Char = Value[Index];
		if (!((Char >= TEXT('0') && Char <= TEXT('9')) || (Char >= TEXT('a') && Char <= TEXT('f'))))
		{
			return false;
		}
	}
	return true;
}

namespace MantleCosmeticLocalDraft
{
	FCriticalSection Lock;
	TSet<FString> Ids;
}

void MantleCosmetic::RegisterLocalDraftId(const FString& ItemId)
{
	FScopeLock Scope(&MantleCosmeticLocalDraft::Lock);
	MantleCosmeticLocalDraft::Ids.Add(ItemId);
}

bool MantleCosmetic::IsLocalDraftId(const FString& ItemId)
{
	FScopeLock Scope(&MantleCosmeticLocalDraft::Lock);
	return MantleCosmeticLocalDraft::Ids.Contains(ItemId);
}

bool MantleCosmetic::ContainsLocalDraftId(const FString& Text)
{
	FScopeLock Scope(&MantleCosmeticLocalDraft::Lock);
	for (const FString& Id : MantleCosmeticLocalDraft::Ids)
	{
		if (Text.Contains(Id, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	return false;
}

void MantleCosmetic::ClearLocalDraftIdsForTesting()
{
	FScopeLock Scope(&MantleCosmeticLocalDraft::Lock);
	MantleCosmeticLocalDraft::Ids.Reset();
}

FString MantleCosmetic::BuildItemBatchBody(const TArray<FString>& ItemIds)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	Values.Reserve(ItemIds.Num());
	for (const FString& Id : ItemIds)
	{
		Values.Add(MakeShared<FJsonValueString>(Id));
	}
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetArrayField(TEXT("ids"), Values);
	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Object, Writer);
	return Json;
}

bool MantleCosmetic::ReadAvatarPublication(const TSharedPtr<FJsonObject>& Object, const FString& Where,
    bool bAvatar, const FString& Skeleton, bool bGarmentCompatible, FString& OutMode, bool& bOutEligible, FString& OutWhyNot)
{
    if (!Object.IsValid()) { OutWhyNot=Where+TEXT("publication facts require an object"); return false; }
    const auto ModeValue=Object->TryGetField(TEXT("avatarPublicationMode"));
    if (ModeValue.IsValid() && ModeValue->Type!=EJson::String)
    { OutWhyNot=Where+TEXT("avatarPublicationMode must be a string when present"); return false; }
    FString Mode; bool bEligible=false;
    using namespace MantleCosmeticItemPrivate;
    if (!OptionalString(Object,TEXT("avatarPublicationMode"),Where,Mode,OutWhyNot)
        || !OptionalBool(Object,TEXT("customizationEligible"),Where,bEligible,OutWhyNot)) return false;
    if ((!Mode.IsEmpty() && Mode!=TEXT("converted") && Mode!=TEXT("as_is"))
        || (!bAvatar && (!Mode.IsEmpty() || bEligible))
        || (bEligible && (Mode!=TEXT("converted") || Skeleton!=TEXT("metahuman") || !bGarmentCompatible)))
    { OutWhyNot=Where+TEXT("avatar publication mode/customization eligibility is inconsistent"); return false; }
    OutMode=MoveTemp(Mode); bOutEligible=bEligible; return true;
}

bool MantleCosmetic::ParseItemBatch(const FString& Json, TArray<FMantleCosmeticItem>& OutItems, TArray<FString>& OutMissing, FString& OutWhyNot)
{
	using namespace MantleCosmeticItemPrivate;
	const TSharedPtr<FJsonObject> Root = ParseRoot(Json, TEXT("the item batch"), OutWhyNot);
	if (!Root.IsValid())
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Entries = RequireArray(Root, TEXT("items"), FString(), OutWhyNot);
	if (!Entries)
	{
		return false;
	}
	TArray<FMantleCosmeticItem> Items;
	for (int32 Index = 0; Index < Entries->Num(); ++Index)
	{
		const TSharedPtr<FJsonValue>& Value = (*Entries)[Index];
		const FString Where = FString::Printf(TEXT("items[%d]."), Index);
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			OutWhyNot = FString::Printf(TEXT("items[%d] must be an object"), Index);
			return false;
		}
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		FMantleCosmeticItem Item;
		FString Kind;
		if (!RequireString(Object, TEXT("id"), Where, Item.Id, OutWhyNot)
			|| !OptionalString(Object, TEXT("slot"), Where, Item.Slot, OutWhyNot)
			|| !OptionalString(Object, TEXT("kind"), Where, Kind, OutWhyNot)
			|| !OptionalString(Object, TEXT("skeleton"), Where, Item.Skeleton, OutWhyNot)
			|| !OptionalBool(Object, TEXT("garmentCompatible"), Where, Item.bGarmentCompatible, OutWhyNot)
			|| !OptionalString(Object, TEXT("jawBone"), Where, Item.JawBone, OutWhyNot))
		{
			return false;
		}
		if (!IsItemId(Item.Id))
		{
			OutWhyNot = FString::Printf(TEXT("%sid %s is not an item id"), *Where, *Item.Id);
			return false;
		}
		if (Object->HasField(TEXT("version")) && !RequireInteger(Object, TEXT("version"), Where, Item.Version, OutWhyNot))
		{
			return false;
		}
		Item.Kind = KindFromApi(Kind);
		if (!ReadAvatarPublication(Object,Where,Item.Kind==EMantleCosmeticKind::Avatar,Item.Skeleton,Item.bGarmentCompatible,Item.AvatarPublicationMode,Item.bCustomizationEligible,OutWhyNot)) return false;
		const struct
		{
			const TCHAR* Name;
			FMantleCosmeticAssetRef FMantleCosmeticItem::* Field;
		} AssetFields[] =
		{
			{ TEXT("mesh"), &FMantleCosmeticItem::Mesh },
			{ TEXT("faceMesh"), &FMantleCosmeticItem::FaceMesh },
			{ TEXT("texture"), &FMantleCosmeticItem::Texture },
			{ TEXT("clip"), &FMantleCosmeticItem::Clip },
			{ TEXT("sourceShape"), &FMantleCosmeticItem::SourceShape },
			{ TEXT("coverage"), &FMantleCosmeticItem::Coverage },
			{ TEXT("irisMask"), &FMantleCosmeticItem::IrisMask },
			{ TEXT("dna"), &FMantleCosmeticItem::Dna },
			{ TEXT("normal"), &FMantleCosmeticItem::Normal },
		};
		for (const auto& AssetField : AssetFields)
		{
			if (!ReadCookedAssetRef(Object, AssetField.Name, Where, Item.*(AssetField.Field), Item.UnverifiedAssets, OutWhyNot))
			{
				return false;
			}
		}
		if (!ReadCookedMaterials(Object, TEXT("materials"), Where, Item.Materials, Item.UnverifiedAssets, OutWhyNot)
			|| !ReadCookedMaterials(Object, TEXT("faceMaterials"), Where, Item.FaceMaterials, Item.UnverifiedAssets, OutWhyNot)
			|| !ReadSocketOverrides(Object, Where, Item.Sockets, OutWhyNot)
			|| !ReadCosmeticTransform(Object->TryGetField(TEXT("attach")), Where + TEXT("attach."), Item.Attach, OutWhyNot))
		{
			return false;
		}
		if (!ValidateClientMaterials(Item, OutWhyNot)) { OutWhyNot = Where + OutWhyNot; return false; }
		for (const FMantleCosmeticMaterial& Material : Item.Materials)
		{
            if (Material.NativeHair.IsSet() && (Item.Kind != EMantleCosmeticKind::Skinned
                || (Item.Slot != TEXT("hair") && Item.Slot != TEXT("eyebrows") && Item.Slot != TEXT("eyelashes") && Item.Slot != TEXT("facial_hair"))))
            { OutWhyNot = Where + TEXT("nativeHair only belongs to one of the four skinned hair slots"); return false; }
			// Clothing, and (#966) a rigid item in an accessory slot (acc_hat, acc_face, ...), as the API accepts them.
			const bool bClothingColors = Item.Kind == EMantleCosmeticKind::Skinned && (Item.Slot == TEXT("top") || Item.Slot == TEXT("bottom") || Item.Slot == TEXT("footwear"));
			const bool bAccessoryColors = Item.Kind == EMantleCosmeticKind::Rigid && Item.Slot.StartsWith(TEXT("acc_"));
			if (!Material.ColorRegions.IsEmpty() && !bClothingColors && !bAccessoryColors)
			{ OutWhyNot = Where + TEXT("only clothing and accessory items declare colour regions"); return false; }
		}
		if (const TSharedPtr<FJsonValue> ManifestValue = Object->TryGetField(TEXT("garmentVariants")); ManifestValue.IsValid() && ManifestValue->Type != EJson::Null)
		{
			if (Item.Kind != EMantleCosmeticKind::Skinned || (Item.Slot == TEXT("hair") || Item.Slot == TEXT("eyebrows") || Item.Slot == TEXT("eyelashes") || Item.Slot == TEXT("facial_hair")) || ManifestValue->Type != EJson::Object)
			{
				OutWhyNot = Where + TEXT("garmentVariants must be a clothing source manifest"); return false;
			}
			const TSharedPtr<FJsonObject> Manifest = ManifestValue->AsObject();
			const FString ManifestWhere = Where + TEXT("garmentVariants.");
			int32 Version = 0;
			if (!RequireInteger(Manifest, TEXT("version"), ManifestWhere, Version, OutWhyNot) || Version != 1
				|| !RequireString(Manifest, TEXT("policy"), ManifestWhere, Item.GarmentVariantPolicy, OutWhyNot)
				|| Item.GarmentVariantPolicy != TEXT("body-rms-v1")
				|| !RequireString(Manifest, TEXT("defaultVariant"), ManifestWhere, Item.DefaultGarmentVariant, OutWhyNot))
			{
				OutWhyNot = ManifestWhere + TEXT("unsupported version or source selection policy"); return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Variants = RequireArray(Manifest, TEXT("variants"), ManifestWhere, OutWhyNot);
			if (!Variants || Variants->Num() < 1 || Variants->Num() > 8)
			{
				OutWhyNot = ManifestWhere + TEXT("variants requires one to eight sources"); return false;
			}
			TSet<FString> Ids;
			for (int32 VariantIndex = 0; VariantIndex < Variants->Num(); ++VariantIndex)
			{
				const TSharedPtr<FJsonValue>& Entry = (*Variants)[VariantIndex];
				if (!Entry || Entry->Type != EJson::Object) { OutWhyNot = ManifestWhere + TEXT("variant must be an object"); return false; }
				const TSharedPtr<FJsonObject> Row = Entry->AsObject();
				const FString RowWhere = FString::Printf(TEXT("%svariants[%d]."), *ManifestWhere, VariantIndex);
				FMantleCosmeticGarmentVariant& Variant = Item.GarmentVariants.AddDefaulted_GetRef();
				if (!RequireString(Row, TEXT("id"), RowWhere, Variant.Id, OutWhyNot)
					|| !OptionalString(Row, TEXT("nativeSourceSize"), RowWhere, Variant.NativeSourceSize, OutWhyNot)
					|| !ReadCookedAssetRef(Row, TEXT("mesh"), RowWhere, Variant.Mesh, Item.UnverifiedAssets, OutWhyNot)
					|| !ReadCookedAssetRef(Row, TEXT("sourceShape"), RowWhere, Variant.SourceShape, Item.UnverifiedAssets, OutWhyNot)
					|| !ReadCookedAssetRef(Row, TEXT("faceMesh"), RowWhere, Variant.FaceMesh, Item.UnverifiedAssets, OutWhyNot)
					|| !ReadCookedAssetRef(Row, TEXT("coverage"), RowWhere, Variant.Coverage, Item.UnverifiedAssets, OutWhyNot)
					|| !ReadCookedMaterials(Row, TEXT("materials"), RowWhere, Variant.Materials, Item.UnverifiedAssets, OutWhyNot)) { return false; }
				for (const FMantleCosmeticMaterial& VariantMaterial : Variant.Materials)
					if (VariantMaterial.NativeHair.IsSet())
					{ OutWhyNot = RowWhere + TEXT("nativeHair is forbidden in every clothing garment variant"); return false; }
				if (!Variant.Mesh.IsLoadable() || !Variant.SourceShape.IsLoadable()) { OutWhyNot = RowWhere + TEXT("mesh and sourceShape must be approved, verifiable cooked references"); return false; }
				bool bValidId = Variant.Id.Len() >= 1 && Variant.Id.Len() <= 32 && Variant.Id[0] >= TEXT('a') && Variant.Id[0] <= TEXT('z');
				for (const TCHAR Character : Variant.Id)
				{
					bValidId &= (Character >= TEXT('a') && Character <= TEXT('z')) || (Character >= TEXT('0') && Character <= TEXT('9')) || Character == TEXT('_') || Character == TEXT('-');
				}
				if (!bValidId || Ids.Contains(Variant.Id)) { OutWhyNot = RowWhere + TEXT("id must be unique lowercase text"); return false; }
				Ids.Add(Variant.Id);
			}
			if (!Ids.Contains(Item.DefaultGarmentVariant)) { OutWhyNot = ManifestWhere + TEXT("defaultVariant names no source"); return false; }
			const FMantleCosmeticGarmentVariant* Default = Item.GarmentVariants.FindByPredicate([&Item](const FMantleCosmeticGarmentVariant& Variant) { return Variant.Id == Item.DefaultGarmentVariant; });
			auto SameAsset = [](const FMantleCosmeticAssetRef& A, const FMantleCosmeticAssetRef& B)
			{
				return A.Url == B.Url && A.Hash == B.Hash && A.Bytes == B.Bytes;
			};
			bool bSameDefault = Default && SameAsset(Item.Mesh, Default->Mesh) && SameAsset(Item.SourceShape, Default->SourceShape)
				&& SameAsset(Item.FaceMesh, Default->FaceMesh) && SameAsset(Item.Coverage, Default->Coverage)
				&& Item.Materials.Num() == Default->Materials.Num();
			for (int32 MaterialIndex = 0; bSameDefault && MaterialIndex < Item.Materials.Num(); ++MaterialIndex)
			{
				const FMantleCosmeticMaterial& A = Item.Materials[MaterialIndex];
				const FMantleCosmeticMaterial& B = Default->Materials[MaterialIndex];
				bSameDefault = A.Role == B.Role && SameAsset(A.Albedo, B.Albedo) && SameAsset(A.Normal, B.Normal) && SameAsset(A.ORM, B.ORM)
					&& SameAsset(A.ColorMask, B.ColorMask) && A.ColorRegions == B.ColorRegions
					&& A.NativeHairProfile == B.NativeHairProfile && !A.NativeHair.IsSet() && !B.NativeHair.IsSet()
					&& A.HairColor.IsSet() == B.HairColor.IsSet() && (!A.HairColor.IsSet() || A.HairColor.GetValue() == B.HairColor.GetValue());
			}
			if (!bSameDefault) { OutWhyNot = ManifestWhere + TEXT("default source must exactly mirror the item's assets and materials"); return false; }
		}
		TMap<FString, FString> LogicalColors;
		auto ReadLogicalColors = [&OutWhyNot](const TArray<FMantleCosmeticMaterial>& Materials, TMap<FString, FString>& Out)
		{
			for (const FMantleCosmeticMaterial& Material : Materials)
				for (const FMantleClothingColorRegion& Region : Material.ColorRegions)
				{
					const FString Declaration = Region.Label + TEXT("/") + Region.DefaultColor.ToHex();
					if (const FString* Previous = Out.Find(Region.Id); Previous && *Previous != Declaration)
					{ OutWhyNot = TEXT("logical colour region declarations disagree between sections"); return false; }
					Out.Add(Region.Id, Declaration);
				}
			if (Out.Num() > MantleClothingColor::MaxColorsPerItem) { OutWhyNot = TEXT("more than eight logical colour regions"); return false; }
			return true;
		};
		if (!ReadLogicalColors(Item.Materials, LogicalColors)) return false;
		for (const FMantleCosmeticGarmentVariant& Variant : Item.GarmentVariants)
		{
			TMap<FString, FString> VariantColors;
			if (!ReadLogicalColors(Variant.Materials, VariantColors) || VariantColors.Num() != LogicalColors.Num())
			{ OutWhyNot = TEXT("all garment sources must declare the same logical colour regions"); return false; }
			for (const auto& Color : LogicalColors)
				if (!VariantColors.Contains(Color.Key) || VariantColors[Color.Key] != Color.Value)
				{ OutWhyNot = TEXT("garment source colour label/default disagrees"); return false; }
		}
		// Measurements load nothing, so unreadable ones leave the item without them rather than failing the answer.
		FString MetricsWhyNot;
		if (!ReadBodyMetrics(Object->TryGetField(TEXT("bodyMetrics")), Where, Item.BodyMetrics, MetricsWhyNot))
		{
			Item.BodyMetrics.Reset();
		}
		// Presets only tune how bones swing, so unreadable ones leave the item on its defaults.
		FString SecondaryWhyNot;
		if (!ReadSecondaryMotion(Object->TryGetField(TEXT("secondaryMotion")), Where, Item.SecondaryMotion, SecondaryWhyNot))
		{
			Item.SecondaryMotion.Reset();
		}
		Items.Add(MoveTemp(Item));
	}
	TArray<FString> Missing;
	if (!ReadMissingList(Root, Missing, OutWhyNot))
	{
		return false;
	}
	OutItems = MoveTemp(Items);
	OutMissing = MoveTemp(Missing);
	return true;
}

bool MantleCosmetic::ReadSecondaryMotion(const TSharedPtr<FJsonValue>& Value, const FString& Where, TMap<FString, FString>& Out,
	FString& OutWhyNot)
{
	Out.Reset();
	if (!Value.IsValid() || Value->Type == EJson::Null)
	{
		return true;
	}
	if (Value->Type != EJson::Object)
	{
		OutWhyNot = FString::Printf(TEXT("%ssecondaryMotion must be an object or null"), *Where);
		return false;
	}
	TMap<FString, FString> Read;
	for (const auto& Field : Value->AsObject()->Values)
	{
		const FString Chain(Field.Key.ToView());
		if (!Field.Value.IsValid() || Field.Value->Type != EJson::String)
		{
			OutWhyNot = FString::Printf(TEXT("%ssecondaryMotion.%s must be a preset id"), *Where, *Chain);
			return false;
		}
		Read.Add(Chain, Field.Value->AsString());
	}
	Out = MoveTemp(Read);
	return true;
}

bool MantleCosmetic::ReadClothingColorRegions(const TSharedPtr<FJsonObject>& Object,
	TArray<FMantleClothingColorRegion>& Out, FString& OutWhyNot)
{
	TArray<FMantleCosmeticMaterial> Materials; TArray<FString> Unverified;
	if (!Object.IsValid() || !MantleCosmeticItemPrivate::ReadCookedMaterials(Object, TEXT("materials"), FString(), Materials, Unverified, OutWhyNot)) return false;
	Out.Reset();
	for (const FMantleCosmeticMaterial& Material : Materials)
		for (const FMantleClothingColorRegion& Region : Material.ColorRegions)
		{
			const FMantleClothingColorRegion* Found = Out.FindByPredicate([&Region](const FMantleClothingColorRegion& Existing) { return Existing.Id == Region.Id; });
			if (Found && (Found->Label != Region.Label || Found->DefaultColor != Region.DefaultColor)) { OutWhyNot = TEXT("logical colour label/default disagree"); return false; }
			if (!Found) Out.Add(Region);
		}
	if (Out.Num() > MantleClothingColor::MaxColorsPerItem) { OutWhyNot = TEXT("more than eight clothing colour controls"); return false; }
	return true;
}

bool MantleCosmetic::ReadBodyMetrics(const TSharedPtr<FJsonValue>& Value, const FString& Where,
	TOptional<FMantleAvatarBodyMetrics>& Out, FString& OutWhyNot)
{
	Out.Reset();
	if (!Value.IsValid() || Value->Type == EJson::Null)
	{
		return true;
	}
	if (Value->Type != EJson::Object || !Value->AsObject().IsValid())
	{
		OutWhyNot = FString::Printf(TEXT("%sbodyMetrics must be an object or null"), *Where);
		return false;
	}
	const TSharedPtr<FJsonObject> Object = Value->AsObject();
	// A length in cm: a finite number, never negative.
	auto ReadLength = [&Object, &Where, &OutWhyNot](const TCHAR* Name, double& OutLength) -> bool
	{
		const TSharedPtr<FJsonValue> Field = Object->TryGetField(Name);
		double Number = 0.0;
		if (!Field.IsValid() || Field->Type != EJson::Number || !Field->TryGetNumber(Number) || !FMath::IsFinite(Number) || Number < 0.0)
		{
			OutWhyNot = FString::Printf(TEXT("%sbodyMetrics.%s must be a length in cm"), *Where, Name);
			return false;
		}
		OutLength = Number;
		return true;
	};
	FMantleAvatarBodyMetrics Metrics;
	if (!ReadLength(TEXT("standingHeightCm"), Metrics.StandingHeightCm)
		|| !ReadLength(TEXT("pelvisHeightCm"), Metrics.PelvisHeightCm)
		|| !ReadLength(TEXT("armLengthCm"), Metrics.ArmLengthCm)
		|| !ReadLength(TEXT("legLengthCm"), Metrics.LegLengthCm)
		|| !ReadLength(TEXT("shoulderWidthCm"), Metrics.ShoulderWidthCm)
		|| !ReadLength(TEXT("hipWidthCm"), Metrics.HipWidthCm))
	{
		return false;
	}
	if (Metrics.StandingHeightCm <= 0.0)
	{
		OutWhyNot = FString::Printf(TEXT("%sbodyMetrics.standingHeightCm must be above 0"), *Where);
		return false;
	}
	const TSharedPtr<FJsonValue> Eyes = Object->TryGetField(TEXT("eyeHeightCm"));
	if (Eyes.IsValid() && Eyes->Type != EJson::Null)
	{
		double EyeHeight = 0.0;
		if (!ReadLength(TEXT("eyeHeightCm"), EyeHeight))
		{
			return false;
		}
		Metrics.EyeHeightCm = EyeHeight;
	}
	Out = Metrics;
	return true;
}

bool MantleCosmetic::ParseServerBodyMetrics(const FString& Json, TMap<FString, TOptional<FMantleAvatarBodyMetrics>>& OutMetrics,
	TArray<FString>& OutMissing, FString& OutWhyNot)
{
	using namespace MantleCosmeticItemPrivate;
	const TSharedPtr<FJsonObject> Root = ParseRoot(Json, TEXT("the avatar metrics answer"), OutWhyNot);
	if (!Root.IsValid())
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Entries = RequireArray(Root, TEXT("items"), FString(), OutWhyNot);
	if (!Entries)
	{
		return false;
	}
	TMap<FString, TOptional<FMantleAvatarBodyMetrics>> Metrics;
	for (int32 Index = 0; Index < Entries->Num(); ++Index)
	{
		const TSharedPtr<FJsonValue>& Value = (*Entries)[Index];
		const FString Where = FString::Printf(TEXT("items[%d]."), Index);
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			OutWhyNot = FString::Printf(TEXT("items[%d] must be an object"), Index);
			return false;
		}
		const TSharedPtr<FJsonObject> Object = Value->AsObject();
		FString Id;
		if (!RequireString(Object, TEXT("id"), Where, Id, OutWhyNot))
		{
			return false;
		}
		if (!IsItemId(Id))
		{
			OutWhyNot = FString::Printf(TEXT("%sid %s is not an item id"), *Where, *Id);
			return false;
		}
		// Unreadable metrics are none: the character plays at its height setting alone.
		TOptional<FMantleAvatarBodyMetrics> ItemMetrics;
		FString MetricsWhyNot;
		if (!ReadBodyMetrics(Object->TryGetField(TEXT("bodyMetrics")), Where, ItemMetrics, MetricsWhyNot))
		{
			ItemMetrics.Reset();
		}
		Metrics.Add(Id, ItemMetrics);
	}
	TArray<FString> Missing;
	if (!ReadMissingList(Root, Missing, OutWhyNot))
	{
		return false;
	}
	OutMetrics = MoveTemp(Metrics);
	OutMissing = MoveTemp(Missing);
	return true;
}

TSharedRef<IMantleCosmeticResolver> MantleCosmetic::CreateApiResolver()
{
	return MakeShared<MantleCosmeticItemPrivate::FApiCosmeticResolver>();
}
