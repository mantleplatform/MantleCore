// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHairCapSplit.h"

#include "Containers/Map.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleHairCapSplitPrivate
{
	int32 CapFindRoot(TArray<int32>& Parent, int32 X)
	{
		while (Parent[X] != X) { Parent[X] = Parent[Parent[X]]; X = Parent[X]; }
		return X;
	}

	double MedianArea(TArray<double> Values)
	{
		if (Values.IsEmpty()) return 0.0;
		Values.Sort();
		const int32 N = Values.Num();
		return N % 2 ? Values[N / 2] : 0.5 * (Values[N / 2 - 1] + Values[N / 2]);
	}
}

bool FMantleHairCapSplit::IsCapName(const FString& SectionName)
{
	const FString Name = SectionName.ToLower();
	return Name.Contains(TEXT("cap")) || Name.Contains(TEXT("scalp")) || Name.Contains(TEXT("base")) || Name.Contains(TEXT("skull"));
}

FString FMantleHairCapSplit::FResult::Describe() const
{
	if (!bFound) return TEXT("Hair cap: none found, so the hair is fitted as cards only.");
	return FString::Printf(TEXT("Hair cap: found %d cap piece(s)%s among %d pieces, %d cap triangles; its open edge is wrapped onto the head."),
		CapPieceCount, CapSection.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" in section %s"), *CapSection), PieceCount, CapIndices.Num() / 3);
}

FMantleHairCapSplit::FResult FMantleHairCapSplit::Split(TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Triangles,
	TConstArrayView<int32> TriangleSections, TConstArrayView<FString> SectionNames)
{
	using namespace MantleHairCapSplitPrivate;
	FResult Out;
	Out.CardIndices = TArray<uint32>(Triangles.GetData(), Triangles.Num());
	if (Triangles.Num() % 3 != 0 || Triangles.IsEmpty()) return Out;
	for (const uint32 I : Triangles)
	{
		if (I >= static_cast<uint32>(Positions.Num())) return Out;
	}

	// Vertices at one position (to 0.01 mm) are one for connectivity; render data splits them at UV seams.
	TArray<int32> Inv;
	Inv.SetNumUninitialized(Positions.Num());
	int32 NumWelded = 0;
	{
		TMap<FIntVector, int32> Seen;
		for (int32 I = 0; I < Positions.Num(); ++I)
		{
			const FVector3f& P = Positions[I];
			const FIntVector Key(FMath::RoundToInt(P.X * 1000.f), FMath::RoundToInt(P.Y * 1000.f), FMath::RoundToInt(P.Z * 1000.f));
			if (const int32* Found = Seen.Find(Key)) { Inv[I] = *Found; continue; }
			Inv[I] = NumWelded;
			Seen.Add(Key, NumWelded++);
		}
	}
	TArray<int32> Parent;
	Parent.SetNumUninitialized(NumWelded);
	for (int32 I = 0; I < NumWelded; ++I) Parent[I] = I;
	const int32 NumTris = Triangles.Num() / 3;
	for (int32 T = 0; T < NumTris; ++T)
	{
		const int32 A = CapFindRoot(Parent, Inv[Triangles[3 * T]]);
		const int32 B = CapFindRoot(Parent, Inv[Triangles[3 * T + 1]]);
		const int32 C = CapFindRoot(Parent, Inv[Triangles[3 * T + 2]]);
		Parent[B] = A;
		Parent[CapFindRoot(Parent, C)] = CapFindRoot(Parent, A);
	}

	struct FPiece { double Area = 0.0; int32 Tris = 0; int32 OpenEdges = 0; int32 NamedTris = 0; };
	TMap<int32, FPiece> Pieces;
	TMap<uint64, int32> EdgeUse;
	TArray<int32> TriPiece;
	TriPiece.SetNumUninitialized(NumTris);
	for (int32 T = 0; T < NumTris; ++T)
	{
		const FVector3f& A = Positions[Triangles[3 * T]];
		const FVector3f& B = Positions[Triangles[3 * T + 1]];
		const FVector3f& C = Positions[Triangles[3 * T + 2]];
		const int32 Root = CapFindRoot(Parent, Inv[Triangles[3 * T]]);
		TriPiece[T] = Root;
		FPiece& Piece = Pieces.FindOrAdd(Root);
		Piece.Area += 0.5 * FVector3f::CrossProduct(B - A, C - A).Size();
		++Piece.Tris;
		if (TriangleSections.IsValidIndex(T) && SectionNames.IsValidIndex(TriangleSections[T]) && IsCapName(SectionNames[TriangleSections[T]])) ++Piece.NamedTris;
		for (int32 E = 0; E < 3; ++E)
		{
			const int32 U = Inv[Triangles[3 * T + E]], V = Inv[Triangles[3 * T + (E + 1) % 3]];
			if (U == V) continue;
			const uint64 Key = (static_cast<uint64>(FMath::Min(U, V)) << 32) | static_cast<uint64>(FMath::Max(U, V));
			++EdgeUse.FindOrAdd(Key);
		}
	}
	for (const TPair<uint64, int32>& Edge : EdgeUse)
	{
		if (Edge.Value == 1) ++Pieces.FindOrAdd(CapFindRoot(Parent, static_cast<int32>(Edge.Key >> 32))).OpenEdges;
	}

	Out.PieceCount = Pieces.Num();
	if (Out.PieceCount < MinPieces) return Out;

	TArray<int32> Roots;
	Pieces.GetKeys(Roots);
	Roots.Sort();
	TSet<int32> CapRoots;
	TArray<double> Areas;
	for (const int32 Root : Roots) Areas.Add(Pieces[Root].Area);
	// Cards far outnumber caps, so the median of all pieces is the median card.
	const double Median = MedianArea(Areas);
	for (const int32 Root : Roots)
	{
		const FPiece& Piece = Pieces[Root];
		const bool bNamed = Piece.NamedTris * 2 >= Piece.Tris;
		const double Ratio = bNamed ? SectionHintAreaRatio : CapAreaRatio;
		if (Piece.Tris >= MinCapTriangles && Piece.OpenEdges > 0 && Median > 0.0 && Piece.Area >= Ratio * Median) CapRoots.Add(Root);
	}
	if (CapRoots.IsEmpty() || CapRoots.Num() > MaxCapPieces) return Out;

	Out.CardIndices.Reset();
	TMap<int32, int32> SectionVotes;
	for (int32 T = 0; T < NumTris; ++T)
	{
		TArray<uint32>& Target = CapRoots.Contains(TriPiece[T]) ? Out.CapIndices : Out.CardIndices;
		Target.Append({Triangles[3 * T], Triangles[3 * T + 1], Triangles[3 * T + 2]});
		if (&Target == &Out.CapIndices && TriangleSections.IsValidIndex(T)) ++SectionVotes.FindOrAdd(TriangleSections[T]);
	}
	if (Out.CardIndices.IsEmpty())
	{
		Out.CardIndices = Out.CapIndices;
		Out.CapIndices.Reset();
		return Out;
	}
	Out.bFound = true;
	Out.CapPieceCount = CapRoots.Num();
	int32 BestVotes = 0, BestSection = INDEX_NONE;
	for (const TPair<int32, int32>& Vote : SectionVotes)
	{
		if (Vote.Value > BestVotes) { BestVotes = Vote.Value; BestSection = Vote.Key; }
	}
	if (SectionNames.IsValidIndex(BestSection)) Out.CapSection = SectionNames[BestSection];
	return Out;
}
