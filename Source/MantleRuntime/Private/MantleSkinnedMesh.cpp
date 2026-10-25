// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSkinnedMesh.h"

#include "MantleRuntime.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "ReferenceSkeleton.h"
#include "Rendering/MultiSizeIndexContainer.h"
#include "Rendering/PositionVertexBuffer.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "Rendering/StaticMeshVertexBuffer.h"

// Named, not anonymous: MantleRuntime builds as a unity build, where anonymous namespaces of
// different files share one translation unit.
namespace MantleSkinnedMeshPrivate
{
	/** Material slots a section may name. The engine stores a section's slot in 16 bits; nothing real comes close. */
	constexpr int32 MaxMaterialSlots = 256;

	bool Reject(FString& OutError, const FString& Error)
	{
		OutError = Error;
		return false;
	}

	bool ValidateLOD(const FMantleSkinnedMeshLOD& LOD, const int32 LODIndex, const int32 NumBones, FString& OutError)
	{
		const int32 N = LOD.NumVertices();
		if (N == 0)
		{
			return Reject(OutError, FString::Printf(TEXT("LOD%d has no vertices"), LODIndex));
		}
		if (LOD.Normals.Num() != N)
		{
			return Reject(OutError, FString::Printf(TEXT("LOD%d has %d normals for %d vertices"), LODIndex, LOD.Normals.Num(), N));
		}
		if (LOD.Tangents.Num() != 0 && LOD.Tangents.Num() != N)
		{
			return Reject(OutError, FString::Printf(TEXT("LOD%d has %d tangents for %d vertices (0 or one per vertex)"), LODIndex, LOD.Tangents.Num(), N));
		}
		if (LOD.UVs.Num() != N)
		{
			return Reject(OutError, FString::Printf(TEXT("LOD%d has %d UVs for %d vertices"), LODIndex, LOD.UVs.Num(), N));
		}
		if (LOD.HairCards.Num() != 0 && LOD.HairCards.Num() != N)
		{
			return Reject(OutError, FString::Printf(TEXT("LOD%d has hair card data for %d of %d vertices (none or one per vertex)"), LODIndex, LOD.HairCards.Num(), N));
		}
		for (int32 Vertex = 0; Vertex < N; ++Vertex)
		{
			if (LOD.Positions[Vertex].ContainsNaN())
			{
				return Reject(OutError, FString::Printf(TEXT("LOD%d vertex %d has a non-finite position"), LODIndex, Vertex));
			}
		}
		for (int32 Vertex = 0; Vertex < LOD.HairCards.Num(); ++Vertex)
		{
			if (LOD.HairCards[Vertex].ContainsNaN())
			{
				return Reject(OutError, FString::Printf(TEXT("LOD%d vertex %d has non-finite hair card data"), LODIndex, Vertex));
			}
		}

		if (LOD.NumInfluences < 1 || LOD.NumInfluences > MAX_TOTAL_INFLUENCES)
		{
			return Reject(OutError, FString::Printf(TEXT("LOD%d has %d influences per vertex (1 to %d)"), LODIndex, LOD.NumInfluences, MAX_TOTAL_INFLUENCES));
		}
		const int64 NumSlots = static_cast<int64>(N) * LOD.NumInfluences;
		if (LOD.InfluenceBones.Num() != NumSlots || LOD.InfluenceWeights.Num() != NumSlots)
		{
			return Reject(OutError, FString::Printf(TEXT("LOD%d has %d influence bones and %d weights; %d vertices x %d influences needs %lld of each"),
				LODIndex, LOD.InfluenceBones.Num(), LOD.InfluenceWeights.Num(), N, LOD.NumInfluences, NumSlots));
		}
		for (int32 Vertex = 0; Vertex < N; ++Vertex)
		{
			int32 Sum = 0;
			for (int32 Slot = 0; Slot < LOD.NumInfluences; ++Slot)
			{
				const int32 Index = Vertex * LOD.NumInfluences + Slot;
				if (LOD.InfluenceBones[Index] >= NumBones)
				{
					return Reject(OutError, FString::Printf(TEXT("LOD%d vertex %d influence %d names bone %d, but the bone table has %d bones"),
						LODIndex, Vertex, Slot, static_cast<int32>(LOD.InfluenceBones[Index]), NumBones));
				}
				Sum += LOD.InfluenceWeights[Index];
			}
			if (Sum != 255)
			{
				return Reject(OutError, FString::Printf(TEXT("LOD%d vertex %d weights sum to %d, not 255"), LODIndex, Vertex, Sum));
			}
		}

		if (LOD.Indices.Num() == 0 || LOD.Indices.Num() % 3 != 0)
		{
			return Reject(OutError, FString::Printf(TEXT("LOD%d has %d indices (a non-zero multiple of 3)"), LODIndex, LOD.Indices.Num()));
		}
		for (int32 Position = 0; Position < LOD.Indices.Num(); ++Position)
		{
			if (LOD.Indices[Position] >= static_cast<uint32>(N))
			{
				return Reject(OutError, FString::Printf(TEXT("LOD%d index %d is %u, out of range for %d vertices"),
					LODIndex, Position, LOD.Indices[Position], N));
			}
		}

		if (LOD.Sections.Num() == 0)
		{
			return Reject(OutError, FString::Printf(TEXT("LOD%d has no sections"), LODIndex));
		}
		for (int32 SectionIndex = 0; SectionIndex < LOD.Sections.Num(); ++SectionIndex)
		{
			const FMantleSkinnedMeshSection& Section = LOD.Sections[SectionIndex];
			if (Section.FirstIndex < 0 || Section.IndexCount < 0 || Section.FirstIndex % 3 != 0 || Section.IndexCount % 3 != 0
				|| static_cast<int64>(Section.FirstIndex) + Section.IndexCount > LOD.Indices.Num())
			{
				return Reject(OutError, FString::Printf(TEXT("LOD%d section %d covers indices [%d, +%d), not whole triangles inside the %d indices"),
					LODIndex, SectionIndex, Section.FirstIndex, Section.IndexCount, LOD.Indices.Num()));
			}
			if (Section.MaterialIndex < 0 || Section.MaterialIndex >= MaxMaterialSlots)
			{
				return Reject(OutError, FString::Printf(TEXT("LOD%d section %d uses material slot %d (0 to %d)"),
					LODIndex, SectionIndex, Section.MaterialIndex, MaxMaterialSlots - 1));
			}
		}
		return true;
	}
}

