// Copyright Joshua Eger. All Rights Reserved.
#include "MantleGarmentSkinTransfer.h"

#include "MantleGarmentClearance.h"
#include "MantleGarmentGeometry.h"
#include "MantleRuntime.h"
#include "Async/ParallelFor.h"
#include "GPUSkinPublicDefs.h"

// Weights are MSK1's 8-bit values summing to 255. Named, not anonymous: MantleRuntime builds as a
// unity build.
namespace MantleGarmentSkinTransferPrivate
{
	/** A vertex's 8-bit weights sum to this (255). */
	constexpr int32 FullWeight = 255;

	/** Cell of the tracked-vertex grid an untracked vertex searches for its nearest tracked one, and the rings before brute force (20 rings = 1 m). */
	constexpr float NearestCellCm = 5.0f;
	constexpr int32 NearestMaxRing = 20;

	/** One skin influence in the float domain (weight 0..1). */
	struct FGarmentInfluence
	{
		int32 Bone;
		float Weight;
	};

	/** A blend of several vertices' influences (three triangle corners, a point and its neighbours). */
	using FGarmentInfluenceBlend = TArray<FGarmentInfluence, TInlineAllocator<3 * MAX_TOTAL_INFLUENCES>>;

	/** One vertex's (or welded point's) influences. */
	using FGarmentInfluenceList = TArray<FGarmentInfluence, TInlineAllocator<MAX_TOTAL_INFLUENCES>>;

	/** Adds Weight to Bone's entry of List (or appends one). */
	template <typename AllocatorType>
	void AddInfluence(TArray<FGarmentInfluence, AllocatorType>& List, const int32 Bone, const float Weight)
	{
		if (FGarmentInfluence* Found = List.FindByPredicate([Bone](const FGarmentInfluence& Entry) { return Entry.Bone == Bone; }))
		{
			Found->Weight += Weight;
		}
		else
		{
			List.Add({ Bone, Weight });
		}
	}

	/** Largest weight first (lower bone first on ties), then only the first Keep kept. Returns the kept weights' sum. */
	template <typename AllocatorType>
	float SortAndTrim(TArray<FGarmentInfluence, AllocatorType>& List, const int32 Keep)
	{
		List.Sort([](const FGarmentInfluence& A, const FGarmentInfluence& B) { return A.Weight > B.Weight || (A.Weight == B.Weight && A.Bone < B.Bone); });
		if (List.Num() > Keep)
		{
			List.SetNum(Keep);
		}
		float Sum = 0.0f;
		for (const FGarmentInfluence& Entry : List)
		{
			Sum += Entry.Weight;
		}
		return Sum;
	}

