// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHairPieceFit.h"

#include "MantleHairScalpFit.h"

#include "Algo/Sort.h"
#include "Async/ParallelFor.h"
#include "Math/RandomStream.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleHairPieceFitPrivate
{
	/** numpy.percentile with linear interpolation, on an ascending array. P is 0 to 1. */
	double PieceFitPercentile(const TArray<double>& Sorted, double P)
	{
		if (Sorted.IsEmpty()) return 0.0;
		const double Rank = P * (Sorted.Num() - 1);
		const int32 Lo = FMath::Clamp(FMath::FloorToInt(Rank), 0, Sorted.Num() - 1);
		const int32 Hi = FMath::Min(Lo + 1, Sorted.Num() - 1);
		return Sorted[Lo] + (Sorted[Hi] - Sorted[Lo]) * (Rank - Lo);
	}

	double PieceFitRound(double Value, double Scale)
	{
		return FMath::RoundToDouble(Value * Scale) / Scale;
	}

	/** What one grid candidate scored. */
	struct FPieceFitScore
	{
		double Outside = 0.0;
		double DeepInside = 0.0;
		double Seated = 0.0;
		double Coverage = 0.0;
		/** Seated times coverage, to 3 decimals so near-equal candidates fall to the closest roots. */
		double Rank = 0.0;
		double RootMedian = 0.0;

		bool KeepsOut() const { return DeepInside <= FMantleHairPieceFit::MaxDeepInsideFraction; }
	};

	/** True when A is the better candidate (see FMantleHairPieceFit's step 3). */
	bool PieceFitBetter(const FPieceFitScore& A, const FPieceFitScore& B)
	{
		if (A.KeepsOut() != B.KeepsOut()) return A.KeepsOut();
		if (!A.KeepsOut()) return A.DeepInside < B.DeepInside;
		if (A.Rank != B.Rank) return A.Rank > B.Rank;
		return A.RootMedian < B.RootMedian;
	}
}

// ---------------------------------------------------------------- head samples

