// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
class UTexture;

/**
 * What a landscape's own material says one paint layer looks like (#628): the base colour and normal
 * textures it blends in for that layer, how big one repeat of them is on the ground, and what the
 * material does to the layer that Mantle's terrain cannot keep. Pure data: the textures are the ones
 * the material (and its instance's overrides) sample, not copies.
 */
struct MANTLEEDITOR_API FMantleLandscapeLayerLook
{
	FName Layer;

	/** The layer's base colour and normal textures, null where the material has none for it. */
	UTexture* BaseColor = nullptr;
	UTexture* Normal = nullptr;

	/** cm per repeat of each texture on the ground: the landscape scale times the layer coordinates' MappingScale, or the Texture Coordinate's tiling. 0 when it could not be read. */
	double BaseColorTilingCm = 0.0;
	double NormalTilingCm = 0.0;

	/** A flat base colour (a constant the layer's input is), used when the layer has no base colour texture. */
	bool bHasFlatColour = false;
	FLinearColor FlatColour = FLinearColor::Black;

	/** The base colour texture's average (or the flat colour), where it could be read: what a folded layer is matched to by. */
	bool bHasAverage = false;
	FLinearColor Average = FLinearColor::Black;

	/**
	 * The layer's roughness (0 to 1) where the material states one for it: an auto chain's Roughness output worked out with the chain's masks
	 * set to show this layer alone (so a roughness multiplied by water's (1 - mask)^4 gives the water layer its glossy value). MakeLayerMaterial
	 * carries it as the layer material's roughness, which the client reads as the layer's RoughnessFactor. Unset, the default 0.85 applies.
	 */
	bool bHasRoughness = false;
	float Roughness = 0.85f;

	/** The material read the layer, but found nothing of its look in it (a procedural look with no texture or constant). */
	bool bUnreadable = false;

	/** What the material does to this layer that is not kept, in plain words ("it also blends 2 other textures (...)"). */
	TArray<FString> Approximations;

	/** The one repeat size the layer is published with: the base colour's, else the normal's, else 0. */
	double TilingCm() const { return BaseColorTilingCm > 0.0 ? BaseColorTilingCm : NormalTilingCm; }
};

/**
 * A slope overlay the landscape's material blends over its painted layers (#628): a final Lerp between the layer
 * blend and a look of its own (rock on steep ground), by a mask that comes from the world normal's Z. Read as a
 * function of that Z, so the exporter can bake it from the heightfield's own normals into a layer of its own.
 */
struct MANTLEEDITOR_API FMantleLandscapeSlopeOverlay
{
	/** Samples in WeightByNormalZ: one per 1/(LutSize - 1) of the normal's Z from 0 to 1. */
	static constexpr int32 LutSize = 1025;

	/** True when the material has the overlay and it could be evaluated. Its look is in FMantleLandscapeMaterialRead::Layers under Layer. */
	bool bFound = false;

	/** The name the baked layer is published under: "Slope", unless the material has a paint layer of that name. */
	FName Layer;

	/** How much of the overlay's look shows (0 to 1) where the normal's Z (1 flat, 0 a cliff) is index / (LutSize - 1). */
	TArray<float> WeightByNormalZ;

	/** The overlay's weight at NormalZ (clamped to 0 to 1), interpolated between the table's samples. 0 when the table is empty. */
	float WeightAt(double NormalZ) const;

	/** The note that the overlay is baked, which Look.Approximations carries so the coverage report says it. */
	static FString BakedNote();
};

/** One point of the ground a FMantleSurfaceProgram is evaluated at: where it is (world cm) and the frame of its surface. */
struct MANTLEEDITOR_API FMantleSurfaceSample
{
	FVector WorldPosition = FVector::ZeroVector;

	/** Where it is on the landscape's own grid, in samples (the unit of the landscape's UV): what Landscape Layer Coords and Texture Coordinate 0 are built from. */
	FVector2D LandscapeSample = FVector2D::ZeroVector;

	/** The surface normal, and the tangent and binormal of the frame a tangent-space vector is turned into world space with (all unit length). */
	FVector Normal = FVector::UpVector;
	FVector Tangent = FVector::ForwardVector;
	FVector Binormal = FVector::RightVector;
};

/**
 * A few scalar masks of a material compiled into a flat list of operations (#628), run per heightfield sample: the
 * slope and height rules of a landscape material that has no paint layers. Pure data once built; Evaluate is safe to
 * call from many threads at once.
 */
class MANTLEEDITOR_API FMantleSurfaceProgram
{
public:
	struct FImpl;

	FMantleSurfaceProgram(TSharedPtr<FImpl> InImpl, int32 InOutputCount);

	/** Masks the program computes. */
	int32 NumOutputs() const { return OutputCount; }

	/** The value of every output (NumOutputs of them, raw: not clamped) at Sample. */
	void Evaluate(const FMantleSurfaceSample& Sample, double* OutValues) const;

private:
	TSharedPtr<FImpl> Impl;
	int32 OutputCount = 0;
};

