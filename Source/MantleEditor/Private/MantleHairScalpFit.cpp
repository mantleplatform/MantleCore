// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHairScalpFit.h"

#include "Algo/Sort.h"
#include "Containers/Map.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleHairScalpFit, Log, All);

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleHairScalpFitPrivate
{
	TArray<FVector> ToDouble(TConstArrayView<FVector3f> In)
	{
		TArray<FVector> Out;
		Out.Reserve(In.Num());
		for (const FVector3f& P : In) Out.Add(FVector(P.X, P.Y, P.Z));
		return Out;
	}

	TArray<FVector3f> ToFloat(const TArray<FVector>& In)
	{
		TArray<FVector3f> Out;
		Out.Reserve(In.Num());
		for (const FVector& P : In) Out.Add(FVector3f(static_cast<float>(P.X), static_cast<float>(P.Y), static_cast<float>(P.Z)));
		return Out;
	}

	bool CheckMesh(const FMantleScalpFit::FMesh& Mesh, const TCHAR* What, FString& Error)
	{
		if (Mesh.Positions.IsEmpty() || Mesh.Indices.IsEmpty() || Mesh.Indices.Num() % 3 != 0)
		{
			Error = FString::Printf(TEXT("the %s mesh has no triangles"), What);
			return false;
		}
		for (const uint32 I : Mesh.Indices)
		{
			if (I >= static_cast<uint32>(Mesh.Positions.Num()))
			{
				Error = FString::Printf(TEXT("the %s mesh has a triangle that names a missing vertex"), What);
				return false;
			}
		}
		return true;
	}

	TArray<uint8> UsedMask(int32 NumVertices, TConstArrayView<uint32> Indices)
	{
		TArray<uint8> Used;
		Used.Init(0, NumVertices);
		for (const uint32 I : Indices) Used[static_cast<int32>(I)] = 1;
		return Used;
	}

	double MedianOf(TArray<double> Values)
	{
		if (Values.IsEmpty()) return 0.0;
		Values.Sort();
		const int32 N = Values.Num();
		return N % 2 ? Values[N / 2] : 0.5 * (Values[N / 2 - 1] + Values[N / 2]);
	}

	/** Vertices at one position (to 0.01 mm) are one vertex for connectivity; render data splits them at UV seams. */
	struct FWelded
	{
		TArray<int32> Inv;
		TArray<FVector> Pos;
	};

	FWelded Weld(const TArray<FVector>& V)
	{
		FWelded W;
		W.Inv.SetNumUninitialized(V.Num());
		TMap<FIntVector, int32> Seen;
		for (int32 I = 0; I < V.Num(); ++I)
		{
			const FIntVector Key(FMath::RoundToInt(V[I].X * 1000.0), FMath::RoundToInt(V[I].Y * 1000.0), FMath::RoundToInt(V[I].Z * 1000.0));
			if (const int32* Found = Seen.Find(Key)) { W.Inv[I] = *Found; W.Pos[*Found] = V[I]; continue; }
			W.Inv[I] = W.Pos.Add(V[I]);
			Seen.Add(Key, W.Inv[I]);
		}
		return W;
	}

	int32 FindRoot(TArray<int32>& Parent, int32 X)
	{
		while (Parent[X] != X) { Parent[X] = Parent[Parent[X]]; X = Parent[X]; }
		return X;
	}

	/** Closest point on triangle ABC to P (Ericson), with the barycentric weights of B and C. */
	FVector ClosestPointOnTriangleWithBary(const FVector& P, const FVector& A, const FVector& B, const FVector& C, double& OutV, double& OutW)
	{
		const FVector AB = B - A, AC = C - A, AP = P - A;
		const double D1 = AB | AP, D2 = AC | AP;
		if (D1 <= 0.0 && D2 <= 0.0) { OutV = 0.0; OutW = 0.0; return A; }
		const FVector BP = P - B;
		const double D3 = AB | BP, D4 = AC | BP;
		if (D3 >= 0.0 && D4 <= D3) { OutV = 1.0; OutW = 0.0; return B; }
		const double VC = D1 * D4 - D3 * D2;
		if (VC <= 0.0 && D1 >= 0.0 && D3 <= 0.0)
		{
			const double T = D1 / (D1 - D3);
			OutV = T; OutW = 0.0;
			return A + AB * T;
		}
		const FVector CP = P - C;
		const double D5 = AB | CP, D6 = AC | CP;
		if (D6 >= 0.0 && D5 <= D6) { OutV = 0.0; OutW = 1.0; return C; }
		const double VB = D5 * D2 - D1 * D6;
		if (VB <= 0.0 && D2 >= 0.0 && D6 <= 0.0)
		{
			const double T = D2 / (D2 - D6);
			OutV = 0.0; OutW = T;
			return A + AC * T;
		}
		const double VA = D3 * D6 - D5 * D4;
		if (VA <= 0.0 && (D4 - D3) >= 0.0 && (D5 - D6) >= 0.0)
		{
			const double T = (D4 - D3) / ((D4 - D3) + (D5 - D6));
			OutV = 1.0 - T; OutW = T;
			return B + (C - B) * T;
		}
		const double Sum = VA + VB + VC;
		if (FMath::Abs(Sum) < 1.e-18) { OutV = 0.0; OutW = 0.0; return A; }
		const double Denom = 1.0 / Sum;
		OutV = VB * Denom; OutW = VC * Denom;
		return A + AB * OutV + AC * OutW;
	}

	/** Dominant direction of a point set about its centroid: the column of the repeatedly squared covariance with the most weight. */
	FVector DominantAxis(const TArray<FVector>& Centred)
	{
		double M[3][3] = {};
		for (const FVector& P : Centred)
			for (int32 R = 0; R < 3; ++R) for (int32 C = 0; C < 3; ++C) M[R][C] += P[R] * P[C];
		for (int32 Iter = 0; Iter < 8; ++Iter)
		{
			double S[3][3] = {};
			double MaxAbs = 0.0;
			for (int32 R = 0; R < 3; ++R) for (int32 C = 0; C < 3; ++C)
			{
				for (int32 K = 0; K < 3; ++K) S[R][C] += M[R][K] * M[K][C];
				MaxAbs = FMath::Max(MaxAbs, FMath::Abs(S[R][C]));
			}
			if (MaxAbs < 1.e-300) return FVector::XAxisVector;
			for (int32 R = 0; R < 3; ++R) for (int32 C = 0; C < 3; ++C) M[R][C] = S[R][C] / MaxAbs;
		}
		FVector Best = FVector::ZeroVector;
		for (int32 C = 0; C < 3; ++C)
		{
			const FVector Col(M[0][C], M[1][C], M[2][C]);
			if (Col.SizeSquared() > Best.SizeSquared()) Best = Col;
		}
		return Best.SizeSquared() > 1.e-24 ? Best.GetSafeNormal() : FVector::XAxisVector;
	}

	double PercentWithin(const TArray<double>& Distances, double Tolerance)
	{
		if (Distances.IsEmpty()) return 0.0;
		int32 Count = 0;
		for (const double D : Distances) if (FMath::Abs(D) <= Tolerance) ++Count;
		return 100.0 * Count / Distances.Num();
	}

	struct FDijkstraItem
	{
		double Dist = 0.0;
		int32 Node = 0;
	};
}

