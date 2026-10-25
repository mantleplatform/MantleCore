// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceAssetConverter.h"
#include "MantleAnimClipWriter.h"
#include "MantleCosmeticExport.h"
#include "MantleHeightfieldReader.h"
#include "MantleMeshReduce.h"
#include "MantlePlaceLimits.h"
#include "MantlePng.h"
#include "MantleProvenance.h"
#include "MantleRuntime.h"
#include "MantleSha256.h"
#include "MantleWeaponSpawnPoint.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "MantleSkeletonHash.h"
#include "MantleTargetFeatures.h"
#include "MantleTerrainExport.h"
#include "MantleTerrainGrassExport.h"
#include "MantleVehicleConverter.h"
#include "MantleVehicleSource.h"
#include "MantleVehicleThumbnail.h"
#include "MantleCosmeticThumbnail.h"
#include "MantleModelThumbnail.h"
#include "MantleThumbnailStore.h"

#include "Animation/AnimSequence.h"
#include "Engine/SkeletalMesh.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Exporters/GLTFExporter.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "IO/IoHash.h"
#include "Materials/MaterialInterface.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "Options/GLTFExportOptions.h"
#include "PhysicsEngine/BodySetup.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "RHIGlobals.h"
#include "Sound/SoundWave.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#include "Editor.h"
#include "Engine/World.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePlaceAssetConverterPrivate
{
	/** Bump this whenever how a conversion is written changes, so stale caches are rebuilt. */
	const TCHAR* const CacheVersion = TEXT("f5-v14");

	const TCHAR* const ConversionRemedy = TEXT("It still publishes. Nothing to do unless it matters to how your place looks.");

	/** 0.8 as "0.8": rounded to 6 decimals, trailing zeros dropped, so equal materials hash equal. */
	FString Number(double Value)
	{
		if (!FMath::IsFinite(Value))
		{
			Value = 0.0;
		}
		const double Rounded = FMath::RoundToDouble(Value * 1000000.0) / 1000000.0;
		FString Text = FString::Printf(TEXT("%.6f"), Rounded);
		if (Text.Contains(TEXT(".")))
		{
			while (Text.EndsWith(TEXT("0")))
			{
				Text.LeftChopInline(1);
			}
			if (Text.EndsWith(TEXT(".")))
			{
				Text.LeftChopInline(1);
			}
		}
		return Text == TEXT("-0") ? FString(TEXT("0")) : Text;
	}

	FString JsonString(const FString& Value)
	{
		FString Out = TEXT("\"");
		for (const TCHAR Char : Value)
		{
			switch (Char)
			{
			case TEXT('"'): Out += TEXT("\\\""); break;
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('\n'): Out += TEXT("\\n"); break;
			case TEXT('\r'): Out += TEXT("\\r"); break;
			case TEXT('\t'): Out += TEXT("\\t"); break;
			default:
				if (Char < 0x20)
				{
					Out += FString::Printf(TEXT("\\u%04x"), static_cast<int32>(Char));
				}
				else
				{
					Out.AppendChar(Char);
				}
			}
		}
		Out += TEXT("\"");
		return Out;
	}

	bool IsUploadedId(const FString& Id)
	{
		if (Id.Len() != 37 || !Id.StartsWith(TEXT("ast_u"), ESearchCase::CaseSensitive))
		{
			return false;
		}
		for (int32 Index = 5; Index < Id.Len(); ++Index)
		{
			const TCHAR Char = Id[Index];
			if (!((Char >= TEXT('0') && Char <= TEXT('9')) || (Char >= TEXT('a') && Char <= TEXT('f'))))
			{
				return false;
			}
		}
		return true;
	}

	const TCHAR* const SlotOrder[] = { TEXT("baseColor"), TEXT("normal"), TEXT("orm"), TEXT("emissive") };

	/** #533: a layer blend's slots, as FMantlePlaceMaterialDocument::Textures keys them and as layerBlend.textures names them. */
	const TCHAR* const LayerSlotKeys[] = { TEXT("layerBaseColor"), TEXT("layerNormal"), TEXT("layerOrm") };
	const TCHAR* const LayerDocumentKeys[] = { TEXT("baseColor"), TEXT("normal"), TEXT("orm") };

	/** #628: water's second normal map, as FMantlePlaceMaterialDocument::Textures keys it (written as water.normal2.texture). */
	const TCHAR* const WaterNormal2SlotKey = TEXT("waterNormal2");

	/** The base slot a layer slot mirrors; a base slot itself. */
	EMantlePlaceTextureSlot BaseSlotOf(EMantlePlaceTextureSlot Slot)
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

	/** A water normal layer as water.normal and water.normal2 write it (#628); Texture is normal2's own, null for the first (textures.normal). */
	FString WriteWaterNormal(const FMantlePlaceDocumentWaterNormal& Layer, const FString* Texture)
	{
		auto Pan = [](double Value) { return FMath::Clamp(Value, -10.0, 10.0); };
		TArray<FString> Parts;
		if (Texture)
		{
			Parts.Add(TEXT("\"texture\":") + JsonString(*Texture));
		}
		Parts.Add(TEXT("\"space\":") + JsonString(Layer.bWorld ? TEXT("world") : TEXT("uv")));
		if (Layer.bWorld)
		{
			Parts.Add(TEXT("\"worldSizeCm\":") + Number(FMath::Clamp(Layer.WorldSizeCm, 1.0, 100000.0)));
		}
		else
		{
			Parts.Add(FString::Printf(TEXT("\"tiling\":[%s,%s]"), *Number(FMath::Clamp(Layer.Tiling[0], -1024.0, 1024.0)), *Number(FMath::Clamp(Layer.Tiling[1], -1024.0, 1024.0))));
		}
		const FString PanU = Number(Pan(Layer.Panning[0]));
		const FString PanV = Number(Pan(Layer.Panning[1]));
		if (!PanU.Equals(TEXT("0")) || !PanV.Equals(TEXT("0")))
		{
			Parts.Add(FString::Printf(TEXT("\"panning\":[%s,%s]"), *PanU, *PanV));
		}
		return TEXT("{") + FString::Join(Parts, TEXT(",")) + TEXT("}");
	}

	/**
	 * A water document (#628, FMantlePlaceMaterialDocument::WritesWater): only what M_MantlePlace_Water reads. The surface
	 * colour, roughness, normal scale, the first normal map and the water object (coefficients, the layers' layout and
	 * pan, the second normal map); none of the keys the schema says do nothing on water.
	 */
	FString WriteWaterDocument(const FMantlePlaceMaterialDocument& Doc, const TMap<FString, FString>& Values)
	{
		auto Unit = [](double Value) { return FMath::Clamp(Value, 0.0, 1.0); };
		const FString* Normal = Values.Find(TEXT("normal"));
		const FString* Normal2 = Values.Find(WaterNormal2SlotKey);

		TArray<FString> Fields;
		Fields.Add(TEXT("\"schemaVersion\":1"));
		Fields.Add(TEXT("\"alphaMode\":\"opaque\""));
		Fields.Add(TEXT("\"doubleSided\":false"));
		Fields.Add(FString::Printf(TEXT("\"baseColorFactor\":[%s,%s,%s,1]"), *Number(Unit(Doc.BaseColorFactor[0])), *Number(Unit(Doc.BaseColorFactor[1])),
			*Number(Unit(Doc.BaseColorFactor[2]))));
		Fields.Add(TEXT("\"roughnessFactor\":") + Number(Unit(Doc.RoughnessFactor)));
		const FString NormalScale = Number(FMath::Clamp(Doc.NormalScale, 0.0, 10.0));
		if (Normal && !NormalScale.Equals(TEXT("1")))
		{
			Fields.Add(TEXT("\"normalScale\":") + NormalScale);
		}

		TArray<FString> Water;
		Water.Add(FString::Printf(TEXT("\"scattering\":[%s,%s,%s]"), *Number(Unit(Doc.WaterScattering[0])), *Number(Unit(Doc.WaterScattering[1])),
			*Number(Unit(Doc.WaterScattering[2]))));
		Water.Add(FString::Printf(TEXT("\"absorption\":[%s,%s,%s]"), *Number(Unit(Doc.WaterAbsorption[0])), *Number(Unit(Doc.WaterAbsorption[1])),
			*Number(Unit(Doc.WaterAbsorption[2]))));
		const FString PhaseG = Number(FMath::Clamp(Doc.WaterPhaseG, -1.0, 1.0));
		if (!PhaseG.Equals(TEXT("0")))
		{
			Water.Add(TEXT("\"phaseG\":") + PhaseG);
		}
		const FString ColorScale = Number(Unit(Doc.WaterColorScaleBehindWater));
		if (!ColorScale.Equals(TEXT("1")))
		{
			Water.Add(TEXT("\"colorScaleBehindWater\":") + ColorScale);
		}
		if (Normal && !Doc.WaterNormal.IsDefault())
		{
			Water.Add(TEXT("\"normal\":") + WriteWaterNormal(Doc.WaterNormal, nullptr));
		}
		if (Normal2)
		{
			Water.Add(TEXT("\"normal2\":") + WriteWaterNormal(Doc.WaterNormal2, Normal2));
		}
		Fields.Add(TEXT("\"water\":{") + FString::Join(Water, TEXT(",")) + TEXT("}"));
		if (Normal)
		{
			Fields.Add(TEXT("\"textures\":{\"normal\":") + JsonString(*Normal) + TEXT("}"));
		}
		return TEXT("{") + FString::Join(Fields, TEXT(",")) + TEXT("}");
	}

	/**
	 * An additive document (#628, alphaMode additive): only what the Additive permutations read. The glow (emissiveFactor x
	 * emissiveStrength x the emissive texture) with the emissive slot's own transform and pan, the shared uvScale and
	 * uvOffset, and doubleSided; none of the keys the schema says do nothing on it (baseColor, metallic, roughness, orm, normal).
	 */
	FString WriteAdditiveDocument(const FMantlePlaceMaterialDocument& Doc, const TMap<FString, FString>& Values)
	{
		auto Unit = [](double Value) { return FMath::Clamp(Value, 0.0, 1.0); };
		auto Uv = [](double Value) { return FMath::Clamp(Value, -1024.0, 1024.0); };
		TArray<FString> Fields;
		Fields.Add(TEXT("\"schemaVersion\":1"));
		Fields.Add(TEXT("\"alphaMode\":\"additive\""));
		Fields.Add(FString::Printf(TEXT("\"doubleSided\":%s"), Doc.bDoubleSided ? TEXT("true") : TEXT("false")));
		Fields.Add(FString::Printf(TEXT("\"emissiveFactor\":[%s,%s,%s]"), *Number(Unit(Doc.EmissiveFactor[0])), *Number(Unit(Doc.EmissiveFactor[1])),
			*Number(Unit(Doc.EmissiveFactor[2]))));
		Fields.Add(TEXT("\"emissiveStrength\":") + Number(FMath::Clamp(Doc.EmissiveStrength, 0.0, 100.0)));
		Fields.Add(FString::Printf(TEXT("\"uvScale\":[%s,%s]"), *Number(Uv(Doc.UvScale[0])), *Number(Uv(Doc.UvScale[1]))));
		Fields.Add(FString::Printf(TEXT("\"uvOffset\":[%s,%s]"), *Number(Uv(Doc.UvOffset[0])), *Number(Uv(Doc.UvOffset[1]))));

		const FString* Emissive = Values.Find(TEXT("emissive"));
		if (Emissive)
		{
			if (const FMantlePlaceDocumentUvTransform* Transform = Doc.UvTransforms.Find(TEXT("emissive")))
			{
				TArray<FString> Parts;
				const FString OffsetU = Number(Uv(Transform->Offset[0]));
				const FString OffsetV = Number(Uv(Transform->Offset[1]));
				if (!OffsetU.Equals(TEXT("0")) || !OffsetV.Equals(TEXT("0")))
				{
					Parts.Add(FString::Printf(TEXT("\"offset\":[%s,%s]"), *OffsetU, *OffsetV));
				}
				const FString ScaleU = Number(Uv(Transform->Scale[0]));
				const FString ScaleV = Number(Uv(Transform->Scale[1]));
				if (!ScaleU.Equals(TEXT("1")) || !ScaleV.Equals(TEXT("1")))
				{
					Parts.Add(FString::Printf(TEXT("\"scale\":[%s,%s]"), *ScaleU, *ScaleV));
				}
				const FString Rotation = Number(FMath::UnwindRadians(Transform->Rotation));
				if (!Rotation.Equals(TEXT("0")))
				{
					Parts.Add(TEXT("\"rotation\":") + Rotation);
				}
				Fields.Add(TEXT("\"uvTransforms\":{\"emissive\":{") + FString::Join(Parts, TEXT(",")) + TEXT("}}"));
			}
			if (const FVector2D* Speed = Doc.UvPanning.Find(TEXT("emissive")))
			{
				auto Pan = [](double Value) { return FMath::Clamp(Value, -10.0, 10.0); };
				if (!Number(Pan(Speed->X)).Equals(TEXT("0")) || !Number(Pan(Speed->Y)).Equals(TEXT("0")))
				{
					Fields.Add(FString::Printf(TEXT("\"uvPanning\":{\"emissive\":[%s,%s]}"), *Number(Pan(Speed->X)), *Number(Pan(Speed->Y))));
				}
			}
			Fields.Add(TEXT("\"textures\":{\"emissive\":") + JsonString(*Emissive) + TEXT("}"));
		}
		return TEXT("{") + FString::Join(Fields, TEXT(",")) + TEXT("}");
	}

	/** The document with each texture slot's value from Values (ids, or keys for the cache's template). */
	FString WriteDocument(const FMantlePlaceMaterialDocument& Doc, const TMap<FString, FString>& Values)
	{
		if (Doc.WritesWater())
		{
			return WriteWaterDocument(Doc, Values);
		}
		if (Doc.WritesAdditive())
		{
			return WriteAdditiveDocument(Doc, Values);
		}
		auto Unit = [](double Value) { return FMath::Clamp(Value, 0.0, 1.0); };
		auto Uv = [](double Value) { return FMath::Clamp(Value, -1024.0, 1024.0); };
		const FString Alpha = Doc.AlphaMode == TEXT("mask") || Doc.AlphaMode == TEXT("blend") ? Doc.AlphaMode : FString(TEXT("opaque"));

		TArray<FString> Fields;
		Fields.Add(TEXT("\"schemaVersion\":1"));
		Fields.Add(TEXT("\"alphaMode\":") + JsonString(Alpha));
		if (Alpha == TEXT("mask"))
		{
			Fields.Add(TEXT("\"alphaCutoff\":") + Number(Unit(Doc.AlphaCutoff)));
		}
		Fields.Add(FString::Printf(TEXT("\"doubleSided\":%s"), Doc.bDoubleSided ? TEXT("true") : TEXT("false")));
		Fields.Add(FString::Printf(TEXT("\"baseColorFactor\":[%s,%s,%s,%s]"), *Number(Unit(Doc.BaseColorFactor[0])), *Number(Unit(Doc.BaseColorFactor[1])),
			*Number(Unit(Doc.BaseColorFactor[2])), *Number(Unit(Doc.BaseColorFactor[3]))));
		Fields.Add(TEXT("\"metallicFactor\":") + Number(Unit(Doc.MetallicFactor)));
		Fields.Add(TEXT("\"roughnessFactor\":") + Number(Unit(Doc.RoughnessFactor)));
		Fields.Add(FString::Printf(TEXT("\"emissiveFactor\":[%s,%s,%s]"), *Number(Unit(Doc.EmissiveFactor[0])), *Number(Unit(Doc.EmissiveFactor[1])),
			*Number(Unit(Doc.EmissiveFactor[2]))));
		Fields.Add(TEXT("\"emissiveStrength\":") + Number(FMath::Clamp(Doc.EmissiveStrength, 0.0, 100.0)));
		Fields.Add(FString::Printf(TEXT("\"uvScale\":[%s,%s]"), *Number(Uv(Doc.UvScale[0])), *Number(Uv(Doc.UvScale[1]))));
		Fields.Add(FString::Printf(TEXT("\"uvOffset\":[%s,%s]"), *Number(Uv(Doc.UvOffset[0])), *Number(Uv(Doc.UvOffset[1]))));

		// #471's keys, each only away from its default, so a material without them keeps its bytes.
		TArray<FString> Transforms;
		for (const TCHAR* Slot : SlotOrder)
		{
			const FMantlePlaceDocumentUvTransform* Transform = Doc.UvTransforms.Find(Slot);
			if (!Transform)
			{
				continue;
			}
			TArray<FString> Parts;
			const FString OffsetU = Number(Uv(Transform->Offset[0]));
			const FString OffsetV = Number(Uv(Transform->Offset[1]));
			if (!OffsetU.Equals(TEXT("0")) || !OffsetV.Equals(TEXT("0")))
			{
				Parts.Add(FString::Printf(TEXT("\"offset\":[%s,%s]"), *OffsetU, *OffsetV));
			}
			const FString ScaleU = Number(Uv(Transform->Scale[0]));
			const FString ScaleV = Number(Uv(Transform->Scale[1]));
			if (!ScaleU.Equals(TEXT("1")) || !ScaleV.Equals(TEXT("1")))
			{
				Parts.Add(FString::Printf(TEXT("\"scale\":[%s,%s]"), *ScaleU, *ScaleV));
			}
			// Wrapped into a half turn either way; six decimals keep pi inside the schema's 3.1416.
			const FString Rotation = Number(FMath::UnwindRadians(Transform->Rotation));
			if (!Rotation.Equals(TEXT("0")))
			{
				Parts.Add(TEXT("\"rotation\":") + Rotation);
			}
			Transforms.Add(JsonString(Slot) + TEXT(":{") + FString::Join(Parts, TEXT(",")) + TEXT("}"));
		}
		if (Transforms.Num() > 0)
		{
			Fields.Add(TEXT("\"uvTransforms\":{") + FString::Join(Transforms, TEXT(",")) + TEXT("}"));
		}
		const FString NormalScale = Number(FMath::Clamp(Doc.NormalScale, 0.0, 10.0));
		if (!NormalScale.Equals(TEXT("1")))
		{
			Fields.Add(TEXT("\"normalScale\":") + NormalScale);
		}
		const FString OcclusionStrength = Number(Unit(Doc.OcclusionStrength));
		if (!OcclusionStrength.Equals(TEXT("1")))
		{
			Fields.Add(TEXT("\"occlusionStrength\":") + OcclusionStrength);
		}
		if (Doc.OcclusionUV == 1)
		{
			Fields.Add(TEXT("\"occlusionUV\":1"));
		}
		if (Doc.VertexColor.Equals(TEXT("multiplyBaseColor"), ESearchCase::CaseSensitive))
		{
			Fields.Add(TEXT("\"vertexColor\":\"multiplyBaseColor\""));
		}

		// #533's keys, each only when set.
		TArray<FString> Pans;
		for (const TCHAR* Slot : SlotOrder)
		{
			const FVector2D* Speed = Doc.UvPanning.Find(Slot);
			auto Pan = [](double Value) { return FMath::Clamp(Value, -10.0, 10.0); };
			if (Speed && (!Number(Pan(Speed->X)).Equals(TEXT("0")) || !Number(Pan(Speed->Y)).Equals(TEXT("0"))))
			{
				Pans.Add(FString::Printf(TEXT("%s:[%s,%s]"), *JsonString(Slot), *Number(Pan(Speed->X)), *Number(Pan(Speed->Y))));
			}
		}
		if (Pans.Num() > 0)
		{
			Fields.Add(TEXT("\"uvPanning\":{") + FString::Join(Pans, TEXT(",")) + TEXT("}"));
		}
		// At most one of the three, as the schema allows: wind first, then world-aligned, then the layer.
		const bool bVariantOpaque = Alpha == TEXT("opaque");
		if (Doc.bWind && Alpha != TEXT("blend"))
		{
			const FString Weight = Doc.WindWeight.Equals(TEXT("vertexColorR"), ESearchCase::CaseSensitive)
				|| Doc.WindWeight.Equals(TEXT("uvV"), ESearchCase::CaseSensitive) ? Doc.WindWeight : FString(TEXT("uniform"));
			Fields.Add(FString::Printf(TEXT("\"wind\":{\"intensity\":%s,\"speed\":%s,\"weight\":%s}"), *Number(FMath::Clamp(Doc.WindIntensity, 0.0, 4.0)),
				*Number(FMath::Clamp(Doc.WindSpeed, 0.0, 4.0)), *JsonString(Weight)));
		}
		else if (Doc.bWorldAligned && bVariantOpaque)
		{
			Fields.Add(FString::Printf(TEXT("\"worldAligned\":{\"tileSizeCm\":%s,\"sharpness\":%s}"), *Number(FMath::Clamp(Doc.TileSizeCm, 1.0, 100000.0)),
				*Number(FMath::Clamp(Doc.Sharpness, 1.0, 16.0))));
		}
		else if (Doc.bLayerBlend && bVariantOpaque && !Doc.VertexColor.Equals(TEXT("multiplyBaseColor"), ESearchCase::CaseSensitive))
		{
			const FString Channel = Doc.LayerChannel == TEXT("g") || Doc.LayerChannel == TEXT("b") || Doc.LayerChannel == TEXT("a")
				? Doc.LayerChannel.ToLower() : FString(TEXT("r"));
			TArray<FString> Parts;
			Parts.Add(TEXT("\"channel\":") + JsonString(Channel));
			TArray<FString> LayerTextures;
			for (int32 Index = 0; Index < static_cast<int32>(UE_ARRAY_COUNT(LayerSlotKeys)); ++Index)
			{
				if (const FString* Value = Values.Find(LayerSlotKeys[Index]))
				{
					LayerTextures.Add(JsonString(LayerDocumentKeys[Index]) + TEXT(":") + JsonString(*Value));
				}
			}
			if (LayerTextures.Num() > 0)
			{
				Parts.Add(TEXT("\"textures\":{") + FString::Join(LayerTextures, TEXT(",")) + TEXT("}"));
			}
			const double* Factor = Doc.LayerBaseColorFactor;
			const FString FactorText = FString::Printf(TEXT("[%s,%s,%s,%s]"), *Number(Unit(Factor[0])), *Number(Unit(Factor[1])), *Number(Unit(Factor[2])),
				*Number(Unit(Factor[3])));
			if (FactorText != TEXT("[1,1,1,1]"))
			{
				Parts.Add(TEXT("\"baseColorFactor\":") + FactorText);
			}
			Fields.Add(TEXT("\"layerBlend\":{") + FString::Join(Parts, TEXT(",")) + TEXT("}"));
		}
		// #628: foliage goes with wind (or alone), masked and two-sided only; each value only away from its default.
		if (Doc.WritesFoliage())
		{
			TArray<FString> Parts;
			const FString Tint = FString::Printf(TEXT("[%s,%s,%s]"), *Number(Unit(Doc.FoliageTint[0])), *Number(Unit(Doc.FoliageTint[1])), *Number(Unit(Doc.FoliageTint[2])));
			if (Tint != TEXT("[1,1,1]"))
			{
				Parts.Add(TEXT("\"tint\":") + Tint);
			}
			const FString Strength = Number(FMath::Clamp(Doc.FoliageStrength, 0.0, 4.0));
			if (Strength != TEXT("1"))
			{
				Parts.Add(TEXT("\"strength\":") + Strength);
			}
			const FString Weight = Number(Unit(Doc.FoliageVertexColorWeight));
			if (Weight != TEXT("0"))
			{
				Parts.Add(TEXT("\"vertexColorWeight\":") + Weight);
			}
			Fields.Add(TEXT("\"foliage\":{") + FString::Join(Parts, TEXT(",")) + TEXT("}"));
		}

		TArray<FString> Textures;
		for (const TCHAR* Slot : SlotOrder)
		{
			if (const FString* Value = Values.Find(Slot))
			{
				Textures.Add(JsonString(Slot) + TEXT(":") + JsonString(*Value));
			}
		}
		if (Textures.Num() > 0)
		{
			Fields.Add(TEXT("\"textures\":{") + FString::Join(Textures, TEXT(",")) + TEXT("}"));
		}

		// #487: only car paint writes either key (the schema allows it on opaque, one-sided documents only).
		if (Doc.bCarPaint && Alpha == TEXT("opaque") && !Doc.bDoubleSided)
		{
			const FMantleCarPaintFinish& Paint = Doc.CarPaint;
			Fields.Add(TEXT("\"shading\":\"carPaint\""));
			Fields.Add(FString::Printf(TEXT("\"carPaint\":{\"flakeDensity\":%s,\"flakeSize\":%s,\"flakeTint\":[%s,%s,%s],\"clearCoat\":%s,\"clearCoatRoughness\":%s}"),
				*Number(Unit(Paint.FlakeDensity)), *Number(FMath::Clamp(Paint.FlakeSize, 0.01, 1.0)), *Number(Unit(Paint.FlakeTint[0])),
				*Number(Unit(Paint.FlakeTint[1])), *Number(Unit(Paint.FlakeTint[2])), *Number(Unit(Paint.ClearCoat)), *Number(Unit(Paint.ClearCoatRoughness))));
		}
		return TEXT("{") + FString::Join(Fields, TEXT(",")) + TEXT("}");
	}

	TArray<uint8> Utf8Bytes(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}

	FString RoleOf(EMantlePlaceTextureSlot Slot)
	{
		switch (BaseSlotOf(Slot))
		{
		case EMantlePlaceTextureSlot::Normal: return TEXT("normal");
		case EMantlePlaceTextureSlot::Orm: return TEXT("data");
		default: return TEXT("color");
		}
	}

	const TCHAR* SlotLabel(EMantlePlaceTextureSlot Slot)
	{
		switch (Slot)
		{
		case EMantlePlaceTextureSlot::Normal: return TEXT("normal map");
		case EMantlePlaceTextureSlot::Orm: return TEXT("occlusion, roughness and metallic");
		case EMantlePlaceTextureSlot::Emissive: return TEXT("emissive");
		case EMantlePlaceTextureSlot::LayerBaseColor: return TEXT("layer base colour");
		case EMantlePlaceTextureSlot::LayerNormal: return TEXT("layer normal map");
		case EMantlePlaceTextureSlot::LayerOrm: return TEXT("layer occlusion, roughness and metallic");
		case EMantlePlaceTextureSlot::WaterNormal2: return TEXT("second water normal map");
		default: return TEXT("base colour");
		}
	}

	FString HexOfHash(const FString& Hash)
	{
		FString Hex = Hash;
		Hex.RemoveFromStart(TEXT("sha256:"));
		return Hex;
	}

	/** Decoded to BGRA8 without changing the bytes of an 8-bit image (no gamma conversion). */
	bool DecodeBgra(TConstArrayView<uint8> Bytes, bool bColour, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight)
	{
		FImage Decoded;
		if (Bytes.Num() == 0 || !FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), Decoded))
		{
			return false;
		}
		const EGammaSpace Gamma = ERawImageFormat::IsHDR(Decoded.Format) ? (bColour ? EGammaSpace::sRGB : EGammaSpace::Linear) : Decoded.GammaSpace;
		FImage Bgra;
		Decoded.CopyTo(Bgra, ERawImageFormat::BGRA8, Gamma);
		OutWidth = Bgra.SizeX;
		OutHeight = Bgra.SizeY;
		const TArrayView64<FColor> View = Bgra.AsBGRA8();
		OutPixels = TArray<FColor>(View.GetData(), static_cast<int32>(View.Num()));
		return OutWidth > 0 && OutHeight > 0;
	}

	/**
	 * A texture's source pixels as a PNG: a decal's or livery's texture, or a slot #533 publishes from its source.
	 * The pixels as stored: an 8-bit image's bytes are kept, whatever gamma it is tagged with, as DecodeBgra keeps
	 * an image's; a floating-point one is encoded as sRGB colour or linear data (bLinear). A normal map (bNormal) is
	 * flipped from Unreal's green-down to an upload's glTF green-up, which the cook flips back
	 * (FMantlePlaceMaterials::PrepareForSlot), unless it is marked Flip Green Channel (green-up already). False with
	 * the reason, a clause, when it has no pixels Mantle can take.
	 */
	bool SourceTexturePng(const UTexture* Texture, const bool bLinear, const bool bNormal, TArray<uint8>& OutPng, FString& OutWhy)
	{
		if (!Texture || !Texture->Source.IsValid())
		{
			OutWhy = TEXT("it has no source pixels (a cooked or generated texture)");
			return false;
		}
		if (Texture->Source.GetNumBlocks() > 1 || Texture->Source.GetNumSlices() > 1)
		{
			OutWhy = TEXT("it is a UDIM, cube or array texture, and Mantle takes one 2D image");
			return false;
		}
		FImage Image;
		if (!const_cast<UTexture*>(Texture)->Source.GetMipImage(Image, 0))
		{
			OutWhy = TEXT("its pixels could not be read");
			return false;
		}
		const EGammaSpace Gamma = ERawImageFormat::IsHDR(Image.Format) ? (bLinear ? EGammaSpace::Linear : EGammaSpace::sRGB) : Image.GammaSpace;
		FImage Bgra;
		Image.CopyTo(Bgra, ERawImageFormat::BGRA8, Gamma);
		const TArrayView64<FColor> View = Bgra.AsBGRA8();
		TArray<FColor> Pixels(View.GetData(), static_cast<int32>(View.Num()));
		if (Pixels.Num() == 0)
		{
			OutWhy = TEXT("it has no pixels");
			return false;
		}
		if (bNormal && !Texture->bFlipGreenChannel)
		{
			for (FColor& Pixel : Pixels)
			{
				Pixel.G = static_cast<uint8>(255 - Pixel.G);
			}
		}
		OutPng = FMantlePng::EncodeRGBA8(Bgra.SizeX, Bgra.SizeY, Pixels);
		return OutPng.Num() > 0;
	}

	void Resize(TArray<FColor>& Pixels, int32& Width, int32& Height, int32 NewWidth, int32 NewHeight, bool bColour)
	{
		TArray<FColor> Resized;
		Resized.SetNumUninitialized(NewWidth * NewHeight);
		FImageUtils::ImageResize(Width, Height, TArrayView<const FColor>(Pixels), NewWidth, NewHeight, TArrayView<FColor>(Resized), bColour, /*bForceOpaqueOutput*/ false);
		Pixels = MoveTemp(Resized);
		Width = NewWidth;
		Height = NewHeight;
	}

	FString SizeText(int32 Width, int32 Height)
	{
		return Width == Height ? FString::Printf(TEXT("%d\u00B2"), Width) : FString::Printf(TEXT("%dx%d"), Width, Height);
	}

	bool EncodeJpeg(const TArray<FColor>& Pixels, int32 Width, int32 Height, int32 Quality, TArray<uint8>& OutBytes)
	{
		TArray64<uint8> Encoded;
		if (!FImageUtils::CompressImage(Encoded, TEXT("jpg"), FImageView(Pixels.GetData(), Width, Height, EGammaSpace::sRGB), Quality) || Encoded.Num() == 0)
		{
			return false;
		}
		OutBytes = TArray<uint8>(Encoded.GetData(), static_cast<int32>(Encoded.Num()));
		return true;
	}

	/** One glTF textureInfo: which texture, which UV set, and its KHR_texture_transform. */
	struct FTextureRef
	{
		int32 Texture = INDEX_NONE;
		int32 TexCoord = 0;
		double Offset[2] = { 0.0, 0.0 };
		double Scale[2] = { 1.0, 1.0 };
		double Rotation = 0.0;

		bool IsSet() const { return Texture != INDEX_NONE; }

		bool SameTransform(const FTextureRef& Other) const
		{
			return FMath::IsNearlyEqual(Offset[0], Other.Offset[0], 1e-4) && FMath::IsNearlyEqual(Offset[1], Other.Offset[1], 1e-4)
				&& FMath::IsNearlyEqual(Scale[0], Other.Scale[0], 1e-4) && FMath::IsNearlyEqual(Scale[1], Other.Scale[1], 1e-4)
				&& FMath::IsNearlyEqual(Rotation, Other.Rotation, 1e-4);
		}
	};

	TSharedPtr<FJsonObject> ObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		return Object.IsValid() && Object->TryGetObjectField(Field, Found) && Found ? *Found : TSharedPtr<FJsonObject>();
	}

	double NumberField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, double Default)
	{
		double Value = Default;
		return Object.IsValid() && Object->TryGetNumberField(Field, Value) ? Value : Default;
	}

	bool NumberArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, double* Out, int32 Count)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid() || !Object->TryGetArrayField(Field, Values) || Values->Num() < Count)
		{
			return false;
		}
		for (int32 Index = 0; Index < Count; ++Index)
		{
			double Value = 0.0;
			if (!(*Values)[Index].IsValid() || !(*Values)[Index]->TryGetNumber(Value))
			{
				return false;
			}
			Out[Index] = Value;
		}
		return true;
	}

	FTextureRef ReadTextureRef(const TSharedPtr<FJsonObject>& Info)
	{
		FTextureRef Ref;
		if (!Info.IsValid())
		{
			return Ref;
		}
		Ref.Texture = static_cast<int32>(NumberField(Info, TEXT("index"), INDEX_NONE));
		Ref.TexCoord = static_cast<int32>(NumberField(Info, TEXT("texCoord"), 0));
		if (const TSharedPtr<FJsonObject> Transform = ObjectField(ObjectField(Info, TEXT("extensions")), TEXT("KHR_texture_transform")))
		{
			NumberArray(Transform, TEXT("offset"), Ref.Offset, 2);
			NumberArray(Transform, TEXT("scale"), Ref.Scale, 2);
			Ref.Rotation = NumberField(Transform, TEXT("rotation"), 0.0);
			Ref.TexCoord = static_cast<int32>(NumberField(Transform, TEXT("texCoord"), Ref.TexCoord));
		}
		return Ref;
	}

	/** The embedded image a texture index points at, its source index and a name for messages. */
	bool ReadTextureImage(const FMantleGlbDocument& Doc, int32 TextureIndex, TArray<uint8>& OutBytes, int32& OutSource, FString& OutName)
	{
		const TArray<TSharedPtr<FJsonValue>>* Textures = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Images = nullptr;
		if (!Doc.Json.IsValid() || !Doc.Json->TryGetArrayField(TEXT("textures"), Textures) || !Doc.Json->TryGetArrayField(TEXT("images"), Images)
			|| !Textures->IsValidIndex(TextureIndex))
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Texture = (*Textures)[TextureIndex]->AsObject();
		OutSource = static_cast<int32>(NumberField(Texture, TEXT("source"), INDEX_NONE));
		if (!Images->IsValidIndex(OutSource))
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Image = (*Images)[OutSource]->AsObject();
		if (Texture.IsValid())
		{
			Texture->TryGetStringField(TEXT("name"), OutName);
		}
		if (OutName.IsEmpty() && Image.IsValid())
		{
			Image->TryGetStringField(TEXT("name"), OutName);
		}
		// GLTFExporter tags a colour-space conversion onto the name; the creator knows the texture by its own.
		OutName.RemoveFromEnd(TEXT("_sRGB"));
		OutName.RemoveFromEnd(TEXT("_Linear"));
		const int32 View = static_cast<int32>(NumberField(Image, TEXT("bufferView"), INDEX_NONE));
		return View != INDEX_NONE && Doc.ReadBufferView(View, OutBytes);
	}

	FMantleValidationIssue WarningIssue(const FString& Problem, const FString& Remedy = FString(ConversionRemedy))
	{
		return FMantleValidationIssue{ EMantleIssueSeverity::Warning, FString(), Problem, Remedy };
	}

	FMantleValidationIssue ErrorIssue(const FString& Problem, const FString& Remedy)
	{
		return FMantleValidationIssue{ EMantleIssueSeverity::Error, FString(), Problem, Remedy };
	}

	/**
	 * #533: what the analysis recognised, with each source texture read (FMantleMaterialExtras). A world-aligned
	 * mapping or layer blend any of whose textures cannot be read is dropped whole, a pan with its slot, each
	 * with a warning (Check this place counted them as carried), and those slots keep GLTFExporter's bake.
	 */
	FMantleMaterialExtras MaterialExtras(const FMantleMaterialAnalysis& Analysis, const FString& MaterialName, TArray<FMantleValidationIssue>& OutIssues)
	{
		FMantleMaterialExtras Extras;
		Extras.bVertexColorTintsBaseColor = Analysis.bVertexColorTintsBaseColor;
		Extras.Fidelity = Analysis.Fidelity2;
		FMantleMaterialFidelity2& Fidelity = Extras.Fidelity;
		FString Failed;
		auto Read = [&Extras, &Failed](const UTexture* Texture, const int32 Image, const bool bLinear, const bool bNormal)
		{
			FString Why;
			if (!SourceTexturePng(Texture, bLinear, bNormal, Extras.SourceImages[Image], Why))
			{
				Extras.SourceImages[Image].Reset();
				Failed = FString::Printf(TEXT("'%s' %s"), Texture ? *Texture->GetName() : TEXT("(a texture)"), *Why);
				return false;
			}
			Extras.SourceNames[Image] = Texture->GetName();
			return true;
		};
		// Images in EMantlePlaceTextureSlot order: base colour, normal, orm, emissive, then the layer's base colour,
		// normal, orm. Normal maps and orm are linear data.
		auto ReadSlot = [&Read](const TWeakObjectPtr<UTexture>& Texture, const int32 Image)
		{
			// Image 7 is water's second normal map (EMantlePlaceTextureSlot::WaterNormal2), read as the normal slot is.
			const int32 Base = Image == static_cast<int32>(EMantlePlaceTextureSlot::WaterNormal2) ? 1 : (Image >= 4 ? Image - 4 : Image);
			return !Texture.IsValid() || Read(Texture.Get(), Image, /*bLinear*/ Base == 1 || Base == 2, /*bNormal*/ Base == 1);
		};

		// #628: water's normal maps (textures.normal and water.normal2.texture): the source image of each layer the water key
		// carries, the first two. A layer whose image cannot be read is left out, with the ones after it; none left, and the
		// normal is the bake's.
		FMantleWaterBake& Water = Fidelity.Water;
		if (Water.PublishesOpaque() && Water.Normals.Num() > 0)
		{
			if (Water.Normals.Num() > 2)
			{
				Water.Normals.SetNum(2);
			}
			for (int32 Layer = 0; Layer < Water.Normals.Num(); ++Layer)
			{
				const int32 Image = Layer == 0 ? static_cast<int32>(EMantlePlaceTextureSlot::Normal) : static_cast<int32>(EMantlePlaceTextureSlot::WaterNormal2);
				if (ReadSlot(Water.Normals[Layer].Texture, Image) && Water.Normals[Layer].Texture.IsValid())
				{
					continue;
				}
				OutIssues.Add(WarningIssue(FString::Printf(TEXT("Material '%s': its %s normal map could not be kept (%s), so %s."), *MaterialName, Layer == 0 ? TEXT("first") : TEXT("second"),
					Water.Normals[Layer].Texture.IsValid() ? *Failed : TEXT("it has no texture"), Layer == 0 ? TEXT("the water's normal is baked onto the mesh's own UVs") : TEXT("the water keeps one normal map"))));
				Water.Normals.SetNum(Layer);
				break;
			}
		}

		if (Fidelity.bWorldAligned || Fidelity.bLayerBlend)
		{
			bool bRead = true;
			for (int32 Slot = 0; Slot < 3 && bRead; ++Slot)
			{
				bRead = ReadSlot(Fidelity.SourceTextures[Slot], Slot) && (!Fidelity.bLayerBlend || ReadSlot(Fidelity.LayerTextures[Slot], 4 + Slot));
			}
			if (!bRead)
			{
				OutIssues.Add(WarningIssue(FString::Printf(TEXT("Material '%s': its %s could not be kept (%s), so it is baked onto the mesh's own UVs."), *MaterialName,
					Fidelity.bWorldAligned ? TEXT("world-aligned mapping") : TEXT("vertex-colour layer blend"), *Failed)));
				Fidelity.bWorldAligned = false;
				Fidelity.bLayerBlend = false;
				for (int32 Slot = 0; Slot < 3; ++Slot)
				{
					Fidelity.SourceTextures[Slot].Reset();
					Fidelity.LayerTextures[Slot].Reset();
					Extras.SourceImages[Slot].Reset();
					Extras.SourceImages[4 + Slot].Reset();
				}
			}
		}
		for (int32 Slot = 0; Slot < 4; ++Slot)
		{
			if (Fidelity.bPanning[Slot] && !ReadSlot(Fidelity.SourceTextures[Slot], Slot))
			{
				OutIssues.Add(WarningIssue(FString::Printf(TEXT("Material '%s': its panning could not be kept (%s), so that texture stands still."), *MaterialName, *Failed)));
				Fidelity.bPanning[Slot] = false;
				Fidelity.SourceTextures[Slot].Reset();
			}
		}
		return Extras;
	}

	/** Writes to a temporary name and moves it in place, so a crash never leaves a half-written cache entry. */
	bool SaveAtomically(const TArray<uint8>& Bytes, const FString& Path)
	{
		const FString Temporary = Path + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
		if (!FFileHelper::SaveArrayToFile(Bytes, *Temporary))
		{
			return false;
		}
		if (!IFileManager::Get().Move(*Path, *Temporary, /*bReplace*/ true))
		{
			IFileManager::Get().Delete(*Temporary);
			return false;
		}
		return true;
	}

	bool SaveStringAtomically(const FString& Text, const FString& Path)
	{
		return SaveAtomically(Utf8Bytes(Text), Path);
	}

	FString JsonText(const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object, Writer);
		return Text;
	}

	TSharedPtr<FJsonObject> ReadJson(const FString& Path)
	{
		FString Text;
		TSharedPtr<FJsonObject> Object;
		if (FFileHelper::LoadFileToString(Text, *Path))
		{
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			FJsonSerializer::Deserialize(Reader, Object);
		}
		return Object;
	}

	TSharedRef<FJsonObject> IssueJson(const FMantleValidationIssue& Issue)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("severity"), Issue.Severity == EMantleIssueSeverity::Error ? TEXT("error") : TEXT("warning"));
		Object->SetStringField(TEXT("problem"), Issue.Problem);
		Object->SetStringField(TEXT("remedy"), Issue.Remedy);
		return Object;
	}

	/** The issues a cache entry recorded (IssueJson's), appended to OutIssues. */
	void ReadCachedIssues(const TSharedPtr<FJsonObject>& Meta, TArray<FMantleValidationIssue>& OutIssues)
	{
		const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
		if (!Meta.IsValid() || !Meta->TryGetArrayField(TEXT("issues"), Issues))
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Issues)
		{
			const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Entry.IsValid())
			{
				continue;
			}
			FMantleValidationIssue Issue;
			FString Severity;
			Entry->TryGetStringField(TEXT("severity"), Severity);
			Issue.Severity = Severity == TEXT("error") ? EMantleIssueSeverity::Error : EMantleIssueSeverity::Warning;
			Entry->TryGetStringField(TEXT("problem"), Issue.Problem);
			Entry->TryGetStringField(TEXT("remedy"), Issue.Remedy);
			OutIssues.Add(MoveTemp(Issue));
		}
	}

	bool IsUploadableCollisionValue(const FVector& Value)
	{
		return !Value.ContainsNaN() && Value.GetAbsMax() <= FMantleStaticMeshReader::MaxCollisionExtent;
	}

	/**
	 * #470: Mesh's simple collision as the cook accepts it (backend cook.StaticCollision): its boxes, spheres,
	 * capsules and convex hulls (each hull's transform applied), in the mesh's space, Unreal centimetres. None
	 * when its collision complexity is Use Complex As Simple (the project default resolved), or when it has no
	 * shapes. A shape the cook would refuse, or a kind MRM2 does not carry, is left out and named in a warning;
	 * past Limits' caps the whole simple collision is left off with a warning (the mesh then collides with its
	 * triangles, as before #470).
	 */
	FMantleStaticMeshCollision ReadSimpleCollision(const UStaticMesh* Mesh, const FMantlePlaceAssetLimits& Limits, TArray<FMantleValidationIssue>& OutIssues)
	{
		FMantleStaticMeshCollision Out;
		const UBodySetup* Body = Mesh ? Mesh->GetBodySetup() : nullptr;
		if (!Body)
		{
			return Out;
		}
		const ECollisionTraceFlag Flag = Body->GetCollisionTraceFlag();
		if (Flag == CTF_UseComplexAsSimple)
		{
			return Out;
		}
		const FKAggregateGeom& Geom = Body->AggGeom;
		TArray<FString> LeftOut;
		for (int32 Index = 0; Index < Geom.BoxElems.Num(); ++Index)
		{
			const FKBoxElem& Box = Geom.BoxElems[Index];
			const FVector Size(Box.X, Box.Y, Box.Z);
			if (!IsUploadableCollisionValue(Box.Center) || !IsUploadableCollisionValue(Size) || !(Box.X > 0.0f && Box.Y > 0.0f && Box.Z > 0.0f)
				|| Box.Rotation.ContainsNaN())
			{
				LeftOut.Add(FString::Printf(TEXT("box %d (no volume)"), Index));
				continue;
			}
			FMantleStaticMeshBox& Shape = Out.Boxes.AddDefaulted_GetRef();
			Shape.Center = FVector3f(Box.Center);
			Shape.Rotation = FQuat4f(Box.Rotation.Quaternion().GetNormalized());
			Shape.Size = FVector3f(Size);
		}
		for (int32 Index = 0; Index < Geom.SphereElems.Num(); ++Index)
		{
			const FKSphereElem& Sphere = Geom.SphereElems[Index];
			if (!IsUploadableCollisionValue(Sphere.Center) || !(Sphere.Radius > 0.0f) || Sphere.Radius > FMantleStaticMeshReader::MaxCollisionExtent)
			{
				LeftOut.Add(FString::Printf(TEXT("sphere %d (no radius)"), Index));
				continue;
			}
			FMantleStaticMeshSphere& Shape = Out.Spheres.AddDefaulted_GetRef();
			Shape.Center = FVector3f(Sphere.Center);
			Shape.Radius = Sphere.Radius;
		}
		for (int32 Index = 0; Index < Geom.SphylElems.Num(); ++Index)
		{
			const FKSphylElem& Capsule = Geom.SphylElems[Index];
			if (!IsUploadableCollisionValue(Capsule.Center) || !(Capsule.Radius > 0.0f) || !(Capsule.Length >= 0.0f) || Capsule.Rotation.ContainsNaN()
				|| Capsule.Radius > FMantleStaticMeshReader::MaxCollisionExtent || Capsule.Length > FMantleStaticMeshReader::MaxCollisionExtent)
			{
				LeftOut.Add(FString::Printf(TEXT("capsule %d (no radius)"), Index));
				continue;
			}
			FMantleStaticMeshCapsule& Shape = Out.Capsules.AddDefaulted_GetRef();
			Shape.Center = FVector3f(Capsule.Center);
			Shape.Rotation = FQuat4f(Capsule.Rotation.Quaternion().GetNormalized());
			Shape.Radius = Capsule.Radius;
			Shape.Length = Capsule.Length;
		}
		bool bHullPastCap = false;
		for (int32 Index = 0; Index < Geom.ConvexElems.Num(); ++Index)
		{
			const FKConvexElem& Convex = Geom.ConvexElems[Index];
			const FTransform Transform = Convex.GetTransform();
			TArray<FVector3f> Points;
			bool bFinite = true;
			for (const FVector& Point : Convex.VertexData)
			{
				const FVector Placed = Transform.TransformPosition(Point);
				bFinite = bFinite && IsUploadableCollisionValue(Placed);
				Points.Add(FVector3f(Placed));
			}
			if (!bFinite || !FMantleStaticMeshReader::HullSpansVolume(Points))
			{
				LeftOut.Add(FString::Printf(TEXT("convex hull %d (flat, or fewer than 4 points)"), Index));
				continue;
			}
			bHullPastCap = bHullPastCap || Points.Num() > Limits.MaxHullVertices;
			Out.Hulls.AddDefaulted_GetRef().Vertices = MoveTemp(Points);
		}
		const int32 Unsupported = Geom.TaperedCapsuleElems.Num() + Geom.LevelSetElems.Num() + Geom.SkinnedLevelSetElems.Num() + Geom.MLLevelSetElems.Num()
			+ Geom.SkinnedTriangleMeshElems.Num();
		if (Unsupported > 0)
		{
			LeftOut.Add(FString::Printf(TEXT("%d tapered capsule or level set shape(s), which Mantle does not carry"), Unsupported));
		}

		const FString Name = Mesh->GetName();
		int32 HullPoints = 0;
		for (const FMantleStaticMeshHull& Hull : Out.Hulls)
		{
			HullPoints += Hull.Vertices.Num();
		}
		const int32 Primitives = Out.Boxes.Num() + Out.Spheres.Num() + Out.Capsules.Num();
		if (Primitives > Limits.MaxCollisionPrimitives || Out.Hulls.Num() > Limits.MaxConvexHulls || bHullPastCap || HullPoints > Limits.MaxTotalHullVertices)
		{
			OutIssues.Add(WarningIssue(FString::Printf(TEXT("Mesh '%s' has more simple collision than a place mesh may carry (%d boxes, spheres and capsules, %d convex hulls of %d points; ")
				TEXT("at most %d, %d, %d points a hull and %d in all), so it collides with its triangles instead."), *Name, Primitives, Out.Hulls.Num(), HullPoints,
				Limits.MaxCollisionPrimitives, Limits.MaxConvexHulls, Limits.MaxHullVertices, Limits.MaxTotalHullVertices),
				TEXT("It still publishes. For cheaper collision, simplify it in the mesh editor (fewer or smaller convex hulls).")));
			return FMantleStaticMeshCollision();
		}
		if (LeftOut.Num() > 0)
		{
			OutIssues.Add(WarningIssue(FString::Printf(TEXT("Mesh '%s': part of its simple collision was left out: %s."), *Name, *FString::Join(LeftOut, TEXT(", "))),
				TEXT("It still publishes with the rest. Replace those shapes in the mesh editor's collision menu if they matter.")));
		}
		Out.bSimpleAsComplex = Flag == CTF_UseSimpleAsComplex;
		return Out;
	}
}

