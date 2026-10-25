// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSkinnedMesh.h"

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleBodyOffsetsPrivate
{
	/** One shape at a strength, for the sliders' sum. */
	struct FContribution
	{
		const FMantleBodyOffsets::FShape* Shape = nullptr;
		float Strength = 0.0f;
	};

	/** The shapes the four sliders ask for: slider k's + shape is 2k, its - shape 2k+1. A missing shape adds nothing. */
	void Collect(const FMantleBodyOffsets& Offsets, const int32 (&Sliders)[4], TArray<FContribution, TInlineAllocator<4>>& Out)
	{
		for (int32 Slider = 0; Slider < 4; ++Slider)
		{
			const int32 Value = FMath::Clamp(Sliders[Slider], -100, 100);
			if (Value == 0)
			{
				continue;
			}
			const int32 Id = Slider * 2 + (Value > 0 ? 0 : 1);
			if (Offsets.Has(Id))
			{
				Out.Add({ &Offsets.Shapes[Id], FMath::Abs(Value) / 100.0f });
			}
		}
	}

	/** A position's bits with -0 folded into +0, so identical positions weld whatever sign a zero carries. */
	TTuple<uint32, uint32, uint32> WeldKey(const FVector3f& Position)
	{
		auto Bits = [](const float Value)
		{
			const float Folded = Value + 0.0f;
			uint32 Out;
			FMemory::Memcpy(&Out, &Folded, sizeof(Out));
			return Out;
		};
		return MakeTuple(Bits(Position.X), Bits(Position.Y), Bits(Position.Z));
	}

	/**
	 * Vertex normals from the triangles: area-weighted face normals summed per position (the copies a UV seam
	 * splits share one sum) and normalised. A vertex whose faces are all degenerate keeps its old normal.
	 */
	void RecomputeNormals(FMantleSkinnedMeshLOD& LOD)
	{
		const int32 NumVertices = LOD.NumVertices();
		TMap<TTuple<uint32, uint32, uint32>, int32> Welded;
		Welded.Reserve(NumVertices);
		TArray<int32> WeldIndex;
		WeldIndex.SetNumUninitialized(NumVertices);
		for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
		{
			WeldIndex[Vertex] = Welded.FindOrAdd(WeldKey(LOD.Positions[Vertex]), Welded.Num());
		}

		// Which way the cross product faces depends on the mesh's winding convention, which is not assumed: the faces
		// are summed as the cross product gives them, then all flipped together when they disagree with the old normals.
		TArray<FVector3d> Sums;
		Sums.SetNumZeroed(Welded.Num());
		double Agreement = 0.0;
		for (int32 Index = 0; Index + 2 < LOD.Indices.Num(); Index += 3)
		{
			const uint32 A = LOD.Indices[Index], B = LOD.Indices[Index + 1], C = LOD.Indices[Index + 2];
			if (A >= static_cast<uint32>(NumVertices) || B >= static_cast<uint32>(NumVertices) || C >= static_cast<uint32>(NumVertices))
			{
				continue;
			}
			// The cross product's length is twice the triangle's area, which is the weight.
			const FVector3d PA(LOD.Positions[A]), PB(LOD.Positions[B]), PC(LOD.Positions[C]);
			const FVector3d Face = FVector3d::CrossProduct(PB - PA, PC - PA);
			Sums[WeldIndex[A]] += Face;
			Sums[WeldIndex[B]] += Face;
			Sums[WeldIndex[C]] += Face;
			Agreement += FVector3d::DotProduct(Face, FVector3d(LOD.Normals[A] + LOD.Normals[B] + LOD.Normals[C]));
		}
		const double Orientation = Agreement < 0.0 ? -1.0 : 1.0;

		for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
		{
			const FVector3d Sum = Sums[WeldIndex[Vertex]] * Orientation;
			if (Sum.SizeSquared() > UE_DOUBLE_SMALL_NUMBER)
			{
				LOD.Normals[Vertex] = FVector3f(Sum.GetSafeNormal());
			}
		}
	}

	/** Gram-Schmidt each tangent against its new normal, keeping W (the binormal sign). */
	void Reorthogonalise(FMantleSkinnedMeshLOD& LOD)
	{
		if (LOD.Tangents.Num() != LOD.NumVertices())
		{
			return;
		}
		for (int32 Vertex = 0; Vertex < LOD.NumVertices(); ++Vertex)
		{
			const FVector3f& Normal = LOD.Normals[Vertex];
			FVector4f& Tangent = LOD.Tangents[Vertex];
			FVector3f Axis(Tangent.X, Tangent.Y, Tangent.Z);
			Axis -= Normal * FVector3f::DotProduct(Normal, Axis);
			if (Axis.SizeSquared() <= UE_SMALL_NUMBER)
			{
				// The tangent lay along the normal: any perpendicular will do, picked off the normal's weakest axis.
				const FVector3f Weakest = FMath::Abs(Normal.X) < 0.9f ? FVector3f::XAxisVector : FVector3f::YAxisVector;
				Axis = FVector3f::CrossProduct(Normal, Weakest);
			}
			Axis = Axis.GetSafeNormal();
			Tangent = FVector4f(Axis.X, Axis.Y, Axis.Z, Tangent.W);
		}
	}
}

