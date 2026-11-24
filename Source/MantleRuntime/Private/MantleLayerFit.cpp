// Copyright Joshua Eger. All Rights Reserved.
#include "MantleLayerFit.h"

#include "MantleBodyCoverage.h"
#include "MantleGarmentClearance.h"
#include "MantleGarmentGeometry.h"
#include "MantleHairHatFit.h"
#include "MantleRuntime.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleLayerFitPrivate
{
	/**
	 * Target for Restore over Under: every Outer vertex whose nearest point on Under lies less than MaxDepthCm inside it
	 * or off it (not on its open edge) is tracked with the clearance it should have, GapCm; vertices MaxDepthCm or more
	 * inside are not tracked. OutNeeding is how many tracked vertices are less than GapCm off Under now.
	 */
	bool MeasureLOD(const FMantleSkinnedMeshLOD& Outer, const FMantleSkinnedMeshLOD& Under, FMantleGarmentClearance& OutTarget, int32& OutNeeding, FString* OutError)
	{
		OutNeeding = 0;
		if (Outer.NumVertices() == 0 || Outer.NumTriangles() == 0)
		{
			OutTarget = FMantleGarmentClearance();
			return true;
		}
		if (!FMantleGarmentClearance::Compute(Outer.Positions, Under, OutTarget, FMantleLayerFit::MaxDepthCm, OutError))
		{
			return false;
		}
		// A vertex whose nearest point is on the under-garment's hem, cuff or collar edge is beside it, not over it.
		OutTarget.UntrackBoundary(Under);
		for (int32 Vertex = 0; Vertex < Outer.NumVertices(); ++Vertex)
		{
			if (!OutTarget.IsTracked(Vertex))
			{
				continue;
			}
			const float Clearance = OutTarget.ClearanceCm[Vertex];
			if (Clearance <= -FMantleLayerFit::MaxDepthCm)
			{
				OutTarget.Untrack(Vertex);
				continue;
			}
			OutNeeding += Clearance < FMantleLayerFit::GapCm ? 1 : 0;
			OutTarget.ClearanceCm[Vertex] = FMantleLayerFit::GapCm;
		}
		return true;
	}

	/** Pushes Outer out of Under to Target's clearance (Restore with no slack, no floor and no authored garment); adds to Stats. */
	bool PushLOD(FMantleSkinnedMeshLOD& Outer, const FMantleGarmentClearance& Target, const FMantleSkinnedMeshLOD& Under, FMantleLayerFitStats& Stats, FString* OutError)
	{
		TArray<int32> Seams;
		int32 Groups = 0;
		int32 Copies = 0;
		MantleGarmentGeometry::SectionSeams(Outer, Seams, Groups, Copies);
		FMantleClearanceRestoreStats RestoreStats;
		if (!FMantleClearanceRestore::Restore(Outer, Target, Under.Positions, nullptr, 0.0f, 0.0f, FMantleGarmentClearance::DefaultSmoothIters, &RestoreStats, OutError, Seams))
		{
			return false;
		}
		Stats.Moved += RestoreStats.Moved;
		Stats.MaxPushCm = FMath::Max(Stats.MaxPushCm, RestoreStats.MaxPushCm);
		return true;
	}

	/** Cell of the grid the open edges are in (FOpenEdges). */
	constexpr float EdgeCellCm = 4.0f;

	/** Squared distance from P to the segment AB. */
	float DistSqToSegment(const FVector3f& P, const FVector3f& A, const FVector3f& B)
	{
		const FVector3f AB = B - A;
		const float LengthSq = AB.SizeSquared();
		const float T = LengthSq > 1.0e-8f ? FMath::Clamp(FVector3f::DotProduct(P - A, AB) / LengthSq, 0.0f, 1.0f) : 0.0f;
		return (P - (A + AB * T)).SizeSquared();
	}

	/**
	 * One garment's open edges (the hems, cuffs, collar and waist: edges only one triangle of its LOD0 uses, over positions
	 * welded so a seam between sections is not one) as segments in a grid, asked "is this point within KeptBandCm of one".
	 */
	struct FOpenEdges
	{
		TArray<FVector3f> Starts;
		TArray<FVector3f> Ends;
		MantleGarmentGeometry::FBoxGrid Grid;

		/** False (nothing built) when LOD's indices are not whole triangles in range. */
		bool Build(const FMantleSkinnedMeshLOD& LOD)
		{
			using namespace MantleGarmentGeometry;
			if (!IndicesInRange(LOD.Indices, LOD.NumVertices()))
			{
				return false;
			}
			TArray<int32> Weld;
			const int32 NumWelded = WeldByPosition(LOD.Positions, Weld);
			TArray<uint8> Flags;
			FindOpenEdges(LOD.Indices, Weld, NumWelded, Flags);
			TArray<FBox3f> Boxes;
			const FVector3f Pad(FMantleLayerFit::KeptBandCm);
			for (int32 Triangle = 0; Triangle < Flags.Num(); ++Triangle)
			{
				for (int32 Edge = 0; Edge < 3; ++Edge)
				{
					if ((Flags[Triangle] & (1u << Edge)) == 0)
					{
						continue;
					}
					const FVector3f& A = LOD.Positions[LOD.Indices[Triangle * 3 + Edge]];
					const FVector3f& B = LOD.Positions[LOD.Indices[Triangle * 3 + (Edge + 1) % 3]];
					Starts.Add(A);
					Ends.Add(B);
					Boxes.Add(FBox3f(A.ComponentMin(B) - Pad, A.ComponentMax(B) + Pad));
				}
			}
			Grid.Build(Boxes, EdgeCellCm);
			return true;
		}

		/** P is within KeptBandCm of an open edge (a segment is listed in every cell its box, padded by the band, touches). */
		bool Near(const FVector3f& P) const
		{
			constexpr float BandSq = FMantleLayerFit::KeptBandCm * FMantleLayerFit::KeptBandCm;
			bool bNear = false;
			Grid.ForEachInCell(Grid.CellOf(P), [&](const int32 Segment)
			{
				bNear = bNear || DistSqToSegment(P, Starts[Segment], Ends[Segment]) <= BandSq;
			});
			return bNear;
		}

		/**
		 * A triangle is near when its centroid or an edge midpoint is (its corners are asked by the caller): a long triangle of
		 * a lower LOD could have an open edge cross it with all three corners past the band.
		 */
		bool NearTriangle(const FVector3f& A, const FVector3f& B, const FVector3f& C) const
		{
			return Near((A + B + C) / 3.0f) || Near((A + B) * 0.5f) || Near((B + C) * 0.5f) || Near((C + A) * 0.5f);
		}
	};

	/** A garment over the one being edited: its LOD0 surface, and its open edges (null for an overlay under it: no band). */
	struct FOver
	{
		const FMantleSkinnedMeshLOD* LOD0 = nullptr;
		const FOpenEdges* Edges = nullptr;
	};

	/** What the edits of one under-garment did, over its LODs. */
	struct FEditCounts
	{
		int32 Hidden = 0;
		int32 HiddenLOD0 = 0;
		int32 Sunk = 0;
		float MaxSunkCm = 0.0f;
		int32 Failed = 0;
	};

	/** The index list is a non-empty whole number of triangles and the sections lie inside it in whole triangles (what Validate asks of an LOD's indices). */
	bool LODIsSound(const FMantleSkinnedMeshLOD& LOD)
	{
		if (LOD.Indices.Num() == 0 || LOD.Indices.Num() % 3 != 0)
		{
			return false;
		}
		for (const FMantleSkinnedMeshSection& Section : LOD.Sections)
		{
			if (Section.FirstIndex < 0 || Section.IndexCount < 0 || Section.FirstIndex % 3 != 0 || Section.IndexCount % 3 != 0
				|| static_cast<int64>(Section.FirstIndex) + Section.IndexCount > LOD.Indices.Num())
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * A LOD with no triangle is not a mesh (FMantleSkinnedMesh::Validate) and the merge refuses it, yet a garment all under
	 * another (the nails under gloves) has none left. So one triangle stays, the first of the first section Source drew, its
	 * corners made 0.01 cm across: nothing to see. LOD is the edited LOD with no indices left; its sections are Source's.
	 */
	void KeepOneTriangle(FMantleSkinnedMeshLOD& LOD, const FMantleSkinnedMeshLOD& Source)
	{
		int32 Owner = INDEX_NONE;
		for (int32 SectionIndex = 0; SectionIndex < Source.Sections.Num() && Owner == INDEX_NONE; ++SectionIndex)
		{
			const FMantleSkinnedMeshSection& Section = Source.Sections[SectionIndex];
			if (Section.IndexCount >= 3 && Section.FirstIndex >= 0 && Section.FirstIndex + 3 <= Source.Indices.Num())
			{
				Owner = SectionIndex;
			}
		}
		if (Owner == INDEX_NONE || LOD.Sections.Num() != Source.Sections.Num())
		{
			return;
		}
		const int32 First = Source.Sections[Owner].FirstIndex;
		const uint32 A = Source.Indices[First];
		const uint32 B = Source.Indices[First + 1];
		const uint32 C = Source.Indices[First + 2];
		LOD.Indices.Add(A);
		LOD.Indices.Add(B);
		LOD.Indices.Add(C);
		for (int32 SectionIndex = 0; SectionIndex < LOD.Sections.Num(); ++SectionIndex)
		{
			LOD.Sections[SectionIndex].FirstIndex = SectionIndex > Owner ? 3 : 0;
			LOD.Sections[SectionIndex].IndexCount = SectionIndex == Owner ? 3 : 0;
		}
		LOD.Positions[B] = LOD.Positions[A] + FVector3f(0.01f, 0.0f, 0.0f);
		LOD.Positions[C] = LOD.Positions[A] + FVector3f(0.0f, 0.01f, 0.0f);
	}

	/**
	 * Hides and sinks one LOD of an under-garment (Source) against the garments over it (Overs, their LOD0): the triangles
	 * whose three corners an over-garment covers, all past its band (bBand: KeptBandCm from the over-garment's open edges; an
	 * overlay has none) are cut; the covered points of the triangles that stay sink to SinkUnderCm under the lowest of the
	 * garments over them, SinkMaxCm at the most. Fills OutLOD and returns true when anything changed; false leaves it alone.
	 */
	bool EditLOD(const FMantleSkinnedMeshLOD& Source, const bool bBand, TConstArrayView<FOver> Overs, FMantleSkinnedMeshLOD& OutLOD, FEditCounts& Counts)
	{
		const int32 NumVertices = Source.NumVertices();
		const int32 NumTriangles = Source.NumTriangles();
		if (NumVertices == 0 || NumTriangles == 0 || Source.Normals.Num() != NumVertices || !MantleGarmentGeometry::IndicesInRange(Source.Indices, NumVertices))
		{
			return false;
		}
		TBitArray<> Covered(false, NumVertices);
		TArray<uint8> Drop;
		Drop.SetNumZeroed(NumTriangles);
		TArray<FMantleGarmentSurface> Surfaces;
		for (const FOver& Over : Overs)
		{
			FMantleGarmentCoverage Coverage;
			FString Error;
			// The under-garment's points stand in for the body's; the near-half rule is off (it assumes a closed body) and a hit
			// counts only on the over-garment's outer layer.
			if (!FMantleGarmentCoverage::Compute(FMantleGarmentSurface(*Over.LOD0), Source.Positions, Source.Normals, Coverage, FMantleGarmentCoverage::DefaultMaxDistanceCm, &Error,
					TConstArrayView<uint32>(), 0.0f, FMantleGarmentCoverage::SkinMaskEdgeTolerance, /*bSameFacingOnly*/ true)
				|| Coverage.Covered.Num() != NumVertices)
			{
				++Counts.Failed;
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_LAYERFIT hide: coverage refused: %s"), *Error);
				continue;
			}
			if (Coverage.NumCovered == 0)
			{
				continue;
			}
			Surfaces.Add(FMantleGarmentSurface(*Over.LOD0));
			Covered.CombineWithBitwiseOR(Coverage.Covered, EBitwiseOperatorFlags::MaintainSize);

			// Covered and past the band: a point the garment over it hides for good.
			TBitArray<> Deep(Coverage.Covered);
			const bool bNearEdges = bBand && Over.Edges != nullptr;
			if (bNearEdges)
			{
				for (TConstSetBitIterator<> It(Coverage.Covered); It; ++It)
				{
					if (Over.Edges->Near(Source.Positions[It.GetIndex()]))
					{
						Deep[It.GetIndex()] = false;
					}
				}
			}
			for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle)
			{
				const int32 A = static_cast<int32>(Source.Indices[Triangle * 3]);
				const int32 B = static_cast<int32>(Source.Indices[Triangle * 3 + 1]);
				const int32 C = static_cast<int32>(Source.Indices[Triangle * 3 + 2]);
				if (Drop[Triangle] || !Deep[A] || !Deep[B] || !Deep[C])
				{
					continue;
				}
				if (bNearEdges && Over.Edges->NearTriangle(Source.Positions[A], Source.Positions[B], Source.Positions[C]))
				{
					continue;
				}
				Drop[Triangle] = 1;
			}
		}
		int32 DropCount = 0;
		for (const uint8 Dropped : Drop)
		{
			DropCount += Dropped;
		}
		if (DropCount == 0 && Surfaces.IsEmpty())
		{
			return false;
		}

		FMantleSkinnedMeshLOD Edited = Source;
		if (DropCount > 0)
		{
			TArray<int32> DroppedTriangles;
			DroppedTriangles.Reserve(DropCount);
			for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle)
			{
				if (Drop[Triangle])
				{
					DroppedTriangles.Add(Triangle);
				}
			}
			FMantleHairHatFit::ApplyDrops(Edited, DroppedTriangles);
		}

		// The covered points of the triangles that stay, under the lowest garment over them: the body's own sink.
		int32 SunkPoints = 0;
		float MaxSunkCm = 0.0f;
		if (Edited.Indices.Num() > 0 && !Surfaces.IsEmpty())
		{
			FMantleBodySinkParams SinkParams;
			SinkParams.UnderCm = FMantleLayerFit::SinkUnderCm;
			SinkParams.MaxSinkCm = FMantleLayerFit::SinkMaxCm;
			SinkParams.SmoothIters = FMantleLayerFit::SinkSmoothIters;
			TArray<float> SinkCm;
			FMantleBodySink SinkInfo;
			FString Error;
			if (!FMantleBodySink::Compute(Edited.Positions, Edited.Normals, Edited.Indices, Surfaces, SinkParams, SinkCm, SinkInfo, &Error, TConstArrayView<uint32>(), &Covered))
			{
				++Counts.Failed;
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_LAYERFIT sink refused: %s"), *Error);
			}
			else if (SinkInfo.Sunk > 0)
			{
				TArray<FVector3f> Sunk;
				FMantleSinkApplyStats Applied;
				FMantleBodySinkApply::Sink(Edited.Positions, Edited.Normals, Edited.Indices, SinkCm, Sunk, Applied);
				Edited.Positions = MoveTemp(Sunk);
				SunkPoints = Applied.Points;
				MaxSunkCm = SinkInfo.MaxSinkCm;
			}
		}
		if (DropCount == 0 && SunkPoints == 0)
		{
			return false;
		}
		if (Edited.Indices.Num() == 0)
		{
			KeepOneTriangle(Edited, Source);
		}
		Counts.Hidden += DropCount;
		Counts.Sunk += SunkPoints;
		Counts.MaxSunkCm = FMath::Max(Counts.MaxSunkCm, MaxSunkCm);
		OutLOD = MoveTemp(Edited);
		return true;
	}

	/**
	 * The hide and sink step: OutShown (one per garment, Placed's order) is Placed with every under-garment edited against the
	 * garments over it (a higher rank, near it: an overlay only under garments that cover overlays). Garments edited come back
	 * as copies, the others as Placed's own pointer.
	 */
	void HideAndSink(TConstArrayView<FMantleLayerGarment> Garments, const TArray<TSharedPtr<const FMantleSkinnedMesh>>& Placed,
		TArray<TSharedPtr<const FMantleSkinnedMesh>>& OutShown, FMantleLayerFitStats& Stats)
	{
		using namespace MantleGarmentGeometry;
		OutShown = Placed;
		const int32 Num = Garments.Num();
		const auto IsLayer = [&Placed](const int32 Index)
		{
			return Placed[Index].IsValid() && Placed[Index]->LODs.Num() > 0 && Placed[Index]->LODs[0].NumTriangles() > 0;
		};
		TArray<FBox3f> Bounds;
		Bounds.Init(FBox3f(ForceInit), Num);
		for (int32 Index = 0; Index < Num; ++Index)
		{
			if (IsLayer(Index))
			{
				Bounds[Index] = Placed[Index]->ComputeBounds(0);
			}
		}
		// What a ray from an under-garment's point reaches: back under its surface and out to the garment over it.
		const float Reach = FMantleGarmentCoverage::InsideReachCm + FMantleGarmentCoverage::DefaultMaxDistanceCm;
		TArray<TUniquePtr<FOpenEdges>> EdgesOf;
		EdgesOf.SetNum(Num);
		TBitArray<> EdgesRefused(false, Num);

		for (int32 Under = 0; Under < Num; ++Under)
		{
			if (!IsLayer(Under))
			{
				continue;
			}
			const FMantleLayerGarment& UnderLayer = Garments[Under];
			const FBox3f UnderBounds = Bounds[Under].ExpandBy(Reach);
			TArray<FOver> Overs;
			for (int32 Over = 0; Over < Num; ++Over)
			{
				if (Over == Under || !IsLayer(Over) || Garments[Over].bOverlay || Garments[Over].Rank <= UnderLayer.Rank
					|| (UnderLayer.bOverlay && !Garments[Over].bCoversOverlay))
				{
					continue;
				}
				const FMantleSkinnedMeshLOD& LOD0 = Placed[Over]->LODs[0];
				if (LOD0.Normals.Num() != LOD0.NumVertices() || !IndicesInRange(LOD0.Indices, LOD0.NumVertices()) || !UnderBounds.Intersect(Bounds[Over]))
				{
					continue;
				}
				FOver Ref;
				Ref.LOD0 = &LOD0;
				if (!UnderLayer.bOverlay)
				{
					if (EdgesRefused[Over])
					{
						continue;
					}
					if (!EdgesOf[Over].IsValid())
					{
						TUniquePtr<FOpenEdges> Built = MakeUnique<FOpenEdges>();
						if (!Built->Build(LOD0))
						{
							EdgesRefused[Over] = true;
							continue;
						}
						EdgesOf[Over] = MoveTemp(Built);
					}
					Ref.Edges = EdgesOf[Over].Get();
				}
				Overs.Add(Ref);
			}
			if (Overs.IsEmpty())
			{
				continue;
			}
			Stats.HidePairs += Overs.Num();

			const FMantleSkinnedMesh& UnderMesh = *Placed[Under];
			TArray<FMantleSkinnedMeshLOD> Edited;
			Edited.SetNum(UnderMesh.LODs.Num());
			TArray<uint8> LODChanged;
			LODChanged.SetNumZeroed(UnderMesh.LODs.Num());
			FEditCounts Counts;
			bool bSound = true;
			int32 Changed = 0;
			for (int32 LODIndex = 0; LODIndex < UnderMesh.LODs.Num(); ++LODIndex)
			{
				const int32 HiddenBefore = Counts.Hidden;
				if (EditLOD(UnderMesh.LODs[LODIndex], !UnderLayer.bOverlay, Overs, Edited[LODIndex], Counts))
				{
					LODChanged[LODIndex] = 1;
					++Changed;
					bSound = bSound && LODIsSound(Edited[LODIndex]);
				}
				if (LODIndex == 0)
				{
					Counts.HiddenLOD0 = Counts.Hidden - HiddenBefore;
				}
			}
			Stats.HideFailed += Counts.Failed;
			if (Changed == 0)
			{
				continue;
			}
			if (!bSound)
			{
				// A cut that left an LOD with no triangle and nothing to keep: the garment shows as it was placed.
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_LAYERFIT garment %d (rank %d) shown as placed: the cut left an LOD with no triangles"), Under, UnderLayer.Rank);
				continue;
			}
			TSharedPtr<FMantleSkinnedMesh> Work = MakeShared<FMantleSkinnedMesh>(UnderMesh);
			for (int32 LODIndex = 0; LODIndex < Edited.Num(); ++LODIndex)
			{
				if (LODChanged[LODIndex])
				{
					Work->LODs[LODIndex] = MoveTemp(Edited[LODIndex]);
				}
			}
			OutShown[Under] = TSharedPtr<const FMantleSkinnedMesh>(Work);
			++Stats.Edited;
			Stats.Hidden += Counts.Hidden;
			Stats.HiddenLOD0 += Counts.HiddenLOD0;
			Stats.Sunk += Counts.Sunk;
			Stats.MaxSunkCm = FMath::Max(Stats.MaxSunkCm, static_cast<double>(Counts.MaxSunkCm));
		}
	}
}