	/** Vertex's slots as a list: weight / 255, zero weights skipped, a bone in several slots merged. */
	template <typename AllocatorType>
	void GatherInfluences(const FMantleSkinnedMeshLOD& LOD, const int32 Vertex, TArray<FGarmentInfluence, AllocatorType>& Out)
	{
		Out.Reset();
		const int32 Slots = LOD.NumInfluences;
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			const uint8 Weight = LOD.InfluenceWeights[Vertex * Slots + Slot];
			if (Weight > 0)
			{
				AddInfluence(Out, static_cast<int32>(LOD.InfluenceBones[Vertex * Slots + Slot]), static_cast<float>(Weight) / static_cast<float>(FullWeight));
			}
		}
	}

	/** Bone of Vertex's heaviest slot (first on ties), INDEX_NONE without weight. */
	int32 DominantBone(TConstArrayView<uint16> Bones, TConstArrayView<uint8> Weights, const int32 Base, const int32 Slots)
	{
		int32 Best = INDEX_NONE;
		uint8 BestWeight = 0;
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			if (Weights[Base + Slot] > BestWeight)
			{
				BestWeight = Weights[Base + Slot];
				Best = Bones[Base + Slot];
			}
		}
		return Best;
	}

	/**
	 * Writes a sorted, trimmed List (Sum = its total, > 0) into one vertex's Slots, quantised so the
	 * weights sum exactly to FullWeight: floors first, then the remainder one step at a time to the
	 * largest fractions. Unused slots get bone 0, weight 0. Returns the used slot count.
	 */
	template <typename AllocatorType>
	int32 WriteQuantised(const TArray<FGarmentInfluence, AllocatorType>& List, const float Sum, const int32 Slots, uint16* OutBones, uint8* OutWeights)
	{
		const int32 Num = FMath::Min3(List.Num(), static_cast<int32>(MAX_TOTAL_INFLUENCES), Slots);
		int32 Quantised[MAX_TOTAL_INFLUENCES] = {};
		float Fraction[MAX_TOTAL_INFLUENCES] = {};
		int32 Assigned = 0;
		for (int32 Index = 0; Index < Num; ++Index)
		{
			const float Exact = List[Index].Weight / Sum * static_cast<float>(FullWeight);
			Quantised[Index] = FMath::Clamp(FMath::FloorToInt32(Exact), 0, FullWeight);
			Fraction[Index] = Exact - static_cast<float>(Quantised[Index]);
			Assigned += Quantised[Index];
		}
		for (int32 Left = FullWeight - Assigned; Left > 0 && Num > 0; --Left)
		{
			int32 Pick = 0;
			for (int32 Index = 1; Index < Num; ++Index)
			{
				Pick = Fraction[Index] > Fraction[Pick] ? Index : Pick;
			}
			++Quantised[Pick];
			Fraction[Pick] -= 1.0f;
		}
		for (int32 Over = Assigned - FullWeight; Over > 0 && Num > 0; --Over)
		{
			// Float round-off only: the excess comes off the largest weight.
			--Quantised[0];
		}
		int32 Used = 0;
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			const bool bUsed = Slot < Num && Quantised[Slot] > 0;
			OutBones[Slot] = bUsed ? static_cast<uint16>(List[Slot].Bone) : static_cast<uint16>(0);
			OutWeights[Slot] = bUsed ? static_cast<uint8>(Quantised[Slot]) : static_cast<uint8>(0);
			Used += bUsed ? 1 : 0;
		}
		return Used;
	}

	/** Skin arrays sized for the vertex count, a sane slot count, and triangle indices in range. */
	bool SkinDataOk(const FMantleSkinnedMeshLOD& LOD)
	{
		const int32 N = LOD.NumVertices();
		const int32 Slots = LOD.NumInfluences;
		return N > 0 && Slots >= 1 && Slots <= MAX_TOTAL_INFLUENCES && LOD.InfluenceBones.Num() == N * Slots && LOD.InfluenceWeights.Num() == N * Slots
			&& MantleGarmentGeometry::IndicesInRange(LOD.Indices, N);
	}

	/** Largest per-bone |new - old| weight of one vertex (0..1). */
	double LargestChange(const FGarmentInfluenceList& Old, const FGarmentInfluenceList& New)
	{
		double Largest = 0.0;
		for (const FGarmentInfluence& Entry : New)
		{
			const FGarmentInfluence* Before = Old.FindByPredicate([&Entry](const FGarmentInfluence& Other) { return Other.Bone == Entry.Bone; });
			Largest = FMath::Max(Largest, FMath::Abs(static_cast<double>(Entry.Weight) - (Before ? static_cast<double>(Before->Weight) : 0.0)));
		}
		for (const FGarmentInfluence& Before : Old)
		{
			if (!New.ContainsByPredicate([&Before](const FGarmentInfluence& Other) { return Other.Bone == Before.Bone; }))
			{
				Largest = FMath::Max(Largest, static_cast<double>(Before.Weight));
			}
		}
		return Largest;
	}

	/** Face shares below half an 8-bit weight step are none. */
	constexpr float MinFaceShare = 0.5f / static_cast<float>(FullWeight);

	/**
	 * Per garment vertex the face's share (0..1) and the vertex whose closest face point it samples the
	 * face's weights at (INDEX_NONE: no share). The raw share, dB / (dB + dF) faded over the face's last
	 * FaceFadeCm (1 where only the face tracks), is smoothed over the welded points' edges as S, the share
	 * of the face-tracked points (0 elsewhere), and C, the face-tracked fraction; the share is S / max(C,
	 * FaceShareMinCoverage), so the holes the face's untracked boundary leaves fill from their neighbours,
	 * and the share fades out evenly where the face-tracked points thin out. A point the face does not
	 * track samples the face at the nearest one (in edges) it does.
	 */
	void FaceShares(const FMantleSkinnedMeshLOD& Garment, const FMantleGarmentClearance& Authored, const FMantleGarmentClearance& Face, TArray<float>& OutShare, TArray<int32>& OutSource)
	{
		using namespace MantleGarmentGeometry;

		const int32 N = Garment.NumVertices();
		OutShare.Init(0.0f, N);
		OutSource.Init(INDEX_NONE, N);
		TArray<int32> Sections;
		VertexSections(Garment, Sections);
		TArray<int32> Weld;
		const int32 NumPoints = WeldBySection(Garment.Positions, Sections, Weld);
		FPointNeighbours Neighbours;
		PointNeighbours(Garment.Indices, Weld, NumPoints, Neighbours);

		// Per point the copies' mean raw share and face-tracked fraction, and a copy the face tracks.
		TArray<float> Share;
		Share.Init(0.0f, NumPoints);
		TArray<float> Coverage;
		Coverage.Init(0.0f, NumPoints);
		TArray<int32> Copies;
		Copies.Init(0, NumPoints);
		TArray<int32> Source;
		Source.Init(INDEX_NONE, NumPoints);
		for (int32 Vertex = 0; Vertex < N; ++Vertex)
		{
			const int32 Point = Weld[Vertex];
			++Copies[Point];
			if (Face.Triangle[Vertex] == INDEX_NONE)
			{
				continue;
			}
			const float FaceCm = FMath::Abs(Face.ClearanceCm[Vertex]);
			const float Fade = FMath::Clamp((FMantleGarmentClearance::FaceSearchCm - FaceCm) / FMantleGarmentSkinTransfer::FaceFadeCm, 0.0f, 1.0f);
			const float BodyCm = FMath::Max(Authored.ClearanceCm.IsValidIndex(Vertex) ? FMath::Abs(Authored.ClearanceCm[Vertex]) : 0.0f, FMantleGarmentSkinTransfer::MinShareDistanceCm);
			Share[Point] += Authored.Triangle[Vertex] == INDEX_NONE ? 1.0f : Fade * BodyCm / (BodyCm + FMath::Max(FaceCm, FMantleGarmentSkinTransfer::MinShareDistanceCm));
			Coverage[Point] += 1.0f;
			Source[Point] = Source[Point] == INDEX_NONE ? Vertex : Source[Point];
		}
		for (int32 Point = 0; Point < NumPoints; ++Point)
		{
			Share[Point] /= static_cast<float>(FMath::Max(Copies[Point], 1));
			Coverage[Point] /= static_cast<float>(FMath::Max(Copies[Point], 1));
		}

		// Passes: half own, half the neighbours' mean, both fields.
		TArray<float> NextShare;
		NextShare.SetNumUninitialized(NumPoints);
		TArray<float> NextCoverage;
		NextCoverage.SetNumUninitialized(NumPoints);
		for (int32 Pass = 0; Pass < FMantleGarmentSkinTransfer::FaceShareSmoothIters; ++Pass)
		{
			ParallelFor(NumPoints, [&Neighbours, &Share, &Coverage, &NextShare, &NextCoverage](const int32 Point)
			{
				const TArray<int32, TInlineAllocator<8>>& Around = Neighbours[Point];
				float AroundShare = 0.0f;
				float AroundCoverage = 0.0f;
				for (const int32 Other : Around)
				{
					AroundShare += Share[Other];
					AroundCoverage += Coverage[Other];
				}
				const float Inv = Around.Num() > 0 ? 0.5f / static_cast<float>(Around.Num()) : 0.0f;
				const float Own = Around.Num() > 0 ? 0.5f : 1.0f;
				NextShare[Point] = Own * Share[Point] + Inv * AroundShare;
				NextCoverage[Point] = Own * Coverage[Point] + Inv * AroundCoverage;
			});
			Swap(Share, NextShare);
			Swap(Coverage, NextCoverage);
		}

		// Points the face does not track take the face sample of the nearest one that it does, as far as the passes reach.
		TArray<int32> Frontier;
		for (int32 Point = 0; Point < NumPoints; ++Point)
		{
			if (Source[Point] != INDEX_NONE)
			{
				Frontier.Add(Point);
			}
		}
		for (int32 Ring = 0; Ring < FMantleGarmentSkinTransfer::FaceShareSmoothIters && Frontier.Num() > 0; ++Ring)
		{
			TArray<int32> Next;
			for (const int32 Point : Frontier)
			{
				for (const int32 Other : Neighbours[Point])
				{
					if (Source[Other] == INDEX_NONE)
					{
						Source[Other] = Source[Point];
						Next.Add(Other);
					}
				}
			}
			Frontier = MoveTemp(Next);
		}

		for (int32 Vertex = 0; Vertex < N; ++Vertex)
		{
			const int32 Point = Weld[Vertex];
			const bool bFaceTracked = Face.Triangle[Vertex] != INDEX_NONE;
			// Only the face tracks it: there are no body weights to mix with. Neither does: the nearest tracked vertex's, later.
			const float VertexShare = Authored.Triangle[Vertex] == INDEX_NONE ? (bFaceTracked ? 1.0f : 0.0f)
				: FMath::Min(Share[Point] / FMath::Max(Coverage[Point], FMantleGarmentSkinTransfer::FaceShareMinCoverage), 1.0f);
			if (VertexShare >= MinFaceShare && Source[Point] != INDEX_NONE)
			{
				OutShare[Vertex] = VertexShare;
				OutSource[Vertex] = bFaceTracked ? Vertex : Source[Point];
			}
		}
	}
}