// ---------------------------------------------------------------------------------------------
// The material document

const TCHAR* FMantlePlaceMaterialDocument::SlotName(EMantlePlaceTextureSlot Slot)
{
	using namespace MantlePlaceAssetConverterPrivate;
	if (Slot == EMantlePlaceTextureSlot::WaterNormal2)
	{
		return WaterNormal2SlotKey;
	}
	const int32 Index = static_cast<int32>(Slot);
	const int32 Base = static_cast<int32>(UE_ARRAY_COUNT(SlotOrder));
	return Index < Base ? SlotOrder[Index] : LayerSlotKeys[FMath::Clamp(Index - Base, 0, 2)];
}

bool FMantlePlaceMaterialDocument::ToJson(const TMap<FString, FString>& TextureIds, FString& OutJson, FString& OutError) const
{
	using namespace MantlePlaceAssetConverterPrivate;
	OutJson.Reset();
	TMap<FString, FString> Ids;
	for (const TPair<FString, FString>& Slot : Textures)
	{
		const FString* Id = TextureIds.Find(Slot.Value);
		if (!Id || Id->IsEmpty())
		{
			OutError = FString::Printf(TEXT("the %s texture (%s) has not been uploaded yet"), *Slot.Key, *Slot.Value);
			return false;
		}
		if (!IsUploadedId(*Id))
		{
			OutError = FString::Printf(TEXT("the %s texture's id \"%s\" is not an uploaded asset id"), *Slot.Key, **Id);
			return false;
		}
		Ids.Add(Slot.Key, *Id);
	}
	OutJson = WriteDocument(*this, Ids);
	return true;
}

FString FMantlePlaceMaterialDocument::ToTemplateJson() const
{
	return MantlePlaceAssetConverterPrivate::WriteDocument(*this, Textures);
}

