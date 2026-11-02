// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/SkinnedAssetCommon.h"
#include "MantleSkinnedMesh.h"
#include "MantleVehicleConverter.h"

class USkeletalMesh;

/**
 * Joins the static pieces a vehicle Blueprint attaches to a bare skeletal body into that body, as one skeletal model (vehicle builder flow plan, step 4).
 *
 * Some cars are a skeleton of bones and wheels with little or nothing to see on it; the real car is static meshes the Blueprint attaches (the body shell, the doors, the
 * seats, the cargo, the lights on their own material). The builder reads one model: its picture, its fit, its paint sections and its lights are all that model's. Joining
 * the pieces makes the whole car that model, with each piece on the bone it rides (so wheels and anything rigged keep moving) and each piece's own materials, a material the
 * body already draws shared.
 */
struct MANTLEEDITOR_API FMantleVehicleBodyJoin
{
	/** One static piece as plain data: its geometry, its materials, the bone it rides and where it sits in the body's component space. */
	struct FPiece
	{
		/** LOD 0 as FMantleVehicleModel reads a static mesh: positions, normals, tangents (empty, or one per vertex), UV 0, indices and sections naming Materials. */
		FMantleSkinnedMeshLOD LOD;
		TArray<FSkeletalMaterial> Materials;
		int32 Bone = 0;
		FTransform Placement;
	};

	/**
	 * Adds Pieces to Body (LOD 0), weighted whole to their bones. A piece's material the body (or an earlier piece) already draws shares its slot; another gets a slot of its
	 * own, named after the piece's, made unique. The triangles are regrouped so each slot is one section. A piece placed with a mirrored scale keeps its faces outward. Tangents
	 * are kept when every piece has them, else the build works them out from the UVs. False, with Body untouched, and the reason in OutError (a clause for a sentence) when a
	 * piece names a bone Body lacks or has no geometry. Pure arithmetic over the arrays.
	 */
	static bool Join(FMantleSkinnedMesh& Body, TArray<FSkeletalMaterial>& Materials, TConstArrayView<FPiece> Pieces, FString& OutError);

	/** Reads Placed's static mesh as a piece; each slot's material is the component's override when it has one, else the mesh's own. False with the reason in OutError (a clause). */
	static bool ReadPiece(const FMantleVehiclePlacedMesh& Placed, FPiece& Out, FString& OutError);

	/**
	 * The body model with Pieces joined: Body's LOD 0 (FMantleSkinnedMesh::ExtractFromSkeletalMesh) plus the pieces, saved as a skeletal mesh and skeleton under PackagePath
	 * (SKM_<Name>, SK_<Name>: FMantleVehicleChassisMesh::Create, a path under /Temp is a throwaway). The skeleton has Body's bones, so its wheels and any bones the Blueprint's
	 * wheel setups name are the same. OutTriangles, when given, gets the model's triangle count. Null with the reason in OutError (a clause) when it cannot be made.
	 */
	static USkeletalMesh* Make(const USkeletalMesh& Body, TConstArrayView<FMantleVehiclePlacedMesh> Pieces, const FString& PackagePath, const FString& Name, FString& OutError,
		int32* OutTriangles = nullptr);
};
