// Copyright Joshua Eger. All Rights Reserved.
#include "MantleDrawDistance.h"

#include "MantleClientFeatures.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceLimits.h"
#include "MantleRuntime.h"
#include "MantleTargetFeatures.h"

#include "Algo/BinarySearch.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
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

	/**
	 * Whether Holder holds Piece up in the pull-down: Holder's bottom is under Piece's, its top within StructureReachCm under to
	 * PullDownAboveCm over Piece's underside, and their footprints overlap by a quarter of the smaller one. (Unlike HoldsUp, a thin roof
	 * slab holds up what stands on it.)
	 */
	bool RestsOn(const FBox3f& Holder, const FBox3f& Piece)
	{
		if (Holder.Min.Z >= Piece.Min.Z || Holder.Max.Z < Piece.Min.Z - FMantleDrawDistance::StructureReachCm
			|| Holder.Max.Z > Piece.Min.Z + FMantleDrawDistance::PullDownAboveCm)
		{
			return false;
		}
		const double OverlapX = FMath::Min(Holder.Max.X, Piece.Max.X) - FMath::Max(Holder.Min.X, Piece.Min.X);
		const double OverlapY = FMath::Min(Holder.Max.Y, Piece.Max.Y) - FMath::Max(Holder.Min.Y, Piece.Min.Y);
		if (OverlapX <= 0.0 || OverlapY <= 0.0)
		{
			return false;
		}
		const double HolderArea = FMath::Max(static_cast<double>(Holder.Max.X - Holder.Min.X), 1.0) * FMath::Max(static_cast<double>(Holder.Max.Y - Holder.Min.Y), 1.0);
		const double PieceArea = FMath::Max(static_cast<double>(Piece.Max.X - Piece.Min.X), 1.0) * FMath::Max(static_cast<double>(Piece.Max.Y - Piece.Min.Y), 1.0);
		return OverlapX * OverlapY >= 0.25 * FMath::Min(HolderArea, PieceArea);
	}

	/** The pull-down's grid cells are numbered from -PullCellBias to PullCellBias - 1 each way (32 m cells: about 1,000 km). */
	constexpr int64 PullCellBias = 1 << 15;

	/** The pull-down gives up after this many passes (each carries a lowered distance at least one piece further up). */
	constexpr int32 PullMaxPasses = 1000;

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
		const double OverlapX = static_cast<double>(FMath::Min(Lower.Max.X, Upper.Max.X) - FMath::Max(Lower.Min.X, Upper.Min.X));
		const double OverlapY = static_cast<double>(FMath::Min(Lower.Max.Y, Upper.Max.Y) - FMath::Max(Lower.Min.Y, Upper.Min.Y));
		if (OverlapX <= 0.0 || OverlapY <= 0.0)
		{
			return false;
		}
		const double LowerArea = FMath::Max(static_cast<double>(Lower.Max.X - Lower.Min.X), 1.0) * FMath::Max(static_cast<double>(Lower.Max.Y - Lower.Min.Y), 1.0);
		const double UpperArea = FMath::Max(static_cast<double>(Upper.Max.X - Upper.Min.X), 1.0) * FMath::Max(static_cast<double>(Upper.Max.Y - Upper.Min.Y), 1.0);
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