bool FMantlePlaceMaterialDocument::FromTemplateJson(const FString& Json, FMantlePlaceMaterialDocument& Out)
{
	using namespace MantlePlaceAssetConverterPrivate;
	Out = FMantlePlaceMaterialDocument();
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return false;
	}
	Object->TryGetStringField(TEXT("alphaMode"), Out.AlphaMode);
	Out.AlphaCutoff = NumberField(Object, TEXT("alphaCutoff"), 0.5);
	Object->TryGetBoolField(TEXT("doubleSided"), Out.bDoubleSided);
	NumberArray(Object, TEXT("baseColorFactor"), Out.BaseColorFactor, 4);
	Out.MetallicFactor = NumberField(Object, TEXT("metallicFactor"), 1.0);
	Out.RoughnessFactor = NumberField(Object, TEXT("roughnessFactor"), 1.0);
	NumberArray(Object, TEXT("emissiveFactor"), Out.EmissiveFactor, 3);
	Out.EmissiveStrength = NumberField(Object, TEXT("emissiveStrength"), 1.0);
	NumberArray(Object, TEXT("uvScale"), Out.UvScale, 2);
	NumberArray(Object, TEXT("uvOffset"), Out.UvOffset, 2);
	if (const TSharedPtr<FJsonObject> Transforms = ObjectField(Object, TEXT("uvTransforms")))
	{
		for (const auto& Pair : Transforms->Values)
		{
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
			{
				continue;
			}
			const TSharedPtr<FJsonObject> Entry = Pair.Value->AsObject();
			FMantlePlaceDocumentUvTransform Transform;
			NumberArray(Entry, TEXT("offset"), Transform.Offset, 2);
			NumberArray(Entry, TEXT("scale"), Transform.Scale, 2);
			Transform.Rotation = NumberField(Entry, TEXT("rotation"), 0.0);
			Out.UvTransforms.Add(FString(Pair.Key.ToView()), Transform);
		}
	}
	Out.NormalScale = NumberField(Object, TEXT("normalScale"), 1.0);
	Out.OcclusionStrength = NumberField(Object, TEXT("occlusionStrength"), 1.0);
	Out.OcclusionUV = NumberField(Object, TEXT("occlusionUV"), 0.0) == 1.0 ? 1 : 0;
	Object->TryGetStringField(TEXT("vertexColor"), Out.VertexColor);
	if (const TSharedPtr<FJsonObject> Panning = ObjectField(Object, TEXT("uvPanning")))
	{
		for (const auto& Pair : Panning->Values)
		{
			double Speed[2] = { 0.0, 0.0 };
			if (NumberArray(Panning, *FString(Pair.Key.ToView()), Speed, 2))
			{
				Out.UvPanning.Add(FString(Pair.Key.ToView()), FVector2D(Speed[0], Speed[1]));
			}
		}
	}
	if (const TSharedPtr<FJsonObject> Wind = ObjectField(Object, TEXT("wind")))
	{
		Out.bWind = true;
		Out.WindIntensity = NumberField(Wind, TEXT("intensity"), 0.0);
		Out.WindSpeed = NumberField(Wind, TEXT("speed"), 0.0);
		Wind->TryGetStringField(TEXT("weight"), Out.WindWeight);
	}
	if (const TSharedPtr<FJsonObject> Aligned = ObjectField(Object, TEXT("worldAligned")))
	{
		Out.bWorldAligned = true;
		Out.TileSizeCm = NumberField(Aligned, TEXT("tileSizeCm"), 100.0);
		Out.Sharpness = NumberField(Aligned, TEXT("sharpness"), 1.0);
	}
	if (const TSharedPtr<FJsonObject> Foliage = ObjectField(Object, TEXT("foliage")))
	{
		Out.bFoliage = true;
		NumberArray(Foliage, TEXT("tint"), Out.FoliageTint, 3);
		Out.FoliageStrength = NumberField(Foliage, TEXT("strength"), 1.0);
		Out.FoliageVertexColorWeight = NumberField(Foliage, TEXT("vertexColorWeight"), 0.0);
	}
	if (const TSharedPtr<FJsonObject> Water = ObjectField(Object, TEXT("water")))
	{
		Out.bWater = true;
		NumberArray(Water, TEXT("scattering"), Out.WaterScattering, 3);
		NumberArray(Water, TEXT("absorption"), Out.WaterAbsorption, 3);
		Out.WaterPhaseG = NumberField(Water, TEXT("phaseG"), 0.0);
		Out.WaterColorScaleBehindWater = NumberField(Water, TEXT("colorScaleBehindWater"), 1.0);
		auto ReadLayer = [](const TSharedPtr<FJsonObject>& Layer, FMantlePlaceDocumentWaterNormal& Into)
		{
			if (!Layer.IsValid())
			{
				return;
			}
			FString Space;
			Into.bWorld = Layer->TryGetStringField(TEXT("space"), Space) && Space.Equals(TEXT("world"), ESearchCase::CaseSensitive);
			NumberArray(Layer, TEXT("tiling"), Into.Tiling, 2);
			Into.WorldSizeCm = NumberField(Layer, TEXT("worldSizeCm"), 100.0);
			NumberArray(Layer, TEXT("panning"), Into.Panning, 2);
		};
		ReadLayer(ObjectField(Water, TEXT("normal")), Out.WaterNormal);
		const TSharedPtr<FJsonObject> Second = ObjectField(Water, TEXT("normal2"));
		ReadLayer(Second, Out.WaterNormal2);
		FString SecondTexture;
		if (Second.IsValid() && Second->TryGetStringField(TEXT("texture"), SecondTexture))
		{
			Out.Textures.Add(WaterNormal2SlotKey, SecondTexture);
		}
	}
	if (const TSharedPtr<FJsonObject> Layer = ObjectField(Object, TEXT("layerBlend")))
	{
		Out.bLayerBlend = true;
		Layer->TryGetStringField(TEXT("channel"), Out.LayerChannel);
		NumberArray(Layer, TEXT("baseColorFactor"), Out.LayerBaseColorFactor, 4);
		if (const TSharedPtr<FJsonObject> LayerTextures = ObjectField(Layer, TEXT("textures")))
		{
			for (int32 Index = 0; Index < static_cast<int32>(UE_ARRAY_COUNT(LayerDocumentKeys)); ++Index)
			{
				FString Key;
				if (LayerTextures->TryGetStringField(LayerDocumentKeys[Index], Key))
				{
					Out.Textures.Add(LayerSlotKeys[Index], Key);
				}
			}
		}
	}
	if (const TSharedPtr<FJsonObject> Textures = ObjectField(Object, TEXT("textures")))
	{
		// auto: FJsonObject::Values is keyed by a shared string in 5.8, so a typed TPair<FString, ...> would copy.
		for (const auto& Pair : Textures->Values)
		{
			FString Key;
			if (Pair.Value.IsValid() && Pair.Value->TryGetString(Key))
			{
				Out.Textures.Add(FString(Pair.Key.ToView()), Key);
			}
		}
	}
	FString Shading;
	Out.bCarPaint = Object->TryGetStringField(TEXT("shading"), Shading) && Shading.Equals(TEXT("carPaint"), ESearchCase::CaseSensitive);
	if (const TSharedPtr<FJsonObject> Paint = ObjectField(Object, TEXT("carPaint")))
	{
		const FMantleCarPaintFinish Defaults;
		Out.CarPaint.FlakeDensity = NumberField(Paint, TEXT("flakeDensity"), Defaults.FlakeDensity);
		Out.CarPaint.FlakeSize = NumberField(Paint, TEXT("flakeSize"), Defaults.FlakeSize);
		NumberArray(Paint, TEXT("flakeTint"), Out.CarPaint.FlakeTint, 3);
		Out.CarPaint.ClearCoat = NumberField(Paint, TEXT("clearCoat"), Defaults.ClearCoat);
		Out.CarPaint.ClearCoatRoughness = NumberField(Paint, TEXT("clearCoatRoughness"), Defaults.ClearCoatRoughness);
	}
	return true;
}

// ---------------------------------------------------------------------------------------------
// The asset set

bool FMantlePlaceAssetSet::HasErrors() const
{
	return Issues.ContainsByPredicate([](const FMantleValidationIssue& Issue) { return Issue.Severity == EMantleIssueSeverity::Error; });
}

const FMantleConvertedMesh* FMantlePlaceAssetSet::FindMesh(const FString& Key) const
{
	return Meshes.FindByPredicate([&Key](const FMantleConvertedMesh& Mesh) { return Mesh.Key == Key; });
}

const FMantleConvertedMaterial* FMantlePlaceAssetSet::FindMaterial(const FString& Key) const
{
	return Materials.FindByPredicate([&Key](const FMantleConvertedMaterial& Material) { return Material.Key == Key; });
}

const FMantleConvertedTexture* FMantlePlaceAssetSet::FindTexture(const FString& Key) const
{
	return Textures.FindByPredicate([&Key](const FMantleConvertedTexture& Texture) { return Texture.Key == Key; });
}

const FMantleConvertedSound* FMantlePlaceAssetSet::FindSound(const FString& Key) const
{
	return Sounds.FindByPredicate([&Key](const FMantleConvertedSound& Sound) { return Sound.Key == Key; });
}

const FMantleConvertedSkeletalMesh* FMantlePlaceAssetSet::FindSkeletalMesh(const FString& Key) const
{
	return SkeletalMeshes.FindByPredicate([&Key](const FMantleConvertedSkeletalMesh& Mesh) { return Mesh.Key == Key; });
}

const FMantleConvertedClip* FMantlePlaceAssetSet::FindClip(const FString& Key) const
{
	return Clips.FindByPredicate([&Key](const FMantleConvertedClip& Clip) { return Clip.Key == Key; });
}

const FMantleConvertedHeightfield* FMantlePlaceAssetSet::FindHeightfield(const FString& Key) const
{
	return Heightfields.FindByPredicate([&Key](const FMantleConvertedHeightfield& Heightfield) { return Heightfield.Key == Key; });
}

const FMantleConvertedVehicle* FMantlePlaceAssetSet::FindVehicle(const FString& Key) const
{
	return Vehicles.FindByPredicate([&Key](const FMantleConvertedVehicle& Vehicle) { return Vehicle.Key == Key; });
}

const FMantleConvertedWeapon* FMantlePlaceAssetSet::FindWeapon(const FString& Key) const
{
	return Weapons.FindByPredicate([&Key](const FMantleConvertedWeapon& Weapon) { return Weapon.Key == Key; });
}

void FMantlePlaceAssetSet::RecordUpload(const FString& Key, const FString& Id, const FString& Hash, int64 Bytes)
{
	FMantlePlaceUploadedRef& Ref = Uploaded.FindOrAdd(Key);
	Ref.Id = Id;
	Ref.Hash = Hash;
	Ref.Bytes = Bytes;
}

FString FMantlePlaceAssetSet::AddTexture(FMantleConvertedTexture&& Texture)
{
	const FString Key = Texture.Key;
	if (!FindTexture(Key))
	{
		Textures.Add(MoveTemp(Texture));
	}
	return Key;
}

bool FMantlePlaceAssetSet::MaterialBytes(const FMantleConvertedMaterial& Material, TArray<uint8>& OutBytes, FString& OutError) const
{
	TMap<FString, FString> Ids;
	for (const TPair<FString, FMantlePlaceUploadedRef>& Pair : Uploaded)
	{
		Ids.Add(Pair.Key, Pair.Value.Id);
	}
	// A local preview for a client that cannot read foliage shading (#628) gets the plain masked, two-sided material.
	FMantlePlaceMaterialDocument Document = Material.Document;
	FMantleTargetFeatures::Current().DowngradeMaterial(Document);
	FString Json;
	if (!Document.ToJson(Ids, Json, OutError))
	{
		OutError = FString::Printf(TEXT("Material '%s': %s"), *Material.Name, *OutError);
		return false;
	}
	OutBytes = MantlePlaceAssetConverterPrivate::Utf8Bytes(Json);
	return true;
}

bool FMantlePlaceAssetSet::VehicleBytes(const FMantleConvertedVehicle& Vehicle, TArray<uint8>& OutBytes, FString& OutError) const
{
	if (!Vehicle.Document.IsValid())
	{
		OutError = FString::Printf(TEXT("Vehicle '%s' was not prepared."), *Vehicle.Name);
		return false;
	}
	TArray<FString> Sections;
	Vehicle.SectionMaterialKeys.GetKeys(Sections);
	Sections.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
	TMap<FString, FString> Ids;
	for (const FString& Section : Sections)
	{
		const FString& MaterialKey = Vehicle.SectionMaterialKeys.FindChecked(Section);
		const FMantlePlaceUploadedRef* Ref = Uploaded.Find(MaterialKey);
		if (!Ref || Ref->Id.IsEmpty())
		{
			const FMantleConvertedMaterial* Material = FindMaterial(MaterialKey);
			OutError = FString::Printf(TEXT("Vehicle '%s' was not uploaded because its material '%s' could not be."), *Vehicle.Name,
				Material ? *Material->Name : *Section);
			return false;
		}
		Ids.Add(Section, Ref->Id);
	}
	TArray<FString> LiveryIds;
	if (!VehicleLiveryIds(Vehicle, LiveryIds, OutError))
	{
		return false;
	}
	// Its attached parts (#435 item 2): each mesh and each slot's material uploaded before it, as meshes and materials are.
	TArray<FMantleVehicleConverter::FPartIds> PartIds;
	for (int32 Index = 0; Index < Vehicle.Parts.Num(); ++Index)
	{
		const FMantleConvertedVehicle::FPart& Part = Vehicle.Parts[Index];
		FMantleVehicleConverter::FPartIds& Entry = PartIds.AddDefaulted_GetRef();
		const FMantlePlaceUploadedRef* MeshRef = Uploaded.Find(Part.MeshKey);
		if (!MeshRef || MeshRef->Id.IsEmpty())
		{
			const FMantleConvertedMesh* Mesh = FindMesh(Part.MeshKey);
			OutError = FString::Printf(TEXT("Vehicle '%s' was not uploaded because the mesh of its part %d ('%s') could not be."), *Vehicle.Name, Index,
				Mesh ? *Mesh->Name : *Part.MeshKey);
			return false;
		}
		Entry.Mesh = MeshRef->Id;
		Entry.Bone = Part.Bone;
		Entry.Transform = Part.Transform;
		Entry.PaintSlots = Part.PaintSlots;
		for (const FString& MaterialKey : Part.MaterialKeys)
		{
			if (MaterialKey.IsEmpty())
			{
				Entry.Materials.Add(FString());
				continue;
			}
			const FMantlePlaceUploadedRef* MaterialRef = Uploaded.Find(MaterialKey);
			if (!MaterialRef || MaterialRef->Id.IsEmpty())
			{
				const FMantleConvertedMaterial* Material = FindMaterial(MaterialKey);
				OutError = FString::Printf(TEXT("Vehicle '%s' was not uploaded because the material '%s' of its part %d could not be."), *Vehicle.Name,
					Material ? *Material->Name : *MaterialKey, Index);
				return false;
			}
			Entry.Materials.Add(MaterialRef->Id);
		}
	}
	// Its horn's sound (#546), uploaded before it as a SoundEmitter's is.
	FString HornSoundId;
	if (!Vehicle.HornSoundKey.IsEmpty())
	{
		const FMantlePlaceUploadedRef* SoundRef = Uploaded.Find(Vehicle.HornSoundKey);
		if (!SoundRef || SoundRef->Id.IsEmpty())
		{
			const FMantleConvertedSound* Sound = FindSound(Vehicle.HornSoundKey);
			OutError = FString::Printf(TEXT("Vehicle '%s' was not uploaded because its horn sound '%s' could not be."), *Vehicle.Name,
				Sound ? *Sound->Name : *Vehicle.HornSoundKey);
			return false;
		}
		HornSoundId = SoundRef->Id;
	}
	FString WriteError;
	const TSharedRef<FJsonObject> Document = FMantleVehicleConverter::WithHorn(FMantleVehicleConverter::WithParts(FMantleVehicleConverter::WithLiveries(
		FMantleVehicleConverter::WithMaterials(Vehicle.Document.ToSharedRef(), Ids), LiveryIds), PartIds), HornSoundId);
	if (!FMantleVehicleConverter::WriteUpload(Vehicle.Body, Document, OutBytes, WriteError))
	{
		OutError = FString::Printf(TEXT("Vehicle '%s' could not be written: %s"), *Vehicle.Name, *WriteError);
		return false;
	}
	return true;
}

bool FMantlePlaceAssetSet::WeaponBytes(const FMantleConvertedWeapon& Weapon, TArray<uint8>& OutBytes, FString& OutError) const
{
	const FMantlePlaceUploadedRef* MeshRef = Uploaded.Find(Weapon.MeshKey);
	if (!MeshRef || MeshRef->Id.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Weapon '%s' was not uploaded because its 3D model could not be."), *Weapon.Name);
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Weapon.DocumentText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = FString::Printf(TEXT("Weapon '%s' was not uploaded because its settings file cannot be read."), *Weapon.Name);
		return false;
	}
	Root->SetStringField(TEXT("mesh"), MeshRef->Id);
	// The hotbar icon is the library picture the weapon uploaded beside it (a colour texture). Create writes the icon's local name;
	// a weapon whose picture could not be taken or uploaded names none.
	const FMantlePlaceUploadedRef* IconRef = Weapon.ThumbnailKey.IsEmpty() ? nullptr : Uploaded.Find(Weapon.ThumbnailKey);
	if (IconRef && !IconRef->Id.IsEmpty())
	{
		Root->SetStringField(TEXT("icon"), IconRef->Id);
	}
	else
	{
		Root->RemoveField(TEXT("icon"));
	}
	// The slots' materials by uploaded id: the weapon's own, and (#898) each extra part's.
	auto SetMaterials = [this, &Weapon, &OutError](FJsonObject& Into, const TArray<FMantleConvertedWeapon::FSlot>& Slots) -> bool
	{
		if (Slots.Num() == 0)
		{
			return true;
		}
		TArray<TSharedPtr<FJsonValue>> SlotMaterials;
		for (const FMantleConvertedWeapon::FSlot& Slot : Slots)
		{
			const FMantlePlaceUploadedRef* MaterialRef = Uploaded.Find(Slot.MaterialKey);
			if (!MaterialRef || MaterialRef->Id.IsEmpty())
			{
				const FMantleConvertedMaterial* Material = FindMaterial(Slot.MaterialKey);
				OutError = FString::Printf(TEXT("Weapon '%s' was not uploaded because its material '%s' could not be."), *Weapon.Name,
					Material ? *Material->Name : *Slot.MaterialKey);
				return false;
			}
			SlotMaterials.Add(MakeShared<FJsonValueString>(MaterialRef->Id));
		}
		Into.SetArrayField(TEXT("materials"), SlotMaterials);
		return true;
	};
	if (!SetMaterials(*Root, Weapon.Slots))
	{
		return false;
	}
	// #898: each extra part names its uploaded model and its materials.
	const TArray<TSharedPtr<FJsonValue>>* Attachments = nullptr;
	const int32 NumAttachments = Root->TryGetArrayField(TEXT("attachments"), Attachments) && Attachments ? Attachments->Num() : 0;
	if (NumAttachments != Weapon.Attachments.Num())
	{
		OutError = FString::Printf(TEXT("Weapon '%s' was not uploaded because its settings file changed while it was publishing."), *Weapon.Name);
		return false;
	}
	for (int32 Index = 0; Index < NumAttachments; ++Index)
	{
		const FMantleConvertedWeapon::FAttachment& Part = Weapon.Attachments[Index];
		// #904: an effect-only attachment has no model (WeaponDocumentProblem's OutParts is null).
		if (Part.MeshKey.IsEmpty())
		{
			continue;
		}
		const TSharedPtr<FJsonObject>* Attachment = nullptr;
		const FMantlePlaceUploadedRef* PartRef = Uploaded.Find(Part.MeshKey);
		if (!(*Attachments)[Index]->TryGetObject(Attachment) || !Attachment || !PartRef || PartRef->Id.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Weapon '%s' was not uploaded because the model of its extra part %d could not be."), *Weapon.Name, Index + 1);
			return false;
		}
		(*Attachment)->SetStringField(TEXT("mesh"), PartRef->Id);
		if (!SetMaterials(**Attachment, Part.Slots))
		{
			return false;
		}
	}
	// #905: the round's model and its materials by uploaded id, as an extra part's.
	if (!Weapon.Projectile.MeshKey.IsEmpty())
	{
		const TSharedPtr<FJsonObject>* Projectile = nullptr;
		const FMantlePlaceUploadedRef* RoundRef = Uploaded.Find(Weapon.Projectile.MeshKey);
		if (!Root->TryGetObjectField(TEXT("projectile"), Projectile) || !Projectile || !RoundRef || RoundRef->Id.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Weapon '%s' was not uploaded because the model of its round or arrow could not be."), *Weapon.Name);
			return false;
		}
		(*Projectile)->SetStringField(TEXT("mesh"), RoundRef->Id);
		if (!SetMaterials(**Projectile, Weapon.Projectile.Slots))
		{
			return false;
		}
	}
	// #898: the attack sounds by uploaded id.
	const TSharedPtr<FJsonObject>* SoundsBlock = nullptr;
	for (const TPair<const TCHAR*, const FString*>& Sound : { TPair<const TCHAR*, const FString*>(TEXT("fire"), &Weapon.FireSoundKey),
		TPair<const TCHAR*, const FString*>(TEXT("fireSuppressed"), &Weapon.FireSuppressedSoundKey) })
	{
		if (Sound.Value->IsEmpty())
		{
			continue;
		}
		const FMantlePlaceUploadedRef* SoundRef = Uploaded.Find(*Sound.Value);
		if (!Root->TryGetObjectField(TEXT("sounds"), SoundsBlock) || !SoundsBlock || !SoundRef || SoundRef->Id.IsEmpty())
		{
			const FMantleConvertedSound* Converted = FindSound(*Sound.Value);
			OutError = FString::Printf(TEXT("Weapon '%s' was not uploaded because its attack sound '%s' could not be."), *Weapon.Name,
				Converted ? *Converted->Name : **Sound.Value);
			return false;
		}
		(*SoundsBlock)->SetStringField(Sound.Key, SoundRef->Id);
	}
	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	OutBytes = MantlePlaceAssetConverterPrivate::Utf8Bytes(Json);
	return true;
}

bool FMantlePlaceAssetSet::VehicleLiveryIds(const FMantleConvertedVehicle& Vehicle, TArray<FString>& OutIds, FString& OutError) const
{
	OutIds.Reset();
	for (int32 Index = 0; Index < Vehicle.LiveryTextureKeys.Num(); ++Index)
	{
		const FString* TextureKey = DecalTextures.Find(Vehicle.LiveryTextureKeys[Index]);
		const FMantlePlaceUploadedRef* Ref = TextureKey ? Uploaded.Find(*TextureKey) : nullptr;
		if (!Ref || Ref->Id.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Vehicle '%s' was not uploaded because its livery %d could not be."), *Vehicle.Name, Index);
			return false;
		}
		OutIds.Add(Ref->Id);
	}
	return true;
}

bool FMantlePlaceAssetSet::TakeUploads(EMantlePlaceUploadStage Stage, TArray<FMantlePlaceUpload>& OutUploads, FString& OutError)
{
	OutUploads.Reset();
	// #804: one file's upload. Its bytes move out of the set (or, spilled, its path goes), and its key is remembered so the
	// next call leaves it out. Null for a file an earlier call took.
	auto Take = [this, Stage, &OutUploads](const FString& Key, const TCHAR* Kind, const FString& Hash, const FString& Name, TArray<uint8>& Bytes) -> FMantlePlaceUpload*
	{
		if (TakenSizes.Contains(Key))
		{
			return nullptr;
		}
		FMantlePlaceUpload& Upload = OutUploads.AddDefaulted_GetRef();
		Upload.Key = Key;
		Upload.Stage = Stage;
		Upload.Options.Kind = Kind;
		Upload.Hash = Hash;
		Upload.SourceName = Name;
		if (const FMantleSpilledFile* Spill = Spilled.Find(Key))
		{
			Upload.File = Spill->Path;
			Upload.FileSize = Spill->Size;
			TakenSizes.Add(Key, Spill->Size);
		}
		else
		{
			TakenSizes.Add(Key, Bytes.Num());
			Upload.Bytes = MoveTemp(Bytes);
		}
		return &Upload;
	};
	switch (Stage)
	{
	case EMantlePlaceUploadStage::Texture:
		for (FMantleConvertedTexture& Texture : Textures)
		{
			if (FMantlePlaceUpload* Upload = Take(Texture.Key, TEXT("texture"), Texture.Hash, Texture.Name, Texture.Bytes))
			{
				Upload->Options.Role = Texture.Role;
			}
		}
		return true;
	case EMantlePlaceUploadStage::Material:
		for (const FMantleConvertedMaterial& Material : Materials)
		{
			FMantlePlaceUpload Upload;
			Upload.Key = Material.Key;
			Upload.Stage = Stage;
			Upload.Options.Kind = TEXT("material");
			if (!MaterialBytes(Material, Upload.Bytes, OutError))
			{
				OutUploads.Reset();
				return false;
			}
			Upload.Hash = FMantleSha256::AssetHash(Upload.Bytes);
			Upload.SourceName = Material.Name;
			OutUploads.Add(MoveTemp(Upload));
		}
		return true;
	case EMantlePlaceUploadStage::Sound:
		for (FMantleConvertedSound& Sound : Sounds)
		{
			Take(Sound.Key, TEXT("sound"), Sound.Hash, Sound.Name, Sound.Bytes);
		}
		return true;
	case EMantlePlaceUploadStage::SkeletalMesh:
		for (FMantleConvertedSkeletalMesh& Mesh : SkeletalMeshes)
		{
			Take(Mesh.Key, TEXT("skeletalmesh"), Mesh.Hash, Mesh.Name, Mesh.Bytes);
		}
		return true;
	case EMantlePlaceUploadStage::AnimClip:
		for (FMantleConvertedClip& Clip : Clips)
		{
			Take(Clip.Key, TEXT("animclip"), Clip.Hash, Clip.Name, Clip.Bytes);
		}
		return true;
	case EMantlePlaceUploadStage::Heightfield:
		for (FMantleConvertedHeightfield& Heightfield : Heightfields)
		{
			Take(Heightfield.Key, TEXT("heightfield"), Heightfield.Hash, Heightfield.Name, Heightfield.Bytes);
		}
		return true;
	case EMantlePlaceUploadStage::Vehicle:
		for (const FMantleConvertedVehicle& Vehicle : Vehicles)
		{
			FMantlePlaceUpload Upload;
			Upload.Key = Vehicle.Key;
			Upload.Stage = Stage;
			Upload.Options.Kind = TEXT("vehicle");
			if (!VehicleBytes(Vehicle, Upload.Bytes, OutError))
			{
				OutUploads.Reset();
				return false;
			}
			Upload.Hash = FMantleSha256::AssetHash(Upload.Bytes);
			Upload.SourceName = Vehicle.Name;
			OutUploads.Add(MoveTemp(Upload));
		}
		return true;
	case EMantlePlaceUploadStage::Weapon:
		for (const FMantleConvertedWeapon& Weapon : Weapons)
		{
			FMantlePlaceUpload Upload;
			Upload.Key = Weapon.Key;
			Upload.Stage = Stage;
			Upload.Options.Kind = TEXT("weapon");
			if (!WeaponBytes(Weapon, Upload.Bytes, OutError))
			{
				OutUploads.Reset();
				return false;
			}
			Upload.Hash = FMantleSha256::AssetHash(Upload.Bytes);
			Upload.SourceName = Weapon.Name;
			OutUploads.Add(MoveTemp(Upload));
		}
		return true;
	case EMantlePlaceUploadStage::Mesh:
	default:
		for (FMantleConvertedMesh& Mesh : Meshes)
		{
			Take(Mesh.Key, TEXT("mesh"), Mesh.Hash, Mesh.Name, Mesh.Bytes);
		}
		return true;
	}
}

int64 FMantlePlaceAssetSet::UploadSize(const FString& Key, const int64 InMemory) const
{
	if (const int64* Taken = TakenSizes.Find(Key))
	{
		return *Taken;
	}
	if (const FMantleSpilledFile* Spill = Spilled.Find(Key))
	{
		return Spill->Size;
	}
	return InMemory;
}

