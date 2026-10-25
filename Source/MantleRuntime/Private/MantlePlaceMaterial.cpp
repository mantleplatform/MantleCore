// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceMaterial.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture.h"
#include "MantlePlaceAssets.h"
#include "MantleRuntime.h"
#include "MantleTrace.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantlePlaceMaterialPrivate
{
	constexpr const TCHAR* MaterialsFolder = TEXT("/Game/Mantle/Materials");

	/** Permutations known to be missing, so a missing asset is looked for once, not per material. */
	bool GPermutationMissing[FMantlePlaceMaterials::PermutationCount] = {};
	bool GPermutationChecked[FMantlePlaceMaterials::PermutationCount] = {};
	bool GWarnedMissing = false;
	bool GWarnedCarPaintMissing = false;
	bool GWarnedExtendedMissing = false;
	bool GWarnedVariantMissing = false;
	bool GWarnedWaterMissing = false;

	/** The schema's normalScale maximum and a uvTransforms rotation's bound either way (#471). */
	constexpr double MaxNormalScale = 10.0;
	constexpr double MaxUVRotation = 3.1416;

	/** #533's bounds: a uvPanning component either way, wind's intensity and speed, worldAligned's two. */
	constexpr double MaxUVPanning = 10.0;
	constexpr double MaxWind = 4.0;
	constexpr double MinTileSizeCm = 1.0;
	constexpr double MaxTileSizeCm = 100000.0;
	constexpr double MinSharpness = 1.0;
	constexpr double MaxSharpness = 16.0;

	/** #533's variants, in permutation order (13.., 17.., 19..), then #628's (21, 22, and water's 23, 24). */
	const EMantlePlaceExtendedVariant Variants[] = {
		EMantlePlaceExtendedVariant::Wind, EMantlePlaceExtendedVariant::WorldAligned, EMantlePlaceExtendedVariant::LayerBlend,
		EMantlePlaceExtendedVariant::Foliage, EMantlePlaceExtendedVariant::FoliageWind,
		EMantlePlaceExtendedVariant::Water, EMantlePlaceExtendedVariant::WaterNormal2,
	};

	/**
	 * The permutation at Index (its asset at Path), loaded once and kept; null when the content isn't
	 * there, remembered so the package is looked for once per process.
	 */
	UMaterialInterface* LoadPermutationAt(const int32 Index, const FSoftObjectPath& Path)
	{
		if (Index < 0 || Index >= FMantlePlaceMaterials::PermutationCount || GPermutationMissing[Index])
		{
			return nullptr;
		}
		UMaterialInterface* Loaded = Cast<UMaterialInterface>(Path.ResolveObject());
		if (!Loaded && (GPermutationChecked[Index] || FPackageName::DoesPackageExist(Path.GetLongPackageName())))
		{
			// Synchronous, once per permutation per process: the first uploaded material of that kind pays it (#126).
			TRACE_CPUPROFILER_EVENT_SCOPE(MantlePlaceMaterials_LoadPermutation);
			MANTLE_TRACE_SCOPE_DETAIL("MantleAssetLoad", Path.ToString());
			Loaded = LoadObject<UMaterialInterface>(nullptr, *Path.ToString(), nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		GPermutationChecked[Index] = true;
		GPermutationMissing[Index] = Loaded == nullptr;
		return Loaded;
	}

	bool FailMaterial(FMantlePlaceMaterialDesc& Out, FString* OutError, const FString& Error)
	{
		Out = FMantlePlaceMaterialDesc();
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("material: %s"), *Error);
		}
		return false;
	}

	bool FailSlot(FString* OutError, const FString& Error)
	{
		if (OutError)
		{
			*OutError = Error;
		}
		return false;
	}

	/** Strict UTF-8: no overlong forms, no surrogates, nothing past U+10FFFF. */
	bool IsValidUtf8(const TConstArrayView<uint8> Bytes)
	{
		int32 Index = 0;
		const int32 Count = Bytes.Num();
		while (Index < Count)
		{
			const uint8 Lead = Bytes[Index];
			if (Lead < 0x80)
			{
				++Index;
				continue;
			}
			int32 Extra = 0;
			uint32 CodePoint = 0;
			uint32 Minimum = 0;
			if ((Lead & 0xE0) == 0xC0) { Extra = 1; CodePoint = Lead & 0x1F; Minimum = 0x80; }
			else if ((Lead & 0xF0) == 0xE0) { Extra = 2; CodePoint = Lead & 0x0F; Minimum = 0x800; }
			else if ((Lead & 0xF8) == 0xF0) { Extra = 3; CodePoint = Lead & 0x07; Minimum = 0x10000; }
			else { return false; }
			if (Index + Extra >= Count)
			{
				return false;
			}
			for (int32 Continuation = 1; Continuation <= Extra; ++Continuation)
			{
				const uint8 Byte = Bytes[Index + Continuation];
				if ((Byte & 0xC0) != 0x80)
				{
					return false;
				}
				CodePoint = (CodePoint << 6) | (Byte & 0x3F);
			}
			if (CodePoint < Minimum || CodePoint > 0x10FFFF || (CodePoint >= 0xD800 && CodePoint <= 0xDFFF))
			{
				return false;
			}
			Index += Extra + 1;
		}
		return true;
	}

	/** A number in [Min, Max]; false (with the reason) otherwise. */
	bool ReadRangedNumber(const TSharedPtr<FJsonValue>& Value, const double Min, const double Max, double& Out, FString& OutWhy)
	{
		if (!Value.IsValid() || Value->Type != EJson::Number)
		{
			OutWhy = TEXT("must be a number");
			return false;
		}
		Out = Value->AsNumber();
		if (!FMath::IsFinite(Out) || Out < Min || Out > Max)
		{
			OutWhy = FString::Printf(TEXT("%g is outside [%g, %g]"), Out, Min, Max);
			return false;
		}
		return true;
	}

	/** An array of exactly Length numbers, each in [Min, Max]. */
	bool ReadRangedArray(const TSharedPtr<FJsonValue>& Value, const int32 Length, const double Min, const double Max,
		double (&Out)[4], FString& OutWhy)
	{
		if (!Value.IsValid() || Value->Type != EJson::Array)
		{
			OutWhy = FString::Printf(TEXT("must be an array of %d numbers"), Length);
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
		if (Items.Num() != Length)
		{
			OutWhy = FString::Printf(TEXT("must be an array of %d numbers"), Length);
			return false;
		}
		for (int32 Index = 0; Index < Length; ++Index)
		{
			FString Why;
			if (!ReadRangedNumber(Items[Index], Min, Max, Out[Index], Why))
			{
				OutWhy = FString::Printf(TEXT("item %d %s"), Index, *Why);
				return false;
			}
		}
		return true;
	}

	const TCHAR* const TopLevelKeys[] = {
		TEXT("schemaVersion"), TEXT("alphaMode"), TEXT("alphaCutoff"), TEXT("doubleSided"), TEXT("baseColorFactor"),
		TEXT("metallicFactor"), TEXT("roughnessFactor"), TEXT("emissiveFactor"), TEXT("emissiveStrength"), TEXT("uvScale"),
		TEXT("uvOffset"), TEXT("textures"), TEXT("shading"), TEXT("carPaint"), TEXT("uvTransforms"), TEXT("normalScale"),
		TEXT("occlusionStrength"), TEXT("occlusionUV"), TEXT("vertexColor"), TEXT("uvPanning"), TEXT("wind"),
		TEXT("worldAligned"), TEXT("layerBlend"), TEXT("foliage"), TEXT("water"),
	};

	const TCHAR* const CarPaintKeys[] = {
		TEXT("flakeDensity"), TEXT("flakeSize"), TEXT("flakeTint"), TEXT("clearCoat"), TEXT("clearCoatRoughness"),
	};

	/** #471's and #533's keys: they build on M_MantlePlace_Extended, so a car-paint document refuses them. */
	const TCHAR* const FidelityKeys[] = {
		TEXT("uvTransforms"), TEXT("normalScale"), TEXT("occlusionStrength"), TEXT("occlusionUV"), TEXT("vertexColor"),
		TEXT("uvPanning"), TEXT("wind"), TEXT("worldAligned"), TEXT("layerBlend"), TEXT("foliage"), TEXT("water"),
	};

	/** #533's keys with permutations of their own, in the order a second one is refused. */
	const TCHAR* const VariantKeys[] = {
		TEXT("wind"), TEXT("worldAligned"), TEXT("layerBlend"),
	};

	const TCHAR* const UVTransformKeys[] = {
		TEXT("offset"), TEXT("scale"), TEXT("rotation"),
	};

	const TCHAR* const WindKeys[] = {
		TEXT("intensity"), TEXT("speed"), TEXT("weight"),
	};

	const TCHAR* const WorldAlignedKeys[] = {
		TEXT("tileSizeCm"), TEXT("sharpness"),
	};

	const TCHAR* const LayerBlendKeys[] = {
		TEXT("channel"), TEXT("textures"), TEXT("baseColorFactor"),
	};

	const TCHAR* const FoliageKeys[] = {
		TEXT("tint"), TEXT("strength"), TEXT("vertexColorWeight"),
	};

	/** The schema's foliage.strength maximum (#628). */
	constexpr double MaxFoliageStrength = 4.0;

	const TCHAR* const WaterKeys[] = {
		TEXT("scattering"), TEXT("absorption"), TEXT("phaseG"), TEXT("colorScaleBehindWater"), TEXT("normal"), TEXT("normal2"),
	};

	const TCHAR* const WaterNormalKeys[] = {
		TEXT("space"), TEXT("tiling"), TEXT("worldSizeCm"), TEXT("panning"),
	};

	/** The schema's water normal worldSizeCm bounds (#628), and a tiling component's either way. */
	constexpr double MinWaterWorldSizeCm = 1.0;
	constexpr double MaxWaterWorldSizeCm = 100000.0;
	constexpr double MaxUVTiling = 1024.0;

	/** layerBlend.textures' keys, in EMantlePlaceTextureSlot order from LayerBaseColor. */
	const TCHAR* const LayerTextureKeys[] = {
		TEXT("baseColor"), TEXT("normal"), TEXT("orm"),
	};

	bool IsOneOf(const FString& Key, TConstArrayView<const TCHAR*> Keys)
	{
		for (const TCHAR* Known : Keys)
		{
			if (Key.Equals(Known, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	/** BC4's eight values for endpoints (Red0, Red1), per the format: eight-value mode when Red0 > Red1. */
	void BC4Palette(const uint8 Red0, const uint8 Red1, uint8 (&OutPalette)[8])
	{
		OutPalette[0] = Red0;
		OutPalette[1] = Red1;
		if (Red0 > Red1)
		{
			for (int32 Index = 2; Index < 8; ++Index)
			{
				OutPalette[Index] = static_cast<uint8>(((8 - Index) * Red0 + (Index - 1) * Red1 + 3) / 7);
			}
		}
		else
		{
			for (int32 Index = 2; Index < 6; ++Index)
			{
				OutPalette[Index] = static_cast<uint8>(((6 - Index) * Red0 + (Index - 1) * Red1 + 2) / 5);
			}
			OutPalette[6] = 0;
			OutPalette[7] = 255;
		}
	}

	/**
	 * One water normal layer (#628): water.normal, or water.normal2 with its texture (bSecond; OutTextureId). The space is
	 * uv (the default; a tiling, no worldSizeCm) or world (worldSizeCm required, no tiling). False, with the whole reason
	 * (path included), on anything else.
	 */
	bool ReadWaterNormal(const TSharedPtr<FJsonValue>& Field, const FString& Path, const bool bSecond,
		FMantlePlaceWaterNormal& Out, FString& OutTextureId, FString& OutWhy)
	{
		if (!Field.IsValid() || Field->Type != EJson::Object || !Field->AsObject().IsValid())
		{
			OutWhy = Path + TEXT(" must be an object");
			return false;
		}
		const TSharedPtr<FJsonObject> Layer = Field->AsObject();
		for (const auto& Pair : Layer->Values)
		{
			const FString Key(Pair.Key.ToView());
			if (!(bSecond && Key.Equals(TEXT("texture"), ESearchCase::CaseSensitive)) && !IsOneOf(Key, WaterNormalKeys))
			{
				OutWhy = FString::Printf(TEXT("%s/%s is not a water normal property"), *Path, *Key);
				return false;
			}
		}

		FMantlePlaceWaterNormal Result;
		double Values[4] = {};
		double Number = 0.0;
		FString Why;
		if (const TSharedPtr<FJsonValue> Space = Layer->TryGetField(TEXT("space")))
		{
			FString SpaceName;
			if (Space->Type != EJson::String || !Space->TryGetString(SpaceName))
			{
				OutWhy = Path + TEXT("/space must be uv or world");
				return false;
			}
			if (SpaceName.Equals(TEXT("uv"), ESearchCase::CaseSensitive)) { Result.Space = EMantlePlaceWaterSpace::Uv; }
			else if (SpaceName.Equals(TEXT("world"), ESearchCase::CaseSensitive)) { Result.Space = EMantlePlaceWaterSpace::World; }
			else
			{
				OutWhy = FString::Printf(TEXT("%s/space '%s' must be uv or world"), *Path, *SpaceName);
				return false;
			}
		}
		const bool bHasTiling = Layer->HasField(TEXT("tiling"));
		const bool bHasSize = Layer->HasField(TEXT("worldSizeCm"));
		if (bHasTiling)
		{
			if (!ReadRangedArray(Layer->TryGetField(TEXT("tiling")), 2, -MaxUVTiling, MaxUVTiling, Values, Why))
			{
				OutWhy = Path + TEXT("/tiling ") + Why;
				return false;
			}
			Result.Tiling = FVector2f(static_cast<float>(Values[0]), static_cast<float>(Values[1]));
		}
		if (bHasSize)
		{
			if (!ReadRangedNumber(Layer->TryGetField(TEXT("worldSizeCm")), MinWaterWorldSizeCm, MaxWaterWorldSizeCm, Number, Why))
			{
				OutWhy = Path + TEXT("/worldSizeCm ") + Why;
				return false;
			}
			Result.WorldSizeCm = static_cast<float>(Number);
		}
		if (Layer->HasField(TEXT("panning")))
		{
			if (!ReadRangedArray(Layer->TryGetField(TEXT("panning")), 2, -MaxUVPanning, MaxUVPanning, Values, Why))
			{
				OutWhy = Path + TEXT("/panning ") + Why;
				return false;
			}
			Result.Panning = FVector2f(static_cast<float>(Values[0]), static_cast<float>(Values[1]));
		}
		if (Result.Space == EMantlePlaceWaterSpace::World && !bHasSize)
		{
			OutWhy = Path + TEXT("/worldSizeCm is required when space is world");
			return false;
		}
		if (Result.Space == EMantlePlaceWaterSpace::World && bHasTiling)
		{
			OutWhy = Path + TEXT("/tiling is not allowed when space is world (use worldSizeCm)");
			return false;
		}
		if (Result.Space == EMantlePlaceWaterSpace::Uv && bHasSize)
		{
			OutWhy = Path + TEXT("/worldSizeCm is only allowed when space is world");
			return false;
		}
		if (bSecond)
		{
			const TSharedPtr<FJsonValue> Texture = Layer->TryGetField(TEXT("texture"));
			FString Id;
			if (!Texture.IsValid() || Texture->Type != EJson::String || !Texture->TryGetString(Id) || !FMantlePlaceAssets::IsUploadedId(Id))
			{
				OutWhy = Path + TEXT("/texture is required and must be an uploaded texture id (ast_u and 32 lowercase hex digits)");
				return false;
			}
			OutTextureId = Id;
		}
		Out = Result;
		return true;
	}

	/** Every mip's byte count is the format's for its size. */
	bool MipsFitFormat(const FMantleDecodedTexture& Texture)
	{
		for (const FMantleTextureMip& Mip : Texture.Mips)
		{
			if (Mip.Width <= 0 || Mip.Height <= 0
				|| Mip.Bytes.Num() != FMantleRuntimeTextureBuilder::ComputeMipBytes(Texture.Format, Mip.Width, Mip.Height))
			{
				return false;
			}
		}
		return Texture.Mips.Num() > 0;
	}
}

const TCHAR* FMantlePlaceMaterials::AlphaModeName(const EMantlePlaceAlphaMode Mode)
{
	switch (Mode)
	{
	case EMantlePlaceAlphaMode::Mask: return TEXT("mask");
	case EMantlePlaceAlphaMode::Blend: return TEXT("blend");
	case EMantlePlaceAlphaMode::Additive: return TEXT("additive");
	case EMantlePlaceAlphaMode::Opaque:
	default: return TEXT("opaque");
	}
}

const TCHAR* FMantlePlaceMaterials::SlotName(const EMantlePlaceTextureSlot Slot)
{
	switch (Slot)
	{
	case EMantlePlaceTextureSlot::Normal: return TEXT("normal");
	case EMantlePlaceTextureSlot::Orm: return TEXT("orm");
	case EMantlePlaceTextureSlot::Emissive: return TEXT("emissive");
	case EMantlePlaceTextureSlot::LayerBaseColor: return TEXT("layerBaseColor");
	case EMantlePlaceTextureSlot::LayerNormal: return TEXT("layerNormal");
	case EMantlePlaceTextureSlot::LayerOrm: return TEXT("layerOrm");
	case EMantlePlaceTextureSlot::WaterNormal2: return TEXT("waterNormal2");
	case EMantlePlaceTextureSlot::BaseColor:
	default: return TEXT("baseColor");
	}
}

EMantlePlaceTextureSlot FMantlePlaceMaterials::BaseSlotOf(const EMantlePlaceTextureSlot Slot)
{
	switch (Slot)
	{
	case EMantlePlaceTextureSlot::LayerBaseColor: return EMantlePlaceTextureSlot::BaseColor;
	case EMantlePlaceTextureSlot::LayerNormal: return EMantlePlaceTextureSlot::Normal;
	case EMantlePlaceTextureSlot::LayerOrm: return EMantlePlaceTextureSlot::Orm;
	case EMantlePlaceTextureSlot::WaterNormal2: return EMantlePlaceTextureSlot::Normal;
	default: return Slot;
	}
}

EMantleTextureRole FMantlePlaceMaterials::RoleForSlot(const EMantlePlaceTextureSlot Slot)
{
	switch (BaseSlotOf(Slot))
	{
	case EMantlePlaceTextureSlot::Normal: return EMantleTextureRole::Normal;
	case EMantlePlaceTextureSlot::Orm: return EMantleTextureRole::Data;
	case EMantlePlaceTextureSlot::BaseColor:
	case EMantlePlaceTextureSlot::Emissive:
	default: return EMantleTextureRole::Color;
	}
}

bool FMantlePlaceMaterials::ParseMaterial(const TConstArrayView<uint8> Bytes, FMantlePlaceMaterialDesc& Out, FString* OutError)
{
	using namespace MantlePlaceMaterialPrivate;

	Out = FMantlePlaceMaterialDesc();
	if (Bytes.Num() == 0)
	{
		return FailMaterial(Out, OutError, TEXT("the document is empty"));
	}
	if (Bytes.Num() > MaxDocumentBytes)
	{
		return FailMaterial(Out, OutError, FString::Printf(TEXT("the document is %d bytes; a material is at most %d"), Bytes.Num(), MaxDocumentBytes));
	}
	if (!IsValidUtf8(Bytes))
	{
		return FailMaterial(Out, OutError, TEXT("the document is not valid UTF-8"));
	}
	const auto Converted = StringCast<TCHAR>(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
	const FString Text = FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return FailMaterial(Out, OutError, TEXT("the document is not a JSON object"));
	}

	for (const auto& Pair : Root->Values)
	{
		const FString Key(Pair.Key.ToView());
		if (!IsOneOf(Key, TopLevelKeys))
		{
			return FailMaterial(Out, OutError, FString::Printf(TEXT("/%s is not a material property"), *Key));
		}
	}

	FMantlePlaceMaterialDesc Desc;
	FString Why;
	double Number = 0.0;

	const TSharedPtr<FJsonValue> Version = Root->TryGetField(TEXT("schemaVersion"));
	if (!Version.IsValid() || Version->Type != EJson::Number || Version->AsNumber() != 1.0)
	{
		return FailMaterial(Out, OutError, TEXT("/schemaVersion must be 1"));
	}

	const TSharedPtr<FJsonValue> Mode = Root->TryGetField(TEXT("alphaMode"));
	FString ModeText;
	if (!Mode.IsValid() || Mode->Type != EJson::String || !Mode->TryGetString(ModeText))
	{
		return FailMaterial(Out, OutError, TEXT("/alphaMode is required and must be opaque, mask, blend or additive"));
	}
	if (ModeText.Equals(TEXT("opaque"), ESearchCase::CaseSensitive)) { Desc.AlphaMode = EMantlePlaceAlphaMode::Opaque; }
	else if (ModeText.Equals(TEXT("mask"), ESearchCase::CaseSensitive)) { Desc.AlphaMode = EMantlePlaceAlphaMode::Mask; }
	else if (ModeText.Equals(TEXT("blend"), ESearchCase::CaseSensitive)) { Desc.AlphaMode = EMantlePlaceAlphaMode::Blend; }
	else if (ModeText.Equals(TEXT("additive"), ESearchCase::CaseSensitive)) { Desc.AlphaMode = EMantlePlaceAlphaMode::Additive; }
	else
	{
		return FailMaterial(Out, OutError, FString::Printf(TEXT("/alphaMode '%s' must be opaque, mask, blend or additive"), *ModeText));
	}

	if (const TSharedPtr<FJsonValue> Cutoff = Root->TryGetField(TEXT("alphaCutoff")))
	{
		if (Desc.AlphaMode != EMantlePlaceAlphaMode::Mask)
		{
			return FailMaterial(Out, OutError, TEXT("/alphaCutoff is only allowed when alphaMode is mask"));
		}
		if (!ReadRangedNumber(Cutoff, 0.0, 1.0, Number, Why))
		{
			return FailMaterial(Out, OutError, TEXT("/alphaCutoff ") + Why);
		}
		Desc.AlphaCutoff = static_cast<float>(Number);
	}

	const TSharedPtr<FJsonValue> DoubleSided = Root->TryGetField(TEXT("doubleSided"));
	if (!DoubleSided.IsValid() || DoubleSided->Type != EJson::Boolean)
	{
		return FailMaterial(Out, OutError, TEXT("/doubleSided is required and must be true or false"));
	}
	Desc.bDoubleSided = DoubleSided->AsBool();

	double Values[4] = {};
	if (const TSharedPtr<FJsonValue> Field = Root->TryGetField(TEXT("baseColorFactor")))
	{
		if (!ReadRangedArray(Field, 4, 0.0, 1.0, Values, Why))
		{
			return FailMaterial(Out, OutError, TEXT("/baseColorFactor ") + Why);
		}
		Desc.BaseColorFactor = FLinearColor(static_cast<float>(Values[0]), static_cast<float>(Values[1]), static_cast<float>(Values[2]), static_cast<float>(Values[3]));
	}
	if (const TSharedPtr<FJsonValue> Field = Root->TryGetField(TEXT("emissiveFactor")))
	{
		if (!ReadRangedArray(Field, 3, 0.0, 1.0, Values, Why))
		{
			return FailMaterial(Out, OutError, TEXT("/emissiveFactor ") + Why);
		}
		Desc.EmissiveFactor = FLinearColor(static_cast<float>(Values[0]), static_cast<float>(Values[1]), static_cast<float>(Values[2]), 1.0f);
	}
	if (const TSharedPtr<FJsonValue> Field = Root->TryGetField(TEXT("uvScale")))
	{
		if (!ReadRangedArray(Field, 2, -1024.0, 1024.0, Values, Why))
		{
			return FailMaterial(Out, OutError, TEXT("/uvScale ") + Why);
		}
		Desc.UVScale = FVector2f(static_cast<float>(Values[0]), static_cast<float>(Values[1]));
	}
	if (const TSharedPtr<FJsonValue> Field = Root->TryGetField(TEXT("uvOffset")))
	{
		if (!ReadRangedArray(Field, 2, -1024.0, 1024.0, Values, Why))
		{
			return FailMaterial(Out, OutError, TEXT("/uvOffset ") + Why);
		}
		Desc.UVOffset = FVector2f(static_cast<float>(Values[0]), static_cast<float>(Values[1]));
	}

	struct FScalarField
	{
		const TCHAR* Key;
		double Max;
		float* Target;
	};
	const FScalarField Scalars[] = {
		{ TEXT("metallicFactor"), 1.0, &Desc.MetallicFactor },
		{ TEXT("roughnessFactor"), 1.0, &Desc.RoughnessFactor },
		{ TEXT("emissiveStrength"), 100.0, &Desc.EmissiveStrength },
		{ TEXT("normalScale"), MaxNormalScale, &Desc.NormalScale },
		{ TEXT("occlusionStrength"), 1.0, &Desc.OcclusionStrength },
	};
	for (const FScalarField& Scalar : Scalars)
	{
		if (const TSharedPtr<FJsonValue> Field = Root->TryGetField(Scalar.Key))
		{
			if (!ReadRangedNumber(Field, 0.0, Scalar.Max, Number, Why))
			{
				return FailMaterial(Out, OutError, FString::Printf(TEXT("/%s %s"), Scalar.Key, *Why));
			}
			*Scalar.Target = static_cast<float>(Number);
		}
	}

	if (const TSharedPtr<FJsonValue> Field = Root->TryGetField(TEXT("shading")))
	{
		FString ShadingText;
		if (Field->Type != EJson::String || !Field->TryGetString(ShadingText))
		{
			return FailMaterial(Out, OutError, TEXT("/shading must be default or carPaint"));
		}
		if (ShadingText.Equals(TEXT("default"), ESearchCase::CaseSensitive)) { Desc.Shading = EMantlePlaceShading::Default; }
		else if (ShadingText.Equals(TEXT("carPaint"), ESearchCase::CaseSensitive)) { Desc.Shading = EMantlePlaceShading::CarPaint; }
		else
		{
			return FailMaterial(Out, OutError, FString::Printf(TEXT("/shading '%s' must be default or carPaint"), *ShadingText));
		}
	}
	if (const TSharedPtr<FJsonValue> Field = Root->TryGetField(TEXT("carPaint")))
	{
		if (Desc.Shading != EMantlePlaceShading::CarPaint)
		{
			return FailMaterial(Out, OutError, TEXT("/carPaint is only allowed when shading is carPaint"));
		}
		if (Field->Type != EJson::Object || !Field->AsObject().IsValid())
		{
			return FailMaterial(Out, OutError, TEXT("/carPaint must be an object"));
		}
		const TSharedPtr<FJsonObject> Finish = Field->AsObject();
		for (const auto& Pair : Finish->Values)
		{
			const FString Key(Pair.Key.ToView());
			if (!IsOneOf(Key, CarPaintKeys))
			{
				return FailMaterial(Out, OutError, FString::Printf(TEXT("/carPaint/%s is not a car-paint property"), *Key));
			}
		}
		if (const TSharedPtr<FJsonValue> Tint = Finish->TryGetField(TEXT("flakeTint")))
		{
			if (!ReadRangedArray(Tint, 3, 0.0, 1.0, Values, Why))
			{
				return FailMaterial(Out, OutError, TEXT("/carPaint/flakeTint ") + Why);
			}
			Desc.CarPaint.FlakeTint = FLinearColor(static_cast<float>(Values[0]), static_cast<float>(Values[1]), static_cast<float>(Values[2]), 1.0f);
		}
		const FScalarField FinishScalars[] = {
			{ TEXT("flakeDensity"), 1.0, &Desc.CarPaint.FlakeDensity },
			{ TEXT("clearCoat"), 1.0, &Desc.CarPaint.ClearCoat },
			{ TEXT("clearCoatRoughness"), 1.0, &Desc.CarPaint.ClearCoatRoughness },
		};
		for (const FScalarField& Scalar : FinishScalars)
		{
			if (const TSharedPtr<FJsonValue> Value = Finish->TryGetField(Scalar.Key))
			{
				if (!ReadRangedNumber(Value, 0.0, Scalar.Max, Number, Why))
				{
					return FailMaterial(Out, OutError, FString::Printf(TEXT("/carPaint/%s %s"), Scalar.Key, *Why));
				}
				*Scalar.Target = static_cast<float>(Number);
			}
		}
		if (const TSharedPtr<FJsonValue> Size = Finish->TryGetField(TEXT("flakeSize")))
		{
			if (!ReadRangedNumber(Size, 0.01, 1.0, Number, Why))
			{
				return FailMaterial(Out, OutError, TEXT("/carPaint/flakeSize ") + Why);
			}
			Desc.CarPaint.FlakeSize = static_cast<float>(Number);
		}
	}
	if (Desc.Shading == EMantlePlaceShading::CarPaint)
	{
		// One cooked permutation: M_MantlePlace_CarPaint is opaque and one-sided.
		if (Desc.AlphaMode != EMantlePlaceAlphaMode::Opaque)
		{
			return FailMaterial(Out, OutError, TEXT("/alphaMode must be opaque when shading is carPaint"));
		}
		if (Desc.bDoubleSided)
		{
			return FailMaterial(Out, OutError, TEXT("/doubleSided must be false when shading is carPaint"));
		}
		// #471's keys build on M_MantlePlace_Extended, which has no car paint.
		for (const TCHAR* Key : FidelityKeys)
		{
			if (Root->HasField(Key))
			{
				return FailMaterial(Out, OutError, FString::Printf(TEXT("/%s is not allowed when shading is carPaint"), Key));
			}
		}
	}

	if (const TSharedPtr<FJsonValue> Field = Root->TryGetField(TEXT("vertexColor")))
	{
		FString ModeName;
		if (Field->Type != EJson::String || !Field->TryGetString(ModeName))
		{
			return FailMaterial(Out, OutError, TEXT("/vertexColor must be none or multiplyBaseColor"));
		}
		if (ModeName.Equals(TEXT("none"), ESearchCase::CaseSensitive)) { Desc.VertexColor = EMantlePlaceVertexColor::None; }
		else if (ModeName.Equals(TEXT("multiplyBaseColor"), ESearchCase::CaseSensitive)) { Desc.VertexColor = EMantlePlaceVertexColor::MultiplyBaseColor; }
		else
		{
			return FailMaterial(Out, OutError, FString::Printf(TEXT("/vertexColor '%s' must be none or multiplyBaseColor"), *ModeName));
		}
	}
	if (const TSharedPtr<FJsonValue> Field = Root->TryGetField(TEXT("occlusionUV")))
	{
		const double Set = Field->Type == EJson::Number ? Field->AsNumber() : -1.0;
		if (Set != 0.0 && Set != 1.0)
		{
			return FailMaterial(Out, OutError, TEXT("/occlusionUV must be 0 or 1"));
		}
		Desc.OcclusionUV = Set == 1.0 ? 1 : 0;
	}
	if (const TSharedPtr<FJsonValue> Field = Root->TryGetField(TEXT("uvTransforms")))
	{
		if (Field->Type != EJson::Object || !Field->AsObject().IsValid())
		{
			return FailMaterial(Out, OutError, TEXT("/uvTransforms must be an object"));
		}
		const TSharedPtr<FJsonObject> Transforms = Field->AsObject();
		for (const auto& Pair : Transforms->Values)
		{
			const FString Key(Pair.Key.ToView());
			int32 SlotIndex = INDEX_NONE;
			for (int32 Slot = 0; Slot < MantlePlaceTextureSlotCount; ++Slot)
			{
				if (Key.Equals(SlotName(static_cast<EMantlePlaceTextureSlot>(Slot)), ESearchCase::CaseSensitive))
				{
					SlotIndex = Slot;
				}
			}
			if (SlotIndex == INDEX_NONE)
			{
				return FailMaterial(Out, OutError, FString::Printf(TEXT("/uvTransforms/%s is not a texture slot"), *Key));
			}
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object || !Pair.Value->AsObject().IsValid())
			{
				return FailMaterial(Out, OutError, FString::Printf(TEXT("/uvTransforms/%s must be an object"), *Key));
			}
			const TSharedPtr<FJsonObject> Entry = Pair.Value->AsObject();
			for (const auto& Part : Entry->Values)
			{
				const FString PartKey(Part.Key.ToView());
				if (!IsOneOf(PartKey, UVTransformKeys))
				{
					return FailMaterial(Out, OutError, FString::Printf(TEXT("/uvTransforms/%s/%s is not a UV transform property"), *Key, *PartKey));
				}
			}
			FMantlePlaceUVTransform Transform;
			if (const TSharedPtr<FJsonValue> Offset = Entry->TryGetField(TEXT("offset")))
			{
				if (!ReadRangedArray(Offset, 2, -1024.0, 1024.0, Values, Why))
				{
					return FailMaterial(Out, OutError, FString::Printf(TEXT("/uvTransforms/%s/offset %s"), *Key, *Why));
				}
				Transform.Offset = FVector2f(static_cast<float>(Values[0]), static_cast<float>(Values[1]));
			}
			if (const TSharedPtr<FJsonValue> Scale = Entry->TryGetField(TEXT("scale")))
			{
				if (!ReadRangedArray(Scale, 2, -1024.0, 1024.0, Values, Why))
				{
					return FailMaterial(Out, OutError, FString::Printf(TEXT("/uvTransforms/%s/scale %s"), *Key, *Why));
				}
				Transform.Scale = FVector2f(static_cast<float>(Values[0]), static_cast<float>(Values[1]));
			}
			if (const TSharedPtr<FJsonValue> Rotation = Entry->TryGetField(TEXT("rotation")))
			{
				if (!ReadRangedNumber(Rotation, -MaxUVRotation, MaxUVRotation, Number, Why))
				{
					return FailMaterial(Out, OutError, FString::Printf(TEXT("/uvTransforms/%s/rotation %s"), *Key, *Why));
				}
				Transform.Rotation = static_cast<float>(Number);
			}
			Desc.bHasUVTransform[SlotIndex] = true;
			Desc.UVTransforms[SlotIndex] = Transform;
		}
	}

	// #533: panning, wind, world-aligned mapping and a layer blend.
	if (const TSharedPtr<FJsonValue> Field = Root->TryGetField(TEXT("uvPanning")))
	{
		if (Field->Type != EJson::Object || !Field->AsObject().IsValid())
		{
			return FailMaterial(Out, OutError, TEXT("/uvPanning must be an object"));
		}
		for (const auto& Pair : Field->AsObject()->Values)
		{
			const FString Key(Pair.Key.ToView());
			int32 SlotIndex = INDEX_NONE;
			for (int32 Slot = 0; Slot < MantlePlaceTextureSlotCount; ++Slot)
			{
				if (Key.Equals(SlotName(static_cast<EMantlePlaceTextureSlot>(Slot)), ESearchCase::CaseSensitive))
				{
					SlotIndex = Slot;
				}
			}
			if (SlotIndex == INDEX_NONE)
			{
				return FailMaterial(Out, OutError, FString::Printf(TEXT("/uvPanning/%s is not a texture slot"), *Key));
			}
			if (!ReadRangedArray(Pair.Value, 2, -MaxUVPanning, MaxUVPanning, Values, Why))
			{
				return FailMaterial(Out, OutError, FString::Printf(TEXT("/uvPanning/%s %s"), *Key, *Why));
			}
			Desc.UVPanning[SlotIndex] = FVector2f(static_cast<float>(Values[0]), static_cast<float>(Values[1]));
		}
	}
	if (const TSharedPtr<FJsonValue> Field = Root->TryGetField(TEXT("wind")))
	{
		if (Field->Type != EJson::Object || !Field->AsObject().IsValid())
		{
			return FailMaterial(Out, OutError, TEXT("/wind must be an object"));
		}
		const TSharedPtr<FJsonObject> Wind = Field->AsObject();
		for (const auto& Pair : Wind->Values)
		{
			const FString Key(Pair.Key.ToView());
			if (!IsOneOf(Key, WindKeys))
			{
				return FailMaterial(Out, OutError, FString::Printf(TEXT("/wind/%s is not a wind property"), *Key));
			}
		}
		const FScalarField WindScalars[] = {
			{ TEXT("intensity"), MaxWind, &Desc.Wind.Intensity },
			{ TEXT("speed"), MaxWind, &Desc.Wind.Speed },
		};
		for (const FScalarField& Scalar : WindScalars)
		{
			if (!ReadRangedNumber(Wind->TryGetField(Scalar.Key), 0.0, Scalar.Max, Number, Why))
			{
				return FailMaterial(Out, OutError, FString::Printf(TEXT("/wind/%s is required and %s"), Scalar.Key, *Why));
			}
			*Scalar.Target = static_cast<float>(Number);
		}
		const TSharedPtr<FJsonValue> Weight = Wind->TryGetField(TEXT("weight"));
		FString WeightName;
		if (!Weight.IsValid() || Weight->Type != EJson::String || !Weight->TryGetString(WeightName))
		{
			return FailMaterial(Out, OutError, TEXT("/wind/weight is required and must be vertexColorR, uvV or uniform"));
		}
		if (WeightName.Equals(TEXT("vertexColorR"), ESearchCase::CaseSensitive)) { Desc.Wind.Weight = EMantlePlaceWindWeight::VertexColorR; }
		else if (WeightName.Equals(TEXT("uvV"), ESearchCase::CaseSensitive)) { Desc.Wind.Weight = EMantlePlaceWindWeight::UvV; }
		else if (WeightName.Equals(TEXT("uniform"), ESearchCase::CaseSensitive)) { Desc.Wind.Weight = EMantlePlaceWindWeight::Uniform; }
		else
		{
			return FailMaterial(Out, OutError, FString::Printf(TEXT("/wind/weight '%s' must be vertexColorR, uvV or uniform"), *WeightName));
		}
		Desc.bHasWind = true;
	}
	if (const TSharedPtr<FJsonValue> Field = Root->TryGetField(TEXT("worldAligned")))
	{
		if (Field->Type != EJson::Object || !Field->AsObject().IsValid())
		{
			return FailMaterial(Out, OutError, TEXT("/worldAligned must be an object"));
		}
		const TSharedPtr<FJsonObject> Aligned = Field->AsObject();
		for (const auto& Pair : Aligned->Values)
		{
			const FString Key(Pair.Key.ToView());
			if (!IsOneOf(Key, WorldAlignedKeys))
			{
				return FailMaterial(Out, OutError, FString::Printf(TEXT("/worldAligned/%s is not a world-aligned property"), *Key));
			}
		}
		if (!ReadRangedNumber(Aligned->TryGetField(TEXT("tileSizeCm")), MinTileSizeCm, MaxTileSizeCm, Number, Why))
		{
			return FailMaterial(Out, OutError, TEXT("/worldAligned/tileSizeCm is required and ") + Why);
		}
		Desc.WorldAligned.TileSizeCm = static_cast<float>(Number);
		if (!ReadRangedNumber(Aligned->TryGetField(TEXT("sharpness")), MinSharpness, MaxSharpness, Number, Why))
		{
			return FailMaterial(Out, OutError, TEXT("/worldAligned/sharpness is required and ") + Why);
		}
		Desc.WorldAligned.Sharpness = static_cast<float>(Number);
		Desc.bHasWorldAligned = true;
	}
	if (const TSharedPtr<FJsonValue> Field = Root->TryGetField(TEXT("layerBlend")))
	{
		if (Field->Type != EJson::Object || !Field->AsObject().IsValid())
		{
			return FailMaterial(Out, OutError, TEXT("/layerBlend must be an object"));
		}
		const TSharedPtr<FJsonObject> Layer = Field->AsObject();
		for (const auto& Pair : Layer->Values)
		{
			const FString Key(Pair.Key.ToView());
			if (!IsOneOf(Key, LayerBlendKeys))
			{
				return FailMaterial(Out, OutError, FString::Printf(TEXT("/layerBlend/%s is not a layer blend property"), *Key));
			}
		}
		const TSharedPtr<FJsonValue> Channel = Layer->TryGetField(TEXT("channel"));
		FString ChannelName;
		if (!Channel.IsValid() || Channel->Type != EJson::String || !Channel->TryGetString(ChannelName))
		{
			return FailMaterial(Out, OutError, TEXT("/layerBlend/channel is required and must be r, g, b or a"));
		}
		if (ChannelName.Equals(TEXT("r"), ESearchCase::CaseSensitive)) { Desc.LayerBlend.Channel = EMantlePlaceLayerChannel::R; }
		else if (ChannelName.Equals(TEXT("g"), ESearchCase::CaseSensitive)) { Desc.LayerBlend.Channel = EMantlePlaceLayerChannel::G; }
		else if (ChannelName.Equals(TEXT("b"), ESearchCase::CaseSensitive)) { Desc.LayerBlend.Channel = EMantlePlaceLayerChannel::B; }
		else if (ChannelName.Equals(TEXT("a"), ESearchCase::CaseSensitive)) { Desc.LayerBlend.Channel = EMantlePlaceLayerChannel::A; }
		else
		{
			return FailMaterial(Out, OutError, FString::Printf(TEXT("/layerBlend/channel '%s' must be r, g, b or a"), *ChannelName));
		}
		if (const TSharedPtr<FJsonValue> Factor = Layer->TryGetField(TEXT("baseColorFactor")))
		{
			if (!ReadRangedArray(Factor, 4, 0.0, 1.0, Values, Why))
			{
				return FailMaterial(Out, OutError, TEXT("/layerBlend/baseColorFactor ") + Why);
			}
			Desc.LayerBlend.BaseColorFactor = FLinearColor(static_cast<float>(Values[0]), static_cast<float>(Values[1]),
				static_cast<float>(Values[2]), static_cast<float>(Values[3]));
		}
		if (const TSharedPtr<FJsonValue> LayerTextures = Layer->TryGetField(TEXT("textures")))
		{
			if (LayerTextures->Type != EJson::Object || !LayerTextures->AsObject().IsValid())
			{
				return FailMaterial(Out, OutError, TEXT("/layerBlend/textures must be an object"));
			}
			for (const auto& Pair : LayerTextures->AsObject()->Values)
			{
				const FString Key(Pair.Key.ToView());
				int32 LayerIndex = INDEX_NONE;
				for (int32 Index = 0; Index < static_cast<int32>(UE_ARRAY_COUNT(LayerTextureKeys)); ++Index)
				{
					if (Key.Equals(LayerTextureKeys[Index], ESearchCase::CaseSensitive))
					{
						LayerIndex = Index;
					}
				}
				if (LayerIndex == INDEX_NONE)
				{
					return FailMaterial(Out, OutError, FString::Printf(TEXT("/layerBlend/textures/%s is not a layer texture slot"), *Key));
				}
				FString Id;
				if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String || !Pair.Value->TryGetString(Id) || !FMantlePlaceAssets::IsUploadedId(Id))
				{
					return FailMaterial(Out, OutError, FString::Printf(TEXT("/layerBlend/textures/%s must be an uploaded texture id (ast_u and 32 lowercase hex digits)"), *Key));
				}
				Desc.Textures[static_cast<int32>(EMantlePlaceTextureSlot::LayerBaseColor) + LayerIndex] = Id;
			}
		}
		Desc.bHasLayerBlend = true;
	}
	// #628: foliage shading; it goes with wind, nothing else.
	if (const TSharedPtr<FJsonValue> Field = Root->TryGetField(TEXT("foliage")))
	{
		if (Field->Type != EJson::Object || !Field->AsObject().IsValid())
		{
			return FailMaterial(Out, OutError, TEXT("/foliage must be an object"));
		}
		const TSharedPtr<FJsonObject> Foliage = Field->AsObject();
		for (const auto& Pair : Foliage->Values)
		{
			const FString Key(Pair.Key.ToView());
			if (!IsOneOf(Key, FoliageKeys))
			{
				return FailMaterial(Out, OutError, FString::Printf(TEXT("/foliage/%s is not a foliage property"), *Key));
			}
		}
		if (const TSharedPtr<FJsonValue> Tint = Foliage->TryGetField(TEXT("tint")))
		{
			if (!ReadRangedArray(Tint, 3, 0.0, 1.0, Values, Why))
			{
				return FailMaterial(Out, OutError, TEXT("/foliage/tint ") + Why);
			}
			Desc.Foliage.Tint = FLinearColor(static_cast<float>(Values[0]), static_cast<float>(Values[1]), static_cast<float>(Values[2]), 1.0f);
		}
		if (const TSharedPtr<FJsonValue> Strength = Foliage->TryGetField(TEXT("strength")))
		{
			if (!ReadRangedNumber(Strength, 0.0, MaxFoliageStrength, Number, Why))
			{
				return FailMaterial(Out, OutError, TEXT("/foliage/strength ") + Why);
			}
			Desc.Foliage.Strength = static_cast<float>(Number);
		}
		if (const TSharedPtr<FJsonValue> Weight = Foliage->TryGetField(TEXT("vertexColorWeight")))
		{
			if (!ReadRangedNumber(Weight, 0.0, 1.0, Number, Why))
			{
				return FailMaterial(Out, OutError, TEXT("/foliage/vertexColorWeight ") + Why);
			}
			Desc.Foliage.VertexColorWeight = static_cast<float>(Number);
		}
		Desc.bHasFoliage = true;
	}
	// #628: water shading, on a master of its own; it stands alone.
	if (const TSharedPtr<FJsonValue> Field = Root->TryGetField(TEXT("water")))
	{
		if (Field->Type != EJson::Object || !Field->AsObject().IsValid())
		{
			return FailMaterial(Out, OutError, TEXT("/water must be an object"));
		}
		const TSharedPtr<FJsonObject> Water = Field->AsObject();
		for (const auto& Pair : Water->Values)
		{
			const FString Key(Pair.Key.ToView());
			if (!IsOneOf(Key, WaterKeys))
			{
				return FailMaterial(Out, OutError, FString::Printf(TEXT("/water/%s is not a water property"), *Key));
			}
		}
		const struct { const TCHAR* Key; FLinearColor* Target; } Coefficients[] = {
			{ TEXT("scattering"), &Desc.Water.Scattering },
			{ TEXT("absorption"), &Desc.Water.Absorption },
		};
		for (const auto& Coefficient : Coefficients)
		{
			if (const TSharedPtr<FJsonValue> Value = Water->TryGetField(Coefficient.Key))
			{
				if (!ReadRangedArray(Value, 3, 0.0, 1.0, Values, Why))
				{
					return FailMaterial(Out, OutError, FString::Printf(TEXT("/water/%s %s"), Coefficient.Key, *Why));
				}
				*Coefficient.Target = FLinearColor(static_cast<float>(Values[0]), static_cast<float>(Values[1]), static_cast<float>(Values[2]), 1.0f);
			}
		}
		if (const TSharedPtr<FJsonValue> PhaseG = Water->TryGetField(TEXT("phaseG")))
		{
			if (!ReadRangedNumber(PhaseG, -1.0, 1.0, Number, Why))
			{
				return FailMaterial(Out, OutError, TEXT("/water/phaseG ") + Why);
			}
			Desc.Water.PhaseG = static_cast<float>(Number);
		}
		if (const TSharedPtr<FJsonValue> Scale = Water->TryGetField(TEXT("colorScaleBehindWater")))
		{
			if (!ReadRangedNumber(Scale, 0.0, 1.0, Number, Why))
			{
				return FailMaterial(Out, OutError, TEXT("/water/colorScaleBehindWater ") + Why);
			}
			Desc.Water.ColorScaleBehindWater = static_cast<float>(Number);
		}
		FString Unused;
		if (const TSharedPtr<FJsonValue> Normal = Water->TryGetField(TEXT("normal")))
		{
			if (!ReadWaterNormal(Normal, TEXT("/water/normal"), false, Desc.Water.Normal, Unused, Why))
			{
				return FailMaterial(Out, OutError, Why);
			}
		}
		if (const TSharedPtr<FJsonValue> Normal2 = Water->TryGetField(TEXT("normal2")))
		{
			FString TextureId;
			if (!ReadWaterNormal(Normal2, TEXT("/water/normal2"), true, Desc.Water.Normal2, TextureId, Why))
			{
				return FailMaterial(Out, OutError, Why);
			}
			Desc.Water.bHasNormal2 = true;
			Desc.Textures[static_cast<int32>(EMantlePlaceTextureSlot::WaterNormal2)] = TextureId;
		}
		Desc.bHasWater = true;
	}
	if (Desc.bHasWater)
	{
		for (const TCHAR* Other : { TEXT("foliage"), TEXT("wind"), TEXT("worldAligned"), TEXT("layerBlend") })
		{
			if (Root->HasField(Other))
			{
				return FailMaterial(Out, OutError, FString::Printf(TEXT("/%s cannot be used with water; water builds on a master of its own"), Other));
			}
		}
		if (Desc.AlphaMode != EMantlePlaceAlphaMode::Opaque)
		{
			return FailMaterial(Out, OutError, TEXT("/alphaMode must be opaque when water is set"));
		}
	}
	// One permutation family at a time, and only the blend modes it was cooked in (none translucent).
	const TCHAR* Variant = nullptr;
	for (const TCHAR* Key : VariantKeys)
	{
		if (!Root->HasField(Key))
		{
			continue;
		}
		if (Variant)
		{
			return FailMaterial(Out, OutError, FString::Printf(TEXT("/%s cannot be used with %s; a material has at most one of wind, worldAligned and layerBlend"), Key, Variant));
		}
		Variant = Key;
	}
	if (Desc.bHasFoliage && (Desc.bHasWorldAligned || Desc.bHasLayerBlend))
	{
		return FailMaterial(Out, OutError, FString::Printf(TEXT("/%s cannot be used with foliage; foliage goes with wind only"), Desc.bHasWorldAligned ? TEXT("worldAligned") : TEXT("layerBlend")));
	}
	if (Desc.bHasFoliage && (Desc.AlphaMode != EMantlePlaceAlphaMode::Mask || !Desc.bDoubleSided))
	{
		return FailMaterial(Out, OutError, TEXT("/alphaMode must be mask and /doubleSided true when foliage is set"));
	}
	if (Desc.bHasWind && Desc.AlphaMode != EMantlePlaceAlphaMode::Opaque && Desc.AlphaMode != EMantlePlaceAlphaMode::Mask)
	{
		return FailMaterial(Out, OutError, TEXT("/alphaMode must be opaque or mask when wind is set"));
	}
	if ((Desc.bHasWorldAligned || Desc.bHasLayerBlend) && Desc.AlphaMode != EMantlePlaceAlphaMode::Opaque)
	{
		return FailMaterial(Out, OutError, FString::Printf(TEXT("/alphaMode must be opaque when %s is set"), Desc.bHasWorldAligned ? TEXT("worldAligned") : TEXT("layerBlend")));
	}
	if (Desc.bHasLayerBlend && Desc.VertexColor != EMantlePlaceVertexColor::None)
	{
		return FailMaterial(Out, OutError, TEXT("/vertexColor must be none when layerBlend is set (both read the vertex colour)"));
	}

	if (const TSharedPtr<FJsonValue> Field = Root->TryGetField(TEXT("textures")))
	{
		if (Field->Type != EJson::Object || !Field->AsObject().IsValid())
		{
			return FailMaterial(Out, OutError, TEXT("/textures must be an object"));
		}
		const TSharedPtr<FJsonObject> Textures = Field->AsObject();
		for (const auto& Pair : Textures->Values)
		{
			const FString Key(Pair.Key.ToView());
			int32 SlotIndex = INDEX_NONE;
			for (int32 Slot = 0; Slot < MantlePlaceTextureSlotCount; ++Slot)
			{
				if (Key.Equals(SlotName(static_cast<EMantlePlaceTextureSlot>(Slot)), ESearchCase::CaseSensitive))
				{
					SlotIndex = Slot;
				}
			}
			if (SlotIndex == INDEX_NONE)
			{
				return FailMaterial(Out, OutError, FString::Printf(TEXT("/textures/%s is not a texture slot"), *Key));
			}
			FString Id;
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::String || !Pair.Value->TryGetString(Id) || !FMantlePlaceAssets::IsUploadedId(Id))
			{
				return FailMaterial(Out, OutError, FString::Printf(TEXT("/textures/%s must be an uploaded texture id (ast_u and 32 lowercase hex digits)"), *Key));
			}
			Desc.Textures[SlotIndex] = Id;
		}
	}

	Out = MoveTemp(Desc);
	return true;
}

int32 FMantlePlaceMaterials::PermutationIndex(const EMantlePlaceAlphaMode Mode, const bool bTwoSided)
{
	int32 Base = 0;
	switch (Mode)
	{
	case EMantlePlaceAlphaMode::Mask: Base = 2; break;
	case EMantlePlaceAlphaMode::Blend:
	case EMantlePlaceAlphaMode::Additive: Base = 4; break; // additive's own permutations are Extended's; this is its fallback
	case EMantlePlaceAlphaMode::Opaque:
	default: Base = 0; break;
	}
	return Base + (bTwoSided ? 1 : 0);
}

FString FMantlePlaceMaterials::PermutationName(const EMantlePlaceAlphaMode Mode, const bool bTwoSided)
{
	const TCHAR* Blend = Mode == EMantlePlaceAlphaMode::Mask ? TEXT("Masked")
		: Mode == EMantlePlaceAlphaMode::Blend || Mode == EMantlePlaceAlphaMode::Additive ? TEXT("Translucent") : TEXT("Opaque");
	return FString::Printf(TEXT("MI_MantlePlace_%s%s"), Blend, bTwoSided ? TEXT("_TwoSided") : TEXT(""));
}

FSoftObjectPath FMantlePlaceMaterials::PermutationPath(const EMantlePlaceAlphaMode Mode, const bool bTwoSided)
{
	const FString Name = PermutationName(Mode, bTwoSided);
	return FSoftObjectPath(FString::Printf(TEXT("%s/%s.%s"), MantlePlaceMaterialPrivate::MaterialsFolder, *Name, *Name));
}

int32 FMantlePlaceMaterials::PermutationIndexOf(const UMaterialInterface* Material)
{
	// Deep enough for any real chain (an instance of an uploaded material's instance of a
	// permutation); the bound also stops at a malformed cycle.
	for (int32 Depth = 0; Material && Depth < 8; ++Depth)
	{
		const FSoftObjectPath Path(Material);
		for (int32 Index = 0; Index < 6; ++Index)
		{
			const EMantlePlaceAlphaMode Mode = Index < 2 ? EMantlePlaceAlphaMode::Opaque
				: Index < 4 ? EMantlePlaceAlphaMode::Mask : EMantlePlaceAlphaMode::Blend;
			if (Path == PermutationPath(Mode, (Index % 2) == 1))
			{
				return Index;
			}
			if (Path == ExtendedPermutationPath(Mode, (Index % 2) == 1))
			{
				return ExtendedPermutationBase + Index; // #471's master, same order
			}
			for (const EMantlePlaceExtendedVariant Variant : MantlePlaceMaterialPrivate::Variants)
			{
				const int32 VariantIndex = ExtendedPermutationIndex(Variant, Mode, (Index % 2) == 1);
				if (VariantIndex != INDEX_NONE && Path == ExtendedPermutationPath(Variant, Mode, (Index % 2) == 1))
				{
					return VariantIndex; // #533's permutations
				}
			}
		}
		for (int32 Side = 0; Side < 2; ++Side)
		{
			// #628: the Additive permutations are named for their variant alone, so the loop above does not reach them.
			if (Path == ExtendedPermutationPath(EMantlePlaceExtendedVariant::Additive, EMantlePlaceAlphaMode::Additive, Side == 1))
			{
				return AdditivePermutationBase + Side;
			}
		}
		if (Path == CarPaintPermutationPath())
		{
			return CarPaintPermutationIndex;
		}
		const UMaterialInstance* Instance = Cast<UMaterialInstance>(Material);
		Material = Instance ? Instance->Parent.Get() : nullptr;
	}
	return INDEX_NONE;
}

UMaterialInterface* FMantlePlaceMaterials::FindExtendedPermutation(const EMantlePlaceAlphaMode Mode, const bool bTwoSided)
{
	return MantlePlaceMaterialPrivate::LoadPermutationAt(ExtendedPermutationBase + PermutationIndex(Mode, bTwoSided),
		ExtendedPermutationPath(Mode, bTwoSided));
}

UMaterialInterface* FMantlePlaceMaterials::FindExtendedPermutation(const EMantlePlaceExtendedVariant Variant, const EMantlePlaceAlphaMode Mode, const bool bTwoSided)
{
	const int32 Index = ExtendedPermutationIndex(Variant, Mode, bTwoSided);
	return Index == INDEX_NONE ? nullptr : MantlePlaceMaterialPrivate::LoadPermutationAt(Index, ExtendedPermutationPath(Variant, Mode, bTwoSided));
}

namespace MantlePlaceMaterialPrivate
{
	/** A document's M_MantlePlace_Extended variant, Plain when it has none or its variant was not cooked for its alpha mode. */
	EMantlePlaceExtendedVariant CookedVariantOf(const FMantlePlaceMaterialDesc& Desc)
	{
		const EMantlePlaceExtendedVariant Variant = Desc.ExtendedVariant();
		return FMantlePlaceMaterials::ExtendedPermutationIndex(Variant, Desc.AlphaMode, Desc.bDoubleSided) != INDEX_NONE
			? Variant : EMantlePlaceExtendedVariant::Plain;
	}
}

int32 FMantlePlaceMaterials::PermutationIndex(const FMantlePlaceMaterialDesc& Desc)
{
	if (Desc.Shading == EMantlePlaceShading::CarPaint)
	{
		return CarPaintPermutationIndex;
	}
	if (!Desc.UsesExtended())
	{
		return PermutationIndex(Desc.AlphaMode, Desc.bDoubleSided);
	}
	return ExtendedPermutationIndex(MantlePlaceMaterialPrivate::CookedVariantOf(Desc), Desc.AlphaMode, Desc.bDoubleSided);
}

FString FMantlePlaceMaterials::PermutationName(const FMantlePlaceMaterialDesc& Desc)
{
	if (Desc.Shading == EMantlePlaceShading::CarPaint)
	{
		return FString(CarPaintPermutationName);
	}
	return Desc.UsesExtended() ? ExtendedPermutationName(MantlePlaceMaterialPrivate::CookedVariantOf(Desc), Desc.AlphaMode, Desc.bDoubleSided)
		: PermutationName(Desc.AlphaMode, Desc.bDoubleSided);
}

FSoftObjectPath FMantlePlaceMaterials::PermutationPath(const FMantlePlaceMaterialDesc& Desc)
{
	if (Desc.Shading == EMantlePlaceShading::CarPaint)
	{
		return CarPaintPermutationPath();
	}
	return Desc.UsesExtended() ? ExtendedPermutationPath(MantlePlaceMaterialPrivate::CookedVariantOf(Desc), Desc.AlphaMode, Desc.bDoubleSided)
		: PermutationPath(Desc.AlphaMode, Desc.bDoubleSided);
}

const TCHAR* FMantlePlaceMaterials::ExtendedVariantName(const EMantlePlaceExtendedVariant Variant)
{
	switch (Variant)
	{
	case EMantlePlaceExtendedVariant::Wind: return TEXT("Wind");
	case EMantlePlaceExtendedVariant::WorldAligned: return TEXT("WorldAligned");
	case EMantlePlaceExtendedVariant::LayerBlend: return TEXT("LayerBlend");
	case EMantlePlaceExtendedVariant::Foliage: return TEXT("Foliage");
	case EMantlePlaceExtendedVariant::FoliageWind: return TEXT("FoliageWind");
	case EMantlePlaceExtendedVariant::Water: return TEXT("Water");
	case EMantlePlaceExtendedVariant::WaterNormal2: return TEXT("WaterNormal2");
	case EMantlePlaceExtendedVariant::Additive: return TEXT("Additive");
	case EMantlePlaceExtendedVariant::Plain:
	default: return TEXT("");
	}
}

int32 FMantlePlaceMaterials::ExtendedPermutationIndex(const EMantlePlaceExtendedVariant Variant, const EMantlePlaceAlphaMode Mode, const bool bTwoSided)
{
	const int32 Side = bTwoSided ? 1 : 0;
	switch (Variant)
	{
	case EMantlePlaceExtendedVariant::Plain:
		return ExtendedPermutationBase + PermutationIndex(Mode, bTwoSided);
	case EMantlePlaceExtendedVariant::Wind:
		return Mode == EMantlePlaceAlphaMode::Opaque || Mode == EMantlePlaceAlphaMode::Mask ? WindPermutationBase + PermutationIndex(Mode, bTwoSided) : INDEX_NONE;
	case EMantlePlaceExtendedVariant::WorldAligned:
		return Mode == EMantlePlaceAlphaMode::Opaque ? WorldAlignedPermutationBase + Side : INDEX_NONE;
	case EMantlePlaceExtendedVariant::LayerBlend:
		return Mode == EMantlePlaceAlphaMode::Opaque ? LayerBlendPermutationBase + Side : INDEX_NONE;
	case EMantlePlaceExtendedVariant::Foliage:
		return Mode == EMantlePlaceAlphaMode::Mask && bTwoSided ? FoliagePermutationIndex : INDEX_NONE;
	case EMantlePlaceExtendedVariant::FoliageWind:
		return Mode == EMantlePlaceAlphaMode::Mask && bTwoSided ? FoliageWindPermutationIndex : INDEX_NONE;
	case EMantlePlaceExtendedVariant::Water:
		return Mode == EMantlePlaceAlphaMode::Opaque ? WaterPermutationIndex : INDEX_NONE;
	case EMantlePlaceExtendedVariant::WaterNormal2:
		return Mode == EMantlePlaceAlphaMode::Opaque ? WaterNormal2PermutationIndex : INDEX_NONE;
	case EMantlePlaceExtendedVariant::Additive:
		return Mode == EMantlePlaceAlphaMode::Additive ? AdditivePermutationBase + Side : INDEX_NONE;
	default:
		return INDEX_NONE;
	}
}

FString FMantlePlaceMaterials::ExtendedPermutationName(const EMantlePlaceExtendedVariant Variant, const EMantlePlaceAlphaMode Mode, const bool bTwoSided)
{
	if (Variant == EMantlePlaceExtendedVariant::Plain)
	{
		return ExtendedPermutationName(Mode, bTwoSided);
	}
	// #628: M_MantlePlace_Water's instances are named for the water master, one for either sidedness.
	if (Variant == EMantlePlaceExtendedVariant::Water)
	{
		return FString(WaterPermutationName);
	}
	if (Variant == EMantlePlaceExtendedVariant::WaterNormal2)
	{
		return FString(WaterNormal2PermutationName);
	}
	// #628: the Additive instances are one per sidedness, named for the variant alone (there is no alpha-mode part).
	if (Variant == EMantlePlaceExtendedVariant::Additive)
	{
		return FString(AdditivePermutationName) + (bTwoSided ? TEXT("_TwoSided") : TEXT(""));
	}
	FString Name = PermutationName(Mode, bTwoSided);
	Name.RemoveFromStart(TEXT("MI_MantlePlace_"), ESearchCase::CaseSensitive);
	return FString::Printf(TEXT("MI_MantlePlace_Extended_%s_%s"), ExtendedVariantName(Variant), *Name);
}

FSoftObjectPath FMantlePlaceMaterials::ExtendedPermutationPath(const EMantlePlaceExtendedVariant Variant, const EMantlePlaceAlphaMode Mode, const bool bTwoSided)
{
	const FString Name = ExtendedPermutationName(Variant, Mode, bTwoSided);
	return FSoftObjectPath(FString::Printf(TEXT("%s/%s.%s"), MantlePlaceMaterialPrivate::MaterialsFolder, *Name, *Name));
}

bool FMantlePlaceMaterials::IsWindMaterial(const UMaterialInterface* Material)
{
	const int32 Index = PermutationIndexOf(Material);
	return (Index >= WindPermutationBase && Index < WorldAlignedPermutationBase) || Index == FoliageWindPermutationIndex;
}

FString FMantlePlaceMaterials::ExtendedPermutationName(const EMantlePlaceAlphaMode Mode, const bool bTwoSided)
{
	FString Name = PermutationName(Mode, bTwoSided);
	Name.RemoveFromStart(TEXT("MI_MantlePlace_"), ESearchCase::CaseSensitive);
	return TEXT("MI_MantlePlace_Extended_") + Name;
}

FSoftObjectPath FMantlePlaceMaterials::ExtendedPermutationPath(const EMantlePlaceAlphaMode Mode, const bool bTwoSided)
{
	const FString Name = ExtendedPermutationName(Mode, bTwoSided);
	return FSoftObjectPath(FString::Printf(TEXT("%s/%s.%s"), MantlePlaceMaterialPrivate::MaterialsFolder, *Name, *Name));
}

void FMantlePlaceUVTransform::ToRows(FLinearColor& OutU, FLinearColor& OutV) const
{
	// KHR_texture_transform's T * R * S in glTF's V-down texture space: u' = c*sx*u + s*sy*v + ox,
	// v' = -s*sx*u + c*sy*v + oy.
	float Sin = 0.0f;
	float Cos = 1.0f;
	FMath::SinCos(&Sin, &Cos, Rotation);
	OutU = FLinearColor(Cos * Scale.X, Sin * Scale.Y, Offset.X, 0.0f);
	OutV = FLinearColor(-Sin * Scale.X, Cos * Scale.Y, Offset.Y, 0.0f);
}

FMantlePlaceUVTransform FMantlePlaceMaterialDesc::UVTransformFor(const EMantlePlaceTextureSlot Slot) const
{
	const int32 Index = static_cast<int32>(Slot);
	if (Index >= 0 && Index < MantlePlaceTextureSlotCount && bHasUVTransform[Index])
	{
		return UVTransforms[Index];
	}
	FMantlePlaceUVTransform Shared;
	Shared.Offset = UVOffset;
	Shared.Scale = UVScale;
	return Shared;
}

bool FMantlePlaceMaterialDesc::UsesExtended() const
{
	if (Shading == EMantlePlaceShading::CarPaint)
	{
		return false;
	}
	bool bOwnTransform = false;
	for (int32 Slot = 0; Slot < MantlePlaceTextureSlotCount; ++Slot)
	{
		bOwnTransform = bOwnTransform || bHasUVTransform[Slot];
	}
	return bOwnTransform || NormalScale != 1.0f || OcclusionStrength != 1.0f || OcclusionUV != 0
		|| VertexColor != EMantlePlaceVertexColor::None || HasPanning() || ExtendedVariant() != EMantlePlaceExtendedVariant::Plain;
}

bool FMantlePlaceMaterialDesc::HasPanning() const
{
	for (int32 Slot = 0; Slot < MantlePlaceTextureSlotCount; ++Slot)
	{
		if (UVPanning[Slot].X != 0.0f || UVPanning[Slot].Y != 0.0f)
		{
			return true;
		}
	}
	return false;
}

EMantlePlaceExtendedVariant FMantlePlaceMaterialDesc::ExtendedVariant() const
{
	return AlphaMode == EMantlePlaceAlphaMode::Additive ? EMantlePlaceExtendedVariant::Additive
		: bHasWater ? (Water.bHasNormal2 ? EMantlePlaceExtendedVariant::WaterNormal2 : EMantlePlaceExtendedVariant::Water)
		: bHasFoliage ? (bHasWind ? EMantlePlaceExtendedVariant::FoliageWind : EMantlePlaceExtendedVariant::Foliage)
		: bHasWind ? EMantlePlaceExtendedVariant::Wind
		: bHasWorldAligned ? EMantlePlaceExtendedVariant::WorldAligned
		: bHasLayerBlend ? EMantlePlaceExtendedVariant::LayerBlend
		: EMantlePlaceExtendedVariant::Plain;
}

FSoftObjectPath FMantlePlaceMaterials::CarPaintPermutationPath()
{
	return FSoftObjectPath(FString::Printf(TEXT("%s/%s.%s"), MantlePlaceMaterialPrivate::MaterialsFolder, CarPaintPermutationName, CarPaintPermutationName));
}

UMaterialInterface* FMantlePlaceMaterials::FindCarPaintPermutation()
{
	return MantlePlaceMaterialPrivate::LoadPermutationAt(CarPaintPermutationIndex, CarPaintPermutationPath());
}

UMaterialInterface* FMantlePlaceMaterials::LoadPermutation(const FMantlePlaceMaterialDesc& Desc, bool* bOutFellBack)
{
	using namespace MantlePlaceMaterialPrivate;

	if (Desc.Shading != EMantlePlaceShading::CarPaint && Desc.UsesExtended())
	{
		// #533: a variant's own permutation first; without it, the plain extended one of the same alpha mode
		// and sidedness below (panning and #471's keys kept, the variant's effect dropped).
		bool bVariantMissing = false;
		const EMantlePlaceExtendedVariant Variant = CookedVariantOf(Desc);
		if (Variant != EMantlePlaceExtendedVariant::Plain)
		{
			const FSoftObjectPath VariantPath = ExtendedPermutationPath(Variant, Desc.AlphaMode, Desc.bDoubleSided);
			if (UMaterialInterface* Found = LoadPermutationAt(ExtendedPermutationIndex(Variant, Desc.AlphaMode, Desc.bDoubleSided), VariantPath))
			{
				if (bOutFellBack)
				{
					*bOutFellBack = false;
				}
				return Found;
			}
			const bool bWater = Variant == EMantlePlaceExtendedVariant::Water || Variant == EMantlePlaceExtendedVariant::WaterNormal2;
			if (bWater && !GWarnedWaterMissing)
			{
				GWarnedWaterMissing = true;
				UE_LOG(LogMantle, Warning,
					TEXT("MANTLE_PLACE_MATERIAL M_MantlePlace_Water is missing (%s not found): water materials use M_MantlePlace_Extended, an opaque surface without Single Layer Water, until the M_MantlePlace_Water asset is created"),
					*VariantPath.ToString());
			}
			else if (!bWater && !GWarnedVariantMissing)
			{
				GWarnedVariantMissing = true;
				UE_LOG(LogMantle, Warning,
					TEXT("MANTLE_PLACE_MATERIAL M_MantlePlace_Extended's %s permutations are missing (%s not found): materials using wind, worldAligned, layerBlend, foliage or additive blending use M_MantlePlace_Extended without them until tools/materials/Extend-MantlePlaceExtendedMaterial.py has run"),
					ExtendedVariantName(Variant), *VariantPath.ToString());
			}
			bVariantMissing = true;
		}

		const int32 Index = ExtendedPermutationBase + PermutationIndex(Desc.AlphaMode, Desc.bDoubleSided);
		if (UMaterialInterface* Extended = LoadPermutationAt(Index, ExtendedPermutationPath(Desc.AlphaMode, Desc.bDoubleSided)))
		{
			if (bOutFellBack)
			{
				*bOutFellBack = bVariantMissing;
			}
			return Extended;
		}
		if (!GWarnedExtendedMissing)
		{
			GWarnedExtendedMissing = true;
			UE_LOG(LogMantle, Warning,
				TEXT("MANTLE_PLACE_MATERIAL M_MantlePlace_Extended is missing (%s not found): materials using uvTransforms, normalScale, occlusionStrength, occlusionUV, vertexColor, uvPanning, wind, worldAligned, layerBlend, foliage or water use M_MantlePlace without them until tools/materials/Create-MantlePlaceExtendedMaterial.py has run"),
				*ExtendedPermutationPath(Desc.AlphaMode, Desc.bDoubleSided).ToString());
		}
		UMaterialInterface* Fallback = LoadPermutation(Desc.AlphaMode, Desc.bDoubleSided, nullptr);
		if (bOutFellBack)
		{
			*bOutFellBack = true;
		}
		return Fallback;
	}
	if (Desc.Shading != EMantlePlaceShading::CarPaint)
	{
		return LoadPermutation(Desc.AlphaMode, Desc.bDoubleSided, bOutFellBack);
	}
	if (UMaterialInterface* CarPaint = FindCarPaintPermutation())
	{
		if (bOutFellBack)
		{
			*bOutFellBack = false;
		}
		return CarPaint;
	}
	if (!GWarnedCarPaintMissing)
	{
		GWarnedCarPaintMissing = true;
		UE_LOG(LogMantle, Warning,
			TEXT("MANTLE_PLACE_MATERIAL M_MantlePlace_CarPaint is missing (%s not found): car-paint materials use MI_MantlePlace_Opaque, without the clear coat, until tools/materials/Create-CarPaintMaterial.py has run"),
			*CarPaintPermutationPath().ToString());
	}
	UMaterialInterface* Fallback = LoadPermutation(EMantlePlaceAlphaMode::Opaque, false, nullptr);
	if (bOutFellBack)
	{
		*bOutFellBack = true;
	}
	return Fallback;
}

void FMantlePlaceMaterials::ApplyCarPaint(UMaterialInstanceDynamic* Instance, const FMantlePlaceCarPaint& CarPaint)
{
	namespace P = MantlePlaceMaterialParameters;

	if (!Instance)
	{
		return;
	}
	Instance->SetScalarParameterValue(P::FlakeDensity, CarPaint.FlakeDensity);
	Instance->SetScalarParameterValue(P::FlakeSize, CarPaint.FlakeSize);
	Instance->SetVectorParameterValue(P::FlakeTint, FLinearColor(CarPaint.FlakeTint.R, CarPaint.FlakeTint.G, CarPaint.FlakeTint.B, 1.0f));
	Instance->SetScalarParameterValue(P::ClearCoat, CarPaint.ClearCoat);
	Instance->SetScalarParameterValue(P::ClearCoatRoughness, CarPaint.ClearCoatRoughness);
}

void FMantlePlaceMaterials::ApplyExtended(UMaterialInstanceDynamic* Instance, const FMantlePlaceMaterialDesc& Desc)
{
	namespace P = MantlePlaceMaterialParameters;

	if (!Instance)
	{
		return;
	}
	Instance->SetScalarParameterValue(P::NormalScale, Desc.NormalScale);
	Instance->SetScalarParameterValue(P::OcclusionStrength, Desc.OcclusionStrength);
	Instance->SetScalarParameterValue(P::VertexColorBaseColor, Desc.VertexColor == EMantlePlaceVertexColor::MultiplyBaseColor ? 1.0f : 0.0f);
	Instance->SetScalarParameterValue(P::OcclusionUV, Desc.OcclusionUV == 1 ? 1.0f : 0.0f);

	// Every slot gets its rows: its own transform, or the shared uvScale/uvOffset (the extended master has no UVScale/UVOffset).
	const TCHAR* const Rows[MantlePlaceTextureSlotCount][2] = {
		{ P::BaseColorUVTransformU, P::BaseColorUVTransformV },
		{ P::NormalUVTransformU, P::NormalUVTransformV },
		{ P::ORMUVTransformU, P::ORMUVTransformV },
		{ P::EmissiveUVTransformU, P::EmissiveUVTransformV },
	};
	for (int32 Slot = 0; Slot < MantlePlaceTextureSlotCount; ++Slot)
	{
		FLinearColor U;
		FLinearColor V;
		Desc.UVTransformFor(static_cast<EMantlePlaceTextureSlot>(Slot)).ToRows(U, V);
		Instance->SetVectorParameterValue(Rows[Slot][0], U);
		Instance->SetVectorParameterValue(Rows[Slot][1], V);
	}

	// #533: every slot's pan (zero when the document gives none), then the variant's values. The variant's
	// static switch is its parent permutation's, so a Plain document sets none of them.
	const TCHAR* const Panning[MantlePlaceTextureSlotCount] = { P::BaseColorUVPanning, P::NormalUVPanning, P::ORMUVPanning, P::EmissiveUVPanning };
	for (int32 Slot = 0; Slot < MantlePlaceTextureSlotCount; ++Slot)
	{
		Instance->SetVectorParameterValue(Panning[Slot], FLinearColor(Desc.UVPanning[Slot].X, Desc.UVPanning[Slot].Y, 0.0f, 0.0f));
	}
	switch (Desc.ExtendedVariant())
	{
	case EMantlePlaceExtendedVariant::FoliageWind:
	case EMantlePlaceExtendedVariant::Wind:
		Instance->SetScalarParameterValue(P::WindIntensity, Desc.Wind.Intensity);
		Instance->SetScalarParameterValue(P::WindSpeed, Desc.Wind.Speed);
		Instance->SetScalarParameterValue(P::WindWeightVertexColor, Desc.Wind.Weight == EMantlePlaceWindWeight::VertexColorR ? 1.0f : 0.0f);
		Instance->SetScalarParameterValue(P::WindWeightUV, Desc.Wind.Weight == EMantlePlaceWindWeight::UvV ? 1.0f : 0.0f);
		break;
	case EMantlePlaceExtendedVariant::WorldAligned:
		Instance->SetScalarParameterValue(P::WorldAlignedTileSize, Desc.WorldAligned.TileSizeCm);
		Instance->SetScalarParameterValue(P::WorldAlignedSharpness, Desc.WorldAligned.Sharpness);
		break;
	case EMantlePlaceExtendedVariant::LayerBlend:
	{
		const EMantlePlaceLayerChannel Channel = Desc.LayerBlend.Channel;
		Instance->SetVectorParameterValue(P::LayerChannel, FLinearColor(Channel == EMantlePlaceLayerChannel::R ? 1.0f : 0.0f,
			Channel == EMantlePlaceLayerChannel::G ? 1.0f : 0.0f, Channel == EMantlePlaceLayerChannel::B ? 1.0f : 0.0f,
			Channel == EMantlePlaceLayerChannel::A ? 1.0f : 0.0f));
		const FLinearColor& Factor = Desc.LayerBlend.BaseColorFactor;
		Instance->SetVectorParameterValue(P::LayerBaseColorFactor, FLinearColor(Factor.R, Factor.G, Factor.B, 1.0f));
		break;
	}
	case EMantlePlaceExtendedVariant::Foliage:
	case EMantlePlaceExtendedVariant::Plain:
	default:
		break;
	}

	// #628: the water values (the Single Layer Water shading model is the master's).
	if (Desc.bHasWater)
	{
		ApplyWater(Instance, Desc.Water);

		// On the M_MantlePlace_Extended fallback (M_MantlePlace_Water is missing) the first normal keeps its tiling, in
		// UV space (its pan was set by ApplyWater, the parameter being the same name). Harmless on the water master.
		if (Desc.Water.Normal.Space == EMantlePlaceWaterSpace::Uv)
		{
			FMantlePlaceUVTransform Tiled;
			Tiled.Scale = Desc.Water.Normal.Tiling;
			FLinearColor U;
			FLinearColor V;
			Tiled.ToRows(U, V);
			Instance->SetVectorParameterValue(P::NormalUVTransformU, U);
			Instance->SetVectorParameterValue(P::NormalUVTransformV, V);
		}
	}

	// #628: the foliage values (the Two Sided Foliage shading model is the permutation's).
	if (Desc.bHasFoliage)
	{
		const FLinearColor& Tint = Desc.Foliage.Tint;
		Instance->SetVectorParameterValue(P::SubsurfaceTint, FLinearColor(Tint.R, Tint.G, Tint.B, 1.0f));
		Instance->SetScalarParameterValue(P::SubsurfaceStrength, Desc.Foliage.Strength);
		Instance->SetScalarParameterValue(P::SubsurfaceVertexColorWeight, Desc.Foliage.VertexColorWeight);
	}
}

void FMantlePlaceMaterials::ApplyWater(UMaterialInstanceDynamic* Instance, const FMantlePlaceWater& Water)
{
	namespace P = MantlePlaceMaterialParameters;

	if (!Instance)
	{
		return;
	}
	Instance->SetVectorParameterValue(P::WaterScattering, FLinearColor(Water.Scattering.R, Water.Scattering.G, Water.Scattering.B, 1.0f));
	Instance->SetVectorParameterValue(P::WaterAbsorption, FLinearColor(Water.Absorption.R, Water.Absorption.G, Water.Absorption.B, 1.0f));
	Instance->SetScalarParameterValue(P::WaterPhaseG, Water.PhaseG);
	Instance->SetScalarParameterValue(P::WaterColorScale, Water.ColorScaleBehindWater);

	// Each layer: its UV tiling, its world scale (repeats per centimetre) and which of the two it reads, and its pan.
	const auto SetLayer = [Instance](const FMantlePlaceWaterNormal& Layer, const TCHAR* Tiling, const TCHAR* WorldSpace,
		const TCHAR* WorldScale, const TCHAR* Panning)
	{
		Instance->SetVectorParameterValue(Tiling, FLinearColor(Layer.Tiling.X, Layer.Tiling.Y, 0.0f, 0.0f));
		Instance->SetScalarParameterValue(WorldSpace, Layer.Space == EMantlePlaceWaterSpace::World ? 1.0f : 0.0f);
		Instance->SetScalarParameterValue(WorldScale, 1.0f / FMath::Max(Layer.WorldSizeCm, 1.0f));
		Instance->SetVectorParameterValue(Panning, FLinearColor(Layer.Panning.X, Layer.Panning.Y, 0.0f, 0.0f));
	};
	SetLayer(Water.Normal, P::NormalUVTiling, P::NormalWorldSpace, P::NormalWorldScale, P::NormalUVPanning);
	if (Water.bHasNormal2)
	{
		SetLayer(Water.Normal2, P::Normal2UVTiling, P::Normal2WorldSpace, P::Normal2WorldScale, P::Normal2UVPanning);
	}
}

UMaterialInterface* FMantlePlaceMaterials::LoadPermutation(const EMantlePlaceAlphaMode Mode, const bool bTwoSided, bool* bOutFellBack)
{
	using namespace MantlePlaceMaterialPrivate;

	UMaterialInterface* Loaded = LoadPermutationAt(PermutationIndex(Mode, bTwoSided), PermutationPath(Mode, bTwoSided));
	if (Loaded)
	{
		if (bOutFellBack)
		{
			*bOutFellBack = false;
		}
		return Loaded;
	}

	if (!GWarnedMissing)
	{
		GWarnedMissing = true;
		UE_LOG(LogMantle, Warning,
			TEXT("MANTLE_PLACE_MATERIAL M_MantlePlace is missing (%s not found): uploaded materials use the engine's default material until the M_MantlePlace permutation assets are added to the plugin content"),
			*PermutationPath(Mode, bTwoSided).ToString());
	}
	if (bOutFellBack)
	{
		*bOutFellBack = true;
	}
	return UMaterial::GetDefaultMaterial(MD_Surface);
}

UMaterialInstanceDynamic* FMantlePlaceMaterials::Build(const FMantlePlaceMaterialDesc& Desc, const TConstArrayView<UTexture*> Textures,
	UObject* Outer, const FName Name, FString* OutError)
{
	return BuildOnParent(LoadPermutation(Desc), Desc, Textures, Outer, Name, OutError);
}

UMaterialInstanceDynamic* FMantlePlaceMaterials::BuildOnParent(UMaterialInterface* Parent, const FMantlePlaceMaterialDesc& Desc,
	const TConstArrayView<UTexture*> Textures, UObject* Outer, const FName Name, FString* OutError)
{
	namespace P = MantlePlaceMaterialParameters;

	if (!IsInGameThread())
	{
		if (OutError)
		{
			*OutError = TEXT("materials are built on the game thread");
		}
		return nullptr;
	}
	if (!Parent)
	{
		if (OutError)
		{
			*OutError = TEXT("there is no parent material to build on");
		}
		return nullptr;
	}

	UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Parent, Outer ? Outer : GetTransientPackage(), Name);
	if (!Instance)
	{
		if (OutError)
		{
			*OutError = TEXT("the engine could not create a material instance");
		}
		return nullptr;
	}
	Instance->SetFlags(RF_Transient);

	// #628: additive is emissive-driven, so the base colour has no part in it: black with full opacity (the Additive permutations are
	// unlit, so the colour is unused there; the Translucent fallback, when they are missing, shows the glow over black and not over white).
	Instance->SetVectorParameterValue(P::BaseColorFactor, Desc.AlphaMode == EMantlePlaceAlphaMode::Additive ? FLinearColor(0.0f, 0.0f, 0.0f, 1.0f) : Desc.BaseColorFactor);
	Instance->SetVectorParameterValue(P::EmissiveFactor, FLinearColor(Desc.EmissiveFactor.R, Desc.EmissiveFactor.G, Desc.EmissiveFactor.B, 1.0f));
	Instance->SetVectorParameterValue(P::UVScale, FLinearColor(Desc.UVScale.X, Desc.UVScale.Y, 0.0f, 0.0f));
	Instance->SetVectorParameterValue(P::UVOffset, FLinearColor(Desc.UVOffset.X, Desc.UVOffset.Y, 0.0f, 0.0f));
	Instance->SetScalarParameterValue(P::MetallicFactor, Desc.MetallicFactor);
	Instance->SetScalarParameterValue(P::RoughnessFactor, Desc.RoughnessFactor);
	Instance->SetScalarParameterValue(P::EmissiveStrength, Desc.EmissiveStrength);
	// Set for every mode so the instance reads the same whatever the parent; only Masked uses it.
	Instance->SetScalarParameterValue(P::AlphaCutoff, Desc.AlphaMode == EMantlePlaceAlphaMode::Mask ? Desc.AlphaCutoff : 0.5f);

	if (Desc.Shading == EMantlePlaceShading::CarPaint)
	{
		ApplyCarPaint(Instance, Desc.CarPaint);
	}
	else if (Desc.UsesExtended())
	{
		ApplyExtended(Instance, Desc);
	}

	// A layer blend's textures (#533) bind to M_MantlePlace_Extended's Layer parameters, water's second normal (#628) to
	// M_MantlePlace_Water's Normal2; another parent ignores them.
	const TCHAR* const TextureParameters[MantlePlaceMaterialTextureCount] = { P::BaseColor, P::Normal, P::ORM, P::Emissive,
		P::LayerBaseColor, P::LayerNormal, P::LayerORM, P::Normal2 };
	for (int32 Slot = 0; Slot < MantlePlaceMaterialTextureCount && Slot < Textures.Num(); ++Slot)
	{
		// An additive material ignores its base colour texture (its alpha would be the opacity).
		if (Textures[Slot] && !(Desc.AlphaMode == EMantlePlaceAlphaMode::Additive && Slot == static_cast<int32>(EMantlePlaceTextureSlot::BaseColor)))
		{
			Instance->SetTextureParameterValue(TextureParameters[Slot], Textures[Slot]);
		}
	}
	return Instance;
}

void FMantlePlaceMaterials::DecodeBC4Block(const uint8* Block, uint8 (&OutValues)[16])
{
	uint8 Palette[8];
	MantlePlaceMaterialPrivate::BC4Palette(Block[0], Block[1], Palette);
	uint64 Indices = 0;
	for (int32 Byte = 0; Byte < 6; ++Byte)
	{
		Indices |= static_cast<uint64>(Block[2 + Byte]) << (8 * Byte);
	}
	for (int32 Texel = 0; Texel < 16; ++Texel)
	{
		OutValues[Texel] = Palette[(Indices >> (3 * Texel)) & 7];
	}
}

bool FMantlePlaceMaterials::PrepareForSlot(const EMantlePlaceTextureSlot Slot, FMantleDecodedTexture& Texture, FString* OutError)
{
	using namespace MantlePlaceMaterialPrivate;

	const EMantleTextureRole Wanted = RoleForSlot(Slot);
	if (Texture.Role != Wanted)
	{
		return FailSlot(OutError, FString::Printf(TEXT("the texture was cooked for another use than the %s slot"), SlotName(Slot)));
	}
	if (!MipsFitFormat(Texture))
	{
		return FailSlot(OutError, TEXT("the texture's mips do not fit its format"));
	}

	// A layer blend's slot (#533) is fixed up as the base slot it mirrors.
	const EMantlePlaceTextureSlot BaseSlot = BaseSlotOf(Slot);
	if (BaseSlot == EMantlePlaceTextureSlot::Normal)
	{
		if (Texture.Format == PF_BC5)
		{
			// Already Unreal's convention: the cook flipped green before encoding.
			return true;
		}
		if (Texture.Format == PF_B8G8R8A8)
		{
			// A role-less PNG/JPEG upload, as the creator exported it: glTF's green-up. BGRA8's green
			// is byte 1 of each texel.
			for (FMantleTextureMip& Mip : Texture.Mips)
			{
				for (int64 Offset = 1; Offset < Mip.Bytes.Num(); Offset += 4)
				{
					Mip.Bytes[Offset] = static_cast<uint8>(255 - Mip.Bytes[Offset]);
				}
			}
			return true;
		}
		return FailSlot(OutError, FString::Printf(TEXT("a normal map must be BC5 or 8-bit colour, not %s"), GetPixelFormatString(Texture.Format)));
	}

	if (BaseSlot == EMantlePlaceTextureSlot::Orm && Texture.Format == PF_BC4)
	{
		for (FMantleTextureMip& Mip : Texture.Mips)
		{
			const int32 BlocksWide = (Mip.Width + 3) / 4;
			const int32 BlocksHigh = (Mip.Height + 3) / 4;
			TArray64<uint8> Expanded;
			Expanded.SetNumUninitialized(static_cast<int64>(Mip.Width) * Mip.Height * 4);
			for (int32 BlockY = 0; BlockY < BlocksHigh; ++BlockY)
			{
				for (int32 BlockX = 0; BlockX < BlocksWide; ++BlockX)
				{
					uint8 Values[16];
					DecodeBC4Block(Mip.Bytes.GetData() + (static_cast<int64>(BlockY) * BlocksWide + BlockX) * 8, Values);
					for (int32 Row = 0; Row < 4; ++Row)
					{
						const int32 Y = BlockY * 4 + Row;
						for (int32 Column = 0; Column < 4 && Y < Mip.Height; ++Column)
						{
							const int32 X = BlockX * 4 + Column;
							if (X >= Mip.Width)
							{
								continue;
							}
							uint8* Texel = Expanded.GetData() + (static_cast<int64>(Y) * Mip.Width + X) * 4;
							const uint8 Value = Values[Row * 4 + Column];
							Texel[0] = Value;
							Texel[1] = Value;
							Texel[2] = Value;
							Texel[3] = 255;
						}
					}
				}
			}
			Mip.Bytes = MoveTemp(Expanded);
		}
		Texture.Format = PF_B8G8R8A8;
		Texture.bSRGB = false;
		return true;
	}
	return true;
}

void FMantlePlaceMaterials::ResetForTests()
{
	using namespace MantlePlaceMaterialPrivate;
	for (int32 Index = 0; Index < PermutationCount; ++Index)
	{
		GPermutationMissing[Index] = false;
		GPermutationChecked[Index] = false;
	}
	GWarnedMissing = false;
	GWarnedCarPaintMissing = false;
	GWarnedExtendedMissing = false;
	GWarnedVariantMissing = false;
	GWarnedWaterMissing = false;
}
