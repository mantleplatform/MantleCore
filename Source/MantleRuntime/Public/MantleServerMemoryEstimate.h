// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"

struct FMantleCellIndex;

/**
 * What the server memory estimate counts, over the root place and every cell (MantleCore#189, the server plan's section 6).
 * Every number is one the place file, its manifest and its cell index already state, so the editor's Check, the publish
 * window, the backend and the server all count the same place the same way.
 */
struct MANTLERUNTIME_API FMantleServerMemoryCounts
{
	/** Entities of every type, root and cells; an InstancedMesh counts once. */
	int64 Entities = 0;

	/** InstancedMesh instances, root and cells, with collision or without. */
	int64 Instances = 0;

	/** Solid, as a cell index counts it: a StaticMesh with collision counts 1, an InstancedMesh with collision counts its instances. */
	int64 Solid = 0;

	/** StaticMesh entries in the manifest. Every mesh the cells use is listed there too. */
	int64 Meshes = 0;

	/** Those meshes' bytes as uploaded (the manifest's byte counts; one whose size is not known yet counts nothing). */
	int64 MeshBytes = 0;

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
 * Fitted (plan 6.4, 2026-11-22) to four places measured on the Linux builder with steps 1-8 in: City Sample, OrbitalBase, Pointer
 * Showcase and Sunny Steps, each within 15% at ready and at its peak, and to Unreal's memory tracker on City Sample and OrbitalBase.
 * The Server memory calibration workflow (tools/bench/calibrate-server-memory.sh) checks them against real servers; refit them
 * when it fails (calibrate_server_memory.py check --header tries new ones on runs already made).
 */
struct MANTLERUNTIME_API FMantleServerMemoryEstimate
{
	/** An empty server: Sunny Steps measured 387 MB at ready. */
	static constexpr double BaseMB = 395.0;

	/** Each entity's record, script record, actor and indexes. Most of a big place's entities are cell scenery that is only a record on a server. */
	static constexpr double BytesPerEntity = 2750.0;

	/** Each instance once ready: a server keeps only the root's lists and the solid sets' own (step 4), so this is small. */
	static constexpr double BytesPerInstance = 16.0;

	/** Each solid thing's collision: its shape in its actor's body and the body's share of the physics scene (City Sample: 2.3 GB of physics for 1.05 M). */
	static constexpr double BytesPerSolid = 2800.0;

	/** Each mesh the server builds, beside its size: its objects and its entry in the asset set. */
	static constexpr double BytesPerMesh = 32768.0;

	/**
	 * Each byte of those meshes as uploaded: the built mesh (collision and LOD0) and its cooked collision. Mesh sizes differ tenfold between places
	 * (City Sample 71 KB a mesh, OrbitalBase 500 KB), so a count of meshes alone cannot say it. Measured 1.6 (City Sample) to 2.0 (OrbitalBase).
	 */
	static constexpr double BytesPerMeshByte = 2.0;

	/** Each heightfield sample: the heights and the physics height field (MANTLE_TERRAIN_BYTES: about 4.4 bytes a sample). */
	static constexpr double BytesPerTerrainSample = 8.0;

	/**
	 * While loading only: each entity of the cell files being read and merged (City Sample peaks about 0.5 GB over ready). Since steps 1 and 4 a
	 * server holds no extra copy of the instance lists, so they add nothing.
	 */
	static constexpr double LoadBytesPerInstance = 0.0;
	static constexpr double LoadBytesPerEntity = 2048.0;

	/** Mantle's biggest hosted server, 4 vCPU and 8 GB. */
	static constexpr double LargestHostedServerMB = 8192.0;

	/**
	 * What that server has for the world beside the engine, 100 players and room for spikes (plan section 2): about 5 GB once ready
	 * and 6 GB at the load's peak. BaseMB is part of the engine's share, so the estimate is compared without it.
	 */
	static constexpr double HostedWorldReadyMB = 5120.0;
	static constexpr double HostedWorldPeakMB = 6144.0;

	/**
	 * The same share on the two smaller hosted servers (plan section 2.1's table), which the backend's session start sizes a
	 * server by: 1.5 vCPU and 3 GB (up to 16 players), and 2 vCPU and 4 GB (up to 50).
	 */
	static constexpr double SmallHostedWorldReadyMB = 1638.0;
	static constexpr double SmallHostedWorldPeakMB = 2048.0;
	static constexpr double MediumHostedWorldReadyMB = 2253.0;
	static constexpr double MediumHostedWorldPeakMB = 2765.0;

	FMantleServerMemoryCounts Counts;

	/** The process's memory once the place is ready, no players. */
	double ReadyMB = 0.0;

	/** The process's memory at the load's peak. */
	double PeakMB = 0.0;

	/** Both fit the biggest hosted server's world share; false means community servers only. */
	bool bFitsHosted = true;

	static FMantleServerMemoryEstimate Of(const FMantleServerMemoryCounts& InCounts);

	/** Adds one entity of the root or a cell to OutCounts. */
	static void AddEntity(const FMantleEntity& Entity, FMantleServerMemoryCounts& OutCounts);

	/** Adds a manifest's StaticMesh entries (each id once, with its bytes) and its heightfields' samples (from their byte counts; one not uploaded yet counts nothing). */
	static void AddManifest(TConstArrayView<FMantleAssetRef> Assets, FMantleServerMemoryCounts& OutCounts);

	/**
	 * Adds every cell's entities, instances and solid, as its cell index states them (what the backend stores too). A dedicated server
	 * counts its place this way once the index is read, before it reads any cell file.
	 */
	static void AddCellIndex(const FMantleCellIndex& Index, FMantleServerMemoryCounts& OutCounts);

	/** "world_gb=6.18 peak_gb=6.88 fits=community entities=... instances=... solid=... meshes=... meshBytes=... terrainSamples=...", for log lines: world_gb is ReadyMB. */
	FString Fields() const;

	/**
	 * What a creator reads: "Estimated server memory: 3.6 GB (5.1 GB while loading). Fits a hosted server." When it does not fit, the
	 * sentence says this world will only run on community servers.
	 */
	FString Describe() const;

	/** Megabytes as gigabytes with one decimal ("3.6"). */
	static FString Gigabytes(double MB);
};
