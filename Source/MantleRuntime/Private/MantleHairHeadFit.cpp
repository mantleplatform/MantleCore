// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHairHeadFit.h"
#include "MantleGarmentClearance.h"

namespace MantleHairHeadFitPrivate
{
	constexpr int32 GridSize = 64;
	/** Vertices within this card coordinate of a card's minimum count as its root (the root edge has several vertices). */
	constexpr float RootCoordTolerance = 1.e-3f;
	bool SkinSurface(const FMantleSkinnedMesh& Face, FMantleSkinnedMeshLOD& Out)
	{
		if (Face.LODs.IsEmpty()) return false;
		Out = Face.LODs[0]; Out.Indices.Reset(); Out.Sections.Reset();
		for (const auto& Section : Face.LODs[0].Sections)
		{
			const FString Name = Section.MaterialName.ToString().ToLower();
			if (!(Name.Contains(TEXT("skin")) || Name.Contains(TEXT("head"))) || Name.Contains(TEXT("eye")) || Name.Contains(TEXT("teeth")) || Name.Contains(TEXT("tongue")) || Name.Contains(TEXT("lash"))) continue;
			Out.Indices.Append(&Face.LODs[0].Indices[Section.FirstIndex], Section.IndexCount);
		}
		return !Out.Indices.IsEmpty();
	}
	int32 Cell(float V) { return FMath::Clamp(FMath::FloorToInt(V * GridSize), 0, GridSize - 1); }
	struct FUVEdgeKey
	{
		FVector2f A, B;
		bool operator==(const FUVEdgeKey& Other) const { return A == Other.A && B == Other.B; }
		friend uint32 GetTypeHash(const FUVEdgeKey& Edge)
		{
			// FVector2f's hash is found through ADL in some engine configurations. Explicit components
			// also normalize signed zero, matching float/vector equality across split UV vertices.
			auto ComponentHash = [](float Value) { return ::GetTypeHash(Value == 0.f ? 0.f : Value); };
			return HashCombine(HashCombine(ComponentHash(Edge.A.X),ComponentHash(Edge.A.Y)),
				HashCombine(ComponentHash(Edge.B.X),ComponentHash(Edge.B.Y)));
		}
	};
	struct FUVSurface
	{
		const FMantleSkinnedMeshLOD& Mesh;
		TMap<int32, TArray<int32>> Grid;
		struct FEdge { int32 Triangle = INDEX_NONE, Corner = 0, Count = 0; };
		TMap<FUVEdgeKey,FEdge> Edges;
		explicit FUVSurface(const FMantleSkinnedMeshLOD& In) : Mesh(In)
		{
			for (int32 T = 0; T < Mesh.NumTriangles(); ++T)
			{
				const FVector2f A = Mesh.UVs[Mesh.Indices[T * 3]], B = Mesh.UVs[Mesh.Indices[T * 3 + 1]], C = Mesh.UVs[Mesh.Indices[T * 3 + 2]];
				const FVector2f UVs[] = {A,B,C};
				if (FMath::Abs((B.X-A.X)*(C.Y-A.Y)-(B.Y-A.Y)*(C.X-A.X)) >= 1.e-10f)
					for (int32 K = 0; K < 3; ++K)
					{
						FVector2f U = UVs[K], V = UVs[(K+1)%3];
						if (U.X > V.X || (U.X == V.X && U.Y > V.Y)) Swap(U,V);
						FEdge& Edge = Edges.FindOrAdd({U,V}); Edge.Triangle = T; Edge.Corner = K; ++Edge.Count;
					}
				for (int32 Y = Cell(FMath::Min3(A.Y, B.Y, C.Y)); Y <= Cell(FMath::Max3(A.Y, B.Y, C.Y)); ++Y)
					for (int32 X = Cell(FMath::Min3(A.X, B.X, C.X)); X <= Cell(FMath::Max3(A.X, B.X, C.X)); ++X)
						Grid.FindOrAdd(Y * GridSize + X).Add(T);
			}
		}
		bool Find(const FVector2f UV, int32& Triangle, FVector3f& Bary) const
		{
			const TArray<int32>* Candidates = Grid.Find(Cell(UV.Y) * GridSize + Cell(UV.X));
			if (!Candidates) return false;
			for (int32 T : *Candidates)
			{
				const FVector2f A = Mesh.UVs[Mesh.Indices[T * 3]], B = Mesh.UVs[Mesh.Indices[T * 3 + 1]], C = Mesh.UVs[Mesh.Indices[T * 3 + 2]];
				const FVector2f E = B - A, F = C - A, P = UV - A;
				const float D = E.X * F.Y - E.Y * F.X;
				if (FMath::Abs(D) < 1.e-10f) continue;
				const float V = (P.X * F.Y - P.Y * F.X) / D, W = (E.X * P.Y - E.Y * P.X) / D;
				if (V >= -1.e-4f && W >= -1.e-4f && V + W <= 1.0001f)
				{
					Triangle = T; Bary = FVector3f(FMath::Max(0.f, 1.f - V - W), FMath::Max(0.f, V), FMath::Max(0.f, W));
					Bary /= Bary.X + Bary.Y + Bary.Z; return true;
				}
			}
			return false;
		}
		bool FindBoundary(const FVector2f UV, int32& Triangle, FVector3f& Bary, FVector2f& ProjectedUV) const
		{
			float Best = FMath::Square(FMantleHairHeadFit::MaxBoundaryUV); bool Found = false;
			for (const auto& Pair : Edges)
			{
				const FEdge& Edge = Pair.Value; if (Edge.Count != 1) continue;
				const int32 K = Edge.Corner, Next = (K+1)%3;
				const FVector2f A = Mesh.UVs[Mesh.Indices[Edge.Triangle*3+K]], B = Mesh.UVs[Mesh.Indices[Edge.Triangle*3+Next]], D = B-A;
				const float Length = D.SizeSquared(); if (Length <= 1.e-20f) continue;
				const float Along = FMath::Clamp(FVector2f::DotProduct(UV-A,D)/Length,0.f,1.f);
				const FVector2f Point = A+Along*D; const float Distance = (Point-UV).SizeSquared();
				if (Distance > Best || (Found && Distance == Best && Edge.Triangle >= Triangle)) continue;
				Best = Distance; Triangle = Edge.Triangle; Bary = FVector3f::ZeroVector; Bary[K] = 1.f-Along; Bary[Next] = Along;
				ProjectedUV = Point; Found = true;
			}
			return Found;
		}
	};
	FVector3f Point(const FMantleSkinnedMeshLOD& L, int32 T, FVector3f B)
	{
		return L.Positions[L.Indices[T * 3]] * B.X + L.Positions[L.Indices[T * 3 + 1]] * B.Y + L.Positions[L.Indices[T * 3 + 2]] * B.Z;
	}
	bool Frame(const FMantleSkinnedMeshLOD& L, int32 T, FVector3f& X, FVector3f& Y, FVector3f& Z)
	{
		const uint32 A = L.Indices[T * 3], B = L.Indices[T * 3 + 1], C = L.Indices[T * 3 + 2];
		const FVector3f E = L.Positions[B] - L.Positions[A], F = L.Positions[C] - L.Positions[A];
		const FVector2f U = L.UVs[B] - L.UVs[A], V = L.UVs[C] - L.UVs[A];
		const float D = U.X * V.Y - U.Y * V.X;
		if (FMath::Abs(D) < 1.e-10f) return false;
		X = ((E * V.Y - F * U.Y) / D).GetSafeNormal(); Z = FVector3f::CrossProduct(E,F).GetSafeNormal();
		if (FVector3f::DotProduct(Z,L.Normals[A]+L.Normals[B]+L.Normals[C]) < 0.f) Z = -Z;
		Y = FVector3f::CrossProduct(Z, X).GetSafeNormal();
		return !X.IsNearlyZero() && !Y.IsNearlyZero() && !Z.IsNearlyZero();
	}
	bool SmallSurfaceCorrection(const FMantleSkinnedMeshLOD& L, int32 T, const FVector2f DeltaUV)
	{
		const uint32 A = L.Indices[T*3], B = L.Indices[T*3+1], C = L.Indices[T*3+2];
		const FVector2f U = L.UVs[B]-L.UVs[A], V = L.UVs[C]-L.UVs[A]; const float D = U.X*V.Y-U.Y*V.X;
		if (FMath::Abs(D) < 1.e-10f) return false;
		const FVector3f E = L.Positions[B]-L.Positions[A], F = L.Positions[C]-L.Positions[A];
		const FVector3f Offset = ((E*V.Y-F*U.Y)*DeltaUV.X+(F*U.X-E*V.X)*DeltaUV.Y)/D;
		return Offset.SizeSquared() <= FMath::Square(FMantleHairHeadFit::MaxBoundaryCorrectionCm);
	}
	/**
	 * One pivot vertex per vertex's card. A card's pivot is its true root: the vertex with the lowest card
	 * coordinate (HairCards.Z, 0 at the root), among those within reach of the skin. Only when the mesh has no
	 * hair card data (or no root is in reach) does it fall back to the vertex nearest the skin.
	 */
	TArray<int32> CardRoots(const FMantleSkinnedMeshLOD& L, const FMantleSkinnedMeshLOD& Surface, const FMantleGarmentClearance& Near)
	{
		TArray<int32> Parent, Root; Parent.SetNumUninitialized(L.NumVertices());
		TMap<FVector3f, int32> Weld;
		for (int32 V = 0; V < L.NumVertices(); ++V) { const int32* P = Weld.Find(L.Positions[V]); Parent[V] = P ? *P : V; if (!P) Weld.Add(L.Positions[V], V); }
		auto Find = [&Parent](int32 V) { while (Parent[V] != V) { Parent[V] = Parent[Parent[V]]; V = Parent[V]; } return V; };
		for (int32 T = 0; T < L.Indices.Num(); T += 3) for (int32 K = 1; K < 3; ++K) { int32 A = Find(L.Indices[T]), B = Find(L.Indices[T + K]); Parent[B] = A; }
		const bool bCardData = L.HairCards.Num() == L.NumVertices();
		TMap<int32, float> MinCoord;
		if (bCardData)
			for (int32 V = 0; V < L.NumVertices(); ++V) { float& M = MinCoord.FindOrAdd(Find(V), TNumericLimits<float>::Max()); M = FMath::Min(M, L.HairCards[V].Z); }
		auto SkinDistance = [&](int32 V) { return FVector3f::DistSquared(L.Positions[V], Point(Surface, Near.Triangle[V], Near.Barycentrics[V])); };
		TMap<int32, int32> BestRoot, BestNearest;
		for (int32 V = 0; V < L.NumVertices(); ++V)
		{
			if (!Near.IsTracked(V)) continue;
			const int32 R = Find(V); const float Distance = SkinDistance(V);
			const int32* Prev = BestNearest.Find(R);
			if (!Prev || Distance < SkinDistance(*Prev)) BestNearest.Add(R, V);
			if (bCardData && L.HairCards[V].Z <= MinCoord[R] + RootCoordTolerance)
			{
				const int32* PrevRoot = BestRoot.Find(R);
				if (!PrevRoot || Distance < SkinDistance(*PrevRoot)) BestRoot.Add(R, V);
			}
		}
		for (int32 V = 0; V < L.NumVertices(); ++V)
		{
			const int32 R = Find(V); const int32* P = BestRoot.Find(R); if (!P) P = BestNearest.Find(R);
			Root.Add(P ? *P : INDEX_NONE);
		}
		return Root;
	}
	/** One card root's source-to-target skin correspondence. */
	struct FRootMap
	{
		FVector2f UV = FVector2f::ZeroVector;
		FVector3f SourceRoot = FVector3f::ZeroVector, TargetRoot = FVector3f::ZeroVector;
		int32 TT = INDEX_NONE; FVector3f TB = FVector3f::ZeroVector;
		FVector3f SX = FVector3f::ZeroVector, SY = FVector3f::ZeroVector, SZ = FVector3f::ZeroVector;
		FVector3f TX = FVector3f::ZeroVector, TY = FVector3f::ZeroVector, TZ = FVector3f::ZeroVector;
		bool bMapped = false;
	};
	void FaceWeights(FMantleSkinnedMeshLOD& L, int32 Vertex, const FMantleSkinnedMeshLOD& Face, int32 T, FVector3f Bary)
	{
		TMap<uint16, float> Blend;
		for (int32 C = 0; C < 3; ++C) for (int32 K = 0; K < Face.NumInfluences; ++K)
		{
			const int32 I = Face.Indices[T * 3 + C] * Face.NumInfluences + K;
			Blend.FindOrAdd(Face.InfluenceBones[I]) += Bary[C] * Face.InfluenceWeights[I];
		}
		TArray<TPair<uint16, float>> Weights; for (const auto& W : Blend) if (W.Value > 0.f) Weights.Emplace(W.Key, W.Value);
		Weights.Sort([](const auto& A, const auto& B) { return A.Value > B.Value || (A.Value == B.Value && A.Key < B.Key); });
		Weights.SetNum(FMath::Min(8, Weights.Num())); float Sum = 0.f; for (const auto& W : Weights) Sum += W.Value;
		check(Sum > 0.f); // Validated face vertices have normalized nonzero weights; barycentrics sum to one.
		int32 Used = 0;
		for (int32 K = 0; K < 8; ++K)
		{
			const int32 I = Vertex * 8 + K; L.InfluenceBones[I] = Weights.IsValidIndex(K) ? Weights[K].Key : 0;
			const int32 W = Weights.IsValidIndex(K) ? FMath::FloorToInt(255.f * Weights[K].Value / Sum) : 0;
			L.InfluenceWeights[I] = static_cast<uint8>(W); Used += W;
		}
		L.InfluenceWeights[Vertex * 8] += static_cast<uint8>(255 - Used);
	}
}

