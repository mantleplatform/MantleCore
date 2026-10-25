// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MantleSurface.h"
#include "MantleTerrainSurface.generated.h"

struct FMantleDecodedTexture;
struct FMantleHeightfield;
struct FMantleTerrainProps;

/**
 * Which paint layer weighs most in each texel of a Terrain's weightmap (#596 step 1b): the CPU copy
 * the surface lookup reads, one byte a texel, on the server as well as every client (the weightmap
 * texture itself is only built where something renders, and its pixels then live on the GPU).
 *
 * Built from the decoded weightmap (its R, G, B and A are layers 0 to 3, FMantleTerrainBuilder's
 * convention); the heaviest channel wins, the lower layer on a tie. The texture's texels span the
 * terrain as UV0 does, 0 at its first sample and 1 at its last, a texel's centre at (i + 0.5) / size.
 * A second weightmap holds layers 4 to 7 the same way; HeaviestLayerOf weighs the two maps' heaviest.
 */
struct MANTLERUNTIME_API FMantleTerrainWeights
{
	int32 Width = 0;
	int32 Height = 0;

	/** Width x Height, row-major: the heaviest layer's index, 0 to 3. */
	TArray<uint8> Dominant;

	/** Width x Height, row-major: the weight (0 to 255) of Dominant's layer, what a second weightmap's heaviest is weighed against. */
	TArray<uint8> DominantWeight;

	/**
	 * Width x Height x 4 bytes, row-major, R, G, B, A per texel: the weightmap's own pixels. Kept only when
	 * asked for (a Terrain's grass map, whose channels are densities); empty otherwise.
	 */
	TArray<uint8> Rgba;

	bool IsValid() const { return Width > 0 && Height > 0 && Dominant.Num() == Width * Height && DominantWeight.Num() == Dominant.Num(); }

	/** Rgba holds every texel's four channels: SampleChannel has something to read. */
	bool HasPixels() const { return IsValid() && Rgba.Num() == Width * Height * 4; }

	/** From Width x Height pixels (R, G, B, A weigh layers 0 to 3). False, and Out empty, when the size is wrong. bKeepPixels also fills Rgba. */
	static bool FromPixels(int32 InWidth, int32 InHeight, TConstArrayView<FColor> Pixels, FMantleTerrainWeights& Out, bool bKeepPixels = false);

	/**
	 * From a decoded weightmap's top mip, which must be uncompressed BGRA8 (a weightmap is uploaded as a
	 * PNG and the cook keeps it lossless). False, with the reason, for anything else. bKeepPixels also
	 * fills Rgba.
	 */
	static bool FromDecodedTexture(const FMantleDecodedTexture& Texture, FMantleTerrainWeights& Out, FString* OutError = nullptr,
		bool bKeepPixels = false);

	/** The heaviest layer at (U, V), each 0 to 1 across the terrain and clamped to it; 0 when empty. */
	int32 DominantLayerAt(double U, double V) const;

	/** The weight (0 to 255) of DominantLayerAt (U, V); 0 when empty. */
	int32 DominantWeightAt(double U, double V) const;

	/**
	 * The heaviest of up to eight layers at (U, V): First's heaviest of layers 0 to 3, or Second's of
	 * layers 4 to 7 (returned as 4 to 7) when it weighs more, the lower layer on a tie. Second may be null
	 * or empty (a terrain with up to four layers); First empty reads as layer 0.
	 */
	static int32 HeaviestLayerOf(const FMantleTerrainWeights& First, const FMantleTerrainWeights* Second, double U, double V);

	/**
	 * One channel (0 R, 1 G, 2 B, 3 A) at (U, V), 0 to 1, read bilinearly the way the GPU filters the same
	 * texture as the terrain draws it (texel centres at (i + 0.5) / size, clamped at the edge). 0 without pixels.
	 * Pure and thread-safe: grass generation reads it from worker threads.
	 */
	double SampleChannel(int32 Channel, double U, double V) const;

	SIZE_T GetAllocatedBytes() const { return Dominant.GetAllocatedSize() + DominantWeight.GetAllocatedSize() + Rgba.GetAllocatedSize(); }
};

/**
 * The surfaces of a built Terrain (#596 step 1b), on its actor: what MantleSurfaceFromHit asks when a
 * hit lands on the terrain's chunks, which carry no surface of their own. The place builder adds it
 * on the server and every client, wherever a layer names a surface other than default; nothing of it
 * replicates, since each machine builds the same terrain from the same place.
 */
UCLASS(ClassGroup = Mantle, NotBlueprintable, Transient)
class MANTLERUNTIME_API UMantleTerrainSurfaceComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UMantleTerrainSurfaceComponent();

	/** A layer of Props names a surface other than default: only then does a terrain get this component. */
	static bool HasSurfaces(const FMantleTerrainProps& Props);

	/** HasSurfaces, on more than one layer with a weightmap to tell them apart: the weights are worth reading. */
	static bool NeedsWeights(const FMantleTerrainProps& Props);

	/**
	 * The terrain's extent from its heightfield (samples times spacing), each layer's surface in the
	 * place's order, and the weightmap's heaviest layers. Weights may be null (a single layer, or a
	 * weightmap that could not be read), and then every hit is layer 0's. Weights2 is the second
	 * weightmap, layers 4 to 7; null for a terrain with up to four layers or one that could not be read.
	 */
	void Init(const FMantleHeightfield& Field, TArray<EMantleSurface> InLayerSurfaces, TSharedPtr<const FMantleTerrainWeights> InWeights,
		TSharedPtr<const FMantleTerrainWeights> InWeights2 = nullptr);

	/** The surface at a point in the terrain's own space, cm from heightfield sample (0, 0); height is ignored. */
	EMantleSurface SurfaceAtLocal(double X, double Y) const;

	/** The surface under a world location: its layer that weighs most there. */
	EMantleSurface SurfaceAt(const FVector& WorldLocation) const;

	bool HasWeights() const { return Weights.IsValid() && Weights->IsValid(); }

private:
	/** Not UPROPERTYs: EMantleSurface is a plain enum, and none of this is saved or referenced by the GC. */
	TArray<EMantleSurface> LayerSurfaces;
	TSharedPtr<const FMantleTerrainWeights> Weights;
	TSharedPtr<const FMantleTerrainWeights> Weights2;

	/** cm the terrain spans along X and Y, first sample to last. */
	FVector2D ExtentCm = FVector2D::ZeroVector;
};