// ---------------------------------------------------------------- k-d tree

void FMantleScalpKdTree::Build(const TArray<FVector>& InPoints)
{
	Points = InPoints;
	Order.SetNumUninitialized(Points.Num());
	for (int32 I = 0; I < Order.Num(); ++I) Order[I] = I;
	BuildRange(0, Order.Num(), 0);
}

void FMantleScalpKdTree::BuildRange(int32 Lo, int32 Hi, int32 Axis)
{
	if (Hi - Lo <= 1) return;
	TArrayView<int32> Range = MakeArrayView(Order.GetData() + Lo, Hi - Lo);
	Algo::Sort(Range, [this, Axis](int32 A, int32 B) { return Points[A][Axis] < Points[B][Axis]; });
	const int32 Mid = (Lo + Hi) / 2;
	BuildRange(Lo, Mid, (Axis + 1) % 3);
	BuildRange(Mid + 1, Hi, (Axis + 1) % 3);
}

void FMantleScalpKdTree::NearestRange(int32 Lo, int32 Hi, int32 Axis, const FVector& P, int32 K, TArray<TPair<double, int32>>& Best) const
{
	if (Lo >= Hi) return;
	const int32 Mid = (Lo + Hi) / 2;
	const int32 Id = Order[Mid];
	const FVector& Q = Points[Id];
	const double D2 = FVector::DistSquared(P, Q);
	if (Best.Num() < K || D2 < Best.Last().Key)
	{
		int32 At = Best.Num();
		while (At > 0 && Best[At - 1].Key > D2) --At;
		Best.Insert(TPair<double, int32>(D2, Id), At);
		if (Best.Num() > K) Best.Pop();
	}
	const double Diff = P[Axis] - Q[Axis];
	const int32 Next = (Axis + 1) % 3;
	const bool bLeftFirst = Diff < 0.0;
	if (bLeftFirst) NearestRange(Lo, Mid, Next, P, K, Best); else NearestRange(Mid + 1, Hi, Next, P, K, Best);
	if (Best.Num() < K || Diff * Diff < Best.Last().Key)
	{
		if (bLeftFirst) NearestRange(Mid + 1, Hi, Next, P, K, Best); else NearestRange(Lo, Mid, Next, P, K, Best);
	}
}

void FMantleScalpKdTree::Nearest(const FVector& P, int32 K, TArray<int32>& OutIds, TArray<double>& OutDist) const
{
	OutIds.Reset();
	OutDist.Reset();
	if (K <= 0 || Points.IsEmpty()) return;
	TArray<TPair<double, int32>> Best;
	Best.Reserve(K + 1);
	NearestRange(0, Order.Num(), 0, P, K, Best);
	for (const TPair<double, int32>& B : Best) { OutIds.Add(B.Value); OutDist.Add(FMath::Sqrt(B.Key)); }
}

void FMantleScalpKdTree::RadiusRange(int32 Lo, int32 Hi, int32 Axis, const FVector& P, double R2, TArray<int32>& OutIds) const
{
	if (Lo >= Hi) return;
	const int32 Mid = (Lo + Hi) / 2;
	const int32 Id = Order[Mid];
	const FVector& Q = Points[Id];
	if (FVector::DistSquared(P, Q) <= R2) OutIds.Add(Id);
	const double Diff = P[Axis] - Q[Axis];
	const int32 Next = (Axis + 1) % 3;
	if (Diff <= 0.0 || Diff * Diff <= R2) RadiusRange(Lo, Mid, Next, P, R2, OutIds);
	if (Diff >= 0.0 || Diff * Diff <= R2) RadiusRange(Mid + 1, Hi, Next, P, R2, OutIds);
}