bool FMantleHairHeadFit::IsFaceSkinned(const FMantleSkinnedMesh& Mesh)
{
	return Mesh.Bones.ContainsByPredicate([](const FMantleSkinnedMeshBone& B) { return B.Name.ToString().StartsWith(TEXT("FACIAL_")); });
}

bool FMantleHairHeadFit::Fit(const FMantleSkinnedMesh& Cards, const FMantleSkinnedMesh& SourceFace, const FMantleSkinnedMesh& TargetFace,
	const FMantleSkinnedMesh& Body, bool bFacial, FMantleSkinnedMesh& Out, FString& Error, int32* OutBorrowedRoots)
{
	if (OutBorrowedRoots) *OutBorrowedRoots = 0;
	using namespace MantleHairHeadFitPrivate;
	Out = FMantleSkinnedMesh();
	if (!Cards.Validate(Error) || !SourceFace.Validate(Error) || !TargetFace.Validate(Error) || !Body.Validate(Error)) return false;
	if (SourceFace.LODs.IsEmpty() || TargetFace.LODs.IsEmpty() || !IsFaceSkinned(SourceFace) || !IsFaceSkinned(TargetFace))
	{ Error = TEXT("head fitting requires approved MetaHuman source and target face meshes"); return false; }
	FMantleSkinnedMeshLOD Source, Target;
	if (!SkinSurface(SourceFace, Source) || !SkinSurface(TargetFace, Target))
	{ Error = TEXT("head fitting needs a named external skin/head section; eyes, teeth and lashes cannot be the binding surface"); return false; }
	if (Source.UVs.Num() != Source.NumVertices() || Target.UVs.Num() != Target.NumVertices())
	{ Error = TEXT("MetaHuman head fitting requires corresponding source and target skin UVs"); return false; }
	const FUVSurface UVTarget(Target);
	Out = Cards;
	int32 Borrowed = 0;
	for (int32 LOD = 0; LOD < Out.LODs.Num(); ++LOD)
	{
		auto& L = Out.LODs[LOD];
		FMantleGarmentClearance Near;
		if (!FMantleGarmentClearance::Compute(L.Positions, Source, Near, bFacial ? 3.f : 30.f, &Error)) return false;
		const TArray<int32> Roots = CardRoots(L, Source, Near);
		if (bFacial) { L.NumInfluences = 8; L.InfluenceBones.SetNumZeroed(L.NumVertices() * 8); L.InfluenceWeights.SetNumZeroed(L.NumVertices() * 8); }
		// 1. Each card root's correspondence, once per root.
		TMap<int32, FRootMap> Maps;
		FString FirstFailure; int32 Failed = 0;
		for (int32 V = 0; V < L.NumVertices(); ++V)
		{
			const int32 R = Roots[V];
			if (R == INDEX_NONE || !Near.IsTracked(R)) { Error = TEXT("a hair card has no root within reach of the authoring head; align it to the source head before upload"); Out = FMantleSkinnedMesh(); return false; }
			if (Maps.Contains(R)) continue;
			FRootMap& M = Maps.Add(R);
			const int32 ST = Near.Triangle[R]; const FVector3f SB = Near.Barycentrics[R];
			M.UV = Source.UVs[Source.Indices[ST * 3]] * SB.X + Source.UVs[Source.Indices[ST * 3 + 1]] * SB.Y + Source.UVs[Source.Indices[ST * 3 + 2]] * SB.Z;
			M.SourceRoot = Point(Source, ST, SB);
			const TCHAR* Failure = nullptr;
			FVector2f TargetUV = M.UV;
			const bool bCovered = UVTarget.Find(M.UV,M.TT,M.TB);
			if (!bCovered && !UVTarget.FindBoundary(M.UV,M.TT,M.TB,TargetUV)) Failure = TEXT("target skin UV coverage is missing beyond the bounded LOD boundary correction");
			if (!Failure && !Frame(Source,ST,M.SX,M.SY,M.SZ)) Failure = TEXT("source skin UV or geometry frame is degenerate");
			if (!Failure && !Frame(Target,M.TT,M.TX,M.TY,M.TZ)) Failure = TEXT("target skin UV or geometry frame is degenerate");
			if (!Failure && !bCovered && (!SmallSurfaceCorrection(Source,ST,TargetUV-M.UV) || !SmallSurfaceCorrection(Target,M.TT,TargetUV-M.UV)
				|| FVector3f::DotProduct(M.SZ,M.TZ) < .8660254f)) Failure = TEXT("the near UV boundary has incompatible geometry or surface orientation");
			if (Failure)
			{
				if (FirstFailure.IsEmpty()) FirstFailure = FString::Printf(TEXT("head correspondence failed (%s), LOD%d card root vertex%d source triangle%d UV(%.8f,%.8f); use corresponding MetaHuman head UVs"),Failure,LOD,R,ST,M.UV.X,M.UV.Y);
				++Failed; continue;
			}
			M.TargetRoot = Point(Target, M.TT, M.TB); M.bMapped = true;
		}
		// 2. A root the wearer's head has no corresponding skin for (a trimmed UV border, usually at the nape or
		// crown) borrows the nearest mapped root's correspondence, instead of the whole hair failing. Too many such
		// roots mean the heads do not correspond at all.
		if (Failed > 0)
		{
			if (Failed > Maps.Num() * MaxBorrowedRootShare) { Error = FirstFailure; Out = FMantleSkinnedMesh(); return false; }
			TArray<int32> Mapped; for (const auto& Pair : Maps) if (Pair.Value.bMapped) Mapped.Add(Pair.Key);
			for (auto& Pair : Maps)
			{
				FRootMap& M = Pair.Value; if (M.bMapped) continue;
				int32 Best = INDEX_NONE; float BestDistance = TNumericLimits<float>::Max();
				for (int32 R : Mapped)
				{
					const float D = FVector3f::DistSquared(M.SourceRoot, Maps[R].SourceRoot);
					if (D < BestDistance || (D == BestDistance && R < Best)) { BestDistance = D; Best = R; }
				}
				const FRootMap& N = Maps[Best];
				const FVector2f UV = M.UV; const FVector3f SourceRoot = M.SourceRoot;
				M = N; M.UV = UV; M.SourceRoot = SourceRoot; M.TargetRoot = SourceRoot + (N.TargetRoot - N.SourceRoot); M.bMapped = false;
			}
			Borrowed += Failed;
		}
		// 3. Every vertex follows its root.
		for (int32 V = 0; V < L.NumVertices(); ++V)
		{
			const FRootMap& M = Maps[Roots[V]];
			const FVector2f UV = M.UV; const FVector3f SourceRoot = M.SourceRoot, TargetRoot = M.TargetRoot;
			const FVector3f &SX = M.SX, &SY = M.SY, &SZ = M.SZ, &TX = M.TX, &TY = M.TY, &TZ = M.TZ;
			const int32 TT = M.TT; const FVector3f TB = M.TB;
			if (bFacial)
			{
				// Brows, lashes and facial hair sit within 3 cm of the skin and are skinned to the face, so they keep the
				// local frame rotation: it follows the surface they lie on, and they are too small for the error to show.
				auto Rotate = [&](FVector3f P) { return TX * FVector3f::DotProduct(P, SX) + TY * FVector3f::DotProduct(P, SY) + TZ * FVector3f::DotProduct(P, SZ); };
				L.Positions[V] = TargetRoot + Rotate(Cards.LODs[LOD].Positions[V] - SourceRoot);
				L.Normals[V] = Rotate(L.Normals[V]).GetSafeNormal();
				if (L.Tangents.IsValidIndex(V)) { const auto T = L.Tangents[V]; const auto XYZ = Rotate(FVector3f(T.X, T.Y, T.Z)).GetSafeNormal(); L.Tangents[V] = FVector4f(XYZ.X, XYZ.Y, XYZ.Z, T.W); }
			}
			// Scalp hair: a rigid translation by the root's source-to-target skin offset; normals and tangents are
			// unchanged. The two heads are triangulated differently, so a rotation taken from one triangle frame on
			// each is noise.
			else L.Positions[V] = Cards.LODs[LOD].Positions[V] + (TargetRoot - SourceRoot);
			if (L.HairCards.IsValidIndex(V)) { L.HairCards[V].X = UV.X; L.HairCards[V].Y = UV.Y; }
			if (bFacial) FaceWeights(L, V, Target, TT, TB);
		}
	}
	if (bFacial) { Out.Bones = TargetFace.Bones; Out.ReferencePose = TargetFace.ReferencePose; }
	else
	{
		const int32 Head = Body.Bones.IndexOfByPredicate([](const FMantleSkinnedMeshBone& B) { return B.Name == FName(TEXT("head")); });
		if (Head == INDEX_NONE) { Error = TEXT("the MetaHuman body has no head bone"); return false; }
		// Positions already lie in target reference space. Rebinding them through the source pose would move them twice.
		Out.Bones = Body.Bones; Out.ReferencePose = Body.ReferencePose;
		for (auto& L : Out.LODs) { L.NumInfluences = 1; L.InfluenceBones.Init(static_cast<uint16>(Head), L.NumVertices()); L.InfluenceWeights.Init(255, L.NumVertices()); }
	}
	if (OutBorrowedRoots) *OutBorrowedRoots = Borrowed;
	return Out.Validate(Error);
}
