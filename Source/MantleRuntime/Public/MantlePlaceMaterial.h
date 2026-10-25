// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleRuntimeTextureBuilder.h"
#include "UObject/SoftObjectPath.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class UObject;
class UTexture;

/** A material document's alphaMode (schema/material.schema.json): glTF's, lower-cased. */
enum class EMantlePlaceAlphaMode : uint8
{
	/** "opaque": alpha ignored (Unreal Opaque). */
	Opaque,

	/** "mask": a pixel is kept where alpha >= alphaCutoff (Unreal Masked). */
	Mask,

	/** "blend": Unreal Translucent, without refraction. */
	Blend,

	/**
	 * "additive" (#628): Unreal Additive, unlit. Emissive-driven: baseColor, metallic, roughness, orm and normal have
	 * no part in it. It builds on M_MantlePlace_Extended's Additive permutations (EMantlePlaceExtendedVariant::Additive);
	 * among the six alpha-mode permutations it stands in as Blend (the fallback when the Additive ones are missing).
	 */
	Additive,
};

/**
 * The four texture slots of a material document, in the schema's order, then a layer blend's three
 * (#533: layerBlend.textures, sampled as the base slot each mirrors), then water's second normal map
 * (#628: water.normal2.texture, read as a normal map).
 */
enum class EMantlePlaceTextureSlot : uint8
{
	BaseColor,
	Normal,
	Orm,
	Emissive,
	LayerBaseColor,
	LayerNormal,
	LayerOrm,
	WaterNormal2,
};

/** The document's own texture slots (textures, uvTransforms, uvPanning): BaseColor to Emissive. */
constexpr int32 MantlePlaceTextureSlotCount = 4;

/** Every texture a material binds: the four slots, a layer blend's three (#533) and water's second normal (#628). */
constexpr int32 MantlePlaceMaterialTextureCount = 8;

/** #533: wind.weight, how much each vertex sways. */
enum class EMantlePlaceWindWeight : uint8
{
	/** "vertexColorR": the vertex colour's red. */
	VertexColorR,

	/** "uvV": 1 minus the first UV set's V (a card's top sways, its root stays). */
	UvV,

	/** "uniform": every vertex fully. */
	Uniform,
};

/** #533: the document's wind object (SimpleGrassWind on World Position Offset). */
struct FMantlePlaceWind
{
	/** 0..4 each. */
	float Intensity = 0.0f;
	float Speed = 0.0f;
	EMantlePlaceWindWeight Weight = EMantlePlaceWindWeight::Uniform;
};

/** #533: the document's worldAligned object (WorldAlignedTexture and WorldAlignedNormal). */
struct FMantlePlaceWorldAligned
{
	/** Centimetres one repeat covers, 1..100000. */
	float TileSizeCm = 100.0f;

	/** The projections' transition contrast, 1..16. */
	float Sharpness = 1.0f;
};

/** #533: the vertex-colour channel a layer blend paints with. */
enum class EMantlePlaceLayerChannel : uint8
{
	R,
	G,
	B,
	A,
};

/** #533: the document's layerBlend object; its textures are the desc's LayerBaseColor..LayerOrm slots. */
struct FMantlePlaceLayerBlend
{
	EMantlePlaceLayerChannel Channel = EMantlePlaceLayerChannel::R;

	/** Linear RGB multiplied into the layer's base colour (A unused). */
	FLinearColor BaseColorFactor = FLinearColor::White;
};

/**
 * #628: the document's foliage object, Unreal's Two Sided Foliage shading. Subsurface Color is
 * base colour x Tint x lerp(1, vertex colour red, VertexColorWeight) x Strength.
 */
struct FMantlePlaceFoliage
{
	/** Linear RGB, 0..1 each. */
	FLinearColor Tint = FLinearColor::White;

	/** 0..4. */
	float Strength = 1.0f;

	/** 0..1: how much the vertex colour's red scales the transmitted light (0 ignores it). */
	float VertexColorWeight = 0.0f;
};

/** #628: where a water normal map is laid out (water.normal's and water.normal2's space). */
enum class EMantlePlaceWaterSpace : uint8
{
	/** "uv": the mesh's first UV set, Tiling repeats across it. */
	Uv,

	/** "world": the world's X and Y, one repeat every WorldSizeCm. */
	World,
};

/** #628: one water normal layer: how its map is laid out and how it moves. Defaults are the schema's. */
struct FMantlePlaceWaterNormal
{
	EMantlePlaceWaterSpace Space = EMantlePlaceWaterSpace::Uv;

	/** Repeats across the mesh's UV set, -1024..1024 each; only with Uv. */
	FVector2f Tiling = FVector2f(1.0f, 1.0f);

	/** Centimetres one repeat covers, 1..100000; only with World. */
	float WorldSizeCm = 100.0f;

	/** Repeats a second, -10..10 each. */
	FVector2f Panning = FVector2f(0.0f, 0.0f);

	bool operator==(const FMantlePlaceWaterNormal& Other) const
	{
		return Space == Other.Space && Tiling == Other.Tiling && WorldSizeCm == Other.WorldSizeCm && Panning == Other.Panning;
	}
};

