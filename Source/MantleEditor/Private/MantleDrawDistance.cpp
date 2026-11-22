// Copyright Joshua Eger. All Rights Reserved.
#include "MantleDrawDistance.h"

#include "MantleClientFeatures.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceLimits.h"
#include "MantleRuntime.h"
#include "MantleTargetFeatures.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "LevelInstance/LevelInstanceInterface.h"
#include "LevelInstance/LevelInstanceSubsystem.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDrawDistancePrivate
{
	/**
	 * Radius below (cm), draw distance. A prop at the top of a band (30 cm radius) is still about 12 pixels across at
	 * its 60 m on a 1080p screen, and one at 3 m radius at 250 m, so nothing pops where a player looks.
	 *
	 * The last band is the big scenery of a large place: ground tiles, whole-building collision, roofs and freeway decks of 10 to
	 * 100 m radius. They draw to 2.5 km, the default cell grid's farthest level (FMantleCellGrid::Default), so they stream in its
	 * 1 km cells instead of all loading with the root. Without it City Sample's Big City kept ~183,000 of them in the root, which
	 * holds at most 100,000 instanced meshes and 8 MiB. A mesh of 100 m radius or more is still never culled.
	 */
	const FMantleDrawDistance::FBand GBands[] = {
		{ 30.0, 6000.0 },
		{ 100.0, 12000.0 },
		{ 300.0, 25000.0 },
		{ 1000.0, 60000.0 },
		{ 10000.0, 250000.0 },
	};

	/** The distance in cm; 0 past MaxDrawDistance (then the mesh is never culled). */
	double InRange(const double Distance)
	{
		return Distance > FMantlePlaceLimits::MaxDrawDistance ? 0.0 : Distance;
	}

	/** The size band's distance for a bounding sphere radius, 0 past the last band (never culled). */
	double BandFor(const double Radius)
	{
		for (const FMantleDrawDistance::FBand& Band : FMantleDrawDistance::Bands())
		{
			if (Radius < Band.MaxRadius)
			{
				return Band.Draw;
			}
		}
		return 0.0;
	}

	/** Whether the component sets its own distance (it keeps it; the structure rule leaves it alone). */
	bool HasOwnDistance(const FMantleDrawDistanceSource& Source)
	{
		return (Source.bInstanced && Source.InstanceEndCullDistance > 0) || Source.LDMaxDrawDistance > 0.0 || Source.CachedMaxDrawDistance > 0.0;
	}

	/** The structure distances ReadComponent looks in while an FStructureScope is alive (the exporter runs on the game thread). */
	const TMap<const UStaticMeshComponent*, double>* GStructureDistances = nullptr;

	/** The grid PlanStructures links pieces in: a piece is looked at, clipped, in each cell its footprint covers. */
	constexpr double StructureCellCm = 3200.0;

	int32 FindRoot(TArray<int32>& Parent, int32 Index)
	{
		while (Parent[Index] != Index)
		{
			Parent[Index] = Parent[Parent[Index]];
			Index = Parent[Index];
		}
		return Index;
	}

	/**
	 * Whether Lower holds Upper up: Upper is raised over Lower's bottom, Lower reaches its underside, and their footprints overlap by at
	 * least a quarter of the smaller one.
	 */
	bool HoldsUp(const FBox& Lower, const FBox& Upper)
	{
		if (Upper.Min.Z < Lower.Min.Z + FMantleDrawDistance::StructureLiftCm || Lower.Max.Z < Upper.Min.Z - FMantleDrawDistance::StructureReachCm)
		{
			return false;
		}
		const double OverlapX = FMath::Min(Lower.Max.X, Upper.Max.X) - FMath::Max(Lower.Min.X, Upper.Min.X);
		const double OverlapY = FMath::Min(Lower.Max.Y, Upper.Max.Y) - FMath::Max(Lower.Min.Y, Upper.Min.Y);
		if (OverlapX <= 0.0 || OverlapY <= 0.0)
		{
			return false;
		}
		const double LowerArea = FMath::Max(Lower.Max.X - Lower.Min.X, 1.0) * FMath::Max(Lower.Max.Y - Lower.Min.Y, 1.0);
		const double UpperArea = FMath::Max(Upper.Max.X - Upper.Min.X, 1.0) * FMath::Max(Upper.Max.Y - Upper.Min.Y, 1.0);
		return OverlapX * OverlapY >= 0.25 * FMath::Min(LowerArea, UpperArea);
	}
}