bool FMantleGarmentSkinTransfer::Transfer(FMantleSkinnedMeshLOD& Garment, const TConstArrayView<FMantleSkinnedMeshBone> GarmentBones, const FMantleGarmentClearance& Authored,
	const FMantleSkinnedMesh& Wearer, const FMantleSkinTransferFace& Face, FMantleSkinTransferStats* OutStats, FString* OutError, TArray<float>* OutFaceShare)
{
	using namespace MantleGarmentSkinTransferPrivate;

	const double Start = MantleGarmentGeometry::NowMs();
	const int32 N = Garment.NumVertices();
	const int32 OldSlots = Garment.NumInfluences;
	const FMantleSkinnedMeshLOD* WearerLOD = Wearer.LODs.Num() > 0 ? &Wearer.LODs[0] : nullptr;
	const int32 NumWearerVertices = WearerLOD ? WearerLOD->NumVertices() : 0;
	const int32 WearerSlots = WearerLOD ? WearerLOD->NumInfluences : 0;
	const int32 NumWearerBones = Wearer.Bones.Num();
	if (!SkinDataOk(Garment) || Authored.Triangle.Num() != N || Authored.Barycentrics.Num() != N || !WearerLOD || NumWearerVertices != Authored.NumSurfaceVertices
		|| WearerSlots < 1 || WearerLOD->InfluenceBones.Num() != NumWearerVertices * WearerSlots || WearerLOD->InfluenceWeights.Num() != NumWearerVertices * WearerSlots
		|| GarmentBones.Num() == 0 || NumWearerBones == 0)
	{
		return MantleGarmentGeometry::RefuseFit(TEXT("skin transfer"), FString::Printf(TEXT("bad inputs (garment %d vertices, %d authored entries, wearer %d vertices vs the authoring body's %d, slots %d / %d, bones %d / %d)"),
			N, Authored.Triangle.Num(), NumWearerVertices, Authored.NumSurfaceVertices, OldSlots, WearerSlots, GarmentBones.Num(), NumWearerBones), OutError);
	}
	const bool bFace = Face.IsSet();
	const FMantleSkinnedMeshLOD* FaceLOD = bFace ? Face.Surface : nullptr;
	const FMantleGarmentClearance* FaceClearance = bFace ? Face.Clearance : nullptr;
	const int32 FaceSlots = FaceLOD ? FaceLOD->NumInfluences : 0;
	if (bFace && (FaceSlots < 1 || FaceSlots > MAX_TOTAL_INFLUENCES || FaceLOD->InfluenceBones.Num() != FaceLOD->NumVertices() * FaceSlots
		|| FaceLOD->InfluenceWeights.Num() != FaceLOD->NumVertices() * FaceSlots || FaceClearance->Triangle.Num() != N || FaceClearance->Barycentrics.Num() != N
		|| FaceClearance->ClearanceCm.Num() != N || FaceClearance->NumSurfaceVertices != FaceLOD->NumVertices() || FaceClearance->SurfaceIndices.Num() != FaceLOD->Indices.Num()))
	{
		return MantleGarmentGeometry::RefuseFit(TEXT("skin transfer"), FString::Printf(TEXT("bad face inputs (face %d vertices, %d slots; face clearance %d entries over %d vertices for %d garment vertices)"),
			FaceLOD->NumVertices(), FaceSlots, FaceClearance->Triangle.Num(), FaceClearance->NumSurfaceVertices, N), OutError);
	}

	// Wearer bone -> garment bone, by name unless the tables are the same (names in order).
	bool bSameTable = NumWearerBones == GarmentBones.Num();
	for (int32 Bone = 0; bSameTable && Bone < NumWearerBones; ++Bone)
	{
		bSameTable = Wearer.Bones[Bone].Name == GarmentBones[Bone].Name;
	}
	TMap<FName, int32> GarmentIndex;
	GarmentIndex.Reserve(GarmentBones.Num());
	for (int32 Bone = 0; Bone < GarmentBones.Num(); ++Bone)
	{
		GarmentIndex.Add(GarmentBones[Bone].Name, Bone);
	}
	TArray<int32> ToGarment;
	ToGarment.Init(INDEX_NONE, NumWearerBones);
	for (int32 Bone = 0; Bone < NumWearerBones; ++Bone)
	{
		const int32* Found = bSameTable ? nullptr : GarmentIndex.Find(Wearer.Bones[Bone].Name);
		ToGarment[Bone] = bSameTable ? Bone : (Found ? *Found : INDEX_NONE);
	}

	// Face bone -> garment bone: by name, else the nearest face ancestor the garment has (the face's own
	// neck joints ride on neck_01 / neck_02 / head, which the garment's table has).
	TArray<int32> FaceToGarment;
	FaceToGarment.Init(INDEX_NONE, bFace ? Face.Bones.Num() : 0);
	for (int32 Bone = 0; Bone < FaceToGarment.Num(); ++Bone)
	{
		for (int32 Ancestor = Bone, Steps = 0; Face.Bones.IsValidIndex(Ancestor) && Steps < Face.Bones.Num(); Ancestor = Face.Bones[Ancestor].ParentIndex, ++Steps)
		{
			if (const int32* Found = GarmentIndex.Find(Face.Bones[Ancestor].Name))
			{
				FaceToGarment[Bone] = *Found;
				break;
			}
		}
	}

	// The slots grow to the skin's count (never below the garment's own): a one-slot garment takes the skin's blend whole.
	const int32 Slots = FMath::Max(OldSlots, FMath::Min(FMath::Max(WearerSlots, FaceSlots), FMath::Min(MaxInfluences, static_cast<int32>(MAX_TOTAL_INFLUENCES))));
	const int32 Keep = FMath::Min(Slots, MaxInfluences);
	const TArray<uint16> OldBones = Garment.InfluenceBones;
	const TArray<uint8> OldWeights = Garment.InfluenceWeights;
	if (Slots != OldSlots)
	{
		Garment.NumInfluences = Slots;
		Garment.InfluenceBones.SetNumZeroed(N * Slots);
		Garment.InfluenceWeights.SetNumZeroed(N * Slots);
		for (int32 Vertex = 0; Vertex < N; ++Vertex)
		{
			for (int32 Slot = 0; Slot < Slots; ++Slot)
			{
				Garment.InfluenceBones[Vertex * Slots + Slot] = Slot < OldSlots ? OldBones[Vertex * OldSlots + Slot] : static_cast<uint16>(0);
				Garment.InfluenceWeights[Vertex * Slots + Slot] = Slot < OldSlots ? OldWeights[Vertex * OldSlots + Slot] : static_cast<uint8>(0);
			}
		}
	}

	enum : uint8 { OriginKept, OriginBody, OriginNearest };
	TArray<uint8> Origin;
	Origin.Init(OriginKept, N);
	TArray<uint8> TookFace;
	TookFace.Init(0, N);
	TArray<int32> Dropped;
	Dropped.SetNumZeroed(N);

	// Adds one skin vertex's influences scaled by Scale, through Map (skin bone -> garment bone); unmapped ones are counted.
	auto AddSkin = [](FGarmentInfluenceBlend& Blend, const FMantleSkinnedMeshLOD& Skin, const int32 SkinVertex, const float Scale, const TArray<int32>& Map, int32& OutDropped)
	{
		const int32 SkinSlots = Skin.NumInfluences;
		for (int32 Slot = 0; Slot < SkinSlots; ++Slot)
		{
			const uint8 SkinWeight = Skin.InfluenceWeights[SkinVertex * SkinSlots + Slot];
			if (SkinWeight == 0)
			{
				continue;
			}
			const int32 SkinBone = Skin.InfluenceBones[SkinVertex * SkinSlots + Slot];
			const int32 GarmentBone = Map.IsValidIndex(SkinBone) ? Map[SkinBone] : INDEX_NONE;
			if (GarmentBone == INDEX_NONE)
			{
				++OutDropped;
				continue;
			}
			AddInfluence(Blend, GarmentBone, Scale * static_cast<float>(SkinWeight) / static_cast<float>(FullWeight));
		}
	};

	// (1) The face's share of each vertex, smoothed over the garment's mesh (FaceShares): from each vertex's
	// own distances it jumps around a collar, and neighbours with different neck / chest mixes make the rim
	// wavy once the neck bends.
	TArray<float> FaceShareOf;
	TArray<int32> FaceSampleOf;
	if (bFace)
	{
		FaceShares(Garment, Authored, *FaceClearance, FaceShareOf, FaceSampleOf);
	}
	else
	{
		FaceShareOf.Init(0.0f, N);
		FaceSampleOf.Init(INDEX_NONE, N);
	}

	// (2) Tracked vertices: blend the skin's weights at the closest point, the body's at the authored one and,
	// within the face's reach, the face's by its share (the collar sits over the face's neck, not the
	// body's). Each vertex writes only its own slots.
	ParallelFor(N, [&](const int32 Vertex)
	{
		const int32 T = Authored.Triangle[Vertex];
		const int32 FaceSample = FaceSampleOf[Vertex];
		const float FaceShare = FaceSample != INDEX_NONE ? FaceShareOf[Vertex] : 0.0f;
		if (T == INDEX_NONE && FaceShare <= 0.0f)
		{
			return;
		}
		FGarmentInfluenceBlend Blend;
		if (T != INDEX_NONE && FaceShare < 1.0f)
		{
			const FVector3f& W = Authored.Barycentrics[Vertex];
			const float Bary[3] = { W.X, W.Y, W.Z };
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				if (Bary[Corner] > 0.0f)
				{
					AddSkin(Blend, *WearerLOD, static_cast<int32>(Authored.SurfaceIndices[T * 3 + Corner]), (1.0f - FaceShare) * Bary[Corner], ToGarment, Dropped[Vertex]);
				}
			}
		}
		if (FaceShare > 0.0f)
		{
			const int32 FT = FaceClearance->Triangle[FaceSample];
			const FVector3f& W = FaceClearance->Barycentrics[FaceSample];
			const float Bary[3] = { W.X, W.Y, W.Z };
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				if (Bary[Corner] > 0.0f)
				{
					AddSkin(Blend, *FaceLOD, static_cast<int32>(FaceClearance->SurfaceIndices[FT * 3 + Corner]), FaceShare * Bary[Corner], FaceToGarment, Dropped[Vertex]);
				}
			}
		}
		const float Sum = SortAndTrim(Blend, Keep);
		if (Sum <= UE_SMALL_NUMBER)
		{
			return;
		}
		WriteQuantised(Blend, Sum, Slots, &Garment.InfluenceBones[Vertex * Slots], &Garment.InfluenceWeights[Vertex * Slots]);
		Origin[Vertex] = OriginBody;
		TookFace[Vertex] = FaceShare > 0.0f ? 1 : 0;
	});

	// (3) Untracked vertices (loose cloth past the clearance search): the new weights of the nearest
	// tracked vertex (fitted positions), ring by ring in a grid, brute force past the last ring.
	TArray<int32> Tracked;
	for (int32 Vertex = 0; Vertex < N; ++Vertex)
	{
		if (Origin[Vertex] == OriginBody)
		{
			Tracked.Add(Vertex);
		}
	}
	if (Tracked.Num() > 0)
	{
		const float Inv = 1.0f / NearestCellCm;
		auto CellOf = [Inv](const FVector3f& P)
		{
			return FIntVector(FMath::FloorToInt32(P.X * Inv), FMath::FloorToInt32(P.Y * Inv), FMath::FloorToInt32(P.Z * Inv));
		};
		TMap<FIntVector, TArray<int32>> Cells;
		for (const int32 Vertex : Tracked)
		{
			Cells.FindOrAdd(CellOf(Garment.Positions[Vertex])).Add(Vertex);
		}
		TArray<int32> Nearest;
		Nearest.Init(INDEX_NONE, N);
		ParallelFor(N, [&](const int32 Vertex)
		{
			if (Authored.Triangle[Vertex] != INDEX_NONE || Origin[Vertex] == OriginBody)
			{
				return;
			}
			const FVector3f P = Garment.Positions[Vertex];
			const FIntVector Home = CellOf(P);
			float BestSq = TNumericLimits<float>::Max();
			int32 Best = INDEX_NONE;
			for (int32 Ring = 0; Ring <= NearestMaxRing; ++Ring)
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
							if (const TArray<int32>* List = Cells.Find(Home + FIntVector(DX, DY, DZ)))
							{
								for (const int32 Other : *List)
								{
									const float DistSq = FVector3f::DistSquared(P, Garment.Positions[Other]);
									if (DistSq < BestSq)
									{
										BestSq = DistSq;
										Best = Other;
									}
								}
							}
						}
					}
				}
				// Every tracked vertex not seen yet lies at least Ring cells away.
				if (Best != INDEX_NONE && BestSq <= FMath::Square(Ring * NearestCellCm))
				{
					break;
				}
			}
			if (Best == INDEX_NONE)
			{
				for (const int32 Other : Tracked)
				{
					const float DistSq = FVector3f::DistSquared(P, Garment.Positions[Other]);
					if (DistSq < BestSq)
					{
						BestSq = DistSq;
						Best = Other;
					}
				}
			}
			Nearest[Vertex] = Best;
		});
		for (int32 Vertex = 0; Vertex < N; ++Vertex)
		{
			if (Nearest[Vertex] != INDEX_NONE)
			{
				for (int32 Slot = 0; Slot < Slots; ++Slot)
				{
					Garment.InfluenceBones[Vertex * Slots + Slot] = Garment.InfluenceBones[Nearest[Vertex] * Slots + Slot];
					Garment.InfluenceWeights[Vertex * Slots + Slot] = Garment.InfluenceWeights[Nearest[Vertex] * Slots + Slot];
				}
				Origin[Vertex] = OriginNearest;
			}
		}
	}

	// (4) The slots cut to the most any vertex uses, each vertex's weights packed first in their order (the
	// skin's 12 slots on a garment whose blends use 6 would only cost the GPU skin).
	int32 Used = 1;
	for (int32 Vertex = 0; Vertex < N; ++Vertex)
	{
		int32 VertexUsed = 0;
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			VertexUsed += Garment.InfluenceWeights[Vertex * Slots + Slot] > 0 ? 1 : 0;
		}
		Used = FMath::Max(Used, VertexUsed);
	}
	if (Used < Slots)
	{
		TArray<uint16> Bones;
		TArray<uint8> Weights;
		Bones.SetNumZeroed(N * Used);
		Weights.SetNumZeroed(N * Used);
		for (int32 Vertex = 0; Vertex < N; ++Vertex)
		{
			int32 To = 0;
			for (int32 Slot = 0; Slot < Slots; ++Slot)
			{
				if (Garment.InfluenceWeights[Vertex * Slots + Slot] > 0)
				{
					Bones[Vertex * Used + To] = Garment.InfluenceBones[Vertex * Slots + Slot];
					Weights[Vertex * Used + To] = Garment.InfluenceWeights[Vertex * Slots + Slot];
					++To;
				}
			}
		}
		Garment.NumInfluences = Used;
		Garment.InfluenceBones = MoveTemp(Bones);
		Garment.InfluenceWeights = MoveTemp(Weights);
	}
	const int32 NewSlots = Garment.NumInfluences;

	// (5) Stats: per changed vertex the largest per-bone change and whether the dominant bone moved.
	FMantleSkinTransferStats Stats;
	double SumChange = 0.0;
	FGarmentInfluenceList Old;
	FGarmentInfluenceList New;
	FMantleSkinnedMeshLOD OldLOD;
	OldLOD.NumInfluences = OldSlots;
	OldLOD.InfluenceBones = OldBones;
	OldLOD.InfluenceWeights = OldWeights;
	for (int32 Vertex = 0; Vertex < N; ++Vertex)
	{
		Stats.DroppedInfluences += Dropped[Vertex];
		if (Origin[Vertex] == OriginKept)
		{
			++Stats.Kept;
			continue;
		}
		Stats.FromBody += Origin[Vertex] == OriginBody ? 1 : 0;
		Stats.FromFace += TookFace[Vertex];
		Stats.FromNearest += Origin[Vertex] == OriginNearest ? 1 : 0;
		GatherInfluences(OldLOD, Vertex, Old);
		GatherInfluences(Garment, Vertex, New);
		const double Largest = LargestChange(Old, New);
		SumChange += Largest;
		Stats.MaxChange = FMath::Max(Stats.MaxChange, Largest);
		Stats.DominantChanged += DominantBone(OldBones, OldWeights, Vertex * OldSlots, OldSlots) != DominantBone(Garment.InfluenceBones, Garment.InfluenceWeights, Vertex * NewSlots, NewSlots) ? 1 : 0;
	}
	const int32 Changed = Stats.FromBody + Stats.FromNearest;
	Stats.bRan = true;
	Stats.SlotsBefore = OldSlots;
	Stats.Influences = NewSlots;
	Stats.MeanChange = Changed > 0 ? SumChange / Changed : 0.0;
	Stats.Ms = MantleGarmentGeometry::NowMs() - Start;
	UE_LOG(LogMantle, Verbose, TEXT("MANTLE_GARMENTFIT skin transfer: %d from the skin (%d with the face's weights), %d from the nearest tracked vertex, %d kept; slots %d -> %d, bone tables %s; change mean %.3f max %.3f, dominant changed %d, dropped %d; %.2f ms"),
		Stats.FromBody, Stats.FromFace, Stats.FromNearest, Stats.Kept, OldSlots, NewSlots, bSameTable ? TEXT("identical") : TEXT("mapped by name"), Stats.MeanChange, Stats.MaxChange,
		Stats.DominantChanged, Stats.DroppedInfluences, Stats.Ms);
	if (OutStats)
	{
		*OutStats = Stats;
	}
	if (OutFaceShare)
	{
		*OutFaceShare = MoveTemp(FaceShareOf);
	}
	return true;
}

