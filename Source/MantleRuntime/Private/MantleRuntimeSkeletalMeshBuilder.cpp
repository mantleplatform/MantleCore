// Copyright Joshua Eger. All Rights Reserved.
#include "MantleRuntimeSkeletalMeshBuilder.h"

#include "MantleRuntime.h"
#include "MantleBodyShape.h"
#include "MantleGarmentFitter.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/IConsoleManager.h"
#include "ReferenceSkeleton.h"
#include "SkeletalMeshReductionSettings.h"
#include "Rendering/MultiSizeIndexContainer.h"
#include "Rendering/PositionVertexBuffer.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "Rendering/StaticMeshVertexBuffer.h"
#include "UObject/Package.h"

// The engine sequence follows FSkeletalMeshMerge (SkeletalMeshMerge.cpp) and the engine's runtime
// mesh-instance update (MUT section 3). Named namespace for the unity build.
namespace MantleSkeletalMeshBuilderPrivate
{
	/** limits.json lods.screenSizes. */
	constexpr float LODScreenSizes[FMantleRuntimeSkeletalMeshBuilder::MaxLODCount] = { 1.0f, 0.35f, 0.15f };

	TAutoConsoleVariable<int32> CVarMantleMaxBuiltLODs(
		TEXT("Mantle.Avatar.MaxBuiltLODs"),
		FMantleRuntimeSkeletalMeshBuilder::MaxLODCount,
		TEXT("The most LODs the runtime skeletal mesh builder builds for avatars and garments: 1 builds LOD0 alone, ")
		TEXT("3 every LOD the cooked mesh has. Read when a mesh is built; already built meshes keep theirs."),
		ECVF_Default);

	USkeletalMesh* Refuse(FString* OutError, const FString& Error)
	{
		UE_LOG(LogMantle, Error, TEXT("MANTLE_SKINNEDMESH FAIL build: %s"), *Error);
		if (OutError)
		{
			*OutError = Error;
		}
		return nullptr;
	}

	/**
	 * Per-vertex tangents from the UV layout, for meshes cooked without any: each triangle's UV
	 * gradient accumulated on its corners, made orthogonal to the vertex normal; W is the binormal
	 * sign in the engine's convention (binormal = (normal ^ tangent) * W). A vertex with no usable
	 * UV gradient gets an arbitrary tangent perpendicular to its normal.
	 */
	void DeriveTangents(const FMantleSkinnedMeshLOD& LOD, TArray<FVector4f>& Out)
	{
		const int32 N = LOD.NumVertices();
		TArray<FVector3f> TangentSum;
		TArray<FVector3f> BinormalSum;
		TangentSum.Init(FVector3f::ZeroVector, N);
		BinormalSum.Init(FVector3f::ZeroVector, N);
		for (int32 First = 0; First + 2 < LOD.Indices.Num(); First += 3)
		{
			const uint32 Corner[3] = { LOD.Indices[First], LOD.Indices[First + 1], LOD.Indices[First + 2] };
			const FVector3f Edge1 = LOD.Positions[Corner[1]] - LOD.Positions[Corner[0]];
			const FVector3f Edge2 = LOD.Positions[Corner[2]] - LOD.Positions[Corner[0]];
			const FVector2f DeltaUV1 = LOD.UVs[Corner[1]] - LOD.UVs[Corner[0]];
			const FVector2f DeltaUV2 = LOD.UVs[Corner[2]] - LOD.UVs[Corner[0]];
			const float Determinant = DeltaUV1.X * DeltaUV2.Y - DeltaUV2.X * DeltaUV1.Y;
			if (FMath::Abs(Determinant) < UE_SMALL_NUMBER)
			{
				continue;
			}
			const FVector3f Tangent = (Edge1 * DeltaUV2.Y - Edge2 * DeltaUV1.Y) / Determinant;
			const FVector3f Binormal = (Edge2 * DeltaUV1.X - Edge1 * DeltaUV2.X) / Determinant;
			for (const uint32 Vertex : Corner)
			{
				TangentSum[Vertex] += Tangent;
				BinormalSum[Vertex] += Binormal;
			}
		}

		Out.SetNumUninitialized(N);
		for (int32 Vertex = 0; Vertex < N; ++Vertex)
		{
			const FVector3f Normal = LOD.Normals[Vertex].GetSafeNormal();
			FVector3f Tangent = (TangentSum[Vertex] - Normal * FVector3f::DotProduct(Normal, TangentSum[Vertex])).GetSafeNormal();
			if (Tangent.IsZero())
			{
				FVector3f Unused;
				Normal.FindBestAxisVectors(Tangent, Unused);
			}
			const float Sign = FVector3f::DotProduct(FVector3f::CrossProduct(Normal, Tangent), BinormalSum[Vertex]) < 0.0f ? -1.0f : 1.0f;
			Out[Vertex] = FVector4f(Tangent.X, Tangent.Y, Tangent.Z, Sign);
		}
	}