int32 FMantlePlaceAssetSet::SpillBytes(const FString& Directory, const int64 MinBytes, FString& OutError)
{
	OutError.Reset();
	int32 Written = 0;
	IFileManager& Files = IFileManager::Get();
	Files.MakeDirectory(*Directory, /*Tree*/ true);
	// One file per hash: equal bytes under two keys are one file on disk.
	auto Spill = [this, &Directory, MinBytes, &OutError, &Written, &Files](const FString& Key, const FString& Hash, TArray<uint8>& Bytes)
	{
		if (Bytes.Num() == 0 || Bytes.Num() < MinBytes || Hash.IsEmpty() || TakenSizes.Contains(Key) || Spilled.Contains(Key))
		{
			return;
		}
		FString Name = Hash;
		Name.ReplaceInline(TEXT(":"), TEXT("-"));
		const FString Path = FPaths::Combine(Directory, Name + TEXT(".bin"));
		const int64 Size = Bytes.Num();
		if (Files.FileSize(*Path) != Size)
		{
			if (!FFileHelper::SaveArrayToFile(Bytes, *Path) || Files.FileSize(*Path) != Size)
			{
				if (OutError.IsEmpty())
				{
					OutError = FString::Printf(TEXT("a converted file could not be written to %s"), *Path);
				}
				Files.Delete(*Path, /*bRequireExists*/ false, /*bEvenReadOnly*/ true, /*bQuiet*/ true);
				return;
			}
			++Written;
		}
		FMantleSpilledFile& File = Spilled.Add(Key);
		File.Path = Path;
		File.Size = Size;
		Bytes.Empty();
	};
	for (FMantleConvertedTexture& Texture : Textures)
	{
		Spill(Texture.Key, Texture.Hash, Texture.Bytes);
	}
	for (FMantleConvertedMesh& Mesh : Meshes)
	{
		Spill(Mesh.Key, Mesh.Hash, Mesh.Bytes);
	}
	for (FMantleConvertedSound& Sound : Sounds)
	{
		Spill(Sound.Key, Sound.Hash, Sound.Bytes);
	}
	for (FMantleConvertedSkeletalMesh& Mesh : SkeletalMeshes)
	{
		Spill(Mesh.Key, Mesh.Hash, Mesh.Bytes);
	}
	for (FMantleConvertedClip& Clip : Clips)
	{
		Spill(Clip.Key, Clip.Hash, Clip.Bytes);
	}
	for (FMantleConvertedHeightfield& Heightfield : Heightfields)
	{
		Spill(Heightfield.Key, Heightfield.Hash, Heightfield.Bytes);
	}
	return Written;
}

int64 FMantlePlaceAssetSet::HeldBytes() const
{
	int64 Held = 0;
	for (const FMantleConvertedTexture& Each : Textures) { Held += Each.Bytes.Num(); }
	for (const FMantleConvertedMesh& Each : Meshes) { Held += Each.Bytes.Num(); }
	for (const FMantleConvertedSound& Each : Sounds) { Held += Each.Bytes.Num(); }
	for (const FMantleConvertedSkeletalMesh& Each : SkeletalMeshes) { Held += Each.Bytes.Num(); }
	for (const FMantleConvertedClip& Each : Clips) { Held += Each.Bytes.Num(); }
	for (const FMantleConvertedHeightfield& Each : Heightfields) { Held += Each.Bytes.Num(); }
	return Held;
}

void FMantlePlaceAssetSet::ReleaseBytes()
{
	for (FMantleConvertedTexture& Texture : Textures)
	{
		Texture.Bytes.Empty();
	}
	for (FMantleConvertedMesh& Mesh : Meshes)
	{
		Mesh.Bytes.Empty();
	}
	for (FMantleConvertedSound& Sound : Sounds)
	{
		Sound.Bytes.Empty();
	}
	for (FMantleConvertedSkeletalMesh& Mesh : SkeletalMeshes)
	{
		Mesh.Bytes.Empty();
	}
	for (FMantleConvertedClip& Clip : Clips)
	{
		Clip.Bytes.Empty();
	}
	for (FMantleConvertedHeightfield& Heightfield : Heightfields)
	{
		Heightfield.Bytes.Empty();
	}
	for (FMantleConvertedVehicle& Vehicle : Vehicles)
	{
		Vehicle.Body = FMantleGlbMesh();
	}
}

// ---------------------------------------------------------------------------------------------
// GLTFExporter

bool FMantleGltfMaterialFlattener::Flatten(UMaterialInterface* Material, const FMantleFlattenSettings& Settings, FMantleGlbDocument& OutDocument,
	TArray<FString>& OutMessages, FString& OutError)
{
	OutDocument = FMantleGlbDocument();
	if (!Material)
	{
		OutError = TEXT("there is no material");
		return false;
	}
	TStrongObjectPtr<UGLTFExportOptions> Options(NewObject<UGLTFExportOptions>());
	Options->ResetToDefault();
	Options->bExportPreviewMesh = false;
	Options->bIncludeCopyrightNotice = false;
	Options->bExportProxyMaterials = false;
	Options->bUseImporterMaterialMapping = false;
	Options->bExportUnlitMaterials = true;
	Options->bExportClearCoatMaterials = false;
	Options->bExportClothMaterials = false;
	Options->bExportThinTranslucentMaterials = false;
	Options->bExportSpecularGlossinessMaterials = false;
	Options->bExportEmissiveStrength = true;
	Options->BakeMaterialInputs = EGLTFMaterialBakeMode::Simple;
	Options->DefaultMaterialBakeSize.X = Settings.DefaultBakeSize;
	Options->DefaultMaterialBakeSize.Y = Settings.DefaultBakeSize;
	Options->DefaultMaterialBakeSize.bAutoDetect = true;
	Options->TextureImageFormat = EGLTFTextureImageFormat::PNG;
	Options->bExportTextureTransforms = true;
	Options->bExportLightmaps = false;
	Options->bAdjustNormalmaps = true;
	Options->bExportVertexColors = false;
	Options->bExportLights = false;
	Options->bExportCameras = false;

	FString SafeName = Material->GetName();
	for (TCHAR& Character : SafeName)
	{
		Character = FChar::IsAlnum(Character) ? Character : TEXT('_');
	}
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Mantle"), TEXT("AssetCache"), TEXT("Bake"),
		FString::Printf(TEXT("%s_%s.glb"), *SafeName, *FGuid::NewGuid().ToString(EGuidFormats::Digits))));

	FGLTFExportMessages Messages;
	const bool bExported = UGLTFExporter::ExportToGLTF(Material, Path, Options.Get(), TSet<AActor*>(), Messages);
	OutMessages.Append(Messages.Warnings);
	OutMessages.Append(Messages.Errors);
	TArray<uint8> Bytes;
	const bool bRead = bExported && FFileHelper::LoadFileToArray(Bytes, *Path);
	IFileManager::Get().Delete(*Path, /*bRequireExists*/ false, /*bEvenReadOnly*/ true, /*bQuiet*/ true);
	if (!bExported)
	{
		OutError = Messages.Errors.Num() > 0 ? Messages.Errors[0] : FString(TEXT("GLTFExporter could not export it"));
		return false;
	}
	if (!bRead || !FMantleGlbDocument::Parse(Bytes, OutDocument, OutError))
	{
		OutError = OutError.IsEmpty() ? FString(TEXT("GLTFExporter's file could not be read")) : OutError;
		return false;
	}
	return true;
}

bool FMantleGltfMaterialFlattener::CanBakeTextures() const
{
	return FApp::CanEverRender() && !GUsingNullRHI;
}

FString FMantleGltfMaterialFlattener::Describe() const
{
	// Without a renderer GLTFExporter bakes nothing and writes no images: never let that result stand in for a real one.
	const bool bCanRender = CanBakeTextures();
	return FString::Printf(TEXT("gltfexporter %s simple png adjustnormals transforms emissivestrength unlit render=%d"),
		*FEngineVersion::Current().ToString(EVersionComponent::Patch), bCanRender ? 1 : 0);
}

// ---------------------------------------------------------------------------------------------
// The converter

FMantlePlaceAssetConverter::FMantlePlaceAssetConverter()
	: FMantlePlaceAssetConverter(MakeShared<FMantleGltfMaterialFlattener>(), DefaultCacheDirectory())
{
}

FMantlePlaceAssetConverter::FMantlePlaceAssetConverter(TSharedRef<IMantleMaterialFlattener> InFlattener, const FString& InCacheDirectory)
	: Limits(FMantlePlaceAssetLimits::FromContract())
	, Flattener(InFlattener)
	, CacheDirectory(InCacheDirectory)
{
}

FString FMantlePlaceAssetConverter::DefaultCacheDirectory()
{
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Mantle"), TEXT("AssetCache")));
}

bool FMantlePlaceAssetConverter::ComputeCacheKey(const FString& Salt, const TArray<const UObject*>& Objects, FString& OutKey, FString& OutWhyNot)
{
	OutKey.Reset();
	OutWhyNot.Reset();
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	TArray<FName> Queue;
	for (const UObject* Object : Objects)
	{
		if (Object)
		{
			Queue.Add(Object->GetOutermost()->GetFName());
		}
	}
	TSet<FName> Seen;
	TArray<FString> Lines;
	while (Queue.Num() > 0)
	{
		const FName Name = Queue.Pop(EAllowShrinking::No);
		if (Seen.Contains(Name))
		{
			continue;
		}
		Seen.Add(Name);
		if (Seen.Num() > 1024)
		{
			OutWhyNot = TEXT("it depends on more than 1,024 packages");
			return false;
		}
		const FString PackageName = Name.ToString();
		if (PackageName.StartsWith(TEXT("/Script/")))
		{
			continue;
		}

		FString Hash;
		if (const UPackage* Package = FindPackage(nullptr, *PackageName))
		{
			if (Package->IsDirty())
			{
				OutWhyNot = FString::Printf(TEXT("%s has unsaved changes"), *PackageName);
				return false;
			}
			const FIoHash Saved = Package->GetSavedHash();
			if (!Saved.IsZero())
			{
				Hash = LexToString(Saved);
			}
		}
		else if (const TOptional<FAssetPackageData> Data = Registry.GetAssetPackageDataCopy(Name))
		{
			const FIoHash Saved = Data->GetPackageSavedHash();
			if (!Saved.IsZero())
			{
				Hash = LexToString(Saved);
			}
		}
		if (Hash.IsEmpty())
		{
			// Content saved before packages carried a hash: the file itself.
			FString Filename;
			if (FPackageName::DoesPackageExist(PackageName, &Filename))
			{
				const FMD5Hash FileHash = FMD5Hash::HashFile(*Filename);
				if (FileHash.IsValid())
				{
					Hash = TEXT("md5:") + LexToString(FileHash);
				}
			}
		}
		if (Hash.IsEmpty())
		{
			OutWhyNot = FString::Printf(TEXT("%s has never been saved"), *PackageName);
			return false;
		}
		Lines.Add(PackageName + TEXT("=") + Hash);

		TArray<FName> Dependencies;
		Registry.GetDependencies(Name, Dependencies, UE::AssetRegistry::EDependencyCategory::Package,
			UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Hard));
		Queue.Append(Dependencies);
	}
	Lines.Sort();
	const FString Text = FString::Printf(TEXT("%s\n%s\n%s"), MantlePlaceAssetConverterPrivate::CacheVersion, *Salt, *FString::Join(Lines, TEXT("\n")));
	OutKey = FMantleSha256::HexDigest(MantlePlaceAssetConverterPrivate::Utf8Bytes(Text));
	return true;
}

bool FMantlePlaceAssetConverter::Convert(const FMantlePlaceDependencies& Dependencies, FMantlePlaceAssetSet& Out)
{
	using namespace MantlePlaceAssetConverterPrivate;
	const int32 Total = NumDependencySteps(Dependencies);
	int32 Done = 0;
	auto Continue = [this, &Done, Total, &Out](const FString& Name)
	{
		if (OnProgress && !OnProgress(Done, Total, Name))
		{
			Out.Issues.Add(ErrorIssue(TEXT("Preparing this place's meshes and materials was cancelled."), TEXT("Publish again to carry on; what was already converted is kept.")));
			return false;
		}
		++Done;
		return true;
	};
	for (const FMantleGatheredMesh& Mesh : Dependencies.Meshes)
	{
		if (!Continue(Mesh.Name))
		{
			return false;
		}
		ConvertMesh(Mesh.Mesh.Get(), Out);
	}
	for (const FMantleGatheredMaterial& Material : Dependencies.Materials)
	{
		if (!Continue(Material.Name))
		{
			return false;
		}
		ConvertMaterial(Material.Material.Get(), Out);
	}
	for (const FMantleGatheredTexture& Texture : Dependencies.Textures)
	{
		if (!Continue(Texture.Name))
		{
			return false;
		}
		ConvertDecalTexture(Texture, Out);
	}
	for (const FMantleGatheredSound& Sound : Dependencies.Sounds)
	{
		if (!Continue(Sound.Name))
		{
			return false;
		}
		ConvertSound(Sound.Sound.Get(), Out);
	}
	for (const FMantleGatheredSkeletalMesh& Mesh : Dependencies.SkeletalMeshes)
	{
		if (!Continue(Mesh.Name))
		{
			return false;
		}
		ConvertSkeletalMesh(Mesh.Mesh.Get(), Out);
	}
	for (const FMantleGatheredClip& Clip : Dependencies.Clips)
	{
		if (!Continue(Clip.Name))
		{
			return false;
		}
		ConvertClip(Clip.Sequence.Get(), Clip.Mesh.Get(), Out);
	}
	for (const FMantleGatheredTerrain& Terrain : Dependencies.Terrains)
	{
		if (!Continue(Terrain.Name))
		{
			return false;
		}
		ConvertTerrain(Terrain.Landscape.Get(), Out);
	}
	for (const FMantleGatheredVehicle& Vehicle : Dependencies.Vehicles)
	{
		if (!Continue(Vehicle.Name))
		{
			return false;
		}
		ConvertVehicle(Vehicle.Vehicle.Get(), Out);
	}
	for (const FMantleGatheredWeapon& Weapon : Dependencies.Weapons)
	{
		if (!Continue(Weapon.Name))
		{
			return false;
		}
		ConvertWeapon(Weapon, Out);
	}
	if (OnProgress)
	{
		OnProgress(Done, Total, FString());
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_F5 converted %d mesh(es) (%d from cache) and %d material(s) (%d from cache), %d texture(s), %d sound(s), %d flattener call(s), %d issue(s)"),
		Stats.MeshesConverted + Stats.MeshesFromCache, Stats.MeshesFromCache, Stats.MaterialsConverted + Stats.MaterialsFromCache, Stats.MaterialsFromCache,
		Out.Textures.Num(), Out.Sounds.Num(), Stats.FlattenerCalls, Out.Issues.Num());
	return !Out.HasErrors();
}

bool FMantlePlaceAssetConverter::ConvertTerrain(AActor* Landscape, FMantlePlaceAssetSet& Out)
{
	using namespace MantlePlaceAssetConverterPrivate;
	if (!Landscape)
	{
		Out.Issues.Add(ErrorIssue(TEXT("A landscape this place uses could not be loaded."), TEXT("Check the level for missing assets, then try again.")));
		return false;
	}
	const FString Key = FMantleTerrainExport::HeightfieldKey(Landscape);
	if (Out.FindHeightfield(Key))
	{
		return true;
	}
	const FString Label = Landscape->GetActorLabel();
	FMantleTerrainSource Source;
	FString Problem;
	if (!FMantleTerrainExport::ReadLandscape(Landscape, Source, Problem))
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Landscape '%s' %s"), *Label, *Problem), TEXT("Fix it in the landscape's Details panel, then publish again.")));
		return false;
	}
	const bool bTerrain = ConvertTerrainSource(Landscape->GetWorld(), Source, Key, FMantleTerrainExport::WeightmapKey(Landscape), Label, Landscape->GetPathName(), Out);
	// Its grass maps (#628): one RGBA texture per four grass types, uploaded with the textures.
	return bTerrain && FMantleTerrainGrassExport::ConvertMaps(Landscape, Source, Limits, Out);
}

bool FMantlePlaceAssetConverter::ConvertTerrainSource(UWorld* World, const FMantleTerrainSource& Source, const FString& Key,
	const FString& WeightmapKey, const FString& Label, const FString& Path, FMantlePlaceAssetSet& Out)
{
	using namespace MantlePlaceAssetConverterPrivate;
	TArray<FMantleTerrainLayerChoice> Layers;
	TArray<FMantleValidationIssue> LayerIssues;
	if (!FMantleTerrainExport::ChooseLayers(World, Source, Label, Layers, LayerIssues))
	{
		Out.Issues.Append(LayerIssues);
		return false;
	}
	// The warnings too (#628): layers folded together, what the landscape's material does that is not kept.
	Out.Issues.Append(LayerIssues);
	const FMantleHeightfield Field = FMantleTerrainExport::MakeHeightfield(Source);
	TArray<uint8> Bytes = FMantleHeightfieldReader::Write(Field);
	if (Bytes.IsEmpty())
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Landscape '%s' could not be turned into a heightfield."), *Label),
			TEXT("This is a Mantle bug rather than a problem with your level. Please report it.")));
		return false;
	}
	if (Field.Width != Source.Width || Field.Height != Source.Height)
	{
		Out.Issues.Add(WarningIssue(FString::Printf(TEXT("Landscape '%s' is %d x %d samples; players get it resampled to %d x %d, %.0f x %.0f cm apart."),
			*Label, Source.Width, Source.Height, Field.Width, Field.Height, Field.SpacingX, Field.SpacingY),
			TEXT("Nothing to do. For every sample, make the landscape smaller or split the world into more than one place.")));
	}

	FMantleConvertedHeightfield Converted;
	Converted.Key = Key;
	Converted.Name = Label;
	Converted.Path = Path;
	Converted.Hash = FMantleSha256::AssetHash(Bytes);
	Converted.Bytes = MoveTemp(Bytes);
	Converted.Width = Field.Width;
	Converted.Height = Field.Height;

	// More than one layer: where each is painted, one RGBA texture over the whole terrain (a second for layers 4 to 7, #628),
	// lossless (no role, so the cook keeps the PNG), halved until the moderation classifier takes them. Layers folded into a kept
	// one paint into its channel, and a baked slope overlay has a channel of its own (FMantleTerrainExport::MakeWeightmaps).
	if (Layers.Num() > 1)
	{
		FIntPoint Size;
		TArray<uint8> Png;
		TArray<uint8> Png2;
		if (!FMantleTerrainExport::EncodeWeightmaps(Source, Layers, Limits.MaxTextureSize, Limits.MaxTextureBytes, Size, Png, Png2))
		{
			Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Landscape '%s''s paint layers could not be made into an image Mantle takes."), *Label),
				TEXT("Paint the layers with fewer, broader strokes, then publish again.")));
			return false;
		}
		FMantleConvertedTexture Texture;
		Texture.Key = WeightmapKey;
		Texture.Name = FString::Printf(TEXT("%s (paint layers)"), *Label);
		Texture.Format = TEXT("png");
		Texture.Width = Size.X;
		Texture.Height = Size.Y;
		Texture.Hash = FMantleSha256::AssetHash(Png);
		Texture.Bytes = MoveTemp(Png);
		Converted.WeightmapKey = Out.AddTexture(MoveTemp(Texture));
		if (!Png2.IsEmpty())
		{
			FMantleConvertedTexture Texture2;
			Texture2.Key = FMantleTerrainExport::Weightmap2KeyForPath(Path);
			Texture2.Name = FString::Printf(TEXT("%s (paint layers 5 to 8)"), *Label);
			Texture2.Format = TEXT("png");
			Texture2.Width = Size.X;
			Texture2.Height = Size.Y;
			Texture2.Hash = FMantleSha256::AssetHash(Png2);
			Texture2.Bytes = MoveTemp(Png2);
			Out.AddTexture(MoveTemp(Texture2));
		}
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_TERRAIN converted landscape %s: %d x %d samples, %d layer(s), %d bytes"),
		*Label, Converted.Width, Converted.Height, Layers.Num(), Converted.Bytes.Num());
	Out.Heightfields.Add(MoveTemp(Converted));
	return true;
}

bool FMantlePlaceAssetConverter::WriteSoundWav(const TConstArrayView<uint8> Pcm16, const int32 SampleRate, const int32 Channels, TArray<uint8>& OutWav)
{
	OutWav.Reset();
	const int64 FrameBytes = static_cast<int64>(Channels) * 2;
	const int64 DataBytes = Pcm16.Num();
	// RIFF sizes are uint32; the cook's cap is far below that, but a writer never wraps one.
	if (Channels <= 0 || SampleRate <= 0 || DataBytes <= 0 || DataBytes % FrameBytes != 0 || DataBytes + 36 > MAX_int32)
	{
		return false;
	}
	OutWav.Reserve(static_cast<int32>(44 + DataBytes));
	auto PutTag = [&OutWav](const char* Chars) { OutWav.Append(reinterpret_cast<const uint8*>(Chars), 4); };
	auto PutU16 = [&OutWav](const uint32 Value)
	{
		OutWav.Add(static_cast<uint8>(Value & 0xFF));
		OutWav.Add(static_cast<uint8>((Value >> 8) & 0xFF));
	};
	auto PutU32 = [&OutWav](const uint32 Value)
	{
		for (int32 Shift = 0; Shift < 32; Shift += 8)
		{
			OutWav.Add(static_cast<uint8>((Value >> Shift) & 0xFF));
		}
	};
	PutTag("RIFF");
	PutU32(static_cast<uint32>(36 + DataBytes));
	PutTag("WAVE");
	PutTag("fmt ");
	PutU32(16);
	PutU16(1); // PCM
	PutU16(static_cast<uint32>(Channels));
	PutU32(static_cast<uint32>(SampleRate));
	PutU32(static_cast<uint32>(SampleRate * FrameBytes)); // byte rate
	PutU16(static_cast<uint32>(FrameBytes)); // block align
	PutU16(16); // bits per sample
	PutTag("data");
	PutU32(static_cast<uint32>(DataBytes));
	OutWav.Append(Pcm16.GetData(), static_cast<int32>(DataBytes));
	return true;
}

bool FMantlePlaceAssetConverter::ConvertSound(USoundWave* Sound, FMantlePlaceAssetSet& Out)
{
	using namespace MantlePlaceAssetConverterPrivate;
	if (!Sound)
	{
		Out.Issues.Add(ErrorIssue(TEXT("A sound this place uses could not be loaded."), TEXT("Check the level for missing assets, then try again.")));
		return false;
	}
	const FString Key = FMantlePlaceAssets::SoundKey(Sound);
	if (Out.FindSound(Key))
	{
		return true;
	}
	const FString Name = Sound->GetName();

	// The same rule Check this place applies, then what the samples themselves say.
	FString Why, Fix;
	if (!FMantlePlaceAssets::CheckSoundRule(Sound, Limits, Why, Fix))
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Sound '%s' %s."), *Name, *Why), Fix));
		return false;
	}
	TArray<uint8> Pcm;
	uint32 SampleRate = 0;
	uint16 Channels = 0;
	const bool bRead = Sound->GetImportedSoundWaveData(Pcm, SampleRate, Channels);
	if (!bRead || Pcm.Num() == 0 || Channels == 0 || SampleRate == 0)
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Sound '%s' has no imported audio in this project to publish."), *Name),
			TEXT("Re-import it from its WAV file (Reimport in the Content Browser), then try again.")));
		return false;
	}
	if (Channels > 2)
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Sound '%s' has %d channels, and Mantle plays mono or stereo sounds."), *Name, static_cast<int32>(Channels)),
			TEXT("Export it from your audio editor as mono or stereo, re-import it, then try again.")));
		return false;
	}
	if (static_cast<int64>(SampleRate) < Limits.MinSoundSampleRate || static_cast<int64>(SampleRate) > Limits.MaxSoundSampleRate)
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Sound '%s' is recorded at %u Hz, and Mantle plays sounds at %d to %d Hz."), *Name, SampleRate,
			Limits.MinSoundSampleRate, Limits.MaxSoundSampleRate), TEXT("Export it at 44,100 or 48,000 Hz, re-import it, then try again.")));
		return false;
	}
	const int64 FrameBytes = static_cast<int64>(Channels) * 2;
	const int64 Frames = Pcm.Num() / FrameBytes;
	const double Seconds = static_cast<double>(Frames) / SampleRate;
	if (Seconds > Limits.MaxSoundSeconds)
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Sound '%s' is %.1f seconds long, and a sound may be at most %g seconds."), *Name, Seconds, Limits.MaxSoundSeconds),
			FString::Printf(TEXT("Trim it to %g seconds or less (a loop needs only one pass), re-import it, then try again."), Limits.MaxSoundSeconds)));
		return false;
	}

	FMantleConvertedSound Converted;
	// Whole frames only: a trailing partial frame (never written by Unreal's importer) is dropped.
	const TConstArrayView<uint8> Whole(Pcm.GetData(), static_cast<int32>(Frames * FrameBytes));
	if (!WriteSoundWav(Whole, static_cast<int32>(SampleRate), static_cast<int32>(Channels), Converted.Bytes))
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Sound '%s' could not be written as a WAV file."), *Name),
			TEXT("Re-import the sound and try again; if it keeps failing, report it with the log.")));
		return false;
	}
	if (Converted.Bytes.Num() > Limits.MaxSoundSourceBytes)
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Sound '%s' is %.1f MB as a WAV file; a sound may be at most %.0f MB."), *Name,
			Converted.Bytes.Num() / (1024.0 * 1024.0), Limits.MaxSoundSourceBytes / (1024.0 * 1024.0)),
			TEXT("Shorten it, or make it mono or a lower sample rate, re-import it, then try again.")));
		return false;
	}
	Converted.Key = Key;
	Converted.Name = Name;
	Converted.Path = Sound->GetPathName();
	Converted.Hash = FMantleSha256::AssetHash(Converted.Bytes);
	Converted.SampleRate = static_cast<int32>(SampleRate);
	Converted.Channels = static_cast<int32>(Channels);
	Converted.Frames = static_cast<int32>(Frames);
	++Stats.SoundsConverted;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_F5 sound %s: %d Hz, %d channel(s), %.2f s, %d bytes as WAV"), *Name, Converted.SampleRate, Converted.Channels,
		Seconds, Converted.Bytes.Num());
	Out.Sounds.Add(MoveTemp(Converted));
	return true;
}