bool FMantleHairHeadSamples::Build(TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Indices, FString& OutError)
{
	using namespace MantleHairPieceFitPrivate;
	*this = FMantleHairHeadSamples();
	if (Positions.IsEmpty() || Indices.IsEmpty() || Indices.Num() % 3 != 0)
	{
		OutError = TEXT("the head mesh has no triangles");
		return false;
	}
	for (const uint32 I : Indices)
	{
		if (I >= static_cast<uint32>(Positions.Num())) { OutError = TEXT("the head mesh has a triangle that names a missing vertex"); return false; }
	}
	const int32 NumTris = Indices.Num() / 3;
	const int32 PerTriangle = FMath::Clamp(250000 / NumTris, 3, MaxSamplesPerTriangle);
	FRandomStream Stream(7);
	Points.Reserve(NumTris * (PerTriangle + 1));
	Normals.Reserve(NumTris * (PerTriangle + 1));
	Top = -TNumericLimits<double>::Max();
	for (int32 T = 0; T < NumTris; ++T)
	{
		const FVector3f& A3 = Positions[static_cast<int32>(Indices[T * 3])];
		const FVector3f& B3 = Positions[static_cast<int32>(Indices[T * 3 + 1])];
		const FVector3f& C3 = Positions[static_cast<int32>(Indices[T * 3 + 2])];
		const FVector A(A3.X, A3.Y, A3.Z), B(B3.X, B3.Y, B3.Z), C(C3.X, C3.Y, C3.Z);
		Top = FMath::Max(Top, FMath::Max3(A.Z, B.Z, C.Z));
		FVector N = (B - A) ^ (C - A);
		const double Length = N.Size();
		if (Length < 1.e-12) continue;
		N /= Length;
		Points.Add((A + B + C) / 3.0);
		Normals.Add(N);
		for (int32 K = 0; K < PerTriangle; ++K)
		{
			double U = Stream.FRand(), V = Stream.FRand();
			if (U + V > 1.0) { U = 1.0 - U; V = 1.0 - V; }
			Points.Add(A * (1.0 - U - V) + B * U + C * V);
			Normals.Add(N);
		}
	}
	if (Points.IsEmpty())
	{
		OutError = TEXT("the head mesh has only degenerate triangles");
		return false;
	}

	// Outward is the side the top quarter of the head faces: flip every normal if those face inward.
	TArray<double> Heights;
	Heights.SetNumUninitialized(Points.Num());
	for (int32 I = 0; I < Points.Num(); ++I) Heights[I] = Points[I].Z;
	TArray<double> Sorted = Heights;
	Sorted.Sort();
	const double Above60 = PieceFitPercentile(Sorted, 0.60);
	const double Above75 = PieceFitPercentile(Sorted, 0.75);
	FVector Centre = FVector::ZeroVector;
	int32 NumCentre = 0;
	for (int32 I = 0; I < Points.Num(); ++I) if (Heights[I] > Above60) { Centre += Points[I]; ++NumCentre; }
	if (NumCentre > 0)
	{
		Centre /= NumCentre;
		double Facing = 0.0;
		int32 NumUp = 0;
		for (int32 I = 0; I < Points.Num(); ++I) if (Heights[I] > Above75) { Facing += (Points[I] - Centre) | Normals[I]; ++NumUp; }
		if (NumUp > 0 && Facing / NumUp < 0.0)
		{
			for (FVector& N : Normals) N = -N;
		}
	}

	// The scalp, where hair roots belong, and the points on it the fit measures the roots' coverage on.
	TArray<int32> ScalpSamples;
	for (int32 I = 0; I < Points.Num(); ++I)
	{
		const double Below = Top - Points[I].Z;
		const FVector& N = Normals[I];
		if ((Below < ScalpTopCm || (N.Y < -0.3 && Below < ScalpBackCm)) && !(N.Y > 0.3 && Below > FaceStartCm)) ScalpSamples.Add(I);
	}
	ScalpOf.Init(INDEX_NONE, Points.Num());
	if (!ScalpSamples.IsEmpty())
	{
		const int32 Take = FMath::Min(ScalpSamples.Num(), MaxScalpPoints);
		for (int32 I = 0; I < Take; ++I) ScalpPoints.Add(Points[ScalpSamples[static_cast<int32>(static_cast<int64>(I) * ScalpSamples.Num() / Take)]]);
		FMantleScalpKdTree Tree;
		Tree.Build(ScalpPoints);
		TArray<int32> Ids;
		TArray<double> Distances;
		for (const int32 S : ScalpSamples)
		{
			Tree.Nearest(Points[S], 1, Ids, Distances);
			ScalpOf[S] = Ids[0];
		}
		ScalpNeighbourStart.Add(0);
		for (const FVector& P : ScalpPoints)
		{
			Tree.InRadius(P, 2.0 * CoverReachCm, Ids);
			ScalpNeighbourIds.Append(Ids);
			ScalpNeighbourStart.Add(ScalpNeighbourIds.Num());
		}
	}

	Order.SetNumUninitialized(Points.Num());
	for (int32 I = 0; I < Order.Num(); ++I) Order[I] = I;
	BuildRange(0, Order.Num(), 0);
	return true;
}

void FMantleHairHeadSamples::BuildRange(int32 Lo, int32 Hi, int32 Axis)
{
	if (Hi - Lo <= 1) return;
	TArrayView<int32> Range = MakeArrayView(Order.GetData() + Lo, Hi - Lo);
	Algo::Sort(Range, [this, Axis](int32 A, int32 B) { return Points[A][Axis] < Points[B][Axis]; });
	const int32 Mid = (Lo + Hi) / 2;
	BuildRange(Lo, Mid, (Axis + 1) % 3);
	BuildRange(Mid + 1, Hi, (Axis + 1) % 3);
}

