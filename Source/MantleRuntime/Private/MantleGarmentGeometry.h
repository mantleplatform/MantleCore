// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"
#include "MantleRuntime.h"
#include "MantleSkinnedMesh.h"

/**
 * Geometry the garment fit's restores, neck samples, skin transfer and checks share.
 * Header-only, in a named namespace: MantleRuntime builds as a unity build.
 */
namespace MantleGarmentGeometry
{
	/** Triangles with a cross product below this (squared, cm^4) are degenerate: no normal, never searched. */
	inline constexpr float DegenerateCrossSq = 1.0e-10f;

	/** Vertices within this quantum share a welded id (render meshes split vertices at UV and normal seams). */
	inline constexpr float WeldQuantumCm = 1.0e-3f;

	/** Moller-Trumbore: a determinant this small is a ray parallel to the triangle. */
	inline constexpr float RayParallelEpsilon = 1.0e-8f;

	inline double NowMs()
	{
		return FPlatformTime::Seconds() * 1000.0;
	}

	/** Logs "MANTLE_GARMENTFIT FAIL <Step>: <Error>", returns the reason in OutError, and false. */
	inline bool RefuseFit(const TCHAR* Step, const FString& Error, FString* OutError)
	{
		UE_LOG(LogMantle, Error, TEXT("MANTLE_GARMENTFIT FAIL %s: %s"), Step, *Error);
		if (OutError)
		{
			*OutError = Error;
		}
		return false;
	}

	/** Closest point on triangle ABC to P (Ericson, Real-Time Collision Detection, 5.1.5); OutBary = the weights of A, B, C. ABC must not be degenerate. */
	inline FVector3f ClosestPointOnTriangle(const FVector3f& P, const FVector3f& A, const FVector3f& B, const FVector3f& C, FVector3f& OutBary)
	{
		const FVector3f AB = B - A;
		const FVector3f AC = C - A;
		const FVector3f AP = P - A;
		const float D1 = FVector3f::DotProduct(AB, AP);
		const float D2 = FVector3f::DotProduct(AC, AP);
		if (D1 <= 0.0f && D2 <= 0.0f)
		{
			OutBary = FVector3f(1.0f, 0.0f, 0.0f);
			return A;
		}
		const FVector3f BP = P - B;
		const float D3 = FVector3f::DotProduct(AB, BP);
		const float D4 = FVector3f::DotProduct(AC, BP);
		if (D3 >= 0.0f && D4 <= D3)
		{
			OutBary = FVector3f(0.0f, 1.0f, 0.0f);
			return B;
		}
		const float VC = D1 * D4 - D3 * D2;
		if (VC <= 0.0f && D1 >= 0.0f && D3 <= 0.0f)
		{
			const float V = D1 / (D1 - D3);
			OutBary = FVector3f(1.0f - V, V, 0.0f);
			return A + AB * V;
		}
		const FVector3f CP = P - C;
		const float D5 = FVector3f::DotProduct(AB, CP);
		const float D6 = FVector3f::DotProduct(AC, CP);
		if (D6 >= 0.0f && D5 <= D6)
		{
			OutBary = FVector3f(0.0f, 0.0f, 1.0f);
			return C;
		}
		const float VB = D5 * D2 - D1 * D6;
		if (VB <= 0.0f && D2 >= 0.0f && D6 <= 0.0f)
		{
			const float W = D2 / (D2 - D6);
			OutBary = FVector3f(1.0f - W, 0.0f, W);
			return A + AC * W;
		}
		const float VA = D3 * D6 - D5 * D4;
		if (VA <= 0.0f && (D4 - D3) >= 0.0f && (D5 - D6) >= 0.0f)
		{
			const float W = (D4 - D3) / ((D4 - D3) + (D5 - D6));
			OutBary = FVector3f(0.0f, 1.0f - W, W);
			return B + (C - B) * W;
		}
		const float Denom = 1.0f / (VA + VB + VC);
		const float V = VB * Denom;
		const float W = VC * Denom;
		OutBary = FVector3f(1.0f - V - W, V, W);
		return A + AB * V + AC * W;
	}

	inline float DistSqPointBox(const FVector3f& P, const FBox3f& Box)
	{
		const float DX = FMath::Max3(Box.Min.X - P.X, 0.0f, P.X - Box.Max.X);
		const float DY = FMath::Max3(Box.Min.Y - P.Y, 0.0f, P.Y - Box.Max.Y);
		const float DZ = FMath::Max3(Box.Min.Z - P.Z, 0.0f, P.Z - Box.Max.Z);
		return DX * DX + DY * DY + DZ * DZ;
	}

