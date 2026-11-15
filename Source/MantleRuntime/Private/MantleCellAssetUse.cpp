// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCellAssetUse.h"

#include "MantlePlaceAssets.h"

void FMantleCellAssetUse::NeedsOf(const TArray<FMantleEntity>& Entities, TArray<FMantleCellAssetNeed>& Out)
{
	Out.Reset();
	TMap<FString, int32> MeshAt;
	TMap<FString, int32> MaterialAt;
	for (const FMantleEntity& Entity : Entities)
	{
		const FMantleStaticMeshProps* Props = MantleMeshPropsOf(Entity);
		if (!Props)
		{
			continue;
		}
		if (FMantlePlaceAssets::IsUploadedId(Props->Mesh))
		{
			const bool bCollision = Props->Collision != EMantleCollision::None;
			if (const int32* At = MeshAt.Find(Props->Mesh))
			{
				Out[*At].bCollision = Out[*At].bCollision || bCollision;
			}
			else
			{
				MeshAt.Add(Props->Mesh, Out.Num());
				FMantleCellAssetNeed& Need = Out.AddDefaulted_GetRef();
				Need.Id = Props->Mesh;
				Need.bCollision = bCollision;
			}
		}
		for (const FString& Material : Props->Materials)
		{
			if (FMantlePlaceAssets::IsUploadedId(Material) && !MaterialAt.Contains(Material))
			{
				MaterialAt.Add(Material, Out.Num());
				FMantleCellAssetNeed& Need = Out.AddDefaulted_GetRef();
				Need.Id = Material;
				Need.bMaterial = true;
			}
		}
	}
}

void FMantleCellAssetUse::Acquire(const int64 Holder, const TArray<FMantleCellAssetNeed>& Needs, TArray<FMantleCellAssetNeed>& OutStarted)
{
	OutStarted.Reset();
	if (Held.Contains(Holder))
	{
		TArray<FMantleCellAssetNeed> Ignored;
		Release(Holder, Ignored);
	}
	TArray<FMantleCellAssetNeed>& Mine = Held.Add(Holder);
	for (const FMantleCellAssetNeed& Need : Needs)
	{
		// A holder holds an id once, whatever it is asked.
		const bool bDuplicate = Mine.ContainsByPredicate([&Need](const FMantleCellAssetNeed& Other) { return Other.Id == Need.Id && Other.bMaterial == Need.bMaterial; });
		if (bDuplicate)
		{
			continue;
		}
		Mine.Add(Need);
		FUse& Use = (Need.bMaterial ? MaterialUsers : MeshUsers).FindOrAdd(Need.Id);
		const bool bFirstUser = Use.Users == 0;
		const bool bFirstCollision = !Need.bMaterial && Need.bCollision && Use.CollisionUsers == 0;
		++Use.Users;
		Use.CollisionUsers += (!Need.bMaterial && Need.bCollision) ? 1 : 0;
		if (bFirstUser || bFirstCollision)
		{
			OutStarted.Add(Need);
		}
	}
}

void FMantleCellAssetUse::Release(const int64 Holder, TArray<FMantleCellAssetNeed>& OutEnded)
{
	OutEnded.Reset();
	TArray<FMantleCellAssetNeed> Mine;
	if (!Held.RemoveAndCopyValue(Holder, Mine))
	{
		return;
	}
	for (const FMantleCellAssetNeed& Need : Mine)
	{
		TMap<FString, FUse>& Map = Need.bMaterial ? MaterialUsers : MeshUsers;
		FUse* Use = Map.Find(Need.Id);
		if (!Use)
		{
			continue;
		}
		--Use->Users;
		Use->CollisionUsers -= (!Need.bMaterial && Need.bCollision) ? 1 : 0;
		if (Use->Users <= 0)
		{
			Map.Remove(Need.Id);
			OutEnded.Add(Need);
		}
	}
}

int32 FMantleCellAssetUse::Users(const FString& Id, const bool bMaterial) const
{
	const FUse* Use = (bMaterial ? MaterialUsers : MeshUsers).Find(Id);
	return Use ? Use->Users : 0;
}

int32 FMantleCellAssetUse::CollisionUsers(const FString& Id) const
{
	const FUse* Use = MeshUsers.Find(Id);
	return Use ? Use->CollisionUsers : 0;
}
