// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleHeightfieldReader.h"
#include "MantleLandscapeMaterialReader.h"
#include "MantlePlaceValidator.h"

class AActor;
class FMantleCoverage;
class UMaterialInterface;
class UPhysicalMaterial;
class UWorld;

/**
 * A landscape read for publishing (#466): every sample of its heights and of each painted layer's
 * weights, before any resampling. Pure data, so the conversions below are testable without a
 * landscape.
 */
struct MANTLEEDITOR_API FMantleTerrainSource
{
	/** Samples along X and Y. */
	int32 Width = 0;
	int32 Height = 0;

	/** The landscape's 16-bit height values, row-major (32768 is the landscape's own zero). */
	TArray<uint16> Heights;

	/** cm between samples along X and Y (the landscape's scale), and cm per height value (its Z scale / 128). */
	double SpacingX = 100.0;
	double SpacingY = 100.0;
	double HeightScale = 100.0 / 128.0;

	/** The height of value 0 in cm: -32768 x HeightScale, so 32768 is the landscape's origin. */
	double HeightOffset = -32768.0 * 100.0 / 128.0;

	/**
	 * The paint layers with weight somewhere, in the landscape's order, and each one's weights (Width x Height, 0 to 255).
	 * A landscape with none whose material picks its ground from slope and height (FMantleLandscapeAutoChain) has the
	 * chain's layers here instead, their weights baked from the heightfield (BakeAutoChain).
	 */
	TArray<FName> LayerNames;
	TArray<TArray<uint8>> LayerWeights;

	/**
	 * Each layer's Phys Material, from its layer info object, by layer name (#596): every layer the
	 * landscape has, painted or not, where one is set. What the published layer's surface comes from.
	 */
	TMap<FName, const UPhysicalMaterial*> LayerPhysMaterials;

	/**
	 * Part of the landscape is a hole (its visibility layer is painted), and how many samples. The heightfield has no hole mask, so the
	 * heights and weights are published as they are, filling the holes: the landscape itself is never changed. ChooseLayers says so.
	 */
	bool bHasHoles = false;
	int32 HoleSamples = 0;

	/**
	 * What the landscape's own material says each paint layer looks like (#628), read when a painted layer has no
	 * Place Settings entry: ChooseLayers draws such a layer from its look. bMaterialRead is false when the material
	 * could not be read (MaterialProblem says why), and then a layer with no entry is refused as before.
	 */
	FMantleLandscapeMaterialRead MaterialRead;
	bool bMaterialRead = false;
	FString MaterialProblem;

	/** The average base colour of each layer's look, by layer name, where known (#628): which kept layer a folded one goes into. */
	TMap<FName, FLinearColor> LayerColours;

	/**
	 * Water Bodies that carve this landscape through a brush edit layer (GhzGarage/mantle#997). The heights above are the landscape's final,
	 * merged heightmap, so the carve is in them; bCarveRefreshed says the edit layers were out of date and were rebuilt first, bCarveStale that
	 * they still were afterwards (the carve may be missing or old).
	 */
	int32 WaterBodiesCarving = 0;
	bool bCarveRefreshed = false;
	bool bCarveStale = false;

	/** The landscape sample (in its own grid) that sample (0, 0) is: what a mask's landscape UV is counted from. */
	FIntPoint FirstSample = FIntPoint::ZeroValue;

	/** Where sample (0, 0) is and how the landscape is turned: the Terrain entity's transform (scale 1). */
	FTransform Transform = FTransform::Identity;
};

/** One Terrain layer as Export writes it: the landscape's layer, the material Mantle Place Settings gives it, and its tiling. */
struct FMantleTerrainLayerChoice
{
	FName Layer;
	UMaterialInterface* Material = nullptr;
	double TilingCm = 400.0;

	/** Index into FMantleTerrainSource::LayerNames and LayerWeights, or INDEX_NONE for a landscape with no painted layer. */
	int32 SourceIndex = INDEX_NONE;

