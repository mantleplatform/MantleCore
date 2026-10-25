// Copyright Joshua Eger. All Rights Reserved.
#include "MantleBodyCoverage.h"

#include "MantleGarmentGeometry.h"
#include "MantleRuntime.h"
#include "MantleRuntimeSkeletalMeshBuilder.h"
#include "MantleSkinMask.h"
#include "Async/ParallelFor.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleBodyCoveragePrivate
{
	bool RefuseBody(const TCHAR* Step, const FString& Error, FString* OutError)
	{
		UE_LOG(LogMantle, Error, TEXT("MANTLE_GARMENTFIT FAIL %s: %s"), Step, *Error);
		if (OutError)
		{
			*OutError = Error;
		}
		return false;
	}

	/** Unit face normal of (A, B, C) by winding, zero for a degenerate triangle. */
	FVector3f SinkFaceNormal(const FVector3f& A, const FVector3f& B, const FVector3f& C)
	{
		return FVector3f::CrossProduct(B - A, C - A).GetSafeNormal(1.0e-12f);
	}

	/** Kept triangles with a Moved corner, Before to After: folded (dot < 0), steep (dot < SteepDot), largest turn. Degenerate-before ones are skipped. */
	void MeasureTurn(TConstArrayView<uint32> KeptIndices, const TBitArray<>& Moved, TConstArrayView<FVector3f> Before, TConstArrayView<FVector3f> After,
		int32& OutFolded, int32& OutSteep, float& OutMaxTurnDeg)
	{
		OutFolded = 0;
		OutSteep = 0;
		OutMaxTurnDeg = 0.0f;
		const int32 N = Before.Num();
		if (After.Num() != N || Moved.Num() != N)
		{
			return;
		}
		int32 Measured = 0;
		float MinDot = 1.0f;
		for (int32 Index = 0; Index + 2 < KeptIndices.Num(); Index += 3)
		{
			const int32 A = static_cast<int32>(KeptIndices[Index]);
			const int32 B = static_cast<int32>(KeptIndices[Index + 1]);
			const int32 C = static_cast<int32>(KeptIndices[Index + 2]);
			if (A >= N || B >= N || C >= N || !(Moved[A] || Moved[B] || Moved[C]))
			{
				continue;
			}
			const FVector3f N0 = SinkFaceNormal(Before[A], Before[B], Before[C]);
			if (N0.IsZero())
			{
				continue;
			}
			// A triangle collapsed by the move has no normal: steep (dot 0), not folded.
			const FVector3f N1 = SinkFaceNormal(After[A], After[B], After[C]);
			const float Dot = FVector3f::DotProduct(N0, N1);
			++Measured;
			OutFolded += Dot < 0.0f ? 1 : 0;
			OutSteep += Dot < FMantleSinkApplyStats::SteepDot ? 1 : 0;
			MinDot = FMath::Min(MinDot, Dot);
		}
		OutMaxTurnDeg = Measured > 0 ? FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(MinDot, -1.0f, 1.0f))) : 0.0f;
	}

	/** A kept triangle with a moved corner and its reference face normals (Normal1 zero when there is no second reference). */
	struct FSinkTriangle
	{
		int32 V[3] = { 0, 0, 0 };
		FVector3f Normal0 = FVector3f::ZeroVector;
		FVector3f Normal1 = FVector3f::ZeroVector;
	};

	/** One hit of a sink ray: its height along the ray, whether its triangle faces outward, its garment. */
	struct FSinkHit
	{
		float T = 0.0f;
		bool bOut = false;
		int32 Garment = 0;
	};

	/** A garment triangle in the sink's combined list. */
	struct FSinkTriangleRef
	{
		int32 Garment = 0;
		int32 Triangle = 0;
	};

	/** The whole body surface in a grid, for the across-body test and the near-half rule. */
	struct FBodySurface
	{
		TConstArrayView<FVector3f> Positions;
		TConstArrayView<uint32> Indices;
		MantleGarmentGeometry::FBoxGrid Grid;

		void Build(TConstArrayView<FVector3f> InPositions, TConstArrayView<uint32> InIndices, const float CellCm)
		{
			Positions = InPositions;
			Indices = InIndices;
			TArray<FBox3f> Boxes;
			Boxes.SetNumUninitialized(Indices.Num() / 3);
			for (int32 Triangle = 0; Triangle < Boxes.Num(); ++Triangle)
			{
				const FVector3f& A = Positions[Indices[Triangle * 3]];
				const FVector3f& B = Positions[Indices[Triangle * 3 + 1]];
				const FVector3f& C = Positions[Indices[Triangle * 3 + 2]];
				Boxes[Triangle] = FVector3f::CrossProduct(B - A, C - A).SizeSquared() <= MantleGarmentGeometry::DegenerateCrossSq
					? FBox3f(ForceInit) : FBox3f(A.ComponentMin(B).ComponentMin(C), A.ComponentMax(B).ComponentMax(C));
			}
			Grid.Build(Boxes, CellCm);
		}

		/**
		 * Whether the body is thinner than Distance at body vertex VertexIndex: a body surface lies on the ray going back
		 * from the vertex (along -Normal) within Distance. The vertex's own skin is skipped: the ray starts
		 * AcrossBodyEpsilonCm in, and the triangles that use the vertex never count (a crease's other side would).
		 */
		bool ThinnerThan(const int32 VertexIndex, const FVector3f& Normal, const float Distance) const
		{
			const float Epsilon = FMantleBodySink::AcrossBodyEpsilonCm;
			if (Indices.Num() == 0 || Distance <= Epsilon)
			{
				return false;
			}
			const uint32 Own = static_cast<uint32>(VertexIndex);
			const FVector3f Dir = -Normal;
			const FVector3f From = Positions[VertexIndex] + Dir * Epsilon;
			const float Reach = Distance - Epsilon;
			const FVector3f To = From + Dir * Reach;
			const FIntVector Lo = Grid.CellOf(From.ComponentMin(To));
			const FIntVector Hi = Grid.CellOf(From.ComponentMax(To));
			bool bFound = false;
			for (int32 Z = Lo.Z; Z <= Hi.Z && !bFound; ++Z)
			{
				for (int32 Y = Lo.Y; Y <= Hi.Y && !bFound; ++Y)
				{
					for (int32 X = Lo.X; X <= Hi.X && !bFound; ++X)
					{
						Grid.ForEachInCell(FIntVector(X, Y, Z), [&](const int32 Triangle)
						{
							const uint32 A = Indices[Triangle * 3];
							const uint32 B = Indices[Triangle * 3 + 1];
							const uint32 C = Indices[Triangle * 3 + 2];
							float T = 0.0f;
							bFound = bFound || (A != Own && B != Own && C != Own
								&& MantleGarmentGeometry::RayHitsTriangle(From, Dir, Positions[A], Positions[B], Positions[C], Reach, T));
						});
					}
				}
			}
			return bFound;
		}
	};
}

