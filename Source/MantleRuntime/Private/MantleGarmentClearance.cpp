// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGarmentClearance.h"

#include "MantleGarmentGeometry.h"
#include "MantleRuntime.h"
#include "Async/ParallelFor.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleGarmentClearancePrivate
{
	/** Cell of the surface-triangle grid Compute searches. */
	constexpr float ClearanceCellCm = 3.0f;

	/** A face cross product this short or shorter has no orientation (FMantleFaceFlips' degenerate rule). */
	constexpr double FlipDegenerateCross = 1.0e-10;

	/** How Restore pushes a point. */
	constexpr uint8 RestoreDirNone = 0;
	constexpr uint8 RestoreDirGarment = 1;
	constexpr uint8 RestoreDirSurface = 2;

	/** Unnormalised face normal (index winding) of triangle Triangle over Positions, in double as FMantleFaceFlips measures it. */
	FVector3d FaceCross(TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Indices, const int32 Triangle)
	{
		const FVector3d P0(Positions[Indices[Triangle * 3]]);
		const FVector3d P1(Positions[Indices[Triangle * 3 + 1]]);
		const FVector3d P2(Positions[Indices[Triangle * 3 + 2]]);
		return (P1 - P0) ^ (P2 - P0);
	}

	/** A triangle Restore moves: its vertices and the faces its orientation is checked against. */
	struct FRestoreTriangle
	{
		int32 V[3] = { 0, 0, 0 };
		FVector3d PreCross = FVector3d::ZeroVector;
		FVector3d AuthoredCross = FVector3d::ZeroVector;
		bool bPre = false;
		bool bAuthored = false;
	};
}

void FMantleGarmentClearance::Untrack(const int32 Vertex)
{
	if (IsTracked(Vertex))
	{
		Triangle[Vertex] = INDEX_NONE;
		ClearanceCm[Vertex] = 0.0f;
		--NumTracked;
	}
}

bool FMantleGarmentClearance::Compute(const TConstArrayView<FVector3f> GarmentPositions, const FMantleSkinnedMeshLOD& Surface, FMantleGarmentClearance& Out,
	const float SearchCm, FString* OutError)
{
	using namespace MantleGarmentClearancePrivate;
	using namespace MantleGarmentGeometry;

	const double Start = NowMs();
	Out = FMantleGarmentClearance();
	const int32 NumGarment = GarmentPositions.Num();
	const int32 NumSurface = Surface.NumVertices();
	const int32 NumTriangles = Surface.NumTriangles();
	if (NumGarment == 0 || NumSurface == 0 || NumTriangles == 0 || Surface.Normals.Num() != NumSurface || SearchCm <= 0.0f
		|| !IndicesInRange(Surface.Indices, NumSurface))
	{
		return MantleGarmentGeometry::RefuseFit(TEXT("clearance"), FString::Printf(TEXT("bad inputs (garment %d vertices, surface %d vertices, %d triangles, %d normals, search %.2f cm)"),
			NumGarment, NumSurface, NumTriangles, Surface.Normals.Num(), SearchCm), OutError);
	}

	Out.SurfaceIndices = Surface.Indices;
	Out.NumSurfaceVertices = NumSurface;
	TArray<FVector3f> TriangleNormals;
	TArray<FBox3f> TriangleBoxes;
	TriangleFrames(Surface.Positions, Surface.Normals, Out.SurfaceIndices, TriangleNormals, Out.TriangleSign, TriangleBoxes);
	FBoxGrid Grid;
	Grid.Build(TriangleBoxes, ClearanceCellCm);

	// Nearest surface triangle per garment vertex, ring by ring: after ring K every triangle not yet seen
	// lies at least K cells away, so the search stops once the best distance is within K cells.
	Out.Triangle.Init(INDEX_NONE, NumGarment);
	Out.Barycentrics.Init(FVector3f::ZeroVector, NumGarment);
	Out.ClearanceCm.Init(0.0f, NumGarment);
	const int32 MaxRing = FMath::CeilToInt32(SearchCm / ClearanceCellCm);
	const float Search2 = SearchCm * SearchCm;
	const TArray<uint32>& Indices = Out.SurfaceIndices;
	ParallelFor(NumGarment, [&](const int32 Vertex)
	{
		const FVector3f P = GarmentPositions[Vertex];
		const FIntVector Home = Grid.CellOf(P);
		float BestSq = Search2;
		int32 BestTriangle = INDEX_NONE;
		FVector3f BestPoint = FVector3f::ZeroVector;
		FVector3f BestBary = FVector3f::ZeroVector;
		for (int32 Ring = 0; Ring <= MaxRing; ++Ring)
		{
			for (int32 DZ = -Ring; DZ <= Ring; ++DZ)
			{
				for (int32 DY = -Ring; DY <= Ring; ++DY)
				{
					for (int32 DX = -Ring; DX <= Ring; ++DX)
					{
						if (FMath::Max3(FMath::Abs(DX), FMath::Abs(DY), FMath::Abs(DZ)) != Ring)
						{
							continue;
						}
						Grid.ForEachInCell(Home + FIntVector(DX, DY, DZ), [&](const int32 Candidate)
						{
							if (DistSqPointBox(P, TriangleBoxes[Candidate]) > BestSq)
							{
								return;
							}
							FVector3f Bary;
							const FVector3f Closest = ClosestPointOnTriangle(P, Surface.Positions[Indices[Candidate * 3]], Surface.Positions[Indices[Candidate * 3 + 1]],
								Surface.Positions[Indices[Candidate * 3 + 2]], Bary);
							const float DistSq = FVector3f::DistSquared(P, Closest);
							if (DistSq <= BestSq && (BestTriangle == INDEX_NONE || DistSq < BestSq))
							{
								BestSq = DistSq;
								BestTriangle = Candidate;
								BestPoint = Closest;
								BestBary = Bary;
							}
						});
					}
				}
			}
			if (BestTriangle != INDEX_NONE && BestSq <= FMath::Square(Ring * ClearanceCellCm))
			{
				break;
			}
		}
		if (BestTriangle != INDEX_NONE)
		{
			Out.Triangle[Vertex] = BestTriangle;
			Out.Barycentrics[Vertex] = BestBary;
			Out.ClearanceCm[Vertex] = FVector3f::DotProduct(P - BestPoint, TriangleNormals[BestTriangle]);
		}
	});

	double Sum = 0.0;
	Out.MinClearanceCm = TNumericLimits<float>::Max();
	Out.MaxClearanceCm = -TNumericLimits<float>::Max();
	for (int32 Vertex = 0; Vertex < NumGarment; ++Vertex)
	{
		if (Out.Triangle[Vertex] != INDEX_NONE)
		{
			++Out.NumTracked;
			Sum += Out.ClearanceCm[Vertex];
			Out.MinClearanceCm = FMath::Min(Out.MinClearanceCm, Out.ClearanceCm[Vertex]);
			Out.MaxClearanceCm = FMath::Max(Out.MaxClearanceCm, Out.ClearanceCm[Vertex]);
		}
	}
	if (Out.NumTracked == 0)
	{
		Out.MinClearanceCm = 0.0f;
		Out.MaxClearanceCm = 0.0f;
	}
	Out.MeanClearanceCm = Out.NumTracked > 0 ? static_cast<float>(Sum / Out.NumTracked) : 0.0f;
	Out.Ms = NowMs() - Start;
	return true;
}

