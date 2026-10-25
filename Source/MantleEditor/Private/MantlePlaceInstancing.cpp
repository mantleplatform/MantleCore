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

	/** An InstancedMesh entity's instances, or null. */
	TArray<FTransform>* InstancesOf(FMantleEntity& Entity)
	{
		FMantleInstancedMeshProps* Props = Entity.Props.TryGet<FMantleInstancedMeshProps>();
		return Props ? &Props->Instances : nullptr;
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

FString FMantleThinReport::Describe() const
{
	using namespace MantlePlaceInstancingPrivate;
	if (!RemovesAnything())
	{
		return TEXT("Nothing needs thinning.");
	}
	FString What;
	if (FoliageRemoved() > 0 && OthersRemoved() > 0)
	{
		What = FString::Printf(TEXT("%s foliage instances first, then %s of the other instances"), *Count(FoliageRemoved()), *Count(OthersRemoved()));
	}
	else if (FoliageRemoved() > 0)
	{
		What = FString::Printf(TEXT("%s foliage instances"), *Count(FoliageRemoved()));
	}
	else
	{
		What = FString::Printf(TEXT("%s instances of props and PCG output"), *Count(OthersRemoved()));
	}
	return FString::Printf(TEXT("%s evenly across the level, %s %s of %s instances (%s removed), and the place file %s %s to %s."),
		bApplied ? TEXT("Thinned") : TEXT("Thinning would remove"), bApplied ? TEXT("kept") : TEXT("keeping"),
		*Count(InstancesAfter), *Count(InstancesBefore), *What,
		bApplied ? TEXT("went from") : TEXT("going from"), *FMantlePlaceInstancing::DescribeBytes(BytesBefore),
		*FMantlePlaceInstancing::DescribeBytes(BytesAfter));
}

int64 FMantlePlaceInstancing::PlaceBytes(const FMantlePlace& Place)
{
	const FTCHARToUTF8 Utf8(*FMantlePlaceExporter::ToJson(Place));
	return Utf8.Length();
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
			// Props beside painted foliage are not what thinning takes first.
			Groups[GroupIndex].bThinFirst = false;
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

FMantleThinReport FMantlePlaceInstancing::ThinToFit(FMantlePlace& Place, const TSet<FString>& ThinFirstIds, const int64 MaxInstances, const int64 MaxBytes,
	const bool bApply)
{
	using namespace MantlePlaceInstancingPrivate;
	FMantleThinReport Report;
	Report.bApplied = bApply;

	// The instanced entities, in place order, with their instances as they are: every round starts from these.
	TArray<int32> Indices;
	TArray<TArray<FTransform>> Originals;
	TArray<bool> bFirst;
	int64 OthersBefore = 0;
	for (int32 Index = 0; Index < Place.Entities.Num(); ++Index)
	{
		if (TArray<FTransform>* Instances = InstancesOf(Place.Entities[Index]))
		{
			Indices.Add(Index);
			Originals.Add(*Instances);
			const bool bThinFirst = ThinFirstIds.Contains(Place.Entities[Index].Id);
			bFirst.Add(bThinFirst);
			(bThinFirst ? Report.FoliageBefore : OthersBefore) += Instances->Num();
		}
	}

	Report.InstancesBefore = Report.FoliageBefore + OthersBefore;
	Report.InstancesAfter = Report.InstancesBefore;
	Report.FoliageAfter = Report.FoliageBefore;
	Report.BytesBefore = PlaceBytes(Place);
	Report.BytesAfter = Report.BytesBefore;

	const bool bOverCount = Report.InstancesBefore > MaxInstances;
	const bool bOverBytes = Report.BytesBefore > MaxBytes;
	if (!bOverCount && !bOverBytes)
	{
		return Report;
	}
	if (Report.InstancesBefore == 0)
	{
		Report.bFits = false;
		return Report;
	}

	auto Restore = [&]()
	{
		for (int32 Slot = 0; Slot < Indices.Num(); ++Slot)
		{
			*InstancesOf(Place.Entities[Indices[Slot]]) = Originals[Slot];
		}
	};

	// What one instance costs in the file: the place with every instance left out against the place with them.
	int64 PerInstance = 1;
	{
		for (const int32 Index : Indices)
		{
			InstancesOf(Place.Entities[Index])->Reset();
		}
		const int64 Without = PlaceBytes(Place);
		Restore();
		PerInstance = FMath::Max<int64>(1, (Report.BytesBefore - Without) / Report.InstancesBefore);
	}

	const int64 TargetBytes = static_cast<int64>(static_cast<double>(MaxBytes) * SizeHeadroom);
	const int64 FirstCap = static_cast<int64>(static_cast<double>(Report.FoliageBefore) * MaxFoliageThinFraction);
	const int64 OtherCap = static_cast<int64>(static_cast<double>(OthersBefore) * MaxFoliageThinFraction);

	int64 Remove = FMath::Max<int64>(0, Report.InstancesBefore - MaxInstances);
	if (bOverBytes)
	{
		Remove = FMath::Max(Remove, (Report.BytesBefore - TargetBytes + PerInstance - 1) / PerInstance);
	}

	for (int32 Round = 0; Round < 6; ++Round)
	{
		Remove = FMath::Min(Remove, FirstCap + OtherCap);
		const int64 RemoveFirst = FMath::Min(Remove, FirstCap);
		const int64 RemoveOther = Remove - RemoveFirst;

		// One order per tier, across all its entities: the n-th instance of the tier is kept by the same rule whichever entity holds it.
		int64 FirstSeen = 0;
		int64 OtherSeen = 0;
		const int64 FirstKeep = Report.FoliageBefore - RemoveFirst;
		const int64 OtherKeep = OthersBefore - RemoveOther;
		for (int32 Slot = 0; Slot < Indices.Num(); ++Slot)
		{
			TArray<FTransform>& Instances = *InstancesOf(Place.Entities[Indices[Slot]]);
			Instances.Reset();
			int64& Seen = bFirst[Slot] ? FirstSeen : OtherSeen;
			const int64 Total = bFirst[Slot] ? Report.FoliageBefore : OthersBefore;
			const int64 Keep = bFirst[Slot] ? FirstKeep : OtherKeep;
			for (const FTransform& Instance : Originals[Slot])
			{
				if (Keeps(Seen, Total, Keep))
				{
					Instances.Add(Instance);
				}
				++Seen;
			}
		}

		Report.FoliageAfter = FirstKeep;
		Report.InstancesAfter = FirstKeep + OtherKeep;
		Report.BytesAfter = PlaceBytes(Place);
		if (Report.BytesAfter <= MaxBytes && Report.InstancesAfter <= MaxInstances)
		{
			break;
		}
		if (Remove >= FirstCap + OtherCap)
		{
			break;
		}
		// The estimate ran short: take what is still over, and a quarter more.
		const int64 StillOver = FMath::Max<int64>(Report.BytesAfter - TargetBytes, 0);
		Remove += FMath::Max<int64>(1, (StillOver * 5 / 4 + PerInstance - 1) / PerInstance);
	}

	Report.bFits = Report.BytesAfter <= MaxBytes && Report.InstancesAfter <= MaxInstances;
	if (!bApply)
	{
		Restore();
		return Report;
	}
	// An entity left with no instance has nothing to place.
	for (int32 Slot = Indices.Num() - 1; Slot >= 0; --Slot)
	{
		if (InstancesOf(Place.Entities[Indices[Slot]])->Num() == 0)
		{
			Place.Entities.RemoveAt(Indices[Slot]);
		}
	}
	return Report;
}

FString FMantlePlaceInstancing::DescribeBytes(const int64 Bytes)
{
	return FString::Printf(TEXT("%.1f MiB"), static_cast<double>(Bytes) / (1024.0 * 1024.0));
}

FString FMantlePlaceInstancing::DescribeOverBudget(const int64 Instances, const int64 InstanceLimit, const int64 Bytes, const int64 ByteLimit,
	const FMantleThinReport& Thin)
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
		Problem = FString::Printf(TEXT("This place has %s instanced meshes (a place holds at most %s) and its place file would be %s (Mantle accepts at most %s)."),
			*Count(Instances), *Count(InstanceLimit), *DescribeBytes(Bytes), *DescribeBytes(ByteLimit));
	}
	else if (bOverCount)
	{
		Problem = FString::Printf(TEXT("This place has %s instanced meshes (foliage, PCG output, instanced props); a place holds at most %s."),
			*Count(Instances), *Count(InstanceLimit));
	}
	else
	{
		Problem = FString::Printf(TEXT("The place file would be %s; Mantle accepts at most %s."), *DescribeBytes(Bytes), *DescribeBytes(ByteLimit));
	}

	if (Thin.RemovesAnything() && Thin.bFits)
	{
		return Problem + TEXT(" Even thinning fixes it. ") + Thin.Describe()
			+ TEXT(" Nothing is removed unless you choose it: use Thin and publish in the Mantle Publish window, or Mantle.Publish -Thin=auto when publishing without the editor.");
	}
	if (Thin.RemovesAnything())
	{
		return Problem + TEXT(" Thinning alone cannot fix it: ") + Thin.Describe()
			+ TEXT(" That still leaves it over, so remove or merge more of the level's repeated objects, or split the world into more than one place.");
	}
	return Problem + TEXT(" There are no instances to thin: merge repeated props into fewer objects, delete what players never see, or split the world into more than one place.");
}