TConstArrayView<FMantleDrawDistance::FBand> FMantleDrawDistance::Bands()
{
	return MakeArrayView(MantleDrawDistancePrivate::GBands);
}

double FMantleDrawDistance::NeverCullRadius()
{
	return MantleDrawDistancePrivate::GBands[UE_ARRAY_COUNT(MantleDrawDistancePrivate::GBands) - 1].MaxRadius;
}

bool FMantleDrawDistance::Apply(const FMantleDrawDistanceSource& Source, const FMantleTargetFeatures& Target, FMantleStaticMeshProps& Props)
{
	using namespace MantleDrawDistancePrivate;
	Props.DrawDistance = 0.0;
	if (!Target.Reads(MantleClientFeatures::DrawDistance) || Source.bNeverDistanceCull)
	{
		return false;
	}

	// The component's own: the end cull distance of instances, else Desired Max Draw Distance, else the current one. Never 0 (refused).
	double Sourced = 0.0;
	if (Source.bInstanced && Source.InstanceEndCullDistance > 0)
	{
		Sourced = Source.InstanceEndCullDistance;
	}
	else if (Source.LDMaxDrawDistance > 0.0)
	{
		Sourced = Source.LDMaxDrawDistance;
	}
	else if (Source.CachedMaxDrawDistance > 0.0)
	{
		Sourced = Source.CachedMaxDrawDistance;
	}

	if (Sourced > 0.0)
	{
		Props.DrawDistance = InRange(Sourced);
	}
	else if (Source.StructureDistance > 0.0)
	{
		// The structure it is part of draws to one distance (PlanStructures).
		Props.DrawDistance = InRange(Source.StructureDistance);
	}
	else
	{
		// The size band, by the mesh's bounding sphere; a mesh past the last one has none.
		for (const FBand& Band : Bands())
		{
			if (Source.SphereRadius < Band.MaxRadius)
			{
				Props.DrawDistance = Band.Draw;
				break;
			}
		}
	}
	return Props.DrawDistance > 0.0;
}

FMantleDrawDistanceSource FMantleDrawDistance::ReadComponent(const UStaticMeshComponent* Component, const UStaticMesh* Mesh)
{
	FMantleDrawDistanceSource Source;
	if (!Component)
	{
		return Source;
	}
	Source.LDMaxDrawDistance = Component->LDMaxDrawDistance;
	Source.CachedMaxDrawDistance = Component->CachedMaxDrawDistance;
	Source.bNeverDistanceCull = Component->bNeverDistanceCull;

	if (!Mesh)
	{
		Mesh = Component->GetStaticMesh();
	}
	double Scale = Component->GetComponentScale().GetAbsMax();
	if (const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component))
	{
		Source.bInstanced = true;
		Source.InstanceEndCullDistance = Instanced->InstanceEndCullDistance;

		// One component, one distance: sized by its largest instance, so a big one is not culled at a small one's distance.
		double LargestInstance = 0.0;
		for (int32 Index = 0; Index < Instanced->GetInstanceCount(); ++Index)
		{
			FTransform Instance;
			if (Instanced->GetInstanceTransform(Index, Instance, /*bWorldSpace*/ false))
			{
				LargestInstance = FMath::Max(LargestInstance, Instance.GetScale3D().GetAbsMax());
			}
		}
		Scale *= FMath::Max(LargestInstance, KINDA_SMALL_NUMBER);
	}
	Source.SphereRadius = Mesh ? Mesh->GetBounds().SphereRadius * Scale : 0.0;
	if (MantleDrawDistancePrivate::GStructureDistances)
	{
		if (const double* Structure = MantleDrawDistancePrivate::GStructureDistances->Find(Component))
		{
			Source.StructureDistance = *Structure;
		}
	}
	return Source;
}