FBox3f FMantleSkinnedMesh::ComputeBounds(const int32 LODIndex) const
{
	FBox3f Box(ForceInit);
	if (LODs.IsValidIndex(LODIndex))
	{
		for (const FVector3f& Position : LODs[LODIndex].Positions)
		{
			Box += Position;
		}
	}
	return Box;
}

bool FMantleSkinnedMesh::Validate(FString& OutError) const
{
	using namespace MantleSkinnedMeshPrivate;

	const int32 NumBones = Bones.Num();
	if (NumBones == 0 || NumBones > MAX_uint16)
	{
		return Reject(OutError, FString::Printf(TEXT("the bone table has %d bones (1 to %d)"), NumBones, static_cast<int32>(MAX_uint16)));
	}
	TSet<FName> Names;
	Names.Reserve(NumBones);
	for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
	{
		const FMantleSkinnedMeshBone& Bone = Bones[BoneIndex];
		if (Bone.Name.IsNone())
		{
			return Reject(OutError, FString::Printf(TEXT("bone %d has no name"), BoneIndex));
		}
		bool bDuplicate = false;
		Names.Add(Bone.Name, &bDuplicate);
		if (bDuplicate)
		{
			return Reject(OutError, FString::Printf(TEXT("bone %d (%s) appears twice in the bone table"), BoneIndex, *Bone.Name.ToString()));
		}
		const bool bParentValid = BoneIndex == 0
			? Bone.ParentIndex == INDEX_NONE
			: (Bone.ParentIndex >= 0 && Bone.ParentIndex < BoneIndex);
		if (!bParentValid)
		{
			return Reject(OutError, FString::Printf(TEXT("bone %d (%s) has parent %d: the table must be parent-first with one root"),
				BoneIndex, *Bone.Name.ToString(), Bone.ParentIndex));
		}
	}

	if (ReferencePose.Num() != 0)
	{
		if (ReferencePose.Num() != NumBones)
		{
			return Reject(OutError, FString::Printf(TEXT("the reference pose has %d transforms for %d bones"), ReferencePose.Num(), NumBones));
		}
		for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
		{
			const FTransform& Local = ReferencePose[BoneIndex];
			if (Local.ContainsNaN() || !Local.GetRotation().IsNormalized())
			{
				return Reject(OutError, FString::Printf(TEXT("bone %d (%s) has a non-finite reference transform or a rotation that is not unit length"),
					BoneIndex, *Bones[BoneIndex].Name.ToString()));
			}
		}
	}

	if (LODs.Num() == 0)
	{
		return Reject(OutError, TEXT("the mesh has no LODs"));
	}
	for (int32 LODIndex = 0; LODIndex < LODs.Num(); ++LODIndex)
	{
		if (!ValidateLOD(LODs[LODIndex], LODIndex, NumBones, OutError))
		{
			return false;
		}
	}
	return true;
}