/**
 * #628: the document's water object, Unreal's Single Layer Water shading on M_MantlePlace_Water. The coefficients
 * are per centimetre; the first normal map is the document's textures.normal, the second (when there is one) is
 * Textures[WaterNormal2].
 */
struct FMantlePlaceWater
{
	/** Linear RGB, 0..1 each (A is 1). */
	FLinearColor Scattering = FLinearColor(0.002f, 0.002f, 0.002f, 1.0f);
	FLinearColor Absorption = FLinearColor(0.02f, 0.007f, 0.005f, 1.0f);

	/** -1..1. */
	float PhaseG = 0.0f;

	/** 0..1. */
	float ColorScaleBehindWater = 1.0f;

	FMantlePlaceWaterNormal Normal;

	/** Whether the document has a second normal map (its texture id is Textures[WaterNormal2]). */
	bool bHasNormal2 = false;
	FMantlePlaceWaterNormal Normal2;
};

/**
 * #533: which family of M_MantlePlace_Extended permutations a document builds on. Plain is #471's six;
 * each other has its own static switch and instances, none translucent.
 */
enum class EMantlePlaceExtendedVariant : uint8
{
	Plain,

	/** Wind: Opaque, Opaque two-sided, Masked, Masked two-sided. */
	Wind,

	/** WorldAligned: Opaque, Opaque two-sided. */
	WorldAligned,

	/** LayerBlend: Opaque, Opaque two-sided. */
	LayerBlend,

	/** #628 Foliage: Masked two-sided only, Two Sided Foliage shading, no wind. */
	Foliage,

	/** #628 FoliageWind: the same with wind (the one combination of two features). */
	FoliageWind,

	/** #628 Water: M_MantlePlace_Water (not an Extended permutation), one normal map; opaque, either sidedness. */
	Water,

	/** #628 WaterNormal2: M_MantlePlace_Water with its SecondNormal switch on. */
	WaterNormal2,

	/** #628 Additive: alphaMode additive; Additive blending and the Unlit shading model, one- or two-sided (no static switch). */
	Additive,
};

/** A material document's shading (schema/material.schema.json, #487): which cooked master it builds on. */
enum class EMantlePlaceShading : uint8
{
	/** "default": M_MantlePlace, one of its six alpha-mode/two-sided permutations. */
	Default,

	/** "carPaint": M_MantlePlace_CarPaint, clear coat over a flaked base; opaque and one-sided only. */
	CarPaint,
};

/**
 * The car-paint finish (the document's carPaint object, #487); only read with EMantlePlaceShading::CarPaint.
 * Defaults are the schema's. AMantleVehicle fills one from its definition for the built-in cars.
 */
struct FMantlePlaceCarPaint
{
	/** Share of the flake tile's flakes that show, 0..1. */
	float FlakeDensity = 0.5f;

	/** One flake's size in cm on the pre-skinned local position, 0.01..1. */
	float FlakeSize = 0.06f;

	/** Linear RGB the flakes shift toward (A is 1). */
	FLinearColor FlakeTint = FLinearColor::White;

	/** Clear-coat strength and roughness, 0..1. */
	float ClearCoat = 1.0f;
	float ClearCoatRoughness = 0.05f;

	bool operator==(const FMantlePlaceCarPaint& Other) const
	{
		return FlakeDensity == Other.FlakeDensity && FlakeSize == Other.FlakeSize && FlakeTint == Other.FlakeTint
			&& ClearCoat == Other.ClearCoat && ClearCoatRoughness == Other.ClearCoatRoughness;
	}
};

/** A material document's vertexColor (schema/material.schema.json, #471). */
enum class EMantlePlaceVertexColor : uint8
{
	/** "none": the mesh's vertex colour is not used. */
	None,

	/** "multiplyBaseColor": its RGB multiplies the base colour. */
	MultiplyBaseColor,
};

/**
 * One texture slot's own UV transform (the document's uvTransforms entry, #471; glTF
 * KHR_texture_transform): uv' = Offset + R(Rotation) x (Scale x uv) in glTF's texture space (V down),
 * so a positive rotation turns the UVs counter-clockwise as the texture is seen. Defaults are the
 * schema's (the identity).
 */
struct FMantlePlaceUVTransform
{
	FVector2f Offset = FVector2f(0.0f, 0.0f);
	FVector2f Scale = FVector2f(1.0f, 1.0f);

	/** Radians. */
	float Rotation = 0.0f;

	/**
	 * The transform as the two rows M_MantlePlace_Extended applies (its <Slot>UVTransformU/V parameters):
	 * u' = dot(OutU.RG, uv) + OutU.B and v' = dot(OutV.RG, uv) + OutV.B; A is 0. The rotation is
	 * evaluated here, so the shader only multiplies and adds.
	 */
	MANTLERUNTIME_API void ToRows(FLinearColor& OutU, FLinearColor& OutV) const;

	bool operator==(const FMantlePlaceUVTransform& Other) const
	{
		return Offset == Other.Offset && Scale == Other.Scale && Rotation == Other.Rotation;
	}
};

