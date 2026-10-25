// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantleGlbWriter.h"
#include "UObject/StrongObjectPtr.h"

class UChaosOutfitAsset;
class UMaterialInterface;
class USkeletalMesh;

/** One authored representation of the same garment, paired with the body/head it actually fits. */
struct MANTLEEDITOR_API FMantleOutfitAuthoredSource
{
	FString Id;
	TWeakObjectPtr<USkeletalMesh> Garment;
	/** Epic's merged authoring body/head with its source MeshDescription, not a runtime body proxy. */
	TWeakObjectPtr<USkeletalMesh> CombinedBodyHead;

	/**
	 * Instead of Garment: the garment's render geometry made in memory (FMantleGarmentOutfit), one LOD, skinned to
	 * CombinedBodyHead's bones (its joints are named as that mesh's reference skeleton names them), and Materials[i] the
	 * material of its glTF material i. The cloth takes CombinedBodyHead as its skeletal mesh.
	 */
	TSharedPtr<const FMantleGlbMesh> Geometry;
	TArray<UMaterialInterface*> Materials;

	/** When set, only the Garment's sections drawn with one of these material slots go into the Outfit (a Garment's kept sections). */
	TOptional<TArray<int32>> KeepSlots;
};

/** Render-only native Outfit authoring. Original meshes and their materials are read without modification. */
struct MANTLEEDITOR_API FMantleParametricOutfitAuthoring
{
	/**
	 * Wrap one or more authored skeletal garment sizes in native Cloth Assets and one sized Outfit.
	 * Generates Epic's source RBF data; it does not manufacture additional artist-authored sizes.
	 * The returned asset is input to the native resizing pipeline, not a runtime payload.
	 * KeepAlive must outlive the returned asset's use. A package Outer permits the caller to save it.
	 */
	static UChaosOutfitAsset* Create(TConstArrayView<FMantleOutfitAuthoredSource> Sources, UObject* Outer,
		FName Name, int32 InterpolationPoints, TArray<TStrongObjectPtr<UObject>>& KeepAlive, FString& Error);
};
