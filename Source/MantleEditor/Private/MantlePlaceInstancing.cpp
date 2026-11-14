// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceInstancing.h"
#include "MantlePlaceExporter.h"


// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePlaceInstancingPrivate
{
	/** The smallest scale an instance is written with (the place file clamps below it), so smaller ones stay entities. */
	constexpr double MinInstanceScale = 0.0001;

	/** 106964 as "106,964": the same in every culture, so the text a harness greps does not change with the machine. */
	FString Count(const int64 Value)
	{
		FString Digits = FString::Printf(TEXT("%lld"), FMath::Abs(Value));
		for (int32 Position = Digits.Len() - 3; Position > 0; Position -= 3)
		{
			Digits.InsertAt(Position, TEXT(","));
		}
		return Value < 0 ? TEXT("-") + Digits : Digits;
	}

	/** Whether item Index of Count is kept when Keep are: one more item is kept at each step the running share crosses a whole number. */
	bool Keeps(const int64 Index, const int64 Total, const int64 Keep)
	{
		return ((Index + 1) * Keep) / Total > (Index * Keep) / Total;
	}

	/** The ids other entities name; an entity named here has to stay an entity. */
	TSet<FString> NamedIds(const FMantlePlace& Place)
	{
		TSet<FString> Named;
		for (const FMantleEntity& Entity : Place.Entities)
		{
			if (!Entity.Parent.IsEmpty())
			{
				Named.Add(Entity.Parent);
			}
			if (const FMantleConstraintProps* Joint = Entity.Props.TryGet<FMantleConstraintProps>())
			{
				Named.Add(Joint->EntityA);
				Named.Add(Joint->EntityB);
			}
			if (const FMantleCrowdSpawnerProps* Crowd = Entity.Props.TryGet<FMantleCrowdSpawnerProps>())
			{
				for (const FMantleCrowdPointOfInterest& Point : Crowd->PointsOfInterest)
				{
					Named.Add(Point.EntityId);
				}
			}
			if (Entity.Interactable.IsSet())
			{
				for (const FMantleInteractablePartSpec& Part : Entity.Interactable.GetValue().Parts)
				{
					Named.Add(Part.AnchorEntity);
					Named.Append(Part.HighlightEntities);
				}
			}
		}
		for (const FMantleSeat& Seat : Place.Seats)
		{
			Named.Add(Seat.Entity);
		}
		return Named;
	}

	/** A StaticMesh entity that nothing could tell from its twins: see FMantlePlaceInstancing::GroupStaticMeshes. */
	bool IsPlainStaticMesh(const FMantleEntity& Entity, const TSet<FString>& Named)
	{
		const FMantleStaticMeshProps* Mesh = Entity.Props.TryGet<FMantleStaticMeshProps>();
		if (Entity.Type != EMantleEntityType::StaticMesh || !Mesh || Mesh->Physics.IsSet())
		{
			return false;
		}
		if (Entity.bDynamic || !Entity.Parent.IsEmpty() || Entity.Tags.Num() > 0 || Entity.Interactable.IsSet() || Named.Contains(Entity.Id))
		{
			return false;
		}
		const FVector Scale = Entity.Transform.GetScale3D();
		return Entity.Transform.IsValid() && Scale.GetMin() >= MinInstanceScale;
	}
}

int64 FMantlePlaceInstancing::CountInstances(const FMantlePlace& Place)
{
	int64 Total = 0;
	for (const FMantleEntity& Entity : Place.Entities)
	{
		if (const FMantleInstancedMeshProps* Props = Entity.Props.TryGet<FMantleInstancedMeshProps>())
		{
			Total += Props->Instances.Num();
		}
	}
	return Total;
}

