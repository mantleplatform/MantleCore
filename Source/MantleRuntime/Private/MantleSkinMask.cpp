// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSkinMask.h"

#include "MantleGarmentGeometry.h"
#include "MantleRuntime.h"
#include "Async/ParallelFor.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleSkinMaskPrivate
{
	bool RefuseMask(const TCHAR* Step, const FString& Error, FString* OutError)
	{
		UE_LOG(LogMantle, Error, TEXT("MANTLE_GARMENTFIT FAIL %s: %s"), Step, *Error);
		if (OutError)
		{
			*OutError = Error;
		}
		return false;
	}

	/** A triangle the rays test, with the welded ids of its corners when it is body skin (INDEX_NONE for a garment's). */
	struct FRayTriangle
	{
		FVector3f A;
		FVector3f B;
		FVector3f C;
		int32 Weld[3] = { INDEX_NONE, INDEX_NONE, INDEX_NONE };

		bool IsBody() const { return Weld[0] != INDEX_NONE; }
		bool Uses(const int32 Point) const { return Weld[0] == Point || Weld[1] == Point || Weld[2] == Point; }
	};

	/** Adds Indices over Positions as ray triangles (Weld null: a garment's); degenerate ones are skipped. */
	void AddRayTriangles(TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Indices, const TArray<int32>* Weld, TArray<FRayTriangle>& Out, TArray<FBox3f>& OutBoxes)
	{
		for (int32 Index = 0; Index + 2 < Indices.Num(); Index += 3)
		{
			FRayTriangle Triangle;
			Triangle.A = Positions[Indices[Index]];
			Triangle.B = Positions[Indices[Index + 1]];
			Triangle.C = Positions[Indices[Index + 2]];
			if (FVector3f::CrossProduct(Triangle.B - Triangle.A, Triangle.C - Triangle.A).SizeSquared() <= MantleGarmentGeometry::DegenerateCrossSq)
			{
				continue;
			}
			if (Weld)
			{
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					Triangle.Weld[Corner] = (*Weld)[Indices[Index + Corner]];
				}
			}
			Out.Add(Triangle);
			OutBoxes.Add(FBox3f(Triangle.A.ComponentMin(Triangle.B).ComponentMin(Triangle.C), Triangle.A.ComponentMax(Triangle.B).ComponentMax(Triangle.C)));
		}
	}

	/**
	 * The grid's cells along the segment From + t Dir, t in [0, Length], in order (Amanatides-Woo). Visit(Cell) returns
	 * true to stop the walk; returns whether it was stopped. Dir must be a unit vector.
	 */
	template <typename FnType>
	bool WalkSegment(const MantleGarmentGeometry::FBoxGrid& Grid, const FVector3f& From, const FVector3f& Dir, const float Length, FnType&& Visit)
	{
		const float Cell = 1.0f / Grid.InvCell;
		FIntVector Current = Grid.CellOf(From);
		const FIntVector Last = Grid.CellOf(From + Dir * Length);
		int32 Step[3];
		float TMax[3];
		float TDelta[3];
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const float D = Dir[Axis];
			if (D > 0.0f)
			{
				Step[Axis] = 1;
				TMax[Axis] = ((Current[Axis] + 1) * Cell - From[Axis]) / D;
				TDelta[Axis] = Cell / D;
			}
			else if (D < 0.0f)
			{
				Step[Axis] = -1;
				TMax[Axis] = (Current[Axis] * Cell - From[Axis]) / D;
				TDelta[Axis] = -Cell / D;
			}
			else
			{
				Step[Axis] = 0;
				TMax[Axis] = TNumericLimits<float>::Max();
				TDelta[Axis] = TNumericLimits<float>::Max();
			}
		}
		// Never more cells than the segment can cross on each axis, plus the first.
		const int32 MaxCells = FMath::Abs(Last.X - Current.X) + FMath::Abs(Last.Y - Current.Y) + FMath::Abs(Last.Z - Current.Z) + 1;
		for (int32 Visited = 0; Visited < MaxCells; ++Visited)
		{
			if (Visit(Current))
			{
				return true;
			}
			const int32 Axis = TMax[0] < TMax[1] ? (TMax[0] < TMax[2] ? 0 : 2) : (TMax[1] < TMax[2] ? 1 : 2);
			if (TMax[Axis] > Length)
			{
				break;
			}
			Current[Axis] += Step[Axis];
			TMax[Axis] += TDelta[Axis];
		}
		return false;
	}

	/** Whether the segment meets a triangle of Triangles (Accept(Triangle) filters them) within Length. */
	template <typename FnType>
	bool SegmentBlocked(const MantleGarmentGeometry::FBoxGrid& Grid, TConstArrayView<FRayTriangle> Triangles, const FVector3f& From, const FVector3f& Dir,
		const float Length, FnType&& Accept)
	{
		return WalkSegment(Grid, From, Dir, Length, [&](const FIntVector& Cell)
		{
			bool bHit = false;
			Grid.ForEachInCell(Cell, [&](const int32 Index)
			{
				float T = 0.0f;
				const FRayTriangle& Triangle = Triangles[Index];
				bHit = bHit || (Accept(Triangle) && MantleGarmentGeometry::RayHitsTriangle(From, Dir, Triangle.A, Triangle.B, Triangle.C, Length, T,
					FMantleGarmentCoverage::SkinMaskEdgeTolerance));
			});
			return bHit;
		});
	}

	/** Count directions over the hemisphere around +Z, cosine-weighted (a Fibonacci spiral in the projected disc). Deterministic. */
	void HemisphereDirections(const int32 Count, TArray<FVector3f>& Out)
	{
		const float Golden = PI * (3.0f - FMath::Sqrt(5.0f));
		Out.SetNumUninitialized(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const float R = FMath::Sqrt((Index + 0.5f) / Count);
			const float Phi = Golden * Index;
			Out[Index] = FVector3f(R * FMath::Cos(Phi), R * FMath::Sin(Phi), FMath::Sqrt(FMath::Max(0.0f, 1.0f - R * R)));
		}
	}

	/** Count directions spread evenly over the whole sphere (a Fibonacci sphere). Deterministic. */
	void SphereDirections(const int32 Count, TArray<FVector3f>& Out)
	{
		const float Golden = PI * (3.0f - FMath::Sqrt(5.0f));
		Out.SetNumUninitialized(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const float Z = 1.0f - (2.0f * Index + 1.0f) / Count;
			const float R = FMath::Sqrt(FMath::Max(0.0f, 1.0f - Z * Z));
			const float Phi = Golden * Index;
			Out[Index] = FVector3f(R * FMath::Cos(Phi), R * FMath::Sin(Phi), Z);
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------
// FMantleSkinMask
// ---------------------------------------------------------------------------------------------------------------

bool FMantleSkinMask::Derive(const FMantleGarmentSurface& Garment, TConstArrayView<FVector3f> SourcePositions, TConstArrayView<FVector3f> HintNormals,
	TConstArrayView<uint32> TopologyIndices, const FMantleSkinMaskParams& Params, FMantleSkinMask& Out, FString* OutError)
{
	using namespace MantleGarmentGeometry;
	using namespace MantleSkinMaskPrivate;
	const double T0 = NowMs();
	Out = FMantleSkinMask();
	Out.BorderCm = Params.BorderCm;
	const int32 NB = SourcePositions.Num();
	Out.Enclosed.Init(false, NB);
	Out.Hidden.Init(false, NB);
	Out.DepthMm.Init(0, NB);
	if (NB == 0 || HintNormals.Num() != NB || TopologyIndices.Num() < 3 || !IndicesInRange(TopologyIndices, NB) || Params.Rays < 1 || Params.RayReachCm <= 0.0f
		|| Params.BorderCm <= 0.0f)
	{
		return RefuseMask(TEXT("skin mask"), FString::Printf(TEXT("bad inputs (source verts %d, hint normals %d, topology indices %d, rays %d, reach %.2f cm, border %.2f cm)"),
			NB, HintNormals.Num(), TopologyIndices.Num(), Params.Rays, Params.RayReachCm, Params.BorderCm), OutError);
	}

	// The source body's own normals: oriented face normals, area-weighted, summed per welded point.
	TArray<int32> Weld;
	const int32 NumPoints = WeldByPosition(SourcePositions, Weld);
	TArray<FVector3f> FaceNormals;
	TArray<int8> FaceSigns;
	TArray<FBox3f> FaceBoxes;
	TriangleFrames(SourcePositions, HintNormals, TopologyIndices, FaceNormals, FaceSigns, FaceBoxes);
	TArray<FVector3f> PointNormals;
	PointNormals.Init(FVector3f::ZeroVector, NumPoints);
	for (int32 Triangle = 0; Triangle < FaceNormals.Num(); ++Triangle)
	{
		const FVector3f& A = SourcePositions[TopologyIndices[Triangle * 3]];
		const FVector3f& B = SourcePositions[TopologyIndices[Triangle * 3 + 1]];
		const FVector3f& C = SourcePositions[TopologyIndices[Triangle * 3 + 2]];
		const FVector3f Weighted = FaceNormals[Triangle] * FVector3f::CrossProduct(B - A, C - A).Size();
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			PointNormals[Weld[TopologyIndices[Triangle * 3 + Corner]]] += Weighted;
		}
	}
	TArray<FVector3f> Normals;
	Normals.SetNumUninitialized(NB);
	for (int32 Vertex = 0; Vertex < NB; ++Vertex)
	{
		const FVector3f Normal = PointNormals[Weld[Vertex]].GetSafeNormal();
		Normals[Vertex] = Normal.IsZero() ? HintNormals[Vertex].GetSafeNormal() : Normal;
	}

	// 1a. The normal ray: today's per-vertex coverage, on the source body (P16 and the near-half rule included).
	FMantleGarmentCoverage Coverage;
	FString CoverageError;
	if (!FMantleGarmentCoverage::Compute(Garment, SourcePositions, Normals, Coverage, Params.NormalReachCm, &CoverageError, TopologyIndices, Params.NearShare,
			FMantleGarmentCoverage::SkinMaskEdgeTolerance))
	{
		return RefuseMask(TEXT("skin mask"), FString::Printf(TEXT("the normal-ray coverage: %s"), *CoverageError), OutError);
	}
	Out.NormalCovered = Coverage.NumCovered;
	Out.CoveredUnder = Coverage.CoveredUnder;

	// 1b. The hemisphere rays, once per welded point (its first vertex), against the garment and the body itself.
	TArray<FRayTriangle> Triangles;
	TArray<FBox3f> Boxes;
	AddRayTriangles(Garment.Positions, Garment.Indices, nullptr, Triangles, Boxes);
	AddRayTriangles(SourcePositions, TopologyIndices, &Weld, Triangles, Boxes);
	FBoxGrid Grid;
	Grid.Build(Boxes, FMantleGarmentCoverage::RayCellCm);
	TArray<FVector3f> Local;
	HemisphereDirections(FMath::Max(Params.Rays - 1, 0), Local);

	TArray<int32> PointVertex;
	PointVertex.Init(INDEX_NONE, NumPoints);
	for (int32 Vertex = 0; Vertex < NB; ++Vertex)
	{
		if (PointVertex[Weld[Vertex]] == INDEX_NONE)
		{
			PointVertex[Weld[Vertex]] = Vertex;
		}
	}
	TArray<uint8> PointEnclosed;
	PointEnclosed.SetNumZeroed(NumPoints);
	TArray<int32> PointRays;
	PointRays.SetNumZeroed(NumPoints);
	const TConstArrayView<FRayTriangle> TriangleView(Triangles);
	ParallelFor(NumPoints, [&](const int32 Point)
	{
		const int32 Vertex = PointVertex[Point];
		if (!Coverage.Covered[Vertex])
		{
			return;
		}
		if (Coverage.Under[Vertex])
		{
			// Outside the garment that passes under it: removing it shows the garment.
			PointEnclosed[Point] = 1;
			return;
		}
		const FVector3f Normal = Normals[Vertex];
		FVector3f Tangent;
		FVector3f Bitangent;
		Normal.FindBestAxisVectors(Tangent, Bitangent);
		const FVector3f From = SourcePositions[Vertex] + Normal * Params.RayStartCm;
		int32 Escaped = 0;
		int32 Cast = 0;
		for (const FVector3f& L : Local)
		{
			const FVector3f Dir = (Tangent * L.X + Bitangent * L.Y + Normal * L.Z).GetSafeNormal();
			++Cast;
			// The vertex's own triangles never block it (the ray starts just over them).
			const bool bBlocked = SegmentBlocked(Grid, TriangleView, From, Dir, Params.RayReachCm,
				[Point](const FRayTriangle& Triangle) { return !Triangle.Uses(Point); });
			Escaped += bBlocked ? 0 : 1;
			if (Escaped > Params.EscapeSlack)
			{
				break;
			}
		}
		PointRays[Point] = Cast;
		PointEnclosed[Point] = Escaped <= Params.EscapeSlack ? 1 : 0;
	});
	for (int32 Point = 0; Point < NumPoints; ++Point)
	{
		Out.RaysCast += PointRays[Point];
	}
	for (int32 Vertex = 0; Vertex < NB; ++Vertex)
	{
		Out.Enclosed[Vertex] = PointEnclosed[Weld[Vertex]] != 0;
	}
	Out.NumEnclosed = Out.Enclosed.CountSetBits();

	// 2. The border: the surface distance to the nearest skin that can be seen.
	SurfaceDepthMm(SourcePositions, TopologyIndices, Out.Enclosed, Out.DepthMm);
	for (int32 Vertex = 0; Vertex < NB; ++Vertex)
	{
		Out.Hidden[Vertex] = Out.Hides(Vertex, Params.BorderCm);
	}
	Out.NumHidden = Out.Hidden.CountSetBits();
	Out.Ms = NowMs() - T0;
	return true;
}

void FMantleSkinMask::SurfaceDepthMm(TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Indices, const TBitArray<>& Inner, TArray<uint8>& OutDepthMm)
{
	using namespace MantleGarmentGeometry;
	const int32 N = Positions.Num();
	OutDepthMm.Init(0, N);
	if (N == 0 || Inner.Num() != N || !IndicesInRange(Indices, N))
	{
		return;
	}
	TArray<int32> Weld;
	const int32 NumPoints = WeldByPosition(Positions, Weld);
	TArray<FVector3f> PointPosition;
	PointPosition.SetNumUninitialized(NumPoints);
	TBitArray<> Source(false, NumPoints);
	for (int32 Vertex = 0; Vertex < N; ++Vertex)
	{
		PointPosition[Weld[Vertex]] = Positions[Vertex];
		Source[Weld[Vertex]] = Source[Weld[Vertex]] || !Inner[Vertex];
	}
	FPointNeighbours Neighbours;
	PointNeighbours(Indices, Weld, NumPoints, Neighbours);

	// A Dijkstra over the surface from every visible point that carries each point's nearest visible point (its seed)
	// and keys on the straight distance to it: an edge path would read up to 1.4 times too deep across a grid's
	// diagonals and thin the border. Stopped past the farthest depth a byte holds.
	const float FarCm = FarDepthMm / 10.0f;
	TArray<float> Distance;
	Distance.Init(TNumericLimits<float>::Max(), NumPoints);
	TArray<int32> Seed;
	Seed.Init(INDEX_NONE, NumPoints);
	struct FQueued
	{
		float Distance;
		int32 Point;
	};
	const auto Nearer = [](const FQueued& L, const FQueued& R) { return L.Distance < R.Distance || (L.Distance == R.Distance && L.Point < R.Point); };
	TArray<FQueued> Heap;
	for (TConstSetBitIterator<> It(Source); It; ++It)
	{
		Distance[It.GetIndex()] = 0.0f;
		Seed[It.GetIndex()] = It.GetIndex();
		Heap.HeapPush({ 0.0f, It.GetIndex() }, Nearer);
	}
	while (Heap.Num() > 0)
	{
		FQueued Top;
		Heap.HeapPop(Top, Nearer, EAllowShrinking::No);
		if (Top.Distance > Distance[Top.Point] || Top.Distance >= FarCm)
		{
			continue;
		}
		const FVector3f& From = PointPosition[Seed[Top.Point]];
		for (const int32 Other : Neighbours[Top.Point])
		{
			const float Straight = FVector3f::Dist(From, PointPosition[Other]);
			if (Straight < Distance[Other])
			{
				Distance[Other] = Straight;
				Seed[Other] = Seed[Top.Point];
				Heap.HeapPush({ Straight, Other }, Nearer);
			}
		}
	}
	for (int32 Vertex = 0; Vertex < N; ++Vertex)
	{
		const float Cm = Distance[Weld[Vertex]];
		OutDepthMm[Vertex] = Cm >= FarCm ? FarDepthMm : static_cast<uint8>(FMath::FloorToInt32(Cm * 10.0f));
	}
}

FString FMantleSkinMask::ToString() const
{
	return FString::Printf(TEXT("enclosed %d of %d (normal-covered %d, under the skin %d), hidden %d at %.1f cm, %lld rays, %.2f ms, rule %u"),
		NumEnclosed, Num(), NormalCovered, CoveredUnder, NumHidden, BorderCm, RaysCast, Ms, RuleVersion);
}

// ---------------------------------------------------------------------------------------------------------------
// FMantleSeeThroughAudit
// ---------------------------------------------------------------------------------------------------------------

bool FMantleSeeThroughAudit::Run(const FMantleSkinnedMeshLOD& Body, TConstArrayView<uint32> KeptIndices, TConstArrayView<FMantleGarmentSurface> Garments,
	FMantleSeeThroughAudit& Out, const int32 Directions, FString* OutError)
{
	using namespace MantleGarmentGeometry;
	using namespace MantleSkinMaskPrivate;
	const double T0 = NowMs();
	Out = FMantleSeeThroughAudit();
	const int32 NB = Body.NumVertices();
	if (NB == 0 || Directions < 1 || !IndicesInRange(Body.Indices, NB) || !IndicesInRange(KeptIndices, NB))
	{
		return RefuseMask(TEXT("see-through audit"), FString::Printf(TEXT("bad inputs (body verts %d, indices %d, kept indices %d, directions %d)"),
			NB, Body.Indices.Num(), KeptIndices.Num(), Directions), OutError);
	}

	// The dropped triangles: the body's, less the kept ones (as corner triples; a repeated triangle counts per copy).
	TMap<FIntVector, int32> Kept;
	for (int32 Index = 0; Index + 2 < KeptIndices.Num(); Index += 3)
	{
		++Kept.FindOrAdd(FIntVector(static_cast<int32>(KeptIndices[Index]), static_cast<int32>(KeptIndices[Index + 1]), static_cast<int32>(KeptIndices[Index + 2])));
	}
	TArray<int32> Dropped;
	for (int32 Triangle = 0; Triangle < Body.NumTriangles(); ++Triangle)
	{
		const FIntVector Key(static_cast<int32>(Body.Indices[Triangle * 3]), static_cast<int32>(Body.Indices[Triangle * 3 + 1]), static_cast<int32>(Body.Indices[Triangle * 3 + 2]));
		int32* Count = Kept.Find(Key);
		if (Count && *Count > 0)
		{
			--*Count;
		}
		else
		{
			Dropped.Add(Triangle);
		}
	}
	Out.DroppedTriangles = Dropped.Num();

	// What blocks a ray: every garment triangle and every kept body triangle. Body ones carry a non-INDEX_NONE id.
	TArray<FRayTriangle> Triangles;
	TArray<FBox3f> Boxes;
	for (int32 GarmentIndex = 0; GarmentIndex < Garments.Num(); ++GarmentIndex)
	{
		const FMantleGarmentSurface& Garment = Garments[GarmentIndex];
		if (!IndicesInRange(Garment.Indices, Garment.Positions.Num()))
		{
			return RefuseMask(TEXT("see-through audit"), FString::Printf(TEXT("garment %d: %d indices out of range for %d verts"),
				GarmentIndex, Garment.Indices.Num(), Garment.Positions.Num()), OutError);
		}
		AddRayTriangles(Garment.Positions, Garment.Indices, nullptr, Triangles, Boxes);
	}
	TArray<int32> BodyIds;
	BodyIds.SetNumUninitialized(NB);
	for (int32 Vertex = 0; Vertex < NB; ++Vertex)
	{
		BodyIds[Vertex] = Vertex;
	}
	AddRayTriangles(Body.Positions, KeptIndices, &BodyIds, Triangles, Boxes);
	if (Dropped.Num() == 0)
	{
		Out.Ms = NowMs() - T0;
		return true;
	}
	FBox3f Bounds(ForceInit);
	for (const FBox3f& Box : Boxes)
	{
		Bounds += Box;
	}
	for (const FVector3f& Position : Body.Positions)
	{
		Bounds += Position;
	}
	const float Reach = Bounds.GetSize().Size() + 1.0f;
	FBoxGrid Grid;
	Grid.Build(Boxes, FMantleGarmentCoverage::RayCellCm);
	TArray<FVector3f> Cameras;
	SphereDirections(Directions, Cameras);

	// From each dropped triangle's centre back toward each camera: blocked by anything, or a garment just under it.
	const TConstArrayView<FRayTriangle> TriangleView(Triangles);
	const float MinSine = FMath::Sin(FMath::DegreesToRadians(MinGrazingDeg));
	TArray<int32> HoleRays;
	HoleRays.SetNumZeroed(Dropped.Num());
	TArray<int32> Asked;
	Asked.SetNumZeroed(Dropped.Num());
	TArray<int32> FirstHole;
	FirstHole.Init(INDEX_NONE, Dropped.Num());
	ParallelFor(Dropped.Num(), [&](const int32 Slot)
	{
		const int32 Triangle = Dropped[Slot];
		const FVector3f& A = Body.Positions[Body.Indices[Triangle * 3]];
		const FVector3f& B = Body.Positions[Body.Indices[Triangle * 3 + 1]];
		const FVector3f& C = Body.Positions[Body.Indices[Triangle * 3 + 2]];
		const FVector3f Centre = (A + B + C) / 3.0f;
		const FVector3f Plane = FVector3f::CrossProduct(B - A, C - A).GetSafeNormal();
		for (int32 Camera = 0; Camera < Cameras.Num(); ++Camera)
		{
			const FVector3f ToCamera = Cameras[Camera];
			if (FMath::Abs(FVector3f::DotProduct(Plane, ToCamera)) < MinSine)
			{
				continue;
			}
			++Asked[Slot];
			if (SegmentBlocked(Grid, TriangleView, Centre, ToCamera, Reach, [](const FRayTriangle&) { return true; }))
			{
				continue;
			}
			if (SegmentBlocked(Grid, TriangleView, Centre, -ToCamera, UnderReachCm, [](const FRayTriangle& Other) { return !Other.IsBody(); }))
			{
				continue;
			}
			++HoleRays[Slot];
			if (FirstHole[Slot] == INDEX_NONE)
			{
				FirstHole[Slot] = Camera;
			}
		}
	});
	for (int32 Slot = 0; Slot < Dropped.Num(); ++Slot)
	{
		Out.RaysCast += Asked[Slot];
		Out.HoleRays += HoleRays[Slot];
		if (HoleRays[Slot] > 0)
		{
			++Out.HoleTriangles;
			if (Out.Examples.Num() < MaxExamples)
			{
				Out.Examples.Add(TPair<int32, FVector3f>(Dropped[Slot], Cameras[FirstHole[Slot]]));
			}
		}
	}
	Out.Ms = NowMs() - T0;
	return true;
}

FString FMantleSeeThroughAudit::ToString() const
{
	FString Text = FString::Printf(TEXT("dropped %d tris, %d rays: %d see-through (%d tris), %.2f ms"), DroppedTriangles, RaysCast, HoleRays, HoleTriangles, Ms);
	for (const TPair<int32, FVector3f>& Example : Examples)
	{
		Text += FString::Printf(TEXT("; tri %d from (%.2f, %.2f, %.2f)"), Example.Key, Example.Value.X, Example.Value.Y, Example.Value.Z);
	}
	return Text;
}
