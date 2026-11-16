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
	 * Part of the landscape is a hole painted in its visibility layer (at least LANDSCAPE_VISIBILITY_THRESHOLD of it, where Unreal cuts the
	 * ground), and how many samples. Published as holes, in the Terrain's hole mask (Holes below).
	 */
	bool bHasHoles = false;
	int32 HoleSamples = 0;

	/**
	 * Samples of the extent no landscape component covers (a landscape whose components do not fill its bounds). They have no ground: they
	 * are holes in the mask too, and their heights are filled from the nearest covered sample, only so that resampling does not pull the
	 * ground around them down to the lowest height.
	 */
	int32 MissingSamples = 0;

	/** Per sample (Width x Height, row-major), 1 where there is no ground (painted hole or no component), else 0. Empty when there is none. */
	TArray<uint8> Holes;

	/**
	 * What the landscape's own material says each paint layer looks like (#628), read when a painted layer has no
	 * Place Settings entry: ChooseLayers draws such a layer from its look. bMaterialRead is false when the material
	 * could not be read (MaterialProblem says why), and then a layer with no entry is refused as before.
	 */
	FMantleLandscapeMaterialRead MaterialRead;
	bool bMaterialRead = false;
	FString MaterialProblem;

	/**
	 * The landscape's own material, null for none. When its layers cannot be read from it (an ordinary UV-mapped material with no Landscape
	 * Layer Blend, DowntownWestModularPack's), ChooseLayers bakes the material itself as the layer's look instead of drawing plain gray.
	 */
	UMaterialInterface* LandscapeMaterial = nullptr;

	/**
	 * The most landscape UV units (landscape quads) any texture of LandscapeMaterial takes to repeat (FMantleLandscapeMaterialReader::LargestUvRepeat:
	 * 10 for a texture tiled by 0.1). 0 when the material does not say, or when it was not needed (its layers were read).
	 */
	double LandscapeMaterialUvRepeat = 0.0;

	/** The average base color of each layer's look, by layer name, where known (#628): which kept layer a folded one goes into. */
	TMap<FName, FLinearColor> LayerColors;

	/**
	 * Water Bodies that carve this landscape through a brush edit layer (GhzGarage/mantle#997). The heights above are the landscape's final,
	 * merged heightmap, so the carve is in them; bCarveRefreshed says the edit layers were out of date and were rebuilt first, bCarveStale that
	 * they still were afterwards (the carve may be missing or old).
	 */
	int32 WaterBodiesCarving = 0;
	bool bCarveRefreshed = false;
	bool bCarveStale = false;

	/**
	 * The slope overlay's weight at every sample (BakeSlopeMask over the whole landscape), Width x Height, when it was baked before the landscape
	 * was cut into terrain tiles (CutTile): a tile's slopes at its border then come from the neighbouring tile's heights too. Empty for none,
	 * and then MakeWeightmaps bakes it from Heights.
	 */
	TArray<uint8> SlopeMask;

	/** Per sample (Width x Height, row-major), 1 where no landscape component covers it (ApplyCoverage), so a terrain tile can count its own MissingSamples. Empty when all are covered. */
	TArray<uint8> Uncovered;

	/** The landscape sample (in its own grid) that sample (0, 0) is: what a mask's landscape UV is counted from. */
	FIntPoint FirstSample = FIntPoint::ZeroValue;

	/** Where sample (0, 0) is and how the landscape is turned: the Terrain entity's transform (scale 1). */
	FTransform Transform = FTransform::Identity;

	/** The landscape actor read, which Mantle Place Settings entries for one landscape name (FMantleTerrainLayerSetting::Landscape); empty for none. */
	FSoftObjectPath LandscapePath;
};

/**
 * One terrain tile of a landscape (stage 4, terrain-tiles-format.md): a block of the landscape's own samples, at most
 * FMantleTerrainExport::TileQuads quads a side, published as a Terrain entity of its own. Neighbouring tiles share
 * their border samples (the last column of one is the first column of the next).
 */
struct FMantleTerrainTile
{
	/** Which tile: (tx, ty), counted from the landscape's sample (0, 0). */
	FIntPoint Index = FIntPoint::ZeroValue;

	/** The landscape's sample (relative to the source's sample (0, 0)) that this tile's sample (0, 0) is. */
	FIntPoint First = FIntPoint::ZeroValue;

	/** Samples along X and Y. */
	int32 Width = 0;
	int32 Height = 0;

