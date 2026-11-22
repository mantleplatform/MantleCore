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
#include "HAL/IConsoleManager.h"
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

void FMantleDrawDistance::PlanStructures(TConstArrayView<FMantleStructureMember> Members, TConstArrayView<FMantleStructurePiece> Links,
	TArray<double>& OutMemberDistance, TArray<int32>* OutMemberStructure)
{
	using namespace MantleDrawDistancePrivate;
	int32 Owners = 0;
	for (const FMantleStructureMember& Member : Members)
	{
		Owners = FMath::Max(Owners, Member.Owner + 1);
	}
	TArray<bool> HasMember;
	HasMember.Init(false, Owners);
	for (const FMantleStructureMember& Member : Members)
	{
		if (Member.Owner >= 0 && Member.Band > 0.0)
		{
			HasMember[Member.Owner] = true;
		}
	}
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
		if (!Piece.Bounds.IsValid || Piece.Owner < 0 || Piece.Owner >= Owners || !HasMember[Piece.Owner])
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

	// Each structure's bottom; whether a big piece of it is raised over that; which pieces take its distance (big, and tall or raised);
	// their bands; then each member's distance.
	auto RootOf = [&Parent, Owners](const FMantleStructureMember& Member)
	{
		return Member.Owner >= 0 && Member.Owner < Owners ? FindRoot(Parent, Member.Owner) : INDEX_NONE;
	};
	TArray<double> Bottom;
	Bottom.Init(TNumericLimits<double>::Max(), Owners);
	for (const FMantleStructureMember& Member : Members)
	{
		const int32 Root = RootOf(Member);
		if (Root != INDEX_NONE && Member.Band > 0.0)
		{
			Bottom[Root] = FMath::Min(Bottom[Root], Member.Bottom);
		}
	}
	auto IsBig = [](const FMantleStructureMember& Member) { return Member.Band > 0.0 && Member.Radius >= LinkMinRadius; };
	TArray<bool> HasRaised;
	HasRaised.Init(false, Owners);
	for (const FMantleStructureMember& Member : Members)
	{
		const int32 Root = RootOf(Member);
		if (Root != INDEX_NONE && IsBig(Member) && Member.Bottom >= Bottom[Root] + StructureLiftCm)
		{
			HasRaised[Root] = true;
		}
	}
	TArray<bool> Takes;
	Takes.Init(false, Members.Num());
	TArray<double> StructureMin;
	TArray<double> StructureMax;
	StructureMin.Init(0.0, Owners);
	StructureMax.Init(0.0, Owners);
	for (int32 Index = 0; Index < Members.Num(); ++Index)
	{
		const FMantleStructureMember& Member = Members[Index];
		const int32 Root = RootOf(Member);
		if (Root == INDEX_NONE || !HasRaised[Root] || !IsBig(Member)
			|| (Member.Height < StructureTallCm && Member.Bottom < Bottom[Root] + StructureLiftCm))
		{
			continue;
		}
		Takes[Index] = true;
		StructureMin[Root] = StructureMin[Root] > 0.0 ? FMath::Min(StructureMin[Root], Member.Band) : Member.Band;
		StructureMax[Root] = FMath::Max(StructureMax[Root], Member.Band);
	}
	OutMemberDistance.SetNumUninitialized(Members.Num());
	for (int32 Index = 0; Index < Members.Num(); ++Index)
	{
		const FMantleStructureMember& Member = Members[Index];
		const int32 Root = RootOf(Member);
		const bool bMixed = Takes[Index] && StructureMin[Root] < StructureMax[Root];
		OutMemberDistance[Index] = !Takes[Index] ? Member.Band : bMixed ? FMath::Min(StructureMax[Root], StructureCapCm) : StructureMax[Root];
	}
	if (OutMemberStructure)
	{
		OutMemberStructure->SetNumUninitialized(Members.Num());
		for (int32 Index = 0; Index < Members.Num(); ++Index)
		{
			(*OutMemberStructure)[Index] = RootOf(Members[Index]);
		}
	}
}

namespace MantleDrawDistancePrivate
{
	/** Every banded mesh component of World with its owner, pieces (an instance each) and own band; the owners' distances and structures. */
	struct FStructurePlan
	{
		struct FMember
		{
			const UStaticMeshComponent* Component = nullptr;
			int32 Owner = INDEX_NONE;
			int32 Pieces = 1;
			double Band = 0.0;
		};
		TArray<FMember> Members;
		TArray<FMantleStructureMember> StructureMembers;
		int32 Owners = 0;
		TArray<double> Distances;
		TArray<int32> Structures;
		int32 LinkPieces = 0;
	};