bool FMantleSkinnedMesh::ExtractFromSkeletalMesh(const USkeletalMesh* Source, FMantleSkinnedMesh& Out,
	FString* OutError, TArray<FSkeletalMaterial>* OutMaterials)
{
	Out = FMantleSkinnedMesh();
	if (OutMaterials)
	{
		OutMaterials->Reset();
	}
	auto Refuse = [OutError](const FString& Error)
	{
		UE_LOG(LogMantle, Error, TEXT("MANTLE_SKINNEDMESH FAIL extract: %s"), *Error);
		if (OutError)
		{
			*OutError = Error;
		}
		return false;
	};
	if (!Source)
	{
		return Refuse(TEXT("no source mesh"));
	}

	const FSkeletalMeshRenderData* RenderData = Source->GetResourceForRendering();
	if (!RenderData || RenderData->LODRenderData.Num() == 0)
	{
		return Refuse(FString::Printf(TEXT("%s has no render data"), *Source->GetName()));
	}
	const FSkeletalMeshLODRenderData& SourceLOD = RenderData->LODRenderData[0];
	const FPositionVertexBuffer& PositionBuffer = SourceLOD.StaticVertexBuffers.PositionVertexBuffer;
	const FStaticMeshVertexBuffer& VertexBuffer = SourceLOD.StaticVertexBuffers.StaticMeshVertexBuffer;
	// The asset's own weights, not a skin weight profile's override.
	const FSkinWeightVertexBuffer& WeightBuffer = SourceLOD.SkinWeightVertexBuffer;

	const uint32 N = PositionBuffer.GetNumVertices();
	if (N == 0 || PositionBuffer.GetVertexData() == nullptr)
	{
		return Refuse(FString::Printf(TEXT("%s LOD0 has no CPU position data (cooked without bAllowCPUAccess?)"), *Source->GetName()));
	}
	if (VertexBuffer.GetNumVertices() != N || VertexBuffer.GetTangentData() == nullptr
		|| VertexBuffer.GetNumTexCoords() == 0 || VertexBuffer.GetTexCoordData() == nullptr)
	{
		return Refuse(FString::Printf(TEXT("%s LOD0 has no CPU tangent or UV data"), *Source->GetName()));
	}
	const FSkinWeightDataVertexBuffer* WeightData = WeightBuffer.GetDataVertexBuffer();
	if (WeightBuffer.GetNumVertices() != N || !WeightData || WeightData->GetWeightData() == nullptr)
	{
		return Refuse(FString::Printf(TEXT("%s LOD0 has no CPU skin weight data (%u weighted vertices for %u)"),
			*Source->GetName(), WeightBuffer.GetNumVertices(), N));
	}
	if (!SourceLOD.MultiSizeIndexContainer.IsIndexBufferValid())
	{
		return Refuse(FString::Printf(TEXT("%s LOD0 has no index buffer"), *Source->GetName()));
	}

	FMantleSkinnedMeshLOD& LOD = Out.LODs.AddDefaulted_GetRef();
	SourceLOD.MultiSizeIndexContainer.GetIndexBuffer(LOD.Indices);
	if (LOD.Indices.Num() == 0)
	{
		return Refuse(FString::Printf(TEXT("%s LOD0 has no CPU index data"), *Source->GetName()));
	}
	LOD.bFullPrecisionUVs = VertexBuffer.GetUseFullPrecisionUVs();
	LOD.bHighPrecisionTangents = VertexBuffer.GetUseHighPrecisionTangentBasis();

	LOD.Positions.SetNumUninitialized(N);
	LOD.Normals.SetNumUninitialized(N);
	LOD.Tangents.SetNumUninitialized(N);
	LOD.UVs.SetNumUninitialized(N);
	for (uint32 Vertex = 0; Vertex < N; ++Vertex)
	{
		LOD.Positions[Vertex] = PositionBuffer.VertexPosition(Vertex);
		const FVector4f TangentX = VertexBuffer.VertexTangentX(Vertex);
		const FVector4f TangentZ = VertexBuffer.VertexTangentZ(Vertex);
		LOD.Normals[Vertex] = FVector3f(TangentZ.X, TangentZ.Y, TangentZ.Z);
		// The packed normal's W is the binormal sign.
		LOD.Tangents[Vertex] = FVector4f(TangentX.X, TangentX.Y, TangentX.Z, TangentZ.W < 0.0f ? -1.0f : 1.0f);
		LOD.UVs[Vertex] = VertexBuffer.GetVertexUV(Vertex, 0);
	}

	// Sections. A section's effective slot goes through the LOD's material map when it has one.
	const FSkeletalMeshLODInfo* LODInfo = Source->GetLODInfo(0);
	const TArray<FSkeletalMaterial>& SourceMaterials = Source->GetMaterials();
	TArray<int32> VertexSection;
	VertexSection.Init(INDEX_NONE, N);
	for (int32 SectionIndex = 0; SectionIndex < SourceLOD.RenderSections.Num(); ++SectionIndex)
	{
		const FSkelMeshRenderSection& RenderSection = SourceLOD.RenderSections[SectionIndex];
		FMantleSkinnedMeshSection& Section = LOD.Sections.AddDefaulted_GetRef();
		Section.MaterialIndex = RenderSection.MaterialIndex;
		if (LODInfo && LODInfo->LODMaterialMap.IsValidIndex(SectionIndex) && SourceMaterials.IsValidIndex(LODInfo->LODMaterialMap[SectionIndex]))
		{
			Section.MaterialIndex = LODInfo->LODMaterialMap[SectionIndex];
		}
		if (SourceMaterials.IsValidIndex(Section.MaterialIndex))
		{
			Section.MaterialName = SourceMaterials[Section.MaterialIndex].MaterialSlotName;
		}
		Section.FirstIndex = static_cast<int32>(RenderSection.BaseIndex);
		Section.IndexCount = static_cast<int32>(RenderSection.NumTriangles) * 3;
		const uint32 End = FMath::Min<uint32>(RenderSection.BaseVertexIndex + RenderSection.NumVertices, N);
		for (uint32 Vertex = RenderSection.BaseVertexIndex; Vertex < End; ++Vertex)
		{
			if (VertexSection[Vertex] == INDEX_NONE)
			{
				VertexSection[Vertex] = SectionIndex;
			}
		}
	}

	// Skin weights. The buffer indexes the owning section's bone map (or the LOD's unified map);
	// converted to mesh bone indices so the data does not depend on how sections split the bones.
	const bool bSixteenBitWeights = WeightBuffer.Use16BitBoneWeight();
	LOD.NumInfluences = FMath::Clamp(static_cast<int32>(WeightBuffer.GetMaxBoneInfluences()), 1, static_cast<int32>(MAX_TOTAL_INFLUENCES));
	LOD.InfluenceBones.SetNumZeroed(static_cast<int32>(N) * LOD.NumInfluences);
	LOD.InfluenceWeights.SetNumZeroed(static_cast<int32>(N) * LOD.NumInfluences);
	int32 BadBoneReferences = 0;
	for (uint32 Vertex = 0; Vertex < N; ++Vertex)
	{
		const int32 SectionIndex = VertexSection[Vertex];
		const TConstArrayView<FBoneIndexType> BoneMap = SectionIndex != INDEX_NONE
			? TConstArrayView<FBoneIndexType>(SourceLOD.RenderSections[SectionIndex].BoneMap)
			: SourceLOD.GetUnifiedBoneMap();
		const FSkinWeightInfo Weights = WeightBuffer.GetVertexSkinWeights(Vertex);
		const int32 Base = static_cast<int32>(Vertex) * LOD.NumInfluences;
		int32 Sum = 0;
		int32 Heaviest = Base;
		for (int32 Slot = 0; Slot < LOD.NumInfluences; ++Slot)
		{
			const uint16 Weight = Weights.InfluenceWeights[Slot];
			if (Weight == 0)
			{
				continue;
			}
			const int32 Local = Weights.InfluenceBones[Slot];
			if (BoneMap.IsValidIndex(Local))
			{
				LOD.InfluenceBones[Base + Slot] = BoneMap[Local];
			}
			else
			{
				++BadBoneReferences;
			}
			// 8-bit storage reads back as (w << 8) | w, so w is the high byte exactly; 16-bit
			// storage is rounded (and fixed up to 255 below).
			const uint8 Weight8 = bSixteenBitWeights
				? static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Weight * (255.0f / 65535.0f)), 0, 255))
				: static_cast<uint8>(Weight >> 8);
			LOD.InfluenceWeights[Base + Slot] = Weight8;
			Sum += Weight8;
			Heaviest = Weight8 > LOD.InfluenceWeights[Heaviest] ? Base + Slot : Heaviest;
		}
		if (bSixteenBitWeights && Sum > 0 && Sum != 255)
		{
			LOD.InfluenceWeights[Heaviest] = static_cast<uint8>(FMath::Clamp(LOD.InfluenceWeights[Heaviest] + 255 - Sum, 0, 255));
		}
	}
	if (BadBoneReferences > 0)
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_SKINNEDMESH extract %s: %d influences outside their section's bone map, mapped to bone 0"),
			*Source->GetName(), BadBoneReferences);
	}
	if (bSixteenBitWeights)
	{
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_SKINNEDMESH extract %s: 16-bit skin weights rounded to 8 bits (not an exact copy)"),
			*Source->GetName());
	}

	// The raw bone table and reference pose (virtual bones come back from the USkeleton on build).
	const FReferenceSkeleton& SourceSkeleton = Source->GetRefSkeleton();
	const TArray<FMeshBoneInfo>& BoneInfo = SourceSkeleton.GetRawRefBoneInfo();
	const TArray<FTransform>& BonePose = SourceSkeleton.GetRawRefBonePose();
	Out.Bones.SetNum(BoneInfo.Num());
	Out.ReferencePose.SetNum(BoneInfo.Num());
	for (int32 BoneIndex = 0; BoneIndex < BoneInfo.Num(); ++BoneIndex)
	{
		Out.Bones[BoneIndex].Name = BoneInfo[BoneIndex].Name;
		Out.Bones[BoneIndex].ParentIndex = BoneInfo[BoneIndex].ParentIndex;
		Out.ReferencePose[BoneIndex] = BonePose.IsValidIndex(BoneIndex) ? BonePose[BoneIndex] : FTransform::Identity;
	}

	if (OutMaterials)
	{
		*OutMaterials = SourceMaterials;
	}

	FString Invalid;
	if (!Out.Validate(Invalid))
	{
		return Refuse(FString::Printf(TEXT("%s LOD0 does not make a valid skinned mesh: %s"), *Source->GetName(), *Invalid));
	}
	return true;
}