/**
 * An uploaded material (kind "material"): the glTF metallic-roughness subset the editor plugin
 * flattens any Unreal material into, validated against schema/material.schema.json. Values and
 * texture asset ids only, never a shader. Defaults are the schema's.
 */
struct FMantlePlaceMaterialDesc
{
	EMantlePlaceAlphaMode AlphaMode = EMantlePlaceAlphaMode::Opaque;

	/** Only meaningful for Mask (the schema refuses it otherwise). */
	float AlphaCutoff = 0.5f;

	bool bDoubleSided = false;

	/** Linear RGBA multiplied into the base colour texture. */
	FLinearColor BaseColorFactor = FLinearColor::White;

	float MetallicFactor = 1.0f;
	float RoughnessFactor = 1.0f;

	/** Linear RGB (A is 1). */
	FLinearColor EmissiveFactor = FLinearColor(0.0f, 0.0f, 0.0f, 1.0f);

	float EmissiveStrength = 1.0f;

	FVector2f UVScale = FVector2f(1.0f, 1.0f);
	FVector2f UVOffset = FVector2f(0.0f, 0.0f);

	/**
	 * Texture asset id per slot (EMantlePlaceTextureSlot order, a layer blend's included); empty when the slot uses
	 * the master material's default.
	 */
	FString Textures[MantlePlaceMaterialTextureCount];

	/** Which master the material builds on; CarPaint implies Opaque and one-sided (the parser refuses anything else). */
	EMantlePlaceShading Shading = EMantlePlaceShading::Default;

	/** Only meaningful for CarPaint (the schema refuses the object otherwise). */
	FMantlePlaceCarPaint CarPaint;

	// #471's fidelity keys. Any of them away from its default builds the material on M_MantlePlace_Extended
	// (UsesExtended); the parser refuses them with CarPaint, whose master has none of them.

	/** Normal map strength, 0..10 (glTF normalTexture.scale). */
	float NormalScale = 1.0f;

	/** Share of the ORM texture's occlusion that applies, 0..1 (glTF occlusionTexture.strength). */
	float OcclusionStrength = 1.0f;

	/** The UV set occlusion is sampled with: 0 or 1 (glTF occlusionTexture.texCoord). */
	int32 OcclusionUV = 0;

	EMantlePlaceVertexColor VertexColor = EMantlePlaceVertexColor::None;

	/** Per slot (EMantlePlaceTextureSlot order): whether the document gives it its own transform, and that transform. */
	bool bHasUVTransform[MantlePlaceTextureSlotCount] = {};
	FMantlePlaceUVTransform UVTransforms[MantlePlaceTextureSlotCount];

	// #533's keys, on M_MantlePlace_Extended too (UsesExtended); the parser refuses them with CarPaint.

	/** Per slot (EMantlePlaceTextureSlot order): UV units a second, each component -10..10; zero when not listed. */
	FVector2f UVPanning[MantlePlaceTextureSlotCount] = { FVector2f(0.0f, 0.0f), FVector2f(0.0f, 0.0f), FVector2f(0.0f, 0.0f), FVector2f(0.0f, 0.0f) };

	/** At most one of the three below (ExtendedVariant; wind also goes with foliage); the parser refuses more. */
	bool bHasWind = false;
	FMantlePlaceWind Wind;
	bool bHasWorldAligned = false;
	FMantlePlaceWorldAligned WorldAligned;
	bool bHasLayerBlend = false;
	FMantlePlaceLayerBlend LayerBlend;

	/** #628: foliage shading; the only key that goes with wind (the parser refuses worldAligned and layerBlend with it). */
	bool bHasFoliage = false;
	FMantlePlaceFoliage Foliage;

	/** #628: water shading, on its own master; exclusive with foliage, wind, worldAligned and layerBlend, and opaque only. */
	bool bHasWater = false;
	FMantlePlaceWater Water;

	const FString& TextureFor(EMantlePlaceTextureSlot Slot) const { return Textures[static_cast<int32>(Slot)]; }

	/** True when any slot pans (a non-zero uvPanning entry). */
	MANTLERUNTIME_API bool HasPanning() const;

	/** #533: the permutation family: Wind, WorldAligned or LayerBlend (or #628's Foliage, FoliageWind, Water, WaterNormal2) when the document has that key, Additive for alphaMode additive, else Plain. */
	MANTLERUNTIME_API EMantlePlaceExtendedVariant ExtendedVariant() const;

	/** The transform a slot samples with: its own when the document gives one, else UVScale and UVOffset. */
	MANTLERUNTIME_API FMantlePlaceUVTransform UVTransformFor(EMantlePlaceTextureSlot Slot) const;

	/**
	 * True when the material needs M_MantlePlace_Extended: a slot with its own transform, a normal scale or
	 * occlusion strength other than 1, occlusion on UV 1, or a vertex colour mode (#471); a panning slot, wind,
	 * world-aligned mapping or a layer blend (#533). A key present at its default renders the same on
	 * M_MantlePlace, so it stays there. Never for CarPaint.
	 */
	MANTLERUNTIME_API bool UsesExtended() const;
};