void FMantleScalpKdTree::InRadius(const FVector& P, double Radius, TArray<int32>& OutIds) const
{
	OutIds.Reset();
	RadiusRange(0, Order.Num(), 0, P, Radius * Radius, OutIds);
}

// ---------------------------------------------------------------- head surface

bool FMantleScalpHeadSurface::Build(TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Indices, FString& OutError)
{
	using namespace MantleHairScalpFitPrivate;
	*this = FMantleScalpHeadSurface();
	if (Positions.IsEmpty() || Indices.IsEmpty() || Indices.Num() % 3 != 0)
	{
		OutError = TEXT("the head mesh has no triangles");
		return false;
	}
	for (const uint32 I : Indices)
	{
		if (I >= static_cast<uint32>(Positions.Num())) { OutError = TEXT("the head mesh has a triangle that names a missing vertex"); return false; }
	}
	Vertices = ToDouble(Positions);
	const int32 NumTris = Indices.Num() / 3;
	Triangles.SetNumUninitialized(NumTris);
	TArray<FVector> FaceNormals;
	FaceNormals.SetNumUninitialized(NumTris);
	TriangleCentroids.SetNumUninitialized(NumTris);
	for (int32 T = 0; T < NumTris; ++T)
	{
		Triangles[T] = FIntVector(static_cast<int32>(Indices[T * 3]), static_cast<int32>(Indices[T * 3 + 1]), static_cast<int32>(Indices[T * 3 + 2]));
		const FVector& A = Vertices[Triangles[T].X];
		const FVector& B = Vertices[Triangles[T].Y];
		const FVector& C = Vertices[Triangles[T].Z];
		const FVector N = (B - A) ^ (C - A);
		FaceNormals[T] = N / (N.Size() + 1.e-12);
		TriangleCentroids[T] = (A + B + C) / 3.0;
	}

	// Outward is the side the top quarter of the head faces: flip every triangle if those face inward.
	TArray<double> Heights;
	Heights.SetNumUninitialized(NumTris);
	for (int32 T = 0; T < NumTris; ++T) Heights[T] = TriangleCentroids[T].Z;
	TArray<double> Sorted = Heights;
	Sorted.Sort();
	const double Rank = 0.75 * (NumTris - 1);
	const int32 Lo = FMath::FloorToInt(Rank);
	const double Threshold = Sorted[Lo] + (Sorted[FMath::Min(Lo + 1, NumTris - 1)] - Sorted[Lo]) * (Rank - Lo);
	FVector Mean = FVector::ZeroVector;
	int32 NumUp = 0;
	for (int32 T = 0; T < NumTris; ++T) if (Heights[T] > Threshold) { Mean += TriangleCentroids[T]; ++NumUp; }
	if (NumUp > 0)
	{
		Mean /= NumUp;
		double Facing = 0.0;
		for (int32 T = 0; T < NumTris; ++T) if (Heights[T] > Threshold) Facing += (TriangleCentroids[T] - Mean) | FaceNormals[T];
		if (Facing < 0.0)
		{
			for (int32 T = 0; T < NumTris; ++T) { Swap(Triangles[T].Y, Triangles[T].Z); FaceNormals[T] = -FaceNormals[T]; }
		}
	}

	VertexNormals.Init(FVector::ZeroVector, Vertices.Num());
	for (int32 T = 0; T < NumTris; ++T)
	{
		VertexNormals[Triangles[T].X] += FaceNormals[T];
		VertexNormals[Triangles[T].Y] += FaceNormals[T];
		VertexNormals[Triangles[T].Z] += FaceNormals[T];
	}
	for (FVector& N : VertexNormals) N = N / (N.Size() + 1.e-12);

	VertexKd.Build(Vertices);
	TriOrder.SetNumUninitialized(NumTris);
	for (int32 T = 0; T < NumTris; ++T) TriOrder[T] = T;
	Nodes.Reset();
	BuildNode(0, NumTris);
	return true;
}

int32 FMantleScalpHeadSurface::BuildNode(int32 First, int32 Count)
{
	const int32 Index = Nodes.AddDefaulted();
	FBox Box(ForceInit);
	FBox CentroidBox(ForceInit);
	for (int32 I = First; I < First + Count; ++I)
	{
		const FIntVector& T = Triangles[TriOrder[I]];
		Box += Vertices[T.X]; Box += Vertices[T.Y]; Box += Vertices[T.Z];
		CentroidBox += TriangleCentroids[TriOrder[I]];
	}
	Nodes[Index].Box = Box;
	if (Count <= 4)
	{
		Nodes[Index].First = First;
		Nodes[Index].Count = Count;
		return Index;
	}
	const FVector Extent = CentroidBox.GetSize();
	const int32 Axis = Extent.X >= Extent.Y && Extent.X >= Extent.Z ? 0 : (Extent.Y >= Extent.Z ? 1 : 2);
	TArrayView<int32> Range = MakeArrayView(TriOrder.GetData() + First, Count);
	Algo::Sort(Range, [this, Axis](int32 A, int32 B) { return TriangleCentroids[A][Axis] < TriangleCentroids[B][Axis]; });
	const int32 Half = Count / 2;
	const int32 Left = BuildNode(First, Half);
	const int32 Right = BuildNode(First + Half, Count - Half);
	Nodes[Index].Left = Left;
	Nodes[Index].Right = Right;
	return Index;
}