FMantleGroupReport FMantlePlaceInstancing::GroupStaticMeshes(FMantlePlace& Place, TArray<FMantleInstancedMeshGroup>& Groups)
{
	using namespace MantlePlaceInstancingPrivate;
	FMantleGroupReport Report;

	const TSet<FString> Named = NamedIds(Place);
	TMap<FString, int32> PlainByKey;
	for (const FMantleEntity& Entity : Place.Entities)
	{
		if (IsPlainStaticMesh(Entity, Named))
		{
			++PlainByKey.FindOrAdd(FMantlePlaceExporter::InstancedGroupKey(Entity.Props.Get<FMantleStaticMeshProps>()));
		}
	}

	TMap<FString, int32> GroupByKey;
	for (int32 Index = 0; Index < Groups.Num(); ++Index)
	{
		GroupByKey.Add(FMantlePlaceExporter::InstancedGroupKey(Groups[Index].Mesh), Index);
	}

	TSet<FString> SetsUsed;
	TArray<FMantleEntity> Kept;
	Kept.Reserve(Place.Entities.Num());
	for (FMantleEntity& Entity : Place.Entities)
	{
		if (!IsPlainStaticMesh(Entity, Named))
		{
			Kept.Add(MoveTemp(Entity));
			continue;
		}
		const FMantleStaticMeshProps& Mesh = Entity.Props.Get<FMantleStaticMeshProps>();
		const FString Key = FMantlePlaceExporter::InstancedGroupKey(Mesh);
		if (PlainByKey.FindChecked(Key) < MinGroupSize)
		{
			Kept.Add(MoveTemp(Entity));
			continue;
		}

		int32 GroupIndex = INDEX_NONE;
		if (const int32* Found = GroupByKey.Find(Key))
		{
			GroupIndex = *Found;
		}
		else
		{
			GroupIndex = Groups.Num();
			GroupByKey.Add(Key, GroupIndex);
			FMantleInstancedMeshGroup& Added = Groups.AddDefaulted_GetRef();
			Added.Mesh = Mesh;
			Added.MeshName = Entity.Name;
		}
		Groups[GroupIndex].WorldInstances.Add(Entity.Transform);
		SetsUsed.Add(Key);
		++Report.Actors;
	}
	Place.Entities = MoveTemp(Kept);
	Report.Sets = SetsUsed.Num();
	return Report;
}

TArray<bool> FMantlePlaceInstancing::EvenKeepMask(const int64 Count, const int64 Keep)
{
	TArray<bool> Mask;
	if (Count <= 0)
	{
		return Mask;
	}
	const int64 Wanted = FMath::Clamp<int64>(Keep, 0, Count);
	Mask.Reserve(static_cast<int32>(Count));
	for (int64 Index = 0; Index < Count; ++Index)
	{
		Mask.Add(MantlePlaceInstancingPrivate::Keeps(Index, Count, Wanted));
	}
	return Mask;
}

FString FMantlePlaceInstancing::DescribeBytes(const int64 Bytes)
{
	return FString::Printf(TEXT("%.1f MiB"), static_cast<double>(Bytes) / (1024.0 * 1024.0));
}

FString FMantlePlaceInstancing::DescribeOverBudget(const int64 Instances, const int64 InstanceLimit, const int64 Bytes, const int64 ByteLimit)
{
	using namespace MantlePlaceInstancingPrivate;
	const bool bOverCount = Instances > InstanceLimit;
	const bool bOverBytes = Bytes > ByteLimit;
	if (!bOverCount && !bOverBytes)
	{
		return FString();
	}

	FString Problem;
	if (bOverCount && bOverBytes)
	{
		Problem = FString::Printf(TEXT("The part of this place that always loads has %s instanced meshes (the limit is %s) and its place file would be %s (Mantle accepts at most %s)."),
			*Count(Instances), *Count(InstanceLimit), *DescribeBytes(Bytes), *DescribeBytes(ByteLimit));
	}
	else if (bOverCount)
	{
		Problem = FString::Printf(TEXT("The part of this place that always loads has %s instanced meshes (foliage, PCG output, instanced props); the limit is %s."),
			*Count(Instances), *Count(InstanceLimit));
	}
	else
	{
		Problem = FString::Printf(TEXT("The part of this place that always loads would make a place file of %s; Mantle accepts at most %s."), *DescribeBytes(Bytes), *DescribeBytes(ByteLimit));
	}
	// Scenery with a draw distance moves to cell files, which have no such limit; what is left is what nothing can be culled from.
	return Problem + TEXT(" Scenery that has a draw distance is stored in cells and does not count. What counts is what always loads: meshes with no draw distance, ")
		TEXT("tagged, physics and interactable objects, and objects a script names. Set a draw distance on large repeated meshes, or merge or remove what players never see.");
}
