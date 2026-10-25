// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHairCards.h"

FColor FMantleHairCards::AtlasPixel(int32 Layout, TConstArrayView<FColor> Textures, bool bPreserveNativeCoordU)
{
	// UE 5.8 HairCardsAttributeCommon.ush GetHairCardTextureIndex/GetHairCardChannelIndex.
	if ((Layout == 0 || Layout == 1) && Textures.Num() >= 4) return FColor(Textures[1].R, Textures[0].R, Textures[3].A, bPreserveNativeCoordU ? Textures[3].B : 255);
	if (Layout == 2 && Textures.Num() >= 2) return FColor(Textures[1].R, Textures[1].G, Textures[1].B, bPreserveNativeCoordU ? Textures[0].A : 255);
	if (Layout == 3 && Textures.Num() >= 3) return FColor(Textures[2].A, Textures[1].B, Textures[2].B, bPreserveNativeCoordU ? Textures[0].A : 255);
	return FColor::Transparent;
}

TArray<int32> FMantleHairCards::SelectWholeCards(TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Indices, int32 Budget)
{
	TArray<int32> Selected;
	if (Budget <= 0 || Indices.Num() % 3 != 0) return Selected;
	for (const uint32 I : Indices) if (I >= static_cast<uint32>(Positions.Num())) return Selected;
	const TArray<TArray<int32>> Cards = FindCards(Positions, Indices);
	if (Cards.IsEmpty()) return Selected;
	TArray<FVector3f> Centres;
	Centres.Reserve(Cards.Num());
	FVector3f Mean = FVector3f::ZeroVector;
	for (const TArray<int32>& Card : Cards)
	{
		FVector3f Centre = FVector3f::ZeroVector;
		for (const int32 T : Card) for (int32 C = 0; C < 3; ++C) Centre += Positions[Indices[T * 3 + C]];
		Centre /= static_cast<float>(Card.Num() * 3);
		Centres.Add(Centre);
		Mean += Centre;
	}
	Mean /= static_cast<float>(Cards.Num());

	// Farthest-point pick: start nearest the middle, then always take the card farthest from every card already kept.
	// A card too big for what is left is dropped for good (later picks only get smaller budgets).
	TArray<float> Gap;
	Gap.Init(TNumericLimits<float>::Max(), Cards.Num());
	TArray<uint8> Done;
	Done.Init(0, Cards.Num());
	int32 Next = 0;
	float Nearest = TNumericLimits<float>::Max();
	for (int32 C = 0; C < Cards.Num(); ++C)
	{
		const float D = FVector3f::DistSquared(Centres[C], Mean);
		if (D < Nearest) { Nearest = D; Next = C; }
	}
	int32 Left = Budget;
	while (Next != INDEX_NONE)
	{
		Done[Next] = 1;
		if (Cards[Next].Num() <= Left)
		{
			Selected.Append(Cards[Next]);
			Left -= Cards[Next].Num();
			for (int32 C = 0; C < Cards.Num(); ++C) if (!Done[C]) Gap[C] = FMath::Min(Gap[C], FVector3f::DistSquared(Centres[C], Centres[Next]));
		}
		Next = INDEX_NONE;
		float Farthest = -1.f;
		for (int32 C = 0; C < Cards.Num(); ++C)
		{
			if (Done[C] || Cards[C].Num() > Left) continue;
			if (Gap[C] > Farthest) { Farthest = Gap[C]; Next = C; }
		}
	}
	Selected.Sort();
	return Selected;
}

int32 FMantleHairCards::SmallestCardTriangles(TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Indices)
{
	if (Indices.Num() % 3 != 0) return 0;
	for (const uint32 I : Indices) if (I >= static_cast<uint32>(Positions.Num())) return 0;
	int32 Smallest = 0;
	for (const TArray<int32>& Card : FindCards(Positions, Indices)) if (Smallest == 0 || Card.Num() < Smallest) Smallest = Card.Num();
	return Smallest;
}