/**
 * The parameter names M_MantlePlace exposes. The C++ side is written against these; the master
 * material and its six permutation instances are content (tools/materials/build_m_mantleplace.py
 * builds them headless, look-dev is a GUI handoff). Kept as TCHAR literals, not FName globals, so
 * nothing constructs an FName during static initialisation.
 */
namespace MantlePlaceMaterialParameters
{
	/** Texture parameters. Sampler types: Color (sRGB), Normal, Masks (linear), Color (sRGB). */
	constexpr const TCHAR* BaseColor = TEXT("BaseColor");
	constexpr const TCHAR* Normal = TEXT("Normal");
	constexpr const TCHAR* ORM = TEXT("ORM");
	constexpr const TCHAR* Emissive = TEXT("Emissive");

	/** Vector parameters: linear RGBA; linear RGB (A unused); UV scale in R, G; UV offset in R, G. */
	constexpr const TCHAR* BaseColorFactor = TEXT("BaseColorFactor");
	constexpr const TCHAR* EmissiveFactor = TEXT("EmissiveFactor");
	constexpr const TCHAR* UVScale = TEXT("UVScale");
	constexpr const TCHAR* UVOffset = TEXT("UVOffset");

	/** Scalar parameters. */
	constexpr const TCHAR* MetallicFactor = TEXT("MetallicFactor");
	constexpr const TCHAR* RoughnessFactor = TEXT("RoughnessFactor");
	constexpr const TCHAR* EmissiveStrength = TEXT("EmissiveStrength");
	constexpr const TCHAR* AlphaCutoff = TEXT("AlphaCutoff");

	/**
	 * M_MantlePlace_CarPaint only (tools/materials/Create-CarPaintMaterial.py, #487). It also has every
	 * parameter above except AlphaCutoff. PaintTint is UMantleVehicleDefinition::PaintParameter's
	 * default, so AMantleVehicle recolours a car-paint body the same way as an authored one.
	 */
	constexpr const TCHAR* PaintTint = TEXT("Paint Tint");
	constexpr const TCHAR* FlakeDensity = TEXT("FlakeDensity");
	constexpr const TCHAR* FlakeSize = TEXT("FlakeSize");
	constexpr const TCHAR* FlakeTint = TEXT("FlakeTint");
	constexpr const TCHAR* ClearCoat = TEXT("ClearCoat");
	constexpr const TCHAR* ClearCoatRoughness = TEXT("ClearCoatRoughness");

	/** The livery layer (#488): sRGB texture with alpha as coverage, its opacity, and 0 = UV0 / 1 = UV1. */
	constexpr const TCHAR* Livery = TEXT("Livery");
	constexpr const TCHAR* LiveryOpacity = TEXT("LiveryOpacity");
	constexpr const TCHAR* LiveryUV1 = TEXT("LiveryUV1");

	/**
	 * M_MantlePlace_Extended only (tools/materials/Create-MantlePlaceExtendedMaterial.py, #471). It also has
	 * every M_MantlePlace parameter except UVScale and UVOffset, which the per-slot rows below replace
	 * (BuildOnParent derives a slot's rows from them when the document gives it no transform of its own).
	 * Scalars: normal strength; occlusion strength; 0 or 1, the vertex colour's RGB multiplying the base
	 * colour; 0 or 1, the UV set occlusion samples.
	 */
	constexpr const TCHAR* NormalScale = TEXT("NormalScale");
	constexpr const TCHAR* OcclusionStrength = TEXT("OcclusionStrength");
	constexpr const TCHAR* VertexColorBaseColor = TEXT("VertexColorBaseColor");
	constexpr const TCHAR* OcclusionUV = TEXT("OcclusionUV");

	/** Vector parameters, one pair per texture slot: FMantlePlaceUVTransform::ToRows's U and V rows. */
	constexpr const TCHAR* BaseColorUVTransformU = TEXT("BaseColorUVTransformU");
	constexpr const TCHAR* BaseColorUVTransformV = TEXT("BaseColorUVTransformV");
	constexpr const TCHAR* NormalUVTransformU = TEXT("NormalUVTransformU");
	constexpr const TCHAR* NormalUVTransformV = TEXT("NormalUVTransformV");
	constexpr const TCHAR* ORMUVTransformU = TEXT("ORMUVTransformU");
	constexpr const TCHAR* ORMUVTransformV = TEXT("ORMUVTransformV");
	constexpr const TCHAR* EmissiveUVTransformU = TEXT("EmissiveUVTransformU");
	constexpr const TCHAR* EmissiveUVTransformV = TEXT("EmissiveUVTransformV");

	/**
	 * M_MantlePlace_Extended, #533 (tools/materials/Extend-MantlePlaceExtendedMaterial.py). Vector parameters,
	 * one per texture slot: its uvPanning speed in R, G (UV units a second).
	 */
	constexpr const TCHAR* BaseColorUVPanning = TEXT("BaseColorUVPanning");
	constexpr const TCHAR* NormalUVPanning = TEXT("NormalUVPanning");
	constexpr const TCHAR* ORMUVPanning = TEXT("ORMUVPanning");
	constexpr const TCHAR* EmissiveUVPanning = TEXT("EmissiveUVPanning");

