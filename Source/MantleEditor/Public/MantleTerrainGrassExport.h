// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleCoverage.h"
#include "MantlePlaceTypes.h"
#include "MantleTerrainExport.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonWriter.h"

class AActor;
class UMaterialInterface;
class UStaticMesh;
struct FGrassVariety;
struct FMantlePlaceAssetLimits;
struct FMantlePlaceAssetSet;
struct FMantlePlaceDependencies;
class ULandscapeGrassType;

/**
 * One Terrain grass entry as Export builds it (#628), before the mesh and the map have upload ids: the
 * variety's mesh and material overrides by slot, which grass map and channel carry its weight, and the
 * numbers. Look.Map, Look.Mesh and Look.Materials stay empty until MakeGrass fills them.
 */
struct MANTLEEDITOR_API FMantleGrassEntryDraft
{
	/** The grass type's name in the landscape material, for messages. */
	FString TypeName;

	/** The variety's mesh, null for a variety that has none (such an entry is never made). */
	const UStaticMesh* Mesh = nullptr;

	/** The variety's material overrides by slot; null keeps the mesh's own. */
	TArray<UMaterialInterface*> Materials;

	/** Which of the grass maps (0 to FMantlePlaceLimits::MaxTerrainGrassMaps - 1) and which channel (0 R to 3 A) carry the weight. */
	int32 Map = 0;
	int32 Channel = 0;

	/** density, scale, alignToSurface, randomYaw, cullDistance and castShadow. */
	FMantleTerrainGrass Look;
};

/** One thing Gather left out or approximated, for the log, the conversion's issues and the coverage report. */
struct MANTLEEDITOR_API FMantleGrassNote
{
	/** Approximated or Dropped. */
	EMantleCoverageOutcome Outcome = EMantleCoverageOutcome::Approximated;

	/** The grass type or variety it is about, for the coverage report; empty for the landscape's whole grass. */
	FString Name;

	/** In plain words, naming the grass type or variety. */
	FString Text;

	FMantleGrassNote() = default;
	FMantleGrassNote(const EMantleCoverageOutcome InOutcome, const FString& InName, const FString& InText)
		: Outcome(InOutcome), Name(InName), Text(InText)
	{
	}
};

/** What reading a landscape's grass gave (#628). */
struct MANTLEEDITOR_API FMantleTerrainGrassResult
{
	/** True when the weights are the engine's own grass maps, false when they are approximated from paint layers. */
	bool bEngineWeights = true;

	/** The grid the weights are on: the landscape's samples, as FMantleTerrainSource. */
	int32 Width = 0;
	int32 Height = 0;

	/** The grass types that have a map channel, in channel order (type n is map n / 4, channel n % 4). */
	TArray<FString> TypeNames;

	/** Each such type's weight, Width x Height, 0 to 255. */
	TArray<TArray<uint8>> TypeWeights;

	/** Each such type's share of the landscape's samples with any weight, 0 to 100. */
	TArray<double> TypeCoverage;

	/** One per exported variety, in order. */
	TArray<FMantleGrassEntryDraft> Entries;

	/** Everything left out or approximated, each naming the grass type or variety. */
	TArray<FMantleGrassNote> Warnings;

	/** Grass types the landscape material has, whether or not they got a map channel. */
	int32 TypesInMaterial = 0;

	/** The grass types left out because the target client cannot read grass (FMantleTargetFeatures::DowngradeGrass); empty otherwise. */
	TArray<FString> NeedsNewerClient;

	/** How many map textures the types need: ceil(TypeNames / 4). */
	int32 NumMaps() const { return (TypeNames.Num() + 3) / 4; }
};

/**
 * A landscape's grass as the Terrain entity's grass[] (#628): what Export, the converter and the
 * dependency gatherer share, so the three never disagree.
 *
 * The weights are the engine's own grass maps (the exact result of the landscape material's Landscape
 * Grass Output), built for every component through ULandscapeSubsystem::BuildGrassMaps and assembled
 * into one grid at the landscape's sample resolution. When the engine cannot build them (no renderer),
 * each grass input wired to a Landscape Layer Sample or Layer Weight takes that paint layer's weights.
 * Each grass type's varieties become one entry each, four types to a RGBA map texture stretched over
 * the terrain exactly as the weightmap is.
 */
class MANTLEEDITOR_API FMantleTerrainGrassExport
{
public:
	/**
	 * Engine GrassDensity is instances per 100 square metres (a 10 m x 10 m square: LandscapeGrass.cpp divides
	 * the cm extent by 1000 x 1000, whatever its own comment says); Mantle's density is per square metre.
	 */
	static double DensityPerSquareMetre(float EngineDensity);