void FMantleScalpHeadSurface::QueryNode(int32 NodeIndex, const FVector& P, double& BestD2, int32& BestTri, double& BestV, double& BestW, FVector& BestPoint) const
{
	using namespace MantleHairScalpFitPrivate;
	const FNode& Node = Nodes[NodeIndex];
	if (Node.Box.ComputeSquaredDistanceToPoint(P) > BestD2) return;
	if (Node.Left == INDEX_NONE)
	{
		for (int32 I = Node.First; I < Node.First + Node.Count; ++I)
		{
			const int32 T = TriOrder[I];
			double V = 0.0, W = 0.0;
			const FVector Q = ClosestPointOnTriangleWithBary(P, Vertices[Triangles[T].X], Vertices[Triangles[T].Y], Vertices[Triangles[T].Z], V, W);
			const double D2 = FVector::DistSquared(P, Q);
			if (D2 < BestD2) { BestD2 = D2; BestTri = T; BestV = V; BestW = W; BestPoint = Q; }
		}
		return;
	}
	const double DL = Nodes[Node.Left].Box.ComputeSquaredDistanceToPoint(P);
	const double DR = Nodes[Node.Right].Box.ComputeSquaredDistanceToPoint(P);
	const int32 First = DL <= DR ? Node.Left : Node.Right;
	const int32 Second = DL <= DR ? Node.Right : Node.Left;
	QueryNode(First, P, BestD2, BestTri, BestV, BestW, BestPoint);
	QueryNode(Second, P, BestD2, BestTri, BestV, BestW, BestPoint);
}

FMantleScalpHeadSurface::FHit FMantleScalpHeadSurface::Query(const FVector& P) const
{
	FHit Hit;
	if (Nodes.IsEmpty()) return Hit;
	double BestD2 = TNumericLimits<double>::Max();
	int32 BestTri = INDEX_NONE;
	double V = 0.0, W = 0.0;
	FVector Point = FVector::ZeroVector;
	QueryNode(0, P, BestD2, BestTri, V, W, Point);
	if (BestTri == INDEX_NONE) return Hit;
	const FIntVector& T = Triangles[BestTri];
	FVector N = VertexNormals[T.X] * (1.0 - V - W) + VertexNormals[T.Y] * V + VertexNormals[T.Z] * W;
	N = N / (N.Size() + 1.e-12);
	const double Distance = FMath::Sqrt(BestD2);
	Hit.Point = Point;
	Hit.Normal = N;
	Hit.SignedDistance = ((P - Point) | N) < 0.0 ? -Distance : Distance;
	return Hit;
}

// ---------------------------------------------------------------- fit

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleHairScalpFitPrivate
{
	/** One card: its vertices, each one's position along the card's long axis, and that axis's range. */
	struct FCardAxis
	{
		TArray<int32> Ids;
		TArray<double> Along;
		double TMin = 0.0;
		double TMax = 0.0;
	};

	/** The cards of a triangle list (connected pieces, vertices at one position are one), each with its long axis. Cards under four vertices are skipped. */
	TArray<FCardAxis> FindCardAxes(const TArray<FVector>& Positions, TConstArrayView<uint32> Indices)
	{
		TArray<FCardAxis> Out;
		if (Indices.Num() % 3 != 0) return Out;
		const FWelded Welded = Weld(Positions);
		TArray<int32> Parent;
		Parent.SetNumUninitialized(Welded.Pos.Num());
		for (int32 I = 0; I < Parent.Num(); ++I) Parent[I] = I;
		TArray<uint8> Used = UsedMask(Positions.Num(), Indices);
		for (int32 T = 0; T + 2 < Indices.Num(); T += 3)
		{
			const int32 A = FindRoot(Parent, Welded.Inv[Indices[T]]);
			const int32 B = FindRoot(Parent, Welded.Inv[Indices[T + 1]]);
			const int32 C = FindRoot(Parent, Welded.Inv[Indices[T + 2]]);
			Parent[B] = A;
			Parent[FindRoot(Parent, C)] = FindRoot(Parent, A);
		}
		TMap<int32, TArray<int32>> Cards;
		for (int32 V = 0; V < Positions.Num(); ++V)
		{
			if (Used[V]) Cards.FindOrAdd(FindRoot(Parent, Welded.Inv[V])).Add(V);
		}
		TArray<int32> Labels;
		Cards.GetKeys(Labels);
		Labels.Sort();
		for (const int32 Label : Labels)
		{
			FCardAxis Card;
			Card.Ids = Cards[Label];
			if (Card.Ids.Num() < 4) continue;
			FVector Centre = FVector::ZeroVector;
			for (const int32 V : Card.Ids) Centre += Positions[V];
			Centre /= Card.Ids.Num();
			TArray<FVector> Centred;
			Centred.Reserve(Card.Ids.Num());
			for (const int32 V : Card.Ids) Centred.Add(Positions[V] - Centre);
			const FVector Axis = DominantAxis(Centred);
			Card.TMin = TNumericLimits<double>::Max();
			Card.TMax = TNumericLimits<double>::Lowest();
			Card.Along.Reserve(Card.Ids.Num());
			for (const FVector& P : Centred) { const double T = P | Axis; Card.Along.Add(T); Card.TMin = FMath::Min(Card.TMin, T); Card.TMax = FMath::Max(Card.TMax, T); }
			Out.Add(MoveTemp(Card));
		}
		return Out;
	}
}