int32 FMantleDrawDistance::PullDownToSupports(TConstArrayView<FMantleHeldPiece> Pieces, TArrayView<double> Distances, TConstArrayView<bool> Movable,
	TArray<double>* OutPieceLimit)
{
	using namespace MantleDrawDistancePrivate;
	const int32 Components = Distances.Num();
	auto Reach = [](const double Distance) { return Distance > 0.0 ? Distance : TNumericLimits<double>::Max(); };
	auto IsMovable = [&Movable](const int32 Component) { return Movable.IsValidIndex(Component) && Movable[Component]; };
	auto CellOf = [](const float Value)
	{
		return static_cast<uint64>(FMath::Clamp<int64>(FMath::FloorToInt64(Value / StructureCellCm), -PullCellBias, PullCellBias - 1) + PullCellBias);
	};

	// Every piece in each grid cell its footprint covers, as (cell, piece) keys: sorted, a cell's pieces are together.
	TArray<uint64> Entries;
	Entries.Reserve(Pieces.Num() + Pieces.Num() / 2);
	for (int32 Index = 0; Index < Pieces.Num(); ++Index)
	{
		const FMantleHeldPiece& Piece = Pieces[Index];
		if (!Piece.Bounds.IsValid || Piece.Component < 0 || Piece.Component >= Components)
		{
			continue;
		}
		const uint64 X1 = CellOf(Piece.Bounds.Max.X);
		const uint64 Y1 = CellOf(Piece.Bounds.Max.Y);
		for (uint64 X = CellOf(Piece.Bounds.Min.X); X <= X1; ++X)
		{
			for (uint64 Y = CellOf(Piece.Bounds.Min.Y); Y <= Y1; ++Y)
			{
				Entries.Add((X << 48) | (Y << 32) | static_cast<uint32>(Index));
			}
		}
	}
	Entries.Sort();

	// What holds each movable piece up: the pieces whose top is near its underside, looked at in the cell where the two footprints'
	// overlap starts (so each pair once). (held piece, holding component) keys.
	TArray<uint64> Held;
	TArray<TPair<float, int32>> ByTop;
	for (int32 Start = 0; Start < Entries.Num();)
	{
		const uint64 Cell = Entries[Start] >> 32;
		ByTop.Reset();
		int32 End = Start;
		for (; End < Entries.Num() && (Entries[End] >> 32) == Cell; ++End)
		{
			const int32 Index = static_cast<int32>(Entries[End] & 0xffffffffull);
			ByTop.Emplace(Pieces[Index].Bounds.Max.Z, Index);
		}
		Start = End;
		if (ByTop.Num() < 2)
		{
			continue;
		}
		ByTop.Sort([](const TPair<float, int32>& A, const TPair<float, int32>& B) { return A.Key < B.Key; });
		const uint64 CellX = Cell >> 16;
		const uint64 CellY = Cell & 0xffffull;
		// Only a piece raised over the cell's ground (its lowest piece) is held up: a lamp on the street is not held by the curb.
		float Ground = TNumericLimits<float>::Max();
		for (const TPair<float, int32>& Entry : ByTop)
		{
			Ground = FMath::Min(Ground, Pieces[Entry.Value].Bounds.Min.Z);
		}
		for (const TPair<float, int32>& Upper : ByTop)
		{
			const FMantleHeldPiece& Piece = Pieces[Upper.Value];
			if (!IsMovable(Piece.Component) || Piece.Bounds.Min.Z < Ground + StructureLiftCm)
			{
				continue;
			}
			const float From = Piece.Bounds.Min.Z - static_cast<float>(StructureReachCm);
			const float To = Piece.Bounds.Min.Z + static_cast<float>(PullDownAboveCm);
			for (int32 At = Algo::LowerBoundBy(ByTop, From, [](const TPair<float, int32>& Entry) { return Entry.Key; });
				At < ByTop.Num() && ByTop[At].Key <= To; ++At)
			{
				const int32 Under = ByTop[At].Value;
				const FMantleHeldPiece& Holder = Pieces[Under];
				if (Under == Upper.Value || CellOf(FMath::Max(Piece.Bounds.Min.X, Holder.Bounds.Min.X)) != CellX
					|| CellOf(FMath::Max(Piece.Bounds.Min.Y, Holder.Bounds.Min.Y)) != CellY || !RestsOn(Holder.Bounds, Piece.Bounds))
				{
					continue;
				}
				Held.Add((static_cast<uint64>(Upper.Value) << 32) | static_cast<uint32>(Holder.Component));
			}
		}
	}
	Entries.Empty();
	Held.Sort();
	int32 Kept = 0;
	for (int32 Index = 0; Index < Held.Num(); ++Index)
	{
		if (Kept == 0 || Held[Index] != Held[Kept - 1])
		{
			Held[Kept++] = Held[Index];
		}
	}
	Held.SetNum(Kept);

	// Each held piece's holders, lowest pieces first, so a change carries up a tower in few passes.
	struct FHeldRange
	{
		int32 Piece;
		int32 Begin;
		int32 End;
	};
	TArray<FHeldRange> Ranges;
	for (int32 Index = 0; Index < Held.Num();)
	{
		const int32 Piece = static_cast<int32>(Held[Index] >> 32);
		int32 End = Index;
		while (End < Held.Num() && static_cast<int32>(Held[End] >> 32) == Piece)
		{
			++End;
		}
		Ranges.Add({ Piece, Index, End });
		Index = End;
	}
	Ranges.Sort([&Pieces](const FHeldRange& A, const FHeldRange& B) { return Pieces[A.Piece].Bounds.Min.Z < Pieces[B.Piece].Bounds.Min.Z; });
	auto LimitOf = [&Held, &Distances, &Reach](const FHeldRange& Range)
	{
		double Limit = 0.0;
		for (int32 At = Range.Begin; At < Range.End; ++At)
		{
			Limit = FMath::Max(Limit, Reach(Distances[static_cast<int32>(Held[At] & 0xffffffffull)]));
		}
		return Limit;
	};

	// Distances only come down, to a holder's distance, so this ends.
	const TArray<double> Before(Distances.GetData(), Distances.Num());
	int32 Passes = 0;
	bool bChanged = true;
	while (bChanged && Passes < PullMaxPasses)
	{
		bChanged = false;
		++Passes;
		for (const FHeldRange& Range : Ranges)
		{
			double& Own = Distances[Pieces[Range.Piece].Component];
			const double Limit = LimitOf(Range);
			if (Limit < Reach(Own))
			{
				Own = Limit;
				bChanged = true;
			}
		}
	}
	if (bChanged)
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_STRUCTURE_PULLDOWN stopped after %d passes with distances still coming down"), Passes);
	}
	if (OutPieceLimit)
	{
		OutPieceLimit->Init(0.0, Pieces.Num());
		for (const FHeldRange& Range : Ranges)
		{
			(*OutPieceLimit)[Range.Piece] = LimitOf(Range);
		}
	}
	int32 Lowered = 0;
	for (int32 Index = 0; Index < Components; ++Index)
	{
		Lowered += Distances[Index] != Before[Index] ? 1 : 0;
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_STRUCTURE_PULLDOWN pieces %d, held up %d (%d holder pairs), components lowered %d, passes %d"),
		Pieces.Num(), Ranges.Num(), Held.Num(), Lowered, Passes);
	return Lowered;
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
		/** Each member's distance by the structure rule alone (PlanStructures), and after the pull-down (PullDownToSupports). */
		TArray<double> StructureDistances;
		TArray<double> Distances;
		TArray<int32> Structures;
		int32 LinkPieces = 0;
		int32 Lowered = 0;
		/** The components that only hold others up (their own distance or never culled), numbered after the members, and their distances. */
		TArray<const UStaticMeshComponent*> Others;
		TArray<double> OtherDistances;
		/** When kept: every piece (Component: a member's number, or Members.Num() + an other's) and its limit (PullDownToSupports). */
		TArray<FMantleHeldPiece> Pieces;
		TArray<double> PieceLimits;
		double Seconds = 0.0;
	};

	/** A component's fixed distance when the rules leave it alone: its own, else none (never culled, or past the bands). */
	double FixedDistance(const FMantleDrawDistanceSource& Source)
	{
		if (Source.bNeverDistanceCull)
		{
			return 0.0;
		}
		const double Own = Source.bInstanced && Source.InstanceEndCullDistance > 0 ? static_cast<double>(Source.InstanceEndCullDistance)
			: Source.LDMaxDrawDistance > 0.0 ? Source.LDMaxDrawDistance : Source.CachedMaxDrawDistance;
		return Own > 0.0 ? InRange(Own) : 0.0;
	}

	FStructurePlan PlanWorld(const UWorld* World, const bool bKeepPieces)
	{
		FStructurePlan Plan;
		if (!World)
		{
			return Plan;
		}
		const double StartSeconds = FPlatformTime::Seconds();
		const ULevelInstanceSubsystem* LevelInstances = World->GetSubsystem<ULevelInstanceSubsystem>();
		TMap<const AActor*, int32> OwnerIndex;
		TArray<FMantleStructurePiece> Links;
		// Component numbers: a member's, or OtherFlag and an other's until the members are counted.
		constexpr int32 OtherFlag = 1 << 30;
		TArray<FMantleHeldPiece> Pieces;
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
				const bool bMember = !Source.bNeverDistanceCull && !HasOwnDistance(Source) && Band > 0.0;
				const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component);
				int32 Number = INDEX_NONE;
				int32 Owner = INDEX_NONE;
				if (bMember)
				{
					int32& Index = OwnerIndex.FindOrAdd(Outer, INDEX_NONE);
					if (Index == INDEX_NONE)
					{
						Index = Plan.Owners++;
					}
					Owner = Index;
					Number = Plan.Members.Num();
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
				}
				else
				{
					Number = OtherFlag | Plan.Others.Num();
					Plan.Others.Add(Component);
					Plan.OtherDistances.Add(FixedDistance(Source));
				}
				// Every piece holds up or is held (the pull-down); the members' big ones also link structures. Instances one by one (one
				// set may be spread over a whole district); a plain mesh as itself.
				const bool bLink = bMember && Source.SphereRadius >= FMantleDrawDistance::LinkMinRadius;
				auto AddPiece = [&](const FBox& Box)
				{
					if (!Box.IsValid)
					{
						return;
					}
					Pieces.Add(FMantleHeldPiece{ Number, FBox3f(Box) });
					if (bLink)
					{
						Links.Add(FMantleStructurePiece{ Owner, Box });
					}
				};
				if (Instanced)
				{
					const FBox MeshBox = Mesh->GetBounds().GetBox();
					for (int32 At = 0; At < Instanced->GetInstanceCount(); ++At)
					{
						FTransform Instance;
						if (Instanced->GetInstanceTransform(At, Instance, /*bWorldSpace*/ true))
						{
							AddPiece(MeshBox.TransformBy(Instance));
						}
					}
				}
				else
				{
					AddPiece(Component->CalcBounds(Component->GetComponentTransform()).GetBox());
				}
			}
		}
		Plan.LinkPieces = Links.Num();
		FMantleDrawDistance::PlanStructures(Plan.StructureMembers, Links, Plan.StructureDistances, &Plan.Structures);
		Links.Empty();

		// Then every raised piece comes down to what holds it up.
		const int32 MemberCount = Plan.Members.Num();
		for (FMantleHeldPiece& Piece : Pieces)
		{
			if (Piece.Component & OtherFlag)
			{
				Piece.Component = MemberCount + (Piece.Component & ~OtherFlag);
			}
		}
		TArray<double> All = Plan.StructureDistances;
		All.Append(Plan.OtherDistances);
		TArray<bool> Movable;
		Movable.Init(false, All.Num());
		for (int32 Index = 0; Index < MemberCount; ++Index)
		{
			Movable[Index] = true;
		}
		Plan.Lowered = FMantleDrawDistance::PullDownToSupports(Pieces, All, Movable, bKeepPieces ? &Plan.PieceLimits : nullptr);
		Plan.Distances = TArray<double>(All.GetData(), MemberCount);
		if (bKeepPieces)
		{
			Plan.Pieces = MoveTemp(Pieces);
		}
		Plan.Seconds = FPlatformTime::Seconds() - StartSeconds;
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
	const FStructurePlan Plan = PlanWorld(World, /*bKeepPieces*/ false);
	int32 Changed = 0;
	for (int32 Index = 0; Index < Plan.Members.Num(); ++Index)
	{
		Out.Add(Plan.Members[Index].Component, Plan.Distances[Index]);
		Changed += Plan.Distances[Index] != Plan.Members[Index].Band ? 1 : 0;
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_STRUCTURE_DISTANCES owners %d, components %d, link pieces %d, components that now draw to another distance %d (pulled down to what holds them up %d), %.1f s"),
		Plan.Owners, Plan.Members.Num(), Plan.LinkPieces, Changed, Plan.Lowered, Plan.Seconds);
}

