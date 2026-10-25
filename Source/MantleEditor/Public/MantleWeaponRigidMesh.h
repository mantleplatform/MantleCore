// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Misc/Optional.h"

class USkeletalMesh;
class USkeleton;

/** One mesh baked rigidly into a weapon's skeletal mesh: a static or skeletal mesh (LOD0, reference pose) and where it sits. */
struct FMantleWeaponRigidPart
{
	/** A UStaticMesh or a USkeletalMesh. */
	UObject* Mesh = nullptr;

	/** Placement in the body mesh's space. */
	FTransform InBody;
};

struct FMantleWeaponRigidMeshInput
{
	/** A UStaticMesh or USkeletalMesh; LOD0, reference pose, all sections. */
	UObject* BodyMesh = nullptr;

	/** Extra meshes baked rigidly into the root bone (attachments). */
	TArray<FMantleWeaponRigidPart> BodyParts;

	/** Becomes the bone "magazine" (a child of root) with its pivot at Magazine->InBody. */
	TOptional<FMantleWeaponRigidPart> Magazine;

	/** In body mesh space (Grip_R, Grip_L, Muzzle, Sight, ShellEject). They go on the root bone; "Magazine" goes on the magazine bone. */
	TMap<FName, FTransform> Sockets;

	/** Applied to everything above (geometry, the magazine bone, sockets) to place the body in the output mesh: the
	 *  builder's orientation into the weapon contract frame (bore +X, up +Z) and its uniform fit scale. */
	FTransform BodyToOutput = FTransform::Identity;

	/** Where the assets are created, e.g. "/Game/Converted/TR15Rigid". */
	FString PackagePath;

	/** The asset base name: "TR15Rigid" makes SKM_TR15Rigid (the mesh) and SK_TR15Rigid (the skeleton). */
	FString Name;
};

struct FMantleWeaponRigidMeshResult
{
	USkeletalMesh* Mesh = nullptr;
	USkeleton* Skeleton = nullptr;
	TArray<FString> Errors;
};

/**
 * Builds a rigid skeletal mesh for a weapon from existing mesh geometry (#729): a root bone and, when the weapon has
 * one, a magazine bone, with every vertex weighted whole to one of them, plus the sockets the runtime reads. The
 * assets are written to disk, replacing same-named ones.
 */
struct MANTLEEDITOR_API FMantleWeaponRigidMesh
{
	/** Builds the mesh and skeleton. On any error Errors is filled and no asset is left behind. */
	static FMantleWeaponRigidMeshResult Build(const FMantleWeaponRigidMeshInput& Input);

	/** Every socket of a skeletal mesh (its own and its skeleton's), in mesh space in the reference pose: the bone chain times the socket's local transform. */
	static TMap<FName, FTransform> ReadSocketsInMeshSpace(const USkeletalMesh* Mesh);
};
