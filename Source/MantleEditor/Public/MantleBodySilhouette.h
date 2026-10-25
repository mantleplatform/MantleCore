// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/** A body's torso widths and depths (cm) at the chest, waist and hips (FMantleBodySilhouette::Measure). */
struct MANTLEEDITOR_API FMantleSilhouette
{
	static constexpr int32 Bands = 3;
	enum EBand : int32 { Chest = 0, Waist = 1, Hips = 2 };

	/** Side to side and front to back, per band: the torso alone (the narrowest cross-section, so arms and cloaks don't count). */
	double Width[Bands] = { 0.0, 0.0, 0.0 };
	double Depth[Bands] = { 0.0, 0.0, 0.0 };
	/** Which bands had a torso to measure (a band under a robe, with no bare skin, has none); bValid when any did. */
	bool bBand[Bands] = { false, false, false };
	bool bValid = false;

	FString ToString(bool bShareOfHeight = false) const;
};

/**
 * How far to move MetaHuman's Weight, Fat and Muscularity sliders, each as a share of that slider's range (negative is below
 * the default), from "keep the face, choose a body" (#50).
 */
struct MANTLEEDITOR_API FMantleSliderSeed
{
	double Weight = 0.0;
	double Fat = 0.0;
	double Muscularity = 0.0;
	FString ToString() const;
};

/**
 * #50: a kept face's body starts from the character's own build instead of MetaHuman's default. The silhouette of the source
 * (its bind pose) is compared with the default body's, and the ratio seeds the body sliders. Players can still change them. Pure.
 */
class MANTLEEDITOR_API FMantleBodySilhouette
{
public:
	/** The bands, as shares of standing height above the feet: chest, waist, hips. */
	static constexpr double BandHeightShare[FMantleSilhouette::Bands] = { 0.72, 0.60, 0.52 };
	/** A band is measured over slabs this far (share of height) above and below its level; the narrowest slab counts. */
	static constexpr double SlabHalfShare = 0.012;
	/** Vertices further apart than this (share of height) across a slab belong to different parts (an arm apart from the torso). */
	static constexpr double GapShare = 0.025;
	/** The run of vertices that is taken for the torso must be at least this wide (share of height): an arm or a leg alone is not one. */
	static constexpr double MinTorsoWidthShare = 0.10;
	/** How far a seed moves a slider per unit of size difference, and the most it moves one (share of the slider's range). */
	static constexpr double SeedGain = 1.0;
	static constexpr double SeedLimit = 0.5;

	/**
	 * The run of Vertices (Z up) within HalfSlabCm of Z that holds the middle one across X, ending at the first gap wider than GapCm
	 * (an arm apart from the torso): its width, its depth (Y) and its middle. False with fewer than 3 vertices in the slab. Pure.
	 */
	static bool CentralRun(TConstArrayView<FVector3f> Vertices, double Z, double HalfSlabCm, double GapCm, double& OutWidth, double& OutDepth, FVector2d& OutMiddle);

	/**
	 * The silhouette of Vertices (Z up, Y front to back, X side to side, cm) standing FeetZ at its lowest and HeightCm tall.
	 * Out.bBand says which bands had a torso (at least MinTorsoWidthShare wide); false when none did (an empty mesh, no bare skin on
	 * the torso, or a height that is not positive). Pass only vertices that are bare or skin-tight: clothing would measure as body.
	 */
	static bool Measure(TConstArrayView<FVector3f> Vertices, double FeetZ, double HeightCm, FMantleSilhouette& Out);

	/**
	 * The slider moves that take the default body's silhouette to Source's, both measured as shares of their own height (pass
	 * silhouettes already divided by height). Weight follows every band both measured, Fat the waist and hips, Muscularity the chest;
	 * a slider whose bands were not measured on Source stays at 0. Each is clamped to +-SeedLimit of the range, and a build
	 * slimmer than the default never goes above it.
	 */
	static FMantleSliderSeed Seed(const FMantleSilhouette& Source, const FMantleSilhouette& Default);

	/** Silhouette divided by HeightCm: the build as a share of height. */
	static FMantleSilhouette Normalised(const FMantleSilhouette& Silhouette, double HeightCm);
};
