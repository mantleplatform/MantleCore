// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UMaterialExpression;
class UMaterialInterface;
class UTexture;
struct FExpressionInput;

/**
 * A material's emissive, worked out on the CPU from its own graph at a point of the mesh (its UV sets there), as the GPU would draw it: the nodes the material
 * uses, its instance's parameters, its textures sampled as the sampler reads them (an sRGB texture decoded, a mask's channel raw). This is how the light finder reads
 * a lamp whose zones (red plastic, the red light, the orange signal, the white reverse light, chrome) are chosen by a mask on a second UV set through If and Lerp nodes
 * (Leartes' MM_Lights02 is one such material; nothing here names it or its numbers).
 *
 * Only a graph made of the nodes it knows is read (constants, parameters, texture samples and coordinates, arithmetic, Lerp, If, masks, Append, Clamp, Power,
 * reroutes); any other node (a material function, a switch, anything view- or time-dependent) and Prepare says which, so the caller keeps its own reading.
 */
struct MANTLEEDITOR_API FMantleEmissiveEval
{
	FMantleEmissiveEval();
	~FMantleEmissiveEval();

	/**
	 * Reads Material's emissive graph. SwitchedOn names scalar parameters that switch the glow (FMantleMaterialGlow::OffScalars): one at 0 is read as 1, the lamp lit.
	 * False, with Why in plain words, when the graph has a node this does not evaluate or a texture whose pixels cannot be read.
	 */
	bool Prepare(const UMaterialInterface* Material, const TArray<FName>& SwitchedOn, FString& OutWhy);

	/**
	 * Whether Material's emissive reads UV set Index, looked up without reading any texture (cheap: for every material on a body). Stops at a node Prepare would not
	 * evaluate, so a graph it cannot work out may say false here.
	 */
	static bool ReadsUV(const UMaterialInterface* Material, int32 Index);

	/** Whether the graph reads UV set Index (a Texture Coordinate node, or a texture sample's own coordinate index). Known after Prepare, even when it failed. */
	bool UsesUV(int32 Index) const;

	/** The emissive color at a point whose UV sets are UVs (UVs[0] the first). A set the mesh does not have reads as (0, 0). Prepare must have succeeded. */
	FLinearColor At(TConstArrayView<FVector2f> UVs) const;

	/** The node's class name that stopped Prepare; empty when it succeeded or stopped for another reason. */
	FString Unsupported;

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};