	/** True when Ancestor is Bone's parent, grandparent, ... in Ref (raw indices). */
	bool IsAncestorOf(const FReferenceSkeleton& Ref, const int32 Ancestor, const int32 Bone)
	{
		for (int32 Current = Ref.GetRawParentIndex(Bone); Current != INDEX_NONE; Current = Ref.GetRawParentIndex(Current))
		{
			if (Current == Ancestor)
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * One LOD's render data: its bone map (the bones its vertices are weighted to, in reference skeleton
	 * order, shared by every section), render sections over the kept triangles, vertex, skin weight and
	 * index buffers (their CPU copies kept when bNeedsCPUAccess, FMantleSkinnedMeshBuildParams::bKeepCPUData),
	 * active and required bones. MeshToBuilt maps the mesh's bone indices to the built mesh's; Filter holds the
	 * LOD's kept triangles (moved out of).
	 */
	void FillLODRenderData(const FMantleSkinnedMeshLOD& LOD, FMantleTriangleFilterResult& Filter, const TArray<int32>& MeshToBuilt, const int32 NumBones,
		const FReferenceSkeleton& BuiltBones, const bool bNeedsCPUAccess, FSkeletalMeshLODRenderData& LODData)
	{
		const int32 N = LOD.NumVertices();

		// One bone map for every section: the bones the vertices are weighted to, in reference skeleton order.
		TBitArray<> UsedBones(false, NumBones);
		for (int32 Slot = 0; Slot < LOD.InfluenceBones.Num(); ++Slot)
		{
			if (LOD.InfluenceWeights[Slot] > 0)
			{
				UsedBones[MeshToBuilt[LOD.InfluenceBones[Slot]]] = true;
			}
		}
		TArray<FBoneIndexType> BoneMap;
		TArray<int32> BuiltToLocal;
		BuiltToLocal.Init(INDEX_NONE, NumBones);
		for (TConstSetBitIterator<> It(UsedBones); It; ++It)
		{
			BuiltToLocal[It.GetIndex()] = BoneMap.Add(static_cast<FBoneIndexType>(It.GetIndex()));
		}

		// Skin weights with bone-map-local indices; 8-bit weights widened the way the engine reads them back.
		TArray<FSkinWeightInfo> Weights;
		Weights.SetNumZeroed(N);
		for (int32 Vertex = 0; Vertex < N; ++Vertex)
		{
			FSkinWeightInfo& Info = Weights[Vertex];
			for (int32 Slot = 0; Slot < LOD.NumInfluences; ++Slot)
			{
				const int32 Index = Vertex * LOD.NumInfluences + Slot;
				const uint8 Weight = LOD.InfluenceWeights[Index];
				if (Weight > 0)
				{
					Info.InfluenceBones[Slot] = static_cast<FBoneIndexType>(BuiltToLocal[MeshToBuilt[LOD.InfluenceBones[Index]]]);
					Info.InfluenceWeights[Slot] = static_cast<uint16>((static_cast<uint16>(Weight) << 8) | Weight);
				}
			}
		}

		TArray<FVector4f> DerivedTangents;
		if (LOD.Tangents.Num() != N)
		{
			DeriveTangents(LOD, DerivedTangents);
		}
		const TArray<FVector4f>& Tangents = LOD.Tangents.Num() == N ? LOD.Tangents : DerivedTangents;

		// Each section's vertex range, over its unfiltered triangles (the renderer reads it as a range).
		TArray<uint32> SectionBaseVertex;
		TArray<uint32> SectionNumVertices;
		for (const FMantleSkinnedMeshSection& Section : LOD.Sections)
		{
			uint32 Lowest = MAX_uint32;
			uint32 Highest = 0;
			for (int32 Position = Section.FirstIndex; Position < Section.FirstIndex + Section.IndexCount; ++Position)
			{
				Lowest = FMath::Min(Lowest, LOD.Indices[Position]);
				Highest = FMath::Max(Highest, LOD.Indices[Position]);
			}
			SectionBaseVertex.Add(Section.IndexCount > 0 ? Lowest : 0);
			SectionNumVertices.Add(Section.IndexCount > 0 ? Highest - Lowest + 1 : 0);
		}

		for (int32 SectionIndex = 0; SectionIndex < LOD.Sections.Num(); ++SectionIndex)
		{
			FSkelMeshRenderSection& RenderSection = LODData.RenderSections.AddDefaulted_GetRef();
			RenderSection.MaterialIndex = static_cast<uint16>(LOD.Sections[SectionIndex].MaterialIndex);
			RenderSection.BaseIndex = static_cast<uint32>(Filter.SectionFirstIndex[SectionIndex]);
			RenderSection.NumTriangles = static_cast<uint32>(Filter.SectionIndexCount[SectionIndex] / 3);
			RenderSection.BaseVertexIndex = SectionBaseVertex[SectionIndex];
			RenderSection.NumVertices = SectionNumVertices[SectionIndex];
			RenderSection.MaxBoneInfluences = LOD.NumInfluences;
			RenderSection.BoneMap = BoneMap;
			RenderSection.bCastShadow = true;
			RenderSection.bVisibleInRayTracing = true;
			RenderSection.bRecomputeTangent = false;
			RenderSection.CorrespondClothAssetIndex = INDEX_NONE;
			// A section the filter emptied stays (slots keep their meaning) but draws nothing.
			RenderSection.bDisabled = RenderSection.NumTriangles == 0;
			// No overlapping-vertex data; the engine still expects a (trivial) buffer per section.
			RenderSection.DuplicatedVerticesBuffer.Init(static_cast<int32>(RenderSection.NumVertices), TMap<int32, TArray<int32>>());
		}

		// The weighted bones and their parents are active; every bone is required so the whole skeleton animates.
		LODData.ActiveBoneIndices = BoneMap;
		BuiltBones.EnsureParentsExistAndSort(LODData.ActiveBoneIndices);
		LODData.RequiredBones.SetNumUninitialized(NumBones);
		for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
		{
			LODData.RequiredBones[BoneIndex] = static_cast<FBoneIndexType>(BoneIndex);
		}

		// Static vertex buffers. The precision flags pick the storage layout, so they come before Init.
		FStaticMeshVertexBuffers& VertexBuffers = LODData.StaticVertexBuffers;
		VertexBuffers.PositionVertexBuffer.Init(LOD.Positions, bNeedsCPUAccess);
		VertexBuffers.StaticMeshVertexBuffer.SetUseFullPrecisionUVs(LOD.bFullPrecisionUVs);
		VertexBuffers.StaticMeshVertexBuffer.SetUseHighPrecisionTangentBasis(LOD.bHighPrecisionTangents);
		// Hair cards (MSK1 flag 16) add two UV channels for the hair material: UV1 = the card's root UV,
		// UV2.x = CoordU (0 at the root, 1 at the tip).
		const bool bHairCards = LOD.HairCards.Num() == N;
		VertexBuffers.StaticMeshVertexBuffer.Init(N, bHairCards ? 3 : 1, bNeedsCPUAccess);
		for (int32 Vertex = 0; Vertex < N; ++Vertex)
		{
			const FVector4f& Tangent = Tangents[Vertex];
			const FVector3f TangentX(Tangent.X, Tangent.Y, Tangent.Z);
			const FVector3f TangentZ = LOD.Normals[Vertex];
			// The binormal the engine rebuilds on unpacking, so SetVertexTangents stores the same sign.
			const FVector3f TangentY = (TangentZ ^ TangentX) * (Tangent.W < 0.0f ? -1.0f : 1.0f);
			VertexBuffers.StaticMeshVertexBuffer.SetVertexTangents(Vertex, TangentX, TangentY, TangentZ);
			VertexBuffers.StaticMeshVertexBuffer.SetVertexUV(Vertex, 0, LOD.UVs[Vertex]);
			if (bHairCards)
			{
				const FVector3f& Card = LOD.HairCards[Vertex];
				VertexBuffers.StaticMeshVertexBuffer.SetVertexUV(Vertex, 1, FVector2f(Card.X, Card.Y));
				VertexBuffers.StaticMeshVertexBuffer.SetVertexUV(Vertex, 2, FVector2f(Card.Z, 0.0f));
			}
		}

		// Skin weights: format first, then the assignment allocates and fills the buffer from it.
		FSkinWeightVertexBuffer& WeightBuffer = LODData.SkinWeightVertexBuffer;
		WeightBuffer.SetNeedsCPUAccess(bNeedsCPUAccess);
		WeightBuffer.SetMaxBoneInfluences(LOD.NumInfluences);
		WeightBuffer.SetUse16BitBoneIndex(BoneMap.Num() > 256);
		WeightBuffer.SetUse16BitBoneWeight(false);
		WeightBuffer = Weights;

		// Index buffer (RebuildIndexBuffer keeps a CPU copy). Never empty: see the class comment.
		TArray<uint32> KeptIndices = MoveTemp(Filter.Indices);
		if (KeptIndices.Num() == 0)
		{
			KeptIndices = { 0, 0, 0 };
		}
		const uint8 IndexSize = static_cast<uint8>(static_cast<uint32>(N - 1) < static_cast<uint32>(MAX_uint16) ? sizeof(uint16) : sizeof(uint32));
		LODData.MultiSizeIndexContainer.RebuildIndexBuffer(IndexSize, KeptIndices);
	}
}

float FMantleRuntimeSkeletalMeshBuilder::DefaultLODScreenSize(const int32 LODIndex)
{
	using namespace MantleSkeletalMeshBuilderPrivate;
	if (LODIndex <= 0)
	{
		return LODScreenSizes[0];
	}
	if (LODIndex < MaxLODCount)
	{
		return LODScreenSizes[LODIndex];
	}
	return LODScreenSizes[MaxLODCount - 1] * FMath::Pow(0.5f, static_cast<float>(LODIndex - MaxLODCount + 1));
}

bool FMantleRuntimeSkeletalMeshBuilder::TransferHiddenVertices(const FMantleSkinnedMeshLOD& From, const TBitArray<>& Hidden, const FMantleSkinnedMeshLOD& To,
	TBitArray<>& Out, FString* OutError)
{
	const int32 NumFrom = From.NumVertices();
	const int32 NumTo = To.NumVertices();
	if (Hidden.Num() != NumFrom)
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("the hidden-vertex set has %d bits for %d vertices"), Hidden.Num(), NumFrom);
		}
		return false;
	}
	Out.Init(false, NumTo);
	if (NumFrom == 0 || NumTo == 0 || Hidden.Find(true) == INDEX_NONE)
	{
		return true;
	}

	// A uniform grid over From, about one vertex per cell, searched ring by ring from To's cell: after ring R
	// every unsearched vertex is at least R cells away, so the search stops once the best is that close.
	FBox3f Box(ForceInit);
	for (const FVector3f& Position : From.Positions)
	{
		Box += Position;
	}
	const FVector3f Size = Box.GetSize();
	const float Longest = FMath::Max3(Size.X, Size.Y, Size.Z);
	const float Cell = FMath::Max(Longest / FMath::Max(1.0f, FMath::Pow(static_cast<float>(NumFrom), 1.0f / 3.0f)), 1.0e-3f);
	auto CellOf = [&Box, Cell](const FVector3f& P)
	{
		return FIntVector(FMath::FloorToInt32((P.X - Box.Min.X) / Cell), FMath::FloorToInt32((P.Y - Box.Min.Y) / Cell), FMath::FloorToInt32((P.Z - Box.Min.Z) / Cell));
	};
	TMap<FIntVector, TArray<int32>> Grid;
	for (int32 Vertex = 0; Vertex < NumFrom; ++Vertex)
	{
		Grid.FindOrAdd(CellOf(From.Positions[Vertex])).Add(Vertex);
	}
	const int32 GridSpan = FMath::CeilToInt32(Longest / Cell) + 1;

	for (int32 Vertex = 0; Vertex < NumTo; ++Vertex)
	{
		const FVector3f P = To.Positions[Vertex];
		const FIntVector Center = CellOf(P);
		// Past the grid's own span plus how far outside it P lies, every cell is empty.
		const int32 Outside = FMath::Max3(FMath::Max(-Center.X, Center.X - GridSpan), FMath::Max(-Center.Y, Center.Y - GridSpan), FMath::Max(-Center.Z, Center.Z - GridSpan));
		const int32 MaxRing = GridSpan + FMath::Max(Outside, 0) + 1;
		int32 Best = INDEX_NONE;
		float BestDistanceSq = TNumericLimits<float>::Max();
		if (MaxRing > 64)
		{
			// Far off the grid (a LOD that is not LOD0's surface): every vertex.
			for (int32 Candidate = 0; Candidate < NumFrom; ++Candidate)
			{
				const float DistanceSq = FVector3f::DistSquared(P, From.Positions[Candidate]);
				if (DistanceSq < BestDistanceSq)
				{
					BestDistanceSq = DistanceSq;
					Best = Candidate;
				}
			}
		}
		else
		{
			for (int32 Ring = 0; Ring <= MaxRing; ++Ring)
			{
				for (int32 DX = -Ring; DX <= Ring; ++DX)
				{
					for (int32 DY = -Ring; DY <= Ring; ++DY)
					{
						const bool bEdge = FMath::Abs(DX) == Ring || FMath::Abs(DY) == Ring;
						for (int32 DZ = -Ring; DZ <= Ring; DZ += (bEdge || Ring == 0) ? 1 : 2 * Ring)
						{
							const TArray<int32>* Bucket = Grid.Find(Center + FIntVector(DX, DY, DZ));
							if (!Bucket)
							{
								continue;
							}
							for (const int32 Candidate : *Bucket)
							{
								const float DistanceSq = FVector3f::DistSquared(P, From.Positions[Candidate]);
								if (DistanceSq < BestDistanceSq || (DistanceSq == BestDistanceSq && Candidate < Best))
								{
									BestDistanceSq = DistanceSq;
									Best = Candidate;
								}
							}
						}
					}
				}
				if (Best != INDEX_NONE && BestDistanceSq <= FMath::Square(Ring * Cell))
				{
					break;
				}
			}
		}
		Out[Vertex] = Best != INDEX_NONE && Hidden[Best];
	}
	return true;
}