TArray<int32> FMantleHairCards::AllocateSectionBudgets(TConstArrayView<int32> SectionTriangles, TConstArrayView<int32> SectionSmallestCard, int32 Budget, bool& bOverBudget)
{
	const int32 Sections = SectionTriangles.Num();
	TArray<int32> Out;
	Out.Init(0, Sections);
	bOverBudget = false;
	int64 Total = 0, Reserved = 0;
	for (int32 S = 0; S < Sections; ++S)
	{
		Total += SectionTriangles[S];
		Out[S] = SectionSmallestCard.IsValidIndex(S) ? SectionSmallestCard[S] : 0;
		Reserved += Out[S];
	}
	if (Total <= 0) return Out;
	if (Reserved > Budget) { bOverBudget = true; return Out; }
	// Everyone keeps their smallest card; what is left of the budget is shared by triangle count.
	const int64 Spare = Budget - Reserved;
	for (int32 S = 0; S < Sections; ++S) Out[S] += static_cast<int32>(Spare * SectionTriangles[S] / Total);
	return Out;
}

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleHairCardsPrivate
{
	/** Render vertices at one position share an id. */
	TArray<int32> WeldByPosition(TConstArrayView<FVector3f> Positions)
	{
		TArray<int32> Welded;
		Welded.SetNumUninitialized(Positions.Num());
		TMap<FVector3f, int32> Seen;
		Seen.Reserve(Positions.Num());
		for (int32 Vertex = 0; Vertex < Positions.Num(); ++Vertex)
		{
			if (const int32* Found = Seen.Find(Positions[Vertex]))
			{
				Welded[Vertex] = *Found;
			}
			else
			{
				Seen.Add(Positions[Vertex], Vertex);
				Welded[Vertex] = Vertex;
			}
		}
		return Welded;
	}

	int32 FindRoot(TArray<int32>& Parent, int32 Node)
	{
		while (Parent[Node] != Node)
		{
			Parent[Node] = Parent[Parent[Node]];
			Node = Parent[Node];
		}
		return Node;
	}

	/** The engine's FSimilarUVVertices: vertices whose coordinate along one axis agrees within UVThreshold. */
	struct FSimilarGroup
	{
		float TexCoord = 0.0f;
		TArray<int32> Vertices;
	};

	void AddSimilar(TArray<FSimilarGroup>& Groups, int32 Vertex, float TexCoord)
	{
		for (FSimilarGroup& Group : Groups)
		{
			if (FMath::Abs(Group.TexCoord - TexCoord) < FMantleHairCards::UVThreshold)
			{
				Group.Vertices.AddUnique(Vertex);
				return;
			}
		}
		FSimilarGroup& Group = Groups.AddDefaulted_GetRef();
		Group.TexCoord = TexCoord;
		Group.Vertices.Add(Vertex);
	}

	void GroupCard(TConstArrayView<int32> Triangles, TConstArrayView<uint32> Indices, TConstArrayView<FVector2f> UVs,
		TArray<FSimilarGroup>& OutU, TArray<FSimilarGroup>& OutV)
	{
		OutU.Reset();
		OutV.Reset();
		for (const int32 Triangle : Triangles)
		{
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const int32 Vertex = static_cast<int32>(Indices[Triangle * 3 + Corner]);
				AddSimilar(OutU, Vertex, UVs[Vertex].X);
				AddSimilar(OutV, Vertex, UVs[Vertex].Y);
			}
		}
	}
}

TArray<TArray<int32>> FMantleHairCards::FindCards(TConstArrayView<FVector3f> Positions, TConstArrayView<uint32> Indices)
{
	using namespace MantleHairCardsPrivate;

	const int32 NumTriangles = Indices.Num() / 3;
	const TArray<int32> Welded = WeldByPosition(Positions);

	TArray<int32> Parent;
	Parent.SetNumUninitialized(NumTriangles);
	for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle)
	{
		Parent[Triangle] = Triangle;
	}

	// Two triangles sharing an edge (by welded position) are one card.
	TMap<uint64, int32> EdgeOwner;
	for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle)
	{
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const uint32 A = Indices[Triangle * 3 + Corner];
			const uint32 B = Indices[Triangle * 3 + (Corner + 1) % 3];
			if (A >= static_cast<uint32>(Positions.Num()) || B >= static_cast<uint32>(Positions.Num()))
			{
				continue;
			}
			const uint32 WA = static_cast<uint32>(Welded[A]);
			const uint32 WB = static_cast<uint32>(Welded[B]);
			if (WA == WB)
			{
				continue;
			}
			const uint64 Key = (static_cast<uint64>(FMath::Min(WA, WB)) << 32) | FMath::Max(WA, WB);
			if (const int32* Other = EdgeOwner.Find(Key))
			{
				const int32 RootA = FindRoot(Parent, Triangle);
				const int32 RootB = FindRoot(Parent, *Other);
				if (RootA != RootB)
				{
					Parent[FMath::Max(RootA, RootB)] = FMath::Min(RootA, RootB);
				}
			}
			else
			{
				EdgeOwner.Add(Key, Triangle);
			}
		}
	}

	// Cards in the order of their first triangle, triangles in index order.
	TArray<TArray<int32>> Cards;
	TMap<int32, int32> CardOfRoot;
	for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle)
	{
		const int32 Root = FindRoot(Parent, Triangle);
		int32* Card = CardOfRoot.Find(Root);
		if (!Card)
		{
			Card = &CardOfRoot.Add(Root, Cards.Num());
			Cards.AddDefaulted();
		}
		Cards[*Card].Add(Triangle);
	}
	return Cards;
}

