// Copyright Joshua Eger. All Rights Reserved.
#include "MantleBodyTopologyData.h"

#include "Engine/SkeletalMesh.h"
#include "Misc/Crc.h"
#include "Rendering/MultiSizeIndexContainer.h"
#include "Rendering/PositionVertexBuffer.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/StaticMeshVertexBuffer.h"

FString FMantleBodyLODTopology::Describe() const
{
	const FVector3f Size = Bounds.IsValid ? Bounds.GetSize() : FVector3f::ZeroVector;
	return FString::Printf(TEXT("verts=%d tris=%d sections=%d uvhash=%s poshash=%s size=(%.1f x %.1f x %.1f) cm minZ=%.1f"),
		VertexCount, TriangleCount, SectionCount, *UMantleBodyTopologyData::FormatHash(UVHash),
		*UMantleBodyTopologyData::FormatHash(PositionsHash), Size.X, Size.Y, Size.Z, Bounds.IsValid ? Bounds.Min.Z : 0.0f);
}

bool UMantleBodyTopologyData::Matches(const FMantleBodyLODTopology& LOD) const
{
	return VertexCount > 0 && LOD.VertexCount == VertexCount && LOD.UVHash == UVHash;
}

int32 UMantleBodyTopologyData::FindReferenceLOD(const USkeletalMesh* Mesh) const
{
	const int32 NumLODs = GetNumLODs(Mesh);
	for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
	{
		FMantleBodyLODTopology LOD;
		if (ReadLOD(Mesh, LODIndex, LOD) && Matches(LOD))
		{
			return LODIndex;
		}
	}
	return INDEX_NONE;
}

uint32 UMantleBodyTopologyData::ComputeUVHash(const TConstArrayView<FVector2f> UVs)
{
	// Each UV is quantized to 2048ths and both ints are CRC32-chained in vertex order. The backend's
	// Go UV hash must match this byte for byte.
	uint32 Hash = 0;
	for (const FVector2f& UV : UVs)
	{
		const int32 U = FMath::RoundToInt(UV.X * 2048.f);
		const int32 V = FMath::RoundToInt(UV.Y * 2048.f);
		Hash = FCrc::MemCrc32(&U, sizeof(U), Hash);
		Hash = FCrc::MemCrc32(&V, sizeof(V), Hash);
	}
	return Hash;
}

uint32 UMantleBodyTopologyData::ComputePositionsHash(const TConstArrayView<FVector3f> Positions)
{
	static_assert(sizeof(FVector3f) == 3 * sizeof(float), "FVector3f is three packed floats");
	return FCrc::MemCrc32(Positions.GetData(), Positions.Num() * static_cast<int32>(sizeof(FVector3f)), 0);
}

FString UMantleBodyTopologyData::FormatHash(const uint32 Hash)
{
	return FString::Printf(TEXT("%08x"), Hash);
}

int32 UMantleBodyTopologyData::GetNumLODs(const USkeletalMesh* Mesh)
{
	const FSkeletalMeshRenderData* RenderData = Mesh ? Mesh->GetResourceForRendering() : nullptr;
	return RenderData ? RenderData->LODRenderData.Num() : 0;
}

bool UMantleBodyTopologyData::ReadLOD(const USkeletalMesh* Mesh, const int32 LODIndex, FMantleBodyLODTopology& OutTopology,
	TArray<FVector3f>* OutPositions, TArray<FVector2f>* OutUVs, FString* OutError)
{
	OutTopology = FMantleBodyLODTopology();
	auto Refuse = [OutError](const FString& Error)
	{
		if (OutError)
		{
			*OutError = Error;
		}
		return false;
	};
	if (!Mesh)
	{
		return Refuse(TEXT("no mesh"));
	}
	const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
	if (!RenderData || !RenderData->LODRenderData.IsValidIndex(LODIndex))
	{
		return Refuse(FString::Printf(TEXT("%s has no render data for LOD%d"), *Mesh->GetName(), LODIndex));
	}
	const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[LODIndex];
	const FPositionVertexBuffer& PositionBuffer = LOD.StaticVertexBuffers.PositionVertexBuffer;
	const FStaticMeshVertexBuffer& VertexBuffer = LOD.StaticVertexBuffers.StaticMeshVertexBuffer;
	const uint32 N = PositionBuffer.GetNumVertices();
	if (N == 0 || PositionBuffer.GetVertexData() == nullptr)
	{
		return Refuse(FString::Printf(TEXT("%s LOD%d has no CPU position data (cooked without bAllowCPUAccess?)"), *Mesh->GetName(), LODIndex));
	}
	if (VertexBuffer.GetNumVertices() != N || VertexBuffer.GetNumTexCoords() == 0 || VertexBuffer.GetTexCoordData() == nullptr)
	{
		return Refuse(FString::Printf(TEXT("%s LOD%d has no CPU UV data"), *Mesh->GetName(), LODIndex));
	}

	TArray<FVector3f> Positions;
	TArray<FVector2f> UVs;
	Positions.SetNumUninitialized(N);
	UVs.SetNumUninitialized(N);
	for (uint32 Vertex = 0; Vertex < N; ++Vertex)
	{
		Positions[Vertex] = PositionBuffer.VertexPosition(Vertex);
		UVs[Vertex] = VertexBuffer.GetVertexUV(Vertex, 0);
		OutTopology.Bounds += Positions[Vertex];
	}
	OutTopology.LODIndex = LODIndex;
	OutTopology.VertexCount = static_cast<int32>(N);
	OutTopology.TriangleCount = LOD.MultiSizeIndexContainer.IsIndexBufferValid()
		? static_cast<int32>(LOD.MultiSizeIndexContainer.GetIndexBuffer()->Num() / 3)
		: 0;
	OutTopology.SectionCount = LOD.RenderSections.Num();
	OutTopology.UVHash = ComputeUVHash(UVs);
	OutTopology.PositionsHash = ComputePositionsHash(Positions);
	if (OutPositions)
	{
		*OutPositions = MoveTemp(Positions);
	}
	if (OutUVs)
	{
		*OutUVs = MoveTemp(UVs);
	}
	return true;
}