// ---------------------------------------------------------------------------------------------------------------
// FMantleGarmentCoverage
// ---------------------------------------------------------------------------------------------------------------

bool FMantleGarmentCoverage::Compute(const FMantleGarmentSurface& Garment, TConstArrayView<FVector3f> BodyPositions, TConstArrayView<FVector3f> BodyNormals,
	FMantleGarmentCoverage& Out, const float ReachCm, FString* OutError, TConstArrayView<uint32> BodyIndices, const float NearShare, const float EdgeTolerance)
{
	using namespace MantleGarmentGeometry;
	using namespace MantleBodyCoveragePrivate;
	const double T0 = NowMs();
	Out = FMantleGarmentCoverage();
	Out.MaxDistanceCm = ReachCm;
	const int32 NB = BodyPositions.Num();
	const int32 NG = Garment.Positions.Num();
	Out.Covered.Init(false, NB);
	Out.Under.Init(false, NB);
	if (NB == 0 || NG == 0 || Garment.Indices.Num() < 3 || BodyNormals.Num() != NB || Garment.Normals.Num() != NG || ReachCm <= 0.0f
		|| !IndicesInRange(Garment.Indices, NG))
	{
		return RefuseBody(TEXT("coverage"), FString::Printf(TEXT("bad inputs (body verts %d, normals %d, garment verts %d, normals %d, indices %d, reach %.2f cm)"),
			NB, BodyNormals.Num(), NG, Garment.Normals.Num(), Garment.Indices.Num(), ReachCm), OutError);
	}
	const bool bNearHalf = NearShare > 0.0f && BodyIndices.Num() > 0;
	if (bNearHalf && !IndicesInRange(BodyIndices, NB))
	{
		return RefuseBody(TEXT("coverage"), FString::Printf(TEXT("%d body indices out of range or not whole triangles"), BodyIndices.Num()), OutError);
	}
	FBodySurface Body;
	if (bNearHalf)
	{
		Body.Build(BodyPositions, BodyIndices, RayCellCm);
	}

	// Triangle normals oriented by the garment's vertex normals (the facing tests), bounds, and the grid the rays walk.
	TArray<FVector3f> TriNormals;
	TArray<int8> TriSigns;
	TArray<FBox3f> TriBoxes;
	Out.GarmentTriangles = TriangleFrames(Garment.Positions, Garment.Normals, Garment.Indices, TriNormals, TriSigns, TriBoxes);
	FBoxGrid Grid;
	Grid.Build(TriBoxes, RayCellCm);
	Out.GridCells = Grid.CellIds.Num();

	// The segment walked starts InsideReachCm under the skin and runs LayerGapCm past the reach (hits there only tell
	// whether a facing-away hit is backed by a second layer); hits between the ray's start and the reach cover.
	enum : uint8 { KindNotHit = 0, KindCovered = 1, KindTight = 2, KindUnder = 3 };
	struct FRayResult
	{
		uint8 Kind = 0;
		bool bFacingAway = false;
		bool bAcrossGap = false;
		bool bFarSide = false;
		float HitCm = 0.0f;
	};
	TArray<FRayResult> Results;
	Results.SetNum(NB);
	const float StartT = InsideReachCm + RayStartOffsetCm;
	const float ReachT = StartT + ReachCm;
	const float WalkT = ReachT + LayerGapCm;
	const float TightT = InsideReachCm - TightBandCm;
	ParallelFor(NB, [&](const int32 BodyVertex)
	{
		const FVector3f Normal = BodyNormals[BodyVertex].GetSafeNormal();
		if (Normal.IsNearlyZero())
		{
			return;
		}
		const FVector3f From = BodyPositions[BodyVertex] - Normal * InsideReachCm;
		const FVector3f To = From + Normal * WalkT;
		const FIntVector Lo = Grid.CellOf(From.ComponentMin(To));
		const FIntVector Hi = Grid.CellOf(From.ComponentMax(To));
		struct FHit
		{
			float T;
			int32 Triangle;
		};
		TArray<FHit, TInlineAllocator<16>> Hits;
		for (int32 Z = Lo.Z; Z <= Hi.Z; ++Z)
		{
			for (int32 Y = Lo.Y; Y <= Hi.Y; ++Y)
			{
				for (int32 X = Lo.X; X <= Hi.X; ++X)
				{
					Grid.ForEachInCell(FIntVector(X, Y, Z), [&](const int32 Triangle)
					{
						float HitT = 0.0f;
						// A triangle listed in several of the walked cells is kept once.
						if (RayHitsTriangle(From, Normal, Garment.Positions[Garment.Indices[Triangle * 3]], Garment.Positions[Garment.Indices[Triangle * 3 + 1]],
								Garment.Positions[Garment.Indices[Triangle * 3 + 2]], WalkT, HitT, EdgeTolerance)
							&& !Hits.ContainsByPredicate([Triangle](const FHit& Hit) { return Hit.Triangle == Triangle; }))
						{
							Hits.Add({ HitT, Triangle });
						}
					});
				}
			}
		}

		int32 Nearest = INDEX_NONE;
		int32 Under = INDEX_NONE;
		float ShallowestUnderT = -1.0f;
		bool bBand = false;
		for (int32 Index = 0; Index < Hits.Num(); ++Index)
		{
			if (Hits[Index].T < StartT)
			{
				if (FVector3f::DotProduct(Normal, TriNormals[Hits[Index].Triangle]) > 0.0f)
				{
					// The garment passes under the skin here (its outside faces the way the skin does): the deepest such hit.
					if (Under == INDEX_NONE || Hits[Index].T < Hits[Under].T)
					{
						Under = Index;
					}
					ShallowestUnderT = FMath::Max(ShallowestUnderT, Hits[Index].T);
				}
				else if (Hits[Index].T >= TightT)
				{
					bBand = true;
				}
			}
			else if (Hits[Index].T <= ReachT && (Nearest == INDEX_NONE || Hits[Index].T < Hits[Nearest].T))
			{
				Nearest = Index;
			}
		}
		FRayResult& Result = Results[BodyVertex];
		if (Nearest == INDEX_NONE)
		{
			// The near-half rule, on the shallowest such hit (a deeper one is further past the middle).
			const float UnderDepthCm = InsideReachCm - ShallowestUnderT;
			if (Under != INDEX_NONE && bNearHalf && UnderDepthCm > 0.0f
				&& Body.ThinnerThan(BodyVertex, Normal, UnderDepthCm / NearShare))
			{
				Result.bFarSide = true;
				Result.Kind = bBand ? KindTight : KindNotHit;
				return;
			}
			if (Under != INDEX_NONE)
			{
				Result.Kind = KindUnder;
				Result.HitCm = Hits[Under].T - InsideReachCm;
				return;
			}
			Result.Kind = bBand ? KindTight : KindNotHit;
			return;
		}
		Result.Kind = KindCovered;
		Result.HitCm = Hits[Nearest].T - InsideReachCm;
		Result.bFacingAway = FVector3f::DotProduct(Normal, TriNormals[Hits[Nearest].Triangle]) < 0.0f;
		if (Result.bFacingAway)
		{
			// Backed by a second layer right behind the hit (the outer layer of a folded collar), or alone (a garment
			// across a gap, e.g. the shirt's side seen from the inner arm).
			const float NearT = Hits[Nearest].T;
			bool bBacked = false;
			for (const FHit& Hit : Hits)
			{
				bBacked |= Hit.T > NearT + SameSurfaceCm && Hit.T <= NearT + LayerGapCm;
			}
			Result.bAcrossGap = !bBacked;
		}
	});

	double HitSum = 0.0;
	for (int32 BodyVertex = 0; BodyVertex < NB; ++BodyVertex)
	{
		const FRayResult& Result = Results[BodyVertex];
		if (Result.Kind == KindCovered || Result.Kind == KindUnder)
		{
			Out.Covered[BodyVertex] = true;
			++Out.NumCovered;
			HitSum += Result.HitCm;
			Out.CoveredFacingAway += Result.bFacingAway ? 1 : 0;
			Out.CoveredAcrossGap += Result.bAcrossGap ? 1 : 0;
			Out.CoveredUnder += Result.Kind == KindUnder ? 1 : 0;
			Out.Under[BodyVertex] = Result.Kind == KindUnder;
		}
		else
		{
			++Out.NotHit;
			Out.MissedTight += Result.Kind == KindTight ? 1 : 0;
			Out.UnderFarSide += Result.bFarSide ? 1 : 0;
		}
	}
	Out.MeanHitCm = Out.NumCovered > 0 ? HitSum / Out.NumCovered : 0.0;
	Out.Ms = NowMs() - T0;
	return true;
}