	/** "grassmap:" + the landscape's path + ":" + the map's index: the key of that grass map's texture. */
	static FString GrassMapKey(const AActor* Landscape, int32 MapIndex);

	/** The map and channel of the Slot-th exported grass type: Slot / 4 and Slot % 4. */
	static void SlotToMapChannel(int32 Slot, int32& OutMap, int32& OutChannel);

	/** The letter a channel index is written as in the place file: r, g, b, a. */
	static const TCHAR* ChannelName(int32 Channel);

	/**
	 * Reads one variety's numbers into Out (everything but Map, Mesh and Materials): density per m2, the
	 * ScaleX range as the uniform scale (X is what the engine uses for a Uniform variety, and for the
	 * others the one range Mantle can keep), cull distances clamped to the limits, alignToSurface,
	 * randomYaw and castShadow. False, with a warning, for a variety that grows nothing (density 0).
	 * Anything clamped or not kept (a free scale, a weight-attenuated scale, an allowed density range) adds
	 * a warning naming Label.
	 */
	static bool ReadVariety(const FGrassVariety& Variety, const FString& Label, FMantleTerrainGrass& Out, TArray<FMantleGrassNote>& Warnings);

	/**
	 * Adds an entry for each usable variety of Type, in order, as the grass type in map slot Slot, until
	 * FMantlePlaceLimits::MaxTerrainGrass entries are in Out.Entries (the rest are named in Warnings).
	 * Returns how many were added; the caller takes the slot only when it is above zero.
	 */
	static int32 AppendEntries(const ULandscapeGrassType* Type, const FString& TypeName, int32 Slot, FMantleTerrainGrassResult& Out);

	/**
	 * Size.X x Size.Y pixels, row by row from the top, stretched over the whole terrain as
	 * FMantleTerrainExport::MakeWeightmap is (texel centres, the same orientation): R, G, B and A are the
	 * weights of Weights[Indices[0]] to [3] (each Width x Height, INDEX_NONE or a missing grid for 0),
	 * each independent of the others. What FMantlePng::EncodeRGBA8 takes.
	 */
	static TArray<FColor> MakeMap(const TArray<TArray<uint8>>& Weights, TConstArrayView<int32> Indices, int32 Width, int32 Height, FIntPoint Size);

	/**
	 * Reads Landscape's grass: its grass types, their weights over Source's grid (built by the engine
	 * when it can, else from paint layers), and the entries and map slots they make. Logs MANTLE_GRASS_EXPORT
	 * and a MANTLE_GRASS_TYPE line per type when bLog. Builds the engine's grass maps when they are missing,
	 * which marks the landscape's package dirty.
	 */
	static FMantleTerrainGrassResult Gather(const AActor* Landscape, const FMantleTerrainSource& Source, bool bLog);

	/**
	 * Adds the grass meshes (and the material overrides) Gather would export to Dependencies, as a Static
	 * Mesh Actor's are, so they upload with the place. Nothing for a landscape without grass.
	 */
	static void AddAssets(const AActor* Landscape, FMantlePlaceDependencies& Dependencies, const FString& Label);

	/**
	 * Converts the grass maps (one RGBA PNG per four grass types, at most the texture size cap, halved
	 * until it is within the upload size) into Out.Textures under GrassMapKey, with a warning issue for
	 * everything Gather left out. True when nothing went wrong; an error issue and false otherwise.
	 */
	static bool ConvertMaps(const AActor* Landscape, const FMantleTerrainSource& Source, const FMantlePlaceAssetLimits& Limits, FMantlePlaceAssetSet& Out);

	/**
	 * Reports what Gather exported and what it did not to Out under "Landscape grass" (#628): each exported
	 * grass type as kept (approximated when its weights come from a paint layer), each note as it is.
	 */
	static void ReportCoverage(FMantleCoverage& Out, const FMantleTerrainGrassResult& Result);

	/** Draft's numbers with its map's upload id, its mesh's id and its material ids (as StaticMesh writes them) filled in. */
	static FMantleTerrainGrass MakeGrass(const FMantleGrassEntryDraft& Draft, const FString& MapId, const FString& MeshId, const TArray<FString>& MaterialIds);

	/** Writes the Terrain properties' "grass" array (every key of each entry, so the client builds what the editor previewed), nothing when Grass is empty. */
	static void WriteJson(const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>& Writer, const TArray<FMantleTerrainGrass>& Grass);
};
