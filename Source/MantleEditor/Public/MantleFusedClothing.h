// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

/** What a skin-looking slot turns out to hold (FMantleFusedClothing::Check). */
enum class EMantleFusedVerdict : uint8
{
	/** Nothing in its shape or name says clothing is part of it. */
	Bare,
	/** Clothing is modelled into it (a second layer just under its surface, or a material named for clothing). */
	FusedClothing,
	/** Too little geometry to tell. */
	Unknown,
};

/** One slot's triangles, as the checks read them (cm, Z up). */
struct MANTLEEDITOR_API FMantleFusedSlot
{
	FString SlotName;
	TConstArrayView<FVector3f> Vertices;
	/** Three indices into Vertices per triangle. */
	TConstArrayView<int32> Indices;
};

/** A slot's verdict and, in plain words, why. */
struct MANTLEEDITOR_API FMantleFusedResult
{
	EMantleFusedVerdict Verdict = EMantleFusedVerdict::Unknown;
	FString Reason;
	/** The share of the sampled surface that had a second layer (0 to 1); 0 when the shape was not tested (a clothing name, too little geometry). */
	double Layered = 0.0;
};

/**
 * #50 / #55: whether a slot that reads as bare skin has clothing built into it (Sophie and Belica: one skin-named slot for the whole
 * body, jacket and boots modelled in). Pure: triangle arrays in, a verdict and a plain reason out. The checks:
 *   - the slot's name: skin that is named for clothing (boots, jacket, armour, belt ...) is FusedClothing;
 *   - its shape: from the outside surface, a ray going straight in meets a second surface facing outward within MaxLayerShare of
 *     the character's height (cloth lying over a body shell inside the same slot). A plain body's ray crosses empty inside and
 *     meets the far side, facing away. When LayeredShare of the sampled surface has that second layer, the slot is FusedClothing.
 * Unknown: fewer than MinTriangles triangles, or no usable surface.
 */
class MANTLEEDITOR_API FMantleFusedClothing
{
public:
	static constexpr int32 MinTriangles = 200;
	/** The most triangles sampled for the shape test (spread evenly through the slot). */
	static constexpr int32 MaxSamples = 400;
	/** A second layer counts when it lies this close under the surface (share of height), and no closer than MinLayerCm. */
	static constexpr double MaxLayerShare = 0.03;
	static constexpr double MinLayerCm = 0.15;
	/** The share of the sampled surface that must have a second layer. */
	static constexpr double LayeredShare = 0.15;

	/** A slot name that names clothing: boot, shoe, jacket, coat, shirt, pant, trouser, dress, skirt, armor, armour, belt, glove, vest, cloth, robe, cape, helmet. Pure. */
	static bool NamesClothing(const FString& SlotName);

	/** The verdict for one slot; HeightCm is the character's standing height (cm, sets how close a layer must lie). */
	static FMantleFusedResult CheckSlot(const FMantleFusedSlot& Slot, double HeightCm);

	/** One verdict per slot, in order. */
	static TArray<FMantleFusedResult> Check(TConstArrayView<FMantleFusedSlot> Slots, double HeightCm);
};