bool FMantleGarmentClearance::Measure(const int32 Vertex, const FVector3f& P, const TConstArrayView<FVector3f> SurfacePositions, float& OutClearanceCm, FVector3f& OutNormal) const
{
	if (!IsTracked(Vertex) || SurfacePositions.Num() != NumSurfaceVertices)
	{
		return false;
	}
	const int32 T = Triangle[Vertex];
	const FVector3f& A = SurfacePositions[SurfaceIndices[T * 3]];
	const FVector3f& B = SurfacePositions[SurfaceIndices[T * 3 + 1]];
	const FVector3f& C = SurfacePositions[SurfaceIndices[T * 3 + 2]];
	const FVector3f Cross = FVector3f::CrossProduct(B - A, C - A);
	if (Cross.SizeSquared() <= MantleGarmentGeometry::DegenerateCrossSq)
	{
		return false;
	}
	OutNormal = Cross.GetUnsafeNormal() * static_cast<float>(TriangleSign[T]);
	const FVector3f& W = Barycentrics[Vertex];
	const FVector3f Corresponding = A * W.X + B * W.Y + C * W.Z;
	OutClearanceCm = FVector3f::DotProduct(P - Corresponding, OutNormal);
	return true;
}

int32 FMantleGarmentClearance::UntrackBoundary(const FMantleSkinnedMeshLOD& Surface, const float Tolerance)
{
	using namespace MantleGarmentGeometry;

	if (Surface.NumVertices() != NumSurfaceVertices || Surface.Indices.Num() != SurfaceIndices.Num())
	{
		return 0;
	}
	TArray<int32> Weld;
	const int32 NumWelded = WeldByPosition(Surface.Positions, Weld);
	TArray<uint8> TriangleFlags;
	FindOpenEdges(SurfaceIndices, Weld, NumWelded, TriangleFlags);
	int32 Untracked = 0;
	for (int32 Vertex = 0; Vertex < Triangle.Num(); ++Vertex)
	{
		const int32 T = Triangle[Vertex];
		if (T == INDEX_NONE || !TriangleFlags.IsValidIndex(T) || TriangleFlags[T] == 0)
		{
			continue;
		}
		const FVector3f& W = Barycentrics[Vertex];
		for (int32 Edge = 0; Edge < 3; ++Edge)
		{
			if ((TriangleFlags[T] & (1u << Edge)) && W[(Edge + 2) % 3] < Tolerance)
			{
				Untrack(Vertex);
				++Untracked;
				break;
			}
		}
	}
	return Untracked;
}

