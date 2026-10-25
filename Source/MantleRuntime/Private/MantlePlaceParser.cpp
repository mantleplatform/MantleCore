// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceParser.h"
#include "MantleEnvironmentOverride.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceStorage.h"
#include "MantleRuntime.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Containers/StringConv.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	/** Accumulates errors with a JSON-pointer-style path, matching the Python validator's output. */
	class FParseContext
	{
	public:
		TArray<FMantleParseError> Errors;

		/** An offline preview's bundle: a VehicleSpawn may carry the draft form, a raw uploaded vehicle id the bundle holds. */
		bool bAcceptDraftVehicleIds = false;

		void Fail(const FString& Path, const FString& Message)
		{
			Errors.Add(FMantleParseError{ Path, Message });
		}
	};

	FString Join(const FString& Path, const FString& Key)
	{
		return Path + TEXT("/") + Key;
	}

	FString Index(const FString& Path, int32 Idx)
	{
		return Path + TEXT("/") + FString::FromInt(Idx);
	}

	/** Enforces additionalProperties:false — an unknown property is as fatal as an unknown type. */
	void RequireNoUnknownKeys(
		FParseContext& Ctx,
		const TSharedPtr<FJsonObject>& Obj,
		std::initializer_list<const TCHAR*> Known,
		const FString& Path)
	{
		for (const auto& Pair : Obj->Values)
		{
			// UE 5.8 keys FJsonObject::Values by TSharedString, not FString.
			const FString Key(Pair.Key.ToView());

			bool bFound = false;
			for (const TCHAR* K : Known)
			{
				if (Key.Equals(K, ESearchCase::CaseSensitive))
				{
					bFound = true;
					break;
				}
			}
			if (!bFound)
			{
				Ctx.Fail(Join(Path, Key), TEXT("unknown property"));
			}
		}
	}

	bool RequireString(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FString& Path, FString& Out)
	{
		// TryGetStringField also turns a number into its text, so the type is checked first: a weightmap2
		// of 7 is not a string, and neither is any other id the schema asks for as one.
		const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Key);
		if (!Value.IsValid() || Value->Type != EJson::String || !Obj->TryGetStringField(Key, Out))
		{
			Ctx.Fail(Join(Path, Key), TEXT("required string is missing or not a string"));
			return false;
		}
		return true;
	}

	void OptionalString(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, FString& Out)
	{
		Obj->TryGetStringField(Key, Out);
	}

	bool RequireNumber(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FString& Path, double& Out)
	{
		if (!Obj->TryGetNumberField(Key, Out))
		{
			Ctx.Fail(Join(Path, Key), TEXT("required number is missing or not a number"));
			return false;
		}
		return true;
	}

	void OptionalNumber(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, double& Out)
	{
		double Value;
		if (Obj->TryGetNumberField(Key, Value))
		{
			Out = Value;
		}
	}

	void OptionalBool(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, bool& Out)
	{
		bool Value;
		if (Obj->TryGetBoolField(Key, Value))
		{
			Out = Value;
		}
	}

	bool IsLocale(const FString& Value)
	{
		FString Language, Region;
		if (Value.Split(TEXT("-"), &Language, &Region))
		{
			return (Language.Len() == 2 || Language.Len() == 3) && Language.ToLower() == Language
				&& Region.Len() == 2 && Region.ToUpper() == Region;
		}
		return (Value.Len() == 2 || Value.Len() == 3) && Value.ToLower() == Value;
	}

	bool IsLocalizationKey(const FString& Value)
	{
		if (Value.IsEmpty() || Value.Len() > 96) return false;
		for (const TCHAR Character : Value)
		{
			if (!(FChar::IsAlnum(Character) || Character == TEXT('.') || Character == TEXT('_') ||
				Character == TEXT('-') || Character == TEXT(':'))) return false;
		}
		return true;
	}

	void RangeCheck(FParseContext& Ctx, double Value, double Min, double Max, const FString& Path)
	{
		if (Value < Min || Value > Max)
		{
			Ctx.Fail(Path, FString::Printf(TEXT("%g is outside the permitted range [%g, %g]"), Value, Min, Max));
		}
	}

	void RequirePositive(FParseContext& Ctx, double Value, const FString& Path)
	{
		if (Value <= 0.0)
		{
			Ctx.Fail(Path, FString::Printf(TEXT("%g must be greater than zero"), Value));
		}
	}

	/**
	 * A script path is relative to Scripts/ and must start with exactly one of the three
	 * case-exact top folders, followed by non-empty segments of path-safe characters: the
	 * schema's pattern, as the backend applies it. On success OutTopFolder names which
	 * one, for the side/path match check.
	 */
	bool IsScriptPath(const FString& Value, FString& OutTopFolder)
	{
		return FMantlePlaceLimits::IsScriptPath(Value, &OutTopFolder);
	}

	/** The side a script's path top folder implies, matching IsScriptPath's OutTopFolder. */
	const TCHAR* ExpectedSideForTopFolder(const FString& TopFolder)
	{
		if (TopFolder == TEXT("Server")) return TEXT("server");
		if (TopFolder == TEXT("Client")) return TEXT("client");
		return TEXT("shared");
	}

	/** Reads a fixed-length numeric array. */
	bool ReadNumberArray(
		FParseContext& Ctx,
		const TSharedPtr<FJsonObject>& Obj,
		const TCHAR* Key,
		int32 Expected,
		const FString& Path,
		TArray<double>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Obj->TryGetArrayField(Key, Array))
		{
			Ctx.Fail(Join(Path, Key), TEXT("required array is missing or not an array"));
			return false;
		}

		if (Array->Num() != Expected)
		{
			Ctx.Fail(Join(Path, Key), FString::Printf(TEXT("expected %d numbers, got %d"), Expected, Array->Num()));
			return false;
		}

		Out.Reset(Expected);
		for (int32 i = 0; i < Array->Num(); ++i)
		{
			double Value = 0.0;
			if (!(*Array)[i]->TryGetNumber(Value))
			{
				Ctx.Fail(Index(Join(Path, Key), i), TEXT("expected a number"));
				return false;
			}
			Out.Add(Value);
		}
		return true;
	}

	bool ReadColor(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FString& Path, FLinearColor& Out)
	{
		TArray<double> Values;
		if (!ReadNumberArray(Ctx, Obj, Key, 3, Path, Values))
		{
			return false;
		}

		bool bOk = true;
		for (int32 i = 0; i < 3; ++i)
		{
			if (Values[i] < 0.0 || Values[i] > 1.0)
			{
				Ctx.Fail(
					Index(Join(Path, Key), i),
					FString::Printf(TEXT("%g is outside the permitted range [0, 1]"), Values[i]));
				bOk = false;
			}
		}

		Out = FLinearColor(
			static_cast<float>(Values[0]),
			static_cast<float>(Values[1]),
			static_cast<float>(Values[2]));
		return bOk;
	}

	bool ReadTransform(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Parent, const FString& Path, FTransform& Out)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Parent->TryGetObjectField(TEXT("transform"), Obj))
		{
			Ctx.Fail(Join(Path, TEXT("transform")), TEXT("required object is missing"));
			return false;
		}

		const FString TPath = Join(Path, TEXT("transform"));
		RequireNoUnknownKeys(Ctx, *Obj, { TEXT("location"), TEXT("rotation"), TEXT("scale") }, TPath);

		TArray<double> Location, Rotation, Scale;
		const bool bLoc = ReadNumberArray(Ctx, *Obj, TEXT("location"), 3, TPath, Location);
		const bool bRot = ReadNumberArray(Ctx, *Obj, TEXT("rotation"), 3, TPath, Rotation);
		const bool bScl = ReadNumberArray(Ctx, *Obj, TEXT("scale"), 3, TPath, Scale);

		if (!bLoc || !bRot || !bScl)
		{
			return false;
		}

		bool bOk = true;
		for (int32 i = 0; i < 3; ++i)
		{
			if (Scale[i] <= 0.0)
			{
				Ctx.Fail(
					Index(Join(TPath, TEXT("scale")), i),
					FString::Printf(TEXT("%g must be greater than zero"), Scale[i]));
				bOk = false;
			}
		}

		// JSON rotation is [pitch, yaw, roll] in degrees.
		Out = FTransform(
			FRotator(Rotation[0], Rotation[1], Rotation[2]),
			FVector(Location[0], Location[1], Location[2]),
			FVector(Scale[0], Scale[1], Scale[2]));
		return bOk;
	}

	void ReadStringArray(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, TArray<FString>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Obj->TryGetArrayField(Key, Array))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Array)
			{
				FString Str;
				if (Value->TryGetString(Str))
				{
					Out.Add(Str);
				}
			}
		}
	}

	// materials is indexed by material slot: null is an unset slot, kept as an empty entry so
	// the ones after it stay on their own slots. Anything else is an error rather than
	// skipped, since skipping it would move every later override down a slot.
	void ReadMaterialSlots(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<FString>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (Obj->TryGetArrayField(TEXT("materials"), Array))
		{
			for (int32 Slot = 0; Slot < Array->Num(); ++Slot)
			{
				const TSharedPtr<FJsonValue>& Value = (*Array)[Slot];
				FString Str;
				if (Value->IsNull())
				{
					Out.Add(FString());
				}
				else if (Value->Type == EJson::String && Value->TryGetString(Str))
				{
					Out.Add(Str);
				}
				else
				{
					Ctx.Fail(Index(Join(Path, TEXT("materials")), Slot), TEXT("expected a string or null"));
					Out.Add(FString());
				}
			}
		}
	}

	/**
	 * A light's optional intensityUnits. Absent leaves Out at Unitless, so every place
	 * written before the field existed builds exactly as it always did.
	 */
	void ReadLightUnits(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Props, const FString& Path, EMantleLightUnits& Out)
	{
		if (!Props->HasField(TEXT("intensityUnits")))
		{
			return;
		}
		FString Units;
		if (!Props->TryGetStringField(TEXT("intensityUnits"), Units))
		{
			Ctx.Fail(Join(Path, TEXT("intensityUnits")), TEXT("must be a string"));
			return;
		}
		if (!MantleParseLightUnits(Units, Out))
		{
			Ctx.Fail(
				Join(Path, TEXT("intensityUnits")),
				FString::Printf(TEXT("'%s' is not a valid light unit (unitless, candelas, lumens, ev, nits)"), *Units));
		}
	}

	/** An optional number in [Min, Max]; absent leaves Out alone. */
	void ReadOptionalRange(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Props, const TCHAR* Key, const FString& Path,
		double Min, double Max, double& Out)
	{
		if (Props->HasField(Key) && RequireNumber(Ctx, Props, Key, Path, Out))
		{
			RangeCheck(Ctx, Out, Min, Max, Join(Path, Key));
		}
	}

	/**
	 * A light's optional useTemperature and temperature. Absent leaves the outputs at the
	 * defaults (no temperature, 6500 K), what every place before the fields meant.
	 */
	void ReadLightTemperature(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Props, const FString& Path, bool& bOutUse, double& OutTemperature)
	{
		OptionalBool(Props, TEXT("useTemperature"), bOutUse);
		ReadOptionalRange(Ctx, Props, TEXT("temperature"), Path,
			FMantlePlaceLimits::MinLightTemperature, FMantlePlaceLimits::MaxLightTemperature, OutTemperature);
	}

	/** A Point or Spot light's optional source shape; absent is a point source. */
	void ReadLightSourceShape(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Props, const FString& Path, FMantleLightSourceShape& Out)
	{
		ReadOptionalRange(Ctx, Props, TEXT("sourceRadius"), Path, 0.0, FMantlePlaceLimits::MaxLightSourceSize, Out.SourceRadius);
		ReadOptionalRange(Ctx, Props, TEXT("softSourceRadius"), Path, 0.0, FMantlePlaceLimits::MaxLightSourceSize, Out.SoftSourceRadius);
		ReadOptionalRange(Ctx, Props, TEXT("sourceLength"), Path, 0.0, FMantlePlaceLimits::MaxLightSourceSize, Out.SourceLength);
	}

	/** Prefix followed by exactly Digits characters, each a digit or, when bHex, a-f. */
	bool IsPrefixedId(const FString& Value, const TCHAR* Prefix, int32 Digits, bool bHex)
	{
		const int32 PrefixLen = FCString::Strlen(Prefix);
		if (Value.Len() != PrefixLen + Digits || !Value.StartsWith(Prefix, ESearchCase::CaseSensitive))
		{
			return false;
		}
		for (int32 i = PrefixLen; i < Value.Len(); ++i)
		{
			const TCHAR C = Value[i];
			if (!FChar::IsDigit(C) && !(bHex && C >= TEXT('a') && C <= TEXT('f')))
			{
				return false;
			}
		}
		return true;
	}

	/** An integer-valued number in [Min, Max], or a failure naming the path. */
	bool ReadBoundedInt(FParseContext& Ctx, double Value, int64 Min, int64 Max, const FString& Path, int32& Out)
	{
		if (Value != FMath::FloorToDouble(Value))
		{
			Ctx.Fail(Path, FString::Printf(TEXT("%g is not an integer"), Value));
			return false;
		}
		if (Value < static_cast<double>(Min) || Value > static_cast<double>(Max))
		{
			Ctx.Fail(Path, FString::Printf(TEXT("%g is outside the permitted range [%lld, %lld]"), Value, Min, Max));
			return false;
		}
		Out = static_cast<int32>(Value);
		return true;
	}

	/** Avatar descriptor v2's wear- and accessory-group slots (schema/avatar/limits.json). */
	bool IsCrowdWornSlot(const FString& Slot)
	{
		static const TCHAR* const Slots[] = {
			TEXT("hair"), TEXT("eyebrows"), TEXT("eyelashes"), TEXT("facial_hair"), TEXT("top"), TEXT("bottom"),
			TEXT("footwear"), TEXT("acc_hat"), TEXT("acc_face"), TEXT("acc_neck"),
			TEXT("acc_shoulder_l"), TEXT("acc_shoulder_r"), TEXT("acc_front"), TEXT("acc_back"), TEXT("acc_waist")
		};
		for (const TCHAR* Known : Slots)
		{
			if (Slot.Equals(Known, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	/** An optional [min, max] pair inside [Lo, Hi] with min <= max; absent leaves the outputs alone. */
	bool ReadRangePair(
		FParseContext& Ctx, const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FString& Path,
		double Lo, double Hi, bool bIntegers, double& OutMin, double& OutMax)
	{
		if (!Obj->HasField(Key))
		{
			return true;
		}
		TArray<double> Pair;
		if (!ReadNumberArray(Ctx, Obj, Key, 2, Path, Pair))
		{
			return false;
		}
		bool bOk = true;
		for (int32 i = 0; i < 2; ++i)
		{
			if (bIntegers && Pair[i] != FMath::FloorToDouble(Pair[i]))
			{
				Ctx.Fail(Index(Join(Path, Key), i), FString::Printf(TEXT("%g is not an integer"), Pair[i]));
				bOk = false;
			}
			else if (Pair[i] < Lo || Pair[i] > Hi)
			{
				Ctx.Fail(Index(Join(Path, Key), i), FString::Printf(TEXT("%g is outside the permitted range [%g, %g]"), Pair[i], Lo, Hi));
				bOk = false;
			}
		}
		if (bOk && Pair[0] > Pair[1])
		{
			Ctx.Fail(Join(Path, Key), FString::Printf(TEXT("min %g is greater than max %g"), Pair[0], Pair[1]));
			bOk = false;
		}
		if (bOk)
		{
			OutMin = Pair[0];
			OutMax = Pair[1];
		}
		return bOk;
	}

	void ParseCrowdLane(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleCrowdLane& Out)
	{
		RequireNoUnknownKeys(Ctx, Obj, { TEXT("points"), TEXT("width"), TEXT("twoWay") }, Path);
		const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
		if (!Obj->TryGetArrayField(TEXT("points"), Points))
		{
			Ctx.Fail(Join(Path, TEXT("points")), TEXT("required array is missing or not an array"));
		}
		else if (Points->Num() < 2 || Points->Num() > FMantlePlaceLimits::MaxCrowdLanePoints)
		{
			Ctx.Fail(
				Join(Path, TEXT("points")),
				FString::Printf(TEXT("has %d points; a lane needs 2 to %d"), Points->Num(), FMantlePlaceLimits::MaxCrowdLanePoints));
		}
		else
		{
			for (int32 i = 0; i < Points->Num(); ++i)
			{
				const TArray<TSharedPtr<FJsonValue>>* Xyz = nullptr;
				double X = 0.0, Y = 0.0, Z = 0.0;
				if (!(*Points)[i]->TryGetArray(Xyz) || Xyz->Num() != 3
					|| !(*Xyz)[0]->TryGetNumber(X) || !(*Xyz)[1]->TryGetNumber(Y) || !(*Xyz)[2]->TryGetNumber(Z))
				{
					Ctx.Fail(Index(Join(Path, TEXT("points")), i), TEXT("expected an array of 3 numbers"));
					continue;
				}
				Out.Points.Add(FVector(X, Y, Z));
			}
		}
		if (Obj->HasField(TEXT("width")) && RequireNumber(Ctx, Obj, TEXT("width"), Path, Out.Width))
		{
			RangeCheck(Ctx, Out.Width, 0.0, FMantlePlaceLimits::MaxCrowdLaneWidth, Join(Path, TEXT("width")));
		}
		OptionalBool(Obj, TEXT("twoWay"), Out.bTwoWay);
	}

	void ParseCrowdLook(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleCrowdLook& Out)
	{
		RequireNoUnknownKeys(Ctx, Obj, { TEXT("body"), TEXT("wearing") }, Path);
		const TSharedPtr<FJsonObject>* Body = nullptr;
		const FString BodyPath = Join(Path, TEXT("body"));
		if (!Obj->TryGetObjectField(TEXT("body"), Body))
		{
			Ctx.Fail(BodyPath, TEXT("required object is missing"));
		}
		else
		{
			RequireNoUnknownKeys(Ctx, *Body, { TEXT("avatarItemId"), TEXT("skinToneId"), TEXT("eyeColorId"), TEXT("heightPct") }, BodyPath);
			if (RequireString(Ctx, *Body, TEXT("avatarItemId"), BodyPath, Out.AvatarItemId)
				&& !IsPrefixedId(Out.AvatarItemId, TEXT("itm_u"), 32, true))
			{
				Ctx.Fail(Join(BodyPath, TEXT("avatarItemId")), TEXT("is not an item id (itm_u + 32 lowercase hex)"));
			}
			// Present but not a string is an error too, as in the schema; absent means the seed picks.
			if ((*Body)->HasField(TEXT("skinToneId"))
				&& (!(*Body)->TryGetStringField(TEXT("skinToneId"), Out.SkinToneId) || !IsPrefixedId(Out.SkinToneId, TEXT("tone_"), 2, false)))
			{
				Ctx.Fail(Join(BodyPath, TEXT("skinToneId")), TEXT("is not a skin tone id (tone_ + 2 digits)"));
			}
			if ((*Body)->HasField(TEXT("eyeColorId"))
				&& (!(*Body)->TryGetStringField(TEXT("eyeColorId"), Out.EyeColorId) || !IsPrefixedId(Out.EyeColorId, TEXT("eye_"), 2, false)))
			{
				Ctx.Fail(Join(BodyPath, TEXT("eyeColorId")), TEXT("is not an eye colour id (eye_ + 2 digits)"));
			}
			double Height = 0.0;
			if ((*Body)->HasField(TEXT("heightPct")) && RequireNumber(Ctx, *Body, TEXT("heightPct"), BodyPath, Height))
			{
				ReadBoundedInt(Ctx, Height, FMantlePlaceLimits::MinCrowdHeightPct, FMantlePlaceLimits::MaxCrowdHeightPct,
					Join(BodyPath, TEXT("heightPct")), Out.HeightPct);
			}
		}

		if (!Obj->HasField(TEXT("wearing")))
		{
			return;
		}
		const FString WearingPath = Join(Path, TEXT("wearing"));
		const TArray<TSharedPtr<FJsonValue>>* Wearing = nullptr;
		if (!Obj->TryGetArrayField(TEXT("wearing"), Wearing))
		{
			Ctx.Fail(WearingPath, TEXT("must be an array"));
			return;
		}
		if (Wearing->Num() > FMantlePlaceLimits::MaxCrowdLookWearing)
		{
			Ctx.Fail(WearingPath, FString::Printf(TEXT("has more than %d entries"), FMantlePlaceLimits::MaxCrowdLookWearing));
			return;
		}
		TSet<FString> SlotsUsed;
		for (int32 i = 0; i < Wearing->Num(); ++i)
		{
			const FString WornPath = Index(WearingPath, i);
			const TSharedPtr<FJsonObject>* Worn = nullptr;
			if (!(*Wearing)[i]->TryGetObject(Worn))
			{
				Ctx.Fail(WornPath, TEXT("expected an object"));
				continue;
			}
			RequireNoUnknownKeys(Ctx, *Worn, { TEXT("slot"), TEXT("itemId") }, WornPath);
			FMantleCrowdWorn Entry;
			if (RequireString(Ctx, *Worn, TEXT("slot"), WornPath, Entry.Slot))
			{
				bool bSlotUsed = false;
				SlotsUsed.Add(Entry.Slot, &bSlotUsed);
				if (!IsCrowdWornSlot(Entry.Slot))
				{
					Ctx.Fail(Join(WornPath, TEXT("slot")), FString::Printf(TEXT("'%s' is not a wear or accessory slot"), *Entry.Slot));
				}
				else if (bSlotUsed)
				{
					// Avatar descriptor v2 allows one item per slot (slotCap_<slot>).
					Ctx.Fail(Join(WornPath, TEXT("slot")), FString::Printf(TEXT("'%s' is worn more than once"), *Entry.Slot));
				}
			}
			if (RequireString(Ctx, *Worn, TEXT("itemId"), WornPath, Entry.ItemId) && !IsPrefixedId(Entry.ItemId, TEXT("itm_u"), 32, true))
			{
				Ctx.Fail(Join(WornPath, TEXT("itemId")), TEXT("is not an item id (itm_u + 32 lowercase hex)"));
			}
			Out.Wearing.Add(MoveTemp(Entry));
		}
	}

	void ParseCrowdVariation(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Variation, const FString& Path, FMantleCrowdSpawnerProps& P)
	{
		RequireNoUnknownKeys(Ctx, Variation, { TEXT("heightPct"), TEXT("skinTones") }, Path);
		double MinHeight = P.MinHeightPct, MaxHeight = P.MaxHeightPct;
		if (ReadRangePair(Ctx, Variation, TEXT("heightPct"), Path,
			FMantlePlaceLimits::MinCrowdHeightPct, FMantlePlaceLimits::MaxCrowdHeightPct, true, MinHeight, MaxHeight))
		{
			P.MinHeightPct = static_cast<int32>(MinHeight);
			P.MaxHeightPct = static_cast<int32>(MaxHeight);
		}
		if (!Variation->HasField(TEXT("skinTones")))
		{
			return;
		}
		const FString TonesPath = Join(Path, TEXT("skinTones"));
		const TArray<TSharedPtr<FJsonValue>>* Tones = nullptr;
		if (!Variation->TryGetArrayField(TEXT("skinTones"), Tones))
		{
			Ctx.Fail(TonesPath, TEXT("must be an array"));
			return;
		}
		if (Tones->Num() < 1 || Tones->Num() > FMantlePlaceLimits::MaxCrowdSkinTones)
		{
			Ctx.Fail(TonesPath, FString::Printf(TEXT("needs 1 to %d tones"), FMantlePlaceLimits::MaxCrowdSkinTones));
			return;
		}
		for (int32 i = 0; i < Tones->Num(); ++i)
		{
			FString Tone;
			if (!(*Tones)[i]->TryGetString(Tone) || !IsPrefixedId(Tone, TEXT("tone_"), 2, false))
			{
				Ctx.Fail(Index(TonesPath, i), TEXT("is not a skin tone id (tone_ + 2 digits)"));
			}
			else if (P.SkinTones.Contains(Tone))
			{
				Ctx.Fail(Index(TonesPath, i), FString::Printf(TEXT("'%s' is listed more than once"), *Tone));
			}
			else
			{
				P.SkinTones.Add(Tone);
			}
		}
	}

	/** One of a spawner's points of interest. That its entity is another one of the place is checked once every entity is read. */
	void ParseCrowdPointOfInterest(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleCrowdPointOfInterest& Out)
	{
		RequireNoUnknownKeys(Ctx, Obj, { TEXT("entity"), TEXT("chance"), TEXT("lingerSeconds"), TEXT("radius") }, Path);
		RequireString(Ctx, Obj, TEXT("entity"), Path, Out.EntityId);
		if (Obj->HasField(TEXT("chance")) && RequireNumber(Ctx, Obj, TEXT("chance"), Path, Out.Chance))
		{
			RangeCheck(Ctx, Out.Chance, 0.0, 1.0, Join(Path, TEXT("chance")));
		}
		ReadRangePair(Ctx, Obj, TEXT("lingerSeconds"), Path, FMantlePlaceLimits::MinCrowdPointLingerSeconds,
			FMantlePlaceLimits::MaxCrowdPointLingerSeconds, false, Out.MinLingerSeconds, Out.MaxLingerSeconds);
		if (Obj->HasField(TEXT("radius")) && RequireNumber(Ctx, Obj, TEXT("radius"), Path, Out.Radius))
		{
			RangeCheck(Ctx, Out.Radius, 0.0, FMantlePlaceLimits::MaxCrowdPointRadius, Join(Path, TEXT("radius")));
		}
	}

	void ParseCrowdSpawner(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Props, const FString& Path, FMantleCrowdSpawnerProps& P)
	{
		RequireNoUnknownKeys(
			Ctx, Props,
			{ TEXT("lanes"), TEXT("count"), TEXT("seed"), TEXT("looks"), TEXT("variation"), TEXT("walkSpeed"),
			  TEXT("idleChance"), TEXT("pointsOfInterest"), TEXT("enabled") },
			Path);

		const TArray<TSharedPtr<FJsonValue>>* Lanes = nullptr;
		if (!Props->TryGetArrayField(TEXT("lanes"), Lanes))
		{
			Ctx.Fail(Join(Path, TEXT("lanes")), TEXT("required array is missing or not an array"));
		}
		else if (Lanes->Num() < 1 || Lanes->Num() > FMantlePlaceLimits::MaxCrowdLanes)
		{
			Ctx.Fail(
				Join(Path, TEXT("lanes")),
				FString::Printf(TEXT("has %d lanes; a spawner needs 1 to %d"), Lanes->Num(), FMantlePlaceLimits::MaxCrowdLanes));
		}
		else
		{
			for (int32 i = 0; i < Lanes->Num(); ++i)
			{
				const TSharedPtr<FJsonObject>* Lane = nullptr;
				if (!(*Lanes)[i]->TryGetObject(Lane))
				{
					Ctx.Fail(Index(Join(Path, TEXT("lanes")), i), TEXT("expected an object"));
					continue;
				}
				FMantleCrowdLane Parsed;
				ParseCrowdLane(Ctx, *Lane, Index(Join(Path, TEXT("lanes")), i), Parsed);
				P.Lanes.Add(MoveTemp(Parsed));
			}
		}

		double Number = 0.0;
		if (RequireNumber(Ctx, Props, TEXT("count"), Path, Number))
		{
			ReadBoundedInt(Ctx, Number, 1, FMantlePlaceLimits::MaxCrowdCount, Join(Path, TEXT("count")), P.Count);
		}
		if (RequireNumber(Ctx, Props, TEXT("seed"), Path, Number))
		{
			ReadBoundedInt(Ctx, Number, 0, FMantlePlaceLimits::MaxCrowdSeed, Join(Path, TEXT("seed")), P.Seed);
		}

		if (Props->HasField(TEXT("looks")))
		{
			const TArray<TSharedPtr<FJsonValue>>* Looks = nullptr;
			if (!Props->TryGetArrayField(TEXT("looks"), Looks))
			{
				Ctx.Fail(Join(Path, TEXT("looks")), TEXT("must be an array"));
			}
			else if (Looks->Num() > FMantlePlaceLimits::MaxCrowdLooks)
			{
				Ctx.Fail(Join(Path, TEXT("looks")), FString::Printf(TEXT("has more than %d entries"), FMantlePlaceLimits::MaxCrowdLooks));
			}
			else
			{
				for (int32 i = 0; i < Looks->Num(); ++i)
				{
					const TSharedPtr<FJsonObject>* Look = nullptr;
					if (!(*Looks)[i]->TryGetObject(Look))
					{
						Ctx.Fail(Index(Join(Path, TEXT("looks")), i), TEXT("expected an object"));
						continue;
					}
					FMantleCrowdLook Parsed;
					ParseCrowdLook(Ctx, *Look, Index(Join(Path, TEXT("looks")), i), Parsed);
					P.Looks.Add(MoveTemp(Parsed));
				}
			}
		}

		if (Props->HasField(TEXT("variation")))
		{
			const TSharedPtr<FJsonObject>* Variation = nullptr;
			if (!Props->TryGetObjectField(TEXT("variation"), Variation))
			{
				Ctx.Fail(Join(Path, TEXT("variation")), TEXT("must be an object"));
			}
			else
			{
				ParseCrowdVariation(Ctx, *Variation, Join(Path, TEXT("variation")), P);
			}
		}

		ReadRangePair(Ctx, Props, TEXT("walkSpeed"), Path,
			FMantlePlaceLimits::MinCrowdWalkSpeed, FMantlePlaceLimits::MaxCrowdWalkSpeed, false, P.MinWalkSpeed, P.MaxWalkSpeed);
		if (Props->HasField(TEXT("idleChance")) && RequireNumber(Ctx, Props, TEXT("idleChance"), Path, P.IdleChance))
		{
			RangeCheck(Ctx, P.IdleChance, 0.0, 1.0, Join(Path, TEXT("idleChance")));
		}
		if (Props->HasField(TEXT("pointsOfInterest")))
		{
			const FString PointsPath = Join(Path, TEXT("pointsOfInterest"));
			const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
			if (!Props->TryGetArrayField(TEXT("pointsOfInterest"), Points))
			{
				Ctx.Fail(PointsPath, TEXT("must be an array"));
			}
			else if (Points->Num() > FMantlePlaceLimits::MaxCrowdPointsOfInterest)
			{
				Ctx.Fail(PointsPath, FString::Printf(TEXT("has more than %d entries"), FMantlePlaceLimits::MaxCrowdPointsOfInterest));
			}
			else
			{
				for (int32 i = 0; i < Points->Num(); ++i)
				{
					const TSharedPtr<FJsonObject>* Point = nullptr;
					if (!(*Points)[i]->TryGetObject(Point))
					{
						Ctx.Fail(Index(PointsPath, i), TEXT("expected an object"));
						continue;
					}
					FMantleCrowdPointOfInterest Parsed;
					ParseCrowdPointOfInterest(Ctx, *Point, Index(PointsPath, i), Parsed);
					P.PointsOfInterest.Add(MoveTemp(Parsed));
				}
			}
		}
		OptionalBool(Props, TEXT("enabled"), P.bEnabled);
	}

	/** The mesh, materials, collision, shadow and draw distance a StaticMesh and an InstancedMesh share. */
	void ReadMeshProps(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Props, const FString& Path, FMantleStaticMeshProps& P)
	{
		RequireString(Ctx, Props, TEXT("mesh"), Path, P.Mesh);
		ReadMaterialSlots(Ctx, Props, Path, P.Materials);
		if (P.Materials.Num() > FMantlePlaceLimits::MaxMaterialSlots)
		{
			Ctx.Fail(Join(Path, TEXT("materials")), FString::Printf(TEXT("has more than %d entries"), FMantlePlaceLimits::MaxMaterialSlots));
		}

		FString Collision;
		if (Props->TryGetStringField(TEXT("collision"), Collision) && !MantleParseCollision(Collision, P.Collision))
		{
			Ctx.Fail(Join(Path, TEXT("collision")), FString::Printf(TEXT("'%s' is not a valid collision mode"), *Collision));
		}
		OptionalBool(Props, TEXT("castShadow"), P.bCastShadow);

		// #630: a typed number in (0, MaxDrawDistance]; absent leaves 0, never culled.
		if (const TSharedPtr<FJsonValue> Distance = Props->TryGetField(TEXT("drawDistance")))
		{
			const FString DistancePath = Join(Path, TEXT("drawDistance"));
			if (Distance->Type != EJson::Number)
			{
				Ctx.Fail(DistancePath, TEXT("must be a number"));
			}
			else if (const double Value = Distance->AsNumber(); !(Value > 0.0 && Value <= FMantlePlaceLimits::MaxDrawDistance))
			{
				Ctx.Fail(DistancePath, FString::Printf(TEXT("%g is outside the permitted range (0, %g]"), Value, FMantlePlaceLimits::MaxDrawDistance));
			}
			else
			{
				P.DrawDistance = Value;
			}
		}
	}

	/**
	 * An optional surface on a mesh or a terrain layer: one of the surface names, case-exact, as
	 * the schema's enum, validate.py and the backend hold them. Left out, Out keeps Default.
	 */
	void ReadSurface(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Obj, const FString& Path, EMantleSurface& Out)
	{
		const TSharedPtr<FJsonValue> Value = Obj->TryGetField(TEXT("surface"));
		if (!Value.IsValid())
		{
			return;
		}
		if (Value->Type != EJson::String || !MantleParseSurface(Value->AsString(), Out))
		{
			Ctx.Fail(Join(Path, TEXT("surface")), FString::Printf(TEXT("must be one of %s"), *MantleSurfaceNameList()));
		}
	}

	/** An optional boolean in a block whose every key is typed (physics): anything else is refused. */
	void ReadStrictOptionalBool(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FString& Path, bool& Out)
	{
		const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Key);
		if (!Value.IsValid())
		{
			return;
		}
		// Typed, not TryGetBoolField: that one reads a number or a string as a boolean too.
		if (Value->Type != EJson::Boolean)
		{
			Ctx.Fail(Join(Path, Key), TEXT("must be true or false"));
			return;
		}
		Out = Value->AsBool();
	}

	/** An optional number of settings.environment: typed, finite and in [Min, Max], or refused. */
	void ReadEnvironmentNumber(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FString& Path,
		double Min, double Max, TOptional<double>& Out)
	{
		const TSharedPtr<FJsonValue> Value = Obj->TryGetField(Key);
		if (!Value.IsValid())
		{
			return;
		}
		double Number = 0.0;
		// Typed, not TryGetNumberField: that one reads a numeric string as a number too.
		if (Value->Type != EJson::Number || !Value->TryGetNumber(Number) || !FMath::IsFinite(Number))
		{
			Ctx.Fail(Join(Path, Key), TEXT("must be a number"));
			return;
		}
		if (Number < Min || Number > Max)
		{
			RangeCheck(Ctx, Number, Min, Max, Join(Path, Key));
			return;
		}
		Out = Number;
	}

	/** An optional linear luminance colour of settings.environment.fog: three numbers, each 0 to MaxFogColor. */
	void ReadEnvironmentLuminance(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FString& Path,
		TOptional<FLinearColor>& Out)
	{
		if (!Obj->HasField(Key))
		{
			return;
		}
		TArray<double> Values;
		if (!ReadNumberArray(Ctx, Obj, Key, 3, Path, Values))
		{
			return;
		}
		bool bOk = true;
		for (int32 Channel = 0; Channel < 3; ++Channel)
		{
			if (!FMath::IsFinite(Values[Channel]) || Values[Channel] < 0.0 || Values[Channel] > MantleEnvironmentDefaults::MaxFogColor)
			{
				RangeCheck(Ctx, Values[Channel], 0.0, MantleEnvironmentDefaults::MaxFogColor, Index(Join(Path, Key), Channel));
				bOk = false;
			}
		}
		if (bOk)
		{
			Out = FLinearColor(static_cast<float>(Values[0]), static_cast<float>(Values[1]), static_cast<float>(Values[2]));
		}
	}

	/**
	 * settings.environment.exposure: the mode (case-exact), the bias in stops and, with mode auto only, the
	 * brightness range, whose minimum is not above its maximum.
	 */
	void ReadEnvironmentExposure(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Environment, const FString& EnvironmentPath,
		TOptional<FMantleExposureSettings>& Out)
	{
		if (!Environment->HasField(TEXT("exposure")))
		{
			return;
		}
		const FString ExposurePath = Join(EnvironmentPath, TEXT("exposure"));
		const TSharedPtr<FJsonObject>* Exposure = nullptr;
		if (!Environment->TryGetObjectField(TEXT("exposure"), Exposure))
		{
			Ctx.Fail(ExposurePath, TEXT("must be an object"));
			return;
		}
		RequireNoUnknownKeys(Ctx, *Exposure, { TEXT("mode"), TEXT("bias"), TEXT("minBrightness"), TEXT("maxBrightness") }, ExposurePath);

		FMantleExposureSettings Parsed;
		bool bModeValid = true;
		if (const TSharedPtr<FJsonValue> Mode = (*Exposure)->TryGetField(TEXT("mode")))
		{
			if (Mode->Type == EJson::String && Mode->AsString().Equals(TEXT("fixed"), ESearchCase::CaseSensitive))
			{
				Parsed.Mode = EMantleExposureMode::Fixed;
			}
			else if (Mode->Type == EJson::String && Mode->AsString().Equals(TEXT("auto"), ESearchCase::CaseSensitive))
			{
				Parsed.Mode = EMantleExposureMode::Auto;
			}
			else
			{
				Ctx.Fail(Join(ExposurePath, TEXT("mode")), TEXT("must be fixed or auto"));
				bModeValid = false;
			}
		}

		TOptional<double> Bias;
		ReadEnvironmentNumber(Ctx, *Exposure, TEXT("bias"), ExposurePath,
			-MantleEnvironmentDefaults::MaxExposureBias, MantleEnvironmentDefaults::MaxExposureBias, Bias);
		Parsed.Bias = Bias.Get(0.0);

		ReadEnvironmentNumber(Ctx, *Exposure, TEXT("minBrightness"), ExposurePath,
			MantleEnvironmentDefaults::MinExposureBrightness, MantleEnvironmentDefaults::MaxExposureBrightness, Parsed.MinBrightness);
		ReadEnvironmentNumber(Ctx, *Exposure, TEXT("maxBrightness"), ExposurePath,
			MantleEnvironmentDefaults::MinExposureBrightness, MantleEnvironmentDefaults::MaxExposureBrightness, Parsed.MaxBrightness);
		if (bModeValid && Parsed.Mode != EMantleExposureMode::Auto)
		{
			for (const TCHAR* Key : { TEXT("minBrightness"), TEXT("maxBrightness") })
			{
				if ((*Exposure)->HasField(Key))
				{
					Ctx.Fail(Join(ExposurePath, Key), TEXT("is read only with mode auto"));
				}
			}
		}
		if (Parsed.MinBrightness.IsSet() && Parsed.MaxBrightness.IsSet() && *Parsed.MinBrightness > *Parsed.MaxBrightness)
		{
			Ctx.Fail(Join(ExposurePath, TEXT("minBrightness")),
				FString::Printf(TEXT("%g is greater than maxBrightness %g"), *Parsed.MinBrightness, *Parsed.MaxBrightness));
		}
		Out = Parsed;
	}

	/** settings.environment.fog: the height fog's look, each key optional. */
	void ReadEnvironmentFog(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Environment, const FString& EnvironmentPath,
		TOptional<FMantleFogSettings>& Out)
	{
		if (!Environment->HasField(TEXT("fog")))
		{
			return;
		}
		const FString FogPath = Join(EnvironmentPath, TEXT("fog"));
		const TSharedPtr<FJsonObject>* Fog = nullptr;
		if (!Environment->TryGetObjectField(TEXT("fog"), Fog))
		{
			Ctx.Fail(FogPath, TEXT("must be an object"));
			return;
		}
		RequireNoUnknownKeys(
			Ctx, *Fog,
			{ TEXT("density"), TEXT("heightFalloff"), TEXT("inscatteringColor"), TEXT("startDistance"), TEXT("directionalInscatteringColor") },
			FogPath);

		FMantleFogSettings Parsed;
		ReadEnvironmentNumber(Ctx, *Fog, TEXT("density"), FogPath, 0.0, MantleEnvironmentDefaults::MaxFogDensity, Parsed.Density);
		ReadEnvironmentNumber(Ctx, *Fog, TEXT("heightFalloff"), FogPath,
			MantleEnvironmentDefaults::MinFogHeightFalloff, MantleEnvironmentDefaults::MaxFogHeightFalloff, Parsed.HeightFalloff);
		ReadEnvironmentNumber(Ctx, *Fog, TEXT("startDistance"), FogPath, 0.0, MantleEnvironmentDefaults::MaxFogStartDistance, Parsed.StartDistance);
		ReadEnvironmentLuminance(Ctx, *Fog, TEXT("inscatteringColor"), FogPath, Parsed.InscatteringColor);
		ReadEnvironmentLuminance(Ctx, *Fog, TEXT("directionalInscatteringColor"), FogPath, Parsed.DirectionalInscatteringColor);
		Out = Parsed;
	}

	/** settings.environment.skyLight: the scale of Mantle's own sky light (#722), and settings.environment.sun: whether the sun reaches the place. */
	void ReadEnvironmentLighting(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Environment, const FString& EnvironmentPath,
		FMantleEnvironmentSettings& Out)
	{
		if (Environment->HasField(TEXT("skyLight")))
		{
			const FString SkyPath = Join(EnvironmentPath, TEXT("skyLight"));
			const TSharedPtr<FJsonObject>* SkyLight = nullptr;
			if (!Environment->TryGetObjectField(TEXT("skyLight"), SkyLight))
			{
				Ctx.Fail(SkyPath, TEXT("must be an object"));
			}
			else
			{
				RequireNoUnknownKeys(Ctx, *SkyLight, { TEXT("intensity") }, SkyPath);
				ReadEnvironmentNumber(Ctx, *SkyLight, TEXT("intensity"), SkyPath, 0.0, 1.0, Out.SkyLightIntensity);
			}
		}
		if (Environment->HasField(TEXT("sun")))
		{
			const FString SunPath = Join(EnvironmentPath, TEXT("sun"));
			const TSharedPtr<FJsonObject>* Sun = nullptr;
			if (!Environment->TryGetObjectField(TEXT("sun"), Sun))
			{
				Ctx.Fail(SunPath, TEXT("must be an object"));
			}
			else
			{
				RequireNoUnknownKeys(Ctx, *Sun, { TEXT("direct") }, SunPath);
				bool bDirect = true;
				ReadStrictOptionalBool(Ctx, *Sun, TEXT("direct"), SunPath, bDirect);
				Out.bNoDirectSun = !bDirect;
			}
		}
	}

	/**
	 * settings.environment: the place's default look, each key optional, in the script API's
	 * ranges (MantleEnvironmentDefaults) as the schema, validate.py and the backend hold them.
	 */
	void ReadEnvironmentSettings(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Settings, const FString& Path, FMantleEnvironmentSettings& Out)
	{
		if (!Settings->HasField(TEXT("environment")))
		{
			return;
		}
		const FString EnvironmentPath = Join(Path, TEXT("environment"));
		const TSharedPtr<FJsonObject>* Environment = nullptr;
		if (!Settings->TryGetObjectField(TEXT("environment"), Environment))
		{
			Ctx.Fail(EnvironmentPath, TEXT("must be an object"));
			return;
		}
		RequireNoUnknownKeys(
			Ctx, *Environment,
			{ TEXT("clockTime"), TEXT("timeScale"), TEXT("weather"), TEXT("colorTint"), TEXT("saturation"),
			  TEXT("bloom"), TEXT("colorGrading"), TEXT("colorGradingIntensity"), TEXT("exposure"), TEXT("fog"),
			  TEXT("skyLight"), TEXT("sun") },
			EnvironmentPath);
		ReadEnvironmentExposure(Ctx, *Environment, EnvironmentPath, Out.Exposure);
		ReadEnvironmentFog(Ctx, *Environment, EnvironmentPath, Out.Fog);
		ReadEnvironmentLighting(Ctx, *Environment, EnvironmentPath, Out);

		// 24 is the same hour as 0, as Environment.ClockTime wraps it.
		ReadEnvironmentNumber(Ctx, *Environment, TEXT("clockTime"), EnvironmentPath, 0.0, 24.0, Out.ClockTime);
		ReadEnvironmentNumber(Ctx, *Environment, TEXT("timeScale"), EnvironmentPath,
			0.0, MantleEnvironmentDefaults::MaxScriptTimeScale, Out.TimeScale);
		ReadEnvironmentNumber(Ctx, *Environment, TEXT("saturation"), EnvironmentPath,
			0.0, MantleEnvironmentDefaults::MaxScriptSaturation, Out.Saturation);
		ReadEnvironmentNumber(Ctx, *Environment, TEXT("bloom"), EnvironmentPath,
			0.0, MantleEnvironmentDefaults::MaxScriptBloom, Out.Bloom);
		ReadEnvironmentNumber(Ctx, *Environment, TEXT("colorGradingIntensity"), EnvironmentPath,
			0.0, 1.0, Out.ColorGradingIntensity);

		if (const TSharedPtr<FJsonValue> Grading = (*Environment)->TryGetField(TEXT("colorGrading")))
		{
			// Case-exact, as Environment.ColorGrading takes it.
			uint8 Preset = MantleEnvironmentOverrideValues::ColorGradingNone;
			if (Grading->Type == EJson::String && FMantleEnvironmentOverride::ParseColorGradingName(Grading->AsString(), Preset))
			{
				Out.ColorGrading = Preset;
			}
			else
			{
				Ctx.Fail(Join(EnvironmentPath, TEXT("colorGrading")),
					FString::Printf(TEXT("must be %s"), *FMantleEnvironmentOverride::ColorGradingNameList()));
			}
		}

		if (const TSharedPtr<FJsonValue> Weather = (*Environment)->TryGetField(TEXT("weather")))
		{
			EMantleWeatherState State = EMantleWeatherState::Clear;
			if (Weather->Type == EJson::String && MantleParseWeatherState(Weather->AsString(), State))
			{
				Out.Weather = State;
			}
			else
			{
				Ctx.Fail(Join(EnvironmentPath, TEXT("weather")), TEXT("must be one of Clear, PartlyCloudy, Overcast, Rain, Storm"));
			}
		}

		if (const TSharedPtr<FJsonValue> Tint = (*Environment)->TryGetField(TEXT("colorTint")))
		{
			// "#RRGGBB" with the hash, as the schema's pattern has it; ParseHexRgb alone would also
			// take it without.
			const FString Text = Tint->Type == EJson::String ? Tint->AsString() : FString();
			uint32 Rgb = 0;
			if (Text.Len() == 7 && Text[0] == TEXT('#') && FMantleEnvironmentOverride::ParseHexRgb(Text, Rgb))
			{
				Out.ColorTint = Rgb;
			}
			else
			{
				Ctx.Fail(Join(EnvironmentPath, TEXT("colorTint")), TEXT("must be a colour \"#RRGGBB\""));
			}
		}
	}

	/**
	 * A StaticMesh's physics block: simulate, mass, linearDamping, angularDamping, gravity,
	 * material, each optional, each held to FMantlePlaceLimits' ranges as the schema, validate.py and the
	 * backend hold them.
	 */
	void ReadPhysicsProps(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Props, const FString& Path, TOptional<FMantlePhysicsProps>& Out)
	{
		if (!Props->HasField(TEXT("physics")))
		{
			return;
		}
		const FString PhysicsPath = Join(Path, TEXT("physics"));
		const TSharedPtr<FJsonObject>* Physics = nullptr;
		if (!Props->TryGetObjectField(TEXT("physics"), Physics))
		{
			Ctx.Fail(PhysicsPath, TEXT("must be an object"));
			return;
		}
		RequireNoUnknownKeys(
			Ctx, *Physics,
			{ TEXT("simulate"), TEXT("mass"), TEXT("linearDamping"), TEXT("angularDamping"), TEXT("gravity"), TEXT("material") },
			PhysicsPath);

		FMantlePhysicsProps P;
		ReadStrictOptionalBool(Ctx, *Physics, TEXT("simulate"), PhysicsPath, P.bSimulate);
		ReadStrictOptionalBool(Ctx, *Physics, TEXT("gravity"), PhysicsPath, P.bGravity);
		double Mass = 0.0;
		if ((*Physics)->HasField(TEXT("mass")) && RequireNumber(Ctx, *Physics, TEXT("mass"), PhysicsPath, Mass))
		{
			RangeCheck(Ctx, Mass, FMantlePlaceLimits::MinPhysicsMass, FMantlePlaceLimits::MaxPhysicsMass, Join(PhysicsPath, TEXT("mass")));
			P.Mass = Mass;
		}
		if ((*Physics)->HasField(TEXT("linearDamping"))
			&& RequireNumber(Ctx, *Physics, TEXT("linearDamping"), PhysicsPath, P.LinearDamping))
		{
			RangeCheck(Ctx, P.LinearDamping, 0.0, FMantlePlaceLimits::MaxPhysicsDamping, Join(PhysicsPath, TEXT("linearDamping")));
		}
		if ((*Physics)->HasField(TEXT("angularDamping"))
			&& RequireNumber(Ctx, *Physics, TEXT("angularDamping"), PhysicsPath, P.AngularDamping))
		{
			RangeCheck(Ctx, P.AngularDamping, 0.0, FMantlePlaceLimits::MaxPhysicsDamping, Join(PhysicsPath, TEXT("angularDamping")));
		}
		// One of the six presets, case-exact, as the schema's enum and the backend hold them.
		if (const TSharedPtr<FJsonValue> Material = (*Physics)->TryGetField(TEXT("material")))
		{
			if (Material->Type != EJson::String || !MantleParsePhysicsMaterial(Material->AsString(), P.Material))
			{
				Ctx.Fail(Join(PhysicsPath, TEXT("material")), TEXT("must be one of default, rubber, ice, metal, wood, stone"));
			}
		}
		Out = P;
	}

	/**
	 * An InstancedMesh's instances: 1 to FMantlePlaceLimits::MaxInstancesPerEntity arrays of
	 * [x, y, z, pitch, yaw, roll, sx, sy, sz], each scale above zero. Rotation is [pitch, yaw, roll]
	 * in degrees, as in ReadTransform. Stops at the first bad instance: a place wrong in every one
	 * of 4096 instances needs one message, not 4096.
	 */
	void ReadInstances(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Props, const FString& Path, TArray<FTransform>& Out)
	{
		const FString InstancesPath = Join(Path, TEXT("instances"));
		const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
		if (!Props->TryGetArrayField(TEXT("instances"), Array))
		{
			Ctx.Fail(InstancesPath, TEXT("required array is missing or not an array"));
			return;
		}
		if (Array->Num() == 0)
		{
			Ctx.Fail(InstancesPath, TEXT("must hold at least one instance"));
			return;
		}
		if (Array->Num() > FMantlePlaceLimits::MaxInstancesPerEntity)
		{
			Ctx.Fail(InstancesPath, FString::Printf(TEXT("has %d instances; an InstancedMesh holds at most %d"),
				Array->Num(), FMantlePlaceLimits::MaxInstancesPerEntity));
			return;
		}

		Out.Reset(Array->Num());
		for (int32 InstanceIndex = 0; InstanceIndex < Array->Num(); ++InstanceIndex)
		{
			const FString InstancePath = Index(InstancesPath, InstanceIndex);
			const TArray<TSharedPtr<FJsonValue>>* Numbers = nullptr;
			if (!(*Array)[InstanceIndex].IsValid() || !(*Array)[InstanceIndex]->TryGetArray(Numbers))
			{
				Ctx.Fail(InstancePath, TEXT("must be an array of 9 numbers"));
				return;
			}
			if (Numbers->Num() != 9)
			{
				Ctx.Fail(InstancePath, FString::Printf(
					TEXT("expected 9 numbers [x, y, z, pitch, yaw, roll, sx, sy, sz], got %d"), Numbers->Num()));
				return;
			}

			double Values[9] = {};
			for (int32 Component = 0; Component < 9; ++Component)
			{
				const TSharedPtr<FJsonValue>& Value = (*Numbers)[Component];
				if (!Value.IsValid() || Value->Type != EJson::Number || !Value->TryGetNumber(Values[Component]))
				{
					Ctx.Fail(Index(InstancePath, Component), TEXT("expected a number"));
					return;
				}
				if (Component >= 6 && Values[Component] <= 0.0)
				{
					Ctx.Fail(Index(InstancePath, Component), FString::Printf(TEXT("%g must be greater than zero"), Values[Component]));
					return;
				}
			}

			Out.Add(FTransform(
				FRotator(Values[3], Values[4], Values[5]),
				FVector(Values[0], Values[1], Values[2]),
				FVector(Values[6], Values[7], Values[8])));
		}
	}

	/**
	 * A Terrain's grass: the schema's array plus the rules it can't state, each reported at the
	 * path the backend and validate.py use: scale in order, a cull distance whose end is above its start
	 * (so above 0), and at most FMantlePlaceLimits::MaxTerrainGrassMaps distinct maps, reported at the
	 * map of the first entry that makes one more. That a map is a Texture is checked against the
	 * manifest once the place is whole (after the entities). An optional key left out keeps
	 * FMantleTerrainGrass' default, the schema's.
	 */
	void ParseTerrainGrass(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Props, const FString& Path, TArray<FMantleTerrainGrass>& Out)
	{
		const FString GrassPath = Join(Path, TEXT("grass"));
		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!Props->TryGetArrayField(TEXT("grass"), Entries))
		{
			Ctx.Fail(GrassPath, TEXT("expected an array"));
			return;
		}
		if (Entries->Num() > FMantlePlaceLimits::MaxTerrainGrass)
		{
			Ctx.Fail(GrassPath, FString::Printf(TEXT("has %d entries; a Terrain holds at most %d"), Entries->Num(), FMantlePlaceLimits::MaxTerrainGrass));
		}

		TArray<FString> Maps;
		for (int32 EntryIndex = 0; EntryIndex < Entries->Num(); ++EntryIndex)
		{
			const FString EntryPath = Index(GrassPath, EntryIndex);
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!(*Entries)[EntryIndex].IsValid() || !(*Entries)[EntryIndex]->TryGetObject(Entry))
			{
				Ctx.Fail(EntryPath, TEXT("expected an object"));
				continue;
			}
			RequireNoUnknownKeys(
				Ctx, *Entry,
				{ TEXT("map"), TEXT("channel"), TEXT("mesh"), TEXT("materials"), TEXT("density"), TEXT("scale"), TEXT("alignToSurface"),
					TEXT("randomYaw"), TEXT("cullDistance"), TEXT("castShadow") },
				EntryPath);

			FMantleTerrainGrass Parsed;
			if (RequireString(Ctx, *Entry, TEXT("map"), EntryPath, Parsed.Map))
			{
				if (Parsed.Map.IsEmpty())
				{
					Ctx.Fail(Join(EntryPath, TEXT("map")), TEXT("names no texture"));
				}
				else
				{
					// Ids are case-sensitive, and a TSet of FString compares them case-insensitively.
					bool bSeen = false;
					for (const FString& Seen : Maps)
					{
						bSeen = bSeen || Seen.Equals(Parsed.Map, ESearchCase::CaseSensitive);
					}
					if (!bSeen)
					{
						Maps.Add(Parsed.Map);
						if (Maps.Num() == FMantlePlaceLimits::MaxTerrainGrassMaps + 1)
						{
							Ctx.Fail(Join(EntryPath, TEXT("map")),
								FString::Printf(TEXT("is distinct map number %d; a Terrain's grass may use at most %d"), Maps.Num(),
									FMantlePlaceLimits::MaxTerrainGrassMaps));
						}
					}
				}
			}

			FString Channel;
			if (RequireString(Ctx, *Entry, TEXT("channel"), EntryPath, Channel))
			{
				if (Channel.Equals(TEXT("r"), ESearchCase::CaseSensitive)) { Parsed.Channel = 0; }
				else if (Channel.Equals(TEXT("g"), ESearchCase::CaseSensitive)) { Parsed.Channel = 1; }
				else if (Channel.Equals(TEXT("b"), ESearchCase::CaseSensitive)) { Parsed.Channel = 2; }
				else if (Channel.Equals(TEXT("a"), ESearchCase::CaseSensitive)) { Parsed.Channel = 3; }
				else
				{
					Ctx.Fail(Join(EntryPath, TEXT("channel")), FString::Printf(TEXT("'%s' is not r, g, b or a"), *Channel));
				}
			}

			if (RequireString(Ctx, *Entry, TEXT("mesh"), EntryPath, Parsed.Mesh) && Parsed.Mesh.IsEmpty())
			{
				Ctx.Fail(Join(EntryPath, TEXT("mesh")), TEXT("names no mesh"));
			}
			ReadMaterialSlots(Ctx, *Entry, EntryPath, Parsed.Materials);
			if (Parsed.Materials.Num() > FMantlePlaceLimits::MaxMaterialSlots)
			{
				Ctx.Fail(Join(EntryPath, TEXT("materials")), FString::Printf(TEXT("has more than %d entries"), FMantlePlaceLimits::MaxMaterialSlots));
			}

			if (RequireNumber(Ctx, *Entry, TEXT("density"), EntryPath, Parsed.Density)
				&& !(Parsed.Density > 0.0 && Parsed.Density <= FMantlePlaceLimits::MaxTerrainGrassDensity))
			{
				Ctx.Fail(Join(EntryPath, TEXT("density")), FString::Printf(TEXT("%g is outside the permitted range (0, %g]"),
					Parsed.Density, FMantlePlaceLimits::MaxTerrainGrassDensity));
			}

			// A pair out of order is refused by ReadRangePair; the cull distance's end must also pass its start.
			ReadRangePair(Ctx, *Entry, TEXT("scale"), EntryPath, FMantlePlaceLimits::MinScatterScale, FMantlePlaceLimits::MaxScatterScale,
				/*bIntegers*/ false, Parsed.MinScale, Parsed.MaxScale);
			if (ReadRangePair(Ctx, *Entry, TEXT("cullDistance"), EntryPath, 0.0, FMantlePlaceLimits::MaxTerrainGrassCullDistance,
				/*bIntegers*/ false, Parsed.CullStart, Parsed.CullEnd) && (*Entry)->HasField(TEXT("cullDistance"))
				&& !(Parsed.CullStart < Parsed.CullEnd))
			{
				Ctx.Fail(Join(EntryPath, TEXT("cullDistance")), FString::Printf(TEXT("end %g must be above start %g"), Parsed.CullEnd, Parsed.CullStart));
			}

			OptionalBool(*Entry, TEXT("alignToSurface"), Parsed.bAlignToSurface);
			OptionalBool(*Entry, TEXT("randomYaw"), Parsed.bRandomYaw);
			OptionalBool(*Entry, TEXT("castShadow"), Parsed.bCastShadow);
			Out.Add(MoveTemp(Parsed));
		}
	}

	/**
	 * A Terrain's properties: the schema's branch plus the rules it can't state, a weightmap
	 * whenever there is more than one layer and a weightmap2 exactly when there are more than
	 * four, reported at the paths the backend and validate.py use.
	 */
	void ParseTerrain(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Props, const FString& Path, FMantleTerrainProps& P)
	{
		RequireNoUnknownKeys(Ctx, Props, { TEXT("heightfield"), TEXT("weightmap"), TEXT("weightmap2"), TEXT("layers"), TEXT("castShadow"), TEXT("grass") }, Path);

		// An empty id could only fail later, at build, on every machine.
		if (RequireString(Ctx, Props, TEXT("heightfield"), Path, P.Heightfield) && P.Heightfield.IsEmpty())
		{
			Ctx.Fail(Join(Path, TEXT("heightfield")), TEXT("names no heightfield"));
		}
		const bool bHasWeightmap = Props->HasField(TEXT("weightmap"));
		if (bHasWeightmap && RequireString(Ctx, Props, TEXT("weightmap"), Path, P.Weightmap) && P.Weightmap.IsEmpty())
		{
			Ctx.Fail(Join(Path, TEXT("weightmap")), TEXT("names no texture"));
		}
		const bool bHasWeightmap2 = Props->HasField(TEXT("weightmap2"));
		if (bHasWeightmap2 && RequireString(Ctx, Props, TEXT("weightmap2"), Path, P.Weightmap2) && P.Weightmap2.IsEmpty())
		{
			Ctx.Fail(Join(Path, TEXT("weightmap2")), TEXT("names no texture"));
		}

		const FString LayersPath = Join(Path, TEXT("layers"));
		const TArray<TSharedPtr<FJsonValue>>* Layers = nullptr;
		if (!Props->TryGetArrayField(TEXT("layers"), Layers))
		{
			Ctx.Fail(LayersPath, TEXT("required array is missing or not an array"));
		}
		else
		{
			if (Layers->Num() < 1 || Layers->Num() > FMantlePlaceLimits::MaxTerrainLayers)
			{
				Ctx.Fail(LayersPath, FString::Printf(TEXT("has %d layers; a Terrain needs 1 to %d"), Layers->Num(), FMantlePlaceLimits::MaxTerrainLayers));
			}
			if (Layers->Num() > 1 && !bHasWeightmap)
			{
				Ctx.Fail(Join(Path, TEXT("weightmap")), TEXT("is required when a Terrain has more than one layer"));
			}
			if (Layers->Num() > 4 && !bHasWeightmap2)
			{
				Ctx.Fail(Join(Path, TEXT("weightmap2")), TEXT("is required when a Terrain has more than 4 layers"));
			}
			if (Layers->Num() <= 4 && bHasWeightmap2)
			{
				Ctx.Fail(Join(Path, TEXT("weightmap2")), FString::Printf(TEXT("is only for layers 4 to 7; this Terrain has %d layers"), Layers->Num()));
			}
			for (int32 LayerIndex = 0; LayerIndex < Layers->Num(); ++LayerIndex)
			{
				const FString LayerPath = Index(LayersPath, LayerIndex);
				const TSharedPtr<FJsonObject>* Layer = nullptr;
				if (!(*Layers)[LayerIndex].IsValid() || !(*Layers)[LayerIndex]->TryGetObject(Layer))
				{
					Ctx.Fail(LayerPath, TEXT("expected an object"));
					continue;
				}
				RequireNoUnknownKeys(Ctx, *Layer, { TEXT("material"), TEXT("tiling"), TEXT("surface") }, LayerPath);
				FMantleTerrainLayer Parsed;
				if (RequireString(Ctx, *Layer, TEXT("material"), LayerPath, Parsed.Material) && Parsed.Material.IsEmpty())
				{
					Ctx.Fail(Join(LayerPath, TEXT("material")), TEXT("names no material"));
				}
				if ((*Layer)->HasField(TEXT("tiling")) && RequireNumber(Ctx, *Layer, TEXT("tiling"), LayerPath, Parsed.Tiling)
					&& (Parsed.Tiling <= 0.0 || Parsed.Tiling > FMantlePlaceLimits::MaxTerrainTilingCm))
				{
					Ctx.Fail(Join(LayerPath, TEXT("tiling")), FString::Printf(TEXT("%g is outside the permitted range (0, %g]"),
						Parsed.Tiling, FMantlePlaceLimits::MaxTerrainTilingCm));
				}
				ReadSurface(Ctx, *Layer, LayerPath, Parsed.Surface);
				P.Layers.Add(MoveTemp(Parsed));
			}
		}
		OptionalBool(Props, TEXT("castShadow"), P.bCastShadow);
		if (Props->HasField(TEXT("grass")))
		{
			ParseTerrainGrass(Ctx, Props, Path, P.Grass);
		}
	}

	/** The Effect keys only the sprites archetype takes, the schema's order. */
	const TCHAR* const EffectSpritesKeys[] = {
		TEXT("texture"), TEXT("blend"), TEXT("colorEnd"), TEXT("alpha"), TEXT("alphaEnd"), TEXT("fadeIn"), TEXT("sizeMin"), TEXT("sizeMax"),
		TEXT("sizeEndScale"), TEXT("lifetimeMin"), TEXT("lifetimeMax"), TEXT("velocityMin"), TEXT("velocityMax"), TEXT("acceleration"),
		TEXT("shape"), TEXT("maxParticles"), TEXT("subImages"), TEXT("subImageMode")
	};

	/** An optional number in [Min, Max] that records whether it was there. */
	bool ReadEffectNumber(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Props, const TCHAR* Key, const FString& Path, double Min, double Max, double& Out)
	{
		if (!Props->HasField(Key))
		{
			return false;
		}
		const double Before = Out;
		ReadOptionalRange(Ctx, Props, Key, Path, Min, Max, Out);
		if (Out < Min || Out > Max)
		{
			Out = Before; // refused: the default stands, the failure is recorded
		}
		return true;
	}

	/** An optional three-number vector with each number in [-Limit, Limit]; true when it was there. */
	bool ReadEffectVector(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Props, const TCHAR* Key, const FString& Path, double Limit, FVector& Out)
	{
		if (!Props->HasField(Key))
		{
			return false;
		}
		TArray<double> Values;
		if (!ReadNumberArray(Ctx, Props, Key, 3, Path, Values))
		{
			return true;
		}
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			RangeCheck(Ctx, Values[Axis], -Limit, Limit, Index(Join(Path, Key), Axis));
			Out[Axis] = FMath::Clamp(Values[Axis], -Limit, Limit);
		}
		return true;
	}

	/**
	 * A sprites Effect's keys, for an Effect whose archetype parsed: the sprites keys refused on every
	 * other archetype, size, speed and lifetime refused on sprites (each at the stray key), then each sprites key
	 * held to the schema's range, the min and max pairs in order (reported at the max, velocity per axis), and
	 * a shape that carries exactly its type's keys. A value that fails keeps its default; the failure is recorded.
	 * The backend (effectSprites) and validate.py (check_effect_sprites_rules) report each at the same path.
	 */
	void ParseEffectSprites(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Props, const FString& Path, const bool bSprites, FMantleEffectProps& P)
	{
		if (!bSprites)
		{
			for (const TCHAR* Key : EffectSpritesKeys)
			{
				if (Props->HasField(Key))
				{
					Ctx.Fail(Join(Path, Key), TEXT("is only valid with the sprites archetype"));
				}
			}
			return;
		}
		for (const TCHAR* Key : { TEXT("size"), TEXT("speed"), TEXT("lifetime") })
		{
			if (Props->HasField(Key))
			{
				Ctx.Fail(Join(Path, Key),
					TEXT("is not used by the sprites archetype; use its own sizeMin and sizeMax, lifetimeMin and lifetimeMax, velocityMin and velocityMax"));
			}
		}
		FMantleSpritesSettings& S = P.Sprites;

		if (Props->HasField(TEXT("texture")) && (!Props->TryGetStringField(TEXT("texture"), P.TextureId) || P.TextureId.IsEmpty()))
		{
			Ctx.Fail(Join(Path, TEXT("texture")), TEXT("must name a texture"));
			P.TextureId.Reset();
		}
		if (Props->HasField(TEXT("blend")))
		{
			FString Blend;
			if (!Props->TryGetStringField(TEXT("blend"), Blend) || !FMantleEffects::ParseBlend(Blend, S.Blend))
			{
				Ctx.Fail(Join(Path, TEXT("blend")), TEXT("must be translucent or additive"));
			}
		}
		const bool bHasColorEnd = Props->HasField(TEXT("colorEnd"));
		if (bHasColorEnd)
		{
			ReadColor(Ctx, Props, TEXT("colorEnd"), Path, S.ColorEnd);
		}
		else
		{
			S.ColorEnd = P.Look.Color;
		}
		ReadEffectNumber(Ctx, Props, TEXT("alpha"), Path, 0.0, 1.0, S.Alpha);
		ReadEffectNumber(Ctx, Props, TEXT("alphaEnd"), Path, 0.0, 1.0, S.AlphaEnd);
		ReadEffectNumber(Ctx, Props, TEXT("fadeIn"), Path, 0.0, 1.0, S.FadeIn);
		ReadEffectNumber(Ctx, Props, TEXT("sizeEndScale"), Path, 0.0, FMantlePlaceLimits::MaxSpriteEndScale, S.SizeEndScale);

		const bool bHasSizeMin = ReadEffectNumber(Ctx, Props, TEXT("sizeMin"), Path, FMantlePlaceLimits::MinSpriteSize, FMantlePlaceLimits::MaxEffectSize, S.SizeMin);
		const bool bHasSizeMax = ReadEffectNumber(Ctx, Props, TEXT("sizeMax"), Path, FMantlePlaceLimits::MinSpriteSize, FMantlePlaceLimits::MaxEffectSize, S.SizeMax);
		FMantleEffects::ResolveSpritesPair(bHasSizeMin, bHasSizeMax, S.SizeMin, S.SizeMax);
		if (bHasSizeMin && bHasSizeMax && S.SizeMin > S.SizeMax)
		{
			Ctx.Fail(Join(Path, TEXT("sizeMax")), FString::Printf(TEXT("%g is below sizeMin %g"), S.SizeMax, S.SizeMin));
			S.SizeMax = S.SizeMin;
		}
		const bool bHasLifetimeMin = ReadEffectNumber(Ctx, Props, TEXT("lifetimeMin"), Path, FMantlePlaceLimits::MinEffectLifetime, FMantlePlaceLimits::MaxEffectLifetime, S.LifetimeMin);
		const bool bHasLifetimeMax = ReadEffectNumber(Ctx, Props, TEXT("lifetimeMax"), Path, FMantlePlaceLimits::MinEffectLifetime, FMantlePlaceLimits::MaxEffectLifetime, S.LifetimeMax);
		FMantleEffects::ResolveSpritesPair(bHasLifetimeMin, bHasLifetimeMax, S.LifetimeMin, S.LifetimeMax);
		if (bHasLifetimeMin && bHasLifetimeMax && S.LifetimeMin > S.LifetimeMax)
		{
			Ctx.Fail(Join(Path, TEXT("lifetimeMax")), FString::Printf(TEXT("%g is below lifetimeMin %g"), S.LifetimeMax, S.LifetimeMin));
			S.LifetimeMax = S.LifetimeMin;
		}

		const bool bHasVelocityMin = ReadEffectVector(Ctx, Props, TEXT("velocityMin"), Path, FMantlePlaceLimits::MaxSpriteVector, S.VelocityMin);
		const bool bHasVelocityMax = ReadEffectVector(Ctx, Props, TEXT("velocityMax"), Path, FMantlePlaceLimits::MaxSpriteVector, S.VelocityMax);
		FMantleEffects::ResolveSpritesVectorPair(bHasVelocityMin, bHasVelocityMax, S.VelocityMin, S.VelocityMax);
		if (bHasVelocityMin && bHasVelocityMax
			&& (S.VelocityMin.X > S.VelocityMax.X || S.VelocityMin.Y > S.VelocityMax.Y || S.VelocityMin.Z > S.VelocityMax.Z))
		{
			Ctx.Fail(Join(Path, TEXT("velocityMax")), TEXT("velocityMax is below velocityMin on an axis"));
			S.VelocityMax = FVector(
				FMath::Max(S.VelocityMin.X, S.VelocityMax.X), FMath::Max(S.VelocityMin.Y, S.VelocityMax.Y), FMath::Max(S.VelocityMin.Z, S.VelocityMax.Z));
		}
		ReadEffectVector(Ctx, Props, TEXT("acceleration"), Path, FMantlePlaceLimits::MaxSpriteVector, S.Acceleration);

		if (Props->HasField(TEXT("maxParticles")))
		{
			double Count = 0.0;
			if (RequireNumber(Ctx, Props, TEXT("maxParticles"), Path, Count))
			{
				ReadBoundedInt(Ctx, Count, 1, FMantlePlaceLimits::MaxSpriteParticles, Join(Path, TEXT("maxParticles")), S.MaxParticles);
			}
		}

		// A flipbook sheet: [columns, rows], each a whole number of 1 to 16, and how a particle picks its frame.
		if (Props->HasField(TEXT("subImages")))
		{
			TArray<double> Sheet;
			if (ReadNumberArray(Ctx, Props, TEXT("subImages"), 2, Path, Sheet))
			{
				const FString SheetPath = Join(Path, TEXT("subImages"));
				ReadBoundedInt(Ctx, Sheet[0], 1, FMantlePlaceLimits::MaxSpriteSubImages, Index(SheetPath, 0), S.SubImageColumns);
				ReadBoundedInt(Ctx, Sheet[1], 1, FMantlePlaceLimits::MaxSpriteSubImages, Index(SheetPath, 1), S.SubImageRows);
			}
		}
		if (Props->HasField(TEXT("subImageMode")))
		{
			FString Mode;
			const TSharedPtr<FJsonValue> ModeValue = Props->TryGetField(TEXT("subImageMode"));
			if (!ModeValue.IsValid() || ModeValue->Type != EJson::String || !Props->TryGetStringField(TEXT("subImageMode"), Mode)
				|| !FMantleEffects::ParseSubImageMode(Mode, S.SubImageMode))
			{
				Ctx.Fail(Join(Path, TEXT("subImageMode")), TEXT("must be random or overLife"));
			}
		}

		if (!Props->HasField(TEXT("shape")))
		{
			return;
		}
		const FString ShapePath = Join(Path, TEXT("shape"));
		const TSharedPtr<FJsonObject>* Shape = nullptr;
		if (!Props->TryGetObjectField(TEXT("shape"), Shape) || !Shape || !Shape->IsValid())
		{
			Ctx.Fail(ShapePath, TEXT("must be an object"));
			return;
		}
		RequireNoUnknownKeys(Ctx, *Shape, { TEXT("type"), TEXT("halfExtent"), TEXT("radius"), TEXT("height") }, ShapePath);
		FString TypeName;
		if (!RequireString(Ctx, *Shape, TEXT("type"), ShapePath, TypeName))
		{
			return;
		}
		if (!FMantleEffects::ParseShape(TypeName, S.Shape))
		{
			Ctx.Fail(Join(ShapePath, TEXT("type")), FString::Printf(TEXT("'%s' is not a spawn shape (point, box, sphere or cylinder)"), *TypeName));
			return;
		}
		const bool bBox = S.Shape == EMantleSpritesShape::Box;
		const bool bSphere = S.Shape == EMantleSpritesShape::Sphere;
		const bool bCylinder = S.Shape == EMantleSpritesShape::Cylinder;
		for (const TCHAR* Key : { TEXT("halfExtent"), TEXT("radius"), TEXT("height") })
		{
			const bool bUsed = FCString::Strcmp(Key, TEXT("halfExtent")) == 0 ? bBox
				: FCString::Strcmp(Key, TEXT("radius")) == 0 ? (bSphere || bCylinder)
				: bCylinder;
			const bool bHas = (*Shape)->HasField(Key);
			if (bHas && !bUsed)
			{
				Ctx.Fail(Join(ShapePath, Key), FString::Printf(TEXT("is not used by a %s shape"), FMantleEffects::ShapeName(S.Shape)));
			}
			else if (!bHas && bUsed)
			{
				Ctx.Fail(Join(ShapePath, Key), FString::Printf(TEXT("is required by a %s shape"), FMantleEffects::ShapeName(S.Shape)));
			}
		}
		if (bBox && (*Shape)->HasField(TEXT("halfExtent")))
		{
			ReadEffectVector(Ctx, *Shape, TEXT("halfExtent"), ShapePath, FMantlePlaceLimits::MaxSpriteShapeLength, S.BoxHalfExtent);
			// The vector reader allows negatives; a half extent starts at 0.
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				if (S.BoxHalfExtent[Axis] < 0.0)
				{
					Ctx.Fail(Index(Join(ShapePath, TEXT("halfExtent")), Axis), FString::Printf(TEXT("%g is outside the permitted range [0, %g]"),
						S.BoxHalfExtent[Axis], FMantlePlaceLimits::MaxSpriteShapeLength));
					S.BoxHalfExtent[Axis] = 0.0;
				}
			}
		}
		if (bSphere)
		{
			ReadEffectNumber(Ctx, *Shape, TEXT("radius"), ShapePath, 0.0, FMantlePlaceLimits::MaxSpriteShapeLength, S.SphereRadius);
		}
		if (bCylinder)
		{
			ReadEffectNumber(Ctx, *Shape, TEXT("radius"), ShapePath, 0.0, FMantlePlaceLimits::MaxSpriteShapeLength, S.CylinderRadius);
			ReadEffectNumber(Ctx, *Shape, TEXT("height"), ShapePath, 0.0, FMantlePlaceLimits::MaxSpriteShapeLength, S.CylinderHeight);
		}
	}

	/**
	 * A Scatter's properties: the schema's branch plus the rules it can't state (a scale range in
	 * order, at least one mesh that weighs more than 0, and the instance count its box and density make),
	 * each reported at the path the backend and validate.py use. An optional key left out keeps
	 * FMantleScatterParams' default, the schema's.
	 */
	void ParseScatter(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Props, const FString& Path, FMantleScatterParams& P)
	{
		RequireNoUnknownKeys(
			Ctx, Props,
			{ TEXT("seed"), TEXT("halfExtent"), TEXT("density"), TEXT("scale"), TEXT("alignment"), TEXT("maxTilt"), TEXT("meshes") },
			Path);
		const int32 ErrorsBefore = Ctx.Errors.Num();

		if (Props->HasField(TEXT("seed")))
		{
			double Seed = 0.0;
			if (RequireNumber(Ctx, Props, TEXT("seed"), Path, Seed))
			{
				ReadBoundedInt(Ctx, Seed, 0, FMantlePlaceLimits::MaxScatterSeed, Join(Path, TEXT("seed")), P.Seed);
			}
		}

		TArray<double> Half;
		if (ReadNumberArray(Ctx, Props, TEXT("halfExtent"), 3, Path, Half))
		{
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				// X and Y above 0, Z (a flat layer's) 0 or more.
				const bool bTooSmall = Axis < 2 ? !(Half[Axis] > 0.0) : !(Half[Axis] >= 0.0);
				if (bTooSmall || Half[Axis] > FMantlePlaceLimits::MaxScatterHalfExtent)
				{
					Ctx.Fail(Index(Join(Path, TEXT("halfExtent")), Axis),
						FString::Printf(TEXT("%g is outside the permitted range %s0, %g]"), Half[Axis], Axis < 2 ? TEXT("(") : TEXT("["),
							FMantlePlaceLimits::MaxScatterHalfExtent));
				}
			}
			P.HalfExtent = FVector(Half[0], Half[1], Half[2]);
		}

		if (RequireNumber(Ctx, Props, TEXT("density"), Path, P.Density) && !(P.Density >= 0.0))
		{
			Ctx.Fail(Join(Path, TEXT("density")), FString::Printf(TEXT("%g must be 0 or more instances per square metre"), P.Density));
		}
		ReadRangePair(Ctx, Props, TEXT("scale"), Path, FMantlePlaceLimits::MinScatterScale, FMantlePlaceLimits::MaxScatterScale,
			/*bIntegers*/ false, P.MinScale, P.MaxScale);

		if (Props->HasField(TEXT("alignment")))
		{
			FString Alignment;
			if (RequireString(Ctx, Props, TEXT("alignment"), Path, Alignment) && !FMantleScatter::ParseAlignment(Alignment, P.Alignment))
			{
				Ctx.Fail(Join(Path, TEXT("alignment")), FString::Printf(TEXT("'%s' is not upright, tilted or tumbled"), *Alignment));
			}
		}
		ReadOptionalRange(Ctx, Props, TEXT("maxTilt"), Path, 0.0, FMantlePlaceLimits::MaxScatterTilt, P.MaxTiltDegrees);

		const FString MeshesPath = Join(Path, TEXT("meshes"));
		const TArray<TSharedPtr<FJsonValue>>* Meshes = nullptr;
		if (!Props->TryGetArrayField(TEXT("meshes"), Meshes))
		{
			Ctx.Fail(MeshesPath, TEXT("required array is missing or not an array"));
		}
		else
		{
			if (Meshes->Num() < 1 || Meshes->Num() > FMantlePlaceLimits::MaxScatterMeshes)
			{
				Ctx.Fail(MeshesPath, FString::Printf(TEXT("has %d meshes; a Scatter takes 1 to %d"), Meshes->Num(), FMantlePlaceLimits::MaxScatterMeshes));
			}
			int32 TotalWeight = 0;
			for (int32 MeshIndex = 0; MeshIndex < Meshes->Num(); ++MeshIndex)
			{
				const FString MeshPath = Index(MeshesPath, MeshIndex);
				const TSharedPtr<FJsonObject>* Mesh = nullptr;
				if (!(*Meshes)[MeshIndex].IsValid() || !(*Meshes)[MeshIndex]->TryGetObject(Mesh))
				{
					Ctx.Fail(MeshPath, TEXT("expected an object"));
					continue;
				}
				RequireNoUnknownKeys(Ctx, *Mesh, { TEXT("mesh"), TEXT("weight"), TEXT("materials") }, MeshPath);
				FMantleScatterMesh Parsed;
				// An empty id could only fail later, at build, on every machine.
				if (RequireString(Ctx, *Mesh, TEXT("mesh"), MeshPath, Parsed.MeshId) && Parsed.MeshId.IsEmpty())
				{
					Ctx.Fail(Join(MeshPath, TEXT("mesh")), TEXT("names no mesh"));
				}
				if ((*Mesh)->HasField(TEXT("weight")))
				{
					double Weight = 0.0;
					if (RequireNumber(Ctx, *Mesh, TEXT("weight"), MeshPath, Weight))
					{
						ReadBoundedInt(Ctx, Weight, 0, FMantlePlaceLimits::MaxScatterMeshWeight, Join(MeshPath, TEXT("weight")), Parsed.Weight);
					}
				}
				ReadMaterialSlots(Ctx, *Mesh, MeshPath, Parsed.Materials);
				if (Parsed.Materials.Num() > FMantlePlaceLimits::MaxMaterialSlots)
				{
					Ctx.Fail(Join(MeshPath, TEXT("materials")), FString::Printf(TEXT("has more than %d entries"), FMantlePlaceLimits::MaxMaterialSlots));
				}
				TotalWeight += FMath::Max(Parsed.Weight, 0);
				P.Meshes.Add(MoveTemp(Parsed));
			}
			if (Meshes->Num() > 0 && TotalWeight == 0)
			{
				Ctx.Fail(MeshesPath, TEXT("every mesh weighs 0; at least one must weigh more"));
			}
		}

		// The instance count its box and density make, once everything it depends on read cleanly.
		FString Why;
		if (Ctx.Errors.Num() == ErrorsBefore && FMantleScatter::CountFor(P, Why) < 0)
		{
			Ctx.Fail(Join(Path, TEXT("density")), Why);
		}
	}

	/** An optional three-number key (a Constraint's anchors and axis); false when absent or refused. */
	bool ReadOptionalVector(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Props, const TCHAR* Key, const FString& Path, FVector& Out)
	{
		TArray<double> Values;
		if (!Props->HasField(Key) || !ReadNumberArray(Ctx, Props, Key, 3, Path, Values))
		{
			return false;
		}
		Out = FVector(Values[0], Values[1], Values[2]);
		return true;
	}

	/**
	 * A Constraint's properties: the schema's branch, including its if/else (axis and limits
	 * are a hinge's alone, length a rope's), reported at the paths the backend and validate.py use.
	 * What entityA and entityB name is checked once every entity is read.
	 */
	void ParseConstraint(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Props, const FString& Path, FMantleConstraintProps& P)
	{
		RequireNoUnknownKeys(
			Ctx, Props,
			{ TEXT("type"), TEXT("entityA"), TEXT("entityB"), TEXT("anchorA"), TEXT("anchorB"), TEXT("axis"), TEXT("limits"),
			  TEXT("length"), TEXT("breakForce") },
			Path);

		FString Type;
		const bool bHasType = Props->HasField(TEXT("type"));
		if (RequireString(Ctx, Props, TEXT("type"), Path, Type) && !MantleParseConstraintType(Type, P.Type))
		{
			Ctx.Fail(Join(Path, TEXT("type")), FString::Printf(TEXT("'%s' is not a constraint type (weld, hinge, rope)"), *Type));
		}
		RequireString(Ctx, Props, TEXT("entityA"), Path, P.EntityA);
		if (Props->HasField(TEXT("entityB")) && !Props->TryGetStringField(TEXT("entityB"), P.EntityB))
		{
			Ctx.Fail(Join(Path, TEXT("entityB")), TEXT("must be a string"));
		}
		ReadOptionalVector(Ctx, Props, TEXT("anchorA"), Path, P.AnchorA);
		FVector AnchorB = FVector::ZeroVector;
		if (ReadOptionalVector(Ctx, Props, TEXT("anchorB"), Path, AnchorB))
		{
			P.AnchorB = AnchorB;
		}

		// The schema's if/else: another type refuses a hinge's or a rope's keys. A missing type is
		// reported above and restricts nothing, as in the schema.
		auto OnlyFor = [&Ctx, &Props, &Path, &Type, bHasType](const TCHAR* Key, const TCHAR* Owner)
		{
			if (!Props->HasField(Key))
			{
				return false;
			}
			if (bHasType && !Type.Equals(Owner, ESearchCase::CaseSensitive))
			{
				Ctx.Fail(Join(Path, Key), FString::Printf(TEXT("is only for a %s"), Owner));
				return false;
			}
			return true;
		};
		if (OnlyFor(TEXT("axis"), TEXT("hinge")) && ReadOptionalVector(Ctx, Props, TEXT("axis"), Path, P.Axis) && P.Axis.IsZero())
		{
			Ctx.Fail(Join(Path, TEXT("axis")), TEXT("must not be [0, 0, 0]; give the direction the hinge turns about"));
		}
		TArray<double> Limits;
		if (OnlyFor(TEXT("limits"), TEXT("hinge")) && ReadNumberArray(Ctx, Props, TEXT("limits"), 2, Path, Limits))
		{
			const FString LimitsPath = Join(Path, TEXT("limits"));
			const int32 ErrorsBefore = Ctx.Errors.Num();
			RangeCheck(Ctx, Limits[0], -FMantlePlaceLimits::MaxHingeAngle, 0.0, Index(LimitsPath, 0));
			RangeCheck(Ctx, Limits[1], 0.0, FMantlePlaceLimits::MaxHingeAngle, Index(LimitsPath, 1));
			if (Ctx.Errors.Num() == ErrorsBefore)
			{
				P.bLimited = true;
				P.MinAngle = Limits[0];
				P.MaxAngle = Limits[1];
			}
		}
		double Number = 0.0;
		if (OnlyFor(TEXT("length"), TEXT("rope")) && RequireNumber(Ctx, Props, TEXT("length"), Path, Number))
		{
			if (Number <= 0.0 || Number > FMantlePlaceLimits::MaxRopeLength)
			{
				Ctx.Fail(Join(Path, TEXT("length")), FString::Printf(TEXT("%g is outside the permitted range (0, %g]"), Number, FMantlePlaceLimits::MaxRopeLength));
			}
			P.Length = Number;
		}
		if (Props->HasField(TEXT("breakForce")) && RequireNumber(Ctx, Props, TEXT("breakForce"), Path, Number))
		{
			if (Number <= 0.0 || Number > FMantlePlaceLimits::MaxConstraintBreakForce)
			{
				Ctx.Fail(Join(Path, TEXT("breakForce")),
					FString::Printf(TEXT("%g is outside the permitted range (0, %g]"), Number, FMantlePlaceLimits::MaxConstraintBreakForce));
			}
			P.BreakForce = Number;
		}
	}

	/** An optional typed number in [Min, Max] (a ReflectionProbe's): a string is not one, and a refused value leaves Out alone. */
	void ReadProbeNumber(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Props, const TCHAR* Key, const FString& Path,
		double Min, double Max, double& Out)
	{
		const TSharedPtr<FJsonValue> Value = Props->TryGetField(Key);
		if (!Value.IsValid())
		{
			return;
		}
		if (Value->Type != EJson::Number)
		{
			Ctx.Fail(Join(Path, Key), TEXT("must be a number"));
			return;
		}
		const double Number = Value->AsNumber();
		RangeCheck(Ctx, Number, Min, Max, Join(Path, Key));
		if (Number >= Min && Number <= Max)
		{
			Out = Number;
		}
	}

	/**
	 * A ReflectionProbe's properties: the schema's branch, including its `not` (a box takes halfExtent and
	 * may take transitionDistance, a sphere takes influenceRadius, and a key the other shape owns is refused), each
	 * reported at the key as the backend does. A key left out keeps FMantleReflectionProbeProps' default, the schema's.
	 */
	void ParseReflectionProbe(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Props, const FString& Path, FMantleReflectionProbeProps& P)
	{
		RequireNoUnknownKeys(
			Ctx, Props,
			{ TEXT("shape"), TEXT("halfExtent"), TEXT("influenceRadius"), TEXT("transitionDistance"), TEXT("brightness"), TEXT("skylightScale") },
			Path);

		FString Shape;
		if (RequireString(Ctx, Props, TEXT("shape"), Path, Shape))
		{
			if (!MantleParseReflectionProbeShape(Shape, P.Shape))
			{
				Ctx.Fail(Join(Path, TEXT("shape")), FString::Printf(TEXT("'%s' is not a probe shape (box or sphere)"), *Shape));
			}
			else
			{
				const bool bBox = P.Shape == EMantleReflectionProbeShape::Box;
				const TCHAR* const Name = MantleReflectionProbeShapeToString(P.Shape);
				const TCHAR* const Needed = bBox ? TEXT("halfExtent") : TEXT("influenceRadius");
				if (!Props->HasField(Needed))
				{
					Ctx.Fail(Join(Path, Needed), FString::Printf(TEXT("is required by a %s shape"), Name));
				}
				for (const TCHAR* Key : { TEXT("influenceRadius"), TEXT("halfExtent"), TEXT("transitionDistance") })
				{
					// The radius is a sphere's; the half extent and the transition distance are a box's.
					const bool bUsed = FCString::Strcmp(Key, TEXT("influenceRadius")) == 0 ? !bBox : bBox;
					if (Props->HasField(Key) && !bUsed)
					{
						Ctx.Fail(Join(Path, Key), FString::Printf(TEXT("is not used by a %s shape"), Name));
					}
				}
			}
		}

		if (Props->HasField(TEXT("halfExtent")))
		{
			TArray<double> Half;
			if (ReadNumberArray(Ctx, Props, TEXT("halfExtent"), 3, Path, Half))
			{
				bool bInRange = true;
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					const int32 ErrorsBefore = Ctx.Errors.Num();
					RangeCheck(Ctx, Half[Axis], FMantlePlaceLimits::MinReflectionProbeExtent, FMantlePlaceLimits::MaxReflectionProbeExtent,
						Index(Join(Path, TEXT("halfExtent")), Axis));
					bInRange = bInRange && Ctx.Errors.Num() == ErrorsBefore;
				}
				if (bInRange)
				{
					P.HalfExtent = FVector(Half[0], Half[1], Half[2]);
				}
			}
		}
		ReadProbeNumber(Ctx, Props, TEXT("influenceRadius"), Path,
			FMantlePlaceLimits::MinReflectionProbeRadius, FMantlePlaceLimits::MaxReflectionProbeRadius, P.InfluenceRadius);
		ReadProbeNumber(Ctx, Props, TEXT("transitionDistance"), Path,
			FMantlePlaceLimits::MinReflectionProbeTransition, FMantlePlaceLimits::MaxReflectionProbeTransition, P.TransitionDistance);
		ReadProbeNumber(Ctx, Props, TEXT("brightness"), Path, 0.0, FMantlePlaceLimits::MaxReflectionProbeBrightness, P.Brightness);
		ReadProbeNumber(Ctx, Props, TEXT("skylightScale"), Path, 0.0, 1.0, P.SkylightScale);
	}

	/**
	 * A Text entity's properties (GhzGarage/mantle#724): only `text` is required; a key left out keeps
	 * FMantleTextProps' default. The colour is linear RGBA.
	 */
	void ParseText(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Props, const FString& Path, FMantleTextProps& P)
	{
		RequireNoUnknownKeys(Ctx, Props,
			{ TEXT("text"), TEXT("color"), TEXT("worldSize"), TEXT("horizontalAlignment"), TEXT("verticalAlignment"), TEXT("xScale"), TEXT("yScale") },
			Path);
		if (RequireString(Ctx, Props, TEXT("text"), Path, P.Text))
		{
			const int32 Length = FMantlePlaceParser::CodePointLength(P.Text);
			if (Length < 1 || Length > FMantlePlaceLimits::MaxTextLength)
			{
				Ctx.Fail(Join(Path, TEXT("text")),
					FString::Printf(TEXT("is %d characters; it must be 1 to %d"), Length, FMantlePlaceLimits::MaxTextLength));
			}
		}
		if (Props->HasField(TEXT("color")))
		{
			TArray<double> Values;
			if (ReadNumberArray(Ctx, Props, TEXT("color"), 4, Path, Values))
			{
				for (int32 Channel = 0; Channel < 4; ++Channel)
				{
					RangeCheck(Ctx, Values[Channel], 0.0, 1.0, Index(Join(Path, TEXT("color")), Channel));
				}
				P.Color = FLinearColor(static_cast<float>(Values[0]), static_cast<float>(Values[1]), static_cast<float>(Values[2]), static_cast<float>(Values[3]));
			}
		}
		ReadProbeNumber(Ctx, Props, TEXT("worldSize"), Path, FMantlePlaceLimits::MinTextWorldSize, FMantlePlaceLimits::MaxTextWorldSize, P.WorldSize);
		ReadProbeNumber(Ctx, Props, TEXT("xScale"), Path, FMantlePlaceLimits::MinTextScale, FMantlePlaceLimits::MaxTextScale, P.XScale);
		ReadProbeNumber(Ctx, Props, TEXT("yScale"), Path, FMantlePlaceLimits::MinTextScale, FMantlePlaceLimits::MaxTextScale, P.YScale);
		FString Alignment;
		if (Props->TryGetStringField(TEXT("horizontalAlignment"), Alignment) && !MantleParseTextHorizontalAlignment(Alignment, P.HorizontalAlignment))
		{
			Ctx.Fail(Join(Path, TEXT("horizontalAlignment")), FString::Printf(TEXT("'%s' is not left, center or right"), *Alignment));
		}
		if (Props->TryGetStringField(TEXT("verticalAlignment"), Alignment) && !MantleParseTextVerticalAlignment(Alignment, P.VerticalAlignment))
		{
			Ctx.Fail(Join(Path, TEXT("verticalAlignment")), FString::Printf(TEXT("'%s' is not top, center, bottom or baseline"), *Alignment));
		}
	}


	/** An optional [x, y] extent, each axis in [FMantlePlaceLimits::MinWaterExtent, MaxWaterExtent] (a WaterBody's oceanExtent, the Water Zone's zoneExtent). */
	void ReadWaterExtent2(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FString& Path, TOptional<FVector2D>& Out)
	{
		if (!Obj->HasField(Key))
		{
			return;
		}
		TArray<double> Values;
		if (!ReadNumberArray(Ctx, Obj, Key, 2, Path, Values))
		{
			return;
		}
		const int32 ErrorsBefore = Ctx.Errors.Num();
		for (int32 Axis = 0; Axis < 2; ++Axis)
		{
			RangeCheck(Ctx, Values[Axis], FMantlePlaceLimits::MinWaterExtent, FMantlePlaceLimits::MaxWaterExtent, Index(Join(Path, Key), Axis));
		}
		if (Ctx.Errors.Num() == ErrorsBefore)
		{
			Out = FVector2D(Values[0], Values[1]);
		}
	}

	/** A required [x, y, z] of any numbers (a spline point's location or tangent). */
	void ReadWaterVector(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FString& Path, FVector& Out)
	{
		TArray<double> Values;
		if (ReadNumberArray(Ctx, Obj, Key, 3, Path, Values))
		{
			Out = FVector(Values[0], Values[1], Values[2]);
		}
	}

	/** An optional linear RGB colour (a WaterBody's look): a colour left out is left unset. */
	void ReadWaterLookColor(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Look, const TCHAR* Key, const FString& Path, TOptional<FLinearColor>& Out)
	{
		if (!Look->HasField(Key))
		{
			return;
		}
		FLinearColor Color;
		if (ReadColor(Ctx, Look, Key, Path, Color))
		{
			Out = Color;
		}
	}

	/** One spline point of a WaterBody: width and velocity are a river's, and a river's width is required. */
	void ParseWaterSplinePoint(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Point, const FString& Path, bool bTypeKnown,
		EMantleWaterBodyType Type, FMantleWaterSplinePoint& P)
	{
		RequireNoUnknownKeys(Ctx, Point,
			{ TEXT("location"), TEXT("arriveTangent"), TEXT("leaveTangent"), TEXT("depth"), TEXT("width"), TEXT("velocity"), TEXT("interp") },
			Path);
		ReadWaterVector(Ctx, Point, TEXT("location"), Path, P.Location);
		ReadWaterVector(Ctx, Point, TEXT("arriveTangent"), Path, P.ArriveTangent);
		ReadWaterVector(Ctx, Point, TEXT("leaveTangent"), Path, P.LeaveTangent);
		if (RequireNumber(Ctx, Point, TEXT("depth"), Path, P.Depth))
		{
			RangeCheck(Ctx, P.Depth, FMantlePlaceLimits::MinWaterSplineDepth, FMantlePlaceLimits::MaxWaterSplineDepth, Join(Path, TEXT("depth")));
		}
		const bool bRiver = bTypeKnown && Type == EMantleWaterBodyType::River;
		if (bTypeKnown && !bRiver)
		{
			for (const TCHAR* Key : { TEXT("width"), TEXT("velocity") })
			{
				if (Point->HasField(Key))
				{
					Ctx.Fail(Join(Path, Key), FString::Printf(TEXT("is not used by a %s body"), MantleWaterBodyTypeToString(Type)));
				}
			}
		}
		else
		{
			if (bRiver)
			{
				if (!Point->HasField(TEXT("width")))
				{
					Ctx.Fail(Join(Path, TEXT("width")), TEXT("is required by a river body"));
				}
			}
			ReadProbeNumber(Ctx, Point, TEXT("width"), Path, FMantlePlaceLimits::MinWaterRiverWidth, FMantlePlaceLimits::MaxWaterRiverWidth, P.Width);
			ReadProbeNumber(Ctx, Point, TEXT("velocity"), Path, 0.0, FMantlePlaceLimits::MaxWaterRiverVelocity, P.Velocity);
		}
		FString Interp;
		if (Point->HasField(TEXT("interp")))
		{
			if (!Point->TryGetStringField(TEXT("interp"), Interp) || !MantleParseWaterSplinePointType(Interp, P.Interp))
			{
				Ctx.Fail(Join(Path, TEXT("interp")), TEXT("is not curve, linear, constant, curveClamped or curveCustomTangent"));
			}
		}
	}

	/**
	 * A WaterBody's properties (GhzGarage/mantle#997): the schema's branch and its `if`/`not`, each failure reported at the
	 * key as the backend does. bodyType decides which keys apply: lake, river and ocean take a spline and refuse a mesh,
	 * custom takes a mesh and refuses a spline; width and velocity are a river point's; waves are never a river's;
	 * collisionExtent, oceanExtent and centerOnZone are an ocean's. A key left out keeps FMantleWaterBodyProps' default.
	 */
	void ParseWaterBody(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Props, const FString& Path, FMantleWaterBodyProps& P)
	{
		RequireNoUnknownKeys(Ctx, Props,
			{ TEXT("bodyType"), TEXT("spline"), TEXT("mesh"), TEXT("waves"), TEXT("look"), TEXT("collision"),
			  TEXT("collisionHeightOffset"), TEXT("collisionExtent"), TEXT("oceanExtent"), TEXT("centerOnZone") },
			Path);

		bool bTypeKnown = false;
		FString BodyType;
		if (RequireString(Ctx, Props, TEXT("bodyType"), Path, BodyType))
		{
			bTypeKnown = MantleParseWaterBodyType(BodyType, P.BodyType);
			if (!bTypeKnown)
			{
				Ctx.Fail(Join(Path, TEXT("bodyType")), FString::Printf(TEXT("'%s' is not a water body type (lake, river, ocean or custom)"), *BodyType));
			}
		}
		if (bTypeKnown)
		{
			const bool bCustom = P.BodyType == EMantleWaterBodyType::Custom;
			const TCHAR* const Name = MantleWaterBodyTypeToString(P.BodyType);
			if (!Props->HasField(bCustom ? TEXT("mesh") : TEXT("spline")))
			{
				Ctx.Fail(Join(Path, bCustom ? TEXT("mesh") : TEXT("spline")), FString::Printf(TEXT("is required by a %s body"), Name));
			}
			if (Props->HasField(bCustom ? TEXT("spline") : TEXT("mesh")))
			{
				Ctx.Fail(Join(Path, bCustom ? TEXT("spline") : TEXT("mesh")), FString::Printf(TEXT("is not used by a %s body"), Name));
			}
			if (P.BodyType == EMantleWaterBodyType::River && Props->HasField(TEXT("waves")))
			{
				Ctx.Fail(Join(Path, TEXT("waves")), TEXT("is not used by a river body"));
			}
			if (P.BodyType != EMantleWaterBodyType::Ocean)
			{
				for (const TCHAR* Key : { TEXT("collisionExtent"), TEXT("oceanExtent"), TEXT("centerOnZone") })
				{
					if (Props->HasField(Key))
					{
						Ctx.Fail(Join(Path, Key), FString::Printf(TEXT("is not used by a %s body"), Name));
					}
				}
			}
		}

		if (Props->HasField(TEXT("spline")))
		{
			const FString SplinePath = Join(Path, TEXT("spline"));
			const TSharedPtr<FJsonObject>* Spline = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
			if (!Props->TryGetObjectField(TEXT("spline"), Spline))
			{
				Ctx.Fail(SplinePath, TEXT("must be an object"));
			}
			else
			{
				RequireNoUnknownKeys(Ctx, *Spline, { TEXT("points") }, SplinePath);
				const FString PointsPath = Join(SplinePath, TEXT("points"));
				if (!(*Spline)->TryGetArrayField(TEXT("points"), Points))
				{
					Ctx.Fail(PointsPath, TEXT("required array is missing or not an array"));
				}
				else
				{
					const bool bLoop = bTypeKnown && (P.BodyType == EMantleWaterBodyType::Lake || P.BodyType == EMantleWaterBodyType::Ocean);
					const int32 MinPoints = bLoop ? FMantlePlaceLimits::MinWaterLoopPoints : FMantlePlaceLimits::MinWaterRiverPoints;
					if (Points->Num() < MinPoints)
					{
						Ctx.Fail(PointsPath, FString::Printf(TEXT("has %d points; a %s needs at least %d"), Points->Num(),
							bTypeKnown ? *(FString(MantleWaterBodyTypeToString(P.BodyType)) + TEXT(" spline")) : TEXT("spline"), MinPoints));
					}
					if (Points->Num() > FMantlePlaceLimits::MaxWaterSplinePointsPerBody)
					{
						Ctx.Fail(PointsPath, FString::Printf(TEXT("has %d points; a body may have at most %d"), Points->Num(),
							FMantlePlaceLimits::MaxWaterSplinePointsPerBody));
					}
					for (int32 i = 0; i < Points->Num(); ++i)
					{
						const TSharedPtr<FJsonObject>* Point = nullptr;
						if (!(*Points)[i]->TryGetObject(Point))
						{
							Ctx.Fail(Index(PointsPath, i), TEXT("must be an object"));
							continue;
						}
						FMantleWaterSplinePoint Parsed;
						ParseWaterSplinePoint(Ctx, *Point, Index(PointsPath, i), bTypeKnown, P.BodyType, Parsed);
						P.SplinePoints.Add(Parsed);
					}
				}
			}
		}

		if (Props->HasField(TEXT("mesh")))
		{
			if (RequireString(Ctx, Props, TEXT("mesh"), Path, P.MeshId) && P.MeshId.IsEmpty())
			{
				Ctx.Fail(Join(Path, TEXT("mesh")), TEXT("must name an asset"));
			}
		}

		if (Props->HasField(TEXT("waves")))
		{
			const FString WavesPath = Join(Path, TEXT("waves"));
			const TSharedPtr<FJsonObject>* Waves = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
			if (!Props->TryGetObjectField(TEXT("waves"), Waves))
			{
				Ctx.Fail(WavesPath, TEXT("must be an object"));
			}
			else
			{
				RequireNoUnknownKeys(Ctx, *Waves, { TEXT("list") }, WavesPath);
				const FString ListPath = Join(WavesPath, TEXT("list"));
				if (!(*Waves)->TryGetArrayField(TEXT("list"), List))
				{
					Ctx.Fail(ListPath, TEXT("required array is missing or not an array"));
				}
				else
				{
					if (List->Num() < 1 || List->Num() > FMantlePlaceLimits::MaxWaterWavesPerBody)
					{
						Ctx.Fail(ListPath, FString::Printf(TEXT("has %d waves; expected 1 to %d"), List->Num(), FMantlePlaceLimits::MaxWaterWavesPerBody));
					}
					for (int32 i = 0; i < List->Num(); ++i)
					{
						const FString WavePath = Index(ListPath, i);
						const TSharedPtr<FJsonObject>* Wave = nullptr;
						if (!(*List)[i]->TryGetObject(Wave))
						{
							Ctx.Fail(WavePath, TEXT("must be an object"));
							continue;
						}
						RequireNoUnknownKeys(Ctx, *Wave,
							{ TEXT("wavelength"), TEXT("amplitude"), TEXT("steepness"), TEXT("directionDeg"), TEXT("phaseOffset") }, WavePath);
						FMantleWaterWave Parsed;
						if (RequireNumber(Ctx, *Wave, TEXT("wavelength"), WavePath, Parsed.Wavelength))
						{
							RangeCheck(Ctx, Parsed.Wavelength, FMantlePlaceLimits::MinWaterWavelength, FMantlePlaceLimits::MaxWaterWavelength,
								Join(WavePath, TEXT("wavelength")));
						}
						if (RequireNumber(Ctx, *Wave, TEXT("amplitude"), WavePath, Parsed.Amplitude))
						{
							RangeCheck(Ctx, Parsed.Amplitude, 0.0, FMantlePlaceLimits::MaxWaterAmplitude, Join(WavePath, TEXT("amplitude")));
						}
						if (RequireNumber(Ctx, *Wave, TEXT("steepness"), WavePath, Parsed.Steepness))
						{
							RangeCheck(Ctx, Parsed.Steepness, 0.0, 1.0, Join(WavePath, TEXT("steepness")));
						}
						if (RequireNumber(Ctx, *Wave, TEXT("directionDeg"), WavePath, Parsed.DirectionDeg))
						{
							RangeCheck(Ctx, Parsed.DirectionDeg, -180.0, 180.0, Join(WavePath, TEXT("directionDeg")));
						}
						ReadProbeNumber(Ctx, *Wave, TEXT("phaseOffset"), WavePath,
							-FMantlePlaceLimits::MaxWaterPhaseOffset, FMantlePlaceLimits::MaxWaterPhaseOffset, Parsed.PhaseOffset);
						P.Waves.Add(Parsed);
					}
				}
			}
		}

		if (Props->HasField(TEXT("look")))
		{
			const FString LookPath = Join(Path, TEXT("look"));
			const TSharedPtr<FJsonObject>* Look = nullptr;
			if (!Props->TryGetObjectField(TEXT("look"), Look))
			{
				Ctx.Fail(LookPath, TEXT("must be an object"));
			}
			else
			{
				RequireNoUnknownKeys(Ctx, *Look, { TEXT("albedo"), TEXT("scattering"), TEXT("absorption") }, LookPath);
				ReadWaterLookColor(Ctx, *Look, TEXT("albedo"), LookPath, P.Albedo);
				ReadWaterLookColor(Ctx, *Look, TEXT("scattering"), LookPath, P.Scattering);
				ReadWaterLookColor(Ctx, *Look, TEXT("absorption"), LookPath, P.Absorption);
			}
		}

		ReadStrictOptionalBool(Ctx, Props, TEXT("collision"), Path, P.bCollision);
		if (Props->HasField(TEXT("collisionHeightOffset")))
		{
			double Offset = 0.0;
			ReadProbeNumber(Ctx, Props, TEXT("collisionHeightOffset"), Path,
				-FMantlePlaceLimits::MaxWaterCollisionShift, FMantlePlaceLimits::MaxWaterCollisionShift, Offset);
			if (Offset >= -FMantlePlaceLimits::MaxWaterCollisionShift && Offset <= FMantlePlaceLimits::MaxWaterCollisionShift)
			{
				P.CollisionHeightOffset = Offset;
			}
		}
		if (Props->HasField(TEXT("collisionExtent")))
		{
			TArray<double> Values;
			if (ReadNumberArray(Ctx, Props, TEXT("collisionExtent"), 3, Path, Values))
			{
				const int32 ErrorsBefore = Ctx.Errors.Num();
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					RangeCheck(Ctx, Values[Axis], FMantlePlaceLimits::MinWaterExtent, FMantlePlaceLimits::MaxWaterExtent,
						Index(Join(Path, TEXT("collisionExtent")), Axis));
				}
				if (Ctx.Errors.Num() == ErrorsBefore)
				{
					P.CollisionExtent = FVector(Values[0], Values[1], Values[2]);
				}
			}
		}
		TOptional<FVector2D> OceanExtent;
		ReadWaterExtent2(Ctx, Props, TEXT("oceanExtent"), Path, OceanExtent);
		P.OceanExtent = OceanExtent;
		if (Props->HasField(TEXT("centerOnZone")))
		{
			bool bCenter = false;
			ReadStrictOptionalBool(Ctx, Props, TEXT("centerOnZone"), Path, bCenter);
			P.CenterOnZone = bCenter;
		}
	}

	/** settings.water, the place's Water Zone (GhzGarage/mantle#997): every key optional. */
	void ReadWaterZone(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Settings, const FString& Path, TOptional<FMantleWaterZone>& Out)
	{
		if (!Settings->HasField(TEXT("water")))
		{
			return;
		}
		const FString WaterPath = Join(Path, TEXT("water"));
		const TSharedPtr<FJsonObject>* Water = nullptr;
		if (!Settings->TryGetObjectField(TEXT("water"), Water))
		{
			Ctx.Fail(WaterPath, TEXT("must be an object"));
			return;
		}
		RequireNoUnknownKeys(Ctx, *Water, { TEXT("zoneExtent"), TEXT("zoneLocation"), TEXT("zoneYawDeg") }, WaterPath);
		FMantleWaterZone Zone;
		TOptional<FVector2D> Extent;
		ReadWaterExtent2(Ctx, *Water, TEXT("zoneExtent"), WaterPath, Extent);
		if (Extent.IsSet())
		{
			Zone.ZoneExtent = Extent.GetValue();
		}
		if ((*Water)->HasField(TEXT("zoneLocation")))
		{
			ReadWaterVector(Ctx, *Water, TEXT("zoneLocation"), WaterPath, Zone.ZoneLocation);
		}
		ReadProbeNumber(Ctx, *Water, TEXT("zoneYawDeg"), WaterPath, -180.0, 180.0, Zone.ZoneYawDeg);
		Out = Zone;
	}

	// An entity's interactable block (the schema's interactable). Colours are linear RGB arrays like every other
	// colour of the place; entity references are checked for existence once every entity is read (ParseFromString).

	bool IsInteractionPartId(const FString& Value)
	{
		if (Value.IsEmpty() || Value.Len() > 32 || !FChar::IsAlpha(Value[0]) || Value[0] > 127)
		{
			return false;
		}
		for (const TCHAR Char : Value)
		{
			if (Char > 127 || !(FChar::IsAlnum(Char) || Char == TEXT('_')))
			{
				return false;
			}
		}
		return true;
	}

	void ReadInteractableDisplay(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantleInteractableDisplaySpec& Out)
	{
		RequireNoUnknownKeys(Ctx, Obj, { TEXT("style"), TEXT("title"), TEXT("description"), TEXT("accentColor") }, Path);
		if (const TSharedPtr<FJsonValue> Style = Obj->TryGetField(TEXT("style")))
		{
			const FString Name = Style->Type == EJson::String ? Style->AsString() : FString();
			if (Name == TEXT("default"))          { Out.Style = EMantleInteractablePromptStyle::Default; }
			else if (Name == TEXT("balanced"))    { Out.Style = EMantleInteractablePromptStyle::Balanced; }
			else if (Name == TEXT("informative")) { Out.Style = EMantleInteractablePromptStyle::Informative; }
			else
			{
				Ctx.Fail(Join(Path, TEXT("style")), TEXT("must be one of default, balanced, informative"));
			}
		}
		OptionalString(Obj, TEXT("title"), Out.Title);
		OptionalString(Obj, TEXT("description"), Out.Description);
		if (FMantlePlaceParser::CodePointLength(Out.Title) > 80)
		{
			Ctx.Fail(Join(Path, TEXT("title")), TEXT("is longer than 80 characters"));
		}
		if (FMantlePlaceParser::CodePointLength(Out.Description) > 200)
		{
			Ctx.Fail(Join(Path, TEXT("description")), TEXT("is longer than 200 characters"));
		}
		if (Obj->HasField(TEXT("accentColor")))
		{
			ReadColor(Ctx, Obj, TEXT("accentColor"), Path, Out.AccentColor);
		}
	}

	void ReadInteractableOptions(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FString& Path,
		TArray<FMantleInteractableOptionSpec>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
		if (!Obj->HasField(Key))
		{
			return;
		}
		const FString ListPath = Join(Path, Key);
		if (!Obj->TryGetArrayField(Key, List))
		{
			Ctx.Fail(ListPath, TEXT("must be an array"));
			return;
		}
		if (List->Num() > FMantlePlaceLimits::MaxInteractableOptions)
		{
			Ctx.Fail(ListPath, FString::Printf(TEXT("has more than %d entries"), FMantlePlaceLimits::MaxInteractableOptions));
			return;
		}
		bool bUsedPrimary = false;
		bool bUsedSecondary = false;
		for (int32 Number = 0; Number < List->Num(); ++Number)
		{
			const FString OptionPath = Index(ListPath, Number);
			const TSharedPtr<FJsonObject>* Option = nullptr;
			if (!(*List)[Number]->TryGetObject(Option))
			{
				Ctx.Fail(OptionPath, TEXT("must be an object"));
				continue;
			}
			RequireNoUnknownKeys(
				Ctx, *Option,
				{ TEXT("key"), TEXT("text"), TEXT("holdSeconds"), TEXT("pressCount"), TEXT("pressDrainPerSecond"),
				  TEXT("afterUse"), TEXT("cooldownSeconds") },
				OptionPath);

			FMantleInteractableOptionSpec Spec;
			if (const TSharedPtr<FJsonValue> KeyValue = (*Option)->TryGetField(TEXT("key")))
			{
				const FString Name = KeyValue->Type == EJson::String ? KeyValue->AsString() : FString();
				if (Name == TEXT("primary"))        { Spec.bSecondaryKey = false; }
				else if (Name == TEXT("secondary")) { Spec.bSecondaryKey = true; }
				else
				{
					Ctx.Fail(Join(OptionPath, TEXT("key")), TEXT("must be one of primary, secondary"));
				}
			}
			bool& bUsed = Spec.bSecondaryKey ? bUsedSecondary : bUsedPrimary;
			if (bUsed)
			{
				Ctx.Fail(Join(OptionPath, TEXT("key")), TEXT("is used by another option of this list; each option needs its own key"));
			}
			bUsed = true;

			OptionalString(*Option, TEXT("text"), Spec.Text);
			if (FMantlePlaceParser::CodePointLength(Spec.Text) > 80)
			{
				Ctx.Fail(Join(OptionPath, TEXT("text")), TEXT("is longer than 80 characters"));
			}
			if ((*Option)->HasField(TEXT("holdSeconds")) && RequireNumber(Ctx, *Option, TEXT("holdSeconds"), OptionPath, Spec.HoldSeconds))
			{
				RangeCheck(Ctx, Spec.HoldSeconds, 0.0, 30.0, Join(OptionPath, TEXT("holdSeconds")));
			}
			double Presses = 0.0;
			if ((*Option)->HasField(TEXT("pressCount")) && RequireNumber(Ctx, *Option, TEXT("pressCount"), OptionPath, Presses))
			{
				ReadBoundedInt(Ctx, Presses, 0, 100, Join(OptionPath, TEXT("pressCount")), Spec.PressCount);
			}
			if ((*Option)->HasField(TEXT("pressDrainPerSecond"))
				&& RequireNumber(Ctx, *Option, TEXT("pressDrainPerSecond"), OptionPath, Spec.PressDrainPerSecond))
			{
				RangeCheck(Ctx, Spec.PressDrainPerSecond, 0.0, 10.0, Join(OptionPath, TEXT("pressDrainPerSecond")));
			}
			if (const TSharedPtr<FJsonValue> AfterUse = (*Option)->TryGetField(TEXT("afterUse")))
			{
				const FString Name = AfterUse->Type == EJson::String ? AfterUse->AsString() : FString();
				if (Name == TEXT("none"))                     { Spec.AfterUse = EMantleInteractableAfterUse::None; }
				else if (Name == TEXT("disableUntilEnabled")) { Spec.AfterUse = EMantleInteractableAfterUse::DisableUntilEnabled; }
				else if (Name == TEXT("cooldown"))            { Spec.AfterUse = EMantleInteractableAfterUse::Cooldown; }
				else if (Name == TEXT("oneShot"))             { Spec.AfterUse = EMantleInteractableAfterUse::OneShot; }
				else
				{
					Ctx.Fail(Join(OptionPath, TEXT("afterUse")), TEXT("must be one of none, disableUntilEnabled, cooldown, oneShot"));
				}
			}
			if ((*Option)->HasField(TEXT("cooldownSeconds"))
				&& RequireNumber(Ctx, *Option, TEXT("cooldownSeconds"), OptionPath, Spec.CooldownSeconds))
			{
				RangeCheck(Ctx, Spec.CooldownSeconds, 0.0, 3600.0, Join(OptionPath, TEXT("cooldownSeconds")));
			}
			if (Spec.AfterUse == EMantleInteractableAfterUse::Cooldown && Spec.CooldownSeconds <= 0.0)
			{
				Ctx.Fail(Join(OptionPath, TEXT("cooldownSeconds")), TEXT("must be above 0 when afterUse is cooldown"));
			}
			Out.Add(MoveTemp(Spec));
		}
	}

	void ReadInteractableEntityId(FParseContext& Ctx, const FString& Value, const FString& Path)
	{
		// Same pattern as the schema's entityId.
		bool bOk = Value.StartsWith(TEXT("ent_")) && Value.Len() >= 5 && Value.Len() <= 44;
		for (int32 Position = 4; bOk && Position < Value.Len(); ++Position)
		{
			const TCHAR Char = Value[Position];
			bOk = (Char >= TEXT('a') && Char <= TEXT('z')) || (Char >= TEXT('0') && Char <= TEXT('9')) || Char == TEXT('_');
		}
		if (!bOk)
		{
			Ctx.Fail(Path, FString::Printf(TEXT("'%s' is not a valid entity id"), *Value));
		}
	}

	void ReadInteractable(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Entity, const FString& Path, TOptional<FMantleInteractableSpec>& Out)
	{
		if (!Entity->HasField(TEXT("interactable")))
		{
			return;
		}
		const FString BlockPath = Join(Path, TEXT("interactable"));
		const TSharedPtr<FJsonObject>* Block = nullptr;
		if (!Entity->TryGetObjectField(TEXT("interactable"), Block))
		{
			Ctx.Fail(BlockPath, TEXT("must be an object"));
			return;
		}
		RequireNoUnknownKeys(Ctx, *Block, { TEXT("range"), TEXT("options"), TEXT("display"), TEXT("parts") }, BlockPath);

		FMantleInteractableSpec Spec;
		if ((*Block)->HasField(TEXT("range")) && RequireNumber(Ctx, *Block, TEXT("range"), BlockPath, Spec.Range))
		{
			RangeCheck(Ctx, Spec.Range, FMantlePlaceLimits::MinInteractableRange, FMantlePlaceLimits::MaxInteractableRange,
				Join(BlockPath, TEXT("range")));
		}
		ReadInteractableOptions(Ctx, *Block, TEXT("options"), BlockPath, Spec.Options);
		if ((*Block)->HasField(TEXT("display")))
		{
			const TSharedPtr<FJsonObject>* Display = nullptr;
			if ((*Block)->TryGetObjectField(TEXT("display"), Display))
			{
				ReadInteractableDisplay(Ctx, *Display, Join(BlockPath, TEXT("display")), Spec.Display);
			}
			else
			{
				Ctx.Fail(Join(BlockPath, TEXT("display")), TEXT("must be an object"));
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
		if ((*Block)->HasField(TEXT("parts")))
		{
			const FString PartsPath = Join(BlockPath, TEXT("parts"));
			if (!(*Block)->TryGetArrayField(TEXT("parts"), Parts))
			{
				Ctx.Fail(PartsPath, TEXT("must be an array"));
			}
			else if (Parts->Num() > FMantlePlaceLimits::MaxInteractableParts)
			{
				Ctx.Fail(PartsPath, FString::Printf(TEXT("has more than %d entries"), FMantlePlaceLimits::MaxInteractableParts));
			}
			else
			{
				TSet<FString> PartIds;
				for (int32 Number = 0; Number < Parts->Num(); ++Number)
				{
					const FString PartPath = Index(PartsPath, Number);
					const TSharedPtr<FJsonObject>* Part = nullptr;
					if (!(*Parts)[Number]->TryGetObject(Part))
					{
						Ctx.Fail(PartPath, TEXT("must be an object"));
						continue;
					}
					RequireNoUnknownKeys(
						Ctx, *Part,
						{ TEXT("id"), TEXT("anchor"), TEXT("highlight"), TEXT("promptOffset"), TEXT("outline"), TEXT("outlineColor"),
						  TEXT("options"), TEXT("display"), TEXT("ignoreObstacles") },
						PartPath);

					FMantleInteractablePartSpec PartSpec;
					if (RequireString(Ctx, *Part, TEXT("id"), PartPath, PartSpec.Id))
					{
						if (!IsInteractionPartId(PartSpec.Id))
						{
							Ctx.Fail(Join(PartPath, TEXT("id")), FString::Printf(TEXT("'%s' is not a valid part id"), *PartSpec.Id));
						}
						else if (PartIds.Contains(PartSpec.Id))
						{
							Ctx.Fail(Join(PartPath, TEXT("id")), FString::Printf(TEXT("'%s' is another part's id; part ids are unique"), *PartSpec.Id));
						}
						PartIds.Add(PartSpec.Id);
					}
					if ((*Part)->HasField(TEXT("anchor")) && RequireString(Ctx, *Part, TEXT("anchor"), PartPath, PartSpec.AnchorEntity))
					{
						ReadInteractableEntityId(Ctx, PartSpec.AnchorEntity, Join(PartPath, TEXT("anchor")));
					}
					const TArray<TSharedPtr<FJsonValue>>* Highlight = nullptr;
					if ((*Part)->HasField(TEXT("highlight")))
					{
						const FString HighlightPath = Join(PartPath, TEXT("highlight"));
						if (!(*Part)->TryGetArrayField(TEXT("highlight"), Highlight))
						{
							Ctx.Fail(HighlightPath, TEXT("must be an array"));
						}
						else if (Highlight->Num() > FMantlePlaceLimits::MaxInteractableHighlights)
						{
							Ctx.Fail(HighlightPath, FString::Printf(TEXT("has more than %d entries"), FMantlePlaceLimits::MaxInteractableHighlights));
						}
						else
						{
							for (int32 H = 0; H < Highlight->Num(); ++H)
							{
								FString Target;
								if ((*Highlight)[H]->Type == EJson::String && (*Highlight)[H]->TryGetString(Target))
								{
									ReadInteractableEntityId(Ctx, Target, Index(HighlightPath, H));
									PartSpec.HighlightEntities.Add(Target);
								}
								else
								{
									Ctx.Fail(Index(HighlightPath, H), TEXT("must be an entity id"));
								}
							}
						}
					}
					if ((*Part)->HasField(TEXT("promptOffset")))
					{
						TArray<double> Offset;
						if (ReadNumberArray(Ctx, *Part, TEXT("promptOffset"), 3, PartPath, Offset))
						{
							PartSpec.PromptOffset = FVector(Offset[0], Offset[1], Offset[2]);
						}
					}
					ReadStrictOptionalBool(Ctx, *Part, TEXT("outline"), PartPath, PartSpec.bOutline);
					ReadStrictOptionalBool(Ctx, *Part, TEXT("ignoreObstacles"), PartPath, PartSpec.bIgnoreObstacles);
					if ((*Part)->HasField(TEXT("outlineColor")))
					{
						PartSpec.bHasOutlineColor = ReadColor(Ctx, *Part, TEXT("outlineColor"), PartPath, PartSpec.OutlineColor);
					}
					ReadInteractableOptions(Ctx, *Part, TEXT("options"), PartPath, PartSpec.Options);
					if ((*Part)->HasField(TEXT("display")))
					{
						const TSharedPtr<FJsonObject>* Display = nullptr;
						if ((*Part)->TryGetObjectField(TEXT("display"), Display))
						{
							PartSpec.bHasDisplay = true;
							ReadInteractableDisplay(Ctx, *Display, Join(PartPath, TEXT("display")), PartSpec.Display);
						}
						else
						{
							Ctx.Fail(Join(PartPath, TEXT("display")), TEXT("must be an object"));
						}
					}
					Spec.Parts.Add(MoveTemp(PartSpec));
				}
			}
		}
		Out = MoveTemp(Spec);
	}

	void ParseEntityProps(
		FParseContext& Ctx,
		const TSharedPtr<FJsonObject>& Props,
		EMantleEntityType Type,
		const FString& Path,
		FMantleEntity& Entity)
	{
		switch (Type)
		{
		case EMantleEntityType::StaticMesh:
		{
			RequireNoUnknownKeys(
				Ctx, Props,
				{ TEXT("mesh"), TEXT("materials"), TEXT("collision"), TEXT("castShadow"), TEXT("physics"), TEXT("surface"), TEXT("drawDistance") },
				Path);
			FMantleStaticMeshProps P;
			ReadMeshProps(Ctx, Props, Path, P);
			ReadPhysicsProps(Ctx, Props, Path, P.Physics);
			// A physics body's surface is its physics.material, so the two together are refused
			// (validate.py and the backend report it at the same path).
			if (Props->HasField(TEXT("physics")) && Props->HasField(TEXT("surface")))
			{
				Ctx.Fail(Join(Path, TEXT("surface")),
					TEXT("is not allowed beside a physics block; a physics entity takes its surface from physics.material"));
			}
			else
			{
				ReadSurface(Ctx, Props, Path, P.Surface);
			}
			Entity.Props.Set<FMantleStaticMeshProps>(P);
			break;
		}

		case EMantleEntityType::InstancedMesh:
		{
			RequireNoUnknownKeys(
				Ctx, Props,
				{ TEXT("mesh"), TEXT("materials"), TEXT("collision"), TEXT("castShadow"), TEXT("surface"), TEXT("drawDistance"), TEXT("instances") },
				Path);
			FMantleInstancedMeshProps P;
			ReadMeshProps(Ctx, Props, Path, P.Mesh);
			ReadSurface(Ctx, Props, Path, P.Mesh.Surface);
			ReadInstances(Ctx, Props, Path, P.Instances);
			Entity.Props.Set<FMantleInstancedMeshProps>(MoveTemp(P));
			break;
		}

		case EMantleEntityType::Group:
		case EMantleEntityType::PlayerStart:
		{
			RequireNoUnknownKeys(Ctx, Props, {}, Path);
			Entity.Props.Set<FMantleEmptyProps>(FMantleEmptyProps{});
			break;
		}

		case EMantleEntityType::PointLight:
		{
			RequireNoUnknownKeys(
				Ctx, Props,
				{ TEXT("intensity"), TEXT("intensityUnits"), TEXT("color"), TEXT("radius"), TEXT("castShadow"),
				  TEXT("useTemperature"), TEXT("temperature"), TEXT("sourceRadius"), TEXT("softSourceRadius"), TEXT("sourceLength") },
				Path);
			FMantlePointLightProps P;
			if (RequireNumber(Ctx, Props, TEXT("intensity"), Path, P.Intensity))
			{
				RangeCheck(Ctx, P.Intensity, 0.0, FMantlePlaceLimits::MaxLocalLightIntensity, Join(Path, TEXT("intensity")));
			}
			ReadLightUnits(Ctx, Props, Path, P.IntensityUnits);
			ReadColor(Ctx, Props, TEXT("color"), Path, P.Color);
			if (RequireNumber(Ctx, Props, TEXT("radius"), Path, P.Radius))
			{
				RequirePositive(Ctx, P.Radius, Join(Path, TEXT("radius")));
			}
			OptionalBool(Props, TEXT("castShadow"), P.bCastShadow);
			ReadLightTemperature(Ctx, Props, Path, P.bUseTemperature, P.Temperature);
			ReadLightSourceShape(Ctx, Props, Path, P.Source);
			Entity.Props.Set<FMantlePointLightProps>(P);
			break;
		}

		case EMantleEntityType::SpotLight:
		{
			RequireNoUnknownKeys(
				Ctx, Props,
				{ TEXT("intensity"), TEXT("intensityUnits"), TEXT("color"), TEXT("radius"), TEXT("innerCone"),
				  TEXT("outerCone"), TEXT("castShadow"), TEXT("useTemperature"), TEXT("temperature"), TEXT("sourceRadius"),
				  TEXT("softSourceRadius"), TEXT("sourceLength") },
				Path);
			FMantleSpotLightProps P;
			if (RequireNumber(Ctx, Props, TEXT("intensity"), Path, P.Intensity))
			{
				RangeCheck(Ctx, P.Intensity, 0.0, FMantlePlaceLimits::MaxLocalLightIntensity, Join(Path, TEXT("intensity")));
			}
			ReadLightUnits(Ctx, Props, Path, P.IntensityUnits);
			ReadColor(Ctx, Props, TEXT("color"), Path, P.Color);
			if (RequireNumber(Ctx, Props, TEXT("radius"), Path, P.Radius))
			{
				RequirePositive(Ctx, P.Radius, Join(Path, TEXT("radius")));
			}
			if (RequireNumber(Ctx, Props, TEXT("innerCone"), Path, P.InnerCone))
			{
				RangeCheck(Ctx, P.InnerCone, 0.0, FMantlePlaceLimits::MaxSpotConeDegrees, Join(Path, TEXT("innerCone")));
			}
			if (RequireNumber(Ctx, Props, TEXT("outerCone"), Path, P.OuterCone))
			{
				RangeCheck(Ctx, P.OuterCone, 0.0, FMantlePlaceLimits::MaxSpotConeDegrees, Join(Path, TEXT("outerCone")));
			}
			OptionalBool(Props, TEXT("castShadow"), P.bCastShadow);
			ReadLightTemperature(Ctx, Props, Path, P.bUseTemperature, P.Temperature);
			ReadLightSourceShape(Ctx, Props, Path, P.Source);
			Entity.Props.Set<FMantleSpotLightProps>(P);
			break;
		}

		case EMantleEntityType::RectLight:
		{
			RequireNoUnknownKeys(
				Ctx, Props,
				{ TEXT("intensity"), TEXT("intensityUnits"), TEXT("color"), TEXT("radius"), TEXT("sourceWidth"),
				  TEXT("sourceHeight"), TEXT("barnDoorAngle"), TEXT("barnDoorLength"), TEXT("useTemperature"),
				  TEXT("temperature"), TEXT("castShadow") },
				Path);
			FMantleRectLightProps P;
			if (RequireNumber(Ctx, Props, TEXT("intensity"), Path, P.Intensity))
			{
				RangeCheck(Ctx, P.Intensity, 0.0, FMantlePlaceLimits::MaxLocalLightIntensity, Join(Path, TEXT("intensity")));
			}
			ReadLightUnits(Ctx, Props, Path, P.IntensityUnits);
			ReadColor(Ctx, Props, TEXT("color"), Path, P.Color);
			if (RequireNumber(Ctx, Props, TEXT("radius"), Path, P.Radius))
			{
				RequirePositive(Ctx, P.Radius, Join(Path, TEXT("radius")));
			}
			ReadOptionalRange(Ctx, Props, TEXT("sourceWidth"), Path, 0.0, FMantlePlaceLimits::MaxRectLightSourceSize, P.SourceWidth);
			ReadOptionalRange(Ctx, Props, TEXT("sourceHeight"), Path, 0.0, FMantlePlaceLimits::MaxRectLightSourceSize, P.SourceHeight);
			ReadOptionalRange(Ctx, Props, TEXT("barnDoorAngle"), Path, 0.0, FMantlePlaceLimits::MaxRectLightBarnDoorAngle, P.BarnDoorAngle);
			ReadOptionalRange(Ctx, Props, TEXT("barnDoorLength"), Path, 0.0, FMantlePlaceLimits::MaxRectLightBarnDoorLength, P.BarnDoorLength);
			ReadLightTemperature(Ctx, Props, Path, P.bUseTemperature, P.Temperature);
			OptionalBool(Props, TEXT("castShadow"), P.bCastShadow);
			Entity.Props.Set<FMantleRectLightProps>(P);
			break;
		}

		case EMantleEntityType::Decal:
		{
			RequireNoUnknownKeys(
				Ctx, Props,
				{ TEXT("baseColor"), TEXT("normal"), TEXT("size"), TEXT("opacity"), TEXT("sortOrder"), TEXT("fadeScreenSize") },
				Path);
			FMantleDecalProps P;
			// An empty id could only fail later, at build, on every player's machine.
			if (RequireString(Ctx, Props, TEXT("baseColor"), Path, P.BaseColor) && P.BaseColor.IsEmpty())
			{
				Ctx.Fail(Join(Path, TEXT("baseColor")), TEXT("names no texture"));
			}
			if (Props->HasField(TEXT("normal")) && (!Props->TryGetStringField(TEXT("normal"), P.Normal) || P.Normal.IsEmpty()))
			{
				Ctx.Fail(Join(Path, TEXT("normal")), TEXT("must name a texture"));
			}
			TArray<double> Size;
			if (ReadNumberArray(Ctx, Props, TEXT("size"), 3, Path, Size))
			{
				for (int32 i = 0; i < 3; ++i)
				{
					if (Size[i] <= 0.0)
					{
						Ctx.Fail(Index(Join(Path, TEXT("size")), i), FString::Printf(TEXT("%g must be greater than zero"), Size[i]));
					}
				}
				P.Size = FVector(Size[0], Size[1], Size[2]);
			}
			ReadOptionalRange(Ctx, Props, TEXT("opacity"), Path, 0.0, 1.0, P.Opacity);
			double SortOrder = 0.0;
			if (Props->HasField(TEXT("sortOrder")) && RequireNumber(Ctx, Props, TEXT("sortOrder"), Path, SortOrder))
			{
				ReadBoundedInt(Ctx, SortOrder, -FMantlePlaceLimits::MaxDecalSortOrder, FMantlePlaceLimits::MaxDecalSortOrder,
					Join(Path, TEXT("sortOrder")), P.SortOrder);
			}
			ReadOptionalRange(Ctx, Props, TEXT("fadeScreenSize"), Path, 0.0, 1.0, P.FadeScreenSize);
			Entity.Props.Set<FMantleDecalProps>(P);
			break;
		}

		case EMantleEntityType::DirectionalLight:
		{
			RequireNoUnknownKeys(Ctx, Props, { TEXT("intensity"), TEXT("color"), TEXT("castShadow") }, Path);
			FMantleDirectionalLightProps P;
			if (RequireNumber(Ctx, Props, TEXT("intensity"), Path, P.Intensity))
			{
				RangeCheck(Ctx, P.Intensity, 0.0, FMantlePlaceLimits::MaxDirectionalLightIntensity, Join(Path, TEXT("intensity")));
			}
			ReadColor(Ctx, Props, TEXT("color"), Path, P.Color);
			OptionalBool(Props, TEXT("castShadow"), P.bCastShadow);
			Entity.Props.Set<FMantleDirectionalLightProps>(P);
			break;
		}

		case EMantleEntityType::TriggerVolume:
		{
			RequireNoUnknownKeys(Ctx, Props, { TEXT("shape"), TEXT("extent") }, Path);
			FMantleTriggerVolumeProps P;
			FString Shape;
			if (RequireString(Ctx, Props, TEXT("shape"), Path, Shape) && !MantleParseTriggerShape(Shape, P.Shape))
			{
				Ctx.Fail(Join(Path, TEXT("shape")), FString::Printf(TEXT("'%s' is not a valid trigger shape"), *Shape));
			}
			TArray<double> Extent;
			if (ReadNumberArray(Ctx, Props, TEXT("extent"), 3, Path, Extent))
			{
				for (int32 i = 0; i < 3; ++i)
				{
					if (Extent[i] <= 0.0)
					{
						Ctx.Fail(
							Index(Join(Path, TEXT("extent")), i),
							FString::Printf(TEXT("%g must be greater than zero"), Extent[i]));
					}
				}
				P.Extent = FVector(Extent[0], Extent[1], Extent[2]);
				// A sphere has one radius and the builder reads X alone, so unequal extents would
				// silently become a different sphere.
				if (P.Shape == EMantleTriggerShape::Sphere && (Extent[0] != Extent[1] || Extent[0] != Extent[2]))
				{
					Ctx.Fail(
						Join(Path, TEXT("extent")),
						FString::Printf(TEXT("a sphere trigger has one radius; its extents (%g, %g, %g) must all be equal"),
							Extent[0], Extent[1], Extent[2]));
				}
			}
			Entity.Props.Set<FMantleTriggerVolumeProps>(P);
			break;
		}

		case EMantleEntityType::SoundEmitter:
		{
			RequireNoUnknownKeys(Ctx, Props, { TEXT("sound"), TEXT("volume"), TEXT("radius"), TEXT("looping"), TEXT("autoplay") }, Path);
			FMantleSoundEmitterProps P;
			// An empty id could only fail later, at build, on every player's machine.
			if (RequireString(Ctx, Props, TEXT("sound"), Path, P.Sound) && P.Sound.IsEmpty())
			{
				Ctx.Fail(Join(Path, TEXT("sound")), TEXT("names no sound"));
			}
			OptionalNumber(Props, TEXT("volume"), P.Volume);
			RangeCheck(Ctx, P.Volume, 0.0, 1.0, Join(Path, TEXT("volume")));
			// Optional; absent means MANTLE_DEFAULT_SOUND_RADIUS, present must be positive.
			if (Props->HasField(TEXT("radius")) && RequireNumber(Ctx, Props, TEXT("radius"), Path, P.Radius))
			{
				RequirePositive(Ctx, P.Radius, Join(Path, TEXT("radius")));
			}
			OptionalBool(Props, TEXT("looping"), P.bLooping);
			// Absent is true, so every place written before the key builds as it always did.
			OptionalBool(Props, TEXT("autoplay"), P.bAutoplay);
			Entity.Props.Set<FMantleSoundEmitterProps>(P);
			break;
		}

		case EMantleEntityType::CrowdSpawner:
		{
			FMantleCrowdSpawnerProps P;
			ParseCrowdSpawner(Ctx, Props, Path, P);
			Entity.Props.Set<FMantleCrowdSpawnerProps>(MoveTemp(P));
			break;
		}

		case EMantleEntityType::SkeletalMesh:
		{
			// An uploaded skeletal mesh, its material slots and an optional clip on its skeleton.
			RequireNoUnknownKeys(
				Ctx, Props,
				{ TEXT("mesh"), TEXT("materials"), TEXT("castShadow"), TEXT("clip"), TEXT("loop"), TEXT("playRate"), TEXT("startOffset") },
				Path);
			FMantleSkeletalMeshProps P;
			if (RequireString(Ctx, Props, TEXT("mesh"), Path, P.Mesh) && P.Mesh.IsEmpty())
			{
				Ctx.Fail(Join(Path, TEXT("mesh")), TEXT("names no mesh"));
			}
			ReadMaterialSlots(Ctx, Props, Path, P.Materials);
			if (P.Materials.Num() > FMantlePlaceLimits::MaxMaterialSlots)
			{
				Ctx.Fail(Join(Path, TEXT("materials")), FString::Printf(TEXT("has more than %d entries"), FMantlePlaceLimits::MaxMaterialSlots));
			}
			OptionalBool(Props, TEXT("castShadow"), P.bCastShadow);
			if (Props->HasField(TEXT("clip")) && RequireString(Ctx, Props, TEXT("clip"), Path, P.Clip) && P.Clip.IsEmpty())
			{
				Ctx.Fail(Join(Path, TEXT("clip")), TEXT("names no clip; leave clip out for none"));
			}
			OptionalBool(Props, TEXT("loop"), P.bLoop);
			if (Props->HasField(TEXT("playRate")) && RequireNumber(Ctx, Props, TEXT("playRate"), Path, P.PlayRate))
			{
				RangeCheck(Ctx, P.PlayRate, 0.0, FMantlePlaceLimits::MaxClipPlayRate, Join(Path, TEXT("playRate")));
			}
			if (Props->HasField(TEXT("startOffset")) && RequireNumber(Ctx, Props, TEXT("startOffset"), Path, P.StartOffset))
			{
				RangeCheck(Ctx, P.StartOffset, 0.0, FMantlePlaceLimits::MaxClipStartOffsetSeconds, Join(Path, TEXT("startOffset")));
			}
			Entity.Props.Set<FMantleSkeletalMeshProps>(MoveTemp(P));
			break;
		}

		case EMantleEntityType::Terrain:
		{
			FMantleTerrainProps P;
			ParseTerrain(Ctx, Props, Path, P);
			Entity.Props.Set<FMantleTerrainProps>(MoveTemp(P));
			break;
		}

		case EMantleEntityType::VehicleSpawn:
		{
			// A creator vehicle parked here: the name of a vehicle in the place's storage.
			// Never a raw uploaded asset id: that would skip the library's and storage's authorization,
			// so "vehicle" stays a known key only to refuse it in plain words.
			RequireNoUnknownKeys(Ctx, Props, { TEXT("vehicle"), TEXT("storageItem"), TEXT("respawnSeconds"), TEXT("enabled"), TEXT("livery") }, Path);
			FMantleVehicleSpawnProps P;
			const bool bHasStorageItem = Props->HasField(TEXT("storageItem"));
			if (Props->HasField(TEXT("vehicle")) && Ctx.bAcceptDraftVehicleIds)
			{
				// An offline preview (Test Locally) runs the level's own converted vehicle from its bundle; publishing stores it and
				// rewrites the spawn to a storageItem. Every other load is strict (the branch below).
				if (bHasStorageItem)
				{
					Ctx.Fail(Path, TEXT("has both vehicle and storageItem; name an uploaded vehicle or a stored one, not both"));
				}
				if (RequireString(Ctx, Props, TEXT("vehicle"), Path, P.Vehicle) && !FMantlePlaceAssets::IsUploadedId(P.Vehicle))
				{
					Ctx.Fail(Join(Path, TEXT("vehicle")), TEXT("must be an uploaded vehicle's id (ast_u and 32 lowercase hex digits)"));
				}
			}
			else if (Props->HasField(TEXT("vehicle")))
			{
				Ctx.Fail(Join(Path, TEXT("vehicle")), TEXT("is not allowed: a vehicle spawn parks a vehicle from the place's storage. Publish the vehicle to the library, add it to the place's storage, and set the spawn's storageItem to its name"));
			}
			else if (!bHasStorageItem)
			{
				Ctx.Fail(Join(Path, TEXT("storageItem")), TEXT("is required: a vehicle spawn parks a vehicle from the place's storage. Publish the vehicle to the library, add it to the place's storage, and set storageItem to its name"));
			}
			if (bHasStorageItem && RequireString(Ctx, Props, TEXT("storageItem"), Path, P.StorageItem)
				&& !FMantlePlaceLimits::IsStorageName(P.StorageItem))
			{
				Ctx.Fail(Join(Path, TEXT("storageItem")), TEXT("must be the name of one of the place's storage entries (a letter followed by up to 63 letters, digits or '_')"));
			}
			if (Props->HasField(TEXT("respawnSeconds")) && RequireNumber(Ctx, Props, TEXT("respawnSeconds"), Path, P.RespawnSeconds))
			{
				RangeCheck(Ctx, P.RespawnSeconds, 0.0, FMantlePlaceLimits::MaxVehicleRespawnSeconds, Join(Path, TEXT("respawnSeconds")));
			}
			OptionalBool(Props, TEXT("enabled"), P.bEnabled);
			// An index into the vehicle's liveries, -1 its default.
			double Livery = -1.0;
			if (Props->HasField(TEXT("livery")) && RequireNumber(Ctx, Props, TEXT("livery"), Path, Livery))
			{
				ReadBoundedInt(Ctx, Livery, -1, FMantlePlaceLimits::MaxVehicleLiveryIndex, Join(Path, TEXT("livery")), P.Livery);
			}
			Entity.Props.Set<FMantleVehicleSpawnProps>(P);
			break;
		}

		case EMantleEntityType::WeaponSpawn:
		{
			// A creator weapon waiting here: an uploaded Weapon asset or the name of a stored weapon. Exactly one.
			RequireNoUnknownKeys(Ctx, Props, { TEXT("weapon"), TEXT("storageItem"), TEXT("respawnSeconds"), TEXT("enabled") }, Path);
			FMantleWeaponSpawnProps P;
			const bool bHasWeapon = Props->HasField(TEXT("weapon"));
			const bool bHasStorageItem = Props->HasField(TEXT("storageItem"));
			if (bHasWeapon && bHasStorageItem)
			{
				Ctx.Fail(Path, TEXT("has both weapon and storageItem; name an uploaded weapon or a stored one, not both"));
			}
			else if (!bHasWeapon && !bHasStorageItem)
			{
				Ctx.Fail(Join(Path, TEXT("weapon")), TEXT("needs exactly one of weapon and storageItem; it has neither"));
			}
			if (bHasWeapon && RequireString(Ctx, Props, TEXT("weapon"), Path, P.Weapon) && !FMantlePlaceAssets::IsUploadedId(P.Weapon))
			{
				Ctx.Fail(Join(Path, TEXT("weapon")), TEXT("must be an uploaded weapon's id (ast_u and 32 lowercase hex digits)"));
			}
			if (bHasStorageItem && RequireString(Ctx, Props, TEXT("storageItem"), Path, P.StorageItem)
				&& !FMantlePlaceLimits::IsStorageName(P.StorageItem))
			{
				Ctx.Fail(Join(Path, TEXT("storageItem")), TEXT("must be the name of one of the place's storage entries (a letter followed by up to 63 letters, digits or '_')"));
			}
			if (Props->HasField(TEXT("respawnSeconds")) && RequireNumber(Ctx, Props, TEXT("respawnSeconds"), Path, P.RespawnSeconds))
			{
				RangeCheck(Ctx, P.RespawnSeconds, 0.0, FMantlePlaceLimits::MaxWeaponRespawnSeconds, Join(Path, TEXT("respawnSeconds")));
			}
			OptionalBool(Props, TEXT("enabled"), P.bEnabled);
			Entity.Props.Set<FMantleWeaponSpawnProps>(P);
			break;
		}

		case EMantleEntityType::ScriptPoint:
		{
			// An anchor for a stored script package: the storage name it belongs to, up to 16 text parameters
			// the package's scripts read as attributes, and whether it is on.
			RequireNoUnknownKeys(Ctx, Props, { TEXT("storageItem"), TEXT("parameters"), TEXT("enabled") }, Path);
			FMantleScriptPointProps P;
			if (RequireString(Ctx, Props, TEXT("storageItem"), Path, P.StorageItem) && !FMantlePlaceLimits::IsStorageName(P.StorageItem))
			{
				Ctx.Fail(Join(Path, TEXT("storageItem")), TEXT("must be the name of one of the place's storage entries (a letter followed by up to 63 letters, digits or '_')"));
			}
			if (Props->HasField(TEXT("parameters")))
			{
				const FString ParametersPath = Join(Path, TEXT("parameters"));
				const TSharedPtr<FJsonObject>* Parameters = nullptr;
				if (!Props->TryGetObjectField(TEXT("parameters"), Parameters) || !Parameters || !Parameters->IsValid())
				{
					Ctx.Fail(ParametersPath, TEXT("must be an object of text values"));
				}
				else
				{
					if ((*Parameters)->Values.Num() > FMantlePlaceLimits::MaxScriptPointParameters)
					{
						Ctx.Fail(ParametersPath, FString::Printf(TEXT("has %d parameters; a Script Point holds at most %d"),
							(*Parameters)->Values.Num(), FMantlePlaceLimits::MaxScriptPointParameters));
					}
					for (const auto& Pair : (*Parameters)->Values)
					{
						// UE 5.8 keys FJsonObject::Values by TSharedString, not FString.
						const FString Key(Pair.Key.ToView());
						const FString KeyPath = Join(ParametersPath, Key);
						if (!FMantlePlaceLimits::IsScriptPointParameterName(Key))
						{
							Ctx.Fail(KeyPath, TEXT("must be a letter followed by up to 31 letters, digits or '_'"));
							continue;
						}
						FString Value;
						if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String || !Pair.Value->TryGetString(Value))
						{
							Ctx.Fail(KeyPath, TEXT("must be a string"));
							continue;
						}
						if (FMantlePlaceParser::CodePointLength(Value) > FMantlePlaceLimits::MaxScriptPointParameterValueLength)
						{
							Ctx.Fail(KeyPath, FString::Printf(TEXT("is longer than %d characters"), FMantlePlaceLimits::MaxScriptPointParameterValueLength));
							continue;
						}
						FMantleScriptPointParameter& Parameter = P.Parameters.AddDefaulted_GetRef();
						Parameter.Name = Key;
						Parameter.Value = MoveTemp(Value);
					}
					P.Parameters.Sort([](const FMantleScriptPointParameter& A, const FMantleScriptPointParameter& B)
					{
						return A.Name.Compare(B.Name, ESearchCase::CaseSensitive) < 0;
					});
				}
			}
			OptionalBool(Props, TEXT("enabled"), P.bEnabled);
			Entity.Props.Set<FMantleScriptPointProps>(MoveTemp(P));
			break;
		}

		case EMantleEntityType::Effect:
		{
			// A built-in particle effect. Every key but the archetype is optional: one left out
			// takes the archetype's own look, so the look is filled once the archetype is known.
			RequireNoUnknownKeys(
				Ctx, Props,
				{ TEXT("archetype"), TEXT("color"), TEXT("size"), TEXT("rate"), TEXT("speed"), TEXT("lifetime"), TEXT("autoplay"), TEXT("loop"),
				  TEXT("texture"), TEXT("blend"), TEXT("colorEnd"), TEXT("alpha"), TEXT("alphaEnd"), TEXT("fadeIn"), TEXT("sizeMin"),
				  TEXT("sizeMax"), TEXT("sizeEndScale"), TEXT("lifetimeMin"), TEXT("lifetimeMax"), TEXT("velocityMin"), TEXT("velocityMax"),
				  TEXT("acceleration"), TEXT("shape"), TEXT("maxParticles"), TEXT("subImages"), TEXT("subImageMode") },
				Path);
			FMantleEffectProps P;
			FString Archetype;
			bool bKnownArchetype = false;
			if (RequireString(Ctx, Props, TEXT("archetype"), Path, Archetype))
			{
				bKnownArchetype = FMantleEffects::ParseArchetype(Archetype, P.Archetype);
				if (!bKnownArchetype)
				{
					Ctx.Fail(Join(Path, TEXT("archetype")),
						FString::Printf(TEXT("'%s' is not a built-in effect (%s)"), *Archetype, *FMantleEffects::ArchetypeList()));
				}
			}
			P.Look = FMantleEffects::DefaultLook(P.Archetype);
			if (Props->HasField(TEXT("color")))
			{
				ReadColor(Ctx, Props, TEXT("color"), Path, P.Look.Color);
			}
			ReadOptionalRange(Ctx, Props, TEXT("size"), Path, FMantlePlaceLimits::MinEffectSize, FMantlePlaceLimits::MaxEffectSize, P.Look.Size);
			ReadOptionalRange(Ctx, Props, TEXT("rate"), Path, 0.0, FMantlePlaceLimits::MaxEffectRate, P.Look.Rate);
			ReadOptionalRange(Ctx, Props, TEXT("speed"), Path, 0.0, FMantlePlaceLimits::MaxEffectSpeed, P.Look.Speed);
			ReadOptionalRange(Ctx, Props, TEXT("lifetime"), Path, FMantlePlaceLimits::MinEffectLifetime, FMantlePlaceLimits::MaxEffectLifetime,
				P.Look.Lifetime);
			ReadStrictOptionalBool(Ctx, Props, TEXT("autoplay"), Path, P.bAutoplay);
			ReadStrictOptionalBool(Ctx, Props, TEXT("loop"), Path, P.bLoop);
			if (bKnownArchetype)
			{
				ParseEffectSprites(Ctx, Props, Path, P.Archetype == EMantleEffectArchetype::Sprites, P);
			}
			Entity.Props.Set<FMantleEffectProps>(P);
			break;
		}

		case EMantleEntityType::Constraint:
		{
			FMantleConstraintProps P;
			ParseConstraint(Ctx, Props, Path, P);
			Entity.Props.Set<FMantleConstraintProps>(MoveTemp(P));
			break;
		}

		case EMantleEntityType::Scatter:
		{
			// A seeded field of instanced meshes, generated on each client from these numbers.
			FMantleScatterParams P;
			ParseScatter(Ctx, Props, Path, P);
			Entity.Props.Set<FMantleScatterParams>(MoveTemp(P));
			break;
		}

		case EMantleEntityType::ReflectionProbe:
		{
			// A runtime-captured box or sphere, built on each client once the place stands.
			FMantleReflectionProbeProps P;
			ParseReflectionProbe(Ctx, Props, Path, P);
			Entity.Props.Set<FMantleReflectionProbeProps>(MoveTemp(P));
			break;
		}

		case EMantleEntityType::Text:
		{
			// World text, drawn on each client in Mantle's built-in font (GhzGarage/mantle#724).
			FMantleTextProps P;
			ParseText(Ctx, Props, Path, P);
			Entity.Props.Set<FMantleTextProps>(MoveTemp(P));
			break;
		}

		case EMantleEntityType::WaterBody:
		{
			// A Water plugin body, built on each client once the place stands (GhzGarage/mantle#997).
			FMantleWaterBodyProps P;
			ParseWaterBody(Ctx, Props, Path, P);
			Entity.Props.Set<FMantleWaterBodyProps>(MoveTemp(P));
			break;
		}
		}
	}

	void ParseEntity(FParseContext& Ctx, const TSharedPtr<FJsonObject>& Obj, const FString& Path, FMantlePlace& Place)
	{
		RequireNoUnknownKeys(
			Ctx, Obj,
			{ TEXT("id"), TEXT("type"), TEXT("name"), TEXT("parent"), TEXT("transform"),
			  TEXT("properties"), TEXT("tags"), TEXT("dynamic"), TEXT("interactable") },
			Path);

		FMantleEntity Entity;
		RequireString(Ctx, Obj, TEXT("id"), Path, Entity.Id);

		FString TypeStr;
		if (!RequireString(Ctx, Obj, TEXT("type"), Path, TypeStr))
		{
			return;
		}
		if (!MantleParseEntityType(TypeStr, Entity.Type))
		{
			Ctx.Fail(
				Join(Path, TEXT("type")),
				FString::Printf(TEXT("'%s' is not a supported entity type"), *TypeStr));
			return;
		}

		OptionalString(Obj, TEXT("name"), Entity.Name);

		// parent is entityId or null.
		const TSharedPtr<FJsonValue> ParentValue = Obj->TryGetField(TEXT("parent"));
		if (ParentValue.IsValid() && ParentValue->Type == EJson::String)
		{
			Entity.Parent = ParentValue->AsString();
		}

		ReadTransform(Ctx, Obj, Path, Entity.Transform);

		const TSharedPtr<FJsonObject>* Props = nullptr;
		if (!Obj->TryGetObjectField(TEXT("properties"), Props))
		{
			Ctx.Fail(Join(Path, TEXT("properties")), TEXT("required object is missing"));
			return;
		}
		ParseEntityProps(Ctx, *Props, Entity.Type, Join(Path, TEXT("properties")), Entity);

		ReadStringArray(Obj, TEXT("tags"), Entity.Tags);
		// The schema's length limits, which nothing else enforced.
		if (FMantlePlaceParser::CodePointLength(Entity.Name) > FMantlePlaceLimits::MaxEntityNameLength)
		{
			Ctx.Fail(Join(Path, TEXT("name")), FString::Printf(TEXT("is longer than %d characters"), FMantlePlaceLimits::MaxEntityNameLength));
		}
		for (int32 TagIndex = 0; TagIndex < Entity.Tags.Num(); ++TagIndex)
		{
			if (FMantlePlaceParser::CodePointLength(Entity.Tags[TagIndex]) > FMantlePlaceLimits::MaxTagLength)
			{
				Ctx.Fail(Index(Join(Path, TEXT("tags")), TagIndex), FString::Printf(TEXT("is longer than %d characters"), FMantlePlaceLimits::MaxTagLength));
			}
		}
		OptionalBool(Obj, TEXT("dynamic"), Entity.bDynamic);
		ReadInteractable(Ctx, Obj, Path, Entity.Interactable);
		// A physics body moves, so it is exempt from the builder's static freeze whatever
		// "dynamic" says.
		if (MantlePhysicsOf(Entity))
		{
			Entity.bDynamic = true;
		}

		Place.Entities.Add(MoveTemp(Entity));
	}
}

