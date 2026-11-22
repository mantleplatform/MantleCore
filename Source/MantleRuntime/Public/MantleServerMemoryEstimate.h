// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

struct FMantleEntity;
struct FMantleAssetRef;

/**
 * What the server memory estimate counts, over the root place and every cell (MantleCore#189, the server plan's section 6).
 * Every number is one the place file, its manifest and its cell index already state, so the editor's Check, the publish
 * window, the backend and the server all count the same place the same way.
 */
struct FMantleServerMemoryCounts
{
	/** Entities of every type, root and cells; an InstancedMesh counts once. */
	int64 Entities = 0;

	/** InstancedMesh instances, root and cells, with collision or without. */
	int64 Instances = 0;

	/** Solid, as a cell index counts it: a StaticMesh with collision counts 1, an InstancedMesh with collision counts its instances. */
	int64 Solid = 0;

	/** StaticMesh entries in the manifest. Every mesh the cells use is listed there too. */
	int64 Meshes = 0;

	/** Heightfield samples over every Terrain. */
	int64 TerrainSamples = 0;

	FMantleServerMemoryCounts& operator+=(const FMantleServerMemoryCounts& Other);
};

/**
 * A dedicated server's memory for a place: once the place is ready with no players in it, and at its peak while it loads.
 * A warning only: nothing refuses a publish on it (the server plan, 6.3).
 *
 * The coefficients are the one home of the formula. The backend's TestServerEstimateMatchesPlugin
 * (backend/internal/places) reads them out of this file, so keep each on one line in the form
 * static constexpr double <Name> = <number>;
 *
 * Provisional: fitted from step 0's four measured places (plan 3.3) and what steps 1-4 are expected to save (plan 4.5).
 * Refit them from the next memory run on the Linux builder, and keep them honest with the calibration in plan 6.4.
 */
struct MANTLERUNTIME_API FMantleServerMemoryEstimate
{
	/** An empty server: Sunny Steps measured 292 MB at ready. */
	static constexpr double BaseMB = 300.0;

	/** Each entity's record, script record, actor and indexes. */
	static constexpr double BytesPerEntity = 3072.0;

	/** Each instance the server still holds once ready: a root InstancedMesh's list, a solid set's own list. */
	static constexpr double BytesPerInstance = 128.0;

	/** Each solid thing's collision: its shape in its actor's body and the body's share of the physics scene. */
	static constexpr double BytesPerSolid = 700.0;

	/** Each mesh the server builds (collision and LOD0 only): step 0 measured at most 0.42 GB for 11,700 meshes. */
	static constexpr double BytesPerMesh = 36864.0;

	/** Each heightfield sample: the place's copy of the heights and the physics height field. */
	static constexpr double BytesPerTerrainSample = 100.0;

	/** While loading only: each instance and entity of the cell files being read and merged. */
	static constexpr double LoadBytesPerInstance = 150.0;
	static constexpr double LoadBytesPerEntity = 1024.0;

	/** Mantle's biggest hosted server, 4 vCPU and 8 GB. */
	static constexpr double LargestHostedServerMB = 8192.0;

	/**
	 * What that server has for the world beside the engine, 100 players and room for spikes (plan section 2): about 5 GB once ready
	 * and 6 GB at the load's peak. BaseMB is part of the engine's share, so the estimate is compared without it.
	 */
	static constexpr double HostedWorldReadyMB = 5120.0;
	static constexpr double HostedWorldPeakMB = 6144.0;

	FMantleServerMemoryCounts Counts;

	/** The process's memory once the place is ready, no players. */
	double ReadyMB = 0.0;

	/** The process's memory at the load's peak. */
	double PeakMB = 0.0;

	/** Both fit the biggest hosted server's world share; false means community servers only. */
	bool bFitsHosted = true;

	static FMantleServerMemoryEstimate Of(const FMantleServerMemoryCounts& Counts);

	/** Adds one entity of the root or a cell to Counts. */
	static void AddEntity(const FMantleEntity& Entity, FMantleServerMemoryCounts& Counts);

	/** Adds a manifest's StaticMesh entries (each id once) and its heightfields' samples (from their byte counts; one not uploaded yet counts nothing). */
	static void AddManifest(TConstArrayView<FMantleAssetRef> Assets, FMantleServerMemoryCounts& Counts);

	/** "world_gb=3.55 peak_gb=5.13 fits=hosted entities=... instances=... solid=... meshes=... terrainSamples=...", for log lines: world_gb is ReadyMB. */
	FString Fields() const;

	/**
	 * What a creator reads: "Estimated server memory: 3.6 GB (5.1 GB while loading). Fits a hosted server." When it does not fit, the
	 * sentence says this world will only run on community servers.
	 */
	FString Describe() const;

	/** Megabytes as gigabytes with one decimal ("3.6"). */
	static FString Gigabytes(double MB);
};