TArray<int32> FMantleScalpFit::FindCardRoots(const TArray<FVector>& Positions, TConstArrayView<uint32> Indices, const TArray<double>& Distances)
{
	using namespace MantleHairScalpFitPrivate;
	TArray<int32> Roots;
	for (const FCardAxis& Card : FindCardAxes(Positions, Indices))
	{
		const TArray<int32>& Ids = Card.Ids;
		const double Length = Card.TMax - Card.TMin;
		double SumA = 0.0, SumB = 0.0;
		int32 NumA = 0, NumB = 0;
		for (int32 I = 0; I < Ids.Num(); ++I)
		{
			if (Card.Along[I] <= Card.TMin + CardEndFraction * Length) { SumA += FMath::Abs(Distances[Ids[I]]); ++NumA; }
			if (Card.Along[I] >= Card.TMax - CardEndFraction * Length) { SumB += FMath::Abs(Distances[Ids[I]]); ++NumB; }
		}
		// The root end is the end nearer the head (a tie goes to the high-t end).
		const bool bUseLow = NumA > 0 && NumB > 0 && SumA / NumA < SumB / NumB;
		int32 Pivot = INDEX_NONE;
		double PivotDist = TNumericLimits<double>::Max();
		for (int32 I = 0; I < Ids.Num(); ++I)
		{
			const bool bInEnd = bUseLow ? Card.Along[I] <= Card.TMin + CardEndFraction * Length : Card.Along[I] >= Card.TMax - CardEndFraction * Length;
			if (!bInEnd) continue;
			const double D = FMath::Abs(Distances[Ids[I]]);
			if (D < PivotDist) { PivotDist = D; Pivot = Ids[I]; }
		}
		if (Pivot != INDEX_NONE) Roots.Add(Pivot);
	}
	return Roots;
}

TArray<FVector3f> FMantleScalpFit::FindCardEnds(TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Indices)
{
	using namespace MantleHairScalpFitPrivate;
	TArray<FVector3f> Ends;
	for (const uint32 I : Indices) if (I >= static_cast<uint32>(Positions.Num())) return Ends;
	const TArray<FVector> Double = ToDouble(Positions);
	for (const FCardAxis& Card : FindCardAxes(Double, Indices))
	{
		int32 Low = 0, High = 0;
		for (int32 I = 1; I < Card.Ids.Num(); ++I)
		{
			if (Card.Along[I] < Card.Along[Low]) Low = I;
			if (Card.Along[I] > Card.Along[High]) High = I;
		}
		Ends.Add(Positions[Card.Ids[Low]]);
		Ends.Add(Positions[Card.Ids[High]]);
	}
	return Ends;
}

void FMantleScalpFit::SmoothField(const FMantleScalpHeadSurface& Head, const TArray<double>& Value, const TArray<uint8>& Mask,
	double Sigma, const TArray<uint8>& Wanted, TArray<double>& Out)
{
	const int32 N = Head.NumVertices();
	Out.Init(0.0, N);
	if (Value.Num() != N || Mask.Num() != N) return;
	TArray<FVector> MaskedPoints;
	TArray<int32> MaskedIds;
	for (int32 I = 0; I < N; ++I)
	{
		if (Mask[I]) { MaskedPoints.Add(Head.Vertex(I)); MaskedIds.Add(I); }
	}
	FMantleScalpKdTree Tree;
	Tree.Build(MaskedPoints);
	const double Radius = 3.0 * Sigma;
	TArray<int32> Near;
	for (int32 I = 0; I < N; ++I)
	{
		if (!Wanted.IsEmpty() && !Wanted[I]) continue;
		double Num = 0.0, Den = 0.0;
		if (Tree.Num() > 0)
		{
			Tree.InRadius(Head.Vertex(I), Radius, Near);
			for (const int32 Id : Near)
			{
				const double D = FVector::Dist(Head.Vertex(I), Tree.Point(Id)) / Sigma;
				const double W = FMath::Exp(-0.5 * D * D);
				Num += W * Value[MaskedIds[Id]];
				Den += W;
			}
		}
		// The vertex itself counts once more on top of the distance matrix, which already holds it.
		if (Mask[I]) { Num += Value[I]; Den += 1.0; }
		Out[I] = Den > 1.e-6 ? Num / Den : 0.0;
	}
}

int32 FMantleScalpFit::LiftSunk(const FMantleScalpHeadSurface& Head, TArray<FVector>& Positions, const TArray<uint8>& Used,
	double Clearance, double& OutMaxLift)
{
	int32 Lifted = 0;
	OutMaxLift = 0.0;
	for (int32 I = 0; I < Positions.Num(); ++I)
	{
		if (!Used[I]) continue;
		const FMantleScalpHeadSurface::FHit Hit = Head.Query(Positions[I]);
		if (Hit.SignedDistance >= SinkCm) continue;
		const double Lift = Clearance - Hit.SignedDistance;
		Positions[I] += Hit.Normal * Lift;
		OutMaxLift = FMath::Max(OutMaxLift, Lift);
		++Lifted;
	}
	return Lifted;
}