void FMantleGarmentSkinTransfer::SmoothFactor(const FMantleGarmentClearance& Authored, const float TightCm, const float LooseCm, TArray<float>& Out, const TArray<float>* FaceShare)
{
	const int32 N = Authored.ClearanceCm.Num();
	Out.SetNumUninitialized(N);
	const float Span = FMath::Max(LooseCm - TightCm, 1.0e-3f);
	const bool bFaceShare = FaceShare && FaceShare->Num() == N;
	for (int32 Vertex = 0; Vertex < N; ++Vertex)
	{
		Out[Vertex] = Authored.IsTracked(Vertex) ? FMath::Clamp((Authored.ClearanceCm[Vertex] - TightCm) / Span, 0.0f, 1.0f) : 1.0f;
		if (bFaceShare)
		{
			// The collar: the face's and the body's weights meet under it, smoothed even where it lies on the skin.
			Out[Vertex] = FMath::Max(Out[Vertex], FaceSmoothFactor * FMath::Clamp((*FaceShare)[Vertex] / FaceSmoothFullShare, 0.0f, 1.0f));
		}
	}
}

bool FMantleGarmentSkinTransfer::Smooth(FMantleSkinnedMeshLOD& Garment, const int32 Iterations, const TArray<float>* Factor, FMantleSkinSmoothStats* OutStats)
{
	using namespace MantleGarmentSkinTransferPrivate;
	using namespace MantleGarmentGeometry;

	if (Iterations <= 0)
	{
		return true;
	}
	const double Start = NowMs();
	if (!SkinDataOk(Garment))
	{
		return MantleGarmentGeometry::RefuseFit(TEXT("skin smooth"), FString::Printf(TEXT("bad skin data (%d vertices, %d slots, %d bones, %d weights)"),
			Garment.NumVertices(), Garment.NumInfluences, Garment.InfluenceBones.Num(), Garment.InfluenceWeights.Num()), nullptr);
	}
	const int32 N = Garment.NumVertices();
	const int32 Slots = Garment.NumInfluences;
	const int32 Keep = FMath::Min(Slots, MaxInfluences);
	const int32 Passes = FMath::Min(Iterations, MaxSmoothIters);

	// Points: split copies welded within their section.
	TArray<int32> Sections;
	VertexSections(Garment, Sections);
	TArray<int32> Weld;
	const int32 NumPoints = WeldBySection(Garment.Positions, Sections, Weld);

	// Each point starts from its copies' weights (they normally agree already).
	TArray<FGarmentInfluenceList> Old;
	Old.SetNum(N);
	ParallelFor(N, [&Garment, &Old](const int32 Vertex)
	{
		GatherInfluences(Garment, Vertex, Old[Vertex]);
	});
	TArray<FGarmentInfluenceList> Current;
	Current.SetNum(NumPoints);
	TArray<int32> Copies;
	Copies.Init(0, NumPoints);
	for (int32 Vertex = 0; Vertex < N; ++Vertex)
	{
		for (const FGarmentInfluence& Entry : Old[Vertex])
		{
			AddInfluence(Current[Weld[Vertex]], Entry.Bone, Entry.Weight);
		}
		++Copies[Weld[Vertex]];
	}
	FMantleSkinSmoothStats Stats;
	for (int32 Point = 0; Point < NumPoints; ++Point)
	{
		Stats.SplitCopies += FMath::Max(Copies[Point] - 1, 0);
		const float Sum = SortAndTrim(Current[Point], Keep);
		for (FGarmentInfluence& Entry : Current[Point])
		{
			Entry.Weight = Sum > UE_SMALL_NUMBER ? Entry.Weight / Sum : 0.0f;
		}
	}

	// The smoothing factor per point (its copies' mean), 1 everywhere without Factor.
	TArray<float> PointFactor;
	PointFactor.Init(1.0f, NumPoints);
	const bool bFactor = Factor && Factor->Num() == N;
	if (Factor && !bFactor)
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_GARMENTFIT skin smooth: %d smoothing factors for %d vertices, ignored (full smoothing)"), Factor->Num(), N);
	}
	if (bFactor)
	{
		TArray<float> SumFactor;
		SumFactor.Init(0.0f, NumPoints);
		for (int32 Vertex = 0; Vertex < N; ++Vertex)
		{
			SumFactor[Weld[Vertex]] += FMath::Clamp((*Factor)[Vertex], 0.0f, 1.0f);
		}
		for (int32 Point = 0; Point < NumPoints; ++Point)
		{
			PointFactor[Point] = Copies[Point] > 0 ? SumFactor[Point] / Copies[Point] : 1.0f;
			Stats.FadedPoints += PointFactor[Point] < 1.0f ? 1 : 0;
			Stats.ExactPoints += PointFactor[Point] <= 0.0f ? 1 : 0;
		}
	}

	FPointNeighbours Neighbours;
	PointNeighbours(Garment.Indices, Weld, NumPoints, Neighbours);

	// Passes: (1 - f/2) own + f/2 neighbour mean over the union of bones; top Keep kept, renormalised.
	TArray<FGarmentInfluenceList> Next;
	Next.SetNum(NumPoints);
	for (int32 Pass = 0; Pass < Passes; ++Pass)
	{
		ParallelFor(NumPoints, [&Neighbours, &PointFactor, &Current, &Next, Keep](const int32 Point)
		{
			const TArray<int32, TInlineAllocator<8>>& Around = Neighbours[Point];
			const float Fade = PointFactor[Point];
			if (Around.Num() == 0 || Fade <= 0.0f)
			{
				Next[Point] = Current[Point];
				return;
			}
			FGarmentInfluenceBlend Mix;
			for (const FGarmentInfluence& Entry : Current[Point])
			{
				AddInfluence(Mix, Entry.Bone, (1.0f - 0.5f * Fade) * Entry.Weight);
			}
			const float Share = 0.5f * Fade / static_cast<float>(Around.Num());
			for (const int32 Other : Around)
			{
				for (const FGarmentInfluence& Entry : Current[Other])
				{
					AddInfluence(Mix, Entry.Bone, Share * Entry.Weight);
				}
			}
			const float Sum = SortAndTrim(Mix, Keep);
			if (Sum <= UE_SMALL_NUMBER)
			{
				Next[Point] = Current[Point];
				return;
			}
			Next[Point].Reset();
			for (const FGarmentInfluence& Entry : Mix)
			{
				Next[Point].Add({ Entry.Bone, Entry.Weight / Sum });
			}
		});
		Swap(Current, Next);
	}

	// Quantise per point, then every copy takes its point's slots (a point without weight leaves its vertices as they were).
	TArray<uint16> PointBones;
	TArray<uint8> PointWeights;
	PointBones.SetNumZeroed(NumPoints * Slots);
	PointWeights.SetNumZeroed(NumPoints * Slots);
	TArray<uint8> PointOk;
	PointOk.Init(0, NumPoints);
	ParallelFor(NumPoints, [&Current, &PointBones, &PointWeights, &PointOk, Keep, Slots](const int32 Point)
	{
		FGarmentInfluenceList List = Current[Point];
		const float Sum = SortAndTrim(List, Keep);
		if (Sum > UE_SMALL_NUMBER)
		{
			WriteQuantised(List, Sum, Slots, &PointBones[Point * Slots], &PointWeights[Point * Slots]);
			PointOk[Point] = 1;
		}
	});
	const TArray<uint16> OldBones = Garment.InfluenceBones;
	const TArray<uint8> OldWeights = Garment.InfluenceWeights;
	for (int32 Vertex = 0; Vertex < N; ++Vertex)
	{
		const int32 Point = Weld[Vertex];
		if (!PointOk[Point])
		{
			continue;
		}
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			Garment.InfluenceBones[Vertex * Slots + Slot] = PointBones[Point * Slots + Slot];
			Garment.InfluenceWeights[Vertex * Slots + Slot] = PointWeights[Point * Slots + Slot];
		}
	}

	// Stats: per vertex the largest per-bone change and dominant bone changes.
	double SumChange = 0.0;
	FGarmentInfluenceList New;
	for (int32 Vertex = 0; Vertex < N; ++Vertex)
	{
		GatherInfluences(Garment, Vertex, New);
		const double Largest = LargestChange(Old[Vertex], New);
		SumChange += Largest;
		Stats.MaxChange = FMath::Max(Stats.MaxChange, Largest);
		Stats.DominantChanged += DominantBone(OldBones, OldWeights, Vertex * Slots, Slots) != DominantBone(Garment.InfluenceBones, Garment.InfluenceWeights, Vertex * Slots, Slots) ? 1 : 0;
	}
	Stats.bRan = true;
	Stats.Iterations = Passes;
	Stats.Points = NumPoints;
	Stats.MeanChange = N > 0 ? SumChange / N : 0.0;
	Stats.Ms = NowMs() - Start;
	if (OutStats)
	{
		*OutStats = Stats;
	}
	return true;
}