	/** The layer's Phys Material (FMantleTerrainSource::LayerPhysMaterials), null for none: Export maps it to the layer's surface (#596). */
	const UPhysicalMaterial* PhysMaterial = nullptr;

	/** The material came from the landscape's own material, not Place Settings (#628). */
	bool bDerived = false;

	/** Source layers whose paint is added to this layer's weight, because the landscape has more painted layers than Mantle publishes (#628). */
	TArray<int32> FoldedSources;

	/**
	 * The landscape material's slope overlay (#628), not a painted layer (SourceIndex is INDEX_NONE): its weight is baked
	 * from the heightfield's slope (BakeSlopeMask), and the painted layers' weights are scaled by what it leaves.
	 */
	bool bSlopeOverlay = false;
};

/**
 * Which of a landscape's painted layers Mantle publishes and where the others go (#628): when more layers are
 * painted than FMantlePlaceLimits::MaxTerrainLayers, the most painted are kept and each other layer's weight is
 * added to the kept layer whose base colour is nearest.
 */
struct FMantleTerrainFold
{
	/** Source layers kept, in the landscape's order. */
	TArray<int32> Kept;

	/** Per source layer: its slot in Kept (a kept layer's own, a folded layer's target), or INDEX_NONE for a layer not considered. */
	TArray<int32> SlotOfSource;

	bool IsFolded(const int32 SourceIndex) const
	{
		return SlotOfSource.IsValidIndex(SourceIndex) && SlotOfSource[SourceIndex] != INDEX_NONE && !Kept.Contains(SourceIndex);
	}
};

/**
 * A level's landscape as a Mantle Terrain (#466): what Export, Check this place and the asset
 * converter share, so the three never disagree.
 *
 * A landscape becomes one heightfield (MHF1, at most 2049 samples a side: a larger one is resampled)
 * and, with more than one painted layer, one RGBA weightmap texture (at most the texture size cap; a
 * second one, weightmap2, for layers 4 to 7), plus up to FMantlePlaceLimits::MaxTerrainLayers layer
 * materials: a layer's entry in Mantle Place Settings, else the look read from the landscape's own
 * material (#628; a landscape material's layer blend is not something a player's client can rebuild, so
 * each layer is published flat). A slope overlay in that material is baked from the heightfield into a
 * layer of its own. A landscape with no painted layer whose material picks its ground from the slope and
 * height (FMantleLandscapeAutoChain) has each of that chain's layers baked from the heightfield, and then
 * goes through the same choosing, folding and weightmaps as painted layers. More painted layers than fit
 * are folded into the nearest kept ones. Holes painted in the visibility layer are filled in the published data (the format has no hole
 * mask), with a warning and an approximated coverage line; the landscape is only read.
 */
class MANTLEEDITOR_API FMantleTerrainExport
{
public:
	/**
	 * limits.json placeAssets.heightfield's ranges, which the cook worker refuses a heightfield
	 * outside (Mantle.Editor.Terrain.LimitsMatchContract holds these to the plugin's copy).
	 */
	static constexpr double MinSpacingCm = 1.0;
	static constexpr double MaxSpacingCm = 10000.0;
	static constexpr double MaxHeightScaleCm = 100.0;
	static constexpr double MaxHeightOffsetCm = 10000000.0;

	/** Every landscape in World, in actor order. */
	static TArray<AActor*> CollectLandscapes(UWorld* World);

	/** "heightfield:" + the landscape's path: the local key the converter, the upload queue and the exporter share. */
	static FString HeightfieldKey(const AActor* Landscape);

	/** "weightmap:" + the landscape's path, the key of its weightmap texture. */
	static FString WeightmapKey(const AActor* Landscape);

	/** "weightmap2:" + the landscape's path, the key of its second weightmap texture (layers 4 to 7, #628). */
	static FString Weightmap2Key(const AActor* Landscape);

	/** Weightmap2Key for a landscape path (what the converter has). */
	static FString Weightmap2KeyForPath(const FString& LandscapePath);

