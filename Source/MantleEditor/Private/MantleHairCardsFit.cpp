// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHairCardsFit.h"

#include "MantleHairCapSplit.h"
#include "MantleHairPieceFit.h"
#include "MantleHairScalpFit.h"

double FMantleHairCardsFit::MedianDistanceToSkin(const FMantleScalpHeadSurface& Head, TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Triangles)
{
	TArray<double> Distances;
	for (const uint32 V : Triangles)
	{
		if (Positions.IsValidIndex(static_cast<int32>(V))) Distances.Add(FMath::Abs(Head.Query(FVector(Positions[V])).SignedDistance));
	}
	if (Distances.IsEmpty()) return 0.0;
	Distances.Sort();
	return Distances[Distances.Num() / 2];
}

bool FMantleHairCardsFit::CoversScalp(TConstArrayView<FVector3f> HeadPositions, TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Triangles)
{
	float Top = -TNumericLimits<float>::Max();
	for (const FVector3f& P : HeadPositions) Top = FMath::Max(Top, P.Z);
	FBox3f Scalp(ForceInit), Cap(ForceInit);
	for (const FVector3f& P : HeadPositions) if (P.Z > Top - FMantleHairHeadSamples::ScalpTopCm) Scalp += P;
	for (const uint32 V : Triangles) if (Positions.IsValidIndex(static_cast<int32>(V))) Cap += Positions[V];
	if (!Scalp.IsValid || !Cap.IsValid) return false;
	const FVector3f ScalpSize = Scalp.GetSize(), CapSize = Cap.GetSize();
	return CapSize.X >= CapMinScalpSpan * ScalpSize.X && CapSize.Y >= CapMinScalpSpan * ScalpSize.Y;
}

bool FMantleHairCardsFit::CoversFace(const FMantleHairHeadSamples& Head, const FVector& P)
{
	int32 Id = INDEX_NONE;
	double D2 = 0.0;
	Head.Nearest(P, Id, D2);
	if (Id == INDEX_NONE || Head.Normal(Id).Y <= 0.3 || Head.TopCm() - Head.Point(Id).Z <= FaceClearTopCm) return false;
	return ((P - Head.Point(Id)) | Head.Normal(Id)) < FaceClearMarginCm;
}

int32 FMantleHairCardsFit::ClearFaceAddedByTheFit(const FMantleHairHeadSamples& Head, TConstArrayView<int32> Vertices, TConstArrayView<FVector3f> Baseline, TArray<FVector3f>& Fitted)
{
	int32 Restored = 0;
	for (const int32 V : Vertices)
	{
		if (!Fitted.IsValidIndex(V) || !Baseline.IsValidIndex(V)) continue;
		if (!CoversFace(Head, FVector(Fitted[V])) || CoversFace(Head, FVector(Baseline[V]))) continue;
		Fitted[V] = Baseline[V];
		++Restored;
	}
	return Restored;
}

FVector3f FMantleHairCardsFit::RotateUpright(const FVector3f& P, int32 QuarterTurns)
{
	// +1: Y-up to Z-up (up becomes +Z, the file's +Z becomes -Y); -1: the other way.
	if (QuarterTurns > 0) return FVector3f(P.X, -P.Z, P.Y);
	if (QuarterTurns < 0) return FVector3f(P.X, P.Z, -P.Y);
	return P;
}