void FMantleDrawDistance::LogStructureBands(const UWorld* World, TConstArrayView<FString> Meshes)
{
	using namespace MantleDrawDistancePrivate;
	const FStructurePlan Plan = PlanWorld(World, /*bKeepPieces*/ true);
	const int32 MemberCount = Plan.Members.Num();
	TMap<int32, int64> Before;
	TMap<int32, int64> Structured;
	TMap<int32, int64> After;
	TMap<int32, int64> StructurePieces;
	TMap<int32, TSet<int32>> StructureOwners;
	TMap<int32, double> StructureDistance;
	int64 LoweredPieces = 0;
	for (int32 Index = 0; Index < MemberCount; ++Index)
	{
		const FStructurePlan::FMember& Member = Plan.Members[Index];
		Before.FindOrAdd(FMath::RoundToInt32(Member.Band)) += Member.Pieces;
		Structured.FindOrAdd(FMath::RoundToInt32(Plan.StructureDistances[Index])) += Member.Pieces;
		After.FindOrAdd(FMath::RoundToInt32(Plan.Distances[Index])) += Member.Pieces;
		LoweredPieces += Plan.Distances[Index] < Plan.StructureDistances[Index] ? Member.Pieces : 0;
		// A structure is measured by the pieces the structure rule moved to another distance.
		if (Plan.StructureDistances[Index] == Member.Band)
		{
			continue;
		}
		const int32 Structure = Plan.Structures[Index];
		StructureDistance.FindOrAdd(Structure) = Plan.StructureDistances[Index];
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
	// A held piece whose component still draws past everything holding it up would hang in the air: the pull-down leaves none.
	int64 HeldPieces = 0;
	int64 PastHolders = 0;
	for (int32 Index = 0; Index < Plan.Pieces.Num(); ++Index)
	{
		const int32 Component = Plan.Pieces[Index].Component;
		const double Limit = Plan.PieceLimits[Index];
		if (Component < MemberCount && Limit > 0.0)
		{
			++HeldPieces;
			PastHolders += Plan.Distances[Component] > Limit ? 1 : 0;
		}
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_STRUCTURE_BANDS before %s | structures %s | after %s | structures that moved pieces %d, largest %d owners %lld pieces moved (to %.0f m) | pulled down %d components, %lld pieces; held up %lld, still past what holds them %lld | %.1f s"),
		*BandLine(Before), *BandLine(Structured), *BandLine(After), StructurePieces.Num(), Largest != INDEX_NONE ? StructureOwners[Largest].Num() : 0,
		Largest != INDEX_NONE ? StructurePieces[Largest] : 0ll, Largest != INDEX_NONE ? StructureDistance[Largest] / 100.0 : 0.0,
		Plan.Lowered, LoweredPieces, HeldPieces, PastHolders, Plan.Seconds);

	for (const FString& Prefix : Meshes)
	{
		auto Matches = [&Prefix](const UStaticMeshComponent* Component)
		{
			const UStaticMesh* Mesh = Component ? Component->GetStaticMesh() : nullptr;
			return Mesh && Mesh->GetName().StartsWith(Prefix);
		};
		TArray<bool> Match;
		Match.SetNumZeroed(MemberCount + Plan.Others.Num());
		for (int32 Index = 0; Index < MemberCount; ++Index)
		{
			Match[Index] = Matches(Plan.Members[Index].Component);
		}
		for (int32 Index = 0; Index < Plan.Others.Num(); ++Index)
		{
			Match[MemberCount + Index] = Matches(Plan.Others[Index]);
		}
		int64 Total = 0, Own = 0, Held = 0, Lowered = 0, Past = 0;
		TMap<int32, int64> Was;
		TMap<int32, int64> Now;
		for (int32 Index = 0; Index < Plan.Pieces.Num(); ++Index)
		{
			const int32 Component = Plan.Pieces[Index].Component;
			if (!Match.IsValidIndex(Component) || !Match[Component])
			{
				continue;
			}
			++Total;
			if (Component >= MemberCount)
			{
				++Own;
				continue;
			}
			const double Limit = Plan.PieceLimits[Index];
			Held += Limit > 0.0 ? 1 : 0;
			Past += Limit > 0.0 && Plan.Distances[Component] > Limit ? 1 : 0;
			Lowered += Plan.Distances[Component] < Plan.StructureDistances[Component] ? 1 : 0;
			Was.FindOrAdd(FMath::RoundToInt32(Plan.StructureDistances[Component])) += 1;
			Now.FindOrAdd(FMath::RoundToInt32(Plan.Distances[Component])) += 1;
		}
		UE_LOG(LogMantle, Log, TEXT("MANTLE_STRUCTURE_MESHES %s*: pieces %lld (own distance or never culled %lld), held up %lld, came down %lld, still past what holds them %lld | before the pull-down %s | after %s"),
			*Prefix, Total, Own, Held, Lowered, Past, *BandLine(Was), *BandLine(Now));
	}
}

namespace MantleDrawDistancePrivate
{
	FAutoConsoleCommandWithWorldAndArgs GStructureDistancesCommand(
		TEXT("Mantle.StructureDistances"),
		TEXT("Logs what the structure rules do to the level (MANTLE_STRUCTURE_BANDS), without exporting. Mesh name prefixes after it get a MANTLE_STRUCTURE_MESHES line each."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateStatic([](const TArray<FString>& Args, UWorld* World) { FMantleDrawDistance::LogStructureBands(World, Args); }));
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