	/**
	 * M_MantlePlace_Water only (#628). It also has BaseColorFactor, RoughnessFactor,
	 * NormalScale and the Normal texture (and NormalUVPanning, which M_MantlePlace_Extended has too). Vectors:
	 * scattering and absorption coefficients (linear RGB, 1/cm). Scalars: the phase function's g, the scale of the
	 * scene behind the water. Per normal layer (Normal, Normal2): UVTiling (R, G), WorldSpace (0 mesh UVs, 1 world X and Y),
	 * WorldScale (repeats per centimetre, 1 / worldSizeCm) and UVPanning (R, G, repeats a second). Normal2 is the second
	 * normal map texture, used by the SecondNormal permutation.
	 */
	constexpr const TCHAR* WaterScattering = TEXT("WaterScattering");
	constexpr const TCHAR* WaterAbsorption = TEXT("WaterAbsorption");
	constexpr const TCHAR* WaterPhaseG = TEXT("WaterPhaseG");
	constexpr const TCHAR* WaterColorScale = TEXT("WaterColorScale");
	constexpr const TCHAR* Normal2 = TEXT("Normal2");
	constexpr const TCHAR* NormalUVTiling = TEXT("NormalUVTiling");
	constexpr const TCHAR* NormalWorldSpace = TEXT("NormalWorldSpace");
	constexpr const TCHAR* NormalWorldScale = TEXT("NormalWorldScale");
	constexpr const TCHAR* Normal2UVTiling = TEXT("Normal2UVTiling");
	constexpr const TCHAR* Normal2WorldSpace = TEXT("Normal2WorldSpace");
	constexpr const TCHAR* Normal2WorldScale = TEXT("Normal2WorldScale");
	constexpr const TCHAR* Normal2UVPanning = TEXT("Normal2UVPanning");

	/**
	 * Static switches, each true only on its own permutation instances (a dynamic instance cannot change a
	 * static switch, so the permutation is the switch).
	 */
	constexpr const TCHAR* WindSwitch = TEXT("Wind");
	constexpr const TCHAR* WorldAlignedSwitch = TEXT("WorldAligned");
	constexpr const TCHAR* LayerBlendSwitch = TEXT("LayerBlend");

	/**
	 * Foliage (#628): SubsurfaceTint is a linear RGB vector; SubsurfaceStrength and SubsurfaceVertexColorWeight are
	 * scalars. The foliage permutations are instances with the Two Sided Foliage shading model, so they have no switch.
	 */
	constexpr const TCHAR* SubsurfaceTint = TEXT("SubsurfaceTint");
	constexpr const TCHAR* SubsurfaceStrength = TEXT("SubsurfaceStrength");
	constexpr const TCHAR* SubsurfaceVertexColorWeight = TEXT("SubsurfaceVertexColorWeight");

	/** Wind scalars: SimpleGrassWind's intensity and speed; 0 or 1, the weight from the vertex colour's red; 0 or 1, from 1 - V. */
	constexpr const TCHAR* WindIntensity = TEXT("WindIntensity");
	constexpr const TCHAR* WindSpeed = TEXT("WindSpeed");
	constexpr const TCHAR* WindWeightVertexColor = TEXT("WindWeightVertexColor");
	constexpr const TCHAR* WindWeightUV = TEXT("WindWeightUV");

	/** World-aligned scalars: centimetres a repeat covers, and the projections' transition contrast. */
	constexpr const TCHAR* WorldAlignedTileSize = TEXT("WorldAlignedTileSize");
	constexpr const TCHAR* WorldAlignedSharpness = TEXT("WorldAlignedSharpness");

	/**
	 * Layer blend: the layer's textures (as BaseColor, Normal, ORM), its base colour factor (linear RGB), and the
	 * vertex-colour channel as a one-hot RGBA mask (the blend's alpha is dot(vertex colour, mask)).
	 */
	constexpr const TCHAR* LayerBaseColor = TEXT("LayerBaseColor");
	constexpr const TCHAR* LayerNormal = TEXT("LayerNormal");
	constexpr const TCHAR* LayerORM = TEXT("LayerORM");
	constexpr const TCHAR* LayerBaseColorFactor = TEXT("LayerBaseColorFactor");
	constexpr const TCHAR* LayerChannel = TEXT("LayerChannel");
}

/**
 * Uploaded place materials at runtime (Fab F8, #342): the material document reader, the choice of
 * M_MantlePlace permutation, the dynamic instance, and the texture fix-ups the material slots need.
 *
 * A shipped client cannot compile shaders, so every uploaded material is a UMaterialInstanceDynamic
 * of one of six constant instances of M_MantlePlace cooked into the client: Opaque, Masked or
 * Translucent, each one- or two-sided (the blend mode and two-sidedness are base property overrides
 * on the constant instance, which is a static permutation and cannot change on a dynamic one).
 * When the permutation asset is missing (the content handoff not done yet) the engine's default
 * surface material stands in, with one warning per process.
 */