FMantleHairCardsFit::FResult FMantleHairCardsFit::Run(const FInput& Input)
{
	FResult Out;
	Out.Positions = TArray<FVector3f>(Input.HairPositions.GetData(), Input.HairPositions.Num());
	Out.Report.Headline = FMantleHairFitReport::Headline();

	FString Error;
	FMantleHairHeadSamples Samples;
	FMantleScalpHeadSurface Surface;
	if (!Samples.Build(Input.HeadPositions, Input.HeadIndices, Error) || !Surface.Build(Input.HeadPositions, Input.HeadIndices, Error))
	{
		Out.Warnings.Add(FString::Printf(TEXT("the hair could not be fitted to the head (%s), so it uploads as it is."), *Error));
		return Out;
	}

	// A head baked into the hair file is not hair: it must not size the piece or be pushed around as cards.
	TArray<uint32> HairTriangles;
	FMantleHairFitReport::FInput Report;
	TArray<uint8> bHairVertex;
	bHairVertex.Init(0, Input.HairPositions.Num());
	for (int32 T = 0; T + 2 < Input.HairIndices.Num(); T += 3)
	{
		const int32 Section = Input.TriangleSections.IsValidIndex(T / 3) ? Input.TriangleSections[T / 3] : INDEX_NONE;
		if (Input.SectionNames.IsValidIndex(Section) && FMantleHairFitReport::IsBakedHeadName(Input.SectionNames[Section]))
		{
			Report.bBakedHead = true;
			Report.BakedHeadSection = Input.SectionNames[Section];
			continue;
		}
		for (int32 E = 0; E < 3; ++E)
		{
			HairTriangles.Add(Input.HairIndices[T + E]);
			if (Input.HairPositions.IsValidIndex(Input.HairIndices[T + E])) bHairVertex[Input.HairIndices[T + E]] = 1;
		}
	}
	TArray<FVector3f> SizingPoints;
	for (int32 V = 0; V < Input.HairPositions.Num(); ++V) if (bHairVertex[V]) SizingPoints.Add(Input.HairPositions[V]);
	if (SizingPoints.IsEmpty())
	{
		Out.Warnings.Add(TEXT("the hair could not be fitted to the head (no hair was found in it), so it uploads as it is."));
		return Out;
	}

	// Which way is up: as imported, or turned a quarter either way about X. Coarse pass only; a turn must win clearly.
	FMantleHairPieceFit::FSearch Coarse = FMantleHairPieceFit::FSearch::Default();
	Coarse.FineScaleStep = 0.0;
	Coarse.FineOffsetStep = 0.0;
	const auto Oriented = [&](int32 Turns)
	{
		TArray<FVector3f> Moved(Input.HairPositions.GetData(), Input.HairPositions.Num());
		for (FVector3f& P : Moved) P = RotateUpright(P, Turns);
		return Moved;
	};
	const auto Sizing = [&](const TArray<FVector3f>& Positions)
	{
		TArray<FVector3f> Points;
		for (int32 V = 0; V < Positions.Num(); ++V) if (bHairVertex[V]) Points.Add(Positions[V]);
		return Points;
	};
	int32 Turns = 0;
	{
		double BestRank = -1.0, UprightRank = 0.0;
		for (const int32 Candidate : {0, 1, -1})
		{
			const TArray<FVector3f> Moved = Oriented(Candidate);
			FMantleHairPieceFit::FResult Trial;
			FString TrialError;
			const double Rank = FMantleHairPieceFit::Fit(Samples, Sizing(Moved), FMantleScalpFit::FindCardEnds(Moved, HairTriangles), Coarse, Trial, TrialError)
				? Trial.SeatedFraction * Trial.ScalpCoverage : 0.0;
			if (Candidate == 0) UprightRank = Rank;
			if (Rank > BestRank + 1.e-9) { BestRank = Rank; Turns = Candidate; }
		}
		if (Turns != 0 && BestRank < UprightRank + OrientMinGain) Turns = 0;
	}

	// The piece is placed where its card roots meet the scalp, so it needs each card's two ends.
	const TArray<FVector3f> Upright = Oriented(Turns);
	SizingPoints = Sizing(Upright);
	const TArray<FVector3f> CardEnds = FMantleScalpFit::FindCardEnds(Upright, HairTriangles);
	FMantleHairPieceFit::FResult Piece;
	if (!FMantleHairPieceFit::Fit(Samples, SizingPoints, CardEnds, Piece, Error))
	{
		Out.Warnings.Add(FString::Printf(TEXT("the hair could not be fitted to the head (%s), so it uploads as it is."), *Error));
		return Out;
	}
	Out.Positions = Upright;
	Out.QuarterTurns = Turns;
	if (Turns != 0) Out.Notes.Add(TEXT("The hair was lying on its side (its file is Y-up), so it was turned upright."));
	FMantleHairPieceFit::ApplyTo(Piece, Out.Positions);
	// The hair as made, sized and seated rigidly: the face-coverage baseline for what the scalp fit then adds.
	const TArray<FVector3f> AfterPiece = Out.Positions;
	Out.Notes.Add(Piece.Describe());
	if (!Piece.bKeepsOutOfHead)
	{
		Out.Notes.Add(TEXT("We couldn't find a size and position that keeps this hair out of the head, so check that it sits on the head."));
	}
	Out.bFitted = true;

	// Cap versus cards, on the fitted positions (ratios, so the unit does not matter).
	TArray<int32> HairSections;
	if (!Input.TriangleSections.IsEmpty())
	{
		for (int32 T = 0; T + 2 < Input.HairIndices.Num(); T += 3)
		{
			const int32 Section = Input.TriangleSections.IsValidIndex(T / 3) ? Input.TriangleSections[T / 3] : INDEX_NONE;
			if (Input.SectionNames.IsValidIndex(Section) && FMantleHairFitReport::IsBakedHeadName(Input.SectionNames[Section])) continue;
			HairSections.Add(Section);
		}
	}
	FMantleHairCapSplit::FResult Split = FMantleHairCapSplit::Split(Out.Positions, HairTriangles, HairSections, Input.SectionNames);
	if (Split.bFound)
	{
		// A cap hugs the scalp. One big piece that hangs far from the skin is a sheet of long hair, not a cap.
		const double CapMedian = MedianDistanceToSkin(Surface, Out.Positions, Split.CapIndices);
		// A cap also spreads over the top of the head. Pigtails, braids and tails are big pieces too, but narrow.
		const bool bCoversScalp = CoversScalp(Input.HeadPositions, Out.Positions, Split.CapIndices);
		if (CapMedian > CapMaxMedianDistanceCm || !bCoversScalp)
		{
			Out.Notes.Add(CapMedian > CapMaxMedianDistanceCm
				? FString::Printf(TEXT("Hair cap: the big piece hangs %.1f cm from the head, so it is treated as hair, not a cap."), CapMedian)
				: FString(TEXT("Hair cap: the big pieces do not spread over the top of the head (tails or braids), so they are treated as hair, not a cap.")));
			Split = FMantleHairCapSplit::FResult();
			Split.CardIndices = HairTriangles;
		}
	}
	Out.Notes.Add(Split.Describe());
	Out.bCapFound = Split.bFound;

	FMantleScalpFit::FMesh CardMesh;
	CardMesh.Positions = Out.Positions;
	CardMesh.Indices = Split.CardIndices;
	FMantleScalpFit::FMesh CapMesh;
	CapMesh.Positions = Out.Positions;
	CapMesh.Indices = Split.CapIndices;
	FMantleScalpFit::FResult Scalp;
	if (!FMantleScalpFit::Run(Surface, CardMesh, Split.bFound ? &CapMesh : nullptr, Scalp, Error))
	{
		Out.Warnings.Add(FString::Printf(TEXT("the hair could not be settled on the scalp (%s), so it keeps the size and position it was given."), *Error));
		Report.bKeepsOutOfHead = Piece.bKeepsOutOfHead;
		Report.RootsSeatedPercent = Piece.SeatedFraction * 100.0;
		Report.bHadCap = Split.bFound;
		Out.Report = FMantleHairFitReport::Describe(Report);
		return Out;
	}

	// Cards move by the card result, the cap by the cap's wrap; a vertex the cap uses is the cap's.
	TArray<uint8> bCapVertex;
	bCapVertex.Init(0, Out.Positions.Num());
	for (const uint32 I : Split.CapIndices) bCapVertex[I] = 1;
	for (int32 V = 0; V < Out.Positions.Num(); ++V)
	{
		if (bCapVertex[V] && Scalp.CapPositions.IsValidIndex(V)) Out.Positions[V] = Scalp.CapPositions[V];
		else if (!bCapVertex[V] && Scalp.CardPositions.IsValidIndex(V)) Out.Positions[V] = Scalp.CardPositions[V];
	}
	// A fringe or a lock over one eye stays; hair the scalp fit pushed onto the face goes back to where the rigid fit had it.
	{
		TArray<int32> CardVertexIds;
		for (const uint32 V : TSet<uint32>(Split.CardIndices)) CardVertexIds.Add(static_cast<int32>(V));
		CardVertexIds.Sort();
		Out.FaceVerticesMoved = ClearFaceAddedByTheFit(Samples, CardVertexIds, AfterPiece, Out.Positions);
		if (Out.FaceVerticesMoved > 0)
		{
			Out.Notes.Add(FString::Printf(TEXT("Hair over the face: %d hair vertices that the fit had pushed onto the face were put back."), Out.FaceVerticesMoved));
		}
	}
	const FMantleScalpFit::FMetrics& M = Scalp.Metrics;
	Out.Notes.Add(FString::Printf(TEXT("Hair scalp fit: %d cards, roots within 5 mm of the skin %.0f%% to %.0f%%, %d vertices lifted off the skin (largest %.1f cm)"),
		M.CardCount, M.RootsWithin5mmBefore, M.RootsWithin5mmAfter, M.CardLifted, M.CardLiftMaxCm));
	if (Scalp.bCheckFit) Out.Notes.Add(Scalp.CheckFitNote);

	Report.bKeepsOutOfHead = Piece.bKeepsOutOfHead;
	Report.bScalpCheckFit = Scalp.bCheckFit;
	// The whole-piece fit's own measure: after Unreal's import, touching cards weld into clumps, so the scalp fit's roots are coarse.
	Report.RootsSeatedPercent = Piece.SeatedFraction * 100.0;
	// Only the vertices the cards use: the metrics count every position, cap and built-in head included.
	TSet<uint32> CardVerticesUsed(Split.CardIndices);
	Report.CardVertices = CardVerticesUsed.Num();
	Report.CardInsideAfter = M.CardInsideAfter;
	Report.bHadCap = Split.bFound;
	Out.Report = FMantleHairFitReport::Describe(Report);
	return Out;
}