	/**
	 * Two-sided Moller-Trumbore: the ray Origin + t Dir hits ABC at t in [0, TMax]. EdgeTolerance (barycentric) widens the
	 * triangle a little past its edges: a ray through an edge or corner several triangles share can otherwise slip
	 * between all of them to rounding (#877: a garment made by pushing the body out along its normals puts every garment
	 * vertex on a body vertex's normal ray). 0, the default, is the exact test.
	 */
	inline bool RayHitsTriangle(const FVector3f& Origin, const FVector3f& Dir, const FVector3f& A, const FVector3f& B, const FVector3f& C, const float TMax, float& OutT,
		const float EdgeTolerance = 0.0f)
	{
		const FVector3f E1 = B - A;
		const FVector3f E2 = C - A;
		const FVector3f PV = FVector3f::CrossProduct(Dir, E2);
		const float Det = FVector3f::DotProduct(E1, PV);
		if (FMath::Abs(Det) <= RayParallelEpsilon)
		{
			return false;
		}
		const float InvDet = 1.0f / Det;
		const FVector3f TV = Origin - A;
		const float U = FVector3f::DotProduct(TV, PV) * InvDet;
		if (U < -EdgeTolerance || U > 1.0f + EdgeTolerance)
		{
			return false;
		}
		const FVector3f QV = FVector3f::CrossProduct(TV, E1);
		const float V = FVector3f::DotProduct(Dir, QV) * InvDet;
		if (V < -EdgeTolerance || U + V > 1.0f + EdgeTolerance)
		{
			return false;
		}
		const float T = FVector3f::DotProduct(E2, QV) * InvDet;
		if (T < 0.0f || T > TMax)
		{
			return false;
		}
		OutT = T;
		return true;
	}