bool FMantlePlaceAssetConverter::ConvertSkeletalMesh(USkeletalMesh* Mesh, FMantlePlaceAssetSet& Out)
{
	using namespace MantlePlaceAssetConverterPrivate;
	if (!Mesh)
	{
		Out.Issues.Add(ErrorIssue(TEXT("A skeletal mesh this place uses could not be loaded."), TEXT("Check the level for missing assets, then try again.")));
		return false;
	}
	const FString Key = FMantlePlaceAssets::SkeletalMeshKey(Mesh);
	if (Out.FindSkeletalMesh(Key))
	{
		return true;
	}
	const FString Name = Mesh->GetName();

	// The same limits Check this place applies (and the cook enforces).
	TArray<FMantleValidationIssue> Checked;
	FMantlePlaceAssets::CheckSkeletalMesh(Mesh, FString(), Limits, Checked);
	bool bRefused = false;
	for (FMantleValidationIssue& Issue : Checked)
	{
		bRefused = bRefused || Issue.Severity == EMantleIssueSeverity::Error;
		Out.Issues.Add(MoveTemp(Issue));
	}
	if (bRefused)
	{
		return false;
	}

	FMantleGlbMesh Glb;
	FMantleConvertedSkeletalMesh Converted;
	int32 CappedVertices = 0;
	float LargestDropped = 0.0f;
	FString Error;
	if (!FMantleCosmeticExport::ReadSkeletalMesh(Mesh, 0, Glb, Converted.Slots, Converted.Triangles, CappedVertices, LargestDropped, Error))
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Skeletal mesh '%s' could not be read: %s"), *Name, *Error),
			TEXT("Open the mesh in the editor once (or re-import it), then try again.")));
		return false;
	}
	if (CappedVertices > 0)
	{
		// #484: the eight heaviest influences are kept and renormalised; a visible change is worth a warning.
		Out.Issues.Add(WarningIssue(FString::Printf(TEXT("Skeletal mesh '%s' has %d vertices weighted to more than 8 bones; each keeps its 8 heaviest (the most any lost was %.0f%% of its weight)."),
			*Name, CappedVertices, LargestDropped * 100.0f),
			TEXT("If it deforms differently in the published place, limit its skin to 8 influences a vertex in your modelling tool.")));
	}
	if (!FMantleGlbWriter::Write(Glb, Converted.Bytes, Error))
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Skeletal mesh '%s' could not be written as a GLB file: %s"), *Name, *Error),
			TEXT("Re-import the mesh and try again; if it keeps failing, report it with the log.")));
		return false;
	}
	if (Converted.Bytes.Num() > Limits.MaxUploadBytes)
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Skeletal mesh '%s' is %.1f MB as a GLB file; an upload may be at most %.0f MB."), *Name,
			Converted.Bytes.Num() / (1024.0 * 1024.0), Limits.MaxUploadBytes / (1024.0 * 1024.0)), TEXT("Use a lower-detail version of it.")));
		return false;
	}
	Converted.Key = Key;
	Converted.Name = Name;
	Converted.Path = Mesh->GetPathName();
	Converted.Hash = FMantleSha256::AssetHash(Converted.Bytes);
	Converted.Bones = Mesh->GetRefSkeleton().GetRawBoneNum();
	Converted.SkeletonHash = FMantleSkeletonHash::Compute(Mesh->GetRefSkeleton());
	++Stats.SkeletalMeshesConverted;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_F5 skeletal mesh %s: %d bones (skeleton %s), %d triangles, %d slot(s), %d bytes as GLB"), *Name, Converted.Bones,
		*Converted.SkeletonHash, Converted.Triangles, Converted.Slots.Num(), Converted.Bytes.Num());
	Out.SkeletalMeshes.Add(MoveTemp(Converted));
	return true;
}

bool FMantlePlaceAssetConverter::ConvertClip(UAnimSequence* Sequence, USkeletalMesh* Mesh, FMantlePlaceAssetSet& Out)
{
	using namespace MantlePlaceAssetConverterPrivate;
	if (!Sequence || !Mesh)
	{
		Out.Issues.Add(ErrorIssue(TEXT("An animation this place plays, or the skeletal mesh it plays on, could not be loaded."),
			TEXT("Check the level for missing assets, then try again.")));
		return false;
	}
	const FString Key = FMantlePlaceAssets::ClipKey(Sequence, Mesh);
	if (Out.FindClip(Key))
	{
		return true;
	}
	const FString Name = Sequence->GetName();

	TArray<FMantleValidationIssue> Checked;
	FMantlePlaceAssets::CheckClip(Sequence, FString(), Limits, Checked);
	bool bRefused = false;
	for (FMantleValidationIssue& Issue : Checked)
	{
		bRefused = bRefused || Issue.Severity == EMantleIssueSeverity::Error;
		Out.Issues.Add(MoveTemp(Issue));
	}
	if (bRefused)
	{
		return false;
	}

	FMantleAnimClipData Clip;
	FString Error;
	if (!FMantleAnimClipWriter::SampleOnMesh(Sequence, Mesh, FMantlePlaceAssetLimits::ClipFrameRate, Clip, Error))
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Animation '%s' could not be read: %s"), *Name, *Error),
			TEXT("Open the animation in the editor once (or re-import it), then try again.")));
		return false;
	}
	FMantleConvertedClip Converted;
	Converted.Bytes = FMantleAnimClipWriter::Encode(Clip);
	if (Converted.Bytes.Num() > Limits.MaxClipCookedBytes)
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Animation '%s' on '%s' is %.1f MB as a Mantle clip; a clip may be at most %.0f MB."), *Name, *Mesh->GetName(),
			Converted.Bytes.Num() / (1024.0 * 1024.0), Limits.MaxClipCookedBytes / (1024.0 * 1024.0)),
			TEXT("Shorten it, or animate fewer bones.")));
		return false;
	}
	Converted.Key = Key;
	Converted.Name = Name;
	Converted.Path = Sequence->GetPathName();
	Converted.Hash = FMantleSha256::AssetHash(Converted.Bytes);
	Converted.Frames = Clip.FrameCount;
	Converted.Tracks = Clip.Tracks.Num();
	Converted.SkeletonHash = Clip.SkeletonHash;
	++Stats.ClipsConverted;
	UE_LOG(LogMantle, Log, TEXT("MANTLE_F5 clip %s on %s: %d frames at %.0f fps, %d track(s), skeleton %s, %d bytes as MAN1"), *Name, *Mesh->GetName(),
		Converted.Frames, FMantlePlaceAssetLimits::ClipFrameRate, Converted.Tracks, *Converted.SkeletonHash, Converted.Bytes.Num());
	Out.Clips.Add(MoveTemp(Converted));
	return true;
}

bool FMantlePlaceAssetConverter::ConvertDecalTexture(const FMantleGatheredTexture& Gathered, FMantlePlaceAssetSet& Out)
{
	using namespace MantlePlaceAssetConverterPrivate;
	if (Out.DecalTextures.Contains(Gathered.Key))
	{
		return true;
	}
	const UTexture* Texture = Gathered.Texture.Get();
	const FString Users = FMantlePlaceAssets::DescribeUsers(Gathered.UsedBy);
	// A vehicle's livery (#488) converts exactly as a decal's base colour does; only the words differ.
	const TCHAR* Kind = Gathered.bLivery ? TEXT("Livery texture") : TEXT("Decal texture");
	const FString What = Users.IsEmpty()
		? FString::Printf(TEXT("%s '%s'"), Kind, *Gathered.Name)
		: FString::Printf(TEXT("%s '%s' (%s)"), Kind, *Gathered.Name, *Users);
	const bool bLivery = Gathered.bLivery;
	auto Refuse = [&Out, &What, bLivery](const FString& Why)
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("%s can't be published: %s."), *What, *Why), bLivery
			? TEXT("Use a 2D texture imported from an image file (PNG, JPEG, TGA) for the vehicle's livery.")
			: TEXT("Use a 2D texture imported from an image file (PNG, JPEG, TGA) for the decal's BaseColor and Normal.")));
		return false;
	};
	TArray<uint8> Png;
	FString Unusable;
	if (!SourceTexturePng(Texture, /*bLinear*/ Gathered.bNormal, Gathered.bNormal, Png, Unusable))
	{
		return Refuse(Unusable);
	}

	FMantleConvertedTexture Converted;
	const EMantlePlaceTextureSlot Slot = Gathered.bNormal ? EMantlePlaceTextureSlot::Normal : EMantlePlaceTextureSlot::BaseColor;
	// Check this place already named a texture over the size cap, so its downscale is not repeated.
	if (!PrepareTexture(Png, Slot, /*bKeepAlpha*/ true, Limits, What, /*bWarnDownscale*/ false, Converted, Out.Issues))
	{
		return Refuse(TEXT("its pixels could not be encoded within the upload limits"));
	}
	Converted.Name = Gathered.bLivery ? FString::Printf(TEXT("%s (vehicle livery)"), *Gathered.Name)
		: FString::Printf(TEXT("%s (decal %s)"), *Gathered.Name, Gathered.bNormal ? TEXT("normal map") : TEXT("base colour"));
	Out.DecalTextures.Add(Gathered.Key, Out.AddTexture(MoveTemp(Converted)));
	++Stats.DecalTexturesConverted;
	return true;
}

bool FMantlePlaceAssetConverter::ConvertUIImage(const FString& PlacePath, const FString& DiskPath, FMantlePlaceAssetSet& Out)
{
	using namespace MantlePlaceAssetConverterPrivate;
	if (Out.UIImages.Contains(PlacePath))
	{
		return true;
	}
	const FString What = FString::Printf(TEXT("UI image %s"), *PlacePath);
	auto Refuse = [&Out, &What](const FString& Why)
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("%s can't be published: %s."), *What, *Why),
			TEXT("Use a PNG or JPEG file of at most 2 MB in the level's UI/ folder.")));
		return false;
	};
	const int64 Size = IFileManager::Get().FileSize(*DiskPath);
	if (Size < 0)
	{
		return Refuse(TEXT("the file is missing"));
	}
	if (Size > FMantlePlaceLimits::MaxUIImageBytes)
	{
		return Refuse(FString::Printf(TEXT("it is %lld KiB, over the %d KiB limit"), Size / 1024, FMantlePlaceLimits::MaxUIImageBytes / 1024));
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *DiskPath, FILEREAD_Silent))
	{
		return Refuse(TEXT("the file could not be read"));
	}
	// The same preparation as a decal's base colour: alpha kept straight, size and bytes capped.
	FMantleConvertedTexture Converted;
	if (!PrepareTexture(Bytes, EMantlePlaceTextureSlot::BaseColor, /*bKeepAlpha*/ true, Limits, What, /*bWarnDownscale*/ true, Converted, Out.Issues))
	{
		return Refuse(TEXT("its pixels could not be read as a PNG or JPEG, or encoded within the upload limits"));
	}
	Converted.Name = FString::Printf(TEXT("%s (UI image)"), *PlacePath);
	Out.UIImages.Add(PlacePath, Out.AddTexture(MoveTemp(Converted)));
	return true;
}

bool FMantlePlaceAssetConverter::ConvertThumbnail(TConstArrayView<FColor> Pixels, const int32 Width, const int32 Height, FMantlePlaceAssetSet& Out)
{
	using namespace MantlePlaceAssetConverterPrivate;
	const FString What = TEXT("The place's thumbnail");
	const TArray<uint8> Png = Width > 0 && Height > 0 && Pixels.Num() == Width * Height ? FMantlePng::EncodeRGBA8(Width, Height, Pixels) : TArray<uint8>();
	FMantleConvertedTexture Converted;
	// A colour picture whose alpha does not matter: JPEG, within the texture limits like any base colour.
	if (Png.IsEmpty() || !PrepareTexture(Png, EMantlePlaceTextureSlot::BaseColor, /*bKeepAlpha*/ false, Limits, What, /*bWarnDownscale*/ false, Converted, Out.Issues))
	{
		Out.Issues.Add(WarningIssue(TEXT("The place's thumbnail could not be prepared, so it publishes without a picture."),
			TEXT("Publish again; if it keeps happening, choose another view on Mantle Place Settings > Thumbnail.")));
		return false;
	}
	// #796: uploaded with no role, so the backend keeps the JPEG as its cooked bytes and the catalogue's thumbnailUrl
	// serves a picture SMantleRemoteImage decodes (a colour role cooks it to MTX1). A comment segment after the JPEG's
	// start marker gives it bytes no colour texture has: one hash is one asset, and a texture cooked for a role serves
	// only that role, so the thumbnails #786 uploaded as colour maps can never be handed back as this one.
	static const ANSICHAR Marker[] = "Mantle place thumbnail";
	constexpr int32 MarkerBytes = UE_ARRAY_COUNT(Marker) - 1;
	const int64 Cap = FMath::Min(Limits.MaxTextureBytes, Limits.MaxUploadBytes);
	if (Converted.Format != TEXT("jpeg") || Converted.Bytes.Num() < 4 || Converted.Bytes[0] != 0xFF || Converted.Bytes[1] != 0xD8
		|| Converted.Bytes.Num() + 4 + MarkerBytes > Cap)
	{
		Out.Issues.Add(WarningIssue(TEXT("The place's thumbnail could not be prepared, so it publishes without a picture."),
			TEXT("Publish again; if it keeps happening, choose another view on Mantle Place Settings > Thumbnail.")));
		return false;
	}
	TArray<uint8> Segment = { 0xFF, 0xFE, static_cast<uint8>((MarkerBytes + 2) >> 8), static_cast<uint8>((MarkerBytes + 2) & 0xFF) };
	Segment.Append(reinterpret_cast<const uint8*>(Marker), MarkerBytes);
	Converted.Bytes.Insert(Segment, 2);
	Converted.Role.Reset();
	Converted.Hash = FMantleSha256::AssetHash(Converted.Bytes);
	Converted.Key = FString::Printf(TEXT("thumbnail:%s"), *HexOfHash(Converted.Hash));
	Converted.Name = TEXT("Place thumbnail");
	Out.Thumbnail = Out.AddTexture(MoveTemp(Converted));
	return true;
}

bool FMantlePlaceAssetConverter::ConvertVehicle(UMantleVehicleSource* Vehicle, FMantlePlaceAssetSet& Out)
{
	using namespace MantlePlaceAssetConverterPrivate;
	if (!Vehicle)
	{
		Out.Issues.Add(ErrorIssue(TEXT("A vehicle this place parks could not be loaded."), TEXT("Check the level's Mantle Vehicle Spawns for missing assets, then try again.")));
		return false;
	}
	const FString Key = FMantleVehicleConverter::VehicleKey(Vehicle);
	if (Out.FindVehicle(Key))
	{
		return true;
	}
	// Check this place's rules, so a publish cannot upload what the cook refuses. Its warnings were Check's to show.
	TArray<FMantleValidationIssue> Issues;
	FMantleVehicleConverter::CheckSource(Vehicle, FString(), Issues);
	bool bRefused = false;
	for (FMantleValidationIssue& Issue : Issues)
	{
		if (Issue.Severity == EMantleIssueSeverity::Error)
		{
			bRefused = true;
			Out.Issues.Add(MoveTemp(Issue));
		}
	}
	if (bRefused)
	{
		return false;
	}

	FMantleConvertedVehicle Converted;
	TArray<UMaterialInterface*> Materials;
	FString ReadError;
	if (!FMantleVehicleConverter::ReadBody(*Vehicle, Converted.Body, Materials, ReadError, /*bApplyFitScale*/ true))
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Vehicle '%s' could not be converted: %s"), *Vehicle->GetName(), *ReadError),
			TEXT("Fix it as the message says, then try again.")));
		return false;
	}
	// Sections are bound by material name: unique (ignoring case, as the document's keys are) and non-empty.
	Converted.Body.MaterialNames = FMantleVehicleConverter::UniqueSectionNames(Converted.Body.MaterialNames);
	for (int32 Index = 0; Index < Converted.Body.MaterialNames.Num(); ++Index)
	{
		UMaterialInterface* Material = Materials.IsValidIndex(Index) ? Materials[Index] : nullptr;
		if (Material && !FMantlePlaceAssets::IsLibraryAsset(Material))
		{
			Converted.SectionMaterialKeys.Add(Converted.Body.MaterialNames[Index], FMantlePlaceAssets::MaterialKey(Material));
		}
	}
	// Its liveries (#488) convert as decal base colours (FMantlePlaceDependencies::AddVehicleSpawn gathers them), and
	// upload with the textures, before it.
	Converted.LiveryTextureKeys = FMantleVehicleConverter::LiveryTextureKeys(*Vehicle);
	// Its attached parts (#435 item 2): each mesh converts and uploads as a mesh does (FMantlePlaceDependencies::AddVehicleSpawn
	// gathers it), with the material each of its upload slots publishes with; a library material is left to the default surface.
	// The parts ride the body, so they take the vehicle's Size as the body did above: read from the export view, which
	// scales their placement and size (#728). At Size 1 the view is the vehicle itself.
	const TStrongObjectPtr<UMantleVehicleSource> SizedView(const_cast<UMantleVehicleSource*>(FMantleVehicleConverter::MakeExportView(*Vehicle)));
	for (const FMantleVehicleSourcePart& Part : SizedView->Parts)
	{
		const UStaticMesh* PartMesh = Part.Mesh.LoadSynchronous();
		if (!PartMesh)
		{
			continue; // CheckSource refused it above
		}
		FMantleConvertedVehicle::FPart& ConvertedPart = Converted.Parts.AddDefaulted_GetRef();
		ConvertedPart.MeshKey = FMantlePlaceAssets::MeshKey(PartMesh);
		ConvertedPart.Bone = Part.Bone;
		ConvertedPart.Transform = Part.Transform;
		for (const int32 Slot : FMantlePlaceAssets::UploadSlots(PartMesh))
		{
			const UMaterialInterface* Material = FMantleVehicleConverter::PartSlotMaterial(Part, PartMesh, Slot);
			ConvertedPart.MaterialKeys.Add(Material && !FMantlePlaceAssets::IsLibraryAsset(Material) ? FMantlePlaceAssets::MaterialKey(Material) : FString());
		}
		// Its painted slots, counted as the upload counts them.
		ConvertedPart.PaintSlots = FMantleVehicleConverter::PartUploadPaintSlots(Part, PartMesh);
	}
	// Its horn's sound (#546): converted and uploaded as a SoundEmitter's is (FMantlePlaceDependencies::AddVehicleSpawn gathers it),
	// before it. CheckSource refused one that is not a sound wave of the project's own.
	if (const USoundBase* Horn = Vehicle->Tuning.HornSound.LoadSynchronous())
	{
		Converted.HornSoundKey = FMantlePlaceAssets::SoundKey(Horn);
	}
	for (const FMantleGlbPrimitive& Primitive : Converted.Body.Primitives)
	{
		Converted.Triangles += Primitive.Indices.Num() / 3;
	}
	Converted.Key = Key;
	Converted.Name = Vehicle->GetName();
	Converted.Path = Vehicle->GetPathName();
	Converted.ImportFilename = FMantleProvenance::ImportFilenameOf(Vehicle->Body.LoadSynchronous());
	Converted.Document = FMantleVehicleConverter::MakeDocument(*Vehicle, TMap<FString, FString>());

	// The size check needs the bytes; the ids in them are not known yet, so placeholders of the same length stand in.
	TMap<FString, FString> Placeholders;
	for (const TPair<FString, FString>& Section : Converted.SectionMaterialKeys)
	{
		Placeholders.Add(Section.Key, FMantlePlaceAssets::ProvisionalId(Section.Value));
	}
	TArray<FString> LiveryPlaceholders;
	for (const FString& LiveryKey : Converted.LiveryTextureKeys)
	{
		LiveryPlaceholders.Add(FMantlePlaceAssets::ProvisionalId(LiveryKey));
	}
	TArray<FMantleVehicleConverter::FPartIds> PartPlaceholders;
	for (const FMantleConvertedVehicle::FPart& Part : Converted.Parts)
	{
		FMantleVehicleConverter::FPartIds& Entry = PartPlaceholders.AddDefaulted_GetRef();
		Entry.Mesh = FMantlePlaceAssets::ProvisionalId(Part.MeshKey);
		Entry.Bone = Part.Bone;
		Entry.Transform = Part.Transform;
		Entry.PaintSlots = Part.PaintSlots;
		for (const FString& MaterialKey : Part.MaterialKeys)
		{
			Entry.Materials.Add(MaterialKey.IsEmpty() ? FString() : FMantlePlaceAssets::ProvisionalId(MaterialKey));
		}
	}
	const FString HornPlaceholder = Converted.HornSoundKey.IsEmpty() ? FString() : FMantlePlaceAssets::ProvisionalId(Converted.HornSoundKey);
	TArray<uint8> Bytes;
	FString WriteError;
	if (!FMantleVehicleConverter::WriteUpload(Converted.Body, FMantleVehicleConverter::WithHorn(FMantleVehicleConverter::WithParts(
			FMantleVehicleConverter::WithLiveries(FMantleVehicleConverter::WithMaterials(Converted.Document.ToSharedRef(), Placeholders), LiveryPlaceholders),
			PartPlaceholders), HornPlaceholder), Bytes, WriteError))
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Vehicle '%s' could not be written: %s"), *Vehicle->GetName(), *WriteError),
			TEXT("Re-import its body and try again; if it keeps failing, report it with the log.")));
		return false;
	}
	if (Bytes.Num() > Limits.MaxUploadBytes)
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Vehicle '%s' is %.1f MB once converted; an upload may be at most %.0f MB."), *Vehicle->GetName(),
			Bytes.Num() / (1024.0 * 1024.0), Limits.MaxUploadBytes / (1024.0 * 1024.0)), TEXT("Use a lower-detail body or tire mesh.")));
		return false;
	}
	// Its library card's picture (#821), taken when it was built: a texture upload beside the vehicle's, with no role so the backend keeps the PNG as it is (the library's
	// thumbnail class is a 512 x 512 PNG). Added here, after the vehicle passed its checks, so a refused vehicle leaves no orphan picture. A vehicle with none has no card picture.
	if (Vehicle->ThumbnailPng.Num() > 0)
	{
		FMantleConvertedTexture Picture;
		Picture.Format = TEXT("png");
		Picture.Width = FMantleVehicleThumbnail::Size;
		Picture.Height = FMantleVehicleThumbnail::Size;
		Picture.Bytes = Vehicle->ThumbnailPng;
		Picture.Hash = FMantleSha256::AssetHash(Picture.Bytes);
		Picture.Key = FString::Printf(TEXT("vehiclepicture:%s"), *HexOfHash(Picture.Hash));
		Picture.Name = FString::Printf(TEXT("%s library picture"), *Vehicle->GetName());
		Converted.ThumbnailKey = Out.AddTexture(MoveTemp(Picture));
	}
	Out.Vehicles.Add(MoveTemp(Converted));
	return true;
}

bool FMantlePlaceAssetConverter::ConvertWeapon(const FMantleGatheredWeapon& Weapon, FMantlePlaceAssetSet& Out)
{
	using namespace MantlePlaceAssetConverterPrivate;
	if (Out.FindWeapon(Weapon.Key))
	{
		return true;
	}
	const USkeletalMesh* Mesh = Weapon.Mesh.Get();
	FString Text;
	if (!Mesh || !FFileHelper::LoadFileToString(Text, *AMantleWeaponSpawnPoint::BuiltDocumentFile(Weapon.Name)))
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("The weapon '%s' could not be loaded."), *Weapon.Name),
			TEXT("Open Tools > Mantle > Build a weapon and press Create weapon again, then publish again.")));
		return false;
	}
	FMantleBuiltWeaponParts Parts;
	const FString Problem = FMantlePlaceAssets::WeaponDocumentProblem(Weapon.Name, Text, &Parts);
	if (!Problem.IsEmpty())
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("The weapon '%s' %s."), *Weapon.Name, *Problem),
			TEXT("Open Tools > Mantle > Build a weapon and press Create weapon again, then publish again.")));
		return false;
	}
	FMantleConvertedWeapon Converted;
	Converted.Key = Weapon.Key;
	Converted.Name = Weapon.Name;
	Converted.Path = TEXT("/Game/MantleWeapons/") + Weapon.Name;
	Converted.DocumentText = MoveTemp(Text);
	Converted.ImportFilename = FMantleProvenance::ImportFilenameOf(Mesh);
	Converted.MeshKey = FMantlePlaceAssets::SkeletalMeshKey(Mesh);
	// The material of each upload slot, as FMantlePlaceDependencies::AddBuiltWeapon gathers it: the model's own, else the default surface,
	// always uploaded (#892: a weapon names no library material). The extra parts' models the same (#898).
	auto SlotOf = [](UMaterialInterface* Material)
	{
		FMantleConvertedWeapon::FSlot Slot;
		Slot.MaterialKey = FMantlePlaceAssets::MaterialKey(Material ? Material : UMaterial::GetDefaultMaterial(MD_Surface));
		return Slot;
	};
	auto SkeletalSlots = [&SlotOf](const USkeletalMesh* Model, TArray<FMantleConvertedWeapon::FSlot>& Into)
	{
		const TArray<FSkeletalMaterial>& ModelMaterials = Model->GetMaterials();
		for (const int32 Slot : FMantlePlaceAssets::SkeletalUploadSlots(Model))
		{
			Into.Add(SlotOf(ModelMaterials.IsValidIndex(Slot) ? ModelMaterials[Slot].MaterialInterface.Get() : nullptr));
		}
	};
	SkeletalSlots(Mesh, Converted.Slots);
	// An extra part's model, or (#905) the round's: its key and its slots' materials; none for no model (an effect-only part, no round).
	auto ReadPart = [&SlotOf, &SkeletalSlots](const UObject* Model, FMantleConvertedWeapon::FAttachment& Part)
	{
		if (const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(Model))
		{
			Part.MeshKey = FMantlePlaceAssets::SkeletalMeshKey(Skeletal);
			SkeletalSlots(Skeletal, Part.Slots);
		}
		else if (const UStaticMesh* Static = Cast<UStaticMesh>(Model))
		{
			Part.MeshKey = FMantlePlaceAssets::MeshKey(Static);
			for (const int32 Slot : FMantlePlaceAssets::UploadSlots(Static))
			{
				Part.Slots.Add(SlotOf(FMantlePlaceAssets::MeshSlotMaterial(Static, Slot)));
			}
		}
	};
	for (const UObject* Model : Parts.Attachments)
	{
		ReadPart(Model, Converted.Attachments.AddDefaulted_GetRef());
	}
	ReadPart(Parts.Projectile, Converted.Projectile);
	Converted.FireSoundKey = Parts.Fire ? FMantlePlaceAssets::SoundKey(Parts.Fire) : FString();
	Converted.FireSuppressedSoundKey = Parts.FireSuppressed ? FMantlePlaceAssets::SoundKey(Parts.FireSuppressed) : FString();
	// #124: its library picture, from the creator's choice (the weapon builder's Thumbnail step) or Automatic: the model in the catalog's thumbnail scene. A texture
	// upload beside the weapon's, like a vehicle's, named on its library item. A weapon that cannot get one (no renderer) publishes without, as before.
	{
		FMantleThumbnailChoice Choice;
		FMantleThumbnailStore::Load(Weapon.Name, TEXT("weapon"), Choice, FMantleThumbnailStore::DefaultConfigFile());
		USkeletalMesh* Model = const_cast<USkeletalMesh*>(Mesh);
		const FMantleThumbnailResult Picture = FMantleThumbnailResolver::Resolve(Choice, FMantleThumbnailTarget::Square(),
			FMantleModelThumbnail::RenderFor([Model]() -> UObject* { return Model; }));
		if (Picture.Succeeded())
		{
			FMantleConvertedTexture Texture;
			Texture.Format = TEXT("png");
			Texture.Width = FMantleThumbnailTarget::Square().Width;
			Texture.Height = FMantleThumbnailTarget::Square().Height;
			Texture.Bytes = FMantleThumbnailResolver::ToPng(Picture, FMantleThumbnailTarget::Square());
			Texture.Hash = FMantleSha256::AssetHash(Texture.Bytes);
			Texture.Key = FString::Printf(TEXT("weaponpicture:%s"), *HexOfHash(Texture.Hash));
			Texture.Name = FString::Printf(TEXT("%s library picture"), *Weapon.Name);
			Converted.ThumbnailKey = Out.AddTexture(MoveTemp(Texture));
			if (!Picture.Warning.IsEmpty())
			{
				Out.Issues.Add(WarningIssue(FString::Printf(TEXT("The weapon '%s' thumbnail: %s"), *Weapon.Name, *Picture.Warning), TEXT("Use a bigger picture in the weapon builder's Thumbnail step.")));
			}
		}
		else if (Choice.Mode == EMantleThumbnailMode::Custom)
		{
			Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("The weapon '%s' thumbnail's custom image could not be used: %s"), *Weapon.Name, *Picture.Error),
				TEXT("Open Tools > Mantle > Build a weapon, choose another picture in the Thumbnail step, press Create weapon, then publish again.")));
			return false;
		}
		else if (FMantleCosmeticThumbnail::CanCapture())
		{
			Out.Issues.Add(WarningIssue(FString::Printf(TEXT("The weapon '%s' thumbnail could not be taken (%s), so its library card has no picture."), *Weapon.Name, *Picture.Error),
				TEXT("Publish again; if it keeps happening, choose a Custom image in the weapon builder's Thumbnail step.")));
		}
	}
	Out.Weapons.Add(MoveTemp(Converted));
	return true;
}