class MANTLERUNTIME_API FMantlePlaceMaterials
{
public:
	/** Where the master material and its permutations live. */
	static constexpr const TCHAR* MasterMaterialPath = TEXT("/Game/Mantle/Materials/M_MantlePlace.M_MantlePlace");

	/** Largest material document accepted (the worker's MaxMaterialBytes). */
	static constexpr int32 MaxDocumentBytes = 16 * 1024;

	/** Where the extended master lives (#471): M_MantlePlace plus per-slot UV transforms, normal and occlusion strength, occlusion UV, vertex colour. */
	static constexpr const TCHAR* ExtendedMasterMaterialPath = TEXT("/Game/Mantle/Materials/M_MantlePlace_Extended.M_MantlePlace_Extended");

	/**
	 * The six M_MantlePlace permutations (0..5), the car-paint one (6), the six M_MantlePlace_Extended ones (7..12),
	 * and #533's: Wind's four (13..16: Opaque, Opaque two-sided, Masked, Masked two-sided), WorldAligned's two
	 * (17, 18: Opaque, Opaque two-sided) and LayerBlend's two (19, 20). None of #533's is translucent, and the three
	 * are exclusive, so each switch combination cooked is one a document can pick (not their full matrix).
	 * #628's two: Foliage (21) and FoliageWind (22), both Masked and two-sided, shaded as Two Sided Foliage.
	 * #628's water: Water (23) and WaterNormal2 (24), instances of M_MantlePlace_Water (Single Layer Water; opaque, and one
	 * instance for either sidedness).
	 * #628's additive: Additive (25) and Additive two-sided (26), instances of M_MantlePlace_Extended with the Additive blend
	 * mode and the Unlit shading model.
	 */
	static constexpr int32 PermutationCount = 27;
	static constexpr int32 CarPaintPermutationIndex = 6;
	static constexpr int32 ExtendedPermutationBase = 7;
	static constexpr int32 WindPermutationBase = 13;
	static constexpr int32 WorldAlignedPermutationBase = 17;
	static constexpr int32 LayerBlendPermutationBase = 19;
	static constexpr int32 FoliagePermutationIndex = 21;
	static constexpr int32 FoliageWindPermutationIndex = 22;
	static constexpr int32 WaterPermutationIndex = 23;
	static constexpr int32 WaterNormal2PermutationIndex = 24;
	static constexpr int32 AdditivePermutationBase = 25;

	/** #533: World Position Offset stops past this many centimetres on a component drawing a Wind material. */
	static constexpr int32 WindDisableDistance = 5000;

	/** The car-paint permutation's asset name: MI_MantlePlace_CarPaint, an instance of M_MantlePlace_CarPaint. */
	static constexpr const TCHAR* CarPaintPermutationName = TEXT("MI_MantlePlace_CarPaint");

	/**
	 * Reads a material document strictly: UTF-8 JSON, at most MaxDocumentBytes, one object with only
	 * the schema's properties, schemaVersion 1, the enums, ranges and array lengths the schema gives,
	 * alphaCutoff only with "mask", texture ids of the uploaded-id form. False, with the reason, on
	 * anything else. Any thread.
	 */
	static bool ParseMaterial(TConstArrayView<uint8> Bytes, FMantlePlaceMaterialDesc& Out, FString* OutError = nullptr);

	/** "opaque", "mask", "blend", "additive". */
	static const TCHAR* AlphaModeName(EMantlePlaceAlphaMode Mode);

	/**
	 * "baseColor", "normal", "orm", "emissive" (the schema's keys); a layer blend's "layerBaseColor", "layerNormal",
	 * "layerOrm" (the backend's cook_info keys for them, #533).
	 */
	static const TCHAR* SlotName(EMantlePlaceTextureSlot Slot);

	/** The base slot a layer slot mirrors (LayerNormal is Normal); a base slot itself. */
	static EMantlePlaceTextureSlot BaseSlotOf(EMantlePlaceTextureSlot Slot);

	/**
	 * How a slot's texture is read: base colour and emissive sRGB colour, the normal map Normal, ORM linear data;
	 * a layer slot as its base slot.
	 */
	static EMantleTextureRole RoleForSlot(EMantlePlaceTextureSlot Slot);

	/** 0..5: Opaque, Opaque two-sided, Masked, Masked two-sided, Translucent, Translucent two-sided (Additive takes Translucent's, its fallback). */
	static int32 PermutationIndex(EMantlePlaceAlphaMode Mode, bool bTwoSided);

	/** The permutation's asset name, "MI_MantlePlace_Masked_TwoSided" and so on. */
	static FString PermutationName(EMantlePlaceAlphaMode Mode, bool bTwoSided);

	/** "/Game/Mantle/Materials/<name>.<name>". */
	static FSoftObjectPath PermutationPath(EMantlePlaceAlphaMode Mode, bool bTwoSided);