bool FMantleRuntimeSkeletalMeshBuilder::FilterTriangles(const FMantleSkinnedMeshLOD& LOD, const TBitArray<>* HiddenVertices,
	FMantleTriangleFilterResult& Out, FString* OutError)
{
	Out = FMantleTriangleFilterResult();
	const int32 N = LOD.NumVertices();
	if (HiddenVertices && HiddenVertices->Num() != N)
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("the hidden-vertex set has %d bits for %d vertices"), HiddenVertices->Num(), N);
		}
		return false;
	}

	Out.Indices.Reserve(LOD.Indices.Num());
	Out.SectionFirstIndex.Reserve(LOD.Sections.Num());
	Out.SectionIndexCount.Reserve(LOD.Sections.Num());
	for (int32 SectionIndex = 0; SectionIndex < LOD.Sections.Num(); ++SectionIndex)
	{
		const FMantleSkinnedMeshSection& Section = LOD.Sections[SectionIndex];
		if (Section.FirstIndex < 0 || Section.IndexCount < 0 || static_cast<int64>(Section.FirstIndex) + Section.IndexCount > LOD.Indices.Num())
		{
			if (OutError)
			{
				*OutError = FString::Printf(TEXT("section %d covers indices outside the index buffer"), SectionIndex);
			}
			return false;
		}
		Out.SectionFirstIndex.Add(Out.Indices.Num());
		int32 Kept = 0;
		for (int32 First = Section.FirstIndex; First + 2 < Section.FirstIndex + Section.IndexCount; First += 3)
		{
			const uint32 A = LOD.Indices[First];
			const uint32 B = LOD.Indices[First + 1];
			const uint32 C = LOD.Indices[First + 2];
			if (A >= static_cast<uint32>(N) || B >= static_cast<uint32>(N) || C >= static_cast<uint32>(N))
			{
				if (OutError)
				{
					*OutError = FString::Printf(TEXT("section %d has an index out of range at %d"), SectionIndex, First);
				}
				return false;
			}
			if (HiddenVertices && (*HiddenVertices)[static_cast<int32>(A)] && (*HiddenVertices)[static_cast<int32>(B)] && (*HiddenVertices)[static_cast<int32>(C)])
			{
				++Out.DroppedTriangles;
				continue;
			}
			Out.Indices.Add(A);
			Out.Indices.Add(B);
			Out.Indices.Add(C);
			++Kept;
		}
		Out.SectionIndexCount.Add(Kept * 3);
		Out.KeptTriangles += Kept;
	}
	return true;
}