void FMantleDrawDistance::PlanStructures(TConstArrayView<double> OwnerMinBand, TConstArrayView<double> OwnerMaxBand, TConstArrayView<FMantleStructurePiece> Links,
	TArray<double>& OutOwnerDistance)
{
	using namespace MantleDrawDistancePrivate;
	const int32 Owners = FMath::Min(OwnerMinBand.Num(), OwnerMaxBand.Num());
	TArray<int32> Parent;
	Parent.SetNumUninitialized(Owners);
	for (int32 Index = 0; Index < Owners; ++Index)
	{
		Parent[Index] = Index;
	}

	// Each owner's pieces, merged per grid cell and clipped to it: pieces spread over the map (trees, sidewalks) are only looked at
	// where they are, and a cell holds a few owners rather than thousands of instances.
	TMap<FIntVector, FBox> PerCellOwner;
	for (const FMantleStructurePiece& Piece : Links)
	{
		if (!Piece.Bounds.IsValid || Piece.Owner < 0 || Piece.Owner >= Owners || OwnerMaxBand[Piece.Owner] <= 0.0)
		{
			continue;
		}
		const int32 X0 = FMath::FloorToInt32(Piece.Bounds.Min.X / StructureCellCm);
		const int32 X1 = FMath::FloorToInt32(Piece.Bounds.Max.X / StructureCellCm);
		const int32 Y0 = FMath::FloorToInt32(Piece.Bounds.Min.Y / StructureCellCm);
		const int32 Y1 = FMath::FloorToInt32(Piece.Bounds.Max.Y / StructureCellCm);
		for (int32 X = X0; X <= X1; ++X)
		{
			for (int32 Y = Y0; Y <= Y1; ++Y)
			{
				FBox Clipped = Piece.Bounds;
				Clipped.Min.X = FMath::Max(Clipped.Min.X, X * StructureCellCm);
				Clipped.Max.X = FMath::Min(Clipped.Max.X, (X + 1) * StructureCellCm);
				Clipped.Min.Y = FMath::Max(Clipped.Min.Y, Y * StructureCellCm);
				Clipped.Max.Y = FMath::Min(Clipped.Max.Y, (Y + 1) * StructureCellCm);
				PerCellOwner.FindOrAdd(FIntVector(X, Y, Piece.Owner), FBox(ForceInit)) += Clipped;
			}
		}
	}
	TMap<FIntPoint, TArray<TPair<int32, FBox>>> Cells;
	for (const TPair<FIntVector, FBox>& Entry : PerCellOwner)
	{
		Cells.FindOrAdd(FIntPoint(Entry.Key.X, Entry.Key.Y)).Emplace(Entry.Key.Z, Entry.Value);
	}
	for (const TPair<FIntPoint, TArray<TPair<int32, FBox>>>& Cell : Cells)
	{
		const TArray<TPair<int32, FBox>>& Items = Cell.Value;
		for (int32 A = 0; A < Items.Num(); ++A)
		{
			for (int32 B = A + 1; B < Items.Num(); ++B)
			{
				const int32 RootA = FindRoot(Parent, Items[A].Key);
				const int32 RootB = FindRoot(Parent, Items[B].Key);
				if (RootA != RootB && (HoldsUp(Items[A].Value, Items[B].Value) || HoldsUp(Items[B].Value, Items[A].Value)))
				{
					Parent[RootB] = RootA;
				}
			}
		}
	}

	// Each structure's bands, then each owner's distance.
	TArray<double> StructureMin;
	TArray<double> StructureMax;
	StructureMin.Init(0.0, Owners);
	StructureMax.Init(0.0, Owners);
	for (int32 Index = 0; Index < Owners; ++Index)
	{
		if (OwnerMaxBand[Index] <= 0.0)
		{
			continue;
		}
		const int32 Root = FindRoot(Parent, Index);
		StructureMin[Root] = StructureMin[Root] > 0.0 ? FMath::Min(StructureMin[Root], OwnerMinBand[Index]) : OwnerMinBand[Index];
		StructureMax[Root] = FMath::Max(StructureMax[Root], OwnerMaxBand[Index]);
	}
	OutOwnerDistance.Init(0.0, Owners);
	for (int32 Index = 0; Index < Owners; ++Index)
	{
		if (OwnerMaxBand[Index] <= 0.0)
		{
			continue;
		}
		const int32 Root = FindRoot(Parent, Index);
		const bool bMixed = StructureMin[Root] < StructureMax[Root];
		OutOwnerDistance[Index] = bMixed ? FMath::Min(StructureMax[Root], StructureCapCm) : StructureMax[Root];
	}
}