	/**
	 * The permutation a document builds on: the car-paint one (index 6); else, for a document using #471's or #533's
	 * keys (FMantlePlaceMaterialDesc::UsesExtended), M_MantlePlace_Extended's for its variant (13..20 for wind,
	 * world-aligned mapping or a layer blend), alpha mode and sidedness (7..12 for the plain ones); else
	 * M_MantlePlace's (0..5).
	 */
	static int32 PermutationIndex(const FMantlePlaceMaterialDesc& Desc);
	static FString PermutationName(const FMantlePlaceMaterialDesc& Desc);
	static FSoftObjectPath PermutationPath(const FMantlePlaceMaterialDesc& Desc);

	/** "/Game/Mantle/Materials/MI_MantlePlace_CarPaint.MI_MantlePlace_CarPaint". */
	static FSoftObjectPath CarPaintPermutationPath();

	/** #628: the additive instances' asset names, "MI_MantlePlace_Extended_Additive" and the two-sided one with "_TwoSided" after it. */
	static constexpr const TCHAR* AdditivePermutationName = TEXT("MI_MantlePlace_Extended_Additive");

	/** #628: the water instances' asset names, instances of M_MantlePlace_Water. */
	static constexpr const TCHAR* WaterPermutationName = TEXT("MI_MantlePlace_Water");
	static constexpr const TCHAR* WaterNormal2PermutationName = TEXT("MI_MantlePlace_Water_Normal2");

	/** An M_MantlePlace_Extended permutation's asset name, "MI_MantlePlace_Extended_Masked_TwoSided" and so on. */
	static FString ExtendedPermutationName(EMantlePlaceAlphaMode Mode, bool bTwoSided);

	/** "/Game/Mantle/Materials/<name>.<name>". */
	static FSoftObjectPath ExtendedPermutationPath(EMantlePlaceAlphaMode Mode, bool bTwoSided);

	/** #533: "Wind", "WorldAligned", "LayerBlend" (#628: "Foliage", "FoliageWind", "Water", "WaterNormal2", "Additive"); "" for Plain. The static switch and the instance names use it. */
	static const TCHAR* ExtendedVariantName(EMantlePlaceExtendedVariant Variant);

	/**
	 * #533: the permutation index of Variant for Mode and sidedness (Plain: 7..12), or INDEX_NONE when that variant
	 * was not cooked for Mode (wind on blend or additive; world-aligned or a layer blend on anything but opaque; water on
	 * anything but opaque, #628; Additive on anything but additive).
	 */
	static int32 ExtendedPermutationIndex(EMantlePlaceExtendedVariant Variant, EMantlePlaceAlphaMode Mode, bool bTwoSided);

	/** #533: "MI_MantlePlace_Extended_Wind_Masked_TwoSided" and so on; Plain's are ExtendedPermutationName's. */
	static FString ExtendedPermutationName(EMantlePlaceExtendedVariant Variant, EMantlePlaceAlphaMode Mode, bool bTwoSided);

	/** "/Game/Mantle/Materials/<name>.<name>". */
	static FSoftObjectPath ExtendedPermutationPath(EMantlePlaceExtendedVariant Variant, EMantlePlaceAlphaMode Mode, bool bTwoSided);

	/**
	 * #533: whether Material is built on one of the Wind permutations (PermutationIndexOf), so its component should
	 * stop World Position Offset past WindDisableDistance. Game thread.
	 */
	static bool IsWindMaterial(const UMaterialInterface* Material);

	/**
	 * #462: which permutation Material is built on (0..5, CarPaintPermutationIndex, or one of #471's
	 * M_MantlePlace_Extended permutations, ExtendedPermutationBase..+5, or #533's, 13..20), walking its instance
	 * parents: every uploaded material's dynamic instance, and any instance made from one.
	 * INDEX_NONE for anything else (a built-in library material, the engine's default, the fallback
	 * a missing permutation leaves). Game thread.
	 */
	static int32 PermutationIndexOf(const UMaterialInterface* Material);

	/**
	 * The M_MantlePlace_Extended permutation (#471) of Mode and sidedness, or null (no fallback, no
	 * warning) when the content isn't there, like FindCarPaintPermutation. Game thread.
	 */
	static UMaterialInterface* FindExtendedPermutation(EMantlePlaceAlphaMode Mode, bool bTwoSided);

	/** #533: as above for Variant's permutation; null too when Variant was not cooked for Mode. Game thread. */
	static UMaterialInterface* FindExtendedPermutation(EMantlePlaceExtendedVariant Variant, EMantlePlaceAlphaMode Mode, bool bTwoSided);

	/**
	 * The permutation instance a material is built on, loaded once and kept. When it is missing, the
	 * engine's default surface material, with bOutFellBack set and one warning per process
	 * ("MANTLE_PLACE_MATERIAL M_MantlePlace is missing"). Game thread.
	 */
	static UMaterialInterface* LoadPermutation(EMantlePlaceAlphaMode Mode, bool bTwoSided, bool* bOutFellBack = nullptr);