/**
 * A landscape material with no paint layers that picks its ground from the surface slope and height (#628): a chain of
 * Lerps, each by a mask of the normal and the world position, over a base look. Layer 0 is the base; layer k (1 and up)
 * is blended over layers 0 to k - 1 by output k - 1 of the program, so its weight is mask k times the product of
 * (1 - mask j) for every later j, and the base's is the product of all (1 - mask).
 */
struct MANTLEEDITOR_API FMantleLandscapeAutoChain
{
	/** Most layers a chain has: the exporter keeps what fits and folds the rest, but bakes each of these. */
	static constexpr int32 MaxLayers = 16;

	bool bFound = false;

	/** The chain's layers, base first: the names of the matching entries of FMantleLandscapeMaterialRead::Layers. */
	TArray<FName> LayerNames;

	/** One output per layer after the base. */
	TSharedPtr<FMantleSurfaceProgram> Program;

	/** The note that each of the layers is baked, which Look.Approximations carries so the coverage report says it. */
	static FString BakedNote();
};

/** What FMantleLandscapeMaterialReader::Read found in a landscape's material. */
struct MANTLEEDITOR_API FMantleLandscapeMaterialRead
{
	/** The landscape material's name. */
	FString MaterialName;

	/** True when the material was readable (it has a graph) and at least one paint layer was found in it. */
	bool bFoundLayers = false;

	/** Every paint layer the material draws, in the order it names them, then the slope overlay's look (SlopeOverlay.Layer) when it has one. */
	TArray<FMantleLandscapeLayerLook> Layers;

	/** The slope overlay, when the material has one (#628): not a paint layer, so the exporter gives it a weightmap channel of its own. */
	FMantleLandscapeSlopeOverlay SlopeOverlay;

	/**
	 * The slope and height rules of a material with no paint layers (#628), when it has them: Layers then holds the chain's
	 * looks in the chain's order, and the exporter bakes each one's weight from the heightfield (FMantleTerrainExport::BakeAutoWeights).
	 */
	FMantleLandscapeAutoChain AutoChain;

	/** What the material does to the whole landscape that is not kept: slope or height overlays, distance-blended tiling, macro variation. */
	TArray<FString> Approximations;

	const FMantleLandscapeLayerLook* Find(FName Layer) const;
};

/**
 * Reads a landscape's material for what each paint layer looks like (#628), so a creator needs no Place
 * Settings entry per layer. It walks the graph that feeds Base Color (and Normal) from the output back to
 * the Landscape Layer Blend, Layer Weight, Layer Sample and Layer Switch nodes, then each layer's input to
 * the texture samples it blends, through material functions and the instance chain's parameter overrides.
 * What it cannot keep it says (FMantleLandscapeMaterialRead::Approximations): Mantle paints each layer as one
 * flat tiled material, so anything the material does from the surface normal, the height or the camera
 * distance is reported, never silently dropped. One thing from the surface normal is kept: a final blend
 * over the paint layers by the normal's Z (a rock look on steep ground) is read as a function of that Z
 * (FMantleLandscapeSlopeOverlay), which the exporter bakes from the heightfield into a layer of its own.
 *
 * Needs the editor graph (the material's expressions), so it reads nothing from a cooked material.
 */
class MANTLEEDITOR_API FMantleLandscapeMaterialReader
{
public:
	/**
	 * Reads LandscapeMaterial. SpacingCm is the landscape's X scale (cm between samples): the landscape's own
	 * UV is one unit per sample, so a layer coordinate's MappingScale of 4 repeats a texture every 4 samples.
	 * A material with no paint layer but a chain of blends by the surface slope and the world height reads as an
	 * AutoChain (#628). False, with OutProblem as a sentence of its own, when there is no graph to read or neither
	 * a paint layer nor such a chain in it; Out then has nothing in it.
	 */
	static bool Read(const UMaterialInterface* LandscapeMaterial, double SpacingCm, FMantleLandscapeMaterialRead& Out, FString& OutProblem);

	/**
	 * A transient material for one layer's look: Base Color and Normal from the look's textures, or its flat
	 * colour (mid grey when it has neither), the textures sampled on plain UVs so the terrain's own tiling
	 * applies. What the exporter converts like any Place Settings material. The same look gives the same
	 * object (named by what it holds) for as long as the editor runs, so Check, Export and the converter agree
	 * on its key. Null when Look is empty of everything.
	 */
	static UMaterialInterface* MakeLayerMaterial(const FMantleLandscapeLayerLook& Look);

	/** The name (without a path) MakeLayerMaterial gives Look's material: stable for the same layer, textures and colour. */
	static FString LayerMaterialName(const FMantleLandscapeLayerLook& Look);

	/**
	 * A layer's name from the name of a parameter or texture it came from: a leading prefix token (letters and an underscore, 4 characters
	 * at most: "MW_", "T_", "TEX_") and a trailing "BaseColor" or "Color" taken off. "MW_WaterColor" is "Water".
	 */
	static FString LayerNameFromParameter(const FString& Name);

	/** The average colour of Texture's source pixels in linear space, sampled on a grid. False when it has no readable source (a cooked or generated texture). */
	static bool AverageColour(UTexture* Texture, FLinearColor& OutColour);
};