void FMantleDrawDistance::BuildStructureDistances(const UWorld* World, TMap<const UStaticMeshComponent*, double>& Out)
{
	using namespace MantleDrawDistancePrivate;
	Out.Reset();
	if (!World)
	{
		return;
	}
	const ULevelInstanceSubsystem* LevelInstances = World->GetSubsystem<ULevelInstanceSubsystem>();
	TMap<const AActor*, int32> OwnerIndex;
	TArray<double> MinBand;
	TArray<double> MaxBand;
	TArray<FMantleStructurePiece> Links;
	TArray<TPair<const UStaticMeshComponent*, int32>> Members;
	for (TActorIterator<AActor> It(const_cast<UWorld*>(World)); It; ++It)
	{
		const AActor* Actor = *It;
		// The outermost actor: a Level Instance's actors publish on their own, but they are one structure with the instance.
		const AActor* Outer = Actor;
		for (int32 Depth = 0; LevelInstances && Depth < 16; ++Depth)
		{
			const ILevelInstanceInterface* Parent = LevelInstances->GetOwningLevelInstance(Outer->GetLevel());
			const AActor* ParentActor = Parent ? Cast<AActor>(Parent) : nullptr;
			if (!ParentActor || ParentActor == Outer)
			{
				break;
			}
			Outer = ParentActor;
		}
		TInlineComponentArray<UStaticMeshComponent*> Components(Actor);
		for (const UStaticMeshComponent* Component : Components)
		{
			const UStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
			if (!Mesh || !FMantlePlaceExporter::IsShownInGame(Component))
			{
				continue;
			}
			const FMantleDrawDistanceSource Source = ReadComponent(Component, Mesh);
			const double Band = BandFor(Source.SphereRadius);
			if (Source.bNeverDistanceCull || HasOwnDistance(Source) || Band <= 0.0)
			{
				continue;
			}
			int32& Index = OwnerIndex.FindOrAdd(Outer, INDEX_NONE);
			if (Index == INDEX_NONE)
			{
				Index = MinBand.Add(Band);
				MaxBand.Add(Band);
			}
			else
			{
				MinBand[Index] = FMath::Min(MinBand[Index], Band);
				MaxBand[Index] = FMath::Max(MaxBand[Index], Band);
			}
			Members.Emplace(Component, Index);
			if (Source.SphereRadius < LinkMinRadius)
			{
				continue;
			}
			// Instances one by one (one set may be spread over a whole district); a plain mesh as itself.
			if (const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component))
			{
				const FBox MeshBox = Mesh->GetBounds().GetBox();
				for (int32 At = 0; At < Instanced->GetInstanceCount(); ++At)
				{
					FTransform Instance;
					if (Instanced->GetInstanceTransform(At, Instance, /*bWorldSpace*/ true))
					{
						Links.Add(FMantleStructurePiece{ Index, MeshBox.TransformBy(Instance) });
					}
				}
			}
			else
			{
				Links.Add(FMantleStructurePiece{ Index, Component->CalcBounds(Component->GetComponentTransform()).GetBox() });
			}
		}
	}
	TArray<double> Distances;
	PlanStructures(MinBand, MaxBand, Links, Distances);
	int32 Changed = 0;
	for (int32 Index = 0; Index < Distances.Num(); ++Index)
	{
		Changed += Distances[Index] != MaxBand[Index] || Distances[Index] != MinBand[Index] ? 1 : 0;
	}
	for (const TPair<const UStaticMeshComponent*, int32>& Member : Members)
	{
		Out.Add(Member.Key, Distances[Member.Value]);
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_STRUCTURE_DISTANCES owners %d, components %d, link pieces %d, owners whose pieces now draw to another distance %d"),
		MinBand.Num(), Members.Num(), Links.Num(), Changed);
}

FMantleDrawDistance::FStructureScope::FStructureScope(const UWorld* World)
{
	BuildStructureDistances(World, Distances);
	Previous = MantleDrawDistancePrivate::GStructureDistances;
	MantleDrawDistancePrivate::GStructureDistances = &Distances;
}

FMantleDrawDistance::FStructureScope::~FStructureScope()
{
	MantleDrawDistancePrivate::GStructureDistances = Previous;
}

int32 FMantleDrawDistance::CountEntities(const FMantlePlace& Place)
{
	int32 Count = 0;
	for (const FMantleEntity& Entity : Place.Entities)
	{
		const FMantleStaticMeshProps* Mesh = Entity.Props.TryGet<FMantleStaticMeshProps>();
		if (const FMantleInstancedMeshProps* Instanced = Entity.Props.TryGet<FMantleInstancedMeshProps>())
		{
			Mesh = &Instanced->Mesh;
		}
		Count += Mesh && Mesh->DrawDistance > 0.0 ? 1 : 0;
	}
	return Count;
}

bool FMantleDrawDistance::HasMeshEntities(const FMantlePlace& Place)
{
	for (const FMantleEntity& Entity : Place.Entities)
	{
		if (Entity.Props.TryGet<FMantleStaticMeshProps>() || Entity.Props.TryGet<FMantleInstancedMeshProps>())
		{
			return true;
		}
	}
	return false;
}