FString FMantleClearanceRestoreStats::ToString() const
{
	if (!bRan)
	{
		return TEXT("not run");
	}
	return FString::Printf(TEXT("%d of %d tracked below d0 - slack, twins coupled %d; %d smoothing passes over %d points: moved %d (%d untracked; along the garment normal %d, the surface normal %d); flip limiter: flipped %d before, %d halving passes, limited %d, zeroed %d, flipped left %d; push max %.2f mean %.2f cm, move max %.2f cm, %.2f ms"),
		Below, Tracked, Coupled, SmoothIters, Points, Moved, UntrackedMoved, AlongGarment, AlongSurface, FlippedBefore, HalvingPasses, Limited, Zeroed, FlippedLeft,
		MaxPushCm, MeanPushCm, MaxMoveCm, Ms);
}

void FMantleLayerCrossing::FindTwins(const FMantleSkinnedMeshLOD& Garment, TArray<int32>& OutTwin)
{
	using namespace MantleGarmentGeometry;

	const int32 N = Garment.NumVertices();
	OutTwin.Init(INDEX_NONE, N);
	if (N == 0 || Garment.Normals.Num() != N)
	{
		return;
	}
	TArray<FBox3f> Boxes;
	Boxes.SetNumUninitialized(N);
	for (int32 Vertex = 0; Vertex < N; ++Vertex)
	{
		Boxes[Vertex] = FBox3f(Garment.Positions[Vertex], Garment.Positions[Vertex]);
	}
	FBoxGrid Grid;
	Grid.Build(Boxes, TwinReachCm);
	const FVector3f Reach(TwinReachCm, TwinReachCm, TwinReachCm);
	ParallelFor(N, [&](const int32 Vertex)
	{
		const FVector3f& P = Garment.Positions[Vertex];
		const FVector3f Normal = Garment.Normals[Vertex].GetSafeNormal();
		if (Normal.IsNearlyZero())
		{
			return;
		}
		const FIntVector Lo = Grid.CellOf(P - Reach);
		const FIntVector Hi = Grid.CellOf(P + Reach);
		float BestDist = TwinReachCm;
		int32 Twin = INDEX_NONE;
		for (int32 Z = Lo.Z; Z <= Hi.Z; ++Z)
		{
			for (int32 Y = Lo.Y; Y <= Hi.Y; ++Y)
			{
				for (int32 X = Lo.X; X <= Hi.X; ++X)
				{
					Grid.ForEachInCell(FIntVector(X, Y, Z), [&](const int32 Other)
					{
						if (Other == Vertex)
						{
							return;
						}
						const FVector3f OtherNormal = Garment.Normals[Other].GetSafeNormal();
						if (FVector3f::DotProduct(Normal, OtherNormal) >= OpposeDot)
						{
							return;
						}
						const float Dist = FVector3f::Dist(P, Garment.Positions[Other]);
						if (Dist < BestDist)
						{
							BestDist = Dist;
							Twin = Other;
						}
					});
				}
			}
		}
		OutTwin[Vertex] = Twin;
	});
}

bool FMantleFaceFlips::Measure(const FMantleSkinnedMeshLOD& Authored, const FMantleSkinnedMeshLOD& Stage, FMantleFaceFlips& Out)
{
	Out = FMantleFaceFlips();
	const int32 NumVertices = Authored.NumVertices();
	bool bOk = NumVertices > 0 && Stage.NumVertices() == NumVertices && Authored.Indices.Num() % 3 == 0 && Stage.Indices.Num() == Authored.Indices.Num();
	for (int32 Index = 0; bOk && Index < Authored.Indices.Num(); ++Index)
	{
		bOk = Stage.Indices[Index] == Authored.Indices[Index] && static_cast<int32>(Authored.Indices[Index]) < NumVertices;
	}
	if (!bOk)
	{
		return false;
	}
	const int32 NumTriangles = Authored.NumTriangles();
	Out.FlippedBits.Init(false, NumTriangles);
	for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle)
	{
		const FVector3d N0 = MantleGarmentClearancePrivate::FaceCross(Authored.Positions, Authored.Indices, Triangle);
		const double Area0 = N0.Size();
		if (Area0 <= MantleGarmentClearancePrivate::FlipDegenerateCross)
		{
			++Out.Degenerate;
			continue;
		}
		const FVector3d N1 = MantleGarmentClearancePrivate::FaceCross(Stage.Positions, Stage.Indices, Triangle);
		const bool bFlipped = FVector3d::DotProduct(N0, N1) < 0.0;
		++Out.Triangles;
		Out.Flipped += bFlipped ? 1 : 0;
		Out.Collapsed += N1.Size() < CollapsedAreaRatio * Area0 ? 1 : 0;
		Out.FlippedBits[Triangle] = bFlipped;
	}
	Out.bMeasured = true;
	return true;
}