void FMantleBodyOffsets::Apply(FMantleSkinnedMesh& Mesh, const int32 Weight, const int32 Muscle, const int32 Chest, const int32 Hips)
{
	using namespace MantleBodyOffsetsPrivate;

	const int32 Sliders[4] = { Weight, Muscle, Chest, Hips };
	TArray<FContribution, TInlineAllocator<4>> Active;
	Collect(Mesh.BodyOffsets, Sliders, Active);
	if (Active.Num() == 0)
	{
		return;
	}

	for (int32 LODIndex = 0; LODIndex < Mesh.LODs.Num(); ++LODIndex)
	{
		FMantleSkinnedMeshLOD& LOD = Mesh.LODs[LODIndex];
		for (const FContribution& Contribution : Active)
		{
			// A shape that does not match the mesh is skipped, never read out of bounds (the reader checks this).
			if (!Contribution.Shape->LODs.IsValidIndex(LODIndex) || Contribution.Shape->LODs[LODIndex].Num() != LOD.NumVertices())
			{
				continue;
			}
			const TArray<FVector3f>& Deltas = Contribution.Shape->LODs[LODIndex];
			for (int32 Vertex = 0; Vertex < Deltas.Num(); ++Vertex)
			{
				LOD.Positions[Vertex] += Deltas[Vertex] * Contribution.Strength;
			}
		}
		RecomputeNormals(LOD);
		Reorthogonalise(LOD);
	}

	// Garments carry no pose (they borrow their wearer's), so there is nothing to move for them.
	if (Mesh.HasReferencePose())
	{
		for (const FContribution& Contribution : Active)
		{
			if (Contribution.Shape->Bones.Num() != Mesh.ReferencePose.Num())
			{
				continue;
			}
			for (int32 Bone = 0; Bone < Mesh.ReferencePose.Num(); ++Bone)
			{
				const FVector3f Delta = Contribution.Shape->Bones[Bone] * Contribution.Strength;
				Mesh.ReferencePose[Bone].AddToTranslation(FVector(Delta));
			}
		}
	}
}

FString FMantleBodyOffsets::Key(const int32 Weight, const int32 Muscle, const int32 Chest, const int32 Hips)
{
	const int32 W = FMath::Clamp(Weight, -100, 100);
	const int32 M = FMath::Clamp(Muscle, -100, 100);
	const int32 C = FMath::Clamp(Chest, -100, 100);
	const int32 H = FMath::Clamp(Hips, -100, 100);
	if (W == 0 && M == 0 && C == 0 && H == 0)
	{
		return FString();
	}
	return FString::Printf(TEXT("bs%d_%d_%d_%d"), W, M, C, H);
}
