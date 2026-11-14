// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCells.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceAssets.h"
#include "MantleSha256.h"
#include "Misc/Compression.h"
#include "Misc/SecureHash.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCellsPrivate
{
	/** Instances are written to 0.01 cm; a pivot is that written number (4.1), so it is rounded the same way before a cell is chosen. */
	double Quantize(const double Value)
	{
		return FMath::RoundToDouble(Value * 100.0) / 100.0;
	}

	/** Below this a scale is not written as an instance (the place file clamps there), so a mesh smaller than it cannot fold. */
	constexpr double MinInstanceScale = 0.0001;

	const TCHAR* StayWords(const EMantleCellStay Reason)
	{
		switch (Reason)
		{
		case EMantleCellStay::OtherType: return TEXT("are not meshes (lights, decals, triggers, spawns and the rest always load)");
		case EMantleCellStay::NoDrawDistance: return TEXT("have no draw distance (they are never culled by distance, so they always load)");
		case EMantleCellStay::DrawDistanceTooFar: return TEXT("are drawn from farther than the largest cell level");
		case EMantleCellStay::Dynamic: return TEXT("are Movable");
		case EMantleCellStay::Tagged: return TEXT("have tags");
		case EMantleCellStay::Physics: return TEXT("have physics");
		case EMantleCellStay::Interactable: return TEXT("are interactable");
		case EMantleCellStay::Named: return TEXT("are named by another object (a parent, a seat, a joint, a crowd point or an interactable part)");
		case EMantleCellStay::ScriptLiteral: return TEXT("have names a script, package or UI file quotes");
		case EMantleCellStay::NotSpatiallyLoaded: return TEXT("have Is Spatially Loaded off");
		case EMantleCellStay::ParentNotPlain: return TEXT("sit under something that is not a plain Group");
		case EMantleCellStay::TransformNotFlat: return TEXT("sit under Groups that skew or mirror them");
		case EMantleCellStay::OutOfGrid: return TEXT("are outside the largest map a cell grid covers");
		default: return TEXT("");
		}
	}

	/** An entity as the cell index and the report count it: StaticMesh 1 (and solid when it collides), InstancedMesh its instances. */
	void CountEntity(const FMantleEntity& Entity, int64& Instances, int64& Solid)
	{
		if (const FMantleStaticMeshProps* Mesh = Entity.Props.TryGet<FMantleStaticMeshProps>())
		{
			Solid += Mesh->Collision != EMantleCollision::None ? 1 : 0;
		}
		else if (const FMantleInstancedMeshProps* Instanced = Entity.Props.TryGet<FMantleInstancedMeshProps>())
		{
			Instances += Instanced->Instances.Num();
			Solid += Instanced->Mesh.Collision != EMantleCollision::None ? Instanced->Instances.Num() : 0;
		}
	}

	/** One set of instances in one cell: what folded in and what an instanced group sent. */
	struct FCellSet
	{
		FString Key;
		FString MeshName;
		FMantleStaticMeshProps Mesh;
		TArray<FTransform> Instances;

		/** Folded static mesh actors' names, to pick a stable name when no group named the mesh. */
		TArray<FString> FoldedNames;
		int32 Folded = 0;
	};

	struct FCellBuild
	{
		FMantleCellKey Key;
		TArray<FMantleEntity> Statics;
		TMap<FString, FCellSet> Sets;
	};

	/** The nine written numbers of an instance, each rounded as the file rounds it: the tie-break of the instance order (5.4). */
	void WrittenNumbers(const FTransform& Instance, double Out[9])
	{
		auto Round = [](const double Value, const double Scale) { return FMath::RoundToDouble(Value * Scale) / Scale; };
		const FVector Location = Instance.GetLocation();
		const FRotator Rotation = Instance.GetRotation().Rotator();
		const FVector Scale = Instance.GetScale3D();
		Out[0] = Round(Location.X, 100.0);
		Out[1] = Round(Location.Y, 100.0);
		Out[2] = Round(Location.Z, 100.0);
		Out[3] = Round(Rotation.Pitch, 1000.0);
		Out[4] = Round(Rotation.Yaw, 1000.0);
		Out[5] = Round(Rotation.Roll, 1000.0);
		Out[6] = Round(FMath::Max(Scale.X, MinInstanceScale), 10000.0);
		Out[7] = Round(FMath::Max(Scale.Y, MinInstanceScale), 10000.0);
		Out[8] = Round(FMath::Max(Scale.Z, MinInstanceScale), 10000.0);
	}

	/** Whether Composed is exactly the matrix product: no shear came of composing a rotation with a non-uniform scale. */
	bool MatricesAgree(const FMatrix& A, const FMatrix& B)
	{
		for (int32 Row = 0; Row < 4; ++Row)
		{
			for (int32 Column = 0; Column < 4; ++Column)
			{
				const double Left = A.M[Row][Column];
				const double Right = B.M[Row][Column];
				if (FMath::Abs(Left - Right) > 1e-4 * FMath::Max(1.0, FMath::Max(FMath::Abs(Left), FMath::Abs(Right))))
				{
					return false;
				}
			}
		}
		return true;
	}
}