int32 FMantlePlaceAssetConverter::NumDependencySteps(const FMantlePlaceDependencies& Dependencies)
{
	return Dependencies.Meshes.Num() + Dependencies.Materials.Num() + Dependencies.Textures.Num() + Dependencies.Sounds.Num()
		+ Dependencies.SkeletalMeshes.Num() + Dependencies.Clips.Num() + Dependencies.Terrains.Num() + Dependencies.Vehicles.Num()
		+ Dependencies.Weapons.Num();
}

FString FMantlePlaceAssetConverter::DependencyStepName(const FMantlePlaceDependencies& Dependencies, int32 Step)
{
	if (Dependencies.Meshes.IsValidIndex(Step))
	{
		return Dependencies.Meshes[Step].Name;
	}
	const int32 Material = Step - Dependencies.Meshes.Num();
	if (Dependencies.Materials.IsValidIndex(Material))
	{
		return Dependencies.Materials[Material].Name;
	}
	// A decal's textures (#472), after the materials, then the level's own sounds (#324).
	const int32 Texture = Material - Dependencies.Materials.Num();
	if (Dependencies.Textures.IsValidIndex(Texture))
	{
		return Dependencies.Textures[Texture].Name;
	}
	const int32 Sound = Texture - Dependencies.Textures.Num();
	if (Dependencies.Sounds.IsValidIndex(Sound))
	{
		return Dependencies.Sounds[Sound].Name;
	}
	// Skeletal meshes and their clips (#469), then the landscape (#466), in FMantlePlaceAssetConverter::Convert's order.
	const int32 Skeletal = Sound - Dependencies.Sounds.Num();
	if (Dependencies.SkeletalMeshes.IsValidIndex(Skeletal))
	{
		return Dependencies.SkeletalMeshes[Skeletal].Name;
	}
	const int32 Clip = Skeletal - Dependencies.SkeletalMeshes.Num();
	if (Dependencies.Clips.IsValidIndex(Clip))
	{
		return Dependencies.Clips[Clip].Name;
	}
	const int32 Terrain = Clip - Dependencies.Clips.Num();
	if (Dependencies.Terrains.IsValidIndex(Terrain))
	{
		return Dependencies.Terrains[Terrain].Name;
	}
	// Creator vehicles (#435): their bodies' materials are among the materials above.
	const int32 Vehicle = Terrain - Dependencies.Terrains.Num();
	if (Dependencies.Vehicles.IsValidIndex(Vehicle))
	{
		return Dependencies.Vehicles[Vehicle].Name;
	}
	// Built weapons (#789), after the vehicles.
	const int32 Weapon = Vehicle - Dependencies.Vehicles.Num();
	return Dependencies.Weapons.IsValidIndex(Weapon) ? Dependencies.Weapons[Weapon].Name : FString();
}

void FMantlePlaceAssetConverter::ConvertDependencyStep(const FMantlePlaceDependencies& Dependencies, int32 Step, FMantlePlaceAssetSet& Out)
{
	if (Dependencies.Meshes.IsValidIndex(Step))
	{
		ConvertMesh(Dependencies.Meshes[Step].Mesh.Get(), Out);
		return;
	}
	const int32 Material = Step - Dependencies.Meshes.Num();
	if (Dependencies.Materials.IsValidIndex(Material))
	{
		ConvertMaterial(Dependencies.Materials[Material].Material.Get(), Out);
		return;
	}
	// A decal's textures (#472).
	const int32 Texture = Material - Dependencies.Materials.Num();
	if (Dependencies.Textures.IsValidIndex(Texture))
	{
		ConvertDecalTexture(Dependencies.Textures[Texture], Out);
		return;
	}
	// The level's own sounds (#324), written as WAV files for upload.
	const int32 Sound = Texture - Dependencies.Textures.Num();
	if (Dependencies.Sounds.IsValidIndex(Sound))
	{
		ConvertSound(Dependencies.Sounds[Sound].Sound.Get(), Out);
		return;
	}
	// Skeletal meshes, then the clips they play (#469).
	const int32 Skeletal = Sound - Dependencies.Sounds.Num();
	if (Dependencies.SkeletalMeshes.IsValidIndex(Skeletal))
	{
		ConvertSkeletalMesh(Dependencies.SkeletalMeshes[Skeletal].Mesh.Get(), Out);
		return;
	}
	const int32 Clip = Skeletal - Dependencies.SkeletalMeshes.Num();
	if (Dependencies.Clips.IsValidIndex(Clip))
	{
		ConvertClip(Dependencies.Clips[Clip].Sequence.Get(), Dependencies.Clips[Clip].Mesh.Get(), Out);
		return;
	}
	// The level's landscape (#466), as a heightfield and a weightmap.
	const int32 Terrain = Clip - Dependencies.Clips.Num();
	if (Dependencies.Terrains.IsValidIndex(Terrain))
	{
		ConvertTerrain(Dependencies.Terrains[Terrain].Landscape.Get(), Out);
		return;
	}
	// Creator vehicles (#435): their documents name their materials' ids.
	const int32 Vehicle = Terrain - Dependencies.Terrains.Num();
	if (Dependencies.Vehicles.IsValidIndex(Vehicle))
	{
		ConvertVehicle(Dependencies.Vehicles[Vehicle].Vehicle.Get(), Out);
		return;
	}
	// Built weapons (#789): their documents name their model's and materials' ids.
	const int32 Weapon = Vehicle - Dependencies.Vehicles.Num();
	if (Dependencies.Weapons.IsValidIndex(Weapon))
	{
		ConvertWeapon(Dependencies.Weapons[Weapon], Out);
	}
}

bool FMantlePlaceAssetConverter::RepairLODScreenSizes(TArray<float>& InOutSizes)
{
	bool bChanged = false;
	for (int32 Index = 0; Index < InOutSizes.Num(); ++Index)
	{
		float& Size = InOutSizes[Index];
		const float Before = Size;
		if (Index == 0)
		{
			Size = FMath::IsFinite(Size) && Size > 0.0f ? FMath::Min(Size, 1.0f) : 1.0f;
		}
		else if (!FMath::IsFinite(Size) || !(Size > 0.0f) || !(Size < InOutSizes[Index - 1]))
		{
			Size = InOutSizes[Index - 1] * 0.5f;
		}
		bChanged |= Size != Before;
	}
	return bChanged;
}

double FMantlePlaceAssetConverter::GlassOpacityFromIor(const double Ior, const double Opacity)
{
	// glTF's index is 1 or more; the default surface reflects as an index of 1.5 does (F0 0.04).
	const double N = FMath::Clamp(Ior, 1.0, 3.0);
	const double F0 = FMath::Square((N - 1.0) / (N + 1.0));
	return FMath::Clamp(FMath::Max(Opacity, MinGlassOpacity) + (F0 - 0.04), 0.0, 1.0);
}

FString FMantlePlaceAssetConverter::MeshCacheSalt(const FMantlePlaceAssetLimits& Limits, const UStaticMesh& Mesh, int32 FromLOD, const FString& ReducedTag)
{
	// The LOD and collision caps change what is written, so they are part of the key.
	TArray<FString> LODCaps;
	for (const int32 Cap : Limits.LODMaxTriangles)
	{
		LODCaps.Add(FString::FromInt(Cap));
	}
	FString Salt = FString::Printf(TEXT("mesh lods=%d/%s collision=%d/%d/%d/%d bytes=%lld %s"), Limits.MaxLODs, *FString::Join(LODCaps, TEXT(",")),
		Limits.MaxCollisionPrimitives, Limits.MaxConvexHulls, Limits.MaxHullVertices, Limits.MaxTotalHullVertices, Limits.MaxUploadBytes, *Mesh.GetPathName());
	// #804: only a mesh published from a smaller version of itself says so; one that fits keeps the key it had.
	if (FromLOD > 0)
	{
		Salt += FString::Printf(TEXT(" fromlod=%d"), FromLOD);
	}
	if (!ReducedTag.IsEmpty())
	{
		Salt += TEXT(" reduced=") + ReducedTag;
	}
	return Salt;
}

namespace MantlePlaceAssetConverterPrivate
{
	/**
	 * The name a mesh is written under in its GLB (the node's name, so part of the file's hash): a baked copy's intended name, not
	 * the engine's uniqueness counter, which grows in an editor session and would make a re-baked copy hash differently (#804).
	 */
	FString PublishedMeshName(const UStaticMesh& Mesh)
	{
		return FMantlePlaceAssets::PublishedMeshName(Mesh);
	}

	/**
	 * Puts Glb's materials, and Slots (the mesh slot of each) with them, in the order of Wanted, the slots as the export lists
	 * them for the creator's own LOD 0 (FMantlePlaceAssets::UploadSlots), which the place's materials follow by position. A
	 * smaller version of a mesh (a lower LOD, a reduced copy) can list the same slots in another order. False, nothing changed,
	 * when it does not draw with exactly those slots.
	 */
	bool ConformSlotOrder(FMantleGlbMesh& Glb, TArray<int32>& Slots, const TArray<int32>& Wanted)
	{
		if (Slots.Num() != Wanted.Num() || Glb.MaterialNames.Num() != Slots.Num())
		{
			return false;
		}
		TArray<int32> NewIndexOf;
		for (const int32 Slot : Slots)
		{
			const int32 NewIndex = Wanted.IndexOfByKey(Slot);
			if (NewIndex == INDEX_NONE || NewIndexOf.Contains(NewIndex))
			{
				return false;
			}
			NewIndexOf.Add(NewIndex);
		}
		TArray<FString> Names;
		Names.SetNum(Slots.Num());
		for (int32 Index = 0; Index < Slots.Num(); ++Index)
		{
			Names[NewIndexOf[Index]] = Glb.MaterialNames[Index];
		}
		for (FMantleGlbPrimitive& Primitive : Glb.Primitives)
		{
			Primitive.MaterialIndex = NewIndexOf.IsValidIndex(Primitive.MaterialIndex) ? NewIndexOf[Primitive.MaterialIndex] : Primitive.MaterialIndex;
		}
		Glb.MaterialNames = MoveTemp(Names);
		Slots = Wanted;
		return true;
	}
}

bool FMantlePlaceAssetConverter::ConvertMesh(UStaticMesh* Mesh, FMantlePlaceAssetSet& Out)
{
	using namespace MantlePlaceAssetConverterPrivate;
	if (!Mesh)
	{
		Out.Issues.Add(ErrorIssue(TEXT("A mesh this place uses could not be loaded."), TEXT("Check the level for missing assets, then try again.")));
		return false;
	}
	const FString Key = FMantlePlaceAssets::MeshKey(Mesh);
	if (Out.FindMesh(Key))
	{
		return true;
	}

	// #804: an over-limit mesh converts from its first lower LOD that fits (that LOD becomes its LOD 0), else from a transient copy
	// reduced to fit, under the creator's mesh's key; the notice is Check this place's (FMantlePlaceAssets::CheckMesh). The cache
	// key says which, and a mesh that fits keeps the key it had.
	const int32 PublishedLOD = FMantlePlaceAssets::PublishedLODIndex(Mesh, Limits);
	const int32 FromLOD = FMath::Max(PublishedLOD, 0);
	const FString ReducedTag = PublishedLOD == INDEX_NONE ? FMantleMeshReduce::CacheTag(Limits) : FString();
	const FString Salt = MeshCacheSalt(Limits, *Mesh, FromLOD, ReducedTag);
	FString CacheKey, WhyNot;
	bool bCacheable = ComputeCacheKey(Salt, { Mesh }, CacheKey, WhyNot);
	FMantleConvertedMesh Converted;
	{
		TArray<FMantleValidationIssue> CachedIssues;
		if (bCacheable && LoadCachedMesh(CacheKey, Converted, CachedIssues))
		{
			Out.Issues.Append(CachedIssues);
			Converted.Key = Key;
			Converted.bFromCache = true;
			++Stats.MeshesFromCache;
			Out.Meshes.Add(MoveTemp(Converted));
			return true;
		}
	}
	if (!bCacheable)
	{
		UE_LOG(LogMantle, Verbose, TEXT("MANTLE_F5 %s is not cached: %s"), *Mesh->GetName(), *WhyNot);
	}

	// What is read: the creator's mesh from LOD FromLOD, or the reduced copy of it (one LOD, its LOD 0).
	const UStaticMesh* ReadFrom = Mesh;
	int32 FirstLOD = FromLOD;
	if (PublishedLOD == INDEX_NONE && !ReducedTag.IsEmpty())
	{
		FString ReduceWhy;
		if (const UStaticMesh* Reduced = FMantleMeshReduce::ReducedCopy(Mesh, Limits, ReduceWhy))
		{
			ReadFrom = Reduced;
			FirstLOD = 0;
		}
		else
		{
			// Check this place has already refused this mesh; what is written here is not the reduction the key names.
			UE_LOG(LogMantle, Log, TEXT("MANTLE_804 %s is converted as it is: %s"), *Mesh->GetName(), *ReduceWhy);
			bCacheable = false;
		}
	}

	FMantleGlbMesh Glb;
	FString ReadError;
	if (!FMantleCosmeticExport::ReadStaticMesh(ReadFrom, FirstLOD, Glb, Converted.Slots, Converted.Triangles, ReadError))
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Mesh '%s' could not be converted: %s"), *Mesh->GetName(), *ReadError),
			TEXT("Fix the mesh as the message says, or replace it, then try again.")));
		return false;
	}
	if (ReadFrom == Mesh && Mesh->GetOutermost() == GetTransientPackage())
	{
		// A baked copy (#804): named without the counter MakeUniqueObjectName added, so a re-bake hashes the same.
		Glb.Name = PublishedMeshName(*Mesh);
	}
	if (ReadFrom != Mesh || FirstLOD > 0)
	{
		// Named and ordered as the creator's own mesh is, whatever it was read from: the place's materials follow its slot order.
		Glb.Name = PublishedMeshName(*Mesh);
		if (!ConformSlotOrder(Glb, Converted.Slots, FMantlePlaceAssets::UploadSlots(Mesh)))
		{
			Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Mesh '%s' could not be converted: its smaller version does not draw with the same materials."), *Mesh->GetName()),
				TEXT("Use a lower-detail version of the mesh, or give it a LOD that fits and draws with the same materials.")));
			return false;
		}
	}
	if (Converted.Slots.Num() > Limits.MaxSections)
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Mesh '%s' has %d material slots; a place mesh may have at most %d."), *Mesh->GetName(),
			Converted.Slots.Num(), Limits.MaxSections), TEXT("Give it fewer materials (Merge Actors can bake several into one).")));
		return false;
	}

	// Sections are bound by material name, so names must be unique and non-empty (the cook refuses otherwise).
	TSet<FString> Used;
	for (int32 Index = 0; Index < Glb.MaterialNames.Num(); ++Index)
	{
		FString Name = Glb.MaterialNames[Index].Left(200);
		if (Name.IsEmpty())
		{
			Name = FString::Printf(TEXT("material_%d"), Index);
		}
		FString Unique = Name;
		for (int32 Suffix = 2; Used.Contains(Unique); ++Suffix)
		{
			Unique = FString::Printf(TEXT("%s_%d"), *Name, Suffix);
		}
		Used.Add(Unique);
		Glb.MaterialNames[Index] = Unique;
	}

	// Warnings about what did not carry over; cached with the conversion so a republish says them again.
	TArray<FMantleValidationIssue> MeshIssues;
	TArray<FMantleGlbMesh> LODs;
	Converted.SectionNames = Glb.MaterialNames;
	Converted.Vertices = Glb.Positions.Num();
	LODs.Add(MoveTemp(Glb));

	// #470: the mesh's own lower LODs, each named by LOD0's section name for its slot, until one breaks a rule. The LODs are
	// counted from the one published as LOD 0 (#804): LOD number LODIndex is the creator's mesh's LOD FirstLOD + LODIndex.
	FMantleGlbStaticExtras Extras;
	const FStaticMeshRenderData* RenderData = ReadFrom->GetRenderData();
	const int32 LODsWanted = FMath::Min(RenderData ? RenderData->LODResources.Num() - FirstLOD : 1, FMath::Clamp(Limits.MaxLODs, 1, FMantleGlbWriter::MaxLODs));
	int32 PreviousTriangles = Converted.Triangles;
	for (int32 LODIndex = 1; LODIndex < LODsWanted; ++LODIndex)
	{
		FMantleGlbMesh Lower;
		TArray<int32> LowerSlots;
		int32 LowerTriangles = 0;
		FString Why;
		if (!FMantleCosmeticExport::ReadStaticMesh(ReadFrom, FirstLOD + LODIndex, Lower, LowerSlots, LowerTriangles, Why))
		{
			Why = Why.IsEmpty() ? FString(TEXT("it could not be read")) : Why;
		}
		else if (LowerTriangles > Limits.LODTriangleCap(LODIndex))
		{
			Why = FString::Printf(TEXT("it has %d triangles and a place mesh's LOD%d may have at most %d"), LowerTriangles, LODIndex, Limits.LODTriangleCap(LODIndex));
		}
		else if (LowerTriangles > PreviousTriangles)
		{
			Why = FString::Printf(TEXT("it has more triangles (%d) than LOD%d (%d)"), LowerTriangles, FirstLOD + LODIndex - 1, PreviousTriangles);
		}
		else if (Lower.Positions.Num() > Limits.MaxVertices)
		{
			Why = FString::Printf(TEXT("it has %d vertices and a place mesh may have at most %d"), Lower.Positions.Num(), Limits.MaxVertices);
		}
		else
		{
			for (int32 Material = 0; Material < Lower.MaterialNames.Num() && Why.IsEmpty(); ++Material)
			{
				const int32 Slot = LowerSlots.IsValidIndex(Material) ? LowerSlots[Material] : INDEX_NONE;
				const int32 InLOD0 = Converted.Slots.IndexOfByKey(Slot);
				if (InLOD0 == INDEX_NONE)
				{
					Why = FString::Printf(TEXT("it draws with material slot %d, which LOD0 does not"), Slot);
				}
				else
				{
					Lower.MaterialNames[Material] = LODs[0].MaterialNames[InLOD0];
				}
			}
		}
		if (!Why.IsEmpty())
		{
			MeshIssues.Add(WarningIssue(FString::Printf(TEXT("Mesh '%s': LOD%d%s was left off because %s; players see the LODs before it at every distance."),
				*Mesh->GetName(), FirstLOD + LODIndex, LODIndex + 1 < LODsWanted ? TEXT(" and the LODs after it") : TEXT(""), *Why),
				TEXT("It still publishes. Rebuild the mesh's LODs in the mesh editor (each smaller than the one before) to publish them.")));
			break;
		}
		PreviousTriangles = LowerTriangles;
		LODs.Add(MoveTemp(Lower));
	}
	if (LODs.Num() > 1)
	{
		// Where the mesh switches in the editor, the render data's (the source models' or the auto-computed ones); from the LOD published as LOD 0.
		for (int32 LODIndex = 0; LODIndex < LODs.Num(); ++LODIndex)
		{
			Extras.ScreenSizes.Add(RenderData->ScreenSize[FirstLOD + LODIndex].Default);
		}
		const TArray<float> Original = Extras.ScreenSizes;
		if (FMantlePlaceAssetConverter::RepairLODScreenSizes(Extras.ScreenSizes))
		{
			const auto Join = [](const TArray<float>& Sizes)
			{
				return FString::JoinBy(Sizes, TEXT(", "), [](const float Size) { return FString::SanitizeFloat(Size); });
			};
			MeshIssues.Add(WarningIssue(FString::Printf(TEXT("Mesh '%s': its LOD screen sizes (%s) were not each below the one before, so they were published as (%s)."),
				*Mesh->GetName(), *Join(Original), *Join(Extras.ScreenSizes)),
				TEXT("The mesh asset is unchanged. Set the LOD screen sizes in the mesh editor so each LOD's is smaller than the one before to publish your own.")));
		}
	}

	// #470: its simple collision, which the client builds in place of the triangles.
	Extras.Collision = ReadSimpleCollision(Mesh, Limits, MeshIssues);

	FString WriteError;
	if (!FMantleGlbWriter::WriteStatic(LODs, Extras, Converted.Bytes, WriteError))
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Mesh '%s' could not be written: %s"), *Mesh->GetName(), *WriteError),
			TEXT("Re-import the mesh and try again; if it keeps failing, report it with the log.")));
		return false;
	}
	if (Converted.Bytes.Num() > Limits.MaxUploadBytes && LODs.Num() > 1)
	{
		// The lower LODs are what can go: LOD0 alone may still fit.
		MeshIssues.Add(WarningIssue(FString::Printf(TEXT("Mesh '%s' with its LODs is %.1f MB once converted, over the %.0f MB upload limit, so only LOD0 was published."),
			*Mesh->GetName(), Converted.Bytes.Num() / (1024.0 * 1024.0), Limits.MaxUploadBytes / (1024.0 * 1024.0)),
			TEXT("It still publishes. Use lower-detail LODs to publish them.")));
		LODs.SetNum(1);
		Extras.ScreenSizes.Reset();
		if (!FMantleGlbWriter::WriteStatic(LODs, Extras, Converted.Bytes, WriteError))
		{
			Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Mesh '%s' could not be written: %s"), *Mesh->GetName(), *WriteError),
				TEXT("Re-import the mesh and try again; if it keeps failing, report it with the log.")));
			return false;
		}
	}
	Converted.Key = Key;
	Converted.Name = Mesh->GetName();
	Converted.Path = Mesh->GetPathName();
	Converted.LODs = LODs.Num();
	Converted.CollisionShapes = Extras.Collision.ShapeCount();
	Converted.Hash = FMantleSha256::AssetHash(Converted.Bytes);
	if (Converted.Bytes.Num() > Limits.MaxUploadBytes)
	{
		Out.Issues.Add(ErrorIssue(FString::Printf(TEXT("Mesh '%s' is %.1f MB once converted; an upload may be at most %.0f MB."), *Mesh->GetName(),
			Converted.Bytes.Num() / (1024.0 * 1024.0), Limits.MaxUploadBytes / (1024.0 * 1024.0)), TEXT("Use a lower-detail version of it.")));
		return false;
	}
	++Stats.MeshesConverted;
	Out.Issues.Append(MeshIssues);
	if (bCacheable)
	{
		StoreCachedMesh(CacheKey, Converted, MeshIssues);
	}
	Out.Meshes.Add(MoveTemp(Converted));
	return true;
}

bool FMantlePlaceAssetConverter::ConvertMaterial(UMaterialInterface* Material, FMantlePlaceAssetSet& Out)
{
	using namespace MantlePlaceAssetConverterPrivate;
	if (!Material)
	{
		Out.Issues.Add(ErrorIssue(TEXT("A material this place uses could not be loaded."), TEXT("Check the level for missing assets, then try again.")));
		return false;
	}
	const FString Key = FMantlePlaceAssets::MaterialKey(Material);
	if (Out.FindMaterial(Key))
	{
		return true;
	}

	const FMantleMaterialAnalysis Analysis = FMantlePlaceAssets::AnalyzeMaterial(Material);
	bool bOversizeReported = false;
	TArray<const UObject*> Objects;
	Objects.Add(Material);
	for (const TWeakObjectPtr<UObject>& Dependency : Analysis.Dependencies)
	{
		Objects.Add(Dependency.Get());
	}
	for (const TWeakObjectPtr<UTexture>& Texture : Analysis.Textures)
	{
		const FIntPoint Size = FMantlePlaceAssets::TextureSize(Texture.Get());
		bOversizeReported = bOversizeReported || FMantlePlaceAssets::PublishedTextureSize(Size, Limits.MaxTextureSize) != Size;
	}

	// #628: a per-instance node is baked at a representative value; the salt keeps an earlier bake (which read it as 0) from standing in.
	const FString Salt = FString::Printf(TEXT("material %s bake=%d max=%d bytes=%lld %s%s"), *Flattener->Describe(), Settings.DefaultBakeSize,
		Limits.MaxTextureSize, Limits.MaxTextureBytes, *Material->GetPathName(), Analysis.bPerInstanceVariation ? TEXT(" perinstance=0.5") : TEXT(""));
	FString CacheKey, WhyNot;
	const bool bCacheable = ComputeCacheKey(Salt, Objects, CacheKey, WhyNot);
	{
		FMantleConvertedMaterial Cached;
		TArray<FMantleConvertedTexture> CachedTextures;
		TArray<FMantleValidationIssue> CachedIssues;
		if (bCacheable && LoadCachedMaterial(CacheKey, Cached, CachedTextures, CachedIssues))
		{
			for (FMantleConvertedTexture& Texture : CachedTextures)
			{
				Out.AddTexture(MoveTemp(Texture));
			}
			Out.Issues.Append(CachedIssues);
			Cached.Key = Key;
			Cached.bFromCache = true;
			++Stats.MaterialsFromCache;
			Out.Materials.Add(MoveTemp(Cached));
			return true;
		}
	}
	if (!bCacheable)
	{
		UE_LOG(LogMantle, Verbose, TEXT("MANTLE_F5 %s is not cached: %s"), *Material->GetName(), *WhyNot);
	}

	const int32 FirstIssue = Out.Issues.Num();
	FMantleConvertedMaterial Converted;
	Converted.Key = Key;
	Converted.Name = Material->GetName();
	Converted.Path = Material->GetPathName();

	// #628: Per Instance Random and its kin have no value in a bake (they read 0, which blacks out a base colour they multiply),
	// so the bake runs on a transient copy with each at a representative value, and the creator is told the variation is averaged.
	TStrongObjectPtr<UMaterialInterface> Neutral;
	if (Analysis.bPerInstanceVariation)
	{
		Neutral = FMantlePlaceAssets::NeutralisePerInstanceValues(Material);
		if (Neutral.IsValid())
		{
			Out.Issues.Add(WarningIssue(FString::Printf(TEXT("Material '%s': %s"), *Converted.Name, *FMantlePlaceAssets::DescribePerInstanceVariation()),
				TEXT("Nothing to do unless the variation matters; give each look its own material to keep it.")));
		}
	}

	++Stats.FlattenerCalls;
	FMantleGlbDocument Flattened;
	TArray<FString> Messages;
	FString FlattenError;
	if (Flattener->Flatten(Neutral.IsValid() ? Neutral.Get() : Material, Settings, Flattened, Messages, FlattenError))
	{
		for (const FString& Message : Messages)
		{
			UE_LOG(LogMantle, Log, TEXT("MANTLE_F5 GLTFExporter on %s: %s"), *Material->GetName(), *Message);
		}
		Converted.Document = MaterialFromGltf(Flattened, 0, Converted.Name, Limits, bOversizeReported, Out, MaterialExtras(Analysis, Converted.Name, Out.Issues));
	}
	else
	{
		// Friendlier than refusing the place: it publishes, plain, and the creator is told why.
		Out.Issues.Add(WarningIssue(FString::Printf(TEXT("Material '%s' could not be flattened (%s), so it publishes as plain grey."), *Converted.Name, *FlattenError),
			TEXT("Try a simpler material on these meshes, or report it with the log if it keeps happening.")));
		Converted.Document.BaseColorFactor[0] = Converted.Document.BaseColorFactor[1] = Converted.Document.BaseColorFactor[2] = 0.5;
		Converted.Document.MetallicFactor = 0.0;
	}
	// A Clear Coat source keeps its clear coat as Mantle's car paint (#487); everything else stays default shading.
	if (FMantlePlaceAssets::PublishesAsCarPaint(Material) && Converted.Document.AlphaMode == TEXT("opaque") && !Converted.Document.bDoubleSided)
	{
		Converted.Document.bCarPaint = true;
		Converted.Document.CarPaint = FMantlePlaceAssets::CarPaintFinish(Material);
		// #471 on car paint: M_MantlePlace_CarPaint has none of the extended master's inputs, and the
		// schema refuses #471's keys next to shading carPaint, so a car-paint document drops them.
		FMantlePlaceMaterialDocument& Paint = Converted.Document;
		const bool bDropsFidelity = Paint.UvTransforms.Num() > 0 || !FMath::IsNearlyEqual(Paint.NormalScale, 1.0)
			|| !FMath::IsNearlyEqual(Paint.OcclusionStrength, 1.0) || Paint.OcclusionUV != 0
			|| !Paint.VertexColor.Equals(TEXT("none"), ESearchCase::CaseSensitive) || Paint.HasFidelity2();
		if (bDropsFidelity)
		{
			Out.Issues.Add(WarningIssue(FString::Printf(TEXT("Material '%s' publishes as car paint, which keeps one texture tiling for all its textures, full-strength normal and occlusion, the first UV channel and no vertex-colour tint, so those parts of it are dropped."), *Converted.Name)));
			Paint.UvTransforms.Reset();
			Paint.NormalScale = 1.0;
			Paint.OcclusionStrength = 1.0;
			Paint.OcclusionUV = 0;
			Paint.VertexColor = TEXT("none");
			// #533's never reach here (AnalyzeMaterial carries none on car paint); dropped all the same.
			Paint.UvPanning.Reset();
			Paint.bWind = false;
			Paint.bWorldAligned = false;
			Paint.bLayerBlend = false;
			Paint.bFoliage = false;
			Paint.bWater = false;
		}
	}
	if (!Flattener->CanBakeTextures())
	{
		Out.Issues.Add(WarningIssue(FString::Printf(TEXT("Material '%s' was flattened without a renderer, so none of its textures could be baked."), *Converted.Name),
			TEXT("Publish from the editor with a window open to get its textures.")));
	}

	++Stats.MaterialsConverted;
	TArray<FMantleValidationIssue> NewIssues;
	for (int32 Index = FirstIssue; Index < Out.Issues.Num(); ++Index)
	{
		NewIssues.Add(Out.Issues[Index]);
	}
	if (bCacheable)
	{
		StoreCachedMaterial(CacheKey, Converted, Out, NewIssues);
	}
	Out.Materials.Add(MoveTemp(Converted));
	return true;
}

