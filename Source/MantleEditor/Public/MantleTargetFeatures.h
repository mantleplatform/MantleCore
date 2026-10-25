// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class FMantleCoverage;
struct FMantlePlaceAssetSet;
struct FMantlePlaceMaterialDocument;
struct FMantleTerrainGrassResult;

/**
 * The gated features (MantleClientFeatures.h, the backend's release flags) the client an export is for reads
 * (#628), and what the export leaves out when that client lacks one.
 *
 * Test Locally targets the client it will launch, which may be a locally built one newer than the released game
 * (it reads more) or today's Steam build (it reads less). The preview export hands the set to the cook tool
 * (--allow-features, ToArgument) so a newer client's keys pass its release check, and downgrades what an older client
 * cannot read instead of failing the export: foliage shading becomes the plain masked, two-sided material, and
 * landscape grass is left out. Each is reported in the coverage report as "needs a newer Mantle client".
 *
 * The default set is every feature this build reads, so a bare export, Publish and everything else that never sets
 * one downgrade nothing: the backend's release gate alone decides what a publish accepts.
 */
struct MANTLEEDITOR_API FMantleTargetFeatures
{
	/** What the coverage report says when a feature is missing from the target. */
	static const TCHAR* const NeedsNewerClient;

	/** Every feature this build reads (MantleClientFeatures::All). */
	FMantleTargetFeatures();

	static FMantleTargetFeatures All();

	/** A client that reads none: an old client's capability answer has no "features". */
	static FMantleTargetFeatures None();

	static FMantleTargetFeatures FromNames(const TArray<FString>& Names);

	/** "all" (any case, among others or alone) is every feature; otherwise the comma-separated names; empty is none. */
	static FMantleTargetFeatures Parse(const FString& List);

	bool Reads(const FString& Feature) const { return Names.Contains(Feature); }

	/** The names, sorted and comma-separated: the value of mantle-cook --allow-features. Empty for none. */
	FString ToArgument() const;

	/** True when a document would write its foliage key (masked, two-sided, not world-aligned or layered). */
	static bool WritesFoliage(const FMantlePlaceMaterialDocument& Document);

	/** True when a document would write alphaMode additive (#628). */
	static bool WritesAdditive(const FMantlePlaceMaterialDocument& Document);

	/**
	 * Without materialFidelity, takes foliage shading off Document so it is written as the plain masked, two-sided
	 * material without the foliage key, and water (#628) so it is written as the opaque material near-opaque water
	 * published as before the water key: its fastest normal map panned (uvTransforms, uvPanning), no second normal.
	 * Without materialAdditive, takes alphaMode additive off Document (#628) so it is written as the translucent material an older
	 * client reads: a black base colour, the glow as emissive and an opacity of clamp(luminance of the emissive factor x its
	 * strength, 0, 1). True when that changed what the document writes.
	 */
	bool DowngradeMaterial(FMantlePlaceMaterialDocument& Document) const;

	/**
	 * Without terrainGrass, empties Result of its entries, maps and approximations and names the grass types in
	 * Result.NeedsNewerClient for the coverage report. True when there was grass to leave out.
	 */
	bool DowngradeGrass(FMantleTerrainGrassResult& Result) const;

	/** One Approximated line under Materials for each converted material whose foliage shading, water key or additive blending DowngradeMaterial takes off. */
	void ReportMaterialDowngrades(const FMantlePlaceAssetSet& Set, FMantleCoverage& Out) const;

	/** The target of the export in progress on the game thread (FMantleTargetFeaturesScope); every feature this build reads outside one. */
	static const FMantleTargetFeatures& Current();

	TSet<FString> Names;
};

/** Makes Features the target (FMantleTargetFeatures::Current) for its lifetime, restoring the previous one after. Game thread only. */
struct MANTLEEDITOR_API FMantleTargetFeaturesScope
{
	explicit FMantleTargetFeaturesScope(const FMantleTargetFeatures& Features);
	~FMantleTargetFeaturesScope();
	FMantleTargetFeaturesScope(const FMantleTargetFeaturesScope&) = delete;
	FMantleTargetFeaturesScope& operator=(const FMantleTargetFeaturesScope&) = delete;

private:
	FMantleTargetFeatures Held;
	const FMantleTargetFeatures* Previous = nullptr;
};