FMantleCellGrid FMantleCellGrid::Default()
{
	FMantleCellGrid Grid;
	Grid.Levels = { { 6400, 15000 }, { 25600, 60000 }, { 102400, 250000 } };
	return Grid;
}

bool FMantleCellGrid::IsValid(FString* OutWhy) const
{
	auto Fail = [OutWhy](const FString& Why)
	{
		if (OutWhy)
		{
			*OutWhy = Why;
		}
		return false;
	};
	if (Levels.Num() < 1 || Levels.Num() > MaxLevels)
	{
		return Fail(FString::Printf(TEXT("a grid has 1 to %d levels, not %d"), MaxLevels, Levels.Num()));
	}
	for (int32 Index = 0; Index < Levels.Num(); ++Index)
	{
		const FMantleCellLevel& Level = Levels[Index];
		if (Level.CellSize < MinCellSize || Level.CellSize > MaxCellSize)
		{
			return Fail(FString::Printf(TEXT("level %d: cellSize %lld is outside %lld to %lld"), Index, Level.CellSize, MinCellSize, MaxCellSize));
		}
		if (Level.MaxDrawDistance <= 0 || Level.MaxDrawDistance > MaxDrawDistanceLimit)
		{
			return Fail(FString::Printf(TEXT("level %d: maxDrawDistance %lld is outside 1 to %lld"), Index, Level.MaxDrawDistance, MaxDrawDistanceLimit));
		}
		if (Index > 0 && (Level.CellSize <= Levels[Index - 1].CellSize || Level.MaxDrawDistance <= Levels[Index - 1].MaxDrawDistance))
		{
			return Fail(FString::Printf(TEXT("level %d does not rise above level %d"), Index, Index - 1));
		}
	}
	return true;
}

int64 FMantleCellGrid::TopMaxDrawDistance() const
{
	return Levels.Num() > 0 ? Levels.Last().MaxDrawDistance : 0;
}