FMantlePlaceMaterialDocument FMantlePlaceAssetConverter::MaterialFromGltf(const FMantleGlbDocument& Document, int32 Index, const FString& MaterialName,
	const FMantlePlaceAssetLimits& Limits, bool bSourceTexturesReported, FMantlePlaceAssetSet& OutSet, bool bVertexColorTintsBaseColor)
{
	FMantleMaterialExtras Extras;
	Extras.bVertexColorTintsBaseColor = bVertexColorTintsBaseColor;
	return MaterialFromGltf(Document, Index, MaterialName, Limits, bSourceTexturesReported, OutSet, Extras);
}

FMantlePlaceMaterialDocument FMantlePlaceAssetConverter::MaterialFromGltf(const FMantleGlbDocument& Document, int32 Index, const FString& MaterialName,
	const FMantlePlaceAssetLimits& Limits, bool bSourceTexturesReported, FMantlePlaceAssetSet& OutSet, const FMantleMaterialExtras& Extras)
{
	using namespace MantlePlaceAssetConverterPrivate;
	const bool bVertexColorTintsBaseColor = Extras.bVertexColorTintsBaseColor;
	const FMantleMaterialFidelity2& Fidelity = Extras.Fidelity;
	FMantlePlaceMaterialDocument Doc;
	const FString Subject = FString::Printf(TEXT("Material '%s'"), *MaterialName);
	TSet<FString> Said;
	auto Warn = [&OutSet, &Subject, &Said](const FString& Problem)
	{
		if (!Said.Contains(Problem))
		{
			Said.Add(Problem);
			OutSet.Issues.Add(WarningIssue(FString::Printf(TEXT("%s: %s"), *Subject, *Problem)));
		}
	};

	const TArray<TSharedPtr<FJsonValue>>* Materials = nullptr;
	if (!Document.Json.IsValid() || !Document.Json->TryGetArrayField(TEXT("materials"), Materials) || !Materials->IsValidIndex(Index)
		|| !(*Materials)[Index].IsValid() || !(*Materials)[Index]->AsObject().IsValid())
	{
		Warn(TEXT("GLTFExporter wrote nothing for it, so it publishes as plain white."));
		return Doc;
	}
	const TSharedPtr<FJsonObject> Material = (*Materials)[Index]->AsObject();

	FString AlphaMode = TEXT("OPAQUE");
	Material->TryGetStringField(TEXT("alphaMode"), AlphaMode);
	Doc.AlphaMode = AlphaMode.ToLower();
	if (Doc.AlphaMode != TEXT("mask") && Doc.AlphaMode != TEXT("blend"))
	{
		Doc.AlphaMode = TEXT("opaque");
	}
	Doc.AlphaCutoff = FMath::Clamp(NumberField(Material, TEXT("alphaCutoff"), 0.5), 0.0, 1.0);
	Material->TryGetBoolField(TEXT("doubleSided"), Doc.bDoubleSided);

	const TSharedPtr<FJsonObject> Extensions = ObjectField(Material, TEXT("extensions"));
	const bool bUnlit = ObjectField(Extensions, TEXT("KHR_materials_unlit")).IsValid();
	if (Extensions.IsValid())
	{
		for (const auto& Pair : Extensions->Values)
		{
			FString Part(Pair.Key.ToView());
			// Clear coat is read from the source material, not the glTF (ConvertMaterial, #487), and Check says what becomes of it.
			// The index of refraction of a translucent surface is drawn as glass (GlassOpacityFromIor, #628).
			if (Part != TEXT("KHR_materials_unlit") && Part != TEXT("KHR_materials_emissive_strength") && Part != TEXT("KHR_materials_clearcoat")
				&& !(Part == TEXT("KHR_materials_ior") && Doc.AlphaMode == TEXT("blend")))
			{
				Part.RemoveFromStart(TEXT("KHR_materials_"));
				Warn(FString::Printf(TEXT("its %s part is dropped."), *Part));
			}
		}
	}

	const TSharedPtr<FJsonObject> Pbr = ObjectField(Material, TEXT("pbrMetallicRoughness"));
	NumberArray(Pbr, TEXT("baseColorFactor"), Doc.BaseColorFactor, 4);
	if (const TSharedPtr<FJsonObject> IorExtension = ObjectField(Extensions, TEXT("KHR_materials_ior")); IorExtension.IsValid() && Doc.AlphaMode == TEXT("blend"))
	{
		Doc.BaseColorFactor[3] = GlassOpacityFromIor(NumberField(IorExtension, TEXT("ior"), 1.5), Doc.BaseColorFactor[3]);
	}
	Doc.MetallicFactor = NumberField(Pbr, TEXT("metallicFactor"), 1.0);
	Doc.RoughnessFactor = NumberField(Pbr, TEXT("roughnessFactor"), 1.0);
	NumberArray(Material, TEXT("emissiveFactor"), Doc.EmissiveFactor, 3);
	Doc.EmissiveStrength = NumberField(ObjectField(Extensions, TEXT("KHR_materials_emissive_strength")), TEXT("emissiveStrength"), 1.0);
	if (Doc.EmissiveStrength > 100.0)
	{
		Warn(FString::Printf(TEXT("its emissive strength of %s is capped at 100."), *Number(Doc.EmissiveStrength)));
	}

	const FTextureRef BaseColor = ReadTextureRef(ObjectField(Pbr, TEXT("baseColorTexture")));
	const FTextureRef MetallicRoughness = ReadTextureRef(ObjectField(Pbr, TEXT("metallicRoughnessTexture")));
	const TSharedPtr<FJsonObject> NormalInfo = ObjectField(Material, TEXT("normalTexture"));
	const TSharedPtr<FJsonObject> OcclusionInfo = ObjectField(Material, TEXT("occlusionTexture"));
	const FTextureRef Normal = ReadTextureRef(NormalInfo);
	const FTextureRef Occlusion = ReadTextureRef(OcclusionInfo);
	const FTextureRef Emissive = ReadTextureRef(ObjectField(Material, TEXT("emissiveTexture")));
	const TCHAR* const SecondUvWarning = TEXT("it samples a second UV channel; Mantle uses the first, so that texture may land in the wrong place.");
	const TCHAR* const TilingClampWarning = TEXT("its texture tiling is beyond what Mantle keeps (1024), so it is clamped.");

	// The shared UV transform (uvScale, uvOffset): the base colour's, else the first texture's, so a
	// material whose textures agree writes what it always did. A texture whose own transform differs
	// (tiling, offset or any rotation) gets it in uvTransforms (#471) once its image is in. Only
	// occlusion may sample the second UV (occlusionUV).
	const FTextureRef* Canonical = nullptr;
	for (const FTextureRef* Ref : { &BaseColor, &Normal, &MetallicRoughness, &Occlusion, &Emissive })
	{
		if (Ref->IsSet())
		{
			Canonical = Canonical ? Canonical : Ref;
			if (Ref->TexCoord != 0 && Ref != &Occlusion)
			{
				Warn(SecondUvWarning);
			}
		}
	}
	FTextureRef Shared;
	if (Canonical)
	{
		Doc.UvScale[0] = Shared.Scale[0] = Canonical->Scale[0];
		Doc.UvScale[1] = Shared.Scale[1] = Canonical->Scale[1];
		Doc.UvOffset[0] = Shared.Offset[0] = Canonical->Offset[0];
		Doc.UvOffset[1] = Shared.Offset[1] = Canonical->Offset[1];
		for (int32 Axis = 0; Axis < 2; ++Axis)
		{
			if (FMath::Abs(Doc.UvScale[Axis]) > 1024.0 || FMath::Abs(Doc.UvOffset[Axis]) > 1024.0)
			{
				Warn(TilingClampWarning);
			}
		}
	}
	// A slot whose texture made it in and whose transform is not the shared one keeps its own.
	auto KeepSlotTransform = [&](EMantlePlaceTextureSlot Slot, const FTextureRef& Ref)
	{
		const TCHAR* const Name = FMantlePlaceMaterialDocument::SlotName(Slot);
		if (!Ref.IsSet() || !Doc.Textures.Contains(Name) || Ref.SameTransform(Shared))
		{
			return;
		}
		FMantlePlaceDocumentUvTransform Transform;
		for (int32 Axis = 0; Axis < 2; ++Axis)
		{
			Transform.Offset[Axis] = Ref.Offset[Axis];
			Transform.Scale[Axis] = Ref.Scale[Axis];
			if (FMath::Abs(Ref.Scale[Axis]) > 1024.0 || FMath::Abs(Ref.Offset[Axis]) > 1024.0)
			{
				Warn(TilingClampWarning);
			}
		}
		Transform.Rotation = Ref.Rotation;
		Doc.UvTransforms.Add(Name, Transform);
	};

	auto AddImage = [&](const FTextureRef& Ref, EMantlePlaceTextureSlot Slot, bool bKeepAlpha) -> bool
	{
		TArray<uint8> Bytes;
		int32 Source = INDEX_NONE;
		FString TextureName;
		if (!ReadTextureImage(Document, Ref.Texture, Bytes, Source, TextureName))
		{
			Warn(FString::Printf(TEXT("its %s image could not be read, so it is left out."), SlotLabel(Slot)));
			return false;
		}
		const FString What = FString::Printf(TEXT("%s (%s of material '%s')"), TextureName.IsEmpty() ? TEXT("baked texture") : *TextureName,
			SlotLabel(Slot), *MaterialName);
		FMantleConvertedTexture Texture;
		if (!PrepareTexture(Bytes, Slot, bKeepAlpha, Limits, What, !bSourceTexturesReported, Texture, OutSet.Issues))
		{
			Warn(FString::Printf(TEXT("its %s image could not be decoded, so it is left out."), SlotLabel(Slot)));
			return false;
		}
		Doc.Textures.Add(FMantlePlaceMaterialDocument::SlotName(Slot), OutSet.AddTexture(MoveTemp(Texture)));
		return true;
	};

	// #533: a slot's source texture (Extras.SourceImages, EMantlePlaceTextureSlot order) in place of the bake.
	auto HasSource = [&Extras](EMantlePlaceTextureSlot Slot) { return Extras.SourceImages[static_cast<int32>(Slot)].Num() > 0; };
	auto AddSource = [&](EMantlePlaceTextureSlot Slot, bool bKeepAlpha) -> bool
	{
		const int32 Source = static_cast<int32>(Slot);
		const FString What = FString::Printf(TEXT("%s (%s of material '%s')"), Extras.SourceNames[Source].IsEmpty() ? TEXT("source texture") : *Extras.SourceNames[Source],
			SlotLabel(Slot), *MaterialName);
		FMantleConvertedTexture Texture;
		if (!PrepareTexture(Extras.SourceImages[Source], Slot, bKeepAlpha, Limits, What, !bSourceTexturesReported, Texture, OutSet.Issues))
		{
			Warn(FString::Printf(TEXT("its %s texture could not be decoded, so it is left out."), SlotLabel(Slot)));
			return false;
		}
		Doc.Textures.Add(FMantlePlaceMaterialDocument::SlotName(Slot), OutSet.AddTexture(MoveTemp(Texture)));
		return true;
	};
	// A source slot samples the first UV set scaled by its own tiling (world-aligned ones by world position, no UVs).
	auto KeepSourceTransform = [&](EMantlePlaceTextureSlot Slot)
	{
		const int32 Source = static_cast<int32>(Slot);
		if (Source >= 4 || !Doc.Textures.Contains(FMantlePlaceMaterialDocument::SlotName(Slot))
			|| (Fidelity.bWorldAligned && Slot != EMantlePlaceTextureSlot::Emissive))
		{
			return;
		}
		FTextureRef Own;
		Own.Scale[0] = Fidelity.SourceTiling[Source][0];
		Own.Scale[1] = Fidelity.SourceTiling[Source][1];
		if (Own.SameTransform(Shared))
		{
			return;
		}
		FMantlePlaceDocumentUvTransform Transform;
		Transform.Scale[0] = Own.Scale[0];
		Transform.Scale[1] = Own.Scale[1];
		if (FMath::Abs(Own.Scale[0]) > 1024.0 || FMath::Abs(Own.Scale[1]) > 1024.0)
		{
			Warn(TilingClampWarning);
		}
		Doc.UvTransforms.Add(FMantlePlaceMaterialDocument::SlotName(Slot), Transform);
	};
	// Every slot that made it in with a pan keeps it; wind rides on any lit or unlit material.
	auto KeepPanningAndWind = [&]()
	{
		for (int32 Slot = 0; Slot < 4; ++Slot)
		{
			const TCHAR* const Name = FMantlePlaceMaterialDocument::SlotName(static_cast<EMantlePlaceTextureSlot>(Slot));
			if (Fidelity.bPanning[Slot] && Doc.Textures.Contains(Name) && (Fidelity.Panning[Slot][0] != 0.0 || Fidelity.Panning[Slot][1] != 0.0))
			{
				Doc.UvPanning.Add(Name, FVector2D(Fidelity.Panning[Slot][0], Fidelity.Panning[Slot][1]));
			}
		}
		if (Fidelity.bWind && (Doc.AlphaMode == TEXT("opaque") || Doc.AlphaMode == TEXT("mask")))
		{
			Doc.bWind = true;
			Doc.WindIntensity = Fidelity.WindIntensity;
			Doc.WindSpeed = Fidelity.WindSpeed;
			Doc.WindWeight = Fidelity.WindWeight;
		}
		// #628: the document's own rule (masked, two-sided) decides; the shading model came from the source material.
		if (Fidelity.bFoliage && Doc.AlphaMode == TEXT("mask") && Doc.bDoubleSided)
		{
			Doc.bFoliage = true;
			for (int32 Channel = 0; Channel < 3; ++Channel)
			{
				Doc.FoliageTint[Channel] = Fidelity.FoliageTint[Channel];
			}
			Doc.FoliageStrength = Fidelity.FoliageStrength;
			Doc.FoliageVertexColorWeight = Fidelity.FoliageVertexColorWeight;
		}
	};

	// #628: an unlit translucent material that draws the scene behind it (Scene Color on its emissive path): the bake saw black there,
	// an opaque solid. It publishes as what the source reads as, see-through tinted glass: a translucent surface with the glass opacity
	// and the source's tint (times its intensity, scaled down) as its glow, with nothing of the bake (every texture of it was black).
	if (Fidelity.Glass.bFound)
	{
		const FMantleSeeThroughGlass& Glass = Fidelity.Glass;
		double Glow[3];
		Glass.Glow(Glow);
		Doc.AlphaMode = TEXT("blend");
		Doc.BaseColorFactor[0] = Doc.BaseColorFactor[1] = Doc.BaseColorFactor[2] = 0.0;
		Doc.BaseColorFactor[3] = FMantleSeeThroughGlass::Opacity;
		Doc.MetallicFactor = 0.0;
		Doc.RoughnessFactor = 0.1;
		for (int32 Channel = 0; Channel < 3; ++Channel)
		{
			Doc.EmissiveFactor[Channel] = Glow[Channel];
		}
		Doc.EmissiveStrength = 1.0;
		Doc.UvScale[0] = Doc.UvScale[1] = 1.0;
		Doc.UvOffset[0] = Doc.UvOffset[1] = 0.0;
		UE_LOG(LogMantle, Log, TEXT("MANTLE_GLASS %s: scene-colour refraction published as see-through glass, opacity %.2f, glow %.4f %.4f %.4f (tint %s %.4f %.4f %.4f x %.3f)"),
			*MaterialName, FMantleSeeThroughGlass::Opacity, Glow[0], Glow[1], Glow[2], Glass.bTint ? TEXT("read") : TEXT("neutral"),
			Glass.Tint[0], Glass.Tint[1], Glass.Tint[2], Glass.Intensity);
		return Doc;
	}

	// #628: additive blending (alphaMode additive): emissive-driven, so the glow is all there is of it. An unlit source (GLTFExporter
	// keeps its glow as the base colour) and a lit one (its glow is the emissive) end up the same: emissive factor, strength and texture,
	// the emissive slot's own transform and pan. A constant opacity scales the glow (additive adds colour x opacity); the base colour
	// has no part in it (the bake's black is not carried), and a base colour texture's alpha is not read.
	if (Fidelity.bAdditive)
	{
		Doc.AlphaMode = TEXT("additive");
		const FTextureRef& Glow = bUnlit ? BaseColor : Emissive;
		if (bUnlit)
		{
			Doc.EmissiveFactor[0] = Doc.BaseColorFactor[0];
			Doc.EmissiveFactor[1] = Doc.BaseColorFactor[1];
			Doc.EmissiveFactor[2] = Doc.BaseColorFactor[2];
			Doc.EmissiveStrength = 1.0;
		}
		const double Opacity = Doc.BaseColorFactor[3];
		if (Opacity > 0.0 && Opacity < 1.0)
		{
			for (int32 Channel = 0; Channel < 3; ++Channel)
			{
				Doc.EmissiveFactor[Channel] *= Opacity;
			}
		}
		Doc.BaseColorFactor[0] = Doc.BaseColorFactor[1] = Doc.BaseColorFactor[2] = 0.0;
		Doc.BaseColorFactor[3] = 1.0;
		Doc.MetallicFactor = 0.0;
		Doc.RoughnessFactor = 1.0;
		if (HasSource(EMantlePlaceTextureSlot::Emissive) && AddSource(EMantlePlaceTextureSlot::Emissive, false))
		{
			KeepSourceTransform(EMantlePlaceTextureSlot::Emissive);
		}
		else if (Glow.IsSet() && AddImage(Glow, EMantlePlaceTextureSlot::Emissive, false))
		{
			KeepSlotTransform(EMantlePlaceTextureSlot::Emissive, Glow);
		}
		if (bVertexColorTintsBaseColor)
		{
			Warn(TEXT("its vertex-colour tint is dropped: additive blending shows the glow only, and vertex colour does not reach it."));
		}
		KeepPanningAndWind();
		return Doc;
	}

	if (bUnlit)
	{
		// Unlit: the colour is light. M_MantlePlace is lit, so the colour moves to emissive over black.
		Doc.EmissiveFactor[0] = Doc.BaseColorFactor[0];
		Doc.EmissiveFactor[1] = Doc.BaseColorFactor[1];
		Doc.EmissiveFactor[2] = Doc.BaseColorFactor[2];
		Doc.EmissiveStrength = 1.0;
		Doc.BaseColorFactor[0] = Doc.BaseColorFactor[1] = Doc.BaseColorFactor[2] = 0.0;
		Doc.MetallicFactor = 0.0;
		Doc.RoughnessFactor = 1.0;
		// A panned glow (#533) publishes its source texture, which is what glTF calls the unlit base colour.
		if (HasSource(EMantlePlaceTextureSlot::Emissive) && AddSource(EMantlePlaceTextureSlot::Emissive, false))
		{
			KeepSourceTransform(EMantlePlaceTextureSlot::Emissive);
			if (BaseColor.IsSet() && Doc.AlphaMode != TEXT("opaque"))
			{
				AddImage(BaseColor, EMantlePlaceTextureSlot::BaseColor, true);
				KeepSlotTransform(EMantlePlaceTextureSlot::BaseColor, BaseColor);
			}
		}
		else if (BaseColor.IsSet())
		{
			AddImage(BaseColor, EMantlePlaceTextureSlot::Emissive, false);
			if (Doc.AlphaMode != TEXT("opaque"))
			{
				AddImage(BaseColor, EMantlePlaceTextureSlot::BaseColor, true);
			}
			KeepSlotTransform(EMantlePlaceTextureSlot::Emissive, BaseColor);
			KeepSlotTransform(EMantlePlaceTextureSlot::BaseColor, BaseColor);
		}
		if (bVertexColorTintsBaseColor)
		{
			Warn(TEXT("its vertex-colour tint is dropped: it is unlit, and Mantle shows an unlit colour as glow, which vertex colour does not reach."));
		}
		KeepPanningAndWind();
		return Doc;
	}

	// #628: a depth-faded translucent material (water) publishes its far-end opacity in place of the bake's.
	const bool bWater = Fidelity.Water.bFound && (Doc.AlphaMode == TEXT("blend") || Fidelity.Water.bSingleLayer);

	// #628: near-opaque water publishes with the water key (Single Layer Water, M_MantlePlace_Water): the surface colour, the
	// roughness, the normal maps with their layout and pan, and coefficients from the deep colour and the fade distance. A client
	// that cannot read the key gets the opaque material it always did (FMantleTargetFeatures::DowngradeMaterial).
	if (bWater && Fidelity.Water.PublishesOpaque())
	{
		const FMantleWaterBake& Water = Fidelity.Water;
		Doc.bWater = true;
		Doc.AlphaMode = TEXT("opaque");
		Doc.BaseColorFactor[3] = 1.0;
		if (Water.bColour)
		{
			for (int32 Channel = 0; Channel < 3; ++Channel)
			{
				Doc.BaseColorFactor[Channel] = Water.Colour[Channel];
			}
		}

		if (Water.bWavesDropped)
		{
			Warn(TEXT("its waves (world position offset) are dropped: Mantle's water is a flat surface with moving normal maps."));
		}

		// What the schema says does nothing on water is not written; what the source had of it is said.
		if (BaseColor.IsSet())
		{
			Warn(TEXT("its base colour texture is dropped: water has a surface colour and a volume, not a colour texture."));
		}
		if (MetallicRoughness.IsSet() || Occlusion.IsSet())
		{
			Warn(TEXT("its roughness, metallic and occlusion textures are dropped: water keeps its roughness value and is not metallic."));
		}
		if (Emissive.IsSet() || Doc.EmissiveFactor[0] > 0.0 || Doc.EmissiveFactor[1] > 0.0 || Doc.EmissiveFactor[2] > 0.0)
		{
			Warn(TEXT("its emissive is dropped: water does not glow."));
		}
		if (bVertexColorTintsBaseColor)
		{
			Warn(TEXT("its vertex-colour tint is dropped: water has none."));
		}
		if (Doc.MetallicFactor > 0.01)
		{
			Warn(TEXT("its metallic is dropped: water is not metallic."));
		}
		Doc.MetallicFactor = 0.0;

		// The volume: coefficients from the deep colour (the surface colour) and the fade distance (FMantleWaterMaterial::Coefficients).
		double Deep[3];
		for (int32 Channel = 0; Channel < 3; ++Channel)
		{
			Deep[Channel] = FMath::Clamp(Doc.BaseColorFactor[Channel], 0.0, 1.0);
		}
		const FMantleWaterCoefficients Coefficients = Water.bVolume ? Water.Volume : FMantleWaterMaterial::Coefficients(Deep, Water.bFadeDistance ? Water.FadeDistanceCm : 0.0);
		for (int32 Channel = 0; Channel < 3; ++Channel)
		{
			Doc.WaterScattering[Channel] = Coefficients.Scattering[Channel];
			Doc.WaterAbsorption[Channel] = Coefficients.Absorption[Channel];
		}

		// The normal maps: the graph's source textures with their layout and pan (Water.Normals, the first two), else the bake's normal.
		auto LayerOf = [&Warn, &TilingClampWarning](const FMantleWaterNormalLayer& Layer)
		{
			FMantlePlaceDocumentWaterNormal Out;
			Out.bWorld = Layer.bWorld;
			Out.WorldSizeCm = Layer.WorldSizeCm > 0.0 ? Layer.WorldSizeCm : 100.0;
			for (int32 Axis = 0; Axis < 2; ++Axis)
			{
				Out.Tiling[Axis] = Layer.Tiling[Axis];
				Out.Panning[Axis] = Layer.Speed[Axis];
				if (!Layer.bWorld && FMath::Abs(Layer.Tiling[Axis]) > 1024.0)
				{
					Warn(TilingClampWarning);
				}
			}
			if (Layer.bApproximated)
			{
				Warn(TEXT("its water normal map is laid out by world position in a way Mantle keeps only roughly (one size on both axes, one sample of a custom function, or a size out of range), so its ripples may not line up."));
			}
			return Out;
		};
		if (HasSource(EMantlePlaceTextureSlot::Normal))
		{
			if (AddSource(EMantlePlaceTextureSlot::Normal, false))
			{
				if (Water.Normals.Num() > 0)
				{
					Doc.WaterNormal = LayerOf(Water.Normals[0]);
					Doc.NormalScale = FMath::Clamp(Water.NormalScale, 0.0, 10.0);
					if (Water.Normals.Num() > 1 && HasSource(EMantlePlaceTextureSlot::WaterNormal2) && AddSource(EMantlePlaceTextureSlot::WaterNormal2, false))
					{
						Doc.WaterNormal2 = LayerOf(Water.Normals[1]);
					}
					if (Water.NormalSamples > 2)
					{
						Warn(FString::Printf(TEXT("it blends %d normal maps; Mantle's water keeps the first two."), Water.NormalSamples));
					}
				}
				else
				{
					// A normal the finder did not read but the generic panned-sample recognition did.
					Doc.WaterNormal.Tiling[0] = Fidelity.SourceTiling[1][0];
					Doc.WaterNormal.Tiling[1] = Fidelity.SourceTiling[1][1];
					if (Fidelity.bPanning[1])
					{
						Doc.WaterNormal.Panning[0] = Fidelity.Panning[1][0];
						Doc.WaterNormal.Panning[1] = Fidelity.Panning[1][1];
					}
				}
			}
		}
		else if (Normal.IsSet() && AddImage(Normal, EMantlePlaceTextureSlot::Normal, false))
		{
			const double Strength = NumberField(NormalInfo, TEXT("scale"), 1.0);
			Doc.NormalScale = FMath::Clamp(Strength, 0.0, 10.0);
			if (Strength < 0.0 || Strength > 10.0)
			{
				Warn(FString::Printf(TEXT("its normal strength of %s is outside what Mantle keeps (0 to 10), so it is clamped."), *Number(Strength)));
			}
		}

		const FString FadeText = Water.bFadeDistance ? FString::Printf(TEXT("%.2f cm"), Water.FadeDistanceCm) : FString(TEXT("not read (100 cm assumed)"));
		UE_LOG(LogMantle, Log, TEXT("MANTLE_WATER %s: published with the water key, colour %.4f %.4f %.4f, fade distance %s, scattering %.4f %.4f %.4f, absorption %.4f %.4f %.4f, %d normal layer(s)%s%s"),
			*MaterialName, Doc.BaseColorFactor[0], Doc.BaseColorFactor[1], Doc.BaseColorFactor[2], *FadeText,
			Doc.WaterScattering[0], Doc.WaterScattering[1], Doc.WaterScattering[2], Doc.WaterAbsorption[0], Doc.WaterAbsorption[1], Doc.WaterAbsorption[2],
			Doc.Textures.Contains(WaterNormal2SlotKey) ? 2 : (Doc.Textures.Contains(TEXT("normal")) ? 1 : 0),
			Water.Why.IsEmpty() ? TEXT("") : TEXT("; "), *Water.Why);
		return Doc;
	}

	// Vertex colour on the base colour (#471): the bake reads it as white, so the tint rides as a flag.
	if (bVertexColorTintsBaseColor && !Fidelity.bLayerBlend)
	{
		Doc.VertexColor = TEXT("multiplyBaseColor");
	}
	if (HasSource(EMantlePlaceTextureSlot::BaseColor))
	{
		if (AddSource(EMantlePlaceTextureSlot::BaseColor, Doc.AlphaMode != TEXT("opaque") && Fidelity.bSourceAlpha))
		{
			KeepSourceTransform(EMantlePlaceTextureSlot::BaseColor);
		}
	}
	else if (BaseColor.IsSet() && !(bWater && Fidelity.Water.bColour))
	{
		// Water's baked alpha is the fade's near end (0), so the image keeps no alpha; the opacity is the factor's.
		AddImage(BaseColor, EMantlePlaceTextureSlot::BaseColor, Doc.AlphaMode != TEXT("opaque") && !bWater);
		KeepSlotTransform(EMantlePlaceTextureSlot::BaseColor, BaseColor);
	}
	if (bWater)
	{
		// #628: the depth fade's far end (FMantleWaterMaterial): one constant opacity, and the deep colour when the graph gave one.
		Doc.BaseColorFactor[3] = Fidelity.Water.Opacity;
		if (Fidelity.Water.PublishesOpaque())
		{
			// Near-opaque water reflects the scene only as an opaque material (translucent ones get no screen-space or Lumen reflections).
			Doc.AlphaMode = TEXT("opaque");
			Doc.BaseColorFactor[3] = 1.0;
		}
		if (Fidelity.Water.bColour)
		{
			for (int32 Channel = 0; Channel < 3; ++Channel)
			{
				Doc.BaseColorFactor[Channel] = Fidelity.Water.Colour[Channel];
			}
		}
		UE_LOG(LogMantle, Log, TEXT("MANTLE_WATER %s: depth fade baked to opacity %.3f%s (%s), colour %s%s%s"), *MaterialName, Fidelity.Water.Opacity,
			Fidelity.Water.PublishesOpaque() ? TEXT(", published opaque") : TEXT(""),
			Fidelity.Water.bOpacityRead ? TEXT("read from the graph") : TEXT("estimated"),
			Fidelity.Water.bColour ? *FString::Printf(TEXT("%.4f %.4f %.4f"), Fidelity.Water.Colour[0], Fidelity.Water.Colour[1], Fidelity.Water.Colour[2]) : TEXT("kept from the bake"),
			Fidelity.Water.Why.IsEmpty() ? TEXT("") : TEXT("; "), *Fidelity.Water.Why);
	}
	if (HasSource(EMantlePlaceTextureSlot::Normal))
	{
		if (AddSource(EMantlePlaceTextureSlot::Normal, false))
		{
			KeepSourceTransform(EMantlePlaceTextureSlot::Normal);
			// #628: water's panned normal keeps its FlattenNormal strength (the source texture carries none of it).
			if (Fidelity.Water.bNormalPanned)
			{
				Doc.NormalScale = FMath::Clamp(Fidelity.Water.NormalScale, 0.0, 10.0);
			}
		}
	}
	else if (Normal.IsSet() && AddImage(Normal, EMantlePlaceTextureSlot::Normal, false))
	{
		KeepSlotTransform(EMantlePlaceTextureSlot::Normal, Normal);
		const double Strength = NumberField(NormalInfo, TEXT("scale"), 1.0);
		Doc.NormalScale = FMath::Clamp(Strength, 0.0, 10.0);
		if (Strength < 0.0 || Strength > 10.0)
		{
			Warn(FString::Printf(TEXT("its normal strength of %s is outside what Mantle keeps (0 to 10), so it is clamped."), *Number(Strength)));
		}
	}
	if (HasSource(EMantlePlaceTextureSlot::Emissive))
	{
		if (AddSource(EMantlePlaceTextureSlot::Emissive, false))
		{
			KeepSourceTransform(EMantlePlaceTextureSlot::Emissive);
		}
	}
	else if (Emissive.IsSet())
	{
		AddImage(Emissive, EMantlePlaceTextureSlot::Emissive, false);
		KeepSlotTransform(EMantlePlaceTextureSlot::Emissive, Emissive);
	}

	// ORM: a source (#533) is already packed as glTF's, with the factors the graph gives; else glTF keeps
	// occlusion (R) apart from metallic-roughness (G, B), and Mantle's orm packs them.
	if (HasSource(EMantlePlaceTextureSlot::Orm))
	{
		if (AddSource(EMantlePlaceTextureSlot::Orm, false))
		{
			KeepSourceTransform(EMantlePlaceTextureSlot::Orm);
			Doc.RoughnessFactor = 1.0;
			Doc.MetallicFactor = Fidelity.OrmMetallicFactor;
			Doc.OcclusionStrength = Fidelity.OrmOcclusionStrength;
			Doc.OcclusionUV = 0;
		}
	}
	else if (MetallicRoughness.IsSet() || Occlusion.IsSet())
	{
		TArray<uint8> MrBytes, OcclusionBytes;
		int32 MrSource = INDEX_NONE, OcclusionSource = INDEX_NONE;
		FString MrName, OcclusionName;
		const bool bMr = MetallicRoughness.IsSet() && ReadTextureImage(Document, MetallicRoughness.Texture, MrBytes, MrSource, MrName);
		const bool bOcclusion = Occlusion.IsSet() && ReadTextureImage(Document, Occlusion.Texture, OcclusionBytes, OcclusionSource, OcclusionName);
		TArray<FColor> Mr, Ao;
		int32 MrWidth = 0, MrHeight = 0, AoWidth = 0, AoHeight = 0;
		const bool bMrPixels = bMr && DecodeBgra(MrBytes, false, Mr, MrWidth, MrHeight);
		bool bAoPixels = bOcclusion && DecodeBgra(OcclusionBytes, false, Ao, AoWidth, AoHeight);
		if (bMrPixels || bAoPixels)
		{
			const bool bSameImage = bMrPixels && bAoPixels && MrSource == OcclusionSource;
			const int32 Width = bMrPixels ? MrWidth : AoWidth;
			const int32 Height = bMrPixels ? MrHeight : AoHeight;
			if (bAoPixels && !bSameImage && (AoWidth != Width || AoHeight != Height))
			{
				Resize(Ao, AoWidth, AoHeight, Width, Height, false);
			}
			TArray<FColor> Orm;
			Orm.SetNumUninitialized(Width * Height);
			for (int32 Pixel = 0; Pixel < Orm.Num(); ++Pixel)
			{
				const uint8 R = bSameImage ? Mr[Pixel].R : (bAoPixels ? Ao[Pixel].R : 255);
				const uint8 G = bMrPixels ? Mr[Pixel].G : 255;
				const uint8 B = bMrPixels ? Mr[Pixel].B : 255;
				Orm[Pixel] = FColor(R, G, B, 255);
			}
			const TArray<uint8> Packed = FMantlePng::EncodeRGBA8(Width, Height, Orm);
			const FString Name = !MrName.IsEmpty() ? MrName : (!OcclusionName.IsEmpty() ? OcclusionName : FString(TEXT("baked texture")));
			const FString What = FString::Printf(TEXT("%s (%s of material '%s')"), *Name, SlotLabel(EMantlePlaceTextureSlot::Orm), *MaterialName);
			FMantleConvertedTexture Texture;
			if (PrepareTexture(Packed, EMantlePlaceTextureSlot::Orm, false, Limits, What, !bSourceTexturesReported, Texture, OutSet.Issues))
			{
				Doc.Textures.Add(FMantlePlaceMaterialDocument::SlotName(EMantlePlaceTextureSlot::Orm), OutSet.AddTexture(MoveTemp(Texture)));
				// One orm, one transform: metallic-roughness's, else occlusion's.
				KeepSlotTransform(EMantlePlaceTextureSlot::Orm, bMrPixels ? MetallicRoughness : Occlusion);
				if (bMrPixels && bAoPixels && !bSameImage && !Occlusion.SameTransform(MetallicRoughness))
				{
					Warn(TEXT("its occlusion texture uses different tiling or offsets from its roughness and metallic texture; Mantle packs them into one, so the occlusion may line up differently."));
				}
				if (bAoPixels)
				{
					// #471: occlusion keeps its strength and may sample the second UV (lightmap-style AO).
					Doc.OcclusionStrength = FMath::Clamp(NumberField(OcclusionInfo, TEXT("strength"), 1.0), 0.0, 1.0);
					Doc.OcclusionUV = Occlusion.TexCoord == 1 ? 1 : 0;
					if (Occlusion.TexCoord > 1)
					{
						Warn(SecondUvWarning);
					}
				}
			}
		}
		else
		{
			Warn(TEXT("its roughness, metallic and occlusion images could not be read, so they are left out."));
		}
	}

	// #533: panning and wind; then world-aligned mapping or a layer blend, opaque only (the analysis saw to it).
	KeepPanningAndWind();
	if (!Doc.bWind && Doc.AlphaMode == TEXT("opaque"))
	{
		if (Fidelity.bWorldAligned)
		{
			Doc.bWorldAligned = true;
			Doc.TileSizeCm = Fidelity.TileSizeCm;
			Doc.Sharpness = Fidelity.Sharpness;
		}
		else if (Fidelity.bLayerBlend)
		{
			Doc.bLayerBlend = true;
			Doc.LayerChannel = Fidelity.LayerChannel;
			for (const EMantlePlaceTextureSlot Layer : { EMantlePlaceTextureSlot::LayerBaseColor, EMantlePlaceTextureSlot::LayerNormal,
					 EMantlePlaceTextureSlot::LayerOrm })
			{
				if (HasSource(Layer))
				{
					AddSource(Layer, false);
				}
			}
		}
	}
	return Doc;
}