	/**
	 * Reads Landscape's final heights and paint weights (every loaded proxy), its holes and its
	 * transform. False, with OutProblem phrased to follow the landscape's name, when it has no data
	 * or a scale Mantle cannot publish (not above zero, or its spacing outside limits.json).
	 */
	static bool ReadLandscape(const AActor* Landscape, FMantleTerrainSource& Out, FString& OutProblem);

	/**
	 * Which of Candidates (indices into Source's layers) are kept at most MaxLayers, and where the rest go (#628):
	 * the layers with the most total paint weight are kept, in the landscape's order, and each other layer's weight
	 * goes into the kept layer whose Source.LayerColours entry is nearest in linear RGB, or into the most painted
	 * kept layer when either colour is unknown. Pure data.
	 */
	static FMantleTerrainFold PlanFold(const FMantleTerrainSource& Source, TConstArrayView<int32> Candidates, int32 MaxLayers);

	/**
	 * The layers Export publishes for Source: each painted layer with its Mantle Place Settings entry, or else
	 * the look the landscape's own material gives it (#628), in the landscape's order, or the settings' first
	 * entry for a landscape with nothing painted. More painted layers than FMantlePlaceLimits::MaxTerrainLayers
	 * keep the most painted and fold the rest into the nearest (PlanFold), with a warning that names them. A slope
	 * overlay the landscape's material has (FMantleLandscapeMaterialRead::SlopeOverlay) is added last, as a layer
	 * with bSlopeOverlay set and a slot of its own, so one fewer painted layer is kept.
	 * Warnings (a fold, something the landscape material does that Mantle cannot keep) go in OutIssues and
	 * Coverage (when given), and do not fail it.
	 * A landscape with nothing painted and no Place Settings entry gets a default layer (the first look its landscape material states, else
	 * plain grey) with a warning. Holes are warned about, never refused (Source.bHasHoles).
	 * False, with each blocking problem in OutIssues under Label: a painted layer with neither an entry
	 * nor a look in a landscape material that could not be read, or a default layer that could not be made.
	 */
	static bool ChooseLayers(UWorld* World, const FMantleTerrainSource& Source, const FString& Label,
		TArray<FMantleTerrainLayerChoice>& OutLayers, TArray<FMantleValidationIssue>& OutIssues, FMantleCoverage* Coverage = nullptr);

	/** For each of Source's layers (SourceCount of them), the weightmap channel (its index in Layers) its paint goes to, folded layers included; INDEX_NONE for a layer not drawn. */
	static TArray<int32> FoldChannels(const TArray<FMantleTerrainLayerChoice>& Layers, int32 SourceCount);

	/**
	 * The heightfield published for Source: its samples as they are when both sides fit MaxSamples,
	 * else resampled (bilinear) to at most MaxSamples a side with the spacing grown so the terrain
	 * keeps its size.
	 */
	static FMantleHeightfield MakeHeightfield(const FMantleTerrainSource& Source, int32 MaxSamples = FMantleHeightfieldReader::MaxSize);

	/** The weightmap's size for a Width x Height grid: the grid's own, each side at most MaxSize. */
	static FIntPoint WeightmapSize(int32 Width, int32 Height, int32 MaxSize);

	/** Weightmap textures a Terrain of LayerCount layers has: none for one layer, one up to 4, two beyond (weightmap2 carries layers 4 to 7). */
	static int32 WeightmapCount(int32 LayerCount);

	/**
	 * The slope overlay's weight at every sample of Source, 0 to 255 (Width x Height, row-major; empty when the overlay
	 * is not found): each sample's normal from the heights' central differences and the spacing, (-dz/dx, -dz/dy, 1)
	 * normalised, then Overlay.WeightAt of its Z, which is the material's own mask (Mask B of the world normal, times
	 * Multiplier, to the Power, then the blend with the layers' height) evaluated for that Z. Flat ground gives 0 for
	 * an overlay that shows on steep ground only (as Spruce's rock does), a 60 degree slope about 255.
	 */
	static TArray<uint8> BakeSlopeMask(const FMantleTerrainSource& Source, const FMantleLandscapeSlopeOverlay& Overlay);