int32 FMantleCellGrid::LevelFor(const double DrawDistance) const
{
	if (!(DrawDistance > 0.0))
	{
		return INDEX_NONE;
	}
	for (int32 Index = 0; Index < Levels.Num(); ++Index)
	{
		if (DrawDistance <= static_cast<double>(Levels[Index].MaxDrawDistance))
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

FMantleCellKey FMantleCellGrid::KeyFor(const int32 Level, const double X, const double Y) const
{
	const double Size = static_cast<double>(Levels[Level].CellSize);
	FMantleCellKey Key;
	Key.Level = Level;
	// Clamped so a value far outside the grid cannot overflow; the caller refuses a key outside MaxCoordinate.
	Key.X = static_cast<int32>(FMath::Clamp(FMath::FloorToDouble(X / Size), -2.0e9, 2.0e9));
	Key.Y = static_cast<int32>(FMath::Clamp(FMath::FloorToDouble(Y / Size), -2.0e9, 2.0e9));
	return Key;
}

const TCHAR* MantleCellStayText(const EMantleCellStay Reason)
{
	return MantleCellsPrivate::StayWords(Reason);
}

int64 FMantlePlannedCell::NumInstances() const
{
	int64 Instances = 0;
	int64 Solid = 0;
	for (const FMantleEntity& Entity : Entities)
	{
		MantleCellsPrivate::CountEntity(Entity, Instances, Solid);
	}
	return Instances;
}

int64 FMantlePlannedCell::NumSolid() const
{
	int64 Instances = 0;
	int64 Solid = 0;
	for (const FMantleEntity& Entity : Entities)
	{
		MantleCellsPrivate::CountEntity(Entity, Instances, Solid);
	}
	return Solid;
}

int32 FMantleCellReport::NumCells() const
{
	int32 Total = 0;
	for (const int32 Count : CellsPerLevel)
	{
		Total += Count;
	}
	return Total;
}

int64 FMantleCellReport::NumStayed() const
{
	int64 Total = 0;
	for (const int64 Count : StayCounts)
	{
		Total += Count;
	}
	return Total;
}

FString FMantleCellPlanner::InstancedEntityId(const FString& GroupKey, const FMantleCellKey& Cell, const int32 Chunk)
{
	const FString Text = FString::Printf(TEXT("%s|L%d|%d|%d"), *GroupKey, Cell.Level, Cell.X, Cell.Y);
	FMD5 Md5;
	const FTCHARToUTF8 Utf8(*Text);
	Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	uint8 Digest[16];
	Md5.Final(Digest);
	FString Hex;
	for (const uint8 Byte : Digest)
	{
		Hex += FString::Printf(TEXT("%02x"), Byte);
	}
	return FString::Printf(TEXT("ent_ism_%s_%d"), *Hex.Left(24), Chunk);
}

FMantleCellPlan FMantleCellPlanner::Plan(FMantlePlace& Place, TArray<FMantleInstancedMeshGroup>& Groups, const FMantleCellPlanInput& Input)
{
	using namespace MantleCellsPrivate;
	FMantleCellPlan Plan;
	FMantleCellReport& Report = Plan.Report;
	const FMantleCellGrid& Grid = Input.Grid;
	const int64 Top = Grid.TopMaxDrawDistance();
	if (Grid.Levels.Num() == 0)
	{
		return Plan;
	}
	auto Stay = [&Report](const EMantleCellStay Reason, const int64 Count = 1) { Report.StayCounts[static_cast<int32>(Reason)] += Count; };

	TArray<FMantleEntity>& Entities = Place.Entities;
	TMap<FString, int32> IndexById;
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		IndexById.Add(Entities[Index].Id, Index);
	}

	// What other entities name (5.2 rule 3): parents are kept apart, because a Group named only by its children is still plain (5.3).
	TSet<FString> ParentIds;
	TSet<FString> NamedOther;
	for (const FMantleEntity& Entity : Entities)
	{
		if (!Entity.Parent.IsEmpty())
		{
			ParentIds.Add(Entity.Parent);
		}
		if (const FMantleConstraintProps* Joint = Entity.Props.TryGet<FMantleConstraintProps>())
		{
			NamedOther.Add(Joint->EntityA);
			NamedOther.Add(Joint->EntityB);
		}
		if (const FMantleCrowdSpawnerProps* Crowd = Entity.Props.TryGet<FMantleCrowdSpawnerProps>())
		{
			for (const FMantleCrowdPointOfInterest& Point : Crowd->PointsOfInterest)
			{
				NamedOther.Add(Point.EntityId);
			}
		}
		if (Entity.Interactable.IsSet())
		{
			for (const FMantleInteractablePartSpec& Part : Entity.Interactable.GetValue().Parts)
			{
				NamedOther.Add(Part.AnchorEntity);
				NamedOther.Append(Part.HighlightEntities);
			}
		}
	}
	for (const FMantleSeat& Seat : Place.Seats)
	{
		NamedOther.Add(Seat.Entity);
	}

	// Plain Groups (5.3), by index; 0 unknown, 1 being decided (a cycle is not plain), 2 plain, 3 not.
	TArray<uint8> GroupState;
	GroupState.SetNumZeroed(Entities.Num());
	TFunction<bool(int32)> IsPlainGroup = [&](const int32 Index) -> bool
	{
		uint8& State = GroupState[Index];
		if (State != 0)
		{
			return State == 2;
		}
		State = 1;
		const FMantleEntity& Group = Entities[Index];
		bool bPlain = Group.Type == EMantleEntityType::Group && !Group.bDynamic && Group.Tags.Num() == 0 && !Group.Interactable.IsSet()
			&& !NamedOther.Contains(Group.Id) && !Input.ScriptLiterals.Contains(Group.Name);
		if (bPlain && !Group.Parent.IsEmpty())
		{
			const int32* ParentIndex = IndexById.Find(Group.Parent);
			bPlain = ParentIndex && IsPlainGroup(*ParentIndex);
		}
		GroupState[Index] = bPlain ? 2 : 3;
		return bPlain;
	};

	TMap<FMantleCellKey, FCellBuild> Builds;
	auto BuildFor = [&Builds](const FMantleCellKey& Key) -> FCellBuild&
	{
		FCellBuild& Build = Builds.FindOrAdd(Key);
		Build.Key = Key;
		return Build;
	};
	auto InGrid = [](const FMantleCellKey& Key) { return FMath::Abs(Key.X) <= FMantleCellGrid::MaxCoordinate && FMath::Abs(Key.Y) <= FMantleCellGrid::MaxCoordinate; };

	// StaticMesh entities (5.2 and 5.3).
	TSet<int32> Moved;
	TMap<int32, FString> MovedParents;
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		FMantleEntity& Entity = Entities[Index];
		if (Entity.Type == EMantleEntityType::Group)
		{
			continue;
		}
		const FMantleStaticMeshProps* Mesh = Entity.Props.TryGet<FMantleStaticMeshProps>();
		if (!Mesh)
		{
			// An InstancedMesh already in the place came from somewhere else than the exporter's own sets: it stays as it is.
			Stay(EMantleCellStay::OtherType);
			continue;
		}
		const int32 Level = Grid.LevelFor(Mesh->DrawDistance);
		if (!(Mesh->DrawDistance > 0.0))
		{
			Stay(EMantleCellStay::NoDrawDistance);
			continue;
		}
		if (Level == INDEX_NONE)
		{
			Stay(EMantleCellStay::DrawDistanceTooFar);
			continue;
		}
		if (Entity.bDynamic)
		{
			Stay(EMantleCellStay::Dynamic);
			continue;
		}
		if (Entity.Tags.Num() > 0)
		{
			Stay(EMantleCellStay::Tagged);
			continue;
		}
		if (Mesh->Physics.IsSet())
		{
			Stay(EMantleCellStay::Physics);
			continue;
		}
		if (Entity.Interactable.IsSet())
		{
			Stay(EMantleCellStay::Interactable);
			continue;
		}
		if (ParentIds.Contains(Entity.Id) || NamedOther.Contains(Entity.Id))
		{
			Stay(EMantleCellStay::Named);
			continue;
		}
		if (Input.ScriptLiterals.Contains(Entity.Name))
		{
			Stay(EMantleCellStay::ScriptLiteral);
			continue;
		}
		if (Input.NotSpatiallyLoaded.Contains(Entity.Id))
		{
			Stay(EMantleCellStay::NotSpatiallyLoaded);
			continue;
		}

		// The Group chain must be plain; the world transform is the composition up it, and must stay a location, rotation and scale.
		FTransform World = Entity.Transform;
		FMatrix WorldMatrix = Entity.Transform.ToMatrixWithScale();
		bool bPlainChain = true;
		FString ParentId = Entity.Parent;
		int32 Hops = 0;
		while (bPlainChain && !ParentId.IsEmpty())
		{
			const int32* ParentIndex = IndexById.Find(ParentId);
			if (!ParentIndex || !IsPlainGroup(*ParentIndex) || ++Hops > Entities.Num())
			{
				bPlainChain = false;
				break;
			}
			const FMantleEntity& Parent = Entities[*ParentIndex];
			World = World * Parent.Transform;
			WorldMatrix = WorldMatrix * Parent.Transform.ToMatrixWithScale();
			ParentId = Parent.Parent;
		}
		if (!bPlainChain)
		{
			Stay(EMantleCellStay::ParentNotPlain);
			continue;
		}
		if (!World.IsValid() || World.GetScale3D().GetMin() <= 0.0 || !MatricesAgree(World.ToMatrixWithScale(), WorldMatrix))
		{
			Stay(EMantleCellStay::TransformNotFlat);
			continue;
		}

		// The pivot is the location as it is written: to 0.01 cm, so a cell file's number and the key agree.
		const FVector Location = World.GetLocation();
		const FVector Pivot(Quantize(Location.X), Quantize(Location.Y), Quantize(Location.Z));
		World.SetLocation(Pivot);
		const FMantleCellKey Key = Grid.KeyFor(Level, Pivot.X, Pivot.Y);
		if (!InGrid(Key))
		{
			Stay(EMantleCellStay::OutOfGrid);
			continue;
		}

		MovedParents.Add(Index, Entity.Parent);
		FMantleEntity Moving = MoveTemp(Entity);
		Moving.Parent.Reset();
		Moving.Transform = World;
		BuildFor(Key).Statics.Add(MoveTemp(Moving));
		Moved.Add(Index);
	}

	// Instances of the exporter's groups (5.4): a group with a draw distance inside the grid goes to cells, instance by instance.
	for (FMantleInstancedMeshGroup& Group : Groups)
	{
		const int64 Count = Group.WorldInstances.Num();
		if (Count == 0)
		{
			continue;
		}
		if (!(Group.Mesh.DrawDistance > 0.0))
		{
			Stay(EMantleCellStay::NoDrawDistance, Count);
			continue;
		}
		const int32 Level = Grid.LevelFor(Group.Mesh.DrawDistance);
		if (Level == INDEX_NONE)
		{
			Stay(EMantleCellStay::DrawDistanceTooFar, Count);
			continue;
		}
		const FString SetKey = FMantlePlaceExporter::InstancedGroupKey(Group.Mesh);
		TArray<FTransform> Kept;
		for (const FTransform& Instance : Group.WorldInstances)
		{
			const FVector Location = Instance.GetLocation();
			const FVector Pivot(Quantize(Location.X), Quantize(Location.Y), Quantize(Location.Z));
			const FMantleCellKey Key = Grid.KeyFor(Level, Pivot.X, Pivot.Y);
			if (!InGrid(Key))
			{
				Stay(EMantleCellStay::OutOfGrid);
				Kept.Add(Instance);
				continue;
			}
			FCellSet& Set = BuildFor(Key).Sets.FindOrAdd(SetKey);
			if (Set.Key.IsEmpty())
			{
				Set.Key = SetKey;
				Set.Mesh = Group.Mesh;
				Set.Mesh.Physics.Reset();
			}
			if (Set.MeshName.IsEmpty())
			{
				Set.MeshName = Group.MeshName;
			}
			FTransform Placed = Instance;
			Placed.SetLocation(Pivot);
			Set.Instances.Add(Placed);
		}
		Group.WorldInstances = MoveTemp(Kept);
	}

	// Folding (5.4): plain static mesh actors of one set fold into that cell's instance list when the cell holds enough of them.
	for (TPair<FMantleCellKey, FCellBuild>& Pair : Builds)
	{
		FCellBuild& Build = Pair.Value;
		TMap<FString, int32> FoldableBySet;
		for (const FMantleEntity& Entity : Build.Statics)
		{
			if (Entity.Transform.GetScale3D().GetMin() >= MinInstanceScale)
			{
				++FoldableBySet.FindOrAdd(FMantlePlaceExporter::InstancedGroupKey(Entity.Props.Get<FMantleStaticMeshProps>()));
			}
		}
		TArray<FMantleEntity> KeptStatics;
		for (FMantleEntity& Entity : Build.Statics)
		{
			const FMantleStaticMeshProps& Mesh = Entity.Props.Get<FMantleStaticMeshProps>();
			const FString SetKey = FMantlePlaceExporter::InstancedGroupKey(Mesh);
			const int32* Foldable = FoldableBySet.Find(SetKey);
			if (!Input.bFoldStaticMeshes || !Foldable || *Foldable < FMantlePlaceInstancing::MinGroupSize || Entity.Transform.GetScale3D().GetMin() < MinInstanceScale)
			{
				KeptStatics.Add(MoveTemp(Entity));
				continue;
			}
			FCellSet& Set = Build.Sets.FindOrAdd(SetKey);
			if (Set.Key.IsEmpty())
			{
				Set.Key = SetKey;
				Set.Mesh = Mesh;
				Set.Mesh.Physics.Reset();
			}
			Set.FoldedNames.Add(Entity.Name);
			++Set.Folded;
			Set.Instances.Add(Entity.Transform);
			++Report.FoldedActors;
		}
		Build.Statics = MoveTemp(KeptStatics);
	}

	// Cells: instance sets chunked in the cell's own Morton order, entities sorted by id, bounds and counts.
	TArray<FMantleCellKey> Keys;
	Builds.GetKeys(Keys);
	Keys.Sort();
	for (const FMantleCellKey& Key : Keys)
	{
		FCellBuild& Build = Builds.FindChecked(Key);
		FMantlePlannedCell Cell;
		Cell.Key = Key;

		auto AddBox = [&Cell](const FString& MeshId, const FMantleCellPlanInput& In, const FTransform& Transform)
		{
			if (const FBox* Local = In.MeshBounds.Find(MeshId); Local && Local->IsValid)
			{
				Cell.Bounds += Local->TransformBy(Transform);
			}
			else
			{
				Cell.Bounds += Transform.GetLocation();
			}
		};

		for (FMantleEntity& Entity : Build.Statics)
		{
			const FMantleStaticMeshProps& Mesh = Entity.Props.Get<FMantleStaticMeshProps>();
			AddBox(Mesh.Mesh, Input, Entity.Transform);
			Cell.DrawDistance = FMath::Max(Cell.DrawDistance, Mesh.DrawDistance);
			Cell.Entities.Add(MoveTemp(Entity));
		}

		TArray<FString> SetKeys;
		Build.Sets.GetKeys(SetKeys);
		SetKeys.Sort();
		for (const FString& SetKey : SetKeys)
		{
			FCellSet& Set = Build.Sets.FindChecked(SetKey);
			if (Set.Instances.Num() == 0)
			{
				continue;
			}
			Cell.DrawDistance = FMath::Max(Cell.DrawDistance, Set.Mesh.DrawDistance);

			FBox Box(ForceInit);
			for (const FTransform& Instance : Set.Instances)
			{
				Box += Instance.GetLocation();
			}
			struct FOrdered { uint64 Code; double Numbers[9]; int32 Index; };
			TArray<FOrdered> Order;
			Order.Reserve(Set.Instances.Num());
			for (int32 InstanceIndex = 0; InstanceIndex < Set.Instances.Num(); ++InstanceIndex)
			{
				FOrdered& Item = Order.AddDefaulted_GetRef();
				Item.Code = FMantlePlaceExporter::MortonCode(Set.Instances[InstanceIndex].GetLocation(), Box);
				WrittenNumbers(Set.Instances[InstanceIndex], Item.Numbers);
				Item.Index = InstanceIndex;
			}
			// A pure function of the cell's own instances (stage 5 depends on it): Morton code over this cell's box, then the
			// written numbers in order. Never the order Unreal loaded actors in.
			Order.Sort([](const FOrdered& A, const FOrdered& B)
			{
				if (A.Code != B.Code)
				{
					return A.Code < B.Code;
				}
				for (int32 Number = 0; Number < 9; ++Number)
				{
					if (A.Numbers[Number] != B.Numbers[Number])
					{
						return A.Numbers[Number] < B.Numbers[Number];
					}
				}
				return false;
			});

			FString MeshName = Set.MeshName;
			if (MeshName.IsEmpty() && Set.FoldedNames.Num() > 0)
			{
				Set.FoldedNames.Sort();
				MeshName = Set.FoldedNames[0];
			}
			const int32 PerEntity = FMantlePlaceLimits::MaxInstancesPerEntity;
			const int32 Chunks = (Order.Num() + PerEntity - 1) / PerEntity;
			for (int32 Chunk = 0; Chunk < Chunks; ++Chunk)
			{
				const int32 First = Chunk * PerEntity;
				const int32 Count = FMath::Min(PerEntity, Order.Num() - First);
				FBox ChunkBox(ForceInit);
				for (int32 Offset = 0; Offset < Count; ++Offset)
				{
					ChunkBox += Set.Instances[Order[First + Offset].Index].GetLocation();
				}
				const FVector Center = ChunkBox.GetCenter();
				const FVector Anchor(FMath::RoundToDouble(Center.X), FMath::RoundToDouble(Center.Y), FMath::RoundToDouble(Center.Z));

				FMantleInstancedMeshProps Props;
				Props.Mesh = Set.Mesh;
				Props.Instances.Reserve(Count);
				for (int32 Offset = 0; Offset < Count; ++Offset)
				{
					const FTransform& Placed = Set.Instances[Order[First + Offset].Index];
					Props.Instances.Emplace(Placed.GetRotation(), Placed.GetLocation() - Anchor, Placed.GetScale3D());
					AddBox(Set.Mesh.Mesh, Input, Placed);
				}

				FMantleEntity Entity;
				Entity.Id = InstancedEntityId(SetKey, Key, Chunk);
				Entity.Type = EMantleEntityType::InstancedMesh;
				Entity.Name = FString::Printf(TEXT("%s (instanced %d/%d)"), *MeshName, Chunk + 1, Chunks).Left(FMantlePlaceLimits::MaxEntityNameLength);
				Entity.Transform = FTransform(Anchor);
				Entity.Props.Set<FMantleInstancedMeshProps>(MoveTemp(Props));
				Cell.Entities.Add(MoveTemp(Entity));
			}
		}

		Cell.Entities.Sort([](const FMantleEntity& A, const FMantleEntity& B) { return A.Id < B.Id; });
		if (Cell.Entities.Num() == 0)
		{
			continue;
		}
		Report.CellEntities += Cell.Entities.Num();
		Report.CellInstances += Cell.NumInstances();
		++Report.CellsPerLevel[FMath::Clamp(Key.Level, 0, FMantleCellGrid::MaxLevels - 1)];
		Plan.Cells.Add(MoveTemp(Cell));
	}

	// The entities that left, and the plain Groups they leave empty (5.3), nested ones too.
	TSet<FString> PlainGroupIds;
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		if (Entities[Index].Type == EMantleEntityType::Group && IsPlainGroup(Index))
		{
			PlainGroupIds.Add(Entities[Index].Id);
		}
	}
	TArray<FMantleEntity> Remaining;
	Remaining.Reserve(Entities.Num());
	TSet<FString> Emptied;
	for (int32 Index = 0; Index < Entities.Num(); ++Index)
	{
		if (Moved.Contains(Index))
		{
			const FString* Parent = MovedParents.Find(Index);
			if (Parent && !Parent->IsEmpty())
			{
				Emptied.Add(*Parent);
			}
			continue;
		}
		Remaining.Add(MoveTemp(Entities[Index]));
	}
	for (bool bChanged = true; bChanged;)
	{
		bChanged = false;
		TSet<FString> HasChild;
		for (const FMantleEntity& Entity : Remaining)
		{
			if (!Entity.Parent.IsEmpty())
			{
				HasChild.Add(Entity.Parent);
			}
		}
		for (int32 Index = 0; Index < Remaining.Num(); ++Index)
		{
			const FMantleEntity& Entity = Remaining[Index];
			if (Entity.Type == EMantleEntityType::Group && Emptied.Contains(Entity.Id) && PlainGroupIds.Contains(Entity.Id) && !HasChild.Contains(Entity.Id))
			{
				if (!Entity.Parent.IsEmpty())
				{
					Emptied.Add(Entity.Parent);
				}
				Remaining.RemoveAt(Index);
				++Report.DroppedGroups;
				bChanged = true;
				break;
			}
		}
	}
	Entities = MoveTemp(Remaining);

	Plan.Cells.Sort([](const FMantlePlannedCell& A, const FMantlePlannedCell& B) { return A.Key < B.Key; });
	TArray<int32> BySize;
	for (int32 Index = 0; Index < Plan.Cells.Num(); ++Index)
	{
		BySize.Add(Index);
	}
	BySize.Sort([&Plan](const int32 A, const int32 B)
	{
		const int32 Left = Plan.Cells[A].NumEntities();
		const int32 Right = Plan.Cells[B].NumEntities();
		return Left != Right ? Left > Right : Plan.Cells[A].Key < Plan.Cells[B].Key;
	});
	for (int32 Rank = 0; Rank < FMath::Min(5, BySize.Num()); ++Rank)
	{
		Report.BiggestCells.Add(Plan.Cells[BySize[Rank]].Key);
	}
	return Plan;
}
