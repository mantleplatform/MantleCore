// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHairHatFit.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleHairHatFitPrivate
{
	/** A hit closer than this to the origin is no hit (a ray never meets what it starts on). */
	constexpr double MinHitCm = 1.e-4;
	/** Triangles are clipped to the part of space this far in front of a cube face before they are projected on it. */
	constexpr double ClipW = 1.e-5;
	/** A ray that grazes an edge by this much (in barycentrics) still hits, so a ray through a shared edge or corner never slips between two triangles. */
	constexpr double BarycentricTolerance = 1.e-9;
	/** Face-coordinate padding of a triangle's cell range; wider than the tolerance above times any realistic projected size. */
	constexpr double BinPad = 1.e-3;
	/** Triangles flatter than this (twice the area, cm^2) are not tested. */
	constexpr double MinDeterminant = 1.e-10;

	FVector3d ToD(const FVector3f& V) { return FVector3d(V.X, V.Y, V.Z); }

	/** Two-sided ray / triangle test (Moller-Trumbore, in doubles): the distance along D, or -1. */
	double RayTriangle(const FVector3d& O, const FVector3d& D, const FVector3d& A, const FVector3d& B, const FVector3d& C)
	{
		const FVector3d E1 = B - A;
		const FVector3d E2 = C - A;
		const FVector3d P = FVector3d::CrossProduct(D, E2);
		const double Det = FVector3d::DotProduct(E1, P);
		if (FMath::Abs(Det) < MinDeterminant)
		{
			return -1.0;
		}
		const double InvDet = 1.0 / Det;
		const FVector3d T = O - A;
		const double U = FVector3d::DotProduct(T, P) * InvDet;
		if (U < -BarycentricTolerance || U > 1.0 + BarycentricTolerance)
		{
			return -1.0;
		}
		const FVector3d Q = FVector3d::CrossProduct(T, E1);
		const double V = FVector3d::DotProduct(D, Q) * InvDet;
		if (V < -BarycentricTolerance || U + V > 1.0 + BarycentricTolerance)
		{
			return -1.0;
		}
		const double Dist = FVector3d::DotProduct(E2, Q) * InvDet;
		return Dist > MinHitCm ? Dist : -1.0;
	}

	/** The cube face a direction points at (0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z) and where it crosses that face's unit square, both -1..1. False for no direction. */
	bool FaceCoords(const FVector3d& P, int32& Face, double& U, double& V)
	{
		const double AX = FMath::Abs(P.X);
		const double AY = FMath::Abs(P.Y);
		const double AZ = FMath::Abs(P.Z);
		const int32 Axis = (AX >= AY && AX >= AZ) ? 0 : (AY >= AZ ? 1 : 2);
		const double W = P[Axis];
		const double Wa = FMath::Abs(W);
		if (!(Wa > 0.0))
		{
			return false;
		}
		Face = Axis * 2 + (W < 0.0 ? 1 : 0);
		U = P[(Axis + 1) % 3] / Wa;
		V = P[(Axis + 2) % 3] / Wa;
		return true;
	}

	int32 CellOf(const double Coord)
	{
		return FMath::Clamp(FMath::FloorToInt32((Coord + 1.0) * 0.5 * FMantleHairHatGrid::CellsPerFaceSide), 0, FMantleHairHatGrid::CellsPerFaceSide - 1);
	}

	/**
	 * Calls Fn(cell) for every cell a triangle (corners relative to the grid's origin) can touch: per cube face, the triangle
	 * is clipped to the front of that face, projected onto it (a straight-line map, so the result is a polygon) and the cells
	 * of the polygon's padded bounding box are visited.
	 */
	template <typename FuncType>
	void ForEachCell(const FVector3d& A, const FVector3d& B, const FVector3d& C, FuncType&& Fn)
	{
		constexpr int32 N = FMantleHairHatGrid::CellsPerFaceSide;
		const FVector3d In[3] = { A, B, C };
		for (int32 Face = 0; Face < 6; ++Face)
		{
			const int32 Axis = Face >> 1;
			const double Sign = (Face & 1) ? -1.0 : 1.0;
			const int32 Axis1 = (Axis + 1) % 3;
			const int32 Axis2 = (Axis + 2) % 3;

			// Sutherland-Hodgman against W >= ClipW; a triangle gives at most four corners.
			FVector3d Poly[4];
			int32 NumPoly = 0;
			for (int32 I = 0; I < 3; ++I)
			{
				const FVector3d& Cur = In[I];
				const FVector3d& Next = In[(I + 1) % 3];
				const double WCur = Sign * Cur[Axis];
				const double WNext = Sign * Next[Axis];
				const bool bCurIn = WCur >= ClipW;
				const bool bNextIn = WNext >= ClipW;
				if (bCurIn)
				{
					Poly[NumPoly++] = Cur;
				}
				if (bCurIn != bNextIn)
				{
					const double T = (ClipW - WCur) / (WNext - WCur);
					Poly[NumPoly++] = Cur + (Next - Cur) * T;
				}
			}
			if (NumPoly == 0)
			{
				continue;
			}

			double MinU = TNumericLimits<double>::Max(), MaxU = -TNumericLimits<double>::Max();
			double MinV = MinU, MaxV = MaxU;
			for (int32 I = 0; I < NumPoly; ++I)
			{
				const double W = FMath::Max(Sign * Poly[I][Axis], ClipW);
				const double U = Poly[I][Axis1] / W;
				const double V = Poly[I][Axis2] / W;
				MinU = FMath::Min(MinU, U); MaxU = FMath::Max(MaxU, U);
				MinV = FMath::Min(MinV, V); MaxV = FMath::Max(MaxV, V);
			}
			if (MaxU < -1.0 - BinPad || MinU > 1.0 + BinPad || MaxV < -1.0 - BinPad || MinV > 1.0 + BinPad)
			{
				continue;
			}
			const int32 U0 = CellOf(MinU - BinPad), U1 = CellOf(MaxU + BinPad);
			const int32 V0 = CellOf(MinV - BinPad), V1 = CellOf(MaxV + BinPad);
			for (int32 CV = V0; CV <= V1; ++CV)
			{
				for (int32 CU = U0; CU <= U1; ++CU)
				{
					Fn((Face * N + CV) * N + CU);
				}
			}
		}
	}

	bool IsFinite(const FVector3f& V)
	{
		return FMath::IsFinite(V.X) && FMath::IsFinite(V.Y) && FMath::IsFinite(V.Z);
	}

	/** Union-find over vertices (the root of a set is its smallest index). */
	int32 FindRoot(TArray<int32>& Parent, int32 V)
	{
		while (Parent[V] != V)
		{
			Parent[V] = Parent[Parent[V]];
			V = Parent[V];
		}
		return V;
	}

	void Union(TArray<int32>& Parent, const int32 A, const int32 B)
	{
		const int32 RA = FindRoot(Parent, A);
		const int32 RB = FindRoot(Parent, B);
		if (RA != RB)
		{
			// The smaller index is the root, so the result does not depend on the order of calls.
			Parent[FMath::Max(RA, RB)] = FMath::Min(RA, RB);
		}
	}

	bool TriangleIsValid(const TArray<uint32>& Indices, const int32 Triangle, const int32 NumVertices)
	{
		return static_cast<int32>(Indices[Triangle * 3]) < NumVertices
			&& static_cast<int32>(Indices[Triangle * 3 + 1]) < NumVertices
			&& static_cast<int32>(Indices[Triangle * 3 + 2]) < NumVertices;
	}

	/**
	 * Speck clean-up: the hair is split into connected cards (triangles that share a point, points at the same spot count as
	 * shared, as the head fit's weld does). A card with SpeckAreaShare or more of its area dropped loses the rest, unless it holds
	 * more than SpeckMaxCardShare of all the triangles. Adds the extra triangles to Drop; returns how many.
	 */
	int32 DropSpecks(const FMantleSkinnedMeshLOD& Hair, TArray<uint8>& Drop)
	{
		const int32 NumVertices = Hair.Positions.Num();
		const int32 NumTriangles = Hair.Indices.Num() / 3;
		TArray<int32> Parent;
		Parent.SetNumUninitialized(NumVertices);
		TArray<int32> Order;
		Order.Reserve(NumVertices);
		for (int32 V = 0; V < NumVertices; ++V)
		{
			Parent[V] = V;
			if (IsFinite(Hair.Positions[V]))
			{
				Order.Add(V);
			}
		}
		const TArray<FVector3f>& P = Hair.Positions;
		Order.Sort([&P](const int32 A, const int32 B)
		{
			if (P[A].X != P[B].X) return P[A].X < P[B].X;
			if (P[A].Y != P[B].Y) return P[A].Y < P[B].Y;
			if (P[A].Z != P[B].Z) return P[A].Z < P[B].Z;
			return A < B;
		});
		for (int32 I = 1; I < Order.Num(); ++I)
		{
			if (P[Order[I]] == P[Order[I - 1]])
			{
				Union(Parent, Order[I - 1], Order[I]);
			}
		}
		for (int32 T = 0; T < NumTriangles; ++T)
		{
			if (TriangleIsValid(Hair.Indices, T, NumVertices))
			{
				Union(Parent, Hair.Indices[T * 3], Hair.Indices[T * 3 + 1]);
				Union(Parent, Hair.Indices[T * 3], Hair.Indices[T * 3 + 2]);
			}
		}

		TArray<double> TotalArea, DroppedArea;
		TArray<int32> TriangleCount;
		TotalArea.SetNumZeroed(NumVertices);
		DroppedArea.SetNumZeroed(NumVertices);
		TriangleCount.SetNumZeroed(NumVertices);
		TArray<int32> CardOf;
		CardOf.Init(INDEX_NONE, NumTriangles);
		for (int32 T = 0; T < NumTriangles; ++T)
		{
			if (!TriangleIsValid(Hair.Indices, T, NumVertices))
			{
				continue;
			}
			const FVector3f& A = P[Hair.Indices[T * 3]];
			const FVector3f& B = P[Hair.Indices[T * 3 + 1]];
			const FVector3f& C = P[Hair.Indices[T * 3 + 2]];
			const double Area = 0.5 * FVector3d::CrossProduct(ToD(B) - ToD(A), ToD(C) - ToD(A)).Size();
			const int32 Card = FindRoot(Parent, Hair.Indices[T * 3]);
			CardOf[T] = Card;
			TotalArea[Card] += FMath::IsFinite(Area) ? Area : 0.0;
			DroppedArea[Card] += (Drop[T] && FMath::IsFinite(Area)) ? Area : 0.0;
			++TriangleCount[Card];
		}

		int32 Added = 0;
		for (int32 T = 0; T < NumTriangles; ++T)
		{
			const int32 Card = CardOf[T];
			if (Card == INDEX_NONE || Drop[T] || !(TotalArea[Card] > 0.0))
			{
				continue;
			}
			const bool bAlmostGone = DroppedArea[Card] >= FMantleHairHatFit::SpeckAreaShare * TotalArea[Card];
			const bool bTooBigACard = TriangleCount[Card] > FMantleHairHatFit::SpeckMaxCardShare * NumTriangles;
			if (bAlmostGone && !bTooBigACard)
			{
				Drop[T] = 1;
				++Added;
			}
		}
		return Added;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// FMantleHairHatGrid
// ---------------------------------------------------------------------------------------------------------------------

void FMantleHairHatGrid::Build(const FVector3f& InOrigin, TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Indices)
{
	using namespace MantleHairHatFitPrivate;
	Origin = InOrigin;
	Corners.Reset();
	const int32 NumCells = 6 * CellsPerFaceSide * CellsPerFaceSide;
	CellStart.Reset();
	CellStart.SetNumZeroed(NumCells + 1);
	CellTriangles.Reset();

	const int32 NumVertices = Positions.Num();
	const FVector3d O = ToD(Origin);
	for (int32 T = 0; T + 2 < Indices.Num(); T += 3)
	{
		const uint32 IA = Indices[T], IB = Indices[T + 1], IC = Indices[T + 2];
		if (static_cast<int32>(IA) >= NumVertices || static_cast<int32>(IB) >= NumVertices || static_cast<int32>(IC) >= NumVertices)
		{
			continue;
		}
		if (!IsFinite(Positions[IA]) || !IsFinite(Positions[IB]) || !IsFinite(Positions[IC]))
		{
			continue;
		}
		Corners.Add(Positions[IA]);
		Corners.Add(Positions[IB]);
		Corners.Add(Positions[IC]);
	}

	const int32 NumTris = Corners.Num() / 3;
	// Pass 1 counts the triangles per cell, pass 2 fills them in (triangle order, so the grid is deterministic).
	for (int32 T = 0; T < NumTris; ++T)
	{
		ForEachCell(ToD(Corners[T * 3]) - O, ToD(Corners[T * 3 + 1]) - O, ToD(Corners[T * 3 + 2]) - O,
			[this](const int32 Cell) { ++CellStart[Cell + 1]; });
	}
	for (int32 Cell = 0; Cell < NumCells; ++Cell)
	{
		CellStart[Cell + 1] += CellStart[Cell];
	}
	CellTriangles.SetNumUninitialized(CellStart[NumCells]);
	TArray<int32> Cursor(CellStart.GetData(), NumCells);
	for (int32 T = 0; T < NumTris; ++T)
	{
		ForEachCell(ToD(Corners[T * 3]) - O, ToD(Corners[T * 3 + 1]) - O, ToD(Corners[T * 3 + 2]) - O,
			[this, &Cursor, T](const int32 Cell) { CellTriangles[Cursor[Cell]++] = T; });
	}
}

float FMantleHairHatGrid::FirstHit(const FVector3f& Direction) const
{
	using namespace MantleHairHatFitPrivate;
	if (CellTriangles.IsEmpty())
	{
		return -1.0f;
	}
	const FVector3d D = ToD(Direction);
	int32 Face = 0;
	double U = 0.0, V = 0.0;
	if (!FaceCoords(D, Face, U, V))
	{
		return -1.0f;
	}
	const int32 Cell = (Face * CellsPerFaceSide + CellOf(V)) * CellsPerFaceSide + CellOf(U);
	const FVector3d O = ToD(Origin);
	double Best = -1.0;
	for (int32 K = CellStart[Cell]; K < CellStart[Cell + 1]; ++K)
	{
		const int32 T = CellTriangles[K];
		const double Dist = RayTriangle(O, D, ToD(Corners[T * 3]), ToD(Corners[T * 3 + 1]), ToD(Corners[T * 3 + 2]));
		if (Dist > 0.0 && (Best < 0.0 || Dist < Best))
		{
			Best = Dist;
		}
	}
	return static_cast<float>(Best);
}

float FMantleHairHatGrid::FirstHitBruteForce(const FVector3f& Origin, const FVector3f& Direction,
	TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Indices)
{
	using namespace MantleHairHatFitPrivate;
	const int32 NumVertices = Positions.Num();
	const FVector3d O = ToD(Origin);
	const FVector3d D = ToD(Direction);
	double Best = -1.0;
	for (int32 T = 0; T + 2 < Indices.Num(); T += 3)
	{
		const uint32 IA = Indices[T], IB = Indices[T + 1], IC = Indices[T + 2];
		if (static_cast<int32>(IA) >= NumVertices || static_cast<int32>(IB) >= NumVertices || static_cast<int32>(IC) >= NumVertices)
		{
			continue;
		}
		if (!IsFinite(Positions[IA]) || !IsFinite(Positions[IB]) || !IsFinite(Positions[IC]))
		{
			continue;
		}
		const double Dist = RayTriangle(O, D, ToD(Positions[IA]), ToD(Positions[IB]), ToD(Positions[IC]));
		if (Dist > 0.0 && (Best < 0.0 || Dist < Best))
		{
			Best = Dist;
		}
	}
	return static_cast<float>(Best);
}

// ---------------------------------------------------------------------------------------------------------------------
// FMantleHairHatFit
// ---------------------------------------------------------------------------------------------------------------------

FMantleHairHatFit::FVertexVerdict FMantleHairHatFit::Judge(const float PointCm, const float HatCm, const float ScalpCm)
{
	FVertexVerdict Verdict;
	// No hit: below the brim, through a hole, an open top. Free.
	if (!(HatCm > 0.0f))
	{
		return Verdict;
	}
	// More than the margin inside the hat's surface: tucked already.
	const float Depth = HatCm - PointCm;
	if (Depth > PokeMarginCm)
	{
		return Verdict;
	}
	Verdict.bPokes = true;
	Verdict.BeyondCm = -Depth;
	float Target = HatCm - PullInCm;
	if (ScalpCm > 0.0f)
	{
		const float Floor = ScalpCm + ScalpClearanceCm;
		Verdict.bRoom = HatCm - Floor >= MinUnderHatCm;
		// With room the point stops above the scalp; without, the hat wins and the head hides what goes under the skin.
		if (Verdict.bRoom)
		{
			Target = FMath::Max(Target, Floor);
		}
	}
	// A pull-in never pushes a point outward.
	Verdict.TargetCm = FMath::Min(Target, PointCm);
	return Verdict;
}

bool FMantleHairHatFit::CornerIsLost(const FVertexVerdict& Verdict, const bool bFacialHair)
{
	if (!Verdict.bPokes)
	{
		return false;
	}
	// Facial hair is never pulled in, so any poking corner is lost.
	if (bFacialHair)
	{
		return true;
	}
	// Hair that could be pulled in under the hat is kept; hair far out through the hat, or with no room under it, is lost.
	return Verdict.BeyondCm > DropBeyondCm || !Verdict.bRoom;
}

void FMantleHairHatFit::Fit(const FMantleSkinnedMeshLOD& Hair, const TConstArrayView<FVector3f> HatPositions, const TConstArrayView<uint32> HatIndices,
	const TConstArrayView<FVector3f> ScalpPositions, const TConstArrayView<uint32> ScalpIndices,
	const FMantleHairHatFitParams& Params, FMantleHairHatFitResult& Out)
{
	using namespace MantleHairHatFitPrivate;
	const double StartSeconds = FPlatformTime::Seconds();
	Out = FMantleHairHatFitResult();
	Out.Positions = Hair.Positions;

	const int32 NumVertices = Hair.Positions.Num();
	const int32 NumTriangles = Hair.Indices.Num() / 3;
	auto Finish = [&Out, StartSeconds]()
	{
		Out.TrianglesDropped = Out.DroppedTriangles.Num();
		Out.Ms = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	};

	if (Params.Rule == EMantleHairHatRule::LeaveAlone)
	{
		Finish();
		return;
	}
	if (Params.Rule == EMantleHairHatRule::HideAll && !Params.bFacialHair)
	{
		Out.DroppedTriangles.Reserve(NumTriangles);
		for (int32 T = 0; T < NumTriangles; ++T)
		{
			Out.DroppedTriangles.Add(T);
		}
		Finish();
		return;
	}
	if (NumVertices == 0 || NumTriangles == 0 || HatIndices.Num() < 3)
	{
		Finish();
		return;
	}

	const bool bPullIn = !Params.bFacialHair;
	const FVector3f Centre = Params.HeadCentre;

	FMantleHairHatGrid HatGrid;
	if (!Params.bBruteForce)
	{
		HatGrid.Build(Centre, HatPositions, HatIndices);
	}
	FMantleHairHatGrid ScalpGrid;
	bool bScalpGridBuilt = false;
	const auto HatHit = [&](const FVector3f& Direction)
	{
		return Params.bBruteForce ? FMantleHairHatGrid::FirstHitBruteForce(Centre, Direction, HatPositions, HatIndices) : HatGrid.FirstHit(Direction);
	};
	const auto ScalpHit = [&](const FVector3f& Direction)
	{
		if (Params.bBruteForce)
		{
			return FMantleHairHatGrid::FirstHitBruteForce(Centre, Direction, ScalpPositions, ScalpIndices);
		}
		if (!bScalpGridBuilt)
		{
			// Built on the first poking point only: a hat the hair clears never pays for the scalp.
			ScalpGrid.Build(Centre, ScalpPositions, ScalpIndices);
			bScalpGridBuilt = true;
		}
		return ScalpGrid.FirstHit(Direction);
	};

	// 1. Each point's verdict, and the pull-in.
	TArray<FVertexVerdict> Verdicts;
	Verdicts.SetNum(NumVertices);
	TArray<uint8> Moved;
	Moved.SetNumZeroed(NumVertices);
	for (int32 V = 0; V < NumVertices; ++V)
	{
		const FVector3f Rel = Hair.Positions[V] - Centre;
		const float Length = Rel.Size();
		if (!(Length > MinRayLengthCm) || !FMath::IsFinite(Length))
		{
			continue;
		}
		const FVector3f Direction = Rel / Length;
		const float HatCm = HatHit(Direction);
		FVertexVerdict Verdict = Judge(Length, HatCm, -1.0f);
		if (Verdict.bPokes && bPullIn)
		{
			Verdict = Judge(Length, HatCm, ScalpHit(Direction));
			if (Length - Verdict.TargetCm > 1.e-4f)
			{
				Out.Positions[V] = Centre + Direction * Verdict.TargetCm;
				Moved[V] = 1;
			}
		}
		Verdicts[V] = Verdict;
	}

	// 2. A triangle goes when all three corners are lost.
	TArray<uint8> Drop;
	Drop.SetNumZeroed(NumTriangles);
	for (int32 T = 0; T < NumTriangles; ++T)
	{
		if (!TriangleIsValid(Hair.Indices, T, NumVertices))
		{
			continue;
		}
		Drop[T] = CornerIsLost(Verdicts[Hair.Indices[T * 3]], Params.bFacialHair)
			&& CornerIsLost(Verdicts[Hair.Indices[T * 3 + 1]], Params.bFacialHair)
			&& CornerIsLost(Verdicts[Hair.Indices[T * 3 + 2]], Params.bFacialHair);
	}
	if (Params.bDropSpecks)
	{
		Out.TrianglesDroppedAsSpecks = DropSpecks(Hair, Drop);
	}
	// A moved point counts when a triangle that stays uses it (the points of dropped triangles are put back below).
	TArray<uint8> Counted;
	Counted.SetNumZeroed(NumVertices);
	for (int32 T = 0; T < NumTriangles; ++T)
	{
		if (Drop[T])
		{
			Out.DroppedTriangles.Add(T);
		}
		else if (TriangleIsValid(Hair.Indices, T, NumVertices))
		{
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const int32 V = Hair.Indices[T * 3 + Corner];
				if (Moved[V] && !Counted[V])
				{
					Counted[V] = 1;
					++Out.PointsPulled;
				}
			}
		}
	}
	// What no kept triangle uses goes back to where it was.
	for (int32 V = 0; V < NumVertices; ++V)
	{
		if (Moved[V] && !Counted[V])
		{
			Out.Positions[V] = Hair.Positions[V];
		}
	}
	Finish();
}

void FMantleHairHatFit::ApplyDrops(FMantleSkinnedMeshLOD& LOD, const TArray<int32>& DroppedTriangles)
{
	const int32 NumTriangles = LOD.Indices.Num() / 3;
	TArray<uint8> Drop;
	Drop.SetNumZeroed(NumTriangles);
	for (const int32 T : DroppedTriangles)
	{
		if (T >= 0 && T < NumTriangles)
		{
			Drop[T] = 1;
		}
	}

	// KeptBefore[T] is how many triangles before T stay; a section's new range is read off it.
	TArray<int32> KeptBefore;
	KeptBefore.SetNumUninitialized(NumTriangles + 1);
	TArray<uint32> NewIndices;
	NewIndices.Reserve(LOD.Indices.Num());
	int32 Kept = 0;
	for (int32 T = 0; T < NumTriangles; ++T)
	{
		KeptBefore[T] = Kept;
		if (!Drop[T])
		{
			NewIndices.Append(&LOD.Indices[T * 3], 3);
			++Kept;
		}
	}
	KeptBefore[NumTriangles] = Kept;

	for (FMantleSkinnedMeshSection& Section : LOD.Sections)
	{
		const int32 First = FMath::Clamp(Section.FirstIndex / 3, 0, NumTriangles);
		const int32 End = FMath::Clamp((Section.FirstIndex + Section.IndexCount) / 3, First, NumTriangles);
		Section.FirstIndex = KeptBefore[First] * 3;
		Section.IndexCount = (KeptBefore[End] - KeptBefore[First]) * 3;
	}
	LOD.Indices = MoveTemp(NewIndices);
}

FVector3f FMantleHairHatFit::HeadCentreFromSkin(const TConstArrayView<FVector3f> Positions, const TConstArrayView<uint32> Indices)
{
	using namespace MantleHairHatFitPrivate;
	TArray<uint8> Used;
	Used.SetNumZeroed(Positions.Num());
	float Top = -TNumericLimits<float>::Max();
	bool bAny = false;
	for (const uint32 Index : Indices)
	{
		if (static_cast<int32>(Index) < Positions.Num() && !Used[Index] && IsFinite(Positions[Index]))
		{
			Used[Index] = 1;
			Top = FMath::Max(Top, Positions[Index].Z);
			bAny = true;
		}
	}
	if (!bAny)
	{
		return FVector3f::ZeroVector;
	}
	double SumX = 0.0, SumY = 0.0;
	int32 Count = 0;
	for (int32 V = 0; V < Positions.Num(); ++V)
	{
		if (Used[V] && Positions[V].Z >= Top - HeadCentreBandCm)
		{
			SumX += Positions[V].X;
			SumY += Positions[V].Y;
			++Count;
		}
	}
	return FVector3f(static_cast<float>(SumX / Count), static_cast<float>(SumY / Count), Top - HeadCentreBelowTopCm);
}
