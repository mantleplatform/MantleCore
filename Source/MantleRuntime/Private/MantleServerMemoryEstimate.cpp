// Copyright Joshua Eger. All Rights Reserved.
#include "MantleServerMemoryEstimate.h"

namespace MantleServerMemoryEstimatePrivate
{
	/** An MHF1 heightfield's header (MantleHeightfieldReader.h); 16-bit samples follow it. */
	constexpr int64 HeightfieldHeaderBytes = 36;

	constexpr double BytesPerMB = 1024.0 * 1024.0;
}

FMantleServerMemoryCounts& FMantleServerMemoryCounts::operator+=(const FMantleServerMemoryCounts& Other)
{
	Entities += Other.Entities;
	Instances += Other.Instances;
	Solid += Other.Solid;
	Meshes += Other.Meshes;
	TerrainSamples += Other.TerrainSamples;
	return *this;
}

FMantleServerMemoryEstimate FMantleServerMemoryEstimate::Of(const FMantleServerMemoryCounts& InCounts)
{
	using namespace MantleServerMemoryEstimatePrivate;
	FMantleServerMemoryEstimate Estimate;
	Estimate.Counts = InCounts;
	const double ReadyBytes = BytesPerEntity * static_cast<double>(InCounts.Entities)
		+ BytesPerInstance * static_cast<double>(InCounts.Instances)
		+ BytesPerSolid * static_cast<double>(InCounts.Solid)
		+ BytesPerMesh * static_cast<double>(InCounts.Meshes)
		+ BytesPerTerrainSample * static_cast<double>(InCounts.TerrainSamples);
	const double LoadBytes = LoadBytesPerInstance * static_cast<double>(InCounts.Instances)
		+ LoadBytesPerEntity * static_cast<double>(InCounts.Entities);
	Estimate.ReadyMB = BaseMB + ReadyBytes / BytesPerMB;
	Estimate.PeakMB = Estimate.ReadyMB + LoadBytes / BytesPerMB;
	Estimate.bFitsHosted = Estimate.ReadyMB - BaseMB <= HostedWorldReadyMB && Estimate.PeakMB - BaseMB <= HostedWorldPeakMB;
	return Estimate;
}

void FMantleServerMemoryEstimate::AddEntity(const FMantleEntity& Entity, FMantleServerMemoryCounts& OutCounts)
{
	++OutCounts.Entities;
	// By type, not by props alone: a default entity's props are a StaticMesh's.
	if (Entity.Type == EMantleEntityType::StaticMesh)
	{
		const FMantleStaticMeshProps* Mesh = Entity.Props.TryGet<FMantleStaticMeshProps>();
		OutCounts.Solid += Mesh && Mesh->Collision != EMantleCollision::None ? 1 : 0;
	}
	else if (Entity.Type == EMantleEntityType::InstancedMesh)
	{
		if (const FMantleInstancedMeshProps* Instanced = Entity.Props.TryGet<FMantleInstancedMeshProps>())
		{
			OutCounts.Instances += Instanced->Instances.Num();
			OutCounts.Solid += Instanced->Mesh.Collision != EMantleCollision::None ? Instanced->Instances.Num() : 0;
		}
	}
}

void FMantleServerMemoryEstimate::AddManifest(TConstArrayView<FMantleAssetRef> Assets, FMantleServerMemoryCounts& OutCounts)
{
	using namespace MantleServerMemoryEstimatePrivate;
	TSet<FString> Meshes;
	TSet<FString> Heightfields;
	for (const FMantleAssetRef& Asset : Assets)
	{
		if (Asset.Kind == EMantleAssetKind::StaticMesh)
		{
			bool bSeen = false;
			Meshes.Add(Asset.Id, &bSeen);
			OutCounts.Meshes += bSeen ? 0 : 1;
		}
		else if (Asset.Kind == EMantleAssetKind::Heightfield && Asset.Bytes > HeightfieldHeaderBytes)
		{
			bool bSeen = false;
			Heightfields.Add(Asset.Id, &bSeen);
			OutCounts.TerrainSamples += bSeen ? 0 : (Asset.Bytes - HeightfieldHeaderBytes) / 2;
		}
	}
}

FString FMantleServerMemoryEstimate::Gigabytes(const double MB)
{
	return FString::Printf(TEXT("%.1f"), MB / 1024.0);
}

FString FMantleServerMemoryEstimate::Fields() const
{
	return FString::Printf(TEXT("world_gb=%.2f peak_gb=%.2f fits=%s entities=%lld instances=%lld solid=%lld meshes=%lld terrainSamples=%lld"),
		ReadyMB / 1024.0, PeakMB / 1024.0, bFitsHosted ? TEXT("hosted") : TEXT("community"),
		Counts.Entities, Counts.Instances, Counts.Solid, Counts.Meshes, Counts.TerrainSamples);
}

FString FMantleServerMemoryEstimate::Describe() const
{
	const FString Memory = FString::Printf(TEXT("Estimated server memory: %s GB (%s GB while loading)."), *Gigabytes(ReadyMB), *Gigabytes(PeakMB));
	if (bFitsHosted)
	{
		return Memory + TEXT(" Fits a hosted server.");
	}
	return Memory + FString::Printf(TEXT(" That is more than Mantle's biggest hosted server (%s GB) has room for beside its players, so this world will only run on community servers."),
		*FString::Printf(TEXT("%.0f"), LargestHostedServerMB / 1024.0));
}