bool FMantleGarmentSkinTransfer::DominantMatch(const FMantleSkinnedMeshLOD& Garment, const TConstArrayView<FMantleSkinnedMeshBone> GarmentBones, const FMantleGarmentClearance& Authored,
	const FMantleSkinnedMesh& Wearer, int32& OutMatches, int32& OutTracked)
{
	using namespace MantleGarmentSkinTransferPrivate;

	OutMatches = 0;
	OutTracked = 0;
	const int32 N = Authored.Triangle.Num();
	const FMantleSkinnedMeshLOD* WearerLOD = Wearer.LODs.Num() > 0 ? &Wearer.LODs[0] : nullptr;
	if (!WearerLOD || Garment.NumVertices() != N || Authored.Barycentrics.Num() != N || WearerLOD->NumVertices() != Authored.NumSurfaceVertices
		|| Garment.InfluenceWeights.Num() != N * Garment.NumInfluences || Garment.InfluenceBones.Num() != N * Garment.NumInfluences
		|| WearerLOD->InfluenceWeights.Num() != WearerLOD->NumVertices() * WearerLOD->NumInfluences
		|| WearerLOD->InfluenceBones.Num() != WearerLOD->NumVertices() * WearerLOD->NumInfluences)
	{
		return false;
	}
	for (int32 Vertex = 0; Vertex < N; ++Vertex)
	{
		const int32 T = Authored.Triangle[Vertex];
		if (T == INDEX_NONE)
		{
			continue;
		}
		++OutTracked;
		const FVector3f& W = Authored.Barycentrics[Vertex];
		const int32 Corner = (W.X >= W.Y && W.X >= W.Z) ? 0 : (W.Y >= W.Z ? 1 : 2);
		const int32 BodyVertex = static_cast<int32>(Authored.SurfaceIndices[T * 3 + Corner]);
		const int32 BodyBone = DominantBone(WearerLOD->InfluenceBones, WearerLOD->InfluenceWeights, BodyVertex * WearerLOD->NumInfluences, WearerLOD->NumInfluences);
		const int32 GarmentBone = DominantBone(Garment.InfluenceBones, Garment.InfluenceWeights, Vertex * Garment.NumInfluences, Garment.NumInfluences);
		OutMatches += (Wearer.Bones.IsValidIndex(BodyBone) && GarmentBones.IsValidIndex(GarmentBone) && Wearer.Bones[BodyBone].Name == GarmentBones[GarmentBone].Name) ? 1 : 0;
	}
	return true;
}