int32 FMantleFaceFlips::CountAdded(const FMantleFaceFlips& Before, const FMantleFaceFlips& After)
{
	if (!After.bMeasured)
	{
		return 0;
	}
	int32 Added = 0;
	for (TConstSetBitIterator<> It(After.FlippedBits); It; ++It)
	{
		const int32 Triangle = It.GetIndex();
		Added += (Before.bMeasured && Before.FlippedBits.IsValidIndex(Triangle) && Before.FlippedBits[Triangle]) ? 0 : 1;
	}
	return Added;
}

bool FMantleClearanceRestore::Restore(FMantleSkinnedMeshLOD& Garment, const FMantleGarmentClearance& Target, const TConstArrayView<FVector3f> Surface,
	const FMantleSkinnedMeshLOD* Authored, const float SlackCm, const float MinKeptCm, int32 SmoothIters,
	FMantleClearanceRestoreStats* OutStats, FString* OutError, const TConstArrayView<int32> Seams)
{
	using namespace MantleGarmentClearancePrivate;
	using namespace MantleGarmentGeometry;

	const double Start = NowMs();
	const int32 N = Garment.NumVertices();
	if (Target.Triangle.Num() != N || Target.Barycentrics.Num() != N || Target.ClearanceCm.Num() != N || Surface.Num() != Target.NumSurfaceVertices
		|| Garment.Normals.Num() != N || !IndicesInRange(Garment.Indices, N) || (Seams.Num() != 0 && Seams.Num() != N))
	{
		return MantleGarmentGeometry::RefuseFit(TEXT("restore"), FString::Printf(TEXT("%d garment vertices vs %d tracked entries (%d normals, %d seam entries), surface %d vertices vs %d: not restored"),
			N, Target.Triangle.Num(), Garment.Normals.Num(), Seams.Num(), Surface.Num(), Target.NumSurfaceVertices), OutError);
	}
	SmoothIters = FMath::Clamp(SmoothIters, 0, FMantleGarmentClearance::MaxSmoothIters);
	bool bAuthored = Authored != nullptr && Authored->NumVertices() == N && Authored->Indices.Num() == Garment.Indices.Num();
	for (int32 Index = 0; bAuthored && Index < Garment.Indices.Num(); ++Index)
	{
		bAuthored = Authored->Indices[Index] == Garment.Indices[Index];
	}
	if (Authored && !bAuthored)
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_GARMENTFIT restore: the authored garment does not match (%d / %d vertices, %d / %d indices): flips limited against the pre-push faces only"),
			Authored->NumVertices(), N, Authored->Indices.Num(), Garment.Indices.Num());
	}

	// 1. Per vertex the required push p = d0 - d1 (0 where d1 >= d0 - slack) and the surface triangle's
	// normal (zero when not tracked). Each vertex reads only its own position.
	TArray<float> Required;
	Required.SetNumZeroed(N);
	TArray<FVector3f> SurfaceNormal;
	SurfaceNormal.Init(FVector3f::ZeroVector, N);
	ParallelFor(N, [&Garment, &Target, &Surface, &Required, &SurfaceNormal, SlackCm, MinKeptCm](const int32 Vertex)
	{
		float D1 = 0.0f;
		FVector3f Normal;
		if (!Target.Measure(Vertex, Garment.Positions[Vertex], Surface, D1, Normal))
		{
			return;
		}
		SurfaceNormal[Vertex] = Normal;
		// The floor is for the outer layer (facing away from the surface), authored at or just above it
		//; an inner layer and a vertex authored inside keep their clearance. MinKeptCm <= 0: no floor.
		const float Authored0 = Target.ClearanceCm[Vertex];
		const bool bOuter = FVector3f::DotProduct(Garment.Normals[Vertex], Normal) > 0.0f;
		const bool bFloor = MinKeptCm > 0.0f && bOuter;
		const float D0 = (bFloor && Authored0 > -0.3f && Authored0 < MinKeptCm) ? MinKeptCm : Authored0;
		if (D1 < D0 - SlackCm)
		{
			Required[Vertex] = D0 - D1;
		}
	});

	// A vertex and its twin on the other layer take the larger of their pushes, so the double
	// surface moves as one instead of one layer being pushed through the other.
	int32 Coupled = 0;
	{
		TArray<int32> Twin;
		FMantleLayerCrossing::FindTwins(Garment, Twin);
		const TArray<float> Own = Required;
		for (int32 Vertex = 0; Vertex < N; ++Vertex)
		{
			const int32 Other = Twin[Vertex];
			if (Other != INDEX_NONE && Own[Other] > Own[Vertex])
			{
				Required[Vertex] = Own[Other];
				++Coupled;
			}
		}
	}

	// 2. Points: split copies welded within their section (they move together or the seam opens). A
	// point needs the largest push of its copies; its normal is the sum of its copies' garment normals,
	// its surface normal that of its tracked copies'.
	TArray<int32> Sections;
	VertexSections(Garment, Sections);
	TArray<int32> Weld;
	const int32 NumPoints = WeldBySection(Garment.Positions, Sections, Weld);
	TArray<float> PointRequired;
	PointRequired.Init(0.0f, NumPoints);
	TArray<FVector3f> PointNormal;
	PointNormal.Init(FVector3f::ZeroVector, NumPoints);
	TArray<FVector3f> PointSurface;
	PointSurface.Init(FVector3f::ZeroVector, NumPoints);
	TArray<int32> FirstVertex;
	FirstVertex.Init(INDEX_NONE, NumPoints);
	int32 Below = 0;
	for (int32 Vertex = 0; Vertex < N; ++Vertex)
	{
		const int32 Point = Weld[Vertex];
		PointRequired[Point] = FMath::Max(PointRequired[Point], Required[Vertex]);
		PointNormal[Point] += Garment.Normals[Vertex];
		PointSurface[Point] += SurfaceNormal[Vertex];
		if (FirstVertex[Point] == INDEX_NONE)
		{
			FirstVertex[Point] = Vertex;
		}
		Below += Required[Vertex] != 0.0f ? 1 : 0;
	}
	for (int32 Point = 0; Point < NumPoints; ++Point)
	{
		FVector3f Own = PointNormal[Point].GetSafeNormal();
		if (Own.IsZero())
		{
			// Copies with opposed normals: the first copy's own normal.
			Own = Garment.Normals[FirstVertex[Point]].GetSafeNormal();
		}
		PointNormal[Point] = Own;
		PointSurface[Point] = PointSurface[Point].GetSafeNormal();
	}
	FPointNeighbours Neighbours;
	PointNeighbours(Garment.Indices, Weld, NumPoints, Neighbours);

	// 3. The push field, smoothed; never below a point's own requirement, so the clearance is still
	// restored everywhere.
	TArray<float> Push = PointRequired;
	int32 Passes = 0;
	if (Below > 0)
	{
		TArray<float> Next;
		Next.SetNumZeroed(NumPoints);
		for (int32 Pass = 0; Pass < SmoothIters; ++Pass)
		{
			ParallelFor(NumPoints, [&Push, &Next, &Neighbours, &PointRequired](const int32 Point)
			{
				float Smoothed = Push[Point];
				const TArray<int32, TInlineAllocator<8>>& Around = Neighbours[Point];
				if (Around.Num() > 0)
				{
					float Sum = 0.0f;
					for (const int32 Other : Around)
					{
						Sum += Push[Other];
					}
					Smoothed = 0.5f * Push[Point] + 0.5f * Sum / static_cast<float>(Around.Num());
				}
				Next[Point] = FMath::Max(PointRequired[Point], Smoothed);
			});
			Swap(Push, Next);
			++Passes;
		}
	}

	// 4. Direction per point: its own normal, moved p / dot so the clearance along the surface normal
	// grows by p; the surface triangle's normal where the two disagree (the inside of a folded collar
	// faces the skin). Untracked points the smoothing reached borrow their neighbours' surface normal.
	TArray<FVector3f> Disp;
	Disp.Init(FVector3f::ZeroVector, NumPoints);
	TArray<uint8> DirKind;
	DirKind.Init(RestoreDirNone, NumPoints);
	TBitArray<> Untracked(false, NumPoints);
	// The surface normal each pushed point's clearance grows along (zero: none), for the seam join below.
	TArray<FVector3f> PushAlong;
	PushAlong.Init(FVector3f::ZeroVector, NumPoints);
	for (int32 Point = 0; Point < NumPoints; ++Point)
	{
		const float Amount = Push[Point];
		if (Amount == 0.0f)
		{
			continue;
		}
		FVector3f Along = PointSurface[Point];
		if (Along.IsZero())
		{
			Untracked[Point] = true;
			FVector3f Sum = FVector3f::ZeroVector;
			for (const int32 Other : Neighbours[Point])
			{
				Sum += PointSurface[Other];
			}
			Along = Sum.GetSafeNormal();
		}
		PushAlong[Point] = Along;
		const FVector3f& Own = PointNormal[Point];
		if (Along.IsZero())
		{
			if (!Own.IsZero())
			{
				Disp[Point] = Own * Amount;
				DirKind[Point] = RestoreDirGarment;
			}
			continue;
		}
		const float Dot = Own.IsZero() ? -1.0f : FVector3f::DotProduct(Own, Along);
		if (Dot < FMantleGarmentClearance::NormalMinDot)
		{
			Disp[Point] = Along * Amount;
			DirKind[Point] = RestoreDirSurface;
		}
		else
		{
			Disp[Point] = Own * (Amount / Dot);
			DirKind[Point] = RestoreDirGarment;
		}
	}

	// The push VECTORS smoothed as many passes as the scalars were (at a concave corner of the
	// wearer neighbouring points were pushed along normals 90 degrees apart); each keeps at least its
	// smoothed scalar length, so the clearance is still restored.
	if (SmoothIters > 0)
	{
		TArray<FVector3f> NextDisp;
		NextDisp.SetNumUninitialized(NumPoints);
		for (int32 Pass = 0; Pass < SmoothIters; ++Pass)
		{
			ParallelFor(NumPoints, [&Disp, &NextDisp, &Neighbours](const int32 Point)
			{
				const TArray<int32, TInlineAllocator<8>>& Around = Neighbours[Point];
				if (Around.Num() == 0)
				{
					NextDisp[Point] = Disp[Point];
					return;
				}
				FVector3f Sum = FVector3f::ZeroVector;
				for (const int32 Other : Around)
				{
					Sum += Disp[Other];
				}
				NextDisp[Point] = 0.5f * Disp[Point] + 0.5f * Sum / static_cast<float>(Around.Num());
			});
			Swap(Disp, NextDisp);
		}
		for (int32 Point = 0; Point < NumPoints; ++Point)
		{
			const float Length = Disp[Point].Size();
			const float Want = FMath::Abs(Push[Point]);
			if (Want > 0.0f && Length > UE_KINDA_SMALL_NUMBER && Length < Want)
			{
				Disp[Point] *= Want / Length;
			}
		}
	}

	// Copies of one position in different sections (Seams) are points of their own sections above,
	// each pushed its own way (a hard edge between sections gives each its own normal), and the seam
	// opened. From here they are one point: pushed along one shared direction, the mean of their pushes,
	// by the largest length any of them needs. A point's need is its push's gain along the surface normal
	// its clearance grows along, divided by the cosine between that normal and the shared direction, so
	// no copy gains less clearance than its own push gave it (the "outermost copy"); a point with no such
	// normal uses its own push direction. Where a cosine falls below NormalMinDot the shared direction is
	// the copies' mean surface normal instead, and the cosine is floored at NormalMinDot. The flip limiter
	// below then works on the joined points. Seams ids are any non-negative ints, INDEX_NONE for none.
	int32 NumMoves = NumPoints;
	int32 SeamJoined = 0;
	if (Seams.Num() == N)
	{
		TArray<int32> SeamOfPoint;
		SeamOfPoint.Init(INDEX_NONE, NumPoints);
		for (int32 Vertex = 0; Vertex < N; ++Vertex)
		{
			if (Seams[Vertex] >= 0 && SeamOfPoint[Weld[Vertex]] == INDEX_NONE)
			{
				SeamOfPoint[Weld[Vertex]] = Seams[Vertex];
			}
		}
		TArray<int32> Move;
		Move.SetNumUninitialized(NumPoints);
		TMap<int32, int32> MoveOfSeam;
		NumMoves = 0;
		for (int32 Point = 0; Point < NumPoints; ++Point)
		{
			const int32 Seam = SeamOfPoint[Point];
			if (Seam == INDEX_NONE)
			{
				Move[Point] = NumMoves++;
			}
			else if (const int32* Found = MoveOfSeam.Find(Seam))
			{
				Move[Point] = *Found;
			}
			else
			{
				MoveOfSeam.Add(Seam, NumMoves);
				Move[Point] = NumMoves++;
			}
		}
		if (NumMoves < NumPoints)
		{
			TArray<int32> Members;
			Members.Init(0, NumMoves);
			for (int32 Point = 0; Point < NumPoints; ++Point)
			{
				++Members[Move[Point]];
			}
			TArray<FVector3f> MoveDisp;
			MoveDisp.Init(FVector3f::ZeroVector, NumMoves);
			TArray<float> MovePush;
			MovePush.Init(0.0f, NumMoves);
			TArray<uint8> MoveKind;
			MoveKind.Init(RestoreDirNone, NumMoves);
			TBitArray<> MoveUntracked(false, NumMoves);
			TArray<FVector3f> SumDisp;
			SumDisp.Init(FVector3f::ZeroVector, NumMoves);
			TArray<FVector3f> SumSurface;
			SumSurface.Init(FVector3f::ZeroVector, NumMoves);
			// The surface normal a point's clearance grows along: the one its push used, else its tracked copies'.
			auto SurfaceOf = [&PushAlong, &PointSurface](const int32 Point)
			{
				return PushAlong[Point].IsZero() ? PointSurface[Point] : PushAlong[Point];
			};
			for (int32 Point = 0; Point < NumPoints; ++Point)
			{
				const int32 M = Move[Point];
				if (Members[M] == 1)
				{
					MoveDisp[M] = Disp[Point];
					MovePush[M] = Push[Point];
					MoveKind[M] = DirKind[Point];
					MoveUntracked[M] = Untracked[Point];
					continue;
				}
				MovePush[M] = FMath::Max(MovePush[M], Push[Point]);
				if (Disp[Point].IsZero())
				{
					continue;
				}
				SumDisp[M] += Disp[Point];
				SumSurface[M] += SurfaceOf(Point);
				MoveKind[M] = MoveKind[M] == RestoreDirNone ? DirKind[Point] : MoveKind[M];
				MoveUntracked[M] = MoveUntracked[M] || Untracked[Point];
			}
			TArray<FVector3f> Shared;
			Shared.Init(FVector3f::ZeroVector, NumMoves);
			for (int32 M = 0; M < NumMoves; ++M)
			{
				Shared[M] = Members[M] > 1 ? SumDisp[M].GetSafeNormal() : FVector3f::ZeroVector;
			}
			// The cosine of a point's need against its group's shared direction.
			auto CosineOf = [&Disp, &SurfaceOf](const int32 Point, const FVector3f& Direction)
			{
				const FVector3f Along = SurfaceOf(Point);
				return FVector3f::DotProduct(Direction, Along.IsZero() ? Disp[Point].GetSafeNormal() : Along);
			};
			TBitArray<> Grazing(false, NumMoves);
			for (int32 Point = 0; Point < NumPoints; ++Point)
			{
				const int32 M = Move[Point];
				if (Members[M] > 1 && !Disp[Point].IsZero() && CosineOf(Point, Shared[M]) < FMantleGarmentClearance::NormalMinDot)
				{
					Grazing[M] = true;
				}
			}
			for (int32 M = 0; M < NumMoves; ++M)
			{
				const FVector3f MeanSurface = SumSurface[M].GetSafeNormal();
				if (Members[M] > 1 && Grazing[M] && !MeanSurface.IsZero())
				{
					Shared[M] = MeanSurface;
				}
			}
			TArray<float> Length;
			Length.Init(0.0f, NumMoves);
			for (int32 Point = 0; Point < NumPoints; ++Point)
			{
				const int32 M = Move[Point];
				if (Members[M] == 1 || Disp[Point].IsZero() || Shared[M].IsZero())
				{
					continue;
				}
				const FVector3f Along = SurfaceOf(Point);
				const float Gain = Along.IsZero() ? Disp[Point].Size() : FVector3f::DotProduct(Disp[Point], Along);
				const float Cosine = FMath::Max(CosineOf(Point, Shared[M]), FMantleGarmentClearance::NormalMinDot);
				Length[M] = FMath::Max(Length[M], Gain / Cosine);
			}
			for (int32 M = 0; M < NumMoves; ++M)
			{
				if (Members[M] > 1)
				{
					MoveDisp[M] = Shared[M] * Length[M];
					SeamJoined += Members[M];
				}
			}
			for (int32 Vertex = 0; Vertex < N; ++Vertex)
			{
				Weld[Vertex] = Move[Weld[Vertex]];
			}
			Disp = MoveTemp(MoveDisp);
			Push = MoveTemp(MovePush);
			DirKind = MoveTemp(MoveKind);
			Untracked = MoveTemp(MoveUntracked);
		}
		else
		{
			NumMoves = NumPoints;
		}
	}

	// 5. Push, then the flip limiter over the triangles a pushed point belongs to, against their
	// pre-push face and the authored one (where the pre-push face still agrees with it).
	const TArray<FVector3f> Pre = Garment.Positions;
	TArray<float> Scale;
	Scale.Init(1.0f, NumMoves);
	auto Apply = [&Garment, &Pre, &Disp, &Scale, &Weld, N]()
	{
		ParallelFor(N, [&Garment, &Pre, &Disp, &Scale, &Weld](const int32 Vertex)
		{
			const int32 Point = Weld[Vertex];
			Garment.Positions[Vertex] = Pre[Vertex] + Disp[Point] * Scale[Point];
		});
	};
	TArray<FRestoreTriangle> Triangles;
	for (int32 Triangle = 0; Triangle < Garment.NumTriangles(); ++Triangle)
	{
		FRestoreTriangle Tri;
		bool bMoved = false;
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			Tri.V[Corner] = static_cast<int32>(Garment.Indices[Triangle * 3 + Corner]);
			bMoved |= !Disp[Weld[Tri.V[Corner]]].IsZero();
		}
		if (!bMoved)
		{
			continue;
		}
		Tri.PreCross = FaceCross(Pre, Garment.Indices, Triangle);
		Tri.bPre = Tri.PreCross.Size() > FlipDegenerateCross;
		if (bAuthored)
		{
			// A triangle the warp itself already turned against the authored garment is not the push's to fix.
			Tri.AuthoredCross = FaceCross(Authored->Positions, Authored->Indices, Triangle);
			Tri.bAuthored = Tri.AuthoredCross.Size() > FlipDegenerateCross && FVector3d::DotProduct(Tri.AuthoredCross, Tri.PreCross) >= 0.0;
		}
		if (Tri.bPre || Tri.bAuthored)
		{
			Triangles.Add(Tri);
		}
	}
	auto IsFlipped = [&Garment](const FRestoreTriangle& Tri)
	{
		const FVector3d P0(Garment.Positions[Tri.V[0]]);
		const FVector3d P1(Garment.Positions[Tri.V[1]]);
		const FVector3d P2(Garment.Positions[Tri.V[2]]);
		const FVector3d Cross = (P1 - P0) ^ (P2 - P0);
		return (Tri.bPre && FVector3d::DotProduct(Tri.PreCross, Cross) < 0.0) || (Tri.bAuthored && FVector3d::DotProduct(Tri.AuthoredCross, Cross) < 0.0);
	};
	Apply();
	int32 FlippedBefore = 0;
	for (const FRestoreTriangle& Tri : Triangles)
	{
		FlippedBefore += IsFlipped(Tri) ? 1 : 0;
	}

	// A flipped triangle halves the push of its pushed points, up to MaxHalvings passes ...
	int32 HalvingPasses = 0;
	for (int32 Pass = 0; FlippedBefore > 0 && Pass < FMantleGarmentClearance::MaxHalvings; ++Pass)
	{
		TBitArray<> Halve(false, NumMoves);
		bool bAny = false;
		for (const FRestoreTriangle& Tri : Triangles)
		{
			if (IsFlipped(Tri))
			{
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const int32 Point = Weld[Tri.V[Corner]];
					if (!Disp[Point].IsZero())
					{
						Halve[Point] = true;
						bAny = true;
					}
				}
			}
		}
		if (!bAny)
		{
			break;
		}
		++HalvingPasses;
		for (TConstSetBitIterator<> It(Halve); It; ++It)
		{
			Scale[It.GetIndex()] *= 0.5f;
		}
		Apply();
	}
	// ... then the points of a triangle still flipped get no push. A triangle flipped by the push has a
	// point with a push left, so every pass zeroes one at least and the loop ends.
	for (int32 Guard = 0; FlippedBefore > 0 && Guard <= NumMoves; ++Guard)
	{
		bool bAny = false;
		for (const FRestoreTriangle& Tri : Triangles)
		{
			if (IsFlipped(Tri))
			{
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const int32 Point = Weld[Tri.V[Corner]];
					if (Scale[Point] > 0.0f && !Disp[Point].IsZero())
					{
						Scale[Point] = 0.0f;
						bAny = true;
					}
				}
			}
		}
		if (!bAny)
		{
			break;
		}
		Apply();
	}
	int32 FlippedLeft = 0;
	for (const FRestoreTriangle& Tri : Triangles)
	{
		FlippedLeft += IsFlipped(Tri) ? 1 : 0;
	}

	// Counts over vertices: every copy of a point counts.
	FMantleClearanceRestoreStats Stats;
	double SumPush = 0.0;
	for (int32 Vertex = 0; Vertex < N; ++Vertex)
	{
		const int32 Point = Weld[Vertex];
		if (Disp[Point].IsZero())
		{
			continue;
		}
		Stats.Limited += Scale[Point] < 1.0f ? 1 : 0;
		if (Scale[Point] <= 0.0f)
		{
			++Stats.Zeroed;
			continue;
		}
		++Stats.Moved;
		Stats.AlongGarment += DirKind[Point] == RestoreDirGarment ? 1 : 0;
		Stats.AlongSurface += DirKind[Point] == RestoreDirSurface ? 1 : 0;
		Stats.UntrackedMoved += Untracked[Point] ? 1 : 0;
		const double Gained = static_cast<double>(FMath::Abs(Push[Point])) * Scale[Point];
		SumPush += Gained;
		Stats.MaxPushCm = FMath::Max(Stats.MaxPushCm, Gained);
		Stats.MaxMoveCm = FMath::Max(Stats.MaxMoveCm, static_cast<double>(Disp[Point].Size()) * Scale[Point]);
	}
	Stats.bRan = true;
	Stats.Tracked = Target.NumTracked;
	Stats.Below = Below;
	Stats.Coupled = Coupled;
	Stats.SmoothIters = Passes;
	Stats.Points = NumPoints;
	Stats.SeamJoined = SeamJoined;
	Stats.FlippedBefore = FlippedBefore;
	Stats.HalvingPasses = HalvingPasses;
	Stats.FlippedLeft = FlippedLeft;
	Stats.MeanPushCm = Stats.Moved > 0 ? SumPush / Stats.Moved : 0.0;
	Stats.Ms = NowMs() - Start;
	if (OutStats)
	{
		*OutStats = Stats;
	}
	return true;
}
