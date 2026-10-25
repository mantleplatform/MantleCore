// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class AActor;
class FMantleCoverage;
class UActorComponent;
class UWorld;
struct FMantleExportResult;
struct FMantlePlaceDependencies;
struct FMantleTerrainLayerChoice;
struct FMantleTerrainSource;

/**
 * Fills FMantleExportResult::Coverage (#628): walks the level and the place Export wrote, and reports every
 * piece of the level by what became of it, so nothing is dropped without a line in the report. Export calls Run
 * last, whether or not it found errors (a refused export still says what it would have lost).
 *
 * It reads what Export read, through the same predicates (IsExportedActor, DescribeUnpublishedContent,
 * DescribeUnpublishedParts, CollectInstanceSources, FMantlePlaceAssets::AnalyzeMaterial...), so the two agree.
 * Export code that handles something better than the survey assumes (a spline mesh baked into a static mesh, a
 * grass type exported) reports it with `Result.Coverage.Add(...)` as it goes. The survey adds a default only
 * where nobody has: landscape material, landscape grass, and a spline mesh whose actor Export still refuses.
 */
class MANTLEEDITOR_API FMantleCoverageSurvey
{
public:
	/** Adds what the level and Result.Place show to Result.Coverage, then sorts it into report order. */
	static void Run(UWorld* World, FMantleExportResult& Result);

	/**
	 * Adds the project materials in Dependencies (a Fab pack's, not Mantle's library ones) to Out as recognised, baked or lost,
	 * by what FMantlePlaceAssets::AnalyzeMaterial finds in each, and sorts Out. Export does not do this itself: it would gather
	 * the level's dependencies a second time, a landscape's grass maps among them. Whoever gathered them for the upload (Test
	 * Locally's export, Publish) calls it with what it has.
	 */
	static void AddMaterials(const FMantlePlaceDependencies& Dependencies, FMantleCoverage& Out);

	/**
	 * Volumes that only drive editor-time generation (a Procedural Foliage Volume and its Blocking Volume): what they
	 * generate, the painted foliage, is exported on its own, so leaving them out of the place changes nothing. Matched by
	 * class name, so MantleEditor needs no dependency on the foliage editor module for it.
	 */
	static bool IsEditorGeneratorVolume(const AActor* Actor);

	/** The report category for a component Export leaves out: particles, decals, sounds, lights, static meshes, else "Other components". */
	static FString ComponentCategory(const UActorComponent* Component);

	/** One LandscapeGrassType a landscape's material grows, as the report names it. */
	struct FGrassType
	{
		/** The asset's name. */
		FString Name;

		/** Its varieties as "mesh (density per 10 m2)", in plain words. */
		FString Varieties;
	};

	/**
	 * Every distinct LandscapeGrassType the landscape's material (and each streaming proxy's) references through its
	 * Landscape Grass Output, sorted by name. A grass input with no grass type chosen grows nothing and is left out.
	 */
	static TArray<FGrassType> FindLandscapeGrassTypes(const AActor* Landscape);

	/**
	 * Reports the layers Export publishes for a landscape (Kept, each named, with its material and tiling) and any
	 * painted layer of Source it does not publish (Dropped). Export calls it once ChooseLayers has chosen. It does
	 * nothing when "Terrain layers" has been reported already (ChooseLayers may report them itself).
	 */
	static void ReportTerrainLayers(FMantleCoverage& Out, const FString& Label, const FMantleTerrainSource& Source,
		const TArray<FMantleTerrainLayerChoice>& Layers);
};
