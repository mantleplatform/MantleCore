// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/SkinnedAssetCommon.h"
#include "MantleVehicleChassisMesh.h"

class UMaterialInterface;
class UStaticMesh;
struct FMantleSkinnedMesh;

/**
 * One piece of a vehicle's body that was split off because the body had more materials than a published body may (#576, option b): the triangles of some of the
 * body's materials that ride one bone, as a static mesh in the body's own space. It publishes as a part of the vehicle (FMantleVehicleSourcePart) on that bone, so it
 * moves with the bone exactly as the body's triangles did.
 */
struct MANTLEEDITOR_API FMantleVehicleBodyPiece
{
	/** The bone the triangles were weighted to, and where it is: ToBone takes a point in the mesh (the body's space) into the bone's space. */
	int32 BoneIndex = 0;
	FName Bone;
	FTransform ToBone;

	/** The geometry, one entry per vertex (a vertex used by two materials is copied). */
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	/** Empty, or one per vertex. */
	TArray<FVector4f> Tangents;
	TArray<FVector2f> UVs;
	TArray<uint32> Indices;

	/** One section per material, in order, each a run of whole triangles of Indices; Material indexes Materials and SlotNames. */
	struct FSection
	{
		int32 FirstIndex = 0;
		int32 IndexCount = 0;
		int32 Material = 0;
	};
	TArray<FSection> Sections;
	TArray<UMaterialInterface*> Materials;
	TArray<FName> SlotNames;

	int32 NumTriangles() const { return Indices.Num() / 3; }
};

/** What splitting an over-cap body did. */
struct MANTLEEDITOR_API FMantleVehicleBodySplitResult
{
	/** Empty when the body was already within the cap (or could not be split: see Problem). */
	TArray<FMantleVehicleBodyPiece> Pieces;

	/** How many materials the body drew before and after. */
	int32 MaterialsBefore = 0;
	int32 MaterialsAfter = 0;
	int32 MovedTriangles = 0;

	/** Why a body over the cap could not be split, as a clause for a sentence; empty when it was split or did not need it. */
	FString Problem;
};

/**
 * Splits a body that draws more materials than a published vehicle's body may (FMantleVehicleLimits::MaxSections, 8) into the body and parts (#576 option b):
 * the body keeps the materials with the most triangles, up to the cap, and the triangles of the others become static-mesh pieces of at most MaxPartMaterials
 * materials each, one piece per bone they ride, which publish as the vehicle's parts. Every triangle keeps its material, its place and its bone, so the vehicle
 * looks as it did. The creator does nothing and needs no Unreal knowledge: the builder says what it did.
 *
 * Pure arithmetic over FMantleSkinnedMesh (tested with meshes made in the test); MakePieceMesh makes the editor asset.
 */
struct MANTLEEDITOR_API FMantleVehicleBodySplit
{
	/** How many different materials LOD 0's sections draw (a section with no triangles draws none). */
	static int32 CountMaterials(const FMantleSkinnedMesh& Body);

	/**
	 * Splits Body (LOD 0) in place when it draws more than MaxBodyMaterials: its geometry, sections and Materials (the model's slots, which keep only the ones
	 * the body still draws, in their order) and Extras (kept in step with the body's vertices) are rewritten. False, with Body untouched and OutResult.Problem
	 * set, when the pieces would be more than MaxPieces. True with no pieces when Body was within the cap.
	 */
	static bool Split(FMantleSkinnedMesh& Body, TArray<FSkeletalMaterial>& Materials, FMantleVehicleBodyExtras& Extras, int32 MaxBodyMaterials, int32 MaxPieceMaterials,
		int32 MaxPieces, FMantleVehicleBodySplitResult& OutResult);

	/** The name a piece's static mesh is saved under: SM_<Name>_Part<N> (N from 1). */
	static FString PieceAssetName(const FString& Name, int32 Number) { return FString::Printf(TEXT("SM_%s_Part%d"), *Name, Number); }

	/**
	 * Makes Piece a saved static mesh asset PieceName under PackagePath (a throwaway /Temp package is not written to disk), writing into one already there so a build
	 * again keeps the same asset. Null, with the reason in OutError as a clause, when it cannot be built or saved. Game thread.
	 */
	static UStaticMesh* MakePieceMesh(const FMantleVehicleBodyPiece& Piece, const FString& PackagePath, const FString& PieceName, FString& OutError);
};
