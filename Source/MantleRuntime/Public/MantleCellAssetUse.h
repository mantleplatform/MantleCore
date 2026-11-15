// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"

/** One asset a group of entities needs: an uploaded mesh (and whether it must be built with collision) or an uploaded material. */
struct MANTLERUNTIME_API FMantleCellAssetNeed
{
	FString Id;

	/** A material; otherwise a mesh. */
	bool bMaterial = false;

	/** Meshes: an entity that collides (collision other than none) uses it, so the mesh needs its collision cooked. */
	bool bCollision = false;

	bool operator==(const FMantleCellAssetNeed& Other) const { return Id == Other.Id && bMaterial == Other.bMaterial && bCollision == Other.bCollision; }
};

/**
 * Who uses which uploaded mesh and material (stage 2 of large places: assets follow the cells).
 *
 * A holder (a cell, in one streamer) takes the assets its entities need and gives them back whole: Acquire counts it once as a user of each, Release
 * removes it. An asset is wanted while it has a user, so the asset set fetches it when its first user arrives and lets it go when its last one leaves.
 * A mesh is wanted with collision while any user of it needs collision, and stays so while it has users (a mesh is built once, with collision when any
 * user needs it; it is not rebuilt without when the last collision user goes).
 *
 * Plain data, any thread. No assets are touched here.
 */
class MANTLERUNTIME_API FMantleCellAssetUse
{
public:
	/** The uploaded meshes and materials the entities use, each once, in first-use order (a mesh with collision when any entity using it collides; built-in library ids are not uploaded and are left out). */
	static void NeedsOf(const TArray<FMantleEntity>& Entities, TArray<FMantleCellAssetNeed>& Out);

	/**
	 * Holder takes Needs; a holder that already holds something is given it back first. OutStarted is what the asset set has to act on: a need whose id had
	 * no user before, and a mesh need whose collision was not wanted before (its id is in the list again with bCollision set).
	 */
	void Acquire(int64 Holder, const TArray<FMantleCellAssetNeed>& Needs, TArray<FMantleCellAssetNeed>& OutStarted);

	/** Holder gives its needs back. OutEnded is every need whose id has no user left (bCollision as the last wanted). A holder that holds nothing changes nothing. */
	void Release(int64 Holder, TArray<FMantleCellAssetNeed>& OutEnded);

	/** What a holder holds now (empty when none). */
	const TArray<FMantleCellAssetNeed>* NeedsOf(int64 Holder) const { return Held.Find(Holder); }

	/** How many holders use the id as a mesh (bMaterial false) or a material, and how many of them need the mesh with collision. */
	int32 Users(const FString& Id, bool bMaterial) const;
	int32 CollisionUsers(const FString& Id) const;

	/** Distinct ids wanted now, by kind. */
	int32 DistinctMeshes() const { return MeshUsers.Num(); }
	int32 DistinctMaterials() const { return MaterialUsers.Num(); }
	int32 HolderCount() const { return Held.Num(); }

private:
	struct FUse
	{
		int32 Users = 0;
		int32 CollisionUsers = 0;
	};

	TMap<int64, TArray<FMantleCellAssetNeed>> Held;
	TMap<FString, FUse> MeshUsers;
	TMap<FString, FUse> MaterialUsers;
};