FString FMantleLayerFitStats::ToString() const
{
	return FString::Printf(TEXT("garments=%d pairs=%d pushed=%d failed=%d below=%d moved=%d maxPushCm=%.2f hidePairs=%d edited=%d hideFailed=%d hidden=%d hiddenLOD0=%d sunk=%d maxSunkCm=%.2f hideMs=%.2f ms=%.2f"),
		Garments, Pairs, Pushed, Failed, Below, Moved, MaxPushCm, HidePairs, Edited, HideFailed, Hidden, HiddenLOD0, Sunk, MaxSunkCm, HideMs, Ms);
}

void FMantleLayerFit::Apply(const TConstArrayView<FMantleLayerGarment> Garments, TArray<TSharedPtr<const FMantleSkinnedMesh>>& OutPlaced, FMantleLayerFitStats& OutStats,
	TArray<TSharedPtr<const FMantleSkinnedMesh>>* OutShown)
{
	using namespace MantleLayerFitPrivate;

	const double Start = MantleGarmentGeometry::NowMs();
	OutStats = FMantleLayerFitStats();
	OutPlaced.Reset(Garments.Num());
	TArray<int32> Order;
	for (int32 Index = 0; Index < Garments.Num(); ++Index)
	{
		const TSharedPtr<const FMantleSkinnedMesh>& Mesh = Garments[Index].Mesh;
		OutPlaced.Add(Mesh);
		if (Mesh.IsValid() && Mesh->LODs.Num() > 0 && Mesh->LODs[0].NumTriangles() > 0)
		{
			++OutStats.Garments;
			// An overlay (nails) is neither pushed nor pushed out of: it is hidden under what covers it (below).
			if (!Garments[Index].bOverlay)
			{
				Order.Add(Index);
			}
		}
	}
	// Inside to outside, by the layer rank; equal ranks stay in the order they came.
	Order.StableSort([&Garments](const int32 A, const int32 B)
	{
		return Garments[A].Rank < Garments[B].Rank;
	});

	TArray<FBox3f> Bounds;
	Bounds.Init(FBox3f(ForceInit), Garments.Num());
	TArray<int32> Placed;
	for (const int32 Index : Order)
	{
		const FMantleSkinnedMesh& Source = *Garments[Index].Mesh;
		TSharedPtr<FMantleSkinnedMesh> Work;
		bool bFailed = false;
		// The garments under this one, the outermost first: what it clears there is the outer envelope of the stack.
		for (int32 Slot = Placed.Num() - 1; Slot >= 0 && !bFailed; --Slot)
		{
			const FMantleSkinnedMesh& UnderMesh = *OutPlaced[Placed[Slot]];
			const FMantleSkinnedMesh& Current = Work.IsValid() ? *Work : Source;
			// A garment on another part of the body (socks and a top) is nowhere near: no measure.
			if (!Current.ComputeBounds(0).ExpandBy(MaxDepthCm).Intersect(Bounds[Placed[Slot]]))
			{
				continue;
			}
			++OutStats.Pairs;
			FString Error;
			FMantleGarmentClearance Target;
			int32 Needing = 0;
			if (!MeasureLOD(Current.LODs[0], UnderMesh.LODs[0], Target, Needing, &Error))
			{
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_LAYERFIT garment %d (rank %d) left as it is: %s"), Index, Garments[Index].Rank, *Error);
				bFailed = true;
				break;
			}
			if (Needing == 0)
			{
				continue;
			}
			OutStats.Below += Needing;
			if (!Work.IsValid())
			{
				Work = MakeShared<FMantleSkinnedMesh>(Source);
			}
			// Every LOD against the under-garment's LOD0 (LOD0's target is the one just measured).
			bool bPushed = PushLOD(Work->LODs[0], Target, UnderMesh.LODs[0], OutStats, &Error);
			for (int32 LODIndex = 1; bPushed && LODIndex < Work->LODs.Num(); ++LODIndex)
			{
				FMantleGarmentClearance LODTarget;
				int32 LODNeeding = 0;
				bPushed = MeasureLOD(Work->LODs[LODIndex], UnderMesh.LODs[0], LODTarget, LODNeeding, &Error)
					&& (LODNeeding == 0 || PushLOD(Work->LODs[LODIndex], LODTarget, UnderMesh.LODs[0], OutStats, &Error));
			}
			if (!bPushed)
			{
				UE_LOG(LogMantle, Warning, TEXT("MANTLE_LAYERFIT garment %d (rank %d) left as it is: %s"), Index, Garments[Index].Rank, *Error);
				bFailed = true;
			}
		}
		if (bFailed)
		{
			++OutStats.Failed;
			Work.Reset();
		}
		else if (Work.IsValid())
		{
			OutPlaced[Index] = TSharedPtr<const FMantleSkinnedMesh>(Work);
			++OutStats.Pushed;
		}
		Bounds[Index] = OutPlaced[Index]->ComputeBounds(0);
		Placed.Add(Index);
	}
	if (OutShown)
	{
		const double HideStart = MantleGarmentGeometry::NowMs();
		HideAndSink(Garments, OutPlaced, *OutShown, OutStats);
		OutStats.HideMs = MantleGarmentGeometry::NowMs() - HideStart;
	}
	OutStats.Ms = MantleGarmentGeometry::NowMs() - Start;
	UE_LOG(LogMantle, Verbose, TEXT("MANTLE_LAYERFIT %s"), *OutStats.ToString());
}
