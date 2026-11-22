// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCellAssetUse.h"

#include "MantlePlaceAssets.h"

namespace MantleCellAssetUsePrivate
{
	/** An entity's transform in the place's space: its own composed with its parents' (a parent outside Entities counts as the place). */
	FTransform WorldTransformOf(const FMantleEntity& Entity, const TMap<FString, const FMantleEntity*>& ById, TMap<FString, FTransform>& Done)
	{
		if (const FTransform* Known = Done.Find(Entity.Id))
		{
			return *Known;
		}
		FTransform World = Entity.Transform;
		const FMantleEntity* Parent = Entity.Parent.IsEmpty() ? nullptr : ById.FindRef(Entity.Parent);
		// A parent chain is a tree in a valid place; the depth guard only keeps a malformed one from looping.
		for (int32 Depth = 0; Parent && Depth < 64; ++Depth)
		{
			World = World * Parent->Transform;
			Parent = Parent->Parent.IsEmpty() ? nullptr : ById.FindRef(Parent->Parent);
		}
		Done.Add(Entity.Id, World);
		return World;
	}
}

void FMantleCellAssetUse::NeedsOf(const TArray<FMantleEntity>& Entities, TArray<FMantleCellAssetNeed>& Out)
{
	using namespace MantleCellAssetUsePrivate;
	Out.Reset();
	TMap<FString, int32> MeshAt;
	TMap<FString, int32> MaterialAt;
	TMap<FString, const FMantleEntity*> ById;
	ById.Reserve(Entities.Num());
	for (const FMantleEntity& Entity : Entities)
	{
		ById.Add(Entity.Id, &Entity);
	}
	TMap<FString, FTransform> WorldOf;
	for (const FMantleEntity& Entity : Entities)
	{
		const FMantleStaticMeshProps* Props = MantleMeshPropsOf(Entity);
		if (!Props)
		{
			continue;
		}
		// Where this entity's meshes stand: its origin, or each of its instances.
		FBox Where(ForceInit);
		const FTransform World = WorldTransformOf(Entity, ById, WorldOf);
		const FMantleInstancedMeshProps* Instanced = Entity.Props.TryGet<FMantleInstancedMeshProps>();
		if (Instanced && Instanced->Instances.Num() > 0)
		{
			for (const FTransform& Instance : Instanced->Instances)
			{
				Where += World.TransformPosition(Instance.GetLocation());
			}
		}
		else
		{
			Where += World.GetLocation();
		}
		if (FMantlePlaceAssets::IsUploadedId(Props->Mesh))
		{
			const bool bCollision = Props->Collision != EMantleCollision::None;
			if (const int32* At = MeshAt.Find(Props->Mesh))
			{
				Out[*At].bCollision = Out[*At].bCollision || bCollision;
				Out[*At].Bounds += Where;
			}
			else
			{
				MeshAt.Add(Props->Mesh, Out.Num());
				FMantleCellAssetNeed& Need = Out.AddDefaulted_GetRef();
				Need.Id = Props->Mesh;
				Need.bCollision = bCollision;
				Need.Bounds = Where;
			}
		}
		// A collision-only mesh draws nothing, so it needs none of its materials.
		static const TArray<FString> NoMaterials;
		for (const FString& Material : Props->bVisible ? Props->Materials : NoMaterials)
		{
			if (!FMantlePlaceAssets::IsUploadedId(Material))
			{
				continue;
			}
			if (const int32* At = MaterialAt.Find(Material))
			{
				Out[*At].Bounds += Where;
			}
			else
			{
				MaterialAt.Add(Material, Out.Num());
				FMantleCellAssetNeed& Need = Out.AddDefaulted_GetRef();
				Need.Id = Material;
				Need.bMaterial = true;
				Need.Bounds = Where;
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