bool FMantlePlaceAssetConverter::PrepareTexture(TConstArrayView<uint8> Image, EMantlePlaceTextureSlot Slot, bool bKeepAlpha, const FMantlePlaceAssetLimits& Limits,
	const FString& What, bool bWarnDownscale, FMantleConvertedTexture& Out, TArray<FMantleValidationIssue>& OutIssues)
{
	using namespace MantlePlaceAssetConverterPrivate;
	Out = FMantleConvertedTexture();
	// A layer blend's slot (#533) is prepared as the base slot it mirrors.
	const EMantlePlaceTextureSlot BaseSlot = BaseSlotOf(Slot);
	const bool bColour = BaseSlot == EMantlePlaceTextureSlot::BaseColor || BaseSlot == EMantlePlaceTextureSlot::Emissive;
	TArray<FColor> Pixels;
	int32 Width = 0, Height = 0;
	if (!DecodeBgra(Image, bColour, Pixels, Width, Height))
	{
		return false;
	}

	// The one texture-size rule and its message, shared with Check this place (#452).
	FMantleValidationIssue Downscale;
	if (FMantlePlaceAssets::TextureDownscaleIssue(What, FIntPoint(Width, Height), Limits.MaxTextureSize, Downscale))
	{
		if (bWarnDownscale)
		{
			OutIssues.Add(MoveTemp(Downscale));
		}
		const FIntPoint Fitted = FMantlePlaceAssets::PublishedTextureSize(FIntPoint(Width, Height), Limits.MaxTextureSize);
		Resize(Pixels, Width, Height, Fitted.X, Fitted.Y, bColour);
	}

	bool bHasAlpha = false;
	for (const FColor& Pixel : Pixels)
	{
		if (Pixel.A < 255)
		{
			bHasAlpha = true;
			break;
		}
	}
	const bool bJpeg = bColour && !(bKeepAlpha && bHasAlpha && BaseSlot == EMantlePlaceTextureSlot::BaseColor);
	const int64 Cap = FMath::Min(Limits.MaxTextureBytes, Limits.MaxUploadBytes);

	TArray<uint8> Bytes;
	if (bJpeg)
	{
		for (FColor& Pixel : Pixels)
		{
			Pixel.A = 255;
		}
		int32 Quality = 0;
		for (const int32 Try : { 90, 80, 70 })
		{
			Quality = Try;
			if (!EncodeJpeg(Pixels, Width, Height, Try, Bytes))
			{
				return false;
			}
			if (Bytes.Num() <= Cap)
			{
				break;
			}
		}
		if (Quality < 90 && Bytes.Num() <= Cap)
		{
			OutIssues.Add(WarningIssue(FString::Printf(TEXT("%s was re-encoded at JPEG quality %d to fit the %lld MB upload limit."), *What, Quality, Cap >> 20)));
		}
	}
	else
	{
		Bytes = FMantlePng::EncodeRGBA8(Width, Height, Pixels);
	}
	// Still over: halve until it fits (never below 256).
	bool bHalved = false;
	while (Bytes.Num() > Cap && FMath::Max(Width, Height) > 256)
	{
		Resize(Pixels, Width, Height, FMath::Max(1, Width / 2), FMath::Max(1, Height / 2), bColour);
		bHalved = true;
		if (bJpeg)
		{
			if (!EncodeJpeg(Pixels, Width, Height, 90, Bytes))
			{
				return false;
			}
		}
		else
		{
			Bytes = FMantlePng::EncodeRGBA8(Width, Height, Pixels);
		}
	}
	if (bHalved)
	{
		OutIssues.Add(WarningIssue(FString::Printf(TEXT("%s was downscaled to %s to fit the %lld MB upload limit."), *What, *SizeText(Width, Height), Cap >> 20)));
	}
	if (Bytes.Num() == 0 || Bytes.Num() > Cap)
	{
		return false;
	}

	Out.Role = RoleOf(BaseSlot);
	Out.Format = bJpeg ? TEXT("jpeg") : TEXT("png");
	Out.Width = Width;
	Out.Height = Height;
	Out.Name = What;
	Out.Hash = FMantleSha256::AssetHash(Bytes);
	Out.Key = FString::Printf(TEXT("texture:%s:%s"), *Out.Role, *HexOfHash(Out.Hash));
	Out.Bytes = MoveTemp(Bytes);
	return true;
}

// ---------------------------------------------------------------------------------------------
// The local conversion cache: <CacheDirectory>/meshes, /materials and /textures (content-addressed).

bool FMantlePlaceAssetConverter::LoadCachedMesh(const FString& CacheKey, FMantleConvertedMesh& Out, TArray<FMantleValidationIssue>& OutIssues) const
{
	using namespace MantlePlaceAssetConverterPrivate;
	const FString Base = FPaths::Combine(CacheDirectory, TEXT("meshes"), CacheKey);
	const TSharedPtr<FJsonObject> Meta = ReadJson(Base + TEXT(".json"));
	TArray<uint8> Bytes;
	if (!Meta.IsValid() || !FFileHelper::LoadFileToArray(Bytes, *(Base + TEXT(".glb")), FILEREAD_Silent))
	{
		return false;
	}
	FString Hash;
	if (!Meta->TryGetStringField(TEXT("hash"), Hash) || FMantleSha256::AssetHash(Bytes) != Hash)
	{
		return false;
	}
	Out = FMantleConvertedMesh();
	Out.Bytes = MoveTemp(Bytes);
	Out.Hash = Hash;
	Meta->TryGetStringField(TEXT("name"), Out.Name);
	Meta->TryGetStringField(TEXT("path"), Out.Path);
	Meta->TryGetStringArrayField(TEXT("sections"), Out.SectionNames);
	Out.Triangles = static_cast<int32>(NumberField(Meta, TEXT("triangles"), 0));
	Out.Vertices = static_cast<int32>(NumberField(Meta, TEXT("vertices"), 0));
	Out.LODs = static_cast<int32>(NumberField(Meta, TEXT("lods"), 1));
	Out.CollisionShapes = static_cast<int32>(NumberField(Meta, TEXT("collisionShapes"), 0));
	ReadCachedIssues(Meta, OutIssues);
	const TArray<TSharedPtr<FJsonValue>>* Slots = nullptr;
	if (!Meta->TryGetArrayField(TEXT("slots"), Slots))
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Slot : *Slots)
	{
		double Value = 0.0;
		if (!Slot.IsValid() || !Slot->TryGetNumber(Value))
		{
			return false;
		}
		Out.Slots.Add(static_cast<int32>(Value));
	}
	return Out.Slots.Num() > 0 && Out.Slots.Num() == Out.SectionNames.Num();
}

void FMantlePlaceAssetConverter::StoreCachedMesh(const FString& CacheKey, const FMantleConvertedMesh& Mesh, const TArray<FMantleValidationIssue>& Issues) const
{
	using namespace MantlePlaceAssetConverterPrivate;
	const FString Base = FPaths::Combine(CacheDirectory, TEXT("meshes"), CacheKey);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Base), /*Tree*/ true);
	const TSharedRef<FJsonObject> Meta = MakeShared<FJsonObject>();
	Meta->SetStringField(TEXT("name"), Mesh.Name);
	Meta->SetStringField(TEXT("path"), Mesh.Path);
	Meta->SetStringField(TEXT("hash"), Mesh.Hash);
	TArray<TSharedPtr<FJsonValue>> Slots;
	for (const int32 Slot : Mesh.Slots)
	{
		Slots.Add(MakeShared<FJsonValueNumber>(Slot));
	}
	Meta->SetArrayField(TEXT("slots"), Slots);
	TArray<TSharedPtr<FJsonValue>> Sections;
	for (const FString& Section : Mesh.SectionNames)
	{
		Sections.Add(MakeShared<FJsonValueString>(Section));
	}
	Meta->SetArrayField(TEXT("sections"), Sections);
	Meta->SetNumberField(TEXT("triangles"), Mesh.Triangles);
	Meta->SetNumberField(TEXT("vertices"), Mesh.Vertices);
	Meta->SetNumberField(TEXT("lods"), Mesh.LODs);
	Meta->SetNumberField(TEXT("collisionShapes"), Mesh.CollisionShapes);
	TArray<TSharedPtr<FJsonValue>> IssueValues;
	for (const FMantleValidationIssue& Issue : Issues)
	{
		IssueValues.Add(MakeShared<FJsonValueObject>(IssueJson(Issue)));
	}
	Meta->SetArrayField(TEXT("issues"), IssueValues);
	// The GLB first: the metadata is what makes an entry count, so it goes last.
	if (SaveAtomically(Mesh.Bytes, Base + TEXT(".glb")))
	{
		SaveStringAtomically(JsonText(Meta), Base + TEXT(".json"));
	}
}

bool FMantlePlaceAssetConverter::LoadCachedMaterial(const FString& CacheKey, FMantleConvertedMaterial& Out, TArray<FMantleConvertedTexture>& OutTextures,
	TArray<FMantleValidationIssue>& OutIssues) const
{
	using namespace MantlePlaceAssetConverterPrivate;
	const TSharedPtr<FJsonObject> Meta = ReadJson(FPaths::Combine(CacheDirectory, TEXT("materials"), CacheKey + TEXT(".json")));
	FString Template;
	if (!Meta.IsValid() || !Meta->TryGetStringField(TEXT("document"), Template) || !FMantlePlaceMaterialDocument::FromTemplateJson(Template, Out.Document))
	{
		return false;
	}
	Meta->TryGetStringField(TEXT("name"), Out.Name);
	Meta->TryGetStringField(TEXT("path"), Out.Path);

	const TArray<TSharedPtr<FJsonValue>>* Textures = nullptr;
	if (Meta->TryGetArrayField(TEXT("textures"), Textures))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Textures)
		{
			const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
			FMantleConvertedTexture& Texture = OutTextures.AddDefaulted_GetRef();
			if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("key"), Texture.Key) || !Entry->TryGetStringField(TEXT("hash"), Texture.Hash)
				|| !Entry->TryGetStringField(TEXT("format"), Texture.Format))
			{
				return false;
			}
			Entry->TryGetStringField(TEXT("name"), Texture.Name);
			Entry->TryGetStringField(TEXT("role"), Texture.Role);
			Texture.Width = static_cast<int32>(NumberField(Entry, TEXT("width"), 0));
			Texture.Height = static_cast<int32>(NumberField(Entry, TEXT("height"), 0));
			const FString File = FPaths::Combine(CacheDirectory, TEXT("textures"),
				HexOfHash(Texture.Hash) + (Texture.Format == TEXT("jpeg") ? TEXT(".jpg") : TEXT(".png")));
			if (!FFileHelper::LoadFileToArray(Texture.Bytes, *File, FILEREAD_Silent) || FMantleSha256::AssetHash(Texture.Bytes) != Texture.Hash)
			{
				return false;
			}
		}
	}
	// Every slot must name a texture the entry brought back.
	for (const TPair<FString, FString>& Slot : Out.Document.Textures)
	{
		if (!OutTextures.ContainsByPredicate([&Slot](const FMantleConvertedTexture& Texture) { return Texture.Key == Slot.Value; }))
		{
			return false;
		}
	}

	ReadCachedIssues(Meta, OutIssues);
	return true;
}

void FMantlePlaceAssetConverter::StoreCachedMaterial(const FString& CacheKey, const FMantleConvertedMaterial& Material, const FMantlePlaceAssetSet& Set,
	const TArray<FMantleValidationIssue>& Issues) const
{
	using namespace MantlePlaceAssetConverterPrivate;
	const FString TextureDirectory = FPaths::Combine(CacheDirectory, TEXT("textures"));
	const FString MaterialDirectory = FPaths::Combine(CacheDirectory, TEXT("materials"));
	IFileManager::Get().MakeDirectory(*TextureDirectory, /*Tree*/ true);
	IFileManager::Get().MakeDirectory(*MaterialDirectory, /*Tree*/ true);

	TArray<TSharedPtr<FJsonValue>> Textures;
	TSet<FString> Written;
	for (const TPair<FString, FString>& Slot : Material.Document.Textures)
	{
		const FMantleConvertedTexture* Texture = Set.FindTexture(Slot.Value);
		if (!Texture)
		{
			return;
		}
		if (Written.Contains(Texture->Key))
		{
			continue;
		}
		Written.Add(Texture->Key);
		const FString File = FPaths::Combine(TextureDirectory, HexOfHash(Texture->Hash) + (Texture->Format == TEXT("jpeg") ? TEXT(".jpg") : TEXT(".png")));
		if (IFileManager::Get().FileSize(*File) <= 0)
		{
			// #804: a texture the publish has already handed on (taken, or spilled to disk) has no bytes here, and a cached
			// texture of 0 bytes would be read back as the texture. The material is simply not cached this time.
			if (Texture->Bytes.IsEmpty() || !SaveAtomically(Texture->Bytes, File))
			{
				return;
			}
		}
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("key"), Texture->Key);
		Entry->SetStringField(TEXT("name"), Texture->Name);
		Entry->SetStringField(TEXT("role"), Texture->Role);
		Entry->SetStringField(TEXT("format"), Texture->Format);
		Entry->SetNumberField(TEXT("width"), Texture->Width);
		Entry->SetNumberField(TEXT("height"), Texture->Height);
		Entry->SetStringField(TEXT("hash"), Texture->Hash);
		Textures.Add(MakeShared<FJsonValueObject>(Entry));
	}

	const TSharedRef<FJsonObject> Meta = MakeShared<FJsonObject>();
	Meta->SetStringField(TEXT("name"), Material.Name);
	Meta->SetStringField(TEXT("path"), Material.Path);
	Meta->SetStringField(TEXT("document"), Material.Document.ToTemplateJson());
	Meta->SetArrayField(TEXT("textures"), Textures);
	TArray<TSharedPtr<FJsonValue>> IssueValues;
	for (const FMantleValidationIssue& Issue : Issues)
	{
		IssueValues.Add(MakeShared<FJsonValueObject>(IssueJson(Issue)));
	}
	Meta->SetArrayField(TEXT("issues"), IssueValues);
	SaveStringAtomically(JsonText(Meta), FPaths::Combine(MaterialDirectory, CacheKey + TEXT(".json")));
}
