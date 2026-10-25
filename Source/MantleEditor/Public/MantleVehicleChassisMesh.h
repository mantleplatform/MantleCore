// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class USkeletalMesh;
struct FMantleSkinnedMesh;
struct FSkeletalMaterial;

/**
 * What a creator's model brings to a body beyond its triangles and its first UV set (#821): its other UV channels and its painted vertex colours, each one entry per vertex of
 * the body, in the body's vertex order. A channel or colour list of any other length is left out. The mesh keeps up to the skeletal mesh limit (MAX_TEXCOORDS, 4 channels in all).
 */
struct FMantleVehicleBodyExtras
{
	/** UV channels 1 and up. */
	TArray<TArray<FVector2f>> UVs;
	/** Vertex colours; empty for none. */
	TArray<FColor> Colors;

	bool IsEmpty() const { return UVs.Num() == 0 && Colors.Num() == 0; }
};

/**
 * A chassis's grey body (FMantleVehicleChassisBuilder::MakeAsset) as a saved skeletal mesh asset (#793 step 4), so a vehicle shaped in the
 * Build a vehicle window is an ordinary vehicle asset: Put it in my level, Test Locally and Publish all read its Body the way they read a
 * creator's model, and no part of them knows the vehicle was never modelled.
 *
 * Two assets, SKM_<Name> (the mesh: a root bone, a bone at each wheel centre, the grey box and a cylinder on each wheel bone, one material
 * slot left empty so it draws Mantle's default surface) and SK_<Name> (its skeleton), the same recipe the weapon builder's rigid mesh uses.
 */
struct MANTLEEDITOR_API FMantleVehicleChassisMesh
{
	/** The names of the two assets Create makes for Name. */
	static FString MeshAssetName(const FString& Name) { return TEXT("SKM_") + Name; }
	static FString SkeletonAssetName(const FString& Name) { return TEXT("SK_") + Name; }

	/** True for a throwaway package (a test's, under /Temp): nothing is registered, deleted or written to disk for it. */
	static bool IsScratchPath(const FString& PackagePath) { return PackagePath.StartsWith(TEXT("/Temp")); }

	/**
	 * Makes the mesh for Body (LOD 0 of an FMantleSkinnedMesh with a reference pose) under PackagePath ("/Game/MantleVehicles/Sedan") and saves
	 * it with its skeleton. A mesh and skeleton already there under these names with the same bones are written into in place, the same objects, so
	 * shaping a vehicle again deletes nothing and what names the mesh keeps it; only ones that cannot be reused (other bones, an old layout) are
	 * deleted and made again. Null, with the reason in OutError as a clause for a sentence (no capital, no full stop), when the body is not valid,
	 * a name is not usable, or a file cannot be saved. Game thread.
	 *
	 * ModelMaterials (#821, chassis from a model): a creator's model's material slots, which the body's sections name by MaterialIndex; the mesh then has those
	 * slots, a polygon group for each. Null (the grey chassis) leaves one empty slot named after the first section.
	 *
	 * The body's own tangents (a creator's model has them) are kept: the build recomputes tangents from the UVs only for a body that has none (the grey chassis). Extras
	 * (the model's other UV channels and vertex colours) are written beside them.
	 */
	static USkeletalMesh* Create(const FMantleSkinnedMesh& Body, const FString& PackagePath, const FString& Name, FString& OutError,
		const TArray<FSkeletalMaterial>* ModelMaterials = nullptr, const FMantleVehicleBodyExtras* Extras = nullptr);

	/** True when Body's LOD 0 has a usable tangent (a unit-ish direction and a sign) for every vertex: the build then keeps them instead of working them out from the UVs. */
	static bool HasUsableTangents(const FMantleSkinnedMesh& Body);
};