	/** Another tile follows this one along X (Y): this is a full tile on that axis and not the last one. */
	bool bNeighborX = false;
	bool bNeighborY = false;
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

	/**
	 * Material is the landscape's own material, baked whole as this layer's look because its layers could not be read from it. It is baked
	 * over UVs 0 to BakeUvRange (landscape quads), so a texture it tiles finely repeats inside the bake, and TilingCm is that many quads.
	 */
	bool bLandscapeMaterialBake = false;
	int32 BakeUvRange = 1;
};

/**
 * Which of a landscape's painted layers Mantle publishes and where the others go (#628): when more layers are
 * painted than FMantlePlaceLimits::MaxTerrainLayers, the most painted are kept and each other layer's weight is
 * added to the kept layer whose base color is nearest.
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
 * A level's landscapes as Mantle Terrains (#466), one Terrain entity each: what Export, Check this place and the asset
 * converter share, so the three never disagree.
 *
 * A landscape becomes one heightfield (MHF1, at most 2049 samples a side) or, when it is larger, one heightfield per terrain tile
 * (PlanTiles: 2,048 quads each, cut at the landscape's own spacing, neighbours sharing a border row or column of samples), each a Terrain
 * entity of its own with its own paint, holes and grass maps, and
 * with more than one painted layer, one RGBA weightmap texture (at most the texture size cap; a
 * second one, weightmap2, for layers 4 to 7), plus up to FMantlePlaceLimits::MaxTerrainLayers layer
 * materials: a layer's entry in Mantle Place Settings, else the look read from the landscape's own
 * material (#628; a landscape material's layer blend is not something a player's client can rebuild, so
 * each layer is published flat). A slope overlay in that material is baked from the heightfield into a
 * layer of its own. A landscape with no painted layer whose material picks its ground from the slope and
 * height (FMantleLandscapeAutoChain) has each of that chain's layers baked from the heightfield, and then
 * goes through the same choosing, folding and weightmaps as painted layers. More painted layers than fit
 * are folded into the nearest kept ones. Holes painted in the visibility layer, and samples no landscape
 * component covers, publish as holes in the Terrain's hole mask; the landscape is only read.
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

	/** Quads in one terrain tile: 2048, 16 chunks of 128 quads, and one less than the most samples a heightfield holds. */
	static constexpr int32 TileQuads = 2048;
	static_assert(TileQuads + 1 == FMantleHeightfieldReader::MaxSize, "a terrain tile is one whole heightfield");

	/**
	 * How a landscape of Width x Height samples is cut into terrain tiles: from its sample (0, 0), TileQuads quads each, the last tile on an
	 * axis taking what is left (at least 2 samples), neighbours sharing their border samples. ceil((samples - 1) / TileQuads) tiles
	 * on an axis, at least 1, so a landscape of at most MaxSize samples a side is one tile that is all of it. Row by row (ty, then tx). Pure data.
	 * bResampled (the landscape's tiles did not fit the place's limits, see FitTerrains): one tile that is all of it, which MakeHeightfield resamples down to one heightfield, as before tiles.
	 */
	static TArray<FMantleTerrainTile> PlanTiles(int32 Width, int32 Height, bool bResampled = false);

	/** The number of tiles PlanTiles gives. */
	static int32 TileCount(int32 Width, int32 Height);

	/** The tiles an actor's landscape extent (as ReadLandscape would read it, without reading it) is cut into; 1 when it has none loaded. */
	static int32 TileCount(const AActor* Landscape);

	/**
	 * What a tile's local keys and entity name end with: "#tx,ty" for a landscape of more than one tile, and nothing for a one-tile
	 * landscape (its keys, name and id are what they were before tiles). TileCount is the landscape's tile count.
	 */
	static FString TileSuffix(FIntPoint Index, int32 TileCount);

	/** The Terrain entity's name for a tile: Label itself for a one-tile landscape, else "<Label>_<tx>_<ty>", the label cut so that the whole name fits MaxEntityNameLength. */
	static FString TileName(const FString& Label, FIntPoint Index, int32 TileCount);

	/**
	 * The Terrain entity's id for a tile, from the id the landscape has (BaseId, "ent_" + its guid): tile (0, 0) keeps it, any other tile has
	 * "ent_" + the first 24 lowercase hex digits of the MD5 of "<guid>|T|<tx>|<ty>" (guid being BaseId without "ent_"). A pure function of
	 * the landscape's id and (tx, ty), so a republish keeps every tile's id.
	 */
	static FString TileEntityId(const FString& BaseId, FIntPoint Index);

	/**
	 * A layer's tiling for the tiles of a landscape of TileCount tiles (terrain-tiles-format.md section 4). The game maps a terrain's UV 0 to 1
	 * over its own extent, so the texture period is that terrain's own tiling on both axes: tiles meet in phase only when they all use the
	 * same tiling and a full tile repeats a texture a whole number of times. Every tile of a landscape of more than one tile gets
	 * E / repeats, E = TileQuads x SpacingX (a full tile's X extent) and repeats = max(round(E / TilingCm), ceil(E / MaxTerrainTilingCm), 1),
	 * so it never goes past the cap the backend and the game parser hold a layer to. A one-tile landscape (or a layer with no tiling) keeps TilingCm.
	 */
	static double TileTiling(double TilingCm, int32 TileCount, double SpacingX);

	/** The Terrain entity's transform for a tile: Source's, moved to the tile's sample (0, 0). */
	static FTransform TileTransform(const FMantleTerrainSource& Source, const FMantleTerrainTile& Tile);

	/** Whether the tile's samples hold any hole (Source.Holes), so its Terrain publishes a hole mask. */
	static bool TileHasHoles(const FMantleTerrainSource& Source, const FMantleTerrainTile& Tile);

	/** The Width x Height grid's samples in Tile, row-major (what a tile gets of a grid over the landscape: a layer's weights, a grass type's). Empty when Grid is not Width x Height. */
	static TArray<uint8> SliceGrid(const TArray<uint8>& Grid, int32 Width, int32 Height, const FMantleTerrainTile& Tile);

	/**
	 * The slope overlay layer's weights over the whole Source, to hand to every CutTile, when Layers has a slope overlay layer; empty otherwise.
	 */
	static TArray<uint8> BakeSlopeFor(const FMantleTerrainSource& Source, const TArray<FMantleTerrainLayerChoice>& Layers);

	/**
	 * The tile as a landscape of its own: its heights (the landscape's own values, bit for bit), the weights of every one of Source's layers
	 * (so a layer's index is the same in every tile), its holes (none when the tile has no hole) and its transform, the same spacing,
	 * height scale and offset, layers and materials. SlopeMask is BakeSlopeFor's (null for none). Everything that makes a Terrain from a
	 * source (MakeHeightfield, MakeWeightmaps, EncodeHoleMask) then works on a tile unchanged.
	 */
	static FMantleTerrainSource CutTile(const FMantleTerrainSource& Source, const FMantleTerrainTile& Tile, const TArray<uint8>* SlopeMask = nullptr);

	/** "heightfield:" + the landscape's path: the local key the converter, the upload queue and the exporter share. A tile adds TileSuffix. */
	static FString HeightfieldKey(const AActor* Landscape);

	/** "weightmap:" + the landscape's path, the key of its weightmap texture. */
	static FString WeightmapKey(const AActor* Landscape);

	/** "weightmap2:" + the landscape's path, the key of its second weightmap texture (layers 4 to 7, #628). */
	static FString Weightmap2Key(const AActor* Landscape);

	/** Weightmap2Key for a landscape path (what the converter has). */
	static FString Weightmap2KeyForPath(const FString& LandscapePath);

	/** "terrainholes:" + the landscape's path, the key of its hole mask texture. */
	static FString HolesKey(const AActor* Landscape);

	/** HolesKey for a landscape path (what the converter has). */
	static FString HolesKeyForPath(const FString& LandscapePath);

	/** Whether Source has samples with no ground, so its Terrain publishes a hole mask. */
	static bool HasHoles(const FMantleTerrainSource& Source) { return Source.Holes.Contains(1); }

	/**
	 * The Terrain's hole mask (the `holes` texture) at Size: texel (x, y) is published sample (x, y) of a Size grid laid over Source's samples
	 * corner to corner (row 0 is the first row), red 0 for a hole and 255 for ground, from the nearest source sample. Empty when Source has
	 * no holes. Size is the heightfield's (MakeHeightfield), a side capped to the texture limit when it is larger (the client maps by nearest).
	 */
	static TArray<FColor> MakeHoleMask(const FMantleTerrainSource& Source, FIntPoint Size);

	/** The hole mask as a PNG at Field's size, each side at most MaxTextureSize; empty when Source has no holes. OutSize is the PNG's. */
	static TArray<uint8> EncodeHoleMask(const FMantleTerrainSource& Source, const FMantleHeightfield& Field, int32 MaxTextureSize, FIntPoint& OutSize);

	/**
	 * Marks Source's samples that no component covers as holes and fills their heights from the nearest covered one, along the row (else
	 * from the nearest filled row). Covered is Width x Height, nonzero where a component covers the sample. Pure data; ReadLandscape uses it.
	 */
	static void ApplyCoverage(FMantleTerrainSource& Source, const TArray<uint8>& Covered);

	/**
	 * Reads Landscape's final heights and paint weights (every loaded proxy), its holes and its
	 * transform. False, with OutProblem phrased to follow the landscape's name, when it has no data
	 * or a scale Mantle cannot publish (not above zero, or its spacing outside limits.json).
	 */
	static bool ReadLandscape(const AActor* Landscape, FMantleTerrainSource& Out, FString& OutProblem);

	/**
	 * Which of Candidates (indices into Source's layers) are kept at most MaxLayers, and where the rest go (#628):
	 * the layers with the most total paint weight are kept, in the landscape's order, and each other layer's weight
	 * goes into the kept layer whose Source.LayerColors entry is nearest in linear RGB, or into the most painted
	 * kept layer when either color is unknown. Pure data.
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
	 * that material baked whole, else plain gray) with a warning. A painted layer with no entry whose landscape material could not be read is
	 * drawn with that material baked whole (bLandscapeMaterialBake), or plain gray when the landscape has no material. Holes are warned about, never refused (Source.bHasHoles).
	 * False, with each blocking problem in OutIssues under Label: a painted layer with neither an entry
	 * nor a look in a landscape material that could not be read, or a default layer that could not be made.
	 */
	static bool ChooseLayers(UWorld* World, const FMantleTerrainSource& Source, const FString& Label,
		TArray<FMantleTerrainLayerChoice>& OutLayers, TArray<FMantleValidationIssue>& OutIssues, FMantleCoverage* Coverage = nullptr);

	/** For each of Source's layers (SourceCount of them), the weightmap channel (its index in Layers) its paint goes to, folded layers included; INDEX_NONE for a layer not drawn. */
	static TArray<int32> FoldChannels(const TArray<FMantleTerrainLayerChoice>& Layers, int32 SourceCount);

	/** The most landscape quads a bake of the landscape's own material may cover: more and each repeat of its textures gets too few pixels. */
	static constexpr int32 MaxLandscapeBakeUvRange = 64;

	/**
	 * How many landscape quads a bake of Source's landscape material covers: its largest texture repeat (Source.LandscapeMaterialUvRepeat)
	 * rounded up, so its coarsest texture repeats whole inside it (finer ones that do not divide it still show a seam), or MaxLandscapeBakeUvRange when the material does not say; at most that.
	 */
	static int32 LandscapeBakeUvRange(const FMantleTerrainSource& Source);

	/**
	 * The heightfield published for Source: its samples as they are when both sides fit MaxSamples,
	 * else resampled (bilinear) to at most MaxSamples a side with the spacing grown so the terrain
	 * keeps its size. Export never resamples: a landscape larger than one heightfield is cut into tiles (PlanTiles, CutTile) first.
	 */
	static FMantleHeightfield MakeHeightfield(const FMantleTerrainSource& Source, int32 MaxSamples = FMantleHeightfieldReader::MaxSize);

	/** The weightmap's size for a Width x Height grid: the grid's own, each side at most MaxSize. */
	static FIntPoint WeightmapSize(int32 Width, int32 Height, int32 MaxSize);

	/** Weightmap textures a Terrain of LayerCount layers has: none for one layer, one up to 4, two beyond (weightmap2 carries layers 4 to 7). */
	static int32 WeightmapCount(int32 LayerCount);

	/**
	 * The slope overlay's weight at every sample of Source, 0 to 255 (Width x Height, row-major; empty when the overlay
	 * is not found): each sample's normal from the heights' central differences and the spacing, (-dz/dx, -dz/dy, 1)
	 * normalized, then Overlay.WeightAt of its Z, which is the material's own mask (Mask B of the world normal, times
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
	 * (indices into Source's layers, INDEX_NONE for none) sampled at the texel's center, summing to
	 * 255 (all of the first where nothing is painted). What FMantlePng::EncodeRGBA8 takes.
	 *
	 * ChannelOfSource (FoldChannels, #628) is empty for none, else one entry per Source layer: the channel
	 * (0 to 3) the layer's weight is added to, so a layer that was folded into a kept one still paints
	 * where it was painted; INDEX_NONE adds it nowhere.
	 */
	static TArray<FColor> MakeWeightmap(const FMantleTerrainSource& Source, TConstArrayView<int32> SourceLayers, FIntPoint Size, TConstArrayView<int32> ChannelOfSource = TConstArrayView<int32>());

	/** The heightfield samples a Width x Height landscape publishes, its tiles' together (a border sample is in both tiles): what FMantlePlaceLimits::MaxTerrainSamplesPerPlace counts. */
	static int64 PublishedSamples(int32 Width, int32 Height);

	/** PublishedSamples of the extent ReadLandscape would read for Landscape, without reading it; 0 when it has none loaded. */
	static int64 PublishedSamples(const AActor* Landscape);

	/** The samples a Width x Height landscape publishes resampled down to one heightfield (the way before tiles): what a landscape whose tiles do not fit the place's limits takes instead. */
	static int64 ResampledSamples(int32 Width, int32 Height);
	static int64 ResampledSamples(const AActor* Landscape);

	/** Whether Landscape, one of its world's Terrain landscapes, publishes resampled to one Terrain because its tiles did not fit what the place's limits had left (TerrainLandscapes' OutResampled). */
	static bool PublishesResampled(const AActor* Landscape);

	/**
	 * Which landscapes of these PublishedSamples (in CollectLandscapes' order) publish as Terrains: the indices kept, in order. Past
	 * FMantlePlaceLimits::MaxTerrainsPerPlace Terrains kept (a landscape is as many Terrains as it has tiles: Tiles, one each when empty),
	 * a landscape is left out (TooManyLandscapes); one that would take the place past MaxTerrainSamplesPerPlace is left out
	 * (OverSampleBudget) and the next may still fit. Each one left out is in OutLeftOut, by index, with its error under Labels' entry. Pure data.
	 * A landscape of several tiles that does not fit as tiles is not left out when it fits resampled to one Terrain (ResampledSamples, per landscape):
	 * it is kept, and its index is in OutResampled. Only one that does not fit even so is left out, with the errors for that one Terrain.
	 */
	static TArray<int32> FitTerrains(TConstArrayView<int64> Samples, TConstArrayView<FString> Labels, TArray<TPair<int32, FMantleValidationIssue>>& OutLeftOut,
		TConstArrayView<int32> Tiles = TConstArrayView<int32>(), TConstArrayView<int64> ResampledSamples = TConstArrayView<int64>(), TArray<int32>* OutResampled = nullptr);

	/** World's landscapes that publish as Terrains (FitTerrains over CollectLandscapes): what Gather, Check and Export all take. */
	static TArray<AActor*> TerrainLandscapes(UWorld* World, TArray<TPair<const AActor*, FMantleValidationIssue>>* OutLeftOut = nullptr, TArray<const AActor*>* OutResampled = nullptr);

	/** The error for landscape Number (counted from 1, in CollectLandscapes' order) past FMantlePlaceLimits::MaxTerrainsPerPlace: it is left out. */
	static FMantleValidationIssue TooManyLandscapes(const FString& Label, int32 Number, int32 Tiles = 1, int32 TerrainsBefore = 0);

	/**
	 * What Check this place says of a landscape of Width x Height samples that publishes as more than one terrain tile: a warning
	 * "This landscape publishes as N terrain tiles", with the grid and what each tile is. Pure data.
	 */
	static FMantleValidationIssue TilesIssue(const FString& Label, int32 Width, int32 Height);

	/** What Check this place says of a landscape whose tiles did not fit the place's limits and that publishes resampled to Field's size as one Terrain. Pure data. */
	static FMantleValidationIssue ResampledIssue(const FString& Label, int32 Width, int32 Height, const FMantleHeightfield& Field);

	/**
	 * The error for a landscape of Samples (PublishedSamples) that the landscapes before it, with Before samples, leave no room for under
	 * FMantlePlaceLimits::MaxTerrainSamplesPerPlace: it is left out, and the next landscape may still fit.
	 */
	static FMantleValidationIssue OverSampleBudget(const FString& Label, int64 Samples, int64 Before);

	/**
	 * Check this place's terrain part: every landscape is a Terrain, at most FMantlePlaceLimits::MaxTerrainsPerPlace of them and
	 * MaxTerrainSamplesPerPlace samples together (in CollectLandscapes' order, as Export counts them), each one readable and with
	 * layers ChooseLayers accepts, and a warning when one is resampled.
	 */
	static void CheckLevel(UWorld* World, TArray<FMantleValidationIssue>& OutIssues);
};