void FMantleHairHeadSamples::NearestRange(int32 Lo, int32 Hi, int32 Axis, const FVector& P, int32& BestId, double& BestD2) const
{
	if (Lo >= Hi) return;
	const int32 Mid = (Lo + Hi) / 2;
	const int32 Id = Order[Mid];
	const FVector& Q = Points[Id];
	const double D2 = FVector::DistSquared(P, Q);
	if (D2 < BestD2) { BestD2 = D2; BestId = Id; }
	const double Diff = P[Axis] - Q[Axis];
	const int32 Next = (Axis + 1) % 3;
	if (Diff < 0.0)
	{
		NearestRange(Lo, Mid, Next, P, BestId, BestD2);
		if (Diff * Diff < BestD2) NearestRange(Mid + 1, Hi, Next, P, BestId, BestD2);
	}
	else
	{
		NearestRange(Mid + 1, Hi, Next, P, BestId, BestD2);
		if (Diff * Diff < BestD2) NearestRange(Lo, Mid, Next, P, BestId, BestD2);
	}
}

void FMantleHairHeadSamples::Nearest(const FVector& P, int32& OutId, double& OutDistSquared) const
{
	OutId = INDEX_NONE;
	OutDistSquared = TNumericLimits<double>::Max();
	NearestRange(0, Order.Num(), 0, P, OutId, OutDistSquared);
}

// ---------------------------------------------------------------- whole-piece fit

FMantleHairPieceFit::FSearch FMantleHairPieceFit::FSearch::Default()
{
	FSearch Out;
	// Coarse: scale 0.8 to 3.0 by 0.1, Y -10 to 10 cm by 2, top -16 to 12 cm by 2; fine: a quarter of each step.
	for (int32 I = 0; I <= 22; ++I) Out.Scales.Add(0.8 + 0.1 * I);
	for (int32 I = -5; I <= 5; ++I) Out.OffsetsY.Add(2.0 * I);
	for (int32 I = -8; I <= 6; ++I) Out.TopOffsets.Add(2.0 * I);
	Out.FineScaleStep = 0.025;
	Out.FineOffsetStep = 0.5;
	return Out;
}

FString FMantleHairPieceFit::FResult::Describe() const
{
	return FString::Printf(TEXT("Hair fitted to the head: sized to %.0f%% of its imported size, moved %.1f cm %s and %.1f cm %s; %.0f%% of the cards have their roots on the scalp, covering %.0f%% of it"),
		TotalScale() * 100.0, FMath::Abs(OffsetY), OffsetY >= 0.0 ? TEXT("forward") : TEXT("back"),
		FMath::Abs(OffsetZ), OffsetZ >= 0.0 ? TEXT("up") : TEXT("down"), SeatedFraction * 100.0, ScalpCoverage * 100.0);
}

bool FMantleHairPieceFit::Fit(const FMantleHairHeadSamples& Head, TConstArrayView<FVector3f> Points, TConstArrayView<FVector3f> CardEnds, FResult& Out, FString& Error)
{
	return Fit(Head, Points, CardEnds, FSearch::Default(), Out, Error);
}

