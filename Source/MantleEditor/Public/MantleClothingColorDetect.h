// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleClothingColor.h"

class UMaterialInterface;
class UTexture;
struct FMantleGlbMesh;

/** #756: how one channel of a candidate area map scored over the texels its clothing material actually uses. */
struct MANTLEEDITOR_API FMantleAreaChannelScore
{
	/** 0 red, 1 green, 2 blue: of the source map, not of the clean map the upload carries. */
	uint8 Channel = 0;
	/** Share of used texels near 0 or near 1 (below LowThreshold or above HighThreshold). */
	float Binary = 0.0f;
	/** Share of used texels above one half. */
	float Coverage = 0.0f;
	/** Mid-tone texels per on/off crossing between neighbours: about 0 to 2 for a hard edge, many for a smooth ramp. */
	float EdgeWidth = 0.0f;
	bool bAccepted = false;
};

/**
 * #756: the creator's answer for one area found on a clothing material. Areas without a choice are kept with their
 * default name, so a creator (or a draft built in code) who changes nothing gets every area found.
 */
struct MANTLEEDITOR_API FMantleClothingColorChoice
{
	FString MaterialName;
	FString RegionId;
	/** Unticked: the area keeps its original colours and players cannot recolour it. */
	bool bEnabled = true;
	/** Empty keeps the default name ("Main", "Accent 1"). */
	FString Label;
};

/** #756: what detection found on one exported clothing material. */
struct MANTLEEDITOR_API FMantleClothingColorDetection
{
	FString MaterialName;
	/** The material's texture parameter the candidate map came from ("Mask", "Masks"); empty when it has none. */
	FString Parameter;
	FString TextureName;
	TArray<FMantleAreaChannelScore> Scores;
	/** The source channels taken, in order: the first becomes the clean map's green (Accent), the second its blue. */
	TArray<uint8> SourceChannels;
	/**
	 * Every area found, before the creator's choices: Main on the clean map's red (covered by no accepted channel),
	 * then one Accent per accepted channel on green and blue. Ids and default names are set; DefaultColor is the area's
	 * average authored colour once measured at export; ReferenceLuminance is measured only for the regions declared.
	 */
	TArray<FMantleClothingColorRegion> Areas;
	/** Why nothing was found, in plain words for the creator; empty when areas were found. */
	FString Why;
};

/**
 * #756: finds the areas of a clothing item players can recolour, from a mask texture its material already carries,
 * so a creator never has to author one. A channel of the map is an area when it is near-binary, hard-edged and covers a
 * real share (but not all) of the texels the material uses; smooth greyscale (shading, occlusion, detail) never is.
 * Three accepted channels look like a many-colour ID map, which v1 does not split: nothing is found.
 *
 * The accepted channels are repacked into the clean linear map FMantleClothingColorMeasure and the runtime read:
 * red is Main (covered by no accepted channel), green the first accepted channel, blue the second; the three sum to
 * one per texel. The source map's own channel order is never uploaded.
 */
class MANTLEEDITOR_API FMantleClothingColorDetect
{
public:
	static constexpr float LowThreshold = 0.1f;
	static constexpr float HighThreshold = 0.9f;
	static constexpr float MinBinary = 0.9f;
	static constexpr float MinCoverage = 0.01f;
	static constexpr float MaxCoverage = 0.99f;
	static constexpr float MaxEdgeWidth = 4.0f;

	/** A texture parameter name that suggests an area map: Mask, Masks, ID, Region, ColorMask, ... (any case, spaces and underscores ignored). Pure. */
	static bool IsMaskParameterName(const FString& Name);

	/**
	 * A texture parameter name that might hold an area map under a seller's own naming: anything with "mask" in it
	 * ("Mask_01", "PatternMask", "Albedo_RGB_Mask", "Masks 01to04") or ending in "id", never opacity, alpha, wrinkle,
	 * animated or body-hiding maps. The map's content (Choose) decides; the name only picks what to try. Pure.
	 */
	static bool IsLooseMaskParameterName(const FString& Name);

	/**
	 * Every texture worth trying as an area map, with its parameter (or, for a texture the material samples directly, its
	 * asset name): IsMaskParameterName parameters first, then loose ones, then used textures named like a mask.
	 */
	static TArray<TPair<FString, UTexture*>> FindMaskTextures(UMaterialInterface* Material);

	/**
	 * The fallback when no map holds separate areas: the whole material is one Main area (clean map all red over its used
	 * texels), so a plain or tinted garment can still be recoloured. False only when the mesh's UVs cannot be read.
	 */
	static bool DetectWhole(const FMantleGlbMesh& Mesh, int32 MaterialIndex, FIntPoint Size, FMantleClothingColorDetection& InOut,
		TArray<FColor>& OutCleanMask, TArray<uint8>& OutUsed, int32& MainNumber, FString& OutError);

	/** Scores one channel of Pixels (Size, linear) over the texels Used marks. Pure. */
	static FMantleAreaChannelScore ScoreChannel(TConstArrayView<FColor> Pixels, TConstArrayView<uint8> Used, FIntPoint Size, uint8 Channel);

	/**
	 * Scores red, green and blue and picks the channels taken: InOut's Scores, SourceChannels and (when none) Why. A channel
	 * marking the same texels as one already taken (a greyscale map) is not taken again. Pure.
	 */
	static void Choose(TConstArrayView<FColor> Pixels, TConstArrayView<uint8> Used, FIntPoint Size, FMantleClothingColorDetection& InOut);

	/** The clean map for SourceChannels (at most two): red Main, green the first, blue the second, alpha opaque. Pure. */
	static TArray<FColor> Repack(TConstArrayView<FColor> Pixels, TConstArrayView<uint8> SourceChannels);

	/**
	 * Detection over one material of Mesh: its used texels on UV0 (FMantleClothingColorMeasure::MarkUsedTexels), Choose,
	 * Repack, and the areas named. Main is left out when the accents cover nearly everything. MainNumber and AccentNumber
	 * carry the item's numbering across materials ("main", "main_2"; "accent_1", "accent_2", ...). False only when the
	 * mesh's UVs cannot be read (OutError); an item with nothing found returns true with no areas and Why set.
	 */
	static bool Detect(const FMantleGlbMesh& Mesh, int32 MaterialIndex, FIntPoint Size, TConstArrayView<FColor> SourceMask,
		FMantleClothingColorDetection& InOut, TArray<FColor>& OutCleanMask, TArray<uint8>& OutUsed,
		int32& MainNumber, int32& AccentNumber, FString& OutError);

	/** The regions to declare: Detection's areas less the unticked ones, with the creator's names. Pure. */
	static TArray<FMantleClothingColorRegion> ApplyChoices(const FMantleClothingColorDetection& Detection, TConstArrayView<FMantleClothingColorChoice> Choices);

	/** The average authored colour (sRGB, opaque) of Albedo weighted by Mask's Channel over Used; mid-grey when it has no weight. Pure. */
	static FColor AverageColor(TConstArrayView<FColor> Albedo, TConstArrayView<FColor> Mask, TConstArrayView<uint8> Used, uint8 Channel);

	/** A distinct, saturated colour per found area (by its order in the item) for the preview's highlight. */
	static FColor HighlightColor(int32 AreaIndex);
};