bool FMantleScalpFit::WrapCapEdge(const FMantleScalpHeadSurface& Head, const TArray<FVector>& Positions, TConstArrayView<uint32> Indices,
	TArray<FVector>& OutPositions, FMetrics& Metrics, TArray<int32>* OutEdgeVertices)
{
	using namespace MantleHairScalpFitPrivate;
	OutPositions.Reset();
	if (OutEdgeVertices) OutEdgeVertices->Reset();
	if (!Head.IsValid() || Indices.Num() % 3 != 0) return false;
	const FWelded Welded = Weld(Positions);
	const int32 N = Welded.Pos.Num();

	// Open boundary: edges used by one triangle.
	TMap<uint64, int32> EdgeUse;
	TArray<TArray<int32>> Adjacent;
	Adjacent.SetNum(N);
	for (int32 T = 0; T + 2 < Indices.Num(); T += 3)
	{
		const int32 Tri[3] = { Welded.Inv[Indices[T]], Welded.Inv[Indices[T + 1]], Welded.Inv[Indices[T + 2]] };
		for (int32 E = 0; E < 3; ++E)
		{
			const int32 A = Tri[E], B = Tri[(E + 1) % 3];
			if (A == B) continue;
			const uint64 Key = (static_cast<uint64>(FMath::Min(A, B)) << 32) | static_cast<uint64>(FMath::Max(A, B));
			int32& Count = EdgeUse.FindOrAdd(Key);
			if (++Count == 1) { Adjacent[A].Add(B); Adjacent[B].Add(A); }
		}
	}
	TArray<uint8> bBoundary;
	bBoundary.Init(0, N);
	for (const TPair<uint64, int32>& Edge : EdgeUse)
	{
		if (Edge.Value != 1) continue;
		bBoundary[static_cast<int32>(Edge.Key >> 32)] = 1;
		bBoundary[static_cast<int32>(Edge.Key & 0xffffffffu)] = 1;
	}

	// Surface distance from the open edge, out to the wrap length: multi-source Dijkstra remembering the nearest edge vertex.
	TArray<double> Dist;
	TArray<int32> Source;
	Dist.Init(TNumericLimits<double>::Max(), N);
	Source.Init(INDEX_NONE, N);
	TArray<FDijkstraItem> Heap;
	const auto Before = [](const FDijkstraItem& A, const FDijkstraItem& B) { return A.Dist < B.Dist; };
	for (int32 I = 0; I < N; ++I)
	{
		if (!bBoundary[I]) continue;
		Dist[I] = 0.0;
		Source[I] = I;
		Heap.HeapPush(FDijkstraItem{0.0, I}, Before);
	}
	while (!Heap.IsEmpty())
	{
		FDijkstraItem Top;
		Heap.HeapPop(Top, Before);
		if (Top.Dist > Dist[Top.Node]) continue;
		for (const int32 Next : Adjacent[Top.Node])
		{
			const double Candidate = Top.Dist + FVector::Dist(Welded.Pos[Top.Node], Welded.Pos[Next]);
			if (Candidate > CapWrapCm || Candidate >= Dist[Next]) continue;
			Dist[Next] = Candidate;
			Source[Next] = Source[Top.Node];
			Heap.HeapPush(FDijkstraItem{Candidate, Next}, Before);
		}
	}

	// Edge displacement: onto the closest scalp point + 1 mm, at most 3 cm per axis.
	TArray<FVector> Delta;
	Delta.Init(FVector::ZeroVector, N);
	TArray<uint8> bClamped;
	bClamped.Init(0, N);
	for (int32 I = 0; I < N; ++I)
	{
		if (!bBoundary[I]) continue;
		const FMantleScalpHeadSurface::FHit Hit = Head.Query(Welded.Pos[I]);
		const FVector Target = Hit.Point + Hit.Normal * CapEdgeOffsetCm;
		const FVector Raw = Target - Welded.Pos[I];
		Delta[I] = FVector(FMath::Clamp(Raw.X, -CapWrapCm, CapWrapCm), FMath::Clamp(Raw.Y, -CapWrapCm, CapWrapCm), FMath::Clamp(Raw.Z, -CapWrapCm, CapWrapCm));
		bClamped[I] = !Delta[I].Equals(Raw, 1.e-12);
	}

	TArray<FVector> Wrapped;
	Wrapped.SetNumUninitialized(N);
	TArray<uint8> bLow;
	bLow.Init(0, N);
	for (int32 I = 0; I < N; ++I)
	{
		FVector P = Welded.Pos[I];
		if (Source[I] != INDEX_NONE && Dist[I] <= CapWrapCm)
		{
			const double Weight = 0.5 * (1.0 + FMath::Cos(UE_DOUBLE_PI * FMath::Min(Dist[I] / CapWrapCm, 1.0)));
			P += Delta[Source[I]] * Weight;
		}
		// Keep the cap above the skin where the wrap left it sunk (the forehead line, the ear cut-out).
		const FMantleScalpHeadSurface::FHit Hit = Head.Query(P);
		if (Hit.SignedDistance < CapLiftBelowCm)
		{
			P = Hit.Point + Hit.Normal * CapEdgeOffsetCm;
			bLow[I] = 1;
		}
		Wrapped[I] = P;
	}

	OutPositions.SetNumUninitialized(Positions.Num());
	Metrics.CapEdgeVertices = 0;
	Metrics.CapEdgeClamped = 0;
	Metrics.CapLifted = 0;
	for (int32 V = 0; V < Positions.Num(); ++V)
	{
		const int32 W = Welded.Inv[V];
		OutPositions[V] = Wrapped[W];
		if (bBoundary[W])
		{
			++Metrics.CapEdgeVertices;
			if (bClamped[W]) ++Metrics.CapEdgeClamped;
			if (OutEdgeVertices) OutEdgeVertices->Add(V);
		}
		if (bLow[W]) ++Metrics.CapLifted;
	}
	return true;
}