	/**
	 * Per triangle of Indices over Positions: the unit normal oriented to agree with the summed vertex
	 * normals, its sign relative to cross(B - A, C - A), and the bounds. Degenerate triangles get a zero
	 * normal, sign +1 and invalid bounds (the grids skip them). Returns the non-degenerate count.
	 */
	inline int32 TriangleFrames(TConstArrayView<FVector3f> Positions, TConstArrayView<FVector3f> VertexNormals, TConstArrayView<uint32> Indices,
		TArray<FVector3f>& OutNormals, TArray<int8>& OutSigns, TArray<FBox3f>& OutBoxes)
	{
		const int32 NumTriangles = Indices.Num() / 3;
		OutNormals.SetNumUninitialized(NumTriangles);
		OutSigns.SetNumUninitialized(NumTriangles);
		OutBoxes.SetNumUninitialized(NumTriangles);
		int32 Valid = 0;
		for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle)
		{
			const uint32 I0 = Indices[Triangle * 3 + 0];
			const uint32 I1 = Indices[Triangle * 3 + 1];
			const uint32 I2 = Indices[Triangle * 3 + 2];
			const FVector3f& A = Positions[I0];
			const FVector3f& B = Positions[I1];
			const FVector3f& C = Positions[I2];
			const FVector3f Cross = FVector3f::CrossProduct(B - A, C - A);
			if (Cross.SizeSquared() <= DegenerateCrossSq)
			{
				OutNormals[Triangle] = FVector3f::ZeroVector;
				OutSigns[Triangle] = 1;
				OutBoxes[Triangle] = FBox3f(ForceInit);
				continue;
			}
			const FVector3f Hint = VertexNormals[I0] + VertexNormals[I1] + VertexNormals[I2];
			const int8 Sign = FVector3f::DotProduct(Cross, Hint) >= 0.0f ? 1 : -1;
			OutNormals[Triangle] = Cross.GetUnsafeNormal() * static_cast<float>(Sign);
			OutSigns[Triangle] = Sign;
			OutBoxes[Triangle] = FBox3f(A.ComponentMin(B).ComponentMin(C), A.ComponentMax(B).ComponentMax(C));
			++Valid;
		}
		return Valid;
	}

	/**
	 * Uniform grid over boxes (triangles, or points as zero-size boxes): each valid box is listed in every
	 * cell it touches. Cell Id owns Items[Start[Id] .. Start[Id + 1]).
	 */
	struct FBoxGrid
	{
		float InvCell = 1.0f;
		TMap<FIntVector, int32> CellIds;
		TArray<int32> Start;
		TArray<int32> Items;

		FIntVector CellOf(const FVector3f& P) const
		{
			return FIntVector(FMath::FloorToInt32(P.X * InvCell), FMath::FloorToInt32(P.Y * InvCell), FMath::FloorToInt32(P.Z * InvCell));
		}

		void Build(TConstArrayView<FBox3f> Boxes, const float CellCm)
		{
			InvCell = 1.0f / CellCm;
			CellIds.Reset();
			Start.Reset();
			Items.Reset();
			// Pass 0 counts the (box, cell) pairs per cell, pass 1 fills them in.
			TArray<int32> Fill;
			for (int32 Pass = 0; Pass < 2; ++Pass)
			{
				for (int32 Item = 0; Item < Boxes.Num(); ++Item)
				{
					const FBox3f& Box = Boxes[Item];
					if (!Box.IsValid)
					{
						continue;
					}
					const FIntVector Lo = CellOf(Box.Min);
					const FIntVector Hi = CellOf(Box.Max);
					for (int32 Z = Lo.Z; Z <= Hi.Z; ++Z)
					{
						for (int32 Y = Lo.Y; Y <= Hi.Y; ++Y)
						{
							for (int32 X = Lo.X; X <= Hi.X; ++X)
							{
								const FIntVector Cell(X, Y, Z);
								if (Pass == 0)
								{
									const int32* Found = CellIds.Find(Cell);
									const int32 Id = Found ? *Found : CellIds.Add(Cell, Start.Num());
									if (!Found)
									{
										Start.Add(0);
									}
									++Start[Id];
								}
								else
								{
									Items[Fill[CellIds.FindChecked(Cell)]++] = Item;
								}
							}
						}
					}
				}
				if (Pass == 0)
				{
					// Counts -> exclusive prefix sums, plus the end sentinel.
					int32 Running = 0;
					for (int32& Count : Start)
					{
						const int32 InCell = Count;
						Count = Running;
						Running += InCell;
					}
					Start.Add(Running);
					Items.SetNumUninitialized(Running);
					Fill = Start;
				}
			}
		}

		template <typename FnType>
		void ForEachInCell(const FIntVector& Cell, FnType&& Fn) const
		{
			if (const int32* Id = CellIds.Find(Cell))
			{
				for (int32 Index = Start[*Id]; Index < Start[*Id + 1]; ++Index)
				{
					Fn(Items[Index]);
				}
			}
		}
	};

	/** One id per distinct position (quantised to WeldQuantumCm): vertices split at seams share it. Returns the id count. */
	inline int32 WeldByPosition(TConstArrayView<FVector3f> Positions, TArray<int32>& OutWeld)
	{
		const float Inv = 1.0f / WeldQuantumCm;
		TMap<FIntVector, int32> Ids;
		Ids.Reserve(Positions.Num());
		OutWeld.SetNumUninitialized(Positions.Num());
		for (int32 Vertex = 0; Vertex < Positions.Num(); ++Vertex)
		{
			const FVector3f& P = Positions[Vertex];
			const FIntVector Key(FMath::RoundToInt32(P.X * Inv), FMath::RoundToInt32(P.Y * Inv), FMath::RoundToInt32(P.Z * Inv));
			const int32* Found = Ids.Find(Key);
			OutWeld[Vertex] = Found ? *Found : Ids.Add(Key, Ids.Num());
		}
		return Ids.Num();
	}

	/**
	 * Open boundary edges of a triangle list over welded ids: edges used by exactly one triangle. Per
	 * triangle, OutFlags bit E (0..2) marks its edge corner E -> corner (E + 1) % 3 as open and bit 3 + C
	 * marks corner C as a vertex on some open edge. Returns the number of open edges.
	 */
	inline int32 FindOpenEdges(TConstArrayView<uint32> Indices, TConstArrayView<int32> Weld, const int32 NumWelded, TArray<uint8>& OutFlags)
	{
		struct FEdgeRef
		{
			uint64 Key;
			int32 TriCorner;
		};
		const int32 NumTriangles = Indices.Num() / 3;
		OutFlags.Init(0, NumTriangles);
		TArray<FEdgeRef> Edges;
		Edges.Reserve(NumTriangles * 3);
		for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle)
		{
			for (int32 Edge = 0; Edge < 3; ++Edge)
			{
				const uint32 A = static_cast<uint32>(Weld[Indices[Triangle * 3 + Edge]]);
				const uint32 B = static_cast<uint32>(Weld[Indices[Triangle * 3 + (Edge + 1) % 3]]);
				if (A != B)
				{
					Edges.Add({ (static_cast<uint64>(FMath::Min(A, B)) << 32) | static_cast<uint64>(FMath::Max(A, B)), Triangle * 3 + Edge });
				}
			}
		}
		Edges.Sort([](const FEdgeRef& L, const FEdgeRef& R) { return L.Key < R.Key; });

		TBitArray<> OnBoundary(false, NumWelded);
		int32 Open = 0;
		for (int32 Index = 0; Index < Edges.Num();)
		{
			int32 Next = Index + 1;
			while (Next < Edges.Num() && Edges[Next].Key == Edges[Index].Key)
			{
				++Next;
			}
			if (Next - Index == 1)
			{
				++Open;
				const int32 Triangle = Edges[Index].TriCorner / 3;
				const int32 Edge = Edges[Index].TriCorner % 3;
				OutFlags[Triangle] |= static_cast<uint8>(1u << Edge);
				OnBoundary[static_cast<int32>(Edges[Index].Key >> 32)] = true;
				OnBoundary[static_cast<int32>(Edges[Index].Key & 0xffffffffull)] = true;
			}
			Index = Next;
		}
		for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle)
		{
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				if (OnBoundary[Weld[Indices[Triangle * 3 + Corner]]])
				{
					OutFlags[Triangle] |= static_cast<uint8>(8u << Corner);
				}
			}
		}
		return Open;
	}

	/** Per vertex the section whose triangles use it first, INDEX_NONE for vertices no triangle uses. */
	inline void VertexSections(const FMantleSkinnedMeshLOD& LOD, TArray<int32>& Out)
	{
		Out.Init(INDEX_NONE, LOD.NumVertices());
		for (int32 SectionIndex = 0; SectionIndex < LOD.Sections.Num(); ++SectionIndex)
		{
			const FMantleSkinnedMeshSection& Section = LOD.Sections[SectionIndex];
			const int32 End = FMath::Min(Section.FirstIndex + Section.IndexCount, LOD.Indices.Num());
			for (int32 Index = FMath::Max(Section.FirstIndex, 0); Index < End; ++Index)
			{
				const int32 Vertex = static_cast<int32>(LOD.Indices[Index]);
				if (Out.IsValidIndex(Vertex) && Out[Vertex] == INDEX_NONE)
				{
					Out[Vertex] = SectionIndex;
				}
			}
		}
	}

	/**
	 * WeldByPosition within each section: one id per (section, quantised position), so split copies of a
	 * vertex share an id and two sections never do. Returns the id count.
	 */
	inline int32 WeldBySection(TConstArrayView<FVector3f> Positions, TConstArrayView<int32> VertexSection, TArray<int32>& OutWeld)
	{
		const float Inv = 1.0f / WeldQuantumCm;
		TArray<TMap<FIntVector, int32>> Ids;
		int32 Count = 0;
		OutWeld.SetNumUninitialized(Positions.Num());
		for (int32 Vertex = 0; Vertex < Positions.Num(); ++Vertex)
		{
			const int32 Group = VertexSection[Vertex] + 1;
			if (Group >= Ids.Num())
			{
				Ids.SetNum(Group + 1);
			}
			const FVector3f& P = Positions[Vertex];
			const FIntVector Key(FMath::RoundToInt32(P.X * Inv), FMath::RoundToInt32(P.Y * Inv), FMath::RoundToInt32(P.Z * Inv));
			if (const int32* Found = Ids[Group].Find(Key))
			{
				OutWeld[Vertex] = *Found;
			}
			else
			{
				Ids[Group].Add(Key, Count);
				OutWeld[Vertex] = Count++;
			}
		}
		return Count;
	}

	using FPointNeighbours = TArray<TArray<int32, TInlineAllocator<8>>>;

	/** Welded points' adjacency from the triangle edges. */
	inline void PointNeighbours(TConstArrayView<uint32> Indices, TConstArrayView<int32> Weld, const int32 NumPoints, FPointNeighbours& Out)
	{
		Out.Reset();
		Out.SetNum(NumPoints);
		for (int32 Triangle = 0; Triangle < Indices.Num() / 3; ++Triangle)
		{
			for (int32 Edge = 0; Edge < 3; ++Edge)
			{
				const int32 A = Weld[Indices[Triangle * 3 + Edge]];
				const int32 B = Weld[Indices[Triangle * 3 + (Edge + 1) % 3]];
				if (A != B)
				{
					Out[A].AddUnique(B);
					Out[B].AddUnique(A);
				}
			}
		}
	}

	/** Every index below NumVertices and a whole number of triangles. */
	inline bool IndicesInRange(TConstArrayView<uint32> Indices, const int32 NumVertices)
	{
		if (Indices.Num() % 3 != 0)
		{
			return false;
		}
		for (const uint32 Index : Indices)
		{
			if (static_cast<int64>(Index) >= NumVertices)
			{
				return false;
			}
		}
		return true;
	}
}
