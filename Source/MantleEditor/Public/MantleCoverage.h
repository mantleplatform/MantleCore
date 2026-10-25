// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/**
 * What happened to one piece of a level when it was exported (#628): kept exactly, approximated or baked, or
 * dropped. Info is for content that has nothing to export (an editor-only generator whose results are exported
 * on their own): it changes nothing in the place, so it is neither a drop nor a loss.
 */
enum class EMantleCoverageOutcome : uint8
{
	Kept,
	Approximated,
	Dropped,
	Info
};

/** The categories the export reports on, spelled once. Any other string is a category too: a new one appears in the report when something adds to it. */
namespace MantleCoverageCategory
{
	inline const TCHAR* const StaticMeshes = TEXT("Static meshes");
	inline const TCHAR* const SkeletalMeshes = TEXT("Skeletal meshes");
	inline const TCHAR* const InstancedMeshes = TEXT("Instanced meshes");
	inline const TCHAR* const PaintedFoliage = TEXT("Painted foliage");
	inline const TCHAR* const Materials = TEXT("Materials");
	inline const TCHAR* const Terrain = TEXT("Terrain");
	inline const TCHAR* const TerrainLayers = TEXT("Terrain layers");
	inline const TCHAR* const LandscapeMaterial = TEXT("Landscape material");
	inline const TCHAR* const LandscapeGrass = TEXT("Landscape grass");
	inline const TCHAR* const WaterCarve = TEXT("Water carve");
	inline const TCHAR* const SplineMeshes = TEXT("Spline meshes");
	inline const TCHAR* const Water = TEXT("Water");
	inline const TCHAR* const Lights = TEXT("Lights");
	inline const TCHAR* const Sky = TEXT("Sky");
	inline const TCHAR* const Fog = TEXT("Fog");
	inline const TCHAR* const PostProcess = TEXT("Post-process");
	inline const TCHAR* const Volumes = TEXT("Volumes");
	inline const TCHAR* const Particles = TEXT("Particles");
	inline const TCHAR* const Decals = TEXT("Decals");
	inline const TCHAR* const Sounds = TEXT("Sounds");
	inline const TCHAR* const Text = TEXT("World text");
	inline const TCHAR* const SubLevels = TEXT("Sub-levels");
	inline const TCHAR* const BlueprintLogic = TEXT("Blueprint logic");
	inline const TCHAR* const OtherComponents = TEXT("Other components");
	inline const TCHAR* const OtherActors = TEXT("Other actors");
	inline const TCHAR* const GameplayActors = TEXT("Groups and gameplay actors");
}

/** One thing a bucket names: an actor or asset, why, and how many of it (a bulk Add of the same name and detail merges into one). */
struct MANTLEEDITOR_API FMantleCoverageExample
{
	FString Name;
	FString Detail;
	int64 Count = 1;
};

/** Every thing of one category that had one outcome: how many, and the first few by name. */
struct MANTLEEDITOR_API FMantleCoverageBucket
{
	int64 Count = 0;

	/** At most FMantleCoverage::MaxExamples distinct (name, detail) pairs, in the order they were added. */
	TArray<FMantleCoverageExample> Examples;
};

/** One row of the report. */
struct MANTLEEDITOR_API FMantleCoverageCategory
{
	FString Name;
	FMantleCoverageBucket Buckets[4];

	const FMantleCoverageBucket& Get(EMantleCoverageOutcome Outcome) const { return Buckets[static_cast<int32>(Outcome)]; }
	int64 Count(EMantleCoverageOutcome Outcome) const { return Get(Outcome).Count; }
	bool IsEmpty() const;
};

/**
 * The export coverage report (#628): for each category, what was kept exactly, what was approximated or baked,
 * and what was dropped, with counts and up to MaxExamples names a line. "Nothing is dropped without a line here"
 * is the rule it enforces.
 *
 * Plain data, built on the main thread. FMantlePlaceExporter::Export fills FMantleExportResult::Coverage:
 * FMantleCoverageSurvey::Run walks the level and the exported place and reports what it finds. Export code that
 * knows better than the survey (a spline mesh baked into a static mesh, a terrain layer that did not fit, a grass
 * type exported) reports it itself, with `Result.Coverage.Add(Category, Outcome, Name, Detail)`, and passes the
 * FMantleCoverage down to helpers that need to. The survey only fills what nobody reported (see
 * FMantleCoverageSurvey), so reporting a category yourself never doubles it.
 *
 * Consumers: Test Locally writes it as coverage.json beside the manifest and shows it in the Mantle Preview
 * panel, Publish lists it in its log, and all of them log it (Log).
 */
class MANTLEEDITOR_API FMantleCoverage
{
public:
	/** The names kept per bucket; the count always covers all of them. */
	static constexpr int32 MaxExamples = 10;

	/**
	 * Records Count (default 1) of Name under Category with Outcome. Name is the actor or asset, Detail the
	 * reason or the loss in plain words; both may be empty (a bulk count with no name). The same name and
	 * detail in the same bucket merge into one example whose count adds up.
	 */
	void Add(const FString& Category, EMantleCoverageOutcome Outcome, const FString& Name, const FString& Detail = FString(), int64 Count = 1);

	/** Adds everything in Other, as if it had been added here. */
	void Append(const FMantleCoverage& Other);

	/** The count of Category's Outcome, 0 for a category nothing reported. */
	int64 Count(const FString& Category, EMantleCoverageOutcome Outcome) const;

	/** All of Outcome, across the categories. */
	int64 Total(EMantleCoverageOutcome Outcome) const;

	/** True when anything at all was reported to Category, whatever the outcome. */
	bool HasCategory(const FString& Category) const;

	const FMantleCoverageCategory* Find(const FString& Category) const;
	const TArray<FMantleCoverageCategory>& GetCategories() const { return Categories; }
	bool IsEmpty() const { return Categories.Num() == 0; }
	void Reset() { Categories.Reset(); }

	/**
	 * Puts the categories in the report's own order (the ones MantleCoverageCategory names, in that order,
	 * then any other in the order it was added), so two exports of one level read the same.
	 */
	void Sort();

	/** "kept", "approx", "dropped" or "info": the key the log lines and coverage.json use. */
	static const TCHAR* OutcomeKey(EMantleCoverageOutcome Outcome);

	/** "kept", "approximated", "dropped" or "nothing to export": for people. */
	static const TCHAR* OutcomeWord(EMantleCoverageOutcome Outcome);

	/** The category as one token for a log line: lower case, runs of anything but letters and digits as a hyphen. */
	static FString Slug(const FString& Category);

	/**
	 * The lines Log writes, without the log prefix. One `MANTLE_COVERAGE <category> kept=N approx=N dropped=N`
	 * per category (` info=N` appended only when above zero), then one `MANTLE_COVERAGE_ITEM <category> <outcome>
	 * <name> [xN]: <detail>` per named example of anything but a plain kept bucket's. Categories with nothing in
	 * them are left out.
	 */
	TArray<FString> ToLogLines() const;

	/** Writes ToLogLines to the log (LogMantle, Display). */
	void Log() const;

	/**
	 * coverage.json: {"schemaVersion":1, "totals":{kept,approx,dropped,info}, "categories":[{"category", and for each
	 * outcome key an object {"count", "items":[{"name","detail","count"}]}}]}, every outcome present.
	 */
	FString ToJson() const;

	/**
	 * The report for the Mantle Preview panel and the publish log: one line per category ("Static meshes: 157 kept,
	 * 3 dropped") and, indented under it, the names of what was approximated, dropped or not exported, each with its
	 * detail. Categories with nothing in them are left out.
	 */
	TArray<FString> ToDisplayLines() const;

	/** "3 dropped, 12 approximated" (the outcomes with something in them), or "nothing dropped" when only kept or info. */
	FString ToSummary() const;

private:
	FMantleCoverageCategory& FindOrAdd(const FString& Category);

	TArray<FMantleCoverageCategory> Categories;
};