bool FMantleScalpFit::Run(const FMantleScalpHeadSurface& Head, const FMesh& Cards, const FMesh* Cap, FResult& Out, FString& Error)
{
	using namespace MantleHairScalpFitPrivate;
	Out = FResult();
	if (!Head.IsValid()) { Error = TEXT("there is no head to fit the hair to"); return false; }
	if (!CheckMesh(Cards, TEXT("hair cards"), Error)) return false;
	const bool bCap = Cap != nullptr;
	if (bCap && !CheckMesh(*Cap, TEXT("hair cap"), Error)) return false;

	FMetrics& M = Out.Metrics;
	const TArray<FVector> CardPos = ToDouble(Cards.Positions);
	const TArray<uint8> CardUsed = UsedMask(CardPos.Num(), Cards.Indices);
	M.CardVertices = CardPos.Num();
	// #51: hair lower than HeadDepthCm below the head's top lies over the neck and shoulders. The head mesh's collar is
	// not the wearer's body, so that hair is neither pushed nor lifted out of it (long hair was shoved out up to 20 cm).
	double HeadTop = -TNumericLimits<double>::Max();
	for (int32 I = 0; I < Head.NumVertices(); ++I) HeadTop = FMath::Max(HeadTop, Head.Vertex(I).Z);
	TArray<uint8> CardOnHead = CardUsed;
	for (int32 I = 0; I < CardPos.Num(); ++I) if (CardPos[I].Z < HeadTop - HeadDepthCm) CardOnHead[I] = 0;

	TArray<double> CardDist;
	CardDist.Init(0.0, CardPos.Num());
	for (int32 I = 0; I < CardPos.Num(); ++I)
	{
		if (CardUsed[I]) CardDist[I] = Head.Query(CardPos[I]).SignedDistance;
	}
	const TArray<int32> Roots = FindCardRoots(CardPos, Cards.Indices, CardDist);
	M.CardCount = Roots.Num();
	for (int32 I = 0; I < CardPos.Num(); ++I) if (CardOnHead[I] && CardDist[I] < SinkCm) ++M.CardInsideBefore;
	{
		TArray<double> RootDist;
		for (const int32 R : Roots) RootDist.Add(CardDist[R]);
		M.RootsWithin3mmBefore = PercentWithin(RootDist, 0.3);
		M.RootsWithin5mmBefore = PercentWithin(RootDist, 0.5);
	}

	// Cap: the lower-edge wrap does not depend on the field, so it runs first and names the open edge the field pulls on.
	TArray<FVector> CapPos, CapWrapped;
	TArray<uint8> CapUsed;
	TArray<double> CapDist;
	TArray<int32> CapEdge;
	if (bCap)
	{
		CapPos = ToDouble(Cap->Positions);
		CapUsed = UsedMask(CapPos.Num(), Cap->Indices);
		M.CapVertices = CapPos.Num();
		if (!WrapCapEdge(Head, CapPos, Cap->Indices, CapWrapped, M, &CapEdge)) { Error = TEXT("the hair cap could not be wrapped onto the head"); return false; }
		CapDist.Init(0.0, CapPos.Num());
		for (int32 I = 0; I < CapPos.Num(); ++I)
		{
			if (CapUsed[I]) CapDist[I] = Head.Query(CapPos[I]).SignedDistance;
		}
	}

	// The scalp offset field: push where hair is closer than its clearance, pull in only floating roots / cap edge.
	const int32 NumHead = Head.NumVertices();
	TArray<double> Push, Pull;
	Push.Init(0.0, NumHead);
	Pull.Init(0.0, NumHead);
	TArray<uint8> Mask;
	Mask.Init(0, NumHead);
	TArray<int32> NearIds;
	TArray<double> NearDist;
	const auto NearestVertex = [&](const FVector& P)
	{
		Head.VertexTree().Nearest(P, 1, NearIds, NearDist);
		return NearIds[0];
	};
	const double CardClearance = bCap ? CardOverCapClearanceCm : ClearanceCm;
	for (int32 I = 0; I < CardPos.Num(); ++I)
	{
		if (!CardUsed[I]) continue;
		const int32 Nn = NearestVertex(CardPos[I]);
		if (CardOnHead[I]) Push[Nn] = FMath::Max(Push[Nn], FMath::Max(0.0, CardClearance - CardDist[I]));
		Mask[Nn] = 1;
	}
	for (int32 I = 0; I < CapPos.Num(); ++I)
	{
		if (!CapUsed[I]) continue;
		const int32 Nn = NearestVertex(CapPos[I]);
		Push[Nn] = FMath::Max(Push[Nn], FMath::Max(0.0, ClearanceCm - CapDist[I]));
		Mask[Nn] = 1;
	}
	const auto AddPull = [&](const FVector& P, double Distance)
	{
		if (Distance <= FloatToleranceCm) return;
		const int32 Nn = NearestVertex(P);
		Pull[Nn] = FMath::Min(Pull[Nn], -(Distance - PullToCm));
	};
	if (bCap) { for (const int32 E : CapEdge) AddPull(CapPos[E], CapDist[E]); }
	else { for (const int32 R : Roots) AddPull(CardPos[R], CardDist[R]); }
	TArray<double> Target;
	Target.SetNumUninitialized(NumHead);
	for (int32 I = 0; I < NumHead; ++I) Target[I] = Push[I] > 0.0 ? Push[I] : Pull[I];

	// Each card vertex reads the field at its 4 nearest head vertices, weighted 1 / (distance + 2 mm).
	const int32 K = FMath::Min(4, NumHead);
	TArray<int32> Neighbours;
	TArray<double> Weights;
	Neighbours.Init(0, CardPos.Num() * K);
	Weights.Init(0.0, CardPos.Num() * K);
	TArray<uint8> Wanted;
	Wanted.Init(0, NumHead);
	for (int32 I = 0; I < CardPos.Num(); ++I)
	{
		if (!CardUsed[I]) continue;
		Head.VertexTree().Nearest(CardPos[I], K, NearIds, NearDist);
		double Sum = 0.0;
		for (int32 J = 0; J < NearIds.Num(); ++J)
		{
			Neighbours[I * K + J] = NearIds[J];
			Weights[I * K + J] = 1.0 / (NearDist[J] + 0.2);
			Sum += Weights[I * K + J];
			Wanted[NearIds[J]] = 1;
		}
		for (int32 J = 0; J < NearIds.Num(); ++J) Weights[I * K + J] /= Sum;
	}
	TArray<double> Field;
	SmoothField(Head, Target, Mask, SigmaCm, Wanted, Field);
	for (int32 I = 0; I < NumHead; ++I)
	{
		if (!Wanted[I]) continue;
		M.MaxPushCm = FMath::Max(M.MaxPushCm, Field[I]);
		M.MaxPullCm = FMath::Min(M.MaxPullCm, Field[I]);
	}

	TArray<FVector> Moved = CardPos;
	TArray<double> Shift;
	for (int32 I = 0; I < CardPos.Num(); ++I)
	{
		if (!CardUsed[I]) continue;
		double F = 0.0;
		FVector Normal = FVector::ZeroVector;
		for (int32 J = 0; J < K; ++J)
		{
			F += Field[Neighbours[I * K + J]] * Weights[I * K + J];
			Normal += Head.VertexNormal(Neighbours[I * K + J]) * Weights[I * K + J];
		}
		Normal = Normal / (Normal.Size() + 1.e-12);
		Moved[I] = CardPos[I] + Normal * F;
		Shift.Add(FMath::Abs(F));
	}
	M.FieldShiftMedianCm = MedianOf(Shift);
	for (const double S : Shift) M.FieldShiftMaxCm = FMath::Max(M.FieldShiftMaxCm, S);

	// Last resort: whatever the field left more than 1 mm inside the skin.
	M.CardLifted = LiftSunk(Head, Moved, CardOnHead, CardClearance, M.CardLiftMaxCm);
	{
		TArray<double> RootDist;
		for (const int32 R : Roots) RootDist.Add(Head.Query(Moved[R]).SignedDistance);
		M.RootsWithin3mmAfter = PercentWithin(RootDist, 0.3);
		M.RootsWithin5mmAfter = PercentWithin(RootDist, 0.5);
		for (double& D : RootDist) D = FMath::Abs(D);
		M.RootMedianMmAfter = 10.0 * MedianOf(RootDist);
	}
	for (int32 I = 0; I < Moved.Num(); ++I)
	{
		if (CardOnHead[I] && Head.Query(Moved[I]).SignedDistance < SinkCm) ++M.CardInsideAfter;
	}
	Out.CardPositions = ToFloat(Moved);
	if (bCap)
	{
		for (int32 I = 0; I < CapWrapped.Num(); ++I)
		{
			if (CapUsed[I] && Head.Query(CapWrapped[I]).SignedDistance < SinkCm) ++M.CapInsideAfter;
		}
		Out.CapPositions = ToFloat(CapWrapped);
	}

	int32 OnHeadVertices = 0;
	for (const uint8 bOnHead : CardOnHead) OnHeadVertices += bOnHead;
	if (M.CardLifted > CheckFitLiftShare * OnHeadVertices || M.CardLiftMaxCm > CheckFitLiftCm)
	{
		Out.bCheckFit = true;
		Out.CheckFitNote = TEXT("Part of this hair had to be moved a long way to stop it sinking into the head. Please check that it sits right on the head.");
	}
	const FString CapText = bCap
		? FString::Printf(TEXT(" and a cap (%d vertices, %d on the open edge, %d at the 3 cm cap, %d lifted off the skin, %d still inside)"),
			M.CapVertices, M.CapEdgeVertices, M.CapEdgeClamped, M.CapLifted, M.CapInsideAfter)
		: FString();
	UE_LOG(LogMantleHairScalpFit, Log,
		TEXT("Scalp fit: %d cards (%d vertices)%s. Field push max %.2f cm, pull max %.2f cm, shift median %.2f cm, max %.2f cm. Lifted %d card vertices (largest %.2f cm; inside before %d, after %d). Roots within 3 mm %.1f%% -> %.1f%%, within 5 mm %.1f%% -> %.1f%%, median %.1f mm.%s"),
		M.CardCount, M.CardVertices, *CapText,
		M.MaxPushCm, M.MaxPullCm, M.FieldShiftMedianCm, M.FieldShiftMaxCm, M.CardLifted, M.CardLiftMaxCm, M.CardInsideBefore, M.CardInsideAfter,
		M.RootsWithin3mmBefore, M.RootsWithin3mmAfter, M.RootsWithin5mmBefore, M.RootsWithin5mmAfter, M.RootMedianMmAfter,
		Out.bCheckFit ? TEXT(" The preview should ask the creator to check the fit.") : TEXT(""));
	return true;
}