	FStructurePlan PlanWorld(const UWorld* World)
	{
		FStructurePlan Plan;
		if (!World)
		{
			return Plan;
		}
		const ULevelInstanceSubsystem* LevelInstances = World->GetSubsystem<ULevelInstanceSubsystem>();
		TMap<const AActor*, int32> OwnerIndex;
		TArray<FMantleStructurePiece> Links;
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
				const FMantleDrawDistanceSource Source = FMantleDrawDistance::ReadComponent(Component, Mesh);
				const double Band = BandFor(Source.SphereRadius);
				if (Source.bNeverDistanceCull || HasOwnDistance(Source) || Band <= 0.0)
				{
					continue;
				}
				int32& Index = OwnerIndex.FindOrAdd(Outer, INDEX_NONE);
				if (Index == INDEX_NONE)
				{
					Index = Plan.Owners++;
				}
				const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component);
				Plan.Members.Add(FStructurePlan::FMember{ Component, Index, Instanced ? Instanced->GetInstanceCount() : 1, Band });
				// Its height at the scale its radius was measured at (the largest instance's), and its lowest point in the world.
				const FBoxSphereBounds MeshBounds = Mesh->GetBounds();
				const double Scale = MeshBounds.SphereRadius > 0.0 ? Source.SphereRadius / MeshBounds.SphereRadius : 1.0;
				FMantleStructureMember& Member = Plan.StructureMembers.AddDefaulted_GetRef();
				Member.Owner = Index;
				Member.Band = Band;
				Member.Radius = Source.SphereRadius;
				Member.Height = MeshBounds.BoxExtent.Z * 2.0 * Scale;
				Member.Bottom = Component->CalcBounds(Component->GetComponentTransform()).GetBox().Min.Z;
				if (Source.SphereRadius < FMantleDrawDistance::LinkMinRadius)
				{
					continue;
				}
				// Instances one by one (one set may be spread over a whole district); a plain mesh as itself.
				if (Instanced)
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
		Plan.LinkPieces = Links.Num();
		FMantleDrawDistance::PlanStructures(Plan.StructureMembers, Links, Plan.Distances, &Plan.Structures);
		return Plan;
	}

	/** "120m=N 250m=N ..." for the pieces at each distance. */
	FString BandLine(const TMap<int32, int64>& Pieces)
	{
		TArray<int32> Keys;
		Pieces.GetKeys(Keys);
		Keys.Sort();
		FString Line;
		for (const int32 Key : Keys)
		{
			Line += FString::Printf(TEXT("%s%dm=%lld"), Line.IsEmpty() ? TEXT("") : TEXT(" "), Key / 100, Pieces[Key]);
		}
		return Line;
	}
}

void FMantleDrawDistance::BuildStructureDistances(const UWorld* World, TMap<const UStaticMeshComponent*, double>& Out)
{
	using namespace MantleDrawDistancePrivate;
	Out.Reset();
	const FStructurePlan Plan = PlanWorld(World);
	int32 Changed = 0;
	for (int32 Index = 0; Index < Plan.Members.Num(); ++Index)
	{
		Out.Add(Plan.Members[Index].Component, Plan.Distances[Index]);
		Changed += Plan.Distances[Index] != Plan.Members[Index].Band ? 1 : 0;
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_STRUCTURE_DISTANCES owners %d, components %d, link pieces %d, components that now draw to another distance %d"),
		Plan.Owners, Plan.Members.Num(), Plan.LinkPieces, Changed);
}

void FMantleDrawDistance::LogStructureBands(const UWorld* World)
{
	using namespace MantleDrawDistancePrivate;
	const FStructurePlan Plan = PlanWorld(World);
	TMap<int32, int64> Before;
	TMap<int32, int64> After;
	TMap<int32, int64> StructurePieces;
	TMap<int32, TSet<int32>> StructureOwners;
	TMap<int32, double> StructureDistance;
	for (int32 Index = 0; Index < Plan.Members.Num(); ++Index)
	{
		const FStructurePlan::FMember& Member = Plan.Members[Index];
		Before.FindOrAdd(FMath::RoundToInt32(Member.Band)) += Member.Pieces;
		After.FindOrAdd(FMath::RoundToInt32(Plan.Distances[Index])) += Member.Pieces;
		// A structure is measured by the pieces it moved to another distance.
		if (Plan.Distances[Index] == Member.Band)
		{
			continue;
		}
		const int32 Structure = Plan.Structures[Index];
		StructureDistance.FindOrAdd(Structure) = Plan.Distances[Index];
		StructurePieces.FindOrAdd(Structure) += Member.Pieces;
		StructureOwners.FindOrAdd(Structure).Add(Member.Owner);
	}
	int32 Largest = INDEX_NONE;
	for (const TPair<int32, int64>& Pair : StructurePieces)
	{
		if (Largest == INDEX_NONE || Pair.Value > StructurePieces[Largest])
		{
			Largest = Pair.Key;
		}
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_STRUCTURE_BANDS before %s | after %s | structures that moved pieces %d, largest %d owners %lld pieces moved (to %.0f m)"),
		*BandLine(Before), *BandLine(After), StructurePieces.Num(), Largest != INDEX_NONE ? StructureOwners[Largest].Num() : 0,
		Largest != INDEX_NONE ? StructurePieces[Largest] : 0ll, Largest != INDEX_NONE ? StructureDistance[Largest] / 100.0 : 0.0);
}

namespace MantleDrawDistancePrivate
{
	FAutoConsoleCommandWithWorld GStructureDistancesCommand(
		TEXT("Mantle.StructureDistances"),
		TEXT("Logs what the one-distance-per-structure rule does to the level (MANTLE_STRUCTURE_BANDS), without exporting."),
		FConsoleCommandWithWorldDelegate::CreateStatic([](UWorld* World) { FMantleDrawDistance::LogStructureBands(World); }));
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