	/**
	 * As above for a document. A car-paint document whose permutation is missing falls back to
	 * MI_MantlePlace_Opaque (same parameter names, so it keeps its colour and textures without the
	 * clear coat), with bOutFellBack set and one warning per process
	 * ("MANTLE_PLACE_MATERIAL M_MantlePlace_CarPaint is missing"). A document using #471's keys whose
	 * M_MantlePlace_Extended permutation is missing falls back to the M_MantlePlace permutation of the same
	 * alpha mode and sidedness (it keeps its factors, textures and uvScale/uvOffset, without the extras),
	 * with bOutFellBack set and one warning per process ("MANTLE_PLACE_MATERIAL M_MantlePlace_Extended is
	 * missing"). A #533 variant whose permutation is missing falls back to the plain M_MantlePlace_Extended
	 * permutation of the same alpha mode and sidedness (panning and #471's keys kept, the variant's effect
	 * dropped), then as above, with bOutFellBack set and one warning per process
	 * ("MANTLE_PLACE_MATERIAL M_MantlePlace_Extended's <variant> permutations are missing"). Game thread.
	 */
	static UMaterialInterface* LoadPermutation(const FMantlePlaceMaterialDesc& Desc, bool* bOutFellBack = nullptr);

	/**
	 * Sets #471's parameters (strengths, occlusion UV, vertex colour, every slot's UV rows) and #533's (every
	 * slot's panning; the wind, world-aligned or layer-blend values the document has) on an instance of
	 * M_MantlePlace_Extended. The static switches are the parent's (the permutation). A water document (#628) also gets
	 * ApplyWater's values, so the instance is right on M_MantlePlace_Water and keeps its first normal's tiling and
	 * panning on the Extended fallback. Game thread.
	 */
	static void ApplyExtended(UMaterialInstanceDynamic* Instance, const FMantlePlaceMaterialDesc& Desc);

	/**
	 * #628: sets M_MantlePlace_Water's values from the document's water object: the coefficients, phase g, the
	 * scale behind the water, and each normal layer's space, tiling (or world scale) and panning. Game thread.
	 */
	static void ApplyWater(UMaterialInstanceDynamic* Instance, const FMantlePlaceWater& Water);

	/**
	 * The car-paint permutation, or null (no fallback, no warning) when the content isn't there, for
	 * callers with a better fallback of their own: AMantleVehicle keeps the car's authored body. Game thread.
	 */
	static UMaterialInterface* FindCarPaintPermutation();

	/** Sets the car-paint parameters (flakes, clear coat) on an instance of M_MantlePlace_CarPaint. Game thread. */
	static void ApplyCarPaint(UMaterialInstanceDynamic* Instance, const FMantlePlaceCarPaint& CarPaint);

	/**
	 * The dynamic instance for a material document: parented to its permutation (or the fallback),
	 * every factor set (and the car-paint ones for a car-paint document, the extended ones for a document
	 * using #471's or #533's keys), and each slot's texture where Textures has one, in EMantlePlaceTextureSlot
	 * order, a layer blend's last (a null or missing entry keeps the master material's default for that slot).
	 * Game thread; also works under -NullRHI.
	 */
	static UMaterialInstanceDynamic* Build(const FMantlePlaceMaterialDesc& Desc, TConstArrayView<UTexture*> Textures,
		UObject* Outer = nullptr, FName Name = NAME_None, FString* OutError = nullptr);

	/** As Build, on an explicit parent (tests, tools). */
	static UMaterialInstanceDynamic* BuildOnParent(UMaterialInterface* Parent, const FMantlePlaceMaterialDesc& Desc,
		TConstArrayView<UTexture*> Textures, UObject* Outer = nullptr, FName Name = NAME_None, FString* OutError = nullptr);

	/**
	 * Makes a decoded texture fit its material slot, between FMantleRuntimeTextureBuilder::Decode and
	 * Build. Any thread.
	 * - Every slot: the texture's role must be the slot's (an MTX1 file carries its own role; the
	 *   worker checks it at material cook, this catches the rest).
	 * - Normal: creators upload glTF's (OpenGL, green up) normal maps and Unreal samples DirectX
	 *   (green down). The cook flips green when it encodes role=normal to BC5 (texturecook.go
	 *   normalizeNormals), so a BC5 normal map is used exactly as downloaded. A normal map
	 *   uploaded with no role stays the uploaded PNG/JPEG, never passed through that flip, and decodes
	 *   to BGRA8: only then is green inverted here, so every normal map reaching M_MantlePlace is in
	 *   Unreal's convention. Other formats are refused.
	 * - ORM: a BC4 texture (the worker's choice for a grey data image) samples as (R, 0, 0), which
	 *   would zero roughness and metallic, so it is expanded to grey BGRA8 (R = G = B).
	 * - A layer blend's slot (#533) is treated as the base slot it mirrors.
	 * False, with the reason, when the texture cannot serve the slot.
	 */
	static bool PrepareForSlot(EMantlePlaceTextureSlot Slot, FMantleDecodedTexture& Texture, FString* OutError = nullptr);

	/** One BC4 block's 16 values, row-major. Exposed for tests. */
	static void DecodeBC4Block(const uint8* Block, uint8 (&OutValues)[16]);

	/** Test hook: forget the cached permutations and the one-time warning. */
	static void ResetForTests();
};