TArray<float> FMantleHairCards::ComputeCoordU(TConstArrayView<FVector3f> Positions, TConstArrayView<FVector2f> UVs,
	TConstArrayView<uint32> Indices, int32* OutCardCount, bool* OutMainAxisIsU)
{
	using namespace MantleHairCardsPrivate;

	TArray<float> CoordU;
	CoordU.SetNumZeroed(Positions.Num());
	if (UVs.Num() != Positions.Num() || Indices.Num() < 3)
	{
		if (OutCardCount)
		{
			*OutCardCount = 0;
		}
		return CoordU;
	}
	for (const uint32 Index : Indices)
	{
		if (Index >= static_cast<uint32>(Positions.Num()))
		{
			return CoordU;
		}
	}

	const TArray<TArray<int32>> Cards = FindCards(Positions, Indices);
	const TArray<int32> Welded = WeldByPosition(Positions);
	if (OutCardCount)
	{
		*OutCardCount = Cards.Num();
	}

	// The atlas's main axis: the one along which more cards have more distinct coordinates.
	int32 MainUCount = 0;
	int32 ValidCount = 0;
	TArray<FSimilarGroup> GroupsU, GroupsV;
	for (const TArray<int32>& Card : Cards)
	{
		GroupCard(Card, Indices, UVs, GroupsU, GroupsV);
		if (GroupsU.Num() != GroupsV.Num())
		{
			++ValidCount;
			MainUCount += GroupsU.Num() > GroupsV.Num() ? 1 : 0;
		}
	}
	const bool bMainU = (static_cast<float>(MainUCount) / static_cast<float>(FMath::Max(1, ValidCount))) > 0.5f;
	if (OutMainAxisIsU)
	{
		*OutMainAxisIsU = bMainU;
	}

	for (const TArray<int32>& Card : Cards)
	{
		GroupCard(Card, Indices, UVs, GroupsU, GroupsV);
		TArray<FSimilarGroup>& Groups = bMainU ? GroupsU : GroupsV;
		Groups.Sort([](const FSimilarGroup& A, const FSimilarGroup& B) { return A.TexCoord < B.TexCoord; });

		float TotalLength = 0.0f;
		FVector3f Previous = FVector3f::ZeroVector;
		for (int32 GroupIndex = 0; GroupIndex < Groups.Num(); ++GroupIndex)
		{
			// The group's centre over distinct positions, as the engine averages mesh-description vertices.
			TArray<int32> Distinct;
			for (const int32 Vertex : Groups[GroupIndex].Vertices)
			{
				Distinct.AddUnique(Welded[Vertex]);
			}
			FVector3f Centre = FVector3f::ZeroVector;
			for (const int32 Vertex : Distinct)
			{
				Centre += Positions[Vertex];
			}
			Centre /= static_cast<float>(FMath::Max(1, Distinct.Num()));
			if (GroupIndex > 0)
			{
				TotalLength += FVector3f::Distance(Centre, Previous);
			}
			for (const int32 Vertex : Groups[GroupIndex].Vertices)
			{
				CoordU[Vertex] = TotalLength;
			}
			Previous = Centre;
		}
		if (Groups.Num() > 1 && TotalLength > 0.0f)
		{
			for (const FSimilarGroup& Group : Groups)
			{
				for (const int32 Vertex : Group.Vertices)
				{
					CoordU[Vertex] = FMath::Clamp(CoordU[Vertex] / TotalLength, 0.0f, 1.0f);
				}
			}
		}
		else
		{
			for (const FSimilarGroup& Group : Groups)
			{
				for (const int32 Vertex : Group.Vertices)
				{
					CoordU[Vertex] = 0.0f;
				}
			}
		}
	}
	return CoordU;
}