bool FMantleHairPieceFit::Fit(const FMantleHairHeadSamples& Head, TConstArrayView<FVector3f> Points, TConstArrayView<FVector3f> CardEnds,
	const FSearch& Search, FResult& Out, FString& Error)
{
	using namespace MantleHairPieceFitPrivate;
	Out = FResult();
	if (!Head.IsValid()) { Error = TEXT("there is no head to fit the hair to"); return false; }
	if (Head.NumScalpPoints() == 0) { Error = TEXT("the head has no scalp to seat the hair on"); return false; }
	if (Points.IsEmpty()) { Error = TEXT("the hair has no vertices to fit"); return false; }
	if (CardEnds.Num() < 2 || CardEnds.Num() % 2 != 0) { Error = TEXT("the hair has no cards to seat on the head"); return false; }
	if (Search.Scales.IsEmpty() || Search.OffsetsY.IsEmpty() || Search.TopOffsets.IsEmpty()) { Error = TEXT("the hair fit has no candidates to try"); return false; }

	double MinX = TNumericLimits<double>::Max(), MaxX = -TNumericLimits<double>::Max(), MaxZ = -TNumericLimits<double>::Max();
	for (const FVector3f& P : Points)
	{
		MinX = FMath::Min<double>(MinX, P.X);
		MaxX = FMath::Max<double>(MaxX, P.X);
		MaxZ = FMath::Max<double>(MaxZ, P.Z);
	}
	const double Width = MaxX - MinX;
	if (Width < 1.e-6) { Error = TEXT("the hair has no width to size against the head"); return false; }
	const double Unit = ReferenceWidthCm / Width;
	const double CardsTop = MaxZ * Unit;

	// An even stride over the piece and its cards scores each candidate, so the result is repeatable.
	const auto Stride = [Unit](TConstArrayView<FVector3f> From, int32 Count, int32 Group, TArray<FVector>& To)
	{
		const int32 Groups = From.Num() / Group;
		const int32 Take = FMath::Min(Groups, Count);
		To.SetNumUninitialized(Take * Group);
		for (int32 I = 0; I < Take; ++I)
		{
			const int32 At = static_cast<int32>(static_cast<int64>(I) * Groups / Take);
			for (int32 G = 0; G < Group; ++G)
			{
				const FVector3f& P = From[At * Group + G];
				To[I * Group + G] = FVector(P.X, P.Y, P.Z) * Unit;
			}
		}
	};
	TArray<FVector> Sample, Ends;
	Stride(Points, MaxFitPoints, 1, Sample);
	Stride(CardEnds, MaxFitCards, 2, Ends);
	const int32 Scored = Sample.Num();
	const int32 Cards = Ends.Num() / 2;
	const double HeadTop = Head.TopCm();

	const auto Evaluate = [&](double Scale, double OffsetY, double OffsetZ)
	{
		FPieceFitScore Score;
		const FVector Offset(0.0, OffsetY, OffsetZ);
		int32 Outside = 0, Deep = 0;
		for (int32 I = 0; I < Scored; ++I)
		{
			const FVector Q = Sample[I] * Scale + Offset;
			int32 Id = INDEX_NONE;
			double D2 = 0.0;
			Head.Nearest(Q, Id, D2);
			if (((Q - Head.Point(Id)) | Head.Normal(Id)) > 0.0) ++Outside;
			else if (D2 > DeepInsideCm * DeepInsideCm && Q.Z > HeadTop - HeadDepthCm) ++Deep;
		}
		// Each card's root is its end nearer the head; a seated root covers the scalp points within CoverReachCm of it.
		TBitArray<> Covered(false, Head.NumScalpPoints());
		TArray<double> RootDistances;
		RootDistances.SetNumUninitialized(Cards);
		int32 Seated = 0, PointsCovered = 0;
		const double Reach2 = FMantleHairHeadSamples::CoverReachCm * FMantleHairHeadSamples::CoverReachCm;
		for (int32 C = 0; C < Cards; ++C)
		{
			const FVector A = Ends[2 * C] * Scale + Offset, B = Ends[2 * C + 1] * Scale + Offset;
			int32 IdA = INDEX_NONE, IdB = INDEX_NONE;
			double DA = 0.0, DB = 0.0;
			Head.Nearest(A, IdA, DA);
			Head.Nearest(B, IdB, DB);
			const bool bA = DA <= DB;
			const double Distance = FMath::Sqrt(bA ? DA : DB);
			RootDistances[C] = Distance;
			const int32 Near = Head.NearestScalpPoint(bA ? IdA : IdB);
			if (Distance > SeatCm || Near == INDEX_NONE) continue;
			++Seated;
			const FVector& Root = bA ? A : B;
			for (const int32 N : Head.ScalpNeighbours(Near))
			{
				if (!Covered[N] && FVector::DistSquared(Root, Head.ScalpPoint(N)) <= Reach2) { Covered[N] = true; ++PointsCovered; }
			}
		}
		RootDistances.Sort();
		Score.Outside = static_cast<double>(Outside) / Scored;
		Score.DeepInside = static_cast<double>(Deep) / Scored;
		Score.Seated = static_cast<double>(Seated) / Cards;
		Score.Coverage = static_cast<double>(PointsCovered) / Head.NumScalpPoints();
		Score.Rank = PieceFitRound(Score.Seated * Score.Coverage, 1000.0);
		Score.RootMedian = PieceFitPercentile(RootDistances, 0.5);
		return Score;
	};
	const auto OffsetZFor = [&](double Scale, double TopOffset) { return HeadTop - Scale * CardsTop + TopOffset; };

	struct FCandidate { double Scale; double OffsetY; double TopOffset; };
	// Scores every candidate in parallel; the first best in list order wins ties, so the result does not depend on the threads.
	const auto Best = [&](const TArray<FCandidate>& Candidates, FPieceFitScore& OutScore)
	{
		TArray<FPieceFitScore> Scores;
		Scores.SetNum(Candidates.Num());
		ParallelFor(Candidates.Num(), [&](int32 Index)
		{
			const FCandidate& C = Candidates[Index];
			Scores[Index] = Evaluate(C.Scale, C.OffsetY, OffsetZFor(C.Scale, C.TopOffset));
		});
		int32 Winner = 0;
		for (int32 Index = 1; Index < Scores.Num(); ++Index) if (PieceFitBetter(Scores[Index], Scores[Winner])) Winner = Index;
		Out.CandidatesScored += Candidates.Num();
		OutScore = Scores[Winner];
		return Candidates[Winner];
	};

	TArray<FCandidate> Grid;
	for (const double S : Search.Scales) for (const double Y : Search.OffsetsY) for (const double T : Search.TopOffsets) Grid.Add({S, Y, T});
	FPieceFitScore Score;
	FCandidate Chosen = Best(Grid, Score);
	if (Search.FineScaleStep > 0.0 && Search.FineOffsetStep > 0.0)
	{
		// Four fine steps either way of the best coarse candidate (one coarse step of the default grid).
		Grid.Reset();
		for (int32 S = -4; S <= 4; ++S) for (int32 Y = -4; Y <= 4; ++Y) for (int32 T = -4; T <= 4; ++T)
		{
			const double Scale = Chosen.Scale + S * Search.FineScaleStep;
			if (Scale > 0.0) Grid.Add({Scale, Chosen.OffsetY + Y * Search.FineOffsetStep, Chosen.TopOffset + T * Search.FineOffsetStep});
		}
		Chosen = Best(Grid, Score);
	}

	Out.UnitScale = Unit;
	Out.Scale = PieceFitRound(Chosen.Scale, 10000.0);
	Out.OffsetY = Chosen.OffsetY;
	Out.OffsetZ = PieceFitRound(OffsetZFor(Chosen.Scale, Chosen.TopOffset), 100.0);
	Out.TopOffset = Chosen.TopOffset;
	Out.OutsideFraction = Score.Outside;
	Out.DeepInsideFraction = Score.DeepInside;
	Out.SeatedFraction = Score.Seated;
	Out.ScalpCoverage = Score.Coverage;
	Out.RootMedianCm = Score.RootMedian;
	Out.bKeepsOutOfHead = Score.KeepsOut();
	return true;
}

void FMantleHairPieceFit::ApplyTo(const FResult& Fit, TArray<FVector3f>& Points)
{
	for (FVector3f& P : Points) P = Fit.Apply(P);
}