// A place the server accepted must not fail to load because an emoji counted as two.
int32 FMantlePlaceParser::CodePointLength(const FString& Value)
{
	int32 Length = 0;
	for (const TCHAR Char : Value)
	{
		if (!StringConv::IsLowSurrogate(Char))
		{
			++Length;
		}
	}
	return Length;
}

FMantleParseResult FMantlePlaceParser::ParseFromString(const FString& Json)
{
	return ParseFromString(Json, TArray<FString>());
}

FMantleParseResult FMantlePlaceParser::ParseFromString(const FString& Json, const TArray<FString>& DiskScriptPackages)
{
	return ParseFromString(Json, DiskScriptPackages, /*bAcceptDraftVehicleIds*/ false);
}

FMantleParseResult FMantlePlaceParser::ParseFromString(const FString& Json, const TArray<FString>& DiskScriptPackages, const bool bAcceptDraftVehicleIds)
{
	FMantleParseResult Result;
	FParseContext Ctx;
	Ctx.bAcceptDraftVehicleIds = bAcceptDraftVehicleIds;

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		Result.Errors.Add(FMantleParseError{ TEXT(""), TEXT("not valid JSON") });
		return Result;
	}

	FMantlePlace& Place = Result.Place;

	// Schema version is checked before anything else — a newer place must be refused
	// outright rather than partially understood.
	double Version = 0.0;
	if (!Root->TryGetNumberField(TEXT("schemaVersion"), Version))
	{
		Ctx.Fail(TEXT("/schemaVersion"), TEXT("required integer is missing"));
		Result.Errors = MoveTemp(Ctx.Errors);
		return Result;
	}

	Place.SchemaVersion = static_cast<int32>(Version);
	if (Place.SchemaVersion > MANTLE_PLACE_SCHEMA_VERSION)
	{
		Ctx.Fail(
			TEXT("/schemaVersion"),
			FString::Printf(
				TEXT("this place needs schema version %d; this client supports up to %d. Update Mantle to open it."),
				Place.SchemaVersion,
				MANTLE_PLACE_SCHEMA_VERSION));
		Result.Errors = MoveTemp(Ctx.Errors);
		return Result;
	}
	if (Place.SchemaVersion < 1)
	{
		Ctx.Fail(TEXT("/schemaVersion"), TEXT("must be at least 1"));
	}

	RequireNoUnknownKeys(
		Ctx, Root,
		{ TEXT("schemaVersion"), TEXT("placeId"), TEXT("revision"), TEXT("metadata"),
		  TEXT("settings"), TEXT("assets"), TEXT("entities"), TEXT("scripts"), TEXT("scriptApi"),
		  TEXT("uiDocuments"), TEXT("storage"), TEXT("seats"), TEXT("localization"), TEXT("audience") },
		TEXT(""));

	RequireString(Ctx, Root, TEXT("placeId"), TEXT(""), Place.PlaceId);

	// revision
	const TSharedPtr<FJsonObject>* RevisionObj = nullptr;
	if (Root->TryGetObjectField(TEXT("revision"), RevisionObj))
	{
		RequireNoUnknownKeys(
			Ctx, *RevisionObj,
			{ TEXT("number"), TEXT("notes"), TEXT("publishedAt"), TEXT("state") },
			TEXT("/revision"));

		double Number = 0.0;
		if (RequireNumber(Ctx, *RevisionObj, TEXT("number"), TEXT("/revision"), Number))
		{
			Place.Revision.Number = static_cast<int32>(Number);
			if (Place.Revision.Number < 1)
			{
				Ctx.Fail(TEXT("/revision/number"), TEXT("must be at least 1"));
			}
		}
		OptionalString(*RevisionObj, TEXT("notes"), Place.Revision.Notes);
		if (FMantlePlaceParser::CodePointLength(Place.Revision.Notes) > FMantlePlaceLimits::MaxRevisionNotesLength)
		{
			Ctx.Fail(TEXT("/revision/notes"), FString::Printf(TEXT("is longer than %d characters"), FMantlePlaceLimits::MaxRevisionNotesLength));
		}
		OptionalString(*RevisionObj, TEXT("publishedAt"), Place.Revision.PublishedAt);

		FString State;
		if (RequireString(Ctx, *RevisionObj, TEXT("state"), TEXT("/revision"), State)
			&& !MantleParseRevisionState(State, Place.Revision.State))
		{
			Ctx.Fail(TEXT("/revision/state"), FString::Printf(TEXT("'%s' is not a valid revision state"), *State));
		}
	}
	else
	{
		Ctx.Fail(TEXT("/revision"), TEXT("required object is missing"));
	}

	// metadata
	const TSharedPtr<FJsonObject>* MetadataObj = nullptr;
	if (Root->TryGetObjectField(TEXT("metadata"), MetadataObj))
	{
		RequireNoUnknownKeys(
			Ctx, *MetadataObj,
			{ TEXT("name"), TEXT("description"), TEXT("authorId"), TEXT("createdAt"),
			  TEXT("updatedAt"), TEXT("thumbnail") },
			TEXT("/metadata"));

		if (RequireString(Ctx, *MetadataObj, TEXT("name"), TEXT("/metadata"), Place.Metadata.Name)
			&& FMantlePlaceParser::CodePointLength(Place.Metadata.Name) > FMantlePlaceLimits::MaxPlaceNameLength)
		{
			Ctx.Fail(TEXT("/metadata/name"), FString::Printf(TEXT("is longer than %d characters"), FMantlePlaceLimits::MaxPlaceNameLength));
		}
		RequireString(Ctx, *MetadataObj, TEXT("authorId"), TEXT("/metadata"), Place.Metadata.AuthorId);
		OptionalString(*MetadataObj, TEXT("description"), Place.Metadata.Description);
		OptionalString(*MetadataObj, TEXT("createdAt"), Place.Metadata.CreatedAt);
		OptionalString(*MetadataObj, TEXT("updatedAt"), Place.Metadata.UpdatedAt);
		OptionalString(*MetadataObj, TEXT("thumbnail"), Place.Metadata.Thumbnail);

		if (Place.Metadata.Name.IsEmpty())
		{
			Ctx.Fail(TEXT("/metadata/name"), TEXT("must not be empty"));
		}
	}
	else
	{
		Ctx.Fail(TEXT("/metadata"), TEXT("required object is missing"));
	}

	// settings — every field optional, defaults already set
	const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
	if (Root->TryGetObjectField(TEXT("settings"), SettingsObj))
	{
		RequireNoUnknownKeys(
			Ctx, *SettingsObj,
			{ TEXT("maxPlayers"), TEXT("gravity"), TEXT("skybox"), TEXT("ambientLight"),
			  TEXT("respawnMode"), TEXT("streamingDistance"), TEXT("allowCommunityServers"),
			  TEXT("allowPrivateServers"), TEXT("avatarHeight"), TEXT("environment"), TEXT("carjacking"), TEXT("water") },
			TEXT("/settings"));

		double MaxPlayers = Place.Settings.MaxPlayers;
		OptionalNumber(*SettingsObj, TEXT("maxPlayers"), MaxPlayers);
		Place.Settings.MaxPlayers = static_cast<int32>(MaxPlayers);
		// Capped at 100, below Iris's hard 128-connection-per-ReplicationSystem ceiling
		// (Engine/Source/Runtime/Net/Iris/Private/Iris/ReplicationSystem/ReplicationConnections.h,
		// not exposed to config, with a reported Linux server crash at the 128th connection).
		RangeCheck(Ctx, MaxPlayers, 1.0, 100.0, TEXT("/settings/maxPlayers"));

		OptionalNumber(*SettingsObj, TEXT("gravity"), Place.Settings.Gravity);
		OptionalString(*SettingsObj, TEXT("skybox"), Place.Settings.Skybox);
		OptionalString(*SettingsObj, TEXT("respawnMode"), Place.Settings.RespawnMode);
		OptionalBool(*SettingsObj, TEXT("allowCommunityServers"), Place.Settings.bAllowCommunityServers);
		OptionalBool(*SettingsObj, TEXT("allowPrivateServers"), Place.Settings.bAllowPrivateServers);
		// "player" (the default) or "uniform", case-exact as the schema's enum is.
		if (const TSharedPtr<FJsonValue> AvatarHeight = (*SettingsObj)->TryGetField(TEXT("avatarHeight")))
		{
			FString AvatarHeightValue;
			if (AvatarHeight->Type == EJson::String)
			{
				AvatarHeightValue = AvatarHeight->AsString();
			}
			const bool bPlayer = AvatarHeightValue.Equals(TEXT("player"), ESearchCase::CaseSensitive);
			const bool bUniform = AvatarHeightValue.Equals(TEXT("uniform"), ESearchCase::CaseSensitive);
			if (!bPlayer && !bUniform)
			{
				Ctx.Fail(TEXT("/settings/avatarHeight"), TEXT("must be \"player\" or \"uniform\""));
			}
			Place.Settings.bUniformAvatarHeight = bUniform;
		}
		// Typed, so a string such as "off" is refused rather than read as a boolean.
		ReadStrictOptionalBool(Ctx, *SettingsObj, TEXT("carjacking"), TEXT("/settings"), Place.Settings.bCarjacking);
		ReadEnvironmentSettings(Ctx, *SettingsObj, TEXT("/settings"), Place.Settings.Environment);
		ReadWaterZone(Ctx, *SettingsObj, TEXT("/settings"), Place.Settings.Water);

		// Deprecated: still accepted, so existing place files parse, but nothing reads it
		// at runtime and the exporter no longer writes it.
		if ((*SettingsObj)->HasField(TEXT("ambientLight")))
		{
			ReadColor(Ctx, *SettingsObj, TEXT("ambientLight"), TEXT("/settings"), Place.Settings.AmbientLight);
		}

		// Deprecated: still accepted and still checked, so existing place files parse the
		// same, but nothing reads it at runtime and the exporter no longer writes it.
		OptionalNumber(*SettingsObj, TEXT("streamingDistance"), Place.Settings.StreamingDistance);
		RequirePositive(Ctx, Place.Settings.StreamingDistance, TEXT("/settings/streamingDistance"));
	}
	else
	{
		Ctx.Fail(TEXT("/settings"), TEXT("required object is missing"));
	}

	// localization — optional, inert string tables consumed only by Mantle's UI host.
	const TSharedPtr<FJsonObject>* LocalizationObj = nullptr;
	if (Root->TryGetObjectField(TEXT("localization"), LocalizationObj))
	{
		RequireNoUnknownKeys(Ctx, *LocalizationObj, {TEXT("defaultLocale"), TEXT("translations")}, TEXT("/localization"));
		OptionalString(*LocalizationObj, TEXT("defaultLocale"), Place.Localization.DefaultLocale);
		if (!IsLocale(Place.Localization.DefaultLocale)) Ctx.Fail(TEXT("/localization/defaultLocale"), TEXT("expected a language tag such as en or fr-CA"));
		const TSharedPtr<FJsonObject>* Translations = nullptr;
		if ((*LocalizationObj)->TryGetObjectField(TEXT("translations"), Translations))
		{
			if ((*Translations)->Values.Num() > 32) Ctx.Fail(TEXT("/localization/translations"), TEXT("must contain at most 32 locales"));
			for (const auto& LocalePair : (*Translations)->Values)
			{
				const FString Locale(LocalePair.Key.ToView());
				const FString LocalePath = TEXT("/localization/translations/") + Locale;
				if (!IsLocale(Locale)) Ctx.Fail(LocalePath, TEXT("invalid locale"));
				const TSharedPtr<FJsonObject>* Table = nullptr;
				if (!LocalePair.Value.IsValid() || !LocalePair.Value->TryGetObject(Table)) { Ctx.Fail(LocalePath, TEXT("expected an object")); continue; }
				if ((*Table)->Values.Num() > 4096) Ctx.Fail(LocalePath, TEXT("must contain at most 4096 strings"));
				for (const auto& TextPair : (*Table)->Values)
				{
					const FString Key(TextPair.Key.ToView()); FString Text;
					if (!IsLocalizationKey(Key)) Ctx.Fail(LocalePath + TEXT("/") + Key, TEXT("invalid localization key"));
					if (!TextPair.Value.IsValid() || !TextPair.Value->TryGetString(Text) || FMantlePlaceParser::CodePointLength(Text) > 2048) Ctx.Fail(LocalePath + TEXT("/") + Key, TEXT("expected a string no longer than 2048 characters"));
					else Place.Localization.Translations.FindOrAdd(Locale).Add(Key, MoveTemp(Text));
				}
			}
		}
	}

	// audience — optional. Catalogue metadata the world never reads, but a place is
	// refused for a malformed one all the same, as the backend and validate.py refuse it.
	const TSharedPtr<FJsonValue> AudienceValue = Root->TryGetField(TEXT("audience"));
	if (AudienceValue.IsValid())
	{
		const TSharedPtr<FJsonObject>* AudienceObj = nullptr;
		if (!AudienceValue->TryGetObject(AudienceObj))
		{
			Ctx.Fail(TEXT("/audience"), TEXT("expected an object"));
		}
		else
		{
			RequireNoUnknownKeys(Ctx, *AudienceObj, { TEXT("maturity"), TEXT("platforms") }, TEXT("/audience"));

			if ((*AudienceObj)->HasField(TEXT("maturity")))
			{
				FString Maturity;
				if (RequireString(Ctx, *AudienceObj, TEXT("maturity"), TEXT("/audience"), Maturity)
					&& !MantleParseMaturity(Maturity, Place.Audience.Maturity))
				{
					Ctx.Fail(TEXT("/audience/maturity"), FString::Printf(TEXT("'%s' is not a valid maturity rating"), *Maturity));
				}
			}

			if ((*AudienceObj)->HasField(TEXT("platforms")))
			{
				const TArray<TSharedPtr<FJsonValue>>* PlatformsArray = nullptr;
				if (!(*AudienceObj)->TryGetArrayField(TEXT("platforms"), PlatformsArray))
				{
					Ctx.Fail(TEXT("/audience/platforms"), TEXT("expected an array"));
				}
				else if (PlatformsArray->Num() == 0)
				{
					// Absent means every platform; an empty list would mean none.
					Ctx.Fail(TEXT("/audience/platforms"), TEXT("must list at least one platform, or be left out for every platform"));
				}
				else
				{
					for (int32 i = 0; i < PlatformsArray->Num(); ++i)
					{
						const FString PlatformPath = Index(TEXT("/audience/platforms"), i);
						FString Name;
						EMantlePlatform Platform = EMantlePlatform::PC;
						if (!(*PlatformsArray)[i].IsValid() || (*PlatformsArray)[i]->Type != EJson::String
							|| !(*PlatformsArray)[i]->TryGetString(Name))
						{
							Ctx.Fail(PlatformPath, TEXT("expected a string"));
						}
						else if (!MantleParsePlatform(Name, Platform))
						{
							Ctx.Fail(PlatformPath, FString::Printf(TEXT("'%s' is not a valid platform"), *Name));
						}
						else if (Place.Audience.Platforms.Contains(Platform))
						{
							Ctx.Fail(PlatformPath, FString::Printf(TEXT("'%s' is listed more than once"), *Name));
						}
						else
						{
							Place.Audience.Platforms.Add(Platform);
						}
					}
				}
			}
		}
	}

	// scriptApi — the API version a place with any scripts was exported against. Optional
	// when there are no scripts; required below once Place.Scripts is known.
	const TSharedPtr<FJsonObject>* ScriptApiObj = nullptr;
	const bool bHasScriptApi = Root->TryGetObjectField(TEXT("scriptApi"), ScriptApiObj);
	if (bHasScriptApi)
	{
		RequireNoUnknownKeys(Ctx, *ScriptApiObj, { TEXT("major"), TEXT("minor") }, TEXT("/scriptApi"));

		double Major = 0.0;
		if (RequireNumber(Ctx, *ScriptApiObj, TEXT("major"), TEXT("/scriptApi"), Major))
		{
			Place.ScriptApi.Major = static_cast<int32>(Major);
			if (Place.ScriptApi.Major < 0)
			{
				Ctx.Fail(TEXT("/scriptApi/major"), TEXT("must not be negative"));
			}
		}
		double Minor = 0.0;
		if (RequireNumber(Ctx, *ScriptApiObj, TEXT("minor"), TEXT("/scriptApi"), Minor))
		{
			Place.ScriptApi.Minor = static_cast<int32>(Minor);
			if (Place.ScriptApi.Minor < 0)
			{
				Ctx.Fail(TEXT("/scriptApi/minor"), TEXT("must not be negative"));
			}
		}
	}

	// assets
	const TArray<TSharedPtr<FJsonValue>>* AssetsArray = nullptr;
	if (Root->TryGetArrayField(TEXT("assets"), AssetsArray))
	{
		for (int32 i = 0; i < AssetsArray->Num(); ++i)
		{
			const FString Path = Index(TEXT("/assets"), i);
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!(*AssetsArray)[i]->TryGetObject(Obj))
			{
				Ctx.Fail(Path, TEXT("expected an object"));
				continue;
			}

			RequireNoUnknownKeys(Ctx, *Obj, { TEXT("id"), TEXT("kind"), TEXT("hash"), TEXT("bytes") }, Path);

			FMantleAssetRef Asset;
			RequireString(Ctx, *Obj, TEXT("id"), Path, Asset.Id);
			RequireString(Ctx, *Obj, TEXT("hash"), Path, Asset.Hash);

			FString Kind;
			if (RequireString(Ctx, *Obj, TEXT("kind"), Path, Kind) && !MantleParseAssetKind(Kind, Asset.Kind))
			{
				Ctx.Fail(Join(Path, TEXT("kind")), FString::Printf(TEXT("'%s' is not a valid asset kind"), *Kind));
			}

			double Bytes = 0.0;
			if (RequireNumber(Ctx, *Obj, TEXT("bytes"), Path, Bytes))
			{
				Asset.Bytes = static_cast<int64>(Bytes);
				if (Asset.Bytes < 0)
				{
					Ctx.Fail(Join(Path, TEXT("bytes")), TEXT("must not be negative"));
				}
			}

			Place.Assets.Add(MoveTemp(Asset));
		}
	}
	else
	{
		Ctx.Fail(TEXT("/assets"), TEXT("required array is missing"));
	}

	// entities
	const TArray<TSharedPtr<FJsonValue>>* EntitiesArray = nullptr;
	if (Root->TryGetArrayField(TEXT("entities"), EntitiesArray))
	{
		for (int32 i = 0; i < EntitiesArray->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!(*EntitiesArray)[i]->TryGetObject(Obj))
			{
				Ctx.Fail(Index(TEXT("/entities"), i), TEXT("expected an object"));
				continue;
			}
			ParseEntity(Ctx, *Obj, Index(TEXT("/entities"), i), Place);
		}
	}
	else
	{
		Ctx.Fail(TEXT("/entities"), TEXT("required array is missing"));
	}

	// The place-wide InstancedMesh total, which the schema cannot express: validate.py's
	// check_instance_total and the backend's instanceTotal, reported at the entity that crosses it.
	{
		int32 InstanceTotal = 0;
		for (int32 i = 0; i < Place.Entities.Num(); ++i)
		{
			const FMantleInstancedMeshProps* Instanced = Place.Entities[i].Props.TryGet<FMantleInstancedMeshProps>();
			if (!Instanced)
			{
				continue;
			}
			InstanceTotal += Instanced->Instances.Num();
			if (InstanceTotal > FMantlePlaceLimits::MaxInstancesPerPlace)
			{
				Ctx.Fail(
					Join(Join(Index(TEXT("/entities"), i), TEXT("properties")), TEXT("instances")),
					FString::Printf(TEXT("brings the place to %d instances; a place holds at most %d"),
						InstanceTotal, FMantlePlaceLimits::MaxInstancesPerPlace));
				break;
			}
		}
	}

	// One Terrain per place: validate.py's check_terrain_rules and the backend's terrainTotal,
	// reported at the type of the first Terrain past it.
	{
		int32 Terrains = 0;
		for (int32 i = 0; i < Place.Entities.Num(); ++i)
		{
			if (Place.Entities[i].Type != EMantleEntityType::Terrain)
			{
				continue;
			}
			if (++Terrains == FMantlePlaceLimits::MaxTerrainsPerPlace + 1)
			{
				Ctx.Fail(
					Join(Index(TEXT("/entities"), i), TEXT("type")),
					FString::Printf(TEXT("is Terrain number %d; a place may have at most %d"), Terrains, FMantlePlaceLimits::MaxTerrainsPerPlace));
			}
		}
	}

	// A Terrain's grass maps are Textures: the kind the manifest gives an id it lists, which the schema
	// cannot say. validate.py and the backend check the same, at the same path.
	for (int32 i = 0; i < Place.Entities.Num(); ++i)
	{
		const FMantleTerrainProps* Terrain = Place.Entities[i].Props.TryGet<FMantleTerrainProps>();
		for (int32 GrassIndex = 0; Terrain && GrassIndex < Terrain->Grass.Num(); ++GrassIndex)
		{
			const FString& MapId = Terrain->Grass[GrassIndex].Map;
			const FMantleAssetRef* Listed = Place.Assets.FindByPredicate([&MapId](const FMantleAssetRef& Asset)
			{
				return Asset.Id.Equals(MapId, ESearchCase::CaseSensitive);
			});
			if (Listed && Listed->Kind != EMantleAssetKind::Texture)
			{
				const FString GrassPath = Index(Join(Join(Index(TEXT("/entities"), i), TEXT("properties")), TEXT("grass")), GrassIndex);
				Ctx.Fail(Join(GrassPath, TEXT("map")), FString::Printf(TEXT("'%s' is listed in the manifest as something other than a texture"), *MapId));
			}
		}
	}

	// scripts — optional
	const TArray<TSharedPtr<FJsonValue>>* ScriptsArray = nullptr;
	if (Root->TryGetArrayField(TEXT("scripts"), ScriptsArray))
	{
		for (int32 i = 0; i < ScriptsArray->Num(); ++i)
		{
			const FString Path = Index(TEXT("/scripts"), i);
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!(*ScriptsArray)[i]->TryGetObject(Obj))
			{
				Ctx.Fail(Path, TEXT("expected an object"));
				continue;
			}

			RequireNoUnknownKeys(
				Ctx, *Obj,
				{ TEXT("id"), TEXT("name"), TEXT("path"), TEXT("side"), TEXT("language"), TEXT("source"), TEXT("authorSource"), TEXT("authoredAs") },
				Path);

			FMantleScript Script;
			RequireString(Ctx, *Obj, TEXT("id"), Path, Script.Id);
			if (RequireString(Ctx, *Obj, TEXT("name"), Path, Script.Name))
			{
				const int32 NameLength = FMantlePlaceParser::CodePointLength(Script.Name);
				if (NameLength == 0 || NameLength > FMantlePlaceLimits::MaxScriptNameLength)
				{
					Ctx.Fail(Path + TEXT("/name"), FString::Printf(TEXT("must be 1 to %d characters long"), FMantlePlaceLimits::MaxScriptNameLength));
				}
			}

			FString TopFolder;
			bool bPathOk = RequireString(Ctx, *Obj, TEXT("path"), Path, Script.Path);
			if (bPathOk && FMantlePlaceParser::CodePointLength(Script.Path) > FMantlePlaceLimits::MaxScriptPathLength)
			{
				Ctx.Fail(Path + TEXT("/path"), FString::Printf(TEXT("is longer than %d characters"), FMantlePlaceLimits::MaxScriptPathLength));
				bPathOk = false;
			}
			if (bPathOk && !IsScriptPath(Script.Path, TopFolder))
			{
				Ctx.Fail(
					Path + TEXT("/path"),
					TEXT("must start with exactly Server/, Client/ or Shared/ followed by a path of letters, digits, '_', '.' and '-' in non-empty folders"));
				bPathOk = false;
			}

			FString Side;
			bool bSideOk = false;
			if (RequireString(Ctx, *Obj, TEXT("side"), Path, Side))
			{
				if (MantleParseScriptSide(Side, Script.Side))
				{
					bSideOk = true;
				}
				else
				{
					Ctx.Fail(Path + TEXT("/side"), FString::Printf(TEXT("'%s' is not a valid script side"), *Side));
				}
			}
			if (bPathOk && bSideOk && !Side.Equals(ExpectedSideForTopFolder(TopFolder), ESearchCase::CaseSensitive))
			{
				Ctx.Fail(
					Path + TEXT("/side"),
					FString::Printf(TEXT("'%s' does not match the '%s/' path"), *Side, *TopFolder));
			}
			FString Language;
			if (RequireString(Ctx, *Obj, TEXT("language"), Path, Language)
				&& !MantleParseScriptLanguage(Language, Script.Language))
			{
				Ctx.Fail(
					Path + TEXT("/language"),
					FString::Printf(TEXT("'%s' is not a valid script language"), *Language));
			}
			if (RequireString(Ctx, *Obj, TEXT("source"), Path, Script.Source)
				&& !FMantlePlaceLimits::IsScriptSourceId(Script.Source))
			{
				Ctx.Fail(
					Path + TEXT("/source"),
					TEXT("expected src_ followed by 32 lowercase hexadecimal characters"));
			}
			if ((*Obj)->HasField(TEXT("authorSource")))
			{
				if (RequireString(Ctx, *Obj, TEXT("authorSource"), Path, Script.AuthorSource)
					&& !FMantlePlaceLimits::IsScriptSourceId(Script.AuthorSource))
				{
					Ctx.Fail(
						Path + TEXT("/authorSource"),
						TEXT("expected src_ followed by 32 lowercase hexadecimal characters"));
				}
			}
			FString AuthoredAs;
			if ((*Obj)->HasField(TEXT("authoredAs")))
			{
				if (RequireString(Ctx, *Obj, TEXT("authoredAs"), Path, AuthoredAs))
				{
					if (AuthoredAs == TEXT("graph"))
					{
						Script.bAuthoredAsGraph = true;
					}
					else
					{
						Ctx.Fail(Path + TEXT("/authoredAs"), TEXT("must be 'graph'"));
					}
				}
			}

			const bool bGraphPath = Script.Path.EndsWith(TEXT(".graph"), ESearchCase::CaseSensitive);
			if (bGraphPath && !Script.bAuthoredAsGraph)
			{
				Ctx.Fail(Path + TEXT("/authoredAs"), TEXT("required when path ends in .graph"));
			}
			if (Script.bAuthoredAsGraph)
			{
				if (!bGraphPath)
				{
					Ctx.Fail(Path + TEXT("/path"), TEXT("a graph-authored script must end in .graph"));
				}
				if (Script.Language != EMantleScriptLanguage::Luau)
				{
					Ctx.Fail(Path + TEXT("/language"), TEXT("a graph-authored script executes as luau"));
				}
				if (Script.AuthorSource.IsEmpty())
				{
					Ctx.Fail(Path + TEXT("/authorSource"), TEXT("a graph-authored script requires its graph source"));
				}
			}
			if (Script.Language == EMantleScriptLanguage::TypeScript && Script.AuthorSource.IsEmpty())
			{
				Ctx.Fail(Path + TEXT("/authorSource"), TEXT("TypeScript requires its original author source"));
			}
			else if (Script.Language != EMantleScriptLanguage::TypeScript && !Script.bAuthoredAsGraph && !Script.AuthorSource.IsEmpty())
			{
				Ctx.Fail(Path + TEXT("/authorSource"), TEXT("only TypeScript or graph-authored scripts may carry separate author source"));
			}
			Place.Scripts.Add(MoveTemp(Script));
		}
	}

	// scriptApi is meaningless, and not required, for a place with no scripts -- but is
	// mandatory once any script is present, since it records what the exporter targeted.
	if (Place.Scripts.Num() > 0 && !bHasScriptApi)
	{
		Ctx.Fail(TEXT("/scriptApi"), TEXT("required when scripts is non-empty"));
	}

	// Case-insensitive path collisions -- not expressible as a per-item schema rule, the same
	// reason the checks below (dangling parent, unknown script id) are done by hand rather
	// than declaratively. Two paths that differ only by case are ambiguous on a
	// case-insensitive filesystem, so the second occurrence is rejected outright.
	{
		TMap<FString, int32> FirstIndexByLowercasePath;
		for (int32 i = 0; i < Place.Scripts.Num(); ++i)
		{
			const FString& ScriptPath = Place.Scripts[i].Path;
			if (ScriptPath.IsEmpty())
			{
				continue;
			}
			const FString Lowercase = ScriptPath.ToLower();
			if (const int32* FirstIndex = FirstIndexByLowercasePath.Find(Lowercase))
			{
				Ctx.Fail(
					Index(TEXT("/scripts"), i) + TEXT("/path"),
					FString::Printf(
						TEXT("'%s' differs only in case from scripts/%d's path; script paths must be case-insensitively unique"),
						*ScriptPath,
						*FirstIndex));
			}
			else
			{
				FirstIndexByLowercasePath.Add(Lowercase, i);
			}
		}
	}

	// uiDocuments — optional. RmlUi documents and styles, carried by reference exactly like
	// scripts: an id, a path under UI/ and a src_ id. Ids are unique, paths unique ignoring case.
	// An image (.png, .jpg) carries its Texture asset's id instead of a src_ id.
	if (Root->HasField(TEXT("uiDocuments")))
	{
		const TArray<TSharedPtr<FJsonValue>>* UIDocumentsArray = nullptr;
		if (!Root->TryGetArrayField(TEXT("uiDocuments"), UIDocumentsArray))
		{
			Ctx.Fail(TEXT("/uiDocuments"), TEXT("expected an array"));
		}
		else
		{
			if (UIDocumentsArray->Num() > FMantlePlaceLimits::MaxUIEntries)
			{
				Ctx.Fail(
					TEXT("/uiDocuments"),
					FString::Printf(TEXT("has %d entries; at most %d are allowed"), UIDocumentsArray->Num(), FMantlePlaceLimits::MaxUIEntries));
			}
			// An image names one of the place's Texture assets (parsed above).
			// Matched case-sensitively, like every id (FString's own comparison ignores case).
			const auto IsTextureAsset = [&Place](const FString& Id)
			{
				return Place.Assets.ContainsByPredicate([&Id](const FMantleAssetRef& Asset)
				{
					return Asset.Kind == EMantleAssetKind::Texture && Asset.Id.Equals(Id, ESearchCase::CaseSensitive);
				});
			};
			int32 TextCount = 0;
			int32 ImageCount = 0;
			TMap<FString, int32> FirstIndexByUIDocumentId;
			TMap<FString, int32> FirstIndexByLowercaseUIPath;
			for (int32 i = 0; i < UIDocumentsArray->Num(); ++i)
			{
				const FString Path = Index(TEXT("/uiDocuments"), i);
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				if (!(*UIDocumentsArray)[i].IsValid() || !(*UIDocumentsArray)[i]->TryGetObject(Obj))
				{
					Ctx.Fail(Path, TEXT("expected an object"));
					continue;
				}
				RequireNoUnknownKeys(Ctx, *Obj, { TEXT("id"), TEXT("path"), TEXT("source"), TEXT("asset") }, Path);
				// Exactly one of source (a document or stylesheet) and asset (an image).
				const bool bHasAsset = (*Obj)->HasField(TEXT("asset"));
				const bool bHasSource = (*Obj)->HasField(TEXT("source"));
				if (bHasAsset && bHasSource)
				{
					Ctx.Fail(Path, TEXT("has both a source and an asset; a document or stylesheet has a source, an image an asset"));
				}

				FMantleUIDocument Document;
				if (RequireString(Ctx, *Obj, TEXT("id"), Path, Document.Id))
				{
					if (!FMantlePlaceLimits::IsUIDocumentId(Document.Id))
					{
						Ctx.Fail(Path + TEXT("/id"), TEXT("expected uid_ followed by 1 to 40 of a-z, 0-9 and '_'"));
					}
					else if (const int32* FirstId = FirstIndexByUIDocumentId.Find(Document.Id))
					{
						Ctx.Fail(Path + TEXT("/id"), FString::Printf(TEXT("'%s' repeats uiDocuments/%d's id"), *Document.Id, *FirstId));
					}
					else
					{
						FirstIndexByUIDocumentId.Add(Document.Id, i);
					}
				}
				if (RequireString(Ctx, *Obj, TEXT("path"), Path, Document.Path))
				{
					if (FMantlePlaceParser::CodePointLength(Document.Path) > FMantlePlaceLimits::MaxUIDocumentPathLength)
					{
						Ctx.Fail(Path + TEXT("/path"), FString::Printf(TEXT("is longer than %d characters"), FMantlePlaceLimits::MaxUIDocumentPathLength));
					}
					else if (bHasAsset ? !FMantlePlaceLimits::IsUIImagePath(Document.Path) : !FMantlePlaceLimits::IsUIDocumentPath(Document.Path))
					{
						Ctx.Fail(
							Path + TEXT("/path"),
							bHasAsset
								? TEXT("an image's path must start with exactly UI/ followed by non-empty folders of letters, digits, '_', '.' and '-', no '..', and end in .png or .jpg")
								: TEXT("must start with exactly UI/ followed by non-empty folders of letters, digits, '_', '.' and '-', no '..', and end in .rml or .rcss"));
					}
					else
					{
						// TMap<FString> keys ignore case already; lowering keeps the intent plain.
						const FString Lowercase = Document.Path.ToLower();
						if (const int32* FirstPath = FirstIndexByLowercaseUIPath.Find(Lowercase))
						{
							Ctx.Fail(
								Path + TEXT("/path"),
								FString::Printf(
									TEXT("'%s' differs only in case from uiDocuments/%d's path; UI document paths must be case-insensitively unique"),
									*Document.Path,
									*FirstPath));
						}
						else
						{
							FirstIndexByLowercaseUIPath.Add(Lowercase, i);
						}
					}
				}
				if (bHasAsset)
				{
					if (RequireString(Ctx, *Obj, TEXT("asset"), Path, Document.Asset) && !IsTextureAsset(Document.Asset))
					{
						Ctx.Fail(Path + TEXT("/asset"), FString::Printf(TEXT("'%s' is not one of the place's Texture assets"), *Document.Asset));
					}
					if (++ImageCount == FMantlePlaceLimits::MaxUIImages + 1)
					{
						Ctx.Fail(TEXT("/uiDocuments"), FString::Printf(TEXT("has more than %d images; at most %d are allowed"),
							FMantlePlaceLimits::MaxUIImages, FMantlePlaceLimits::MaxUIImages));
					}
				}
				else
				{
					if (RequireString(Ctx, *Obj, TEXT("source"), Path, Document.Source)
						&& !FMantlePlaceLimits::IsScriptSourceId(Document.Source))
					{
						Ctx.Fail(Path + TEXT("/source"), TEXT("expected src_ followed by 32 lowercase hexadecimal characters"));
					}
					if (++TextCount == FMantlePlaceLimits::MaxUIDocuments + 1)
					{
						Ctx.Fail(TEXT("/uiDocuments"), FString::Printf(TEXT("has more than %d documents and stylesheets; at most %d are allowed"),
							FMantlePlaceLimits::MaxUIDocuments, FMantlePlaceLimits::MaxUIDocuments));
					}
				}
				Place.UIDocuments.Add(MoveTemp(Document));
			}
		}
	}

	// storage — optional. The library items the place pins, by name: {name, item, version}.
	// The rules the schema states (the count, the name and item patterns, unique names, a version of at least 1)
	// are FMantleStorage::CheckEntries', shared with the exporter and the editor validator.
	if (Root->HasField(TEXT("storage")))
	{
		const TArray<TSharedPtr<FJsonValue>>* StorageArray = nullptr;
		if (!Root->TryGetArrayField(TEXT("storage"), StorageArray))
		{
			Ctx.Fail(TEXT("/storage"), TEXT("expected an array"));
		}
		else
		{
			for (int32 i = 0; i < StorageArray->Num(); ++i)
			{
				const FString Path = Index(TEXT("/storage"), i);
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				// An entry that is not an object still takes its index, so every later path matches the file.
				FMantleStorageEntry Entry;
				if (!(*StorageArray)[i].IsValid() || !(*StorageArray)[i]->TryGetObject(Obj))
				{
					Ctx.Fail(Path, TEXT("expected an object"));
					Place.Storage.Add(MoveTemp(Entry));
					continue;
				}
				RequireNoUnknownKeys(Ctx, *Obj, { TEXT("name"), TEXT("item"), TEXT("version"), TEXT("settings") }, Path);
				RequireString(Ctx, *Obj, TEXT("name"), Path, Entry.Name);
				RequireString(Ctx, *Obj, TEXT("item"), Path, Entry.Item);
				double EntryVersion = 0.0;
				if (RequireNumber(Ctx, *Obj, TEXT("version"), Path, EntryVersion))
				{
					ReadBoundedInt(Ctx, EntryVersion, 1, MAX_int32, Join(Path, TEXT("version")), Entry.Version);
				}
				// A script package's settings, {key: text}. Only the shape is the parser's (the key rules and the
				// value length are CheckEntries' below); whether the item is a script is unknown here.
				if ((*Obj)->HasField(TEXT("settings")))
				{
					const FString SettingsPath = Join(Path, TEXT("settings"));
					const TSharedPtr<FJsonObject>* SettingsObject = nullptr;
					if (!(*Obj)->TryGetObjectField(TEXT("settings"), SettingsObject) || !SettingsObject || !SettingsObject->IsValid())
					{
						Ctx.Fail(SettingsPath, TEXT("must be an object of text values"));
					}
					else
					{
						for (const auto& Pair : (*SettingsObject)->Values)
						{
							// UE 5.8 keys FJsonObject::Values by TSharedString, not FString.
							const FString Key(Pair.Key.ToView());
							FString Value;
							if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String || !Pair.Value->TryGetString(Value))
							{
								Ctx.Fail(Join(SettingsPath, Key), TEXT("must be a string"));
								continue;
							}
							if (Entry.Settings.Contains(Key))
							{
								// The map's keys ignore case; two spellings of one name are never two settings.
								Ctx.Fail(Join(SettingsPath, Key), TEXT("repeats another setting's name in another letter case"));
								continue;
							}
							Entry.Settings.Add(Key, MoveTemp(Value));
						}
					}
				}
				Place.Storage.Add(MoveTemp(Entry));
			}
			for (const FMantleStorageProblem& Problem : FMantleStorage::CheckEntries(Place.Storage))
			{
				Ctx.Fail(Problem.Path, Problem.Message);
			}
		}
	}

	// A VehicleSpawn's storageItem names one of the entries just read (matched case-sensitively, like every name).
	for (int32 i = 0; i < Place.Entities.Num(); ++i)
	{
		const FMantleVehicleSpawnProps* Spawn = Place.Entities[i].Props.TryGet<FMantleVehicleSpawnProps>();
		if (!Spawn || Spawn->StorageItem.IsEmpty() || !FMantlePlaceLimits::IsStorageName(Spawn->StorageItem))
		{
			continue; // not set, or refused already where it was read
		}
		const bool bDeclared = Place.Storage.ContainsByPredicate([Spawn](const FMantleStorageEntry& Entry)
		{
			return Entry.Name.Equals(Spawn->StorageItem, ESearchCase::CaseSensitive);
		});
		if (!bDeclared)
		{
			Ctx.Fail(
				Join(Join(Index(TEXT("/entities"), i), TEXT("properties")), TEXT("storageItem")),
				FString::Printf(TEXT("'%s' is not one of the place's storage names"), *Spawn->StorageItem));
		}
	}

	// A WeaponSpawn's storageItem likewise.
	for (int32 i = 0; i < Place.Entities.Num(); ++i)
	{
		const FMantleWeaponSpawnProps* Spawn = Place.Entities[i].Props.TryGet<FMantleWeaponSpawnProps>();
		if (!Spawn || Spawn->StorageItem.IsEmpty() || !FMantlePlaceLimits::IsStorageName(Spawn->StorageItem))
		{
			continue;
		}
		const bool bDeclared = Place.Storage.ContainsByPredicate([Spawn](const FMantleStorageEntry& Entry)
		{
			return Entry.Name.Equals(Spawn->StorageItem, ESearchCase::CaseSensitive);
		});
		if (!bDeclared)
		{
			Ctx.Fail(
				Join(Join(Index(TEXT("/entities"), i), TEXT("properties")), TEXT("storageItem")),
				FString::Printf(TEXT("'%s' is not one of the place's storage names"), *Spawn->StorageItem));
		}
	}

	// A ScriptPoint's storageItem likewise. Whether that entry is a script is the backend's to say at publish,
	// and FMantleStorage::Apply turns a point off when the resolve says otherwise.
	for (int32 i = 0; i < Place.Entities.Num(); ++i)
	{
		const FMantleScriptPointProps* Point = Place.Entities[i].Props.TryGet<FMantleScriptPointProps>();
		if (!Point || Point->StorageItem.IsEmpty() || !FMantlePlaceLimits::IsStorageName(Point->StorageItem))
		{
			continue;
		}
		const bool bDeclared = Place.Storage.ContainsByPredicate([Point](const FMantleStorageEntry& Entry)
		{
			return Entry.Name.Equals(Point->StorageItem, ESearchCase::CaseSensitive);
		});
		// A local -MantlePlace file may name a package that exists only beside it on disk (ParseFromFile's DiskScriptPackages).
		const bool bOnDisk = DiskScriptPackages.ContainsByPredicate([Point](const FString& DiskPackage)
		{
			return DiskPackage.Equals(Point->StorageItem, ESearchCase::CaseSensitive);
		});
		if (!bDeclared && !bOnDisk)
		{
			Ctx.Fail(
				Join(Join(Index(TEXT("/entities"), i), TEXT("properties")), TEXT("storageItem")),
				FString::Printf(TEXT("'%s' is not one of the place's storage names"), *Point->StorageItem));
		}
	}

	// Referential integrity — a parent that does not exist is a broken place.
	// A duplicate id would make the builder silently keep only one of them.
	TSet<FString> EntityIds;
	TMap<FString, const FMantleEntity*> EntityById;
	for (int32 i = 0; i < Place.Entities.Num(); ++i)
	{
		const FMantleEntity& Entity = Place.Entities[i];
		bool bAlreadyUsed = false;
		EntityIds.Add(Entity.Id, &bAlreadyUsed);
		if (bAlreadyUsed)
		{
			Ctx.Fail(
				Join(Index(TEXT("/entities"), i), TEXT("id")),
				FString::Printf(TEXT("'%s' is used by more than one entity; entity ids must be unique"), *Entity.Id));
		}
		else
		{
			EntityById.Add(Entity.Id, &Entity);
		}
	}

	// seats — optional. Sit spots (#142): {entity, position, yaw, style}, each on an entity of this place.
	if (Root->HasField(TEXT("seats")))
	{
		const TArray<TSharedPtr<FJsonValue>>* SeatsArray = nullptr;
		if (!Root->TryGetArrayField(TEXT("seats"), SeatsArray))
		{
			Ctx.Fail(TEXT("/seats"), TEXT("expected an array"));
		}
		else
		{
			if (SeatsArray->Num() > FMantlePlaceLimits::MaxSeatsPerPlace)
			{
				Ctx.Fail(TEXT("/seats"), FString::Printf(TEXT("has %d seats; a place may have at most %d"),
					SeatsArray->Num(), FMantlePlaceLimits::MaxSeatsPerPlace));
			}
			for (int32 i = 0; i < SeatsArray->Num() && i < FMantlePlaceLimits::MaxSeatsPerPlace; ++i)
			{
				const FString Path = Index(TEXT("/seats"), i);
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				FMantleSeat Seat;
				if (!(*SeatsArray)[i].IsValid() || !(*SeatsArray)[i]->TryGetObject(Obj))
				{
					Ctx.Fail(Path, TEXT("expected an object"));
					Place.Seats.Add(MoveTemp(Seat));
					continue;
				}
				RequireNoUnknownKeys(Ctx, *Obj, { TEXT("entity"), TEXT("position"), TEXT("yaw"), TEXT("style") }, Path);
				if (RequireString(Ctx, *Obj, TEXT("entity"), Path, Seat.Entity) && !EntityIds.Contains(Seat.Entity))
				{
					Ctx.Fail(Join(Path, TEXT("entity")), FString::Printf(TEXT("'%s' does not name any entity in this place"), *Seat.Entity));
				}
				TArray<double> Position;
				if (ReadNumberArray(Ctx, *Obj, TEXT("position"), 3, Path, Position))
				{
					for (int32 Axis = 0; Axis < 3; ++Axis)
					{
						RangeCheck(Ctx, Position[Axis], -FMantlePlaceLimits::MaxSeatOffset, FMantlePlaceLimits::MaxSeatOffset,
							Index(Join(Path, TEXT("position")), Axis));
					}
					Seat.Position = FVector(Position[0], Position[1], Position[2]);
				}
				double Yaw = 0.0;
				if (RequireNumber(Ctx, *Obj, TEXT("yaw"), Path, Yaw))
				{
					RangeCheck(Ctx, Yaw, -360.0, 360.0, Join(Path, TEXT("yaw")));
					Seat.Yaw = Yaw;
				}
				FString Style;
				if (RequireString(Ctx, *Obj, TEXT("style"), Path, Style) && !MantleParseSeatStyle(Style, Seat.Style))
				{
					Ctx.Fail(Join(Path, TEXT("style")), FString::Printf(TEXT("'%s' is not a seat style; the only one is 'bench'"), *Style));
				}
				Place.Seats.Add(MoveTemp(Seat));
			}
		}
	}

	for (int32 i = 0; i < Place.Entities.Num(); ++i)
	{
		const FMantleEntity& Entity = Place.Entities[i];
		if (!Entity.Parent.IsEmpty() && !EntityIds.Contains(Entity.Parent))
		{
			Ctx.Fail(
				Join(Index(TEXT("/entities"), i), TEXT("parent")),
				FString::Printf(TEXT("'%s' does not name any entity in this place"), *Entity.Parent));
		}
	}

	// A part's anchor and highlight are entities of this place, as validate.py's check_interactable_rules and the
	// backend's interactableReferences hold.
	for (int32 i = 0; i < Place.Entities.Num(); ++i)
	{
		const FMantleEntity& Entity = Place.Entities[i];
		if (!Entity.Interactable)
		{
			continue;
		}
		const FString PartsPath = Join(Join(Index(TEXT("/entities"), i), TEXT("interactable")), TEXT("parts"));
		for (int32 PartIndex = 0; PartIndex < Entity.Interactable->Parts.Num(); ++PartIndex)
		{
			const FMantleInteractablePartSpec& Part = Entity.Interactable->Parts[PartIndex];
			const FString PartPath = Index(PartsPath, PartIndex);
			if (!Part.AnchorEntity.IsEmpty() && !EntityIds.Contains(Part.AnchorEntity))
			{
				Ctx.Fail(Join(PartPath, TEXT("anchor")),
					FString::Printf(TEXT("'%s' does not name any entity in this place"), *Part.AnchorEntity));
			}
			for (int32 H = 0; H < Part.HighlightEntities.Num(); ++H)
			{
				if (!EntityIds.Contains(Part.HighlightEntities[H]))
				{
					Ctx.Fail(Index(Join(PartPath, TEXT("highlight")), H),
						FString::Printf(TEXT("'%s' does not name any entity in this place"), *Part.HighlightEntities[H]));
				}
			}
		}
	}

	// A parent chain that comes back to itself has no root to attach to. Each entity is
	// walked once, as in the backend's entityIntegrity: a walk per entity is quadratic, and one
	// long chain in a large place would stall every server and client that loads it.
	enum class EWalk : uint8 { OnWalk, Finished };
	TMap<const FMantleEntity*, EWalk> WalkState;
	WalkState.Reserve(EntityById.Num());
	TSet<const FMantleEntity*> OnLoop;
	TArray<const FMantleEntity*> Walk;
	for (const TPair<FString, const FMantleEntity*>& Start : EntityById)
	{
		Walk.Reset();
		const FMantleEntity* Current = Start.Value;
		bool bLooped = false;
		while (Current)
		{
			if (const EWalk* State = WalkState.Find(Current))
			{
				bLooped = *State == EWalk::OnWalk;
				break;
			}
			WalkState.Add(Current, EWalk::OnWalk);
			Walk.Add(Current);
			const FMantleEntity* const* Parent =
				Current->Parent.IsEmpty() ? nullptr : EntityById.Find(Current->Parent);
			Current = Parent ? *Parent : nullptr;
		}
		if (bLooped)
		{
			// The walk came back to itself: everything from Current onwards is the loop.
			for (int32 WalkIndex = Walk.IndexOfByKey(Current); WalkIndex < Walk.Num(); ++WalkIndex)
			{
				OnLoop.Add(Walk[WalkIndex]);
			}
		}
		for (const FMantleEntity* Walked : Walk)
		{
			WalkState[Walked] = EWalk::Finished;
		}
	}
	if (OnLoop.Num() > 0)
	{
		for (int32 i = 0; i < Place.Entities.Num(); ++i)
		{
			const FMantleEntity* const* Owner = EntityById.Find(Place.Entities[i].Id);
			if (Owner && OnLoop.Contains(*Owner))
			{
				Ctx.Fail(
					Join(Index(TEXT("/entities"), i), TEXT("parent")),
					FString::Printf(
						TEXT("'%s' is its own ancestor; parents must not form a loop"), *Place.Entities[i].Id));
			}
		}
	}

	// Crowd budgets span entities, so the schema can't hold them: the backend and
	// validate.py apply the same two totals. The error sits on the spawner that goes over.
	int32 CrowdSpawners = 0;
	int32 CrowdNpcs = 0;
	for (int32 i = 0; i < Place.Entities.Num(); ++i)
	{
		const FMantleCrowdSpawnerProps* Crowd = Place.Entities[i].Props.TryGet<FMantleCrowdSpawnerProps>();
		if (!Crowd)
		{
			continue;
		}
		if (++CrowdSpawners == FMantlePlaceLimits::MaxCrowdSpawnersPerPlace + 1)
		{
			Ctx.Fail(
				Join(Index(TEXT("/entities"), i), TEXT("type")),
				FString::Printf(TEXT("is CrowdSpawner number %d; a place may have at most %d"),
					CrowdSpawners, FMantlePlaceLimits::MaxCrowdSpawnersPerPlace));
		}
		const int32 Before = CrowdNpcs;
		CrowdNpcs += Crowd->Count;
		if (Before <= FMantlePlaceLimits::MaxCrowdNpcsPerPlace && CrowdNpcs > FMantlePlaceLimits::MaxCrowdNpcsPerPlace)
		{
			Ctx.Fail(
				Join(Join(Index(TEXT("/entities"), i), TEXT("properties")), TEXT("count")),
				FString::Printf(TEXT("brings the place to %d crowd NPCs; a place may have at most %d"),
					CrowdNpcs, FMantlePlaceLimits::MaxCrowdNpcsPerPlace));
		}
		// A point of interest is another entity of this place, as validate.py and the backend's crowdPoints hold.
		for (int32 PointIndex = 0; PointIndex < Crowd->PointsOfInterest.Num(); ++PointIndex)
		{
			const FString& Target = Crowd->PointsOfInterest[PointIndex].EntityId;
			if (Target.IsEmpty())
			{
				continue; // missing: already reported where it was read
			}
			const FString PointPath = Join(Index(Join(Join(Index(TEXT("/entities"), i), TEXT("properties")), TEXT("pointsOfInterest")), PointIndex),
				TEXT("entity"));
			const FMantleEntity* const* Found = EntityById.Find(Target);
			if (Target.Equals(Place.Entities[i].Id, ESearchCase::CaseSensitive))
			{
				Ctx.Fail(PointPath, FString::Printf(TEXT("'%s' is this spawner; a point of interest names another entity"), *Target));
			}
			else if (!Found || !(*Found)->Id.Equals(Target, ESearchCase::CaseSensitive))
			{
				Ctx.Fail(PointPath, FString::Printf(TEXT("'%s' does not name any entity in this place"), *Target));
			}
		}
	}

	// Physics bodies span entities too: validate.py's check_physics_total and the backend's
	// physicsTotal apply the same cap, reported at the physics block that crosses it.
	{
		int32 PhysicsObjects = 0;
		for (int32 i = 0; i < Place.Entities.Num(); ++i)
		{
			if (MantlePhysicsOf(Place.Entities[i]) && ++PhysicsObjects == FMantlePlaceLimits::MaxPhysicsObjectsPerPlace + 1)
			{
				Ctx.Fail(
					Join(Join(Index(TEXT("/entities"), i), TEXT("properties")), TEXT("physics")),
					FString::Printf(TEXT("brings the place to %d physics objects; a place holds at most %d"),
						PhysicsObjects, FMantlePlaceLimits::MaxPhysicsObjectsPerPlace));
			}
		}
	}

	// The VehicleSpawn budget, which the schema states in prose: the backend and validate.py
	// report the same spawn, at its type.
	int32 VehicleSpawns = 0;
	for (int32 i = 0; i < Place.Entities.Num(); ++i)
	{
		if (Place.Entities[i].Type == EMantleEntityType::VehicleSpawn
			&& ++VehicleSpawns == FMantlePlaceLimits::MaxVehicleSpawnsPerPlace + 1)
		{
			Ctx.Fail(
				Join(Index(TEXT("/entities"), i), TEXT("type")),
				FString::Printf(TEXT("is VehicleSpawn number %d; a place may have at most %d"),
					VehicleSpawns, FMantlePlaceLimits::MaxVehicleSpawnsPerPlace));
		}
	}

	// The WeaponSpawn budget, reported like VehicleSpawn's.
	int32 WeaponSpawns = 0;
	for (int32 i = 0; i < Place.Entities.Num(); ++i)
	{
		if (Place.Entities[i].Type == EMantleEntityType::WeaponSpawn
			&& ++WeaponSpawns == FMantlePlaceLimits::MaxWeaponSpawnsPerPlace + 1)
		{
			Ctx.Fail(
				Join(Index(TEXT("/entities"), i), TEXT("type")),
				FString::Printf(TEXT("is WeaponSpawn number %d; a place may have at most %d"),
					WeaponSpawns, FMantlePlaceLimits::MaxWeaponSpawnsPerPlace));
		}
	}

	// The ScriptPoint budget, reported like WeaponSpawn's.
	int32 ScriptPoints = 0;
	for (int32 i = 0; i < Place.Entities.Num(); ++i)
	{
		if (Place.Entities[i].Type == EMantleEntityType::ScriptPoint
			&& ++ScriptPoints == FMantlePlaceLimits::MaxScriptPoints + 1)
		{
			Ctx.Fail(
				Join(Index(TEXT("/entities"), i), TEXT("type")),
				FString::Printf(TEXT("is ScriptPoint number %d; a place may have at most %d"),
					ScriptPoints, FMantlePlaceLimits::MaxScriptPoints));
		}
	}

	// The Effect budget, stated in the schema's prose like VehicleSpawn's and reported the same way.
	int32 Effects = 0;
	for (int32 i = 0; i < Place.Entities.Num(); ++i)
	{
		if (Place.Entities[i].Type == EMantleEntityType::Effect && ++Effects == FMantlePlaceLimits::MaxEffectsPerPlace + 1)
		{
			Ctx.Fail(
				Join(Index(TEXT("/entities"), i), TEXT("type")),
				FString::Printf(TEXT("is Effect number %d; a place may have at most %d"), Effects, FMantlePlaceLimits::MaxEffectsPerPlace));
		}
	}

	// The ReflectionProbe budget, stated in the schema's prose and reported the same way as the Effect's.
	int32 ReflectionProbes = 0;
	for (int32 i = 0; i < Place.Entities.Num(); ++i)
	{
		if (Place.Entities[i].Type == EMantleEntityType::ReflectionProbe && ++ReflectionProbes == FMantlePlaceLimits::MaxReflectionProbesPerPlace + 1)
		{
			Ctx.Fail(
				Join(Index(TEXT("/entities"), i), TEXT("type")),
				FString::Printf(TEXT("is ReflectionProbe number %d; a place may have at most %d"),
					ReflectionProbes, FMantlePlaceLimits::MaxReflectionProbesPerPlace));
		}
	}

	// The Text budget (GhzGarage/mantle#724), stated in the schema's prose and reported the same way as the Effect's.
	int32 Texts = 0;
	for (int32 i = 0; i < Place.Entities.Num(); ++i)
	{
		if (Place.Entities[i].Type == EMantleEntityType::Text && ++Texts == FMantlePlaceLimits::MaxTextsPerPlace + 1)
		{
			Ctx.Fail(
				Join(Index(TEXT("/entities"), i), TEXT("type")),
				FString::Printf(TEXT("is Text number %d; a place may have at most %d"), Texts, FMantlePlaceLimits::MaxTextsPerPlace));
		}
	}

	// The WaterBody budgets (GhzGarage/mantle#997), stated in the schema's prose and reported the same way as the Effect's: the
	// body that crosses the count, and the body that takes the spline points past their total, each at its type.
	int32 WaterBodies = 0;
	int32 WaterSplinePoints = 0;
	bool bWaterPointsReported = false;
	for (int32 i = 0; i < Place.Entities.Num(); ++i)
	{
		const FMantleWaterBodyProps* Water = Place.Entities[i].Props.TryGet<FMantleWaterBodyProps>();
		if (!Water)
		{
			continue;
		}
		if (++WaterBodies == FMantlePlaceLimits::MaxWaterBodiesPerPlace + 1)
		{
			Ctx.Fail(
				Join(Index(TEXT("/entities"), i), TEXT("type")),
				FString::Printf(TEXT("is WaterBody number %d; a place may have at most %d"), WaterBodies, FMantlePlaceLimits::MaxWaterBodiesPerPlace));
		}
		WaterSplinePoints += Water->SplinePoints.Num();
		if (WaterSplinePoints > FMantlePlaceLimits::MaxWaterSplinePointsPerPlace && !bWaterPointsReported)
		{
			bWaterPointsReported = true;
			Ctx.Fail(
				Join(Index(TEXT("/entities"), i), TEXT("type")),
				FString::Printf(TEXT("takes the place's water spline points to %d; a place may have at most %d"),
					WaterSplinePoints, FMantlePlaceLimits::MaxWaterSplinePointsPerPlace));
		}
	}

	// The place-wide Scatter instance total, which the schema cannot express: validate.py's
	// check_scatter_rules and the backend's scatterTotal, reported at the density of the Scatter that
	// crosses it. A Scatter already refused on its own is not counted.
	{
		int64 ScatterTotal = 0;
		for (int32 i = 0; i < Place.Entities.Num(); ++i)
		{
			const FMantleScatterParams* Scatter = Place.Entities[i].Props.TryGet<FMantleScatterParams>();
			FString Why;
			const int64 Count = Scatter ? FMantleScatter::CountFor(*Scatter, Why) : -1;
			if (Count < 0)
			{
				continue;
			}
			ScatterTotal += Count;
			if (ScatterTotal > FMantlePlaceLimits::MaxScatterInstancesPerPlace)
			{
				Ctx.Fail(
					Join(Join(Index(TEXT("/entities"), i), TEXT("properties")), TEXT("density")),
					FString::Printf(TEXT("brings the place to %lld scatter instances; a place holds at most %d"),
						static_cast<long long>(ScatterTotal), FMantlePlaceLimits::MaxScatterInstancesPerPlace));
				break;
			}
		}
	}

	// Constraints span entities: entityA and entityB each name a StaticMesh of this place with
	// a physics block, never the same one, and a place holds at most MaxConstraintsPerPlace, as
	// validate.py's check_constraint_rules and the backend's constraintRules hold them.
	int32 Constraints = 0;
	for (int32 i = 0; i < Place.Entities.Num(); ++i)
	{
		const FMantleConstraintProps* Joint = Place.Entities[i].Props.TryGet<FMantleConstraintProps>();
		if (!Joint)
		{
			continue;
		}
		const FString PropsPath = Join(Index(TEXT("/entities"), i), TEXT("properties"));
		if (++Constraints == FMantlePlaceLimits::MaxConstraintsPerPlace + 1)
		{
			Ctx.Fail(
				Join(Index(TEXT("/entities"), i), TEXT("type")),
				FString::Printf(TEXT("is Constraint number %d; a place may have at most %d"), Constraints, FMantlePlaceLimits::MaxConstraintsPerPlace));
		}
		for (int32 Side = 0; Side < 2; ++Side)
		{
			const FString& Id = Side == 0 ? Joint->EntityA : Joint->EntityB;
			if (Id.IsEmpty())
			{
				continue; // entityA missing is reported where it was read; entityB absent is the world
			}
			const FString KeyPath = Join(PropsPath, Side == 0 ? TEXT("entityA") : TEXT("entityB"));
			const FMantleEntity* const* Found = EntityById.Find(Id);
			const bool bSelf = Side == 1 && Id.Equals(Joint->EntityA, ESearchCase::CaseSensitive);
			if (bSelf)
			{
				Ctx.Fail(KeyPath, FString::Printf(
					TEXT("'%s' is entityA too; join two different entities, or leave entityB out to join entityA to the world"), *Id));
			}
			else if (!Found || !(*Found)->Id.Equals(Id, ESearchCase::CaseSensitive))
			{
				Ctx.Fail(KeyPath, FString::Printf(TEXT("'%s' does not name any entity in this place"), *Id));
			}
			else if (!MantlePhysicsOf(**Found))
			{
				Ctx.Fail(KeyPath, FString::Printf(
					TEXT("'%s' is not a StaticMesh with a physics block; a Constraint joins physics entities"), *Id));
			}
		}
	}

	Result.Errors = MoveTemp(Ctx.Errors);
	Result.bSuccess = Result.Errors.Num() == 0;
	return Result;
}

FMantleParseResult FMantlePlaceParser::ParseFromFile(const FString& FilePath)
{
	return ParseFromFile(FilePath, TArray<FString>(), /*bAcceptDraftVehicleIds*/ false);
}

FMantleParseResult FMantlePlaceParser::ParseFromFile(const FString& FilePath, const TArray<FString>& DiskScriptPackages, const bool bAcceptDraftVehicleIds)
{
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *FilePath))
	{
		FMantleParseResult Result;
		Result.Errors.Add(FMantleParseError{ TEXT(""), FString::Printf(TEXT("could not read '%s'"), *FilePath) });
		return Result;
	}
	return ParseFromString(Json, DiskScriptPackages, bAcceptDraftVehicleIds);
}