	/**
	 * The weights of every layer of Source.MaterialRead.AutoChain (#628), one Width x Height array of 0 to 255 per chain layer in
	 * its order (empty when the material has no chain or the heights are not Width x Height). At each sample the program is
	 * evaluated with the sample's world position (Source.Transform of its place on the grid, the height scale and offset) and
	 * its normal, tangent and binormal from the heights' central differences, as BakeSlopeMask takes them. Each mask is
	 * clamped to 0 to 1; layer k's weight is its mask times (1 - mask j) for every later j, and the base's is the product of
	 * all (1 - mask). Each sample's weights sum to 255, the remainder going to its heaviest layer.
	 */
	static TArray<TArray<uint8>> BakeAutoWeights(const FMantleTerrainSource& Source);

	/**
	 * Adds Source's auto chain layers (#628) to its LayerNames and LayerWeights as BakeAutoWeights bakes them, the layers that
	 * weigh nothing anywhere left out as an unpainted layer is. False, with Source as it was, when there is nothing to add.
	 */
	static bool BakeAutoChain(FMantleTerrainSource& Source);

	/**
	 * The weightmaps for Layers (ChooseLayers' result) at Size: OutFirst holds layers 0 to 3 in R, G, B and A, OutSecond
	 * layers 4 to 7 (left empty up to 4 layers). Each painted layer's weight is the landscape's own, with the weights of
	 * the layers folded into it added; a slope overlay layer's is BakeSlopeMask's, and the painted layers' weights are
	 * scaled by what it leaves (1 - mask), so each texel sums to 255 across both maps (all of the first layer where nothing
	 * is painted). Row by row from the top, stretched over the whole terrain, as MakeWeightmap.
	 */
	static void MakeWeightmaps(const FMantleTerrainSource& Source, const TArray<FMantleTerrainLayerChoice>& Layers, FIntPoint Size,
		TArray<FColor>& OutFirst, TArray<FColor>& OutSecond);

	/**
	 * MakeWeightmaps as PNG files for the upload: the first (always) and the second (only with more than 4 layers, else empty),
	 * at the Source's own size up to MaxSize a side, halved together until both are within MaxBytes (not below 64). False,
	 * with the sizes left as they were, when they still do not fit.
	 */
	static bool EncodeWeightmaps(const FMantleTerrainSource& Source, const TArray<FMantleTerrainLayerChoice>& Layers, int32 MaxSize, int32 MaxBytes,
		FIntPoint& OutSize, TArray<uint8>& OutFirst, TArray<uint8>& OutSecond);

	/**
	 * Size.X x Size.Y pixels, row by row from the top (row 0 is the terrain's first row of samples),
	 * stretched over the whole terrain: R, G, B and A are the weights of SourceLayers[0] to [3]
	 * (indices into Source's layers, INDEX_NONE for none) sampled at the texel's centre, summing to
	 * 255 (all of the first where nothing is painted). What FMantlePng::EncodeRGBA8 takes.
	 *
	 * ChannelOfSource (FoldChannels, #628) is empty for none, else one entry per Source layer: the channel
	 * (0 to 3) the layer's weight is added to, so a layer that was folded into a kept one still paints
	 * where it was painted; INDEX_NONE adds it nowhere.
	 */
	static TArray<FColor> MakeWeightmap(const FMantleTerrainSource& Source, TConstArrayView<int32> SourceLayers, FIntPoint Size, TConstArrayView<int32> ChannelOfSource = TConstArrayView<int32>());

	/**
	 * Check this place's terrain part: at most FMantlePlaceLimits::MaxTerrainsPerPlace landscapes,
	 * each one readable and with layers ChooseLayers accepts, and a warning when one is resampled.
	 */
	static void CheckLevel(UWorld* World, TArray<FMantleValidationIssue>& OutIssues);
};