FString FMantleGarmentCoverage::ToString() const
{
	return FString::Printf(TEXT("covered %d of %d (under the skin %d, facing away %d, across a gap %d), not hit %d (tight %d, far side %d), mean hit %.2f cm, reach %.1f cm, %d tris in %d cells, %.2f ms"),
		NumCovered, Covered.Num(), CoveredUnder, CoveredFacingAway, CoveredAcrossGap, NotHit, MissedTight, UnderFarSide, MeanHitCm, MaxDistanceCm, GarmentTriangles, GridCells, Ms);
}

// ---------------------------------------------------------------------------------------------------------------
// FMantleBodySinkApply
// ---------------------------------------------------------------------------------------------------------------

void FMantleBodySinkApply::Sink(TConstArrayView<FVector3f> Base, TConstArrayView<FVector3f> Normals, TConstArrayView<uint32> KeptIndices, TConstArrayView<float> AmountCm,
	TArray<FVector3f>& OutPositions, FMantleSinkApplyStats& OutStats, TConstArrayView<FVector3f> Original)
{
	using namespace MantleGarmentGeometry;
	using namespace MantleBodyCoveragePrivate;
	const int32 N = Base.Num();
	OutPositions = TArray<FVector3f>(Base.GetData(), N);
	OutStats = FMantleSinkApplyStats();
	if (N == 0 || Normals.Num() != N || AmountCm.Num() != N || (Original.Num() != 0 && Original.Num() != N) || !IndicesInRange(KeptIndices, N))
	{
		return;
	}

	// Points: welded positions with an amount (the largest of their copies'); every copy at such a position moves with it.
	TArray<int32> Weld;
	const int32 NumWelded = WeldByPosition(Base, Weld);
	TArray<float> WeldAmount;
	WeldAmount.Init(0.0f, NumWelded);
	for (int32 Vertex = 0; Vertex < N; ++Vertex)
	{
		WeldAmount[Weld[Vertex]] = FMath::Max(WeldAmount[Weld[Vertex]], AmountCm[Vertex]);
	}
	TArray<int32> PointOfWeld;
	PointOfWeld.Init(INDEX_NONE, NumWelded);
	TArray<float> PointAmount;
	TArray<int32> FirstVertex;
	TArray<int32> PointOf;
	PointOf.Init(INDEX_NONE, N);
	TBitArray<> Moved(false, N);
	for (int32 Vertex = 0; Vertex < N; ++Vertex)
	{
		const int32 Id = Weld[Vertex];
		if (WeldAmount[Id] <= 0.0f)
		{
			continue;
		}
		if (PointOfWeld[Id] == INDEX_NONE)
		{
			PointOfWeld[Id] = PointAmount.Add(WeldAmount[Id]);
			FirstVertex.Add(Vertex);
		}
		PointOf[Vertex] = PointOfWeld[Id];
		Moved[Vertex] = true;
		OutStats.TwinVertices += AmountCm[Vertex] <= 0.0f ? 1 : 0;
	}
	const int32 NumPoints = PointAmount.Num();
	OutStats.Points = NumPoints;
	if (NumPoints == 0)
	{
		return;
	}

	// Per point: the direction (the normalised sum of its copies' normals; the first copy's own when they cancel).
	TArray<FVector3f> Dir;
	Dir.Init(FVector3f::ZeroVector, NumPoints);
	for (TConstSetBitIterator<> It(Moved); It; ++It)
	{
		Dir[PointOf[It.GetIndex()]] += Normals[It.GetIndex()].GetSafeNormal();
	}
	for (int32 Point = 0; Point < NumPoints; ++Point)
	{
		Dir[Point] = Dir[Point].GetSafeNormal();
		if (Dir[Point].IsZero())
		{
			Dir[Point] = Normals[FirstVertex[Point]].GetSafeNormal();
		}
	}

	// The kept triangles a moved vertex belongs to, with their reference normals (degenerate ones cannot fold).
	TArray<FSinkTriangle> Triangles;
	for (int32 Index = 0; Index + 2 < KeptIndices.Num(); Index += 3)
	{
		FSinkTriangle Triangle;
		Triangle.V[0] = static_cast<int32>(KeptIndices[Index]);
		Triangle.V[1] = static_cast<int32>(KeptIndices[Index + 1]);
		Triangle.V[2] = static_cast<int32>(KeptIndices[Index + 2]);
		if (!(Moved[Triangle.V[0]] || Moved[Triangle.V[1]] || Moved[Triangle.V[2]]))
		{
			continue;
		}
		Triangle.Normal0 = SinkFaceNormal(Base[Triangle.V[0]], Base[Triangle.V[1]], Base[Triangle.V[2]]);
		if (Original.Num() == N)
		{
			Triangle.Normal1 = SinkFaceNormal(Original[Triangle.V[0]], Original[Triangle.V[1]], Original[Triangle.V[2]]);
		}
		if (!Triangle.Normal0.IsZero())
		{
			Triangles.Add(Triangle);
		}
	}
	OutStats.AffectedTriangles = Triangles.Num();

	TArray<float> Scale;
	Scale.Init(1.0f, NumPoints);
	auto Apply = [&]()
	{
		for (TConstSetBitIterator<> It(Moved); It; ++It)
		{
			const int32 Vertex = It.GetIndex();
			const int32 Point = PointOf[Vertex];
			OutPositions[Vertex] = Base[Vertex] - Dir[Point] * (PointAmount[Point] * Scale[Point]);
		}
	};
	auto IsFolded = [&OutPositions](const FSinkTriangle& Triangle)
	{
		const FVector3f Cross = FVector3f::CrossProduct(OutPositions[Triangle.V[1]] - OutPositions[Triangle.V[0]], OutPositions[Triangle.V[2]] - OutPositions[Triangle.V[0]]);
		return FVector3f::DotProduct(Cross, Triangle.Normal0) < 0.0f || FVector3f::DotProduct(Cross, Triangle.Normal1) < 0.0f;
	};
	Apply();

	// The fold limiter: a folded triangle halves the sink of its moved points, up to MaxHalvings passes ...
	for (int32 Pass = 0; Pass < FMantleSinkApplyStats::MaxHalvings; ++Pass)
	{
		TBitArray<> Halve(false, NumPoints);
		bool bAny = false;
		for (const FSinkTriangle& Triangle : Triangles)
		{
			if (IsFolded(Triangle))
			{
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const int32 Point = PointOf[Triangle.V[Corner]];
					if (Point != INDEX_NONE)
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
		++OutStats.HalvingPasses;
		for (TConstSetBitIterator<> It(Halve); It; ++It)
		{
			Scale[It.GetIndex()] *= 0.5f;
		}
		Apply();
	}
	// ... then the points of a triangle still folded go back to no sink. A folded triangle always has a point with a sink
	// left (with none it is its base self), so every pass zeroes one at least and the loop ends.
	for (int32 Guard = 0; Guard <= NumPoints; ++Guard)
	{
		bool bAny = false;
		for (const FSinkTriangle& Triangle : Triangles)
		{
			if (IsFolded(Triangle))
			{
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const int32 Point = PointOf[Triangle.V[Corner]];
					if (Point != INDEX_NONE && Scale[Point] > 0.0f)
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

	for (int32 Vertex = 0; Vertex < N; ++Vertex)
	{
		if (AmountCm[Vertex] > 0.0f)
		{
			const float S = Scale[PointOf[Vertex]];
			OutStats.LimitedVertices += S < 1.0f ? 1 : 0;
			OutStats.ZeroedVertices += S <= 0.0f ? 1 : 0;
		}
	}
	MeasureTurn(KeptIndices, Moved, Base, OutPositions, OutStats.Folded, OutStats.Steep, OutStats.MaxTurnDeg);
}

void FMantleBodySinkApply::ShrinkSeam(TConstArrayView<FVector3f> Base, TConstArrayView<FVector3f> Normals, TConstArrayView<uint32> KeptIndices, const TBitArray<>& Seam,
	const float ShrinkCm, TArray<FVector3f>& OutPositions, FMantleSinkApplyStats& OutStats)
{
	TArray<float> Amount;
	Amount.Init(0.0f, Base.Num());
	if (ShrinkCm > 0.0f && Seam.Num() == Base.Num())
	{
		for (TConstSetBitIterator<> It(Seam); It; ++It)
		{
			Amount[It.GetIndex()] = ShrinkCm;
		}
	}
	Sink(Base, Normals, KeptIndices, Amount, OutPositions, OutStats);
}

// ---------------------------------------------------------------------------------------------------------------
// FMantleBodySink
// ---------------------------------------------------------------------------------------------------------------

bool FMantleBodySink::Compute(TConstArrayView<FVector3f> BodyPositions, TConstArrayView<FVector3f> BodyNormals, TConstArrayView<uint32> KeptIndices,
	TConstArrayView<FMantleGarmentSurface> Garments, const FMantleBodySinkParams& Params, TArray<float>& OutSinkCm, FMantleBodySink& Out, FString* OutError,
	TConstArrayView<uint32> AcrossBodyIndices, const TBitArray<>* SinkOnly)
{
	using namespace MantleGarmentGeometry;
	using namespace MantleBodyCoveragePrivate;
	const double T0 = NowMs();
	Out = FMantleBodySink();
	Out.UnderCm = Params.UnderCm;
	Out.SmoothIters = FMath::Clamp(Params.SmoothIters, 0, MaxSmoothIters);
	const int32 NB = BodyPositions.Num();
	OutSinkCm.Init(0.0f, NB);
	if (NB == 0 || BodyNormals.Num() != NB || Params.UnderCm < 0.0f || Params.BackCm <= 0.0f || Params.MaxSinkCm <= 0.0f || !IndicesInRange(KeptIndices, NB)
		|| (SinkOnly && SinkOnly->Num() != NB))
	{
		return RefuseBody(TEXT("body sink"), FString::Printf(TEXT("bad inputs (body verts %d, normals %d, kept indices %d, under %.2f, back %.2f, max %.2f, sink-only bits %d)"),
			NB, BodyNormals.Num(), KeptIndices.Num(), Params.UnderCm, Params.BackCm, Params.MaxSinkCm, SinkOnly ? SinkOnly->Num() : -1), OutError);
	}

	// Every garment's triangles in one list, with their outward normals and bounds, and the grid the rays walk.
	TArray<FSinkTriangleRef> Refs;
	TArray<FVector3f> RefNormals;
	TArray<FBox3f> RefBoxes;
	for (int32 GarmentIndex = 0; GarmentIndex < Garments.Num(); ++GarmentIndex)
	{
		const FMantleGarmentSurface& Garment = Garments[GarmentIndex];
		const int32 NG = Garment.Positions.Num();
		if (Garment.Normals.Num() != NG || !IndicesInRange(Garment.Indices, NG))
		{
			return RefuseBody(TEXT("body sink"), FString::Printf(TEXT("garment %d: %d verts, %d normals, %d indices out of range or not whole triangles"),
				GarmentIndex, NG, Garment.Normals.Num(), Garment.Indices.Num()), OutError);
		}
		TArray<FVector3f> Normals;
		TArray<int8> Signs;
		TArray<FBox3f> Boxes;
		TriangleFrames(Garment.Positions, Garment.Normals, Garment.Indices, Normals, Signs, Boxes);
		for (int32 Triangle = 0; Triangle < Boxes.Num(); ++Triangle)
		{
			if (Boxes[Triangle].IsValid)
			{
				Refs.Add({ GarmentIndex, Triangle });
				RefNormals.Add(Normals[Triangle]);
				RefBoxes.Add(Boxes[Triangle]);
			}
		}
	}
	Out.GarmentTriangles = Refs.Num();
	if (Refs.Num() == 0)
	{
		Out.Ms = NowMs() - T0;
		return true;
	}
	FBoxGrid Grid;
	Grid.Build(RefBoxes, CellCm);

	// The across-body test and the near-half rule: the whole body surface in a grid of its own.
	const bool bAcrossBody = AcrossBodyIndices.Num() > 0;
	if (bAcrossBody && !IndicesInRange(AcrossBodyIndices, NB))
	{
		return RefuseBody(TEXT("body sink"), FString::Printf(TEXT("%d across-body indices out of range or not whole triangles"), AcrossBodyIndices.Num()), OutError);
	}
	const bool bNearHalf = bAcrossBody && Params.NearShare > 0.0f;
	FBodySurface Body;
	if (bAcrossBody)
	{
		Body.Build(BodyPositions, AcrossBodyIndices, CellCm);
	}
	const FBoxGrid& BodyGrid = Body.Grid;
	TArray<uint8> Across;
	Across.SetNumZeroed(NB);
	TArray<uint8> FarSide;
	FarSide.SetNumZeroed(NB);

	// Rendered vertices: used by a kept triangle.
	TBitArray<> Rendered(false, NB);
	for (const uint32 Index : KeptIndices)
	{
		Rendered[static_cast<int32>(Index)] = true;
	}

	// One ray per rendered vertex; the garment's height over the skin is that of the outermost outward-facing hit.
	enum : uint8 { SinkNone = 0, SinkOutside = 1, SinkInside = 2 };
	TArray<uint8> Kind;
	Kind.Init(SinkNone, NB);
	TArray<float> Required;
	Required.Init(0.0f, NB);
	const float RayBackCm = Params.BackCm;
	const float RayUnderCm = Params.UnderCm;
	const float WalkT = RayBackCm + RayUnderCm + ChainReachCm;
	ParallelFor(NB, [&](const int32 BodyVertex)
	{
		if (!Rendered[BodyVertex])
		{
			return;
		}
		const FVector3f Normal = BodyNormals[BodyVertex].GetSafeNormal();
		if (Normal.IsNearlyZero())
		{
			return;
		}
		const FVector3f From = BodyPositions[BodyVertex] - Normal * RayBackCm;
		const FVector3f To = From + Normal * WalkT;
		const FIntVector Lo = Grid.CellOf(From.ComponentMin(To));
		const FIntVector Hi = Grid.CellOf(From.ComponentMax(To));
		TArray<FSinkHit, TInlineAllocator<8>> Hits;
		TArray<int32, TInlineAllocator<8>> Seen;
		for (int32 Z = Lo.Z; Z <= Hi.Z; ++Z)
		{
			for (int32 Y = Lo.Y; Y <= Hi.Y; ++Y)
			{
				for (int32 X = Lo.X; X <= Hi.X; ++X)
				{
					Grid.ForEachInCell(FIntVector(X, Y, Z), [&](const int32 Ref)
					{
						if (Seen.Contains(Ref))
						{
							return;
						}
						const FMantleGarmentSurface& Garment = Garments[Refs[Ref].Garment];
						const int32 Triangle = Refs[Ref].Triangle;
						float HitT = 0.0f;
						if (RayHitsTriangle(From, Normal, Garment.Positions[Garment.Indices[Triangle * 3]], Garment.Positions[Garment.Indices[Triangle * 3 + 1]],
								Garment.Positions[Garment.Indices[Triangle * 3 + 2]], WalkT, HitT))
						{
							Seen.Add(Ref);
							Hits.Add({ HitT, FVector3f::DotProduct(RefNormals[Ref], Normal) >= 0.0f, Refs[Ref].Garment });
						}
					});
				}
			}
		}
		if (Hits.Num() == 0)
		{
			return;
		}
		Hits.Sort([](const FSinkHit& L, const FSinkHit& R) { return L.T < R.T; });
		float OuterT = -1.0f;
		for (const FSinkHit& RayHit : Hits)
		{
			if (RayHit.bOut)
			{
				OuterT = RayHit.T;
			}
		}
		if (OuterT < 0.0f)
		{
			return;
		}
		const float Height = OuterT - RayBackCm;
		if (bAcrossBody && Height < RayUnderCm && Height < 0.0f)
		{
			// Is there body surface between the hit (under the skin) and the vertex? Then the garment belongs to
			// another part seen through a thin one, not under this skin.
			const float SegLo = OuterT + AcrossBodyEpsilonCm;
			const float SegHi = RayBackCm - AcrossBodyEpsilonCm;
			bool bBetween = false;
			if (SegHi > SegLo)
			{
				const FVector3f SegA = From + Normal * SegLo;
				const FVector3f SegB = From + Normal * SegHi;
				const FIntVector CellLo = BodyGrid.CellOf(SegA.ComponentMin(SegB));
				const FIntVector CellHi = BodyGrid.CellOf(SegA.ComponentMax(SegB));
				for (int32 Z = CellLo.Z; Z <= CellHi.Z && !bBetween; ++Z)
				{
					for (int32 Y = CellLo.Y; Y <= CellHi.Y && !bBetween; ++Y)
					{
						for (int32 X = CellLo.X; X <= CellHi.X && !bBetween; ++X)
						{
							BodyGrid.ForEachInCell(FIntVector(X, Y, Z), [&](const int32 Triangle)
							{
								float BodyT = 0.0f;
								if (!bBetween && RayHitsTriangle(From, Normal, BodyPositions[AcrossBodyIndices[Triangle * 3]], BodyPositions[AcrossBodyIndices[Triangle * 3 + 1]],
										BodyPositions[AcrossBodyIndices[Triangle * 3 + 2]], SegHi, BodyT) && BodyT > SegLo)
								{
									bBetween = true;
								}
							});
						}
					}
				}
			}
			if (bBetween)
			{
				Across[BodyVertex] = 1;
				return;
			}
		}
		// A hit under the skin past the middle of a thin part (a sandal's footbed under a toe) is the far side's.
		if (bNearHalf && Height < 0.0f && Body.ThinnerThan(BodyVertex, Normal, -Height / Params.NearShare))
		{
			FarSide[BodyVertex] = 1;
			return;
		}
		Kind[BodyVertex] = Height < 0.0f ? SinkInside : SinkOutside;
		// #877: skin not under cloth on this wearer is measured (the skin-poke counts) but never moved.
		if (Height < RayUnderCm && (!SinkOnly || (*SinkOnly)[BodyVertex]))
		{
			Required[BodyVertex] = FMath::Min(RayUnderCm - Height, Params.MaxSinkCm);
		}
	});
	for (int32 BodyVertex = 0; BodyVertex < NB; ++BodyVertex)
	{
		Out.Tested += Rendered[BodyVertex] ? 1 : 0;
		Out.Hit += Kind[BodyVertex] != SinkNone ? 1 : 0;
		Out.Inside += Kind[BodyVertex] == SinkInside ? 1 : 0;
		Out.AcrossBody += Across[BodyVertex];
		Out.FarSide += FarSide[BodyVertex];
		Out.MaxRequiredCm = FMath::Max(Out.MaxRequiredCm, Required[BodyVertex]);
	}

	// Smoothing over the kept triangles' adjacency, welded by position, never below the required sink, so every vertex
	// still ends up under the garment.
	OutSinkCm = Required;
	if (Out.SmoothIters > 0 && Out.MaxRequiredCm > 0.0f)
	{
		TArray<int32> Weld;
		const int32 NumPoints = WeldByPosition(BodyPositions, Weld);
		TArray<float> Current;
		Current.Init(0.0f, NumPoints);
		for (int32 BodyVertex = 0; BodyVertex < NB; ++BodyVertex)
		{
			Current[Weld[BodyVertex]] = FMath::Max(Current[Weld[BodyVertex]], Required[BodyVertex]);
		}
		const TArray<float> PointRequired = Current;
		// #877: a point none of whose copies may sink stays at 0, so the smoothing never spreads onto it.
		TBitArray<> PointMay(SinkOnly == nullptr, NumPoints);
		if (SinkOnly)
		{
			for (TConstSetBitIterator<> It(*SinkOnly); It; ++It)
			{
				PointMay[Weld[It.GetIndex()]] = true;
			}
		}
		FPointNeighbours Neighbours;
		PointNeighbours(KeptIndices, Weld, NumPoints, Neighbours);
		TArray<float> Next;
		Next.SetNumUninitialized(NumPoints);
		for (int32 Pass = 0; Pass < Out.SmoothIters; ++Pass)
		{
			ParallelFor(NumPoints, [&](const int32 Point)
			{
				const TArray<int32, TInlineAllocator<8>>& Around = Neighbours[Point];
				if (!PointMay[Point])
				{
					Next[Point] = 0.0f;
					return;
				}
				if (Around.Num() == 0)
				{
					Next[Point] = Current[Point];
					return;
				}
				float Sum = 0.0f;
				for (const int32 Other : Around)
				{
					Sum += Current[Other];
				}
				Next[Point] = FMath::Max(PointRequired[Point], 0.5f * Current[Point] + 0.5f * Sum / static_cast<float>(Around.Num()));
			});
			Swap(Current, Next);
		}
		for (int32 BodyVertex = 0; BodyVertex < NB; ++BodyVertex)
		{
			OutSinkCm[BodyVertex] = Rendered[BodyVertex] ? Current[Weld[BodyVertex]] : 0.0f;
		}
	}
	double SinkSum = 0.0;
	for (int32 BodyVertex = 0; BodyVertex < NB; ++BodyVertex)
	{
		if (OutSinkCm[BodyVertex] > 0.0f)
		{
			++Out.Sunk;
			SinkSum += OutSinkCm[BodyVertex];
			Out.MaxSinkCm = FMath::Max(Out.MaxSinkCm, OutSinkCm[BodyVertex]);
		}
	}
	Out.MeanSinkCm = Out.Sunk > 0 ? static_cast<float>(SinkSum / Out.Sunk) : 0.0f;
	Out.Ms = NowMs() - T0;
	return true;
}

FString FMantleBodySink::ToString() const
{
	return FString::Printf(TEXT("tested %d rendered verts against %d garment tris: reached %d (inside %d, across the body %d, far side %d), sunk %d (max %.2f cm, mean %.2f cm, required max %.2f, under %.2f cm, %d passes), %.2f ms"),
		Tested, GarmentTriangles, Hit, Inside, AcrossBody, FarSide, Sunk, MaxSinkCm, MeanSinkCm, MaxRequiredCm, UnderCm, SmoothIters, Ms);
}

// ---------------------------------------------------------------------------------------------------------------
// FMantleDressedBody
// ---------------------------------------------------------------------------------------------------------------

bool FMantleDressedBody::Compute(const FMantleSkinnedMeshLOD& Body, TConstArrayView<FMantleGarmentSurface> Garments, const FMantleDressedBodyParams& Params,
	FMantleDressedBody& Out, FString* OutError, TConstArrayView<FMantleDressedMask> Masks)
{
	using namespace MantleGarmentGeometry;
	using namespace MantleBodyCoveragePrivate;
	const double T0 = NowMs();
	Out = FMantleDressedBody();
	const int32 NB = Body.NumVertices();
	Out.Positions = Body.Positions;
	Out.KeptIndices = Body.Indices;
	Out.Hidden.Init(false, NB);
	Out.KeptTriangles = Body.NumTriangles();
	if (NB == 0 || Body.Normals.Num() != NB || !IndicesInRange(Body.Indices, NB))
	{
		return RefuseBody(TEXT("dressed body"), FString::Printf(TEXT("bad body (%d verts, %d normals, %d indices)"), NB, Body.Normals.Num(), Body.Indices.Num()), OutError);
	}
	if (Garments.Num() == 0)
	{
		Out.TotalMs = NowMs() - T0;
		return true;
	}

	// 1. Coverage per garment on this wearer, and the union. #877 (bSkinMask): each garment hides its mask's bits (or,
	// without one, its coverage at the border's depth over this body) AND its coverage here.
	Out.Coverage.SetNum(Garments.Num());
	if (Params.bSkinMask)
	{
		Out.WearerCovered.Init(false, NB);
		Out.SlackMm.Init(0, NB);
	}
	for (int32 GarmentIndex = 0; GarmentIndex < Garments.Num(); ++GarmentIndex)
	{
		FString Error;
		const FMantleGarmentCoverage& Coverage = Out.Coverage[GarmentIndex];
		if (!FMantleGarmentCoverage::Compute(Garments[GarmentIndex], Body.Positions, Body.Normals, Out.Coverage[GarmentIndex], Params.CoverageReachCm, &Error,
				Body.Indices, Params.NearShare, Params.EdgeTolerance))
		{
			return RefuseBody(TEXT("dressed body"), FString::Printf(TEXT("garment %d's coverage: %s"), GarmentIndex, *Error), OutError);
		}
		if (!Params.bSkinMask)
		{
			Out.Hidden.CombineWithBitwiseOR(Coverage.Covered, EBitwiseOperatorFlags::MaintainSize);
			continue;
		}
		Out.WearerCovered.CombineWithBitwiseOR(Coverage.Covered, EBitwiseOperatorFlags::MaintainSize);
		const FMantleDressedMask Mask = Masks.IsValidIndex(GarmentIndex) ? Masks[GarmentIndex] : FMantleDressedMask();
		TArray<uint8> WearerDepthMm;
		TConstArrayView<uint8> DepthMm;
		if (Mask.Mask && Mask.Mask->Num() == NB)
		{
			DepthMm = Mask.Mask->DepthMm;
		}
		else
		{
			// No source mask (it failed, or a test hook gave none): the border on this wearer's own coverage.
			++Out.FallbackGarments;
			FMantleSkinMask::SurfaceDepthMm(Body.Positions, Body.Indices, Coverage.Covered, WearerDepthMm);
			DepthMm = WearerDepthMm;
		}
		const uint8 BorderMm = FMantleSkinMask::BorderMm(Mask.BorderCm);
		for (int32 Vertex = 0; Vertex < NB; ++Vertex)
		{
			if (DepthMm[Vertex] < BorderMm)
			{
				continue;
			}
			++Out.MaskHidden;
			if (!Coverage.Covered[Vertex])
			{
				++Out.MaskKeptByWearer;
				continue;
			}
			Out.Hidden[Vertex] = true;
			Out.SlackMm[Vertex] = FMath::Max(Out.SlackMm[Vertex], static_cast<uint8>(DepthMm[Vertex] - BorderMm));
		}
	}
	Out.CoveredVertices = Out.Hidden.CountSetBits();
	Out.CoverageMs = NowMs() - T0;

	// 2. The builder's triangle filter (a triangle whose three vertices are covered is dropped) and the seam ring: covered
	// vertices a kept triangle still uses.
	const double T1 = NowMs();
	FMantleTriangleFilterResult Filter;
	FString FilterError;
	if (!FMantleRuntimeSkeletalMeshBuilder::FilterTriangles(Body, &Out.Hidden, Filter, &FilterError))
	{
		return RefuseBody(TEXT("dressed body"), FString::Printf(TEXT("the triangle filter: %s"), *FilterError), OutError);
	}
	Out.KeptIndices = MoveTemp(Filter.Indices);
	Out.KeptTriangles = Filter.KeptTriangles;
	Out.DroppedTriangles = Filter.DroppedTriangles;
	TBitArray<> SeamRing(false, NB);
	for (const uint32 Index : Out.KeptIndices)
	{
		if (Out.Hidden[static_cast<int32>(Index)])
		{
			SeamRing[static_cast<int32>(Index)] = true;
		}
	}
	Out.SeamVertices = SeamRing.CountSetBits();

	// 3. The seam shrink (welded, fold-limited).
	TArray<FVector3f> Shrunk;
	FMantleBodySinkApply::ShrinkSeam(Body.Positions, Body.Normals, Out.KeptIndices, SeamRing, Params.SeamShrinkCm, Shrunk, Out.Seam);

	// 4. The body sink under the fitted garments, measured on the shrunk skin, applied fold-limited against both the
	// shrunk and the original faces.
	Out.Positions = Shrunk;
	if (Params.Sink.UnderCm > 0.0f)
	{
		TArray<float> SinkCm;
		FString Error;
		// The near-half rule over the whole body surface (hidden triangles included), at the shrunk positions.
		FMantleBodySinkParams SinkParams = Params.Sink;
		SinkParams.NearShare = Params.NearShare;
		const TConstArrayView<uint32> SurfaceIndices = Params.NearShare > 0.0f ? TConstArrayView<uint32>(Body.Indices) : TConstArrayView<uint32>();
		if (!FMantleBodySink::Compute(Shrunk, Body.Normals, Out.KeptIndices, Garments, SinkParams, SinkCm, Out.Sink, &Error, SurfaceIndices,
				Params.bSkinMask ? &Out.WearerCovered : nullptr))
		{
			return RefuseBody(TEXT("dressed body"), FString::Printf(TEXT("the body sink: %s"), *Error), OutError);
		}
		if (Out.Sink.Sunk > 0)
		{
			FMantleBodySinkApply::Sink(Shrunk, Body.Normals, Out.KeptIndices, SinkCm, Out.Positions, Out.SinkApplied, Body.Positions);
		}
	}
	Out.SinkMs = NowMs() - T1;
	Out.TotalMs = NowMs() - T0;
	return true;
}

FString FMantleDressedBody::ToString() const
{
	FString Text = FString::Printf(TEXT("covered %d of %d verts, dropped %d of %d tris, seam %d verts (%d points, limited %d, zeroed %d, max turn %.1f deg), sink applied to %d points (limited %d, zeroed %d, max turn %.1f deg); sink: %s; coverage %.2f ms, seam + sink %.2f ms"),
		CoveredVertices, Hidden.Num(), DroppedTriangles, KeptTriangles + DroppedTriangles, SeamVertices, Seam.Points, Seam.LimitedVertices, Seam.ZeroedVertices, Seam.MaxTurnDeg,
		SinkApplied.Points, SinkApplied.LimitedVertices, SinkApplied.ZeroedVertices, SinkApplied.MaxTurnDeg, *Sink.ToString(), CoverageMs, SinkMs);
	if (SlackMm.Num() > 0)
	{
		Text += FString::Printf(TEXT("; skin mask: hides %d, kept by the wearer %d, under cloth %d, %d garments on the wearer fallback"),
			MaskHidden, MaskKeptByWearer, WearerCovered.CountSetBits(), FallbackGarments);
	}
	return Text;
}

bool FMantleDressedBody::LowerLODHidden(const FMantleSkinnedMesh& WearerBody, const FMantleDressedBody& Dressed, TArray<TBitArray<>>& Out, FString* OutError)
{
	using namespace MantleBodyCoveragePrivate;
	Out.Reset();
	if (Dressed.SlackMm.Num() == 0 || WearerBody.LODs.Num() < 2)
	{
		return true;
	}
	const FMantleSkinnedMeshLOD& LOD0 = WearerBody.LODs[0];
	const int32 N = LOD0.NumVertices();
	if (Dressed.SlackMm.Num() != N || Dressed.Hidden.Num() != N)
	{
		return RefuseBody(TEXT("dressed body LODs"), FString::Printf(TEXT("%d slack and %d hidden entries for %d LOD0 vertices"),
			Dressed.SlackMm.Num(), Dressed.Hidden.Num(), N), OutError);
	}
	Out.SetNum(WearerBody.LODs.Num() - 1);
	for (int32 LODIndex = 1; LODIndex < WearerBody.LODs.Num(); ++LODIndex)
	{
		const FMantleSkinnedMeshLOD& LOD = WearerBody.LODs[LODIndex];
		double EdgeSum = 0.0;
		int32 Edges = 0;
		for (int32 Index = 0; Index + 2 < LOD.Indices.Num(); Index += 3)
		{
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const uint32 A = LOD.Indices[Index + Corner];
				const uint32 B = LOD.Indices[Index + (Corner + 1) % 3];
				if (static_cast<int64>(FMath::Max(A, B)) < LOD.NumVertices())
				{
					EdgeSum += FVector3f::Dist(LOD.Positions[A], LOD.Positions[B]);
					++Edges;
				}
			}
		}
		// Half the LOD's mean edge past the border, in mm.
		const int32 NeedMm = Edges > 0 ? FMath::CeilToInt32(5.0 * EdgeSum / Edges) : 0;
		TBitArray<> Deep(false, N);
		for (TConstSetBitIterator<> It(Dressed.Hidden); It; ++It)
		{
			Deep[It.GetIndex()] = Dressed.SlackMm[It.GetIndex()] >= NeedMm;
		}
		FString Error;
		if (!FMantleRuntimeSkeletalMeshBuilder::TransferHiddenVertices(LOD0, Deep, LOD, Out[LODIndex - 1], &Error))
		{
			Out.Reset();
			return RefuseBody(TEXT("dressed body LODs"), FString::Printf(TEXT("LOD%d: %s"), LODIndex, *Error), OutError);
		}
	}
	return true;
}

FMantleSkinnedMesh FMantleDressedBody::MakeDressedMesh(const FMantleSkinnedMesh& WearerBody, const FMantleDressedBody& Dressed)
{
	FMantleSkinnedMesh Out;
	Out.Bones = WearerBody.Bones;
	Out.ReferencePose = WearerBody.ReferencePose;
	if (WearerBody.LODs.Num() > 0)
	{
		// Every LOD, so the dressed body keeps its LODs. The coverage, the seam shrink and the sink are
		// LOD0's (Dressed.Positions); the builder carries the hidden bits down to the lower LODs itself
		// (FMantleRuntimeSkeletalMeshBuilder::TransferHiddenVertices), which keep their own positions.
		Out.LODs = WearerBody.LODs;
		if (Dressed.Positions.Num() == Out.LODs[0].NumVertices())
		{
			Out.LODs[0].Positions = Dressed.Positions;
		}
	}
	return Out;
}