void FMantleRuntimeSkeletalMeshBuilder::DeriveTangents(const FMantleSkinnedMeshLOD& LOD, TArray<FVector4f>& OutTangents)
{
	MantleSkeletalMeshBuilderPrivate::DeriveTangents(LOD, OutTangents);
}

USkeletalMesh* FMantleRuntimeSkeletalMeshBuilder::Build(const FMantleSkinnedMesh& Mesh, const FMantleSkinnedMeshBuildParams& Params,
	FString* OutError)
{
	using namespace MantleSkeletalMeshBuilderPrivate;

	FString Invalid;
	if (!Mesh.Validate(Invalid))
	{
		return Refuse(OutError, FString::Printf(TEXT("invalid mesh data: %s"), *Invalid));
	}
	// Garments supplied with a wearer's pose can contain helper bones omitted by that wearer.
	// Normalize through the same ancestor/weight-merge policy as the production fitter before
	// constructing render buffers. Avatar builds on their own pose remain strict.
	if (Params.bAllowMissingHelperBones && Params.ReferencePose && !Params.bOwnSkeleton)
	{
		const bool bHasMissingBone = Mesh.Bones.ContainsByPredicate([&Params](const FMantleSkinnedMeshBone& Bone)
		{
			return Params.ReferencePose->FindRawBoneIndex(Bone.Name) == INDEX_NONE;
		});
		if (bHasMissingBone)
		{
			FMantleSkinnedMesh Rebound = Mesh;
			if (!FMantleGarmentFitter::Rebind(Rebound, FMantleReferencePose::FromReferenceSkeleton(*Params.ReferencePose), &Invalid))
			{
				return Refuse(OutError, Invalid);
			}
			return Build(Rebound, Params, OutError);
		}
	}
	if (Params.bOwnSkeleton && (Params.Skeleton || Params.ReferencePose))
	{
		return Refuse(OutError, TEXT("a mesh built on its own skeleton takes no supplied skeleton or reference pose"));
	}
	if (!Params.Skeleton && !Params.bOwnSkeleton)
	{
		return Refuse(OutError, TEXT("no skeleton to build on"));
	}
	const FMantleSkinnedMeshLOD& LOD0 = Mesh.LODs[0];
	if (Params.HiddenVertices && Params.HiddenVertices->Num() != LOD0.NumVertices())
	{
		return Refuse(OutError, FString::Printf(TEXT("the hidden-vertex set has %d bits for %d vertices"), Params.HiddenVertices->Num(), LOD0.NumVertices()));
	}
	const int32 RequestedLODs = Params.MaxLODs > 0 ? Params.MaxLODs : CVarMantleMaxBuiltLODs.GetValueOnAnyThread();
	const int32 NumLODs = FMath::Clamp(RequestedLODs, 1, FMath::Min(Mesh.LODs.Num(), static_cast<int32>(MaxLODCount)));

	// --- The built mesh's bone table: its own (avatars) or the supplied reference skeleton (garments). ---
	TArray<FName> BoneNames;
	TArray<int32> BoneParents;
	TArray<FTransform> BonePose;
	TArray<int32> MeshToBuilt;
	MeshToBuilt.SetNumUninitialized(Mesh.Bones.Num());
	if (Params.ReferencePose)
	{
		const TArray<FMeshBoneInfo>& SuppliedInfo = Params.ReferencePose->GetRawRefBoneInfo();
		const TArray<FTransform>& SuppliedPose = Params.ReferencePose->GetRawRefBonePose();
		if (SuppliedInfo.Num() == 0 || SuppliedPose.Num() != SuppliedInfo.Num())
		{
			return Refuse(OutError, TEXT("the supplied reference pose is empty"));
		}
		for (int32 BoneIndex = 0; BoneIndex < SuppliedInfo.Num(); ++BoneIndex)
		{
			BoneNames.Add(SuppliedInfo[BoneIndex].Name);
			BoneParents.Add(SuppliedInfo[BoneIndex].ParentIndex);
			BonePose.Add(SuppliedPose[BoneIndex]);
		}
		for (int32 BoneIndex = 0; BoneIndex < Mesh.Bones.Num(); ++BoneIndex)
		{
			MeshToBuilt[BoneIndex] = Params.ReferencePose->FindRawBoneIndex(Mesh.Bones[BoneIndex].Name);
			if (MeshToBuilt[BoneIndex] == INDEX_NONE)
			{
				return Refuse(OutError, FString::Printf(TEXT("unknown bone %s (mesh bone %d): not in the supplied reference pose"),
					*Mesh.Bones[BoneIndex].Name.ToString(), BoneIndex));
			}
		}
	}
	else
	{
		if (!Mesh.HasReferencePose())
		{
			return Refuse(OutError, TEXT("the mesh has no reference pose of its own and none was supplied"));
		}
		for (int32 BoneIndex = 0; BoneIndex < Mesh.Bones.Num(); ++BoneIndex)
		{
			BoneNames.Add(Mesh.Bones[BoneIndex].Name);
			BoneParents.Add(Mesh.Bones[BoneIndex].ParentIndex);
			BonePose.Add(Mesh.ReferencePose[BoneIndex]);
			MeshToBuilt[BoneIndex] = BoneIndex;
		}
	}
	const int32 NumBones = BoneNames.Num();
	if (NumBones > MAX_uint16)
	{
		return Refuse(OutError, FString::Printf(TEXT("%d bones is more than a skeletal mesh can index"), NumBones));
	}

	// --- Every bone on the skeleton, by name, under an ancestor the skeleton agrees with. A mesh on its own
	// skeleton is its skeleton: nothing to agree with. ---
	TArray<int32> SkeletonIndex;
	SkeletonIndex.SetNumUninitialized(NumBones);
	for (int32 BoneIndex = 0; BoneIndex < NumBones && Params.Skeleton; ++BoneIndex)
	{
		const FReferenceSkeleton& SkeletonBones = Params.Skeleton->GetReferenceSkeleton();
		SkeletonIndex[BoneIndex] = SkeletonBones.FindRawBoneIndex(BoneNames[BoneIndex]);
		if (SkeletonIndex[BoneIndex] == INDEX_NONE)
		{
			return Refuse(OutError, FString::Printf(TEXT("unknown bone %s: not on skeleton %s"),
				*BoneNames[BoneIndex].ToString(), *Params.Skeleton->GetName()));
		}
		const int32 Parent = BoneParents[BoneIndex];
		if (Parent != INDEX_NONE && !IsAncestorOf(SkeletonBones, SkeletonIndex[Parent], SkeletonIndex[BoneIndex]))
		{
			return Refuse(OutError, FString::Printf(TEXT("bone %s is under %s in the mesh but not on skeleton %s"),
				*BoneNames[BoneIndex].ToString(), *BoneNames[Parent].ToString(), *Params.Skeleton->GetName()));
		}
	}

	// --- Kept triangles per LOD. LOD0 takes the hidden bits as given; each lower LOD its nearest LOD0 vertex's. ---
	TArray<TBitArray<>> LowerHidden;
	LowerHidden.SetNum(NumLODs);
	TArray<FMantleTriangleFilterResult> Filters;
	Filters.SetNum(NumLODs);
	for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
	{
		const TBitArray<>* Hidden = LODIndex == 0 ? Params.HiddenVertices : nullptr;
		FString LODError;
		if (LODIndex > 0 && Params.LowerLODHiddenVertices.IsValidIndex(LODIndex - 1) && Params.LowerLODHiddenVertices[LODIndex - 1].Num() > 0)
		{
			// The caller's own bits for this LOD (FilterTriangles checks the count).
			Hidden = &Params.LowerLODHiddenVertices[LODIndex - 1];
		}
		else if (LODIndex > 0 && Params.HiddenVertices)
		{
			if (!TransferHiddenVertices(LOD0, *Params.HiddenVertices, Mesh.LODs[LODIndex], LowerHidden[LODIndex], &LODError))
			{
				return Refuse(OutError, FString::Printf(TEXT("LOD%d: %s"), LODIndex, *LODError));
			}
			Hidden = &LowerHidden[LODIndex];
		}
		if (!FilterTriangles(Mesh.LODs[LODIndex], Hidden, Filters[LODIndex], &LODError))
		{
			return Refuse(OutError, NumLODs > 1 ? FString::Printf(TEXT("LOD%d: %s"), LODIndex, *LODError) : LODError);
		}
	}

	// --- The mesh object, its skeleton and reference skeleton (FSkeletalMeshMerge's order). ---
	UObject* MeshOuter = Params.Outer ? Params.Outer : GetTransientPackage();
	const FName MeshName = Params.Name.IsNone()
		? NAME_None
		: MakeUniqueObjectName(MeshOuter, USkeletalMesh::StaticClass(), Params.Name);
	USkeletalMesh* Built = NewObject<USkeletalMesh>(MeshOuter, MeshName, RF_Transient);
	// A mesh on its own skeleton gets a new, empty one here, filled from the built bones below.
	USkeleton* Skeleton = Params.bOwnSkeleton ? NewObject<USkeleton>(MeshOuter, NAME_None, RF_Transient) : Params.Skeleton;
	Built->SetSkeleton(Skeleton);
	{
		FReferenceSkeleton BuiltBones;
		{
			// Rebuilds the name map and the final (virtual bone) arrays when it goes out of scope.
			FReferenceSkeletonModifier Modifier(BuiltBones, Skeleton);
			for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
			{
				Modifier.Add(FMeshBoneInfo(BoneNames[BoneIndex], BoneNames[BoneIndex].ToString(), BoneParents[BoneIndex]), BonePose[BoneIndex]);
				// Add renormalizes the rotation; the given pose is kept exactly (Validate checked it
				// is unit length), so a round trip reproduces the source's reference pose bit for bit.
				Modifier.UpdateRefPoseTransform(BoneIndex, BonePose[BoneIndex]);
			}
		}
		Built->SetRefSkeleton(BuiltBones);
	}
	// Skinning needs the inverse reference matrices before anything renders.
	Built->GetRefBasesInvMatrix().Empty();
	Built->CalculateInvRefMatrices();
	if (Params.bOwnSkeleton && !Skeleton->MergeAllBonesToBoneTree(Built, /*bShowProgress*/ false))
	{
		return Refuse(OutError, TEXT("could not make a skeleton of the mesh's own bones"));
	}
	// A cooked build finds sockets only through the mesh's socket map, which a loaded asset builds in PostLoad and a
	// built one never does: without this, an uploaded avatar has no acc_* socket in Shipping (the editor walks the
	// skeleton's sockets instead). After the reference skeleton, which the map's bone indices come from.
	Built->RebuildSocketMap();

	// Material slots, padded so every section's slot exists on every built LOD. A padded slot takes the material
	// name of a section drawn with it (MSK1 carries one per section, and a slot means the same on every LOD), so
	// callers can bind materials by name.
	TArray<FSkeletalMaterial> Materials = Params.Materials;
	int32 NumSlots = FMath::Max(Materials.Num(), 1);
	for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
	{
		for (const FMantleSkinnedMeshSection& Section : Mesh.LODs[LODIndex].Sections)
		{
			NumSlots = FMath::Max(NumSlots, Section.MaterialIndex + 1);
		}
	}
	while (Materials.Num() < NumSlots)
	{
		const int32 SlotIndex = Materials.Num();
		FSkeletalMaterial& Slot = Materials.AddDefaulted_GetRef();
		Slot.MaterialSlotName = FName(*FString::Printf(TEXT("Slot%d"), SlotIndex));
		bool bNamed = false;
		for (int32 LODIndex = 0; LODIndex < NumLODs && !bNamed; ++LODIndex)
		{
			for (const FMantleSkinnedMeshSection& Section : Mesh.LODs[LODIndex].Sections)
			{
				if (Section.MaterialIndex == SlotIndex && !Section.MaterialName.IsNone())
				{
					Slot.MaterialSlotName = Section.MaterialName;
					bNamed = true;
					break;
				}
			}
		}
	}
	Built->SetMaterials(Materials);
	Built->SetHasVertexColors(false);

	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	Built->AllocateResourceForRendering();
	FSkeletalMeshRenderData* RenderData = Built->GetResourceForRendering();

	// bAllowCPUAccess plus the CPU-access buffers keep the CPU copies after the RHI upload, on every LOD (unless
	// the caller reads nothing back: Params.bKeepCPUData).
	const bool bNeedsCPUAccess = Params.bKeepCPUData;
	for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
	{
		const FMantleSkinnedMeshLOD& LOD = Mesh.LODs[LODIndex];

		FSkeletalMeshLODInfo& LODInfo = Built->AddLODInfo();
		LODInfo.ScreenSize.Default = Params.LODScreenSizes.IsValidIndex(LODIndex) ? Params.LODScreenSizes[LODIndex] : DefaultLODScreenSize(LODIndex);
		LODInfo.LODHysteresis = 0.0f;
		LODInfo.LODMaterialMap.Reset();
		LODInfo.bAllowCPUAccess = bNeedsCPUAccess;
		LODInfo.BuildSettings.bUseFullPrecisionUVs = LOD.bFullPrecisionUVs;
		LODInfo.BuildSettings.bUseHighPrecisionTangentBasis = LOD.bHighPrecisionTangents;
		LODInfo.BuildSettings.bUseHighPrecisionSkinWeights = false;
		LODInfo.BuildSettings.bUseBackwardsCompatibleF16TruncUVs = false;
		// The render data is filled directly, but a later build must not halve it: the default reduction settings are half the triangles.
		LODInfo.ReductionSettings.NumOfTrianglesPercentage = 1.0f;
		LODInfo.ReductionSettings.NumOfVertPercentage = 1.0f;

		// Owned by the render data's indirect array from here on (as in FSkeletalMeshMerge::GenerateLODModel).
		FSkeletalMeshLODRenderData* LODData = new FSkeletalMeshLODRenderData();
		RenderData->LODRenderData.Add(LODData);
		FillLODRenderData(LOD, Filters[LODIndex], MeshToBuilt, NumBones, Built->GetRefSkeleton(), bNeedsCPUAccess, *LODData);
	}

	// Every LOD inline and always resident: there is nothing to stream them from.
	RenderData->NumInlinedLODs = static_cast<uint8>(NumLODs);
	RenderData->NumNonOptionalLODs = static_cast<uint8>(NumLODs);
	// Per-section bone maps, no unified map: this leaves the render data's unified map empty.
	RenderData->InitUnifiedBoneMapFromLODs();
	PRAGMA_ENABLE_DEPRECATION_WARNINGS

	const FBox3f Bounds = Mesh.ComputeBounds(0);
	Built->SetImportedBounds(FBoxSphereBounds(FBox(FVector(Bounds.Min), FVector(Bounds.Max))));
	Built->NeverStream = true;

	// Creates the RHI resources (render thread) so the mesh can go on a component.
	Built->InitResources();
	return Built;
}
