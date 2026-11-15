// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceFreezePlan.h"

namespace MantlePlaceFreezePlanPrivate
{
	/** The entity's transform in the world: its own, then each ancestor's, innermost first (a bad place's parent loop ends at the entity count). */
	FTransform ComposeWorld(const FMantleEntity& Entity, const TMap<FString, const FMantleEntity*>& ById, const int32 Limit)
	{
		FTransform World = Entity.Transform;
		FString ParentId = Entity.Parent;
		for (int32 Depth = 0; !ParentId.IsEmpty() && Depth < Limit; ++Depth)
		{
			const FMantleEntity* const* Parent = ById.Find(ParentId);
			if (!Parent)
			{
				break;
			}
			World = World * (*Parent)->Transform;
			ParentId = (*Parent)->Parent;
		}
		return World;
	}
}

FMantlePlaceFreezePlan FMantlePlaceFreezePlan::Make(const FMantlePlace& Place)
{
	using namespace MantlePlaceFreezePlanPrivate;
	FMantlePlaceFreezePlan Plan;
	TMap<FString, const FMantleEntity*> EntityById;
	EntityById.Reserve(Place.Entities.Num());
	for (const FMantleEntity& Entity : Place.Entities)
	{
		EntityById.Add(Entity.Id, &Entity);
	}
	const int32 Limit = Place.Entities.Num();
	Plan.AllWorld.Reserve(Place.Entities.Num());
	for (const FMantleEntity& Entity : Place.Entities)
	{
		Plan.AllWorld.Add(Entity.Id, ComposeWorld(Entity, EntityById, Limit));
	}

	TSet<const FMantleEntity*> GroupSet;
	TArray<const FMantleEntity*> Groups;
	for (const FMantleEntity& Entity : Place.Entities)
	{
		const bool bMesh = Entity.Type == EMantleEntityType::StaticMesh || Entity.Type == EMantleEntityType::InstancedMesh;
		const bool bTerrain = Entity.Type == EMantleEntityType::Terrain;
		const bool bScatter = Entity.Type == EMantleEntityType::Scatter;
		const FMantleStaticMeshProps* Props = bMesh ? MantleMeshPropsOf(Entity) : nullptr;
		const bool bBlocking = bTerrain || bScatter || (Props && Props->Collision == EMantleCollision::Blocking);
		if (!bBlocking || Entity.bDynamic)
		{
			continue;
		}

		// Every ancestor, innermost first, as long as each is a Group that isn't dynamic. The parser refuses parent loops; the Contains check only
		// keeps a bad place from hanging.
		TArray<const FMantleEntity*> Ancestors;
		bool bStaticChain = true;
		for (FString ParentId = Entity.Parent; !ParentId.IsEmpty(); )
		{
			const FMantleEntity* const* Parent = EntityById.Find(ParentId);
			if (!Parent || (*Parent)->Type != EMantleEntityType::Group || (*Parent)->bDynamic
				|| *Parent == &Entity || Ancestors.Contains(*Parent))
			{
				bStaticChain = false;
				break;
			}
			Ancestors.Add(*Parent);
			ParentId = (*Parent)->Parent;
		}
		if (!bStaticChain)
		{
			continue;
		}
		Plan.GeometryIds.Add(Entity.Id);
		Plan.StaticWorld.Add(Entity.Id, ComposeWorld(Entity, EntityById, Limit));
		// Outermost first. A Group already listed keeps its place, so every Group comes after all of its own ancestors.
		for (int32 Index = Ancestors.Num() - 1; Index >= 0; --Index)
		{
			if (!GroupSet.Contains(Ancestors[Index]))
			{
				GroupSet.Add(Ancestors[Index]);
				Groups.Add(Ancestors[Index]);
			}
		}
	}
	for (const FMantleEntity* Group : Groups)
	{
		Plan.GroupIds.Add(Group->Id);
		Plan.StaticWorld.Add(Group->Id, ComposeWorld(*Group, EntityById, Limit));
	}
	return Plan;
}
