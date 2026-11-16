// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "MantleApiClient.h"
#include "MantleAssetStore.h"
#include "MantleCellAssetUse.h"
#include "MantleHeightfieldReader.h"
#include "MantleNaniteSideFile.h"
#include "MantlePlaceMaterial.h"
#include "MantlePlaceTypes.h"
#include "MantleRuntimeMesh.h"
#include "MantleRuntimeTextureBuilder.h"
#include "MantleSoundReader.h"
#include "MantleStaticMeshReader.h"
#include "MantleVehicleAsset.h"
#include "MantleWeaponAsset.h"
#include "MantleWheelAsset.h"
#include "UObject/GCObject.h"
#include "UObject/ObjectPtr.h"

class UMaterialInterface;
class UMantleBuiltVehicle;
class UMantleBuiltWeapon;
class UMantleBuiltWheel;
class UObject;
class USkeletalMesh;
class UStaticMesh;
class UTexture;
struct FMantleAnimClip;
struct FMantlePreparedNanite;
struct FMantlePreparedStaticMesh;
struct FMantleSkinnedMesh;
struct FMantleTerrainWeights;

/**
 * How much Nanite data a frame may register. The engine's streaming manager copies every root page of every mesh added since the last frame in one go, with no limit
 * (NaniteStreamingManager.cpp ProcessNewResources), at about 0.08 to 0.20 ms a MiB on the render thread (measured, S3-A3 scaling table); so the asset set lets a frame build meshes whose root data adds up to
 * BytesPerFrame, and one bigger mesh gets a frame to itself.
 */
struct MANTLERUNTIME_API FMantleNaniteFrameLimit
{
	static constexpr int64 BytesPerFrame = 8 * 1024 * 1024;

	/**
	 * Whether a mesh with MeshBytes of Nanite root data may be built in a frame that has already taken TakenBytes. A mesh with none (0) always may. Once an earlier mesh of
	 * this frame had to wait (bEarlierWaiting) no later one with Nanite data goes ahead of it: the order is kept. Else a frame that took nothing builds any mesh, however big
	 * (it gets the frame to itself), and one that took something builds it only if the two fit in BytesPerFrame (exactly BytesPerFrame fits).
	 */
	static bool MayBuild(int64 TakenBytes, int64 MeshBytes, bool bEarlierWaiting);
};

/**
 * The Nanite root pages the asset set will have attached at once. The engine's root pool has a hard ceiling (NaniteStreamingManager.cpp:299-304 and 646-648: (2048 MB less the
 * streaming pool) / 32 KB = 49,152 pages with a 512 MB pool (the ceiling this budget is taken from; DefaultEngine.ini now has 256 MB, which allows more), on Nvidia and Intel cards; about twice that on AMD) guarded only by a check that a shipping
 * build does not run, and nothing else counts against it. Past the budget a mesh is drawn as its plain model. Pages, not bytes: a root page is 32 KB of GPU memory whatever it holds.
 */
struct MANTLERUNTIME_API FMantleNaniteRootPages
{
	/** The engine's ceiling for the root pool with our streaming pool (512 MB at most): (2048 MB - 512 MB) / 32 KB. A smaller pool raises it. */
	static constexpr int64 EngineCeilingPages = (2048 - 512) * 1024 * 1024 / 32768;

	/**
	 * What the asset set may attach: 36,864 pages = 1.125 GB, 75% of the ceiling. The rest (12,288 pages, 384 MB) is left for what the count cannot see: a mesh needs one
	 * run of free pages as long as itself (a 2,863-page whole-resident mesh needs 2,863 in a row after others were freed), the pool grows in 16 MB steps, meshes released
	 * here live until the next garbage collection, and anything else registered in the process. Medieval's version 1 files attach 26,958 pages over a whole run (73% of this)
	 * and its version 2 files about 2,500 (at most 32 root pages a mesh), so today's places stay under it.
	 */
	static constexpr int64 BudgetPages = 36864;

	/** Whether Attached pages and a mesh of MeshPages fit in the budget. */
	static bool Allows(int64 Attached, int64 MeshPages);
};

/** What one resolve call got back. */
struct MANTLERUNTIME_API FMantleAssetResolveResult
{
	/** The request was answered and read. False: every id asked failed with Error, and is retried. */
	bool bSuccess = false;

	int32 StatusCode = 0;
	FString Error;

	/** The assets found. May hold ids not asked for (a material's textures). */
	TArray<FMantleUploadedAsset> Assets;

	/** Ids the API does not know: a permanent failure. */
	TArray<FString> Missing;

	/** Ids that failed on their own (a per-id fallback's transient error), with the reason; retried. */
	TMap<FString, FString> Failed;
};

/**
 * Turns uploaded asset ids into what to download: POST /assets/resolve (or the per-id GET
 * /assets/:id fallback) in the client, POST /server/assets/resolve on a dedicated server, a stub in
 * tests.
 */
class MANTLERUNTIME_API IMantleAssetResolver
{
public:
	virtual ~IMantleAssetResolver() = default;

	/** Called on the game thread with at most FMantleApiClient::MaxIdsPerResolve ids. OnDone exactly once, from any thread. */
	virtual void Resolve(const TArray<FString>& Ids, TFunction<void(const FMantleAssetResolveResult&)> OnDone) = 0;
};

/** The loader's knobs. ForThisProcess reads [Mantle.PlaceAssets] in the game ini over these defaults. */
struct MANTLERUNTIME_API FMantlePlaceAssetSettings
{
	/**
	 * Resolve, download and build textures and materials. Off on a dedicated server: nothing renders
	 * there, so it resolves and downloads meshes only (for collision) and never touches a material, a
	 * sound, a skeletal mesh or a clip (a skeletal mesh has no collision).
	 */
	bool bRenderAssets = true;

	/** Resolve requests in flight at once. */
	int32 MaxConcurrentResolves = 2;

	/** Attempts per id before a resolve failure is final. */
	int32 MaxResolveAttempts = 3;

	/** Store requests outstanding at once (the store itself caps concurrent downloads). */
	int32 MaxOutstandingFetches = 24;

	/** Decodes (MRM2 read, material parse, texture decode) running on background tasks at once. */
	int32 MaxConcurrentDecodes = 4;

	/** Downloaded or decoded bytes waiting for the game thread; no new download starts above it. */
	int64 MaxPendingBuildBytes = 512ll * 1024 * 1024;

	/** Game-thread time spent building per Pump; at least one build always runs. */
	double BuildSliceMs = 8.0;

	/** Cook mesh collision on a worker (UBodySetup::CreatePhysicsMeshesAsync) rather than in the build slice. */
	bool bAsyncCollisionCook = true;

	/** Largest GPU footprint of one texture (FMantleTextureBuildParams::MaxGpuBytes). */
	int64 MaxTextureGpuBytes = 24ll * 1024 * 1024;

	/** Seconds between progress log lines while loading; 0 turns them off. */
	double ProgressLogSeconds = 2.0;

	/** Pump from the core ticker every frame. Tests turn it off and pump by hand. */
	bool bAutoPump = true;

	/**
	 * Download, unpack and attach the Nanite files the manifest names for its meshes. Decided before anything is downloaded:
	 * ForThisProcess turns it on only where bRenderAssets is on and this machine's graphics run Nanite
	 * (FMantleNaniteSideFile::CanRunNanite), so a server or a machine without Nanite never fetches one. Off in a default-built settings
	 * (tests build settings by hand and turn it on).
	 */
	bool bLoadNanite = false;

	/** Where bytes come from; the process's store when null. */
	TSharedPtr<FMantleAssetStore> Store;

	/** How ids resolve; the API's when null (the server form when bRenderAssets is off on a dedicated server). */
	TSharedPtr<IMantleAssetResolver> Resolver;

	/**
	 * FMantlePlaceAssets::PrepareManifestOnly takes at most this many of the manifest's
	 * uploaded assets that no entity uses, and at most MaxManifestOnlyBytes of them by the
	 * manifest's own sizes; the rest are skipped with a log line, and a script that asks for one is
	 * told it was not downloaded. Bounds what a place can make every joining player download for
	 * things it may never spawn.
	 */
	int32 MaxManifestOnlyAssets = 64;
	int64 MaxManifestOnlyBytes = 256ll * 1024 * 1024;

	/** This process's settings: bRenderAssets off on a dedicated server, the ini's knobs over the defaults. */
	static FMantlePlaceAssetSettings ForThisProcess();

	/**
	 * This process's settings for PrepareManifestOnly: ForThisProcess at a gentler pace (one
	 * resolve, four downloads and two decodes at a time, a 2 ms build slice), since it runs while
	 * the place is already being played.
	 */
	static FMantlePlaceAssetSettings ForManifestOnly();
};

/** Where an uploaded id stands in the live sets, for a caller that can wait for it (a script's spawn). */
enum class EMantleLiveAssetState : uint8
{
	/** No live set prepared it (not in the place, or skipped over the manifest-only cap). */
	NotPrepared,

	/** A live set is resolving, downloading or building it. */
	Loading,

	/** Built: FindLiveMesh / FindLiveMaterial return it. */
	Ready,

	/** It could not be loaded; the reason comes with it. */
	Failed,
};

/** Counts and bytes of a loading place, for a progress bar. */
struct MANTLERUNTIME_API FMantlePlaceAssetProgress
{
	int32 TotalCount = 0;
	int32 ReadyCount = 0;
	int32 FailedCount = 0;

	/** Cooked bytes of the distinct assets resolved so far (grows as materials reveal their textures). */
	int64 TotalBytes = 0;

	/** Bytes of assets already fetched plus bytes received so far for the ones downloading. */
	int64 DoneBytes = 0;

	bool IsComplete() const { return TotalCount > 0 && ReadyCount + FailedCount >= TotalCount; }
};

/** What preparing a place's uploaded assets came to. */
struct MANTLERUNTIME_API FMantlePlaceAssetsResult
{
	/**
	 * Every asset resolved and built. Sounds count once they have settled: OnDone can fire before
	 * they have (they never hold up the place), and the result is completed when they do.
	 */
	bool bSuccess = false;

	/** One per asset that could not be resolved, downloaded or built: Path names the kind and id. */
	TArray<FMantleParseError> Errors;

	int32 Meshes = 0;
	int32 Materials = 0;
	int32 Textures = 0;

	/** Creator vehicles built. */
	int32 Vehicles = 0;

	/** Uploaded sounds decoded. Filled in once they settle (FMantlePlaceAssets::AreSoundsSettled). */
	int32 Sounds = 0;

	/** Uploaded skeletal meshes built and clips decoded. */
	int32 SkeletalMeshes = 0;
	int32 Clips = 0;
	/** Terrain heightfields decoded. The server needs them too: they are the terrain's collision. */
	int32 Heightfields = 0;

	/** Dedicated server: material slots it did not resolve (nothing renders there). */
	int32 SkippedMaterials = 0;

	/** Dedicated server: uploaded sounds it did not resolve (nothing is heard there). */
	int32 SkippedSounds = 0;

	/** Dedicated server: uploaded skeletal meshes and clips it did not resolve (nothing renders there). */
	int32 SkippedSkeletal = 0;

	/** Cooked bytes of every distinct asset, and how many of them came over the network. */
	int64 Bytes = 0;
	int64 DownloadedBytes = 0;

	double Seconds = 0.0;

	FString ErrorSummary() const
	{
		TArray<FString> Lines;
		for (const FMantleParseError& Error : Errors)
		{
			Lines.Add(Error.ToString());
		}
		return FString::Join(Lines, TEXT("\n"));
	}
};

DECLARE_DELEGATE_OneParam(FMantlePlaceAssetsDone, const FMantlePlaceAssetsResult&);

/**
 * A place's uploaded assets (ast_u ids), resolved, downloaded, decoded and built before the place
 * builder spawns anything: Fab F8.
 *
 *   resolve   Ids to {kind, url, cookedHash, cookedBytes} in batches (IMantleAssetResolver). A
 *             material's textures are resolved once the material is (its cookInfo names them) and
 *             again from its document, which is the truth.
 *   fetch     Through FMantleAssetStore: content-addressed, verified against the cooked BLAKE3,
 *             coalesced per hash, cached across joins, pinned while this set lives.
 *   decode    On background tasks: MRM2 (or MRM1) meshes, material documents, textures (with the
 *             material-slot fix-ups: normal green flipped to Unreal's, a BC4 ORM expanded).
 *   build     On the game thread in time slices: UStaticMesh with complex-as-simple collision
 *             cooked on a worker, UTexture2D, and a UMaterialInstanceDynamic of M_MantlePlace once
 *             its textures are built.
 *
 * Deduplicated by content: one download, decode and build per (cooked hash, use), however many ids
 * or entities share it, and an object still alive from an earlier set (the same place rebuilt) is
 * reused without any of the three.
 *
 * A Decal's textures are records of their own, read in the decal's roles, and built like a
 * material's. A dedicated server resolves and builds meshes only, and skips every material (and so
 * every texture) and every sound. While the set lives, FMantleAssetLibrary::LoadMesh/LoadMaterial/
 * LoadTexture answer its ids (the newest live set first), which is how the unchanged
 * FMantlePlaceBuilder finds them. A failed asset is an error naming its id, reported in the result
 * and logged, never a silent stand-in.
 *
 * Sounds (a SoundEmitter's uploaded sound, any sound the manifest lists, which scripts
 * may play through Sounds:Play, and a creator vehicle's horn) take the same resolve, fetch and decode path (MAU1
 * read on a background task) but never hold up the place: OnDone fires once everything else has
 * settled, sounds download after every other asset has started, and the builder binds each emitter
 * when its sound arrives (WhenLiveSoundSettles). Nothing is built on the game thread for a sound:
 * each emitter plays the shared decoded samples through its own UMantleSoundWave. The set keeps
 * pumping itself until its sounds have settled too.
 *
 * Skeletal meshes and clips (a SkeletalMesh entity's uploaded mesh and clip) hold the place up like
 * meshes: an MSK1 place skeletal mesh (role 3) is read on a background task and built on the game thread
 * as a USkeletalMesh on a transient skeleton of its own bones; a MAN1 place clip is read on a background
 * task and its decoded keys are the build (nothing is made on the game thread; each entity samples the
 * shared clip). A dedicated server resolves neither: nothing renders there.
 *
 * A Terrain adds three uses: its heightfield (MHF1, read on a background task; the decoded
 * grid is the build, as a sound's samples are, and the place builder turns it into chunk meshes),
 * which a dedicated server needs as well because it is the terrain's collision; its weightmap, a
 * texture read as linear data; and each layer's material. The last two are skipped on a server. When
 * its layers name surfaces, the weightmap is also read on the CPU, on every net mode, into
 * the heaviest layer per texel (TerrainWeights), which is what a hit on the terrain resolves against.
 * Its grass (drawn only) takes its meshes and materials like a Scatter's, and its grass maps as
 * TerrainWeights that keep their pixels, off a dedicated server.
 *
 * Log markers: MANTLE_PLACE_ASSETS preflight/resolved/built/collision/skip/FAIL/READY/SOUNDS.
 *
 * Game thread, like the store it drives. Keep the returned reference for as long as the place built
 * from it exists.
 */
class MANTLERUNTIME_API FMantlePlaceAssets : public FGCObject, public TSharedFromThis<FMantlePlaceAssets>
{
public:
	UE_NONCOPYABLE(FMantlePlaceAssets);

	/** ast_u and 32 lowercase hex digits: an id the API resolves rather than the built-in library. */
	static bool IsUploadedId(const FString& Id);

	/**
	 * A SoundEmitter's sound that the live set answers: an uploaded id the built-in library
	 * does not know. A library entry wins, so a sound id the library resolves (tests register
	 * ast_u-shaped ids there) builds exactly as it did before uploaded sounds existed.
	 */
	static bool IsUploadedSoundId(const FString& Id);

	/**
	 * True when an entity of the place uses an uploaded mesh, material, (a Decal's) texture or (a
	 * VehicleSpawn's) vehicle, a Terrain (whose heightfield is always uploaded), or (off a
	 * dedicated server, which never hears or draws one) an uploaded sound, skeletal mesh or clip, or
	 * the manifest lists an uploaded sound (one scripts may play) or vehicle (a script can spawn it),
	 * or (off a dedicated server) a UI document shows an uploaded image:
	 * whether Prepare is needed at all. Other manifest entries alone (an uploaded thumbnail) do not count.
	 */
	static bool HasUploadedAssets(const FMantlePlace& Place);

	/**
	 * Starts preparing every uploaded asset the place's entities use. OnDone fires once on the game
	 * thread, from a later Pump, never from inside Prepare.
	 */
	static TSharedRef<FMantlePlaceAssets> Prepare(const FMantlePlace& Place, const FMantlePlaceAssetSettings& Settings,
		FMantlePlaceAssetsDone OnDone);

	/**
	 * True when the place's manifest lists uploaded meshes (and, when bRenderAssets, uploaded
	 * materials) that no entity uses: assets a script can spawn (Assets:Spawn) or swap onto an
	 * entity (Entity:SetMaterial). Prepare leaves those out, so they need PrepareManifestOnly. A
	 * vehicle's part meshes and materials are Prepare's, not these: they count as used once a
	 * live set has built the vehicle, so ask only after the place's own set has completed.
	 */
	static bool HasManifestOnlyAssets(const FMantlePlace& Place, bool bRenderAssets);

	/**
	 * Prepares the manifest-only assets HasManifestOnlyAssets describes, exactly as Prepare
	 * does the entities' (meshes with collision; materials and their textures only when
	 * Settings.bRenderAssets), up to Settings.MaxManifestOnlyAssets and MaxManifestOnlyBytes. Start
	 * it once the place is built, never before: the place and the join never wait for it. While it
	 * loads, GetLiveMeshState / GetLiveMaterialState say Loading for its ids.
	 */
	static TSharedRef<FMantlePlaceAssets> PrepareManifestOnly(const FMantlePlace& Place, const FMantlePlaceAssetSettings& Settings,
		FMantlePlaceAssetsDone OnDone);

	/**
	 * PrepareManifestOnly with ForManifestOnly settings for a place this process has just
	 * built, or null when the place has no manifest-only assets. Logs a summary when it completes.
	 * Keep the result exactly as long as the place's own set.
	 */
	static TSharedPtr<FMantlePlaceAssets> PrepareManifestOnlyForThisProcess(const FMantlePlace& Place);

	/**
	 * Prepare for a place that streams its cells (stage 2 of large places): the root's own assets as Prepare does, and the set then stays alive and pumping
	 * so that the cells' meshes and materials are added when a cell is wanted (AcquireEntityAssets) and taken away when no cell wants them (ReleaseEntityAssets).
	 * Root is the place as built (its manifest is the asset list of every cell too). Nothing else about the set changes: OnDone fires when the root's assets have.
	 */
	static TSharedRef<FMantlePlaceAssets> PrepareStreaming(const FMantlePlace& Root, const FMantlePlaceAssetSettings& Settings, FMantlePlaceAssetsDone OnDone);

	/** This set was made by PrepareStreaming. */
	bool IsStreaming() const { return bStreaming; }

	/**
	 * Holder (a cell of one streamer) wants the uploaded meshes and materials Entities use (FMantleCellAssetUse::NeedsOf). Each id is resolved, fetched, decoded and built
	 * once however many holders want it, with the mesh's Nanite and Lumen data and a material's textures, exactly as the root's are. A holder that holds something already
	 * is given it back first. Streaming sets only.
	 */
	void AcquireEntityAssets(int64 Holder, const TArray<FMantleEntity>& Entities);

	/** What a holder's assets have come to: ids that are not built yet (and not failed), and ids that failed (the reasons are logged by the set once). Settled when nothing is pending. */
	struct FHolderAssets
	{
		TArray<FString> PendingIds;
		TArray<FString> FailedIds;
		bool IsSettled() const { return PendingIds.IsEmpty(); }
	};
	FHolderAssets GetHolderAssets(int64 Holder) const;

	/** Holder gives its assets back: an id nobody else wants is dropped from the set (its object, its Nanite and Lumen data, its textures), as soon as its build is not in flight. */
	void ReleaseEntityAssets(int64 Holder);

	/** What a streaming set holds now: records and built objects by kind (ids no cell wants any more but whose release is waiting on a build in flight count until it is done), and the ids wanted. */
	struct FStreamedCounts
	{
		int32 Meshes = 0;
		int32 Materials = 0;
		int32 Textures = 0;
		int32 NaniteFiles = 0;
		int32 WantedMeshes = 0;
		int32 WantedMaterials = 0;
		int32 Holders = 0;
		int32 DropsWaiting = 0;
		/** Distinct uploaded ids this set has taken for cells since it started, and how many it has let go. */
		int64 Acquired = 0;
		int64 Dropped = 0;
		/** Pump calls since the set began streaming, the time they took (seconds) and the longest one. */
		int64 Pumps = 0;
		double PumpSeconds = 0.0;
		double WorstPumpSeconds = 0.0;
		/** Nanite files read since the start (a mesh built again reads its file again). */
		int32 NaniteFilesBuilt = 0;
		/** Mesh builds that had a Nanite record whose file was not there when the mesh was built (and had not failed): meshes drawn without their Nanite data. */
		int32 NaniteFileMisses = 0;
		/** Nanite records whose file is still being fetched or read, and mesh records whose build with collision is still being made. */
		int32 NaniteInFlight = 0;
		int32 UpgradesInFlight = 0;
	};

	/**
	 * Streaming sets: how long a Pump may spend building (seconds, at least one build is always made). The cell streamer sets it every tick so that asset builds and cell
	 * pieces share one budget; until it does, and for any set that is not streaming, Settings.BuildSliceMs.
	 */
	void SetStreamBuildSlice(double Seconds) { StreamSliceSeconds = Seconds; }
	FStreamedCounts GetStreamedCounts() const;

	/** One line a record of a mesh, side file or texture: what the set holds and why (for the tests and for finding a leak). */
	FString DescribeStreamedRecords() const;

	/** The API resolver: the server route on a dedicated server (bServer), else the batch route with a per-id fallback. */
	static TSharedRef<IMantleAssetResolver> CreateApiResolver(bool bServer);

	/** The built mesh for an uploaded id in the newest live set that has it; null with the reason otherwise. */
	static UStaticMesh* FindLiveMesh(const FString& Id, FString& OutError);

	/** The built material for an uploaded id, as FindLiveMesh. */
	static UMaterialInterface* FindLiveMaterial(const FString& Id, FString& OutError);

	/** The built texture an entity names directly (a Decal's), read with Role, as FindLiveMesh. */
	static UTexture* FindLiveTexture(const FString& Id, EMantleTextureRole Role, FString& OutError);

	/**
	 * Where an uploaded texture read with Role stands in the newest live set that has it, as GetLiveMeshState: a
	 * vehicle's livery (read as color) is prepared with its vehicle off a dedicated server, so a car can wait for it.
	 */
	static EMantleLiveAssetState GetLiveTextureState(const FString& Id, EMantleTextureRole Role, FString* OutError = nullptr);

	/** The built skeletal mesh (on its own skeleton) for an uploaded id, as FindLiveMesh. */
	static USkeletalMesh* FindLiveSkeletalMesh(const FString& Id, FString& OutError);

	/**
	 * The decoded place clip for an uploaded id, as FindLiveMesh; shared by every entity playing it.
	 * bOutStillLoading, when given, says the null is only because it has not arrived yet (a script's
	 * Entity:PlayClip of a manifest-only clip, script API 1.18, waits for it).
	 */
	static TSharedPtr<const FMantleAnimClip> FindLiveClip(const FString& Id, FString& OutError, bool* bOutStillLoading = nullptr);

	/**
	 * The decoded sound for an uploaded id in the newest live set that has it; null with the
	 * reason otherwise. bOutStillLoading, when given, says the null is only because it has not
	 * arrived yet (WhenLiveSoundSettles then binds it later).
	 */
	static TSharedPtr<const FMantleSoundData> FindLiveSound(const FString& Id, FString& OutError, bool* bOutStillLoading = nullptr);

	/**
	 * Calls OnSettled once with the decoded sound for an uploaded id, or null and the reason: at once
	 * (returning true) when it has settled or no live set has it, else from a later Pump of the set
	 * that is loading it (returning false). Dropped, uncalled, if that set goes away first. Game thread.
	 */
	static bool WhenLiveSoundSettles(const FString& Id, TFunction<void(TSharedPtr<const FMantleSoundData> Sound, const FString& Error)> OnSettled);

	/**
	 * The built creator vehicle for an uploaded Vehicle id in the newest live set that has it; null
	 * with the reason otherwise. Every vehicle a place's VehicleSpawns use or its manifest lists is prepared
	 * with the place, on the server (for its physics) and on clients.
	 */
	static UMantleBuiltVehicle* FindLiveVehicle(const FString& Id, FString& OutError);

	/** Where an uploaded vehicle id stands in the newest live set that has it, as GetLiveMeshState. */
	static EMantleLiveAssetState GetLiveVehicleState(const FString& Id, FString* OutError = nullptr);

	/**
	 * The read creator weapon for an uploaded Weapon id in the newest live set that has it: the document text
	 * and the ids it names. Null with the reason otherwise. Every weapon a place's WeaponSpawns use or its manifest lists is
	 * prepared with the place on every machine, with the skeletal mesh and attachment meshes the document names (and, off a
	 * dedicated server, its materials and sounds): FindLiveSkeletalMesh / FindLiveMesh / FindLiveMaterial / FindLiveSound
	 * answer for them. An attachment id is one of FindLiveSkeletalMesh or FindLiveMesh, whichever the API says it is.
	 */
	static UMantleBuiltWeapon* FindLiveWeapon(const FString& Id, FString& OutError);

	/** Where an uploaded weapon id stands in the newest live set that has it, as GetLiveMeshState. */
	static EMantleLiveAssetState GetLiveWeaponState(const FString& Id, FString* OutError = nullptr);

	/**
	 * The built creator wheel for an uploaded Wheel id in the newest live set that has it: the document, and on a machine
	 * that renders its static mesh and materials (a material that did not load is the default surface). Null with the reason
	 * otherwise. Every wheel a place's storage lists is prepared with the place; a dedicated server keeps only the document
	 * (Mesh null, Materials empty) and fetches none of the wheel's mesh, material or texture bytes.
	 */
	static UMantleBuiltWheel* FindLiveWheel(const FString& Id, FString& OutError);

	/** Where an uploaded wheel id stands in the newest live set that has it, as GetLiveMeshState. */
	static EMantleLiveAssetState GetLiveWheelState(const FString& Id, FString* OutError = nullptr);

	/** Where an uploaded mesh id stands in the newest live set that has it; OutError says why when it failed or was never prepared. */
	static EMantleLiveAssetState GetLiveMeshState(const FString& Id, FString* OutError = nullptr);

	/** The same for an uploaded material id. */
	static EMantleLiveAssetState GetLiveMaterialState(const FString& Id, FString* OutError = nullptr);

	/** The same for an uploaded skeletal mesh id (a weapon's mesh, or an attachment the API says is one). */
	static EMantleLiveAssetState GetLiveSkeletalMeshState(const FString& Id, FString* OutError = nullptr);

	/** The decoded heightfield for an uploaded id in the newest live set that has it; null with the reason otherwise. */
	static TSharedPtr<const FMantleHeightfield> FindLiveHeightfield(const FString& Id, FString& OutError);

	/**
	 * A VoxelWorld's starting blocks (an MBW1 file) or one of its tile textures (the uploaded texture's file, MTX1 or an
	 * image), as downloaded, from the newest live set that prepared it; null with OutError otherwise. Game thread.
	 */
	static TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> FindLiveVoxelFile(const FString& Id, bool bTile, FString& OutError);

	/**
	 * A Terrain weightmap's heaviest layer per texel for an uploaded id in the newest live set
	 * that has it; null with the reason otherwise. Prepared on every net mode, the server's included,
	 * but only for a Terrain whose layers name a surface (UMantleTerrainSurfaceComponent::NeedsWeights).
	 */
	static TSharedPtr<const FMantleTerrainWeights> FindLiveTerrainWeights(const FString& Id, FString& OutError);

	virtual ~FMantlePlaceAssets() override;

	/** Delivers finished work, starts resolves, downloads and decodes, and runs one build slice. The core ticker calls it when bAutoPump is set. */
	void Pump();

	/**
	 * Pumps the store, this set and the game thread's task queue until complete, sounds included, or
	 * out of time. For tools and tests, never a running game.
	 */
	bool WaitUntilComplete(float TimeoutSeconds);

	/** Inflated Nanite files held now, and the most held at once, in bytes (tests and the load log). */
	int64 GetNaniteHeldBytes() const { return NaniteHeldBytes; }
	int64 GetNaniteHeldPeakBytes() const { return NaniteHeldPeakBytes; }
	int64 GetNaniteRootPagesAttached() const { return NaniteRootPagesAttached; }

	/** Everything but the sounds has settled and OnDone has fired. */
	bool IsComplete() const { return bCompleted; }

	/** The sounds have settled too (true once complete when there are none). */
	bool AreSoundsSettled() const { return bSoundsSettled; }

	/** The place's loading progress: sounds are left out, as they never hold the place up. */
	FMantlePlaceAssetProgress GetProgress() const;

	/** Valid once complete; its sound part once AreSoundsSettled. */
	const FMantlePlaceAssetsResult& GetResult() const { return Result; }

	UStaticMesh* FindMesh(const FString& Id, FString* OutError = nullptr) const;
	UMaterialInterface* FindMaterial(const FString& Id, FString* OutError = nullptr) const;
	USkeletalMesh* FindSkeletalMesh(const FString& Id, FString* OutError = nullptr) const;
	TSharedPtr<const FMantleAnimClip> FindClip(const FString& Id, FString* OutError = nullptr, bool* bOutStillLoading = nullptr) const;
	UMantleBuiltVehicle* FindVehicle(const FString& Id, FString* OutError = nullptr) const;
	UMantleBuiltWeapon* FindWeapon(const FString& Id, FString* OutError = nullptr) const;
	UMantleBuiltWheel* FindWheel(const FString& Id, FString* OutError = nullptr) const;

	/** The decoded sound for an uploaded id; null with the reason otherwise, bOutStillLoading when it has not arrived yet. */
	TSharedPtr<const FMantleSoundData> FindSound(const FString& Id, FString* OutError = nullptr, bool* bOutStillLoading = nullptr) const;

	/** The decoded heightfield for an uploaded id; null with the reason otherwise. */
	TSharedPtr<const FMantleHeightfield> FindHeightfield(const FString& Id, FString* OutError = nullptr) const;

	/** A Terrain weightmap's heaviest layers for an uploaded id; null with the reason otherwise. */
	TSharedPtr<const FMantleTerrainWeights> FindTerrainWeights(const FString& Id, FString* OutError = nullptr) const;

	// FGCObject
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("FMantlePlaceAssets"); }

private:
	/**
	 * TerrainWeights is an uploaded Texture (the API's kind) read on the CPU into a Terrain's
	 * heaviest layer per texel, beside (not instead of) the GPU texture a client builds from the same id.
	 */
	enum class EAssetKind : uint8 { Mesh, Material, Texture, Sound, SkeletalMesh, AnimClip, Heightfield, Vehicle, TerrainWeights, Weapon, Lumen, VoxelWorld, VoxelTile, Wheel, Nanite };

	using FSoundSettled = TFunction<void(TSharedPtr<const FMantleSoundData>, const FString&)>;

	enum class EBuildState : uint8 { Queued, Fetching, Fetched, Decoding, WaitingForTextures, Building, CookingCollision, Ready, Failed };

	/** One distinct thing to build: a cooked hash and what it is used as. */
	struct FBuildKey
	{
		FMantleContentHash Hash;
		EAssetKind Kind = EAssetKind::Mesh;

		/** Meshes: 1 with collision. Textures: the EMantleTextureRole. Terrain weights: 1 keeps the pixels. Materials: 0. */
		uint8 Variant = 0;

		bool operator==(const FBuildKey& Other) const { return Hash == Other.Hash && Kind == Other.Kind && Variant == Other.Variant; }
		friend uint32 GetTypeHash(const FBuildKey& Key)
		{
			return HashCombine(GetTypeHash(Key.Hash), (static_cast<uint32>(Key.Kind) << 8) | Key.Variant);
		}
	};

	struct FBuild
	{
		FBuildKey Key;
		FMantleStoredAssetRef Ref;
		EBuildState State = EBuildState::Queued;
		FString Error;

		/** The first id that asked for it, and every id that shares it (logs). */
		FString Id;
		TArray<FString> RecordIds;

		/** What was built, for the log line. */
		FString Detail;

		uint64 RequestId = 0;
		bool bPinned = false;
		bool bFromCache = false;

		/** An earlier set's object, still alive, was taken as it was. */
		bool bReused = false;

		/** Bytes held for the game thread (downloaded, then decoded), counted against MaxPendingBuildBytes. */
		int64 PendingBytes = 0;

		TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Bytes;

		/**
		 * Lumen side files (#149): the MLD1 as downloaded, which is what the build is (kept while the set lives); each mesh
		 * that names it reads its own copy of the data from it when it builds.
		 */
		TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> LumenFile;

		/**
		 * Nanite files: the inflated head of the MNZ2 (read and inflated by the decode task from the file in the store, so the game thread never does), and the container's table.
		 * Each mesh that names it reads its own copy of the data from the head when it builds; dropped once the last such mesh has. The streamed pages are never held: the mesh's page store reads them from the file.
		 */
		TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> NaniteFile;
		TSharedPtr<const FMantleNaniteTable, ESPMode::ThreadSafe> NaniteTable;

		/** Where the file is in the store (valid while the build, which pins it, lives). */
		FString NanitePath;

		/** When its fetch began (the time the store takes to verify a cached file the first time is logged). */
		double FetchStartedAt = 0.0;

		/** The size of the head and table as counted in NaniteHeldBytes (0 once released). */
		int64 NaniteHeld = 0;

		/**
		 * A VoxelWorldData (MBW1) or a block tile texture, as downloaded: the voxel world reads both itself, the tiles
		 * into one texture array (kept while the set lives).
		 */
		TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> VoxelFile;

		/**
		 * Meshes (#149): the record of the mesh's Lumen side file (empty when the place gives it none, or one baked by another
		 * engine), and the mesh's upload hash the file must name. The mesh waits for that record to settle before it builds
		 * (WaitingForTextures, as a material waits for its textures); a file that fails is "not present".
		 */
		FString LumenRecord;
		FString LumenMeshTag;

		/**
		 * Meshes: likewise the record of the mesh's Nanite file (empty when the place gives it none, or this process does not take one) and
		 * the mesh's upload hash the file must name. The mesh waits for the record to settle (the second texture slot of the wait), and a
		 * file that fails is "not present": the mesh draws as it is built.
		 */
		FString NaniteRecord;
		FString NaniteMeshTag;

		TSharedPtr<FMantleStaticMeshData> MeshV2;
		/** The MRM2 as mesh descriptions, made with the decode on a worker (FMantleRuntimeStaticMeshBuilder::Prepare); the build on the game thread finishes from it. */
		TSharedPtr<FMantlePreparedStaticMesh> Prepared;
		/** A big Nanite side file read on a worker (started when the mesh is first ready to build; the build waits for it, a frame at a time). */
		TSharedPtr<FMantlePreparedNanite, ESPMode::ThreadSafe> NaniteRead;
		/** The root bytes of the Nanite data this mesh will register (from its file's header): -1 not asked yet, 0 none. */
		int64 NaniteRootBytes = -1;
		/** The root pages this mesh attached (counted in NaniteRootPagesAttached, taken out when the build is erased). */
		int32 NaniteRootPages = 0;
		TSharedPtr<FMantleRuntimeMesh> MeshV1;
		TSharedPtr<FMantleDecodedTexture> Texture;
		TSharedPtr<FMantlePlaceMaterialDesc> Material;

		/** Sounds: the decoded samples, which are what a sound build is (kept while the set lives). */
		TSharedPtr<const FMantleSoundData> Sound;

		/** Skeletal meshes: the decoded MSK1, until the game thread builds it. */
		TSharedPtr<FMantleSkinnedMesh> Skinned;

		/** Clips: the decoded keys, which are what a clip build is (kept while the set lives). */
		TSharedPtr<const FMantleAnimClip> Clip;

		/** Heightfields: the decoded grid, likewise the build; the place builder makes the meshes. */
		TSharedPtr<const FMantleHeightfield> Heightfield;

		/** Terrain weights: the heaviest layer per texel, likewise the build; the weightmap's pixels are dropped once read. */
		TSharedPtr<const FMantleTerrainWeights> TerrainWeights;

		/** Vehicles: the read MVH1, until the game thread builds its UMantleBuiltVehicle. */
		TSharedPtr<FMantleVehicleAsset> Vehicle;

		/** Weapons: the read MWP1, until the game thread wraps it in its UMantleBuiltWeapon. */
		TSharedPtr<FMantleWeaponAsset> Weapon;

		/** Wheels: the read MWL1, until the game thread wraps it in its UMantleBuiltWheel. */
		TSharedPtr<FMantleWheelAsset> Wheel;

		/** Wheels: the mesh and material records the wheel waits for before it builds (WaitingForTextures, as a material waits). */
		TArray<FString> PartRecords;

		/** Materials: the texture record per slot, a layer blend's included (empty when the slot has none). */
		FString TextureRecords[MantlePlaceMaterialTextureCount];

		TObjectPtr<UObject> Object;

		bool IsTerminal() const { return State == EBuildState::Ready || State == EBuildState::Failed; }
	};

	/** One uploaded id in one use (a texture used as color and as data is two records). */
	struct FRecord
	{
		FString Id;
		EAssetKind Kind = EAssetKind::Mesh;

		/** Textures: the role it is read with. */
		EMantleTextureRole Role = EMantleTextureRole::Color;

		/** Meshes: an entity collides with it. */
		bool bCollision = false;

		/** Terrain weights: a grass map, whose four channels are read too, so the decode keeps the pixels. */
		bool bKeepPixels = false;

		/**
		 * Meshes: a weapon's attachment, which the API may say is a skeletal mesh instead. When it does, ApplyResolved
		 * swaps this record for a skeletal mesh one.
		 */
		bool bMeshOrSkeletal = false;

		bool bResolving = false;
		bool bResolved = false;
		int32 ResolveAttempts = 0;
		double NextResolveAt = 0.0;

		/** Re-resolved after a 403/404 download (an expired URL). */
		bool bReresolved = false;

		/** Streaming sets: made after the set completed, for a cell (or for what a cell's asset names), so it may be dropped when nothing wants it. The root's records never are. */
		bool bStreamed = false;

		/** Streaming sets, meshes: a user that collides arrived after the mesh was resolved without collision; the mesh is built again with it once its build is not in flight. */
		bool bUpgradeToCollision = false;

		/**
		 * Streaming sets, meshes: the build with collision that is being made while the record still answers with the first one (cells already built from it, and cells that have their
		 * assets and are waiting to be built, find their mesh all the while). The record points to it once it is built.
		 */
		TOptional<FBuildKey> UpgradeBuild;

		/** The build with collision failed: the first build stays, and a user that needs collision is told the mesh failed. */
		bool bUpgradeFailed = false;

		/** Set when the record itself failed (unknown id, wrong kind, not approved); else the build decides. */
		FString Error;
		bool bFailed = false;

		TOptional<FBuildKey> Build;
	};

	using FCompletion = TFunction<void(FMantlePlaceAssets&)>;

	/** Finished work handed back from tasks and callbacks; shared so a task never holds the set itself. */
	struct FCompletionQueue
	{
		FCriticalSection Lock;
		TArray<FCompletion> Items;

		void Push(FCompletion&& Completion)
		{
			FScopeLock Guard(&Lock);
			Items.Add(MoveTemp(Completion));
		}
	};

	FMantlePlaceAssets(const FMantlePlaceAssetSettings& InSettings, FMantlePlaceAssetsDone InOnDone);

	static FString RecordKey(const FString& Id, EAssetKind Kind, EMantleTextureRole Role);
	static const TCHAR* KindName(EAssetKind Kind);

	void Start(const FMantlePlace& Place);
	/** PrepareManifestOnly's Start: the manifest's uploaded meshes and materials no entity or built vehicle uses, within the caps. */
	void StartManifestOnly(const FMantlePlace& Place);

	/**
	 * #149: a record for the Lumen side file of every mesh record so far whose manifest entry names one baked by this
	 * engine (FMantleMeshLumenFile::EngineTag), and MeshLumen for ApplyResolved. One baked by another engine is "not present".
	 */
	void AddLumenRecords(const FMantlePlace& Place);

	/** AddLumenRecords and AddNaniteRecords for the named meshes, from a manifest by id (a streaming set adds a cell's meshes one cell at a time). */
	void AddLumenRecordsFor(const TMap<FString, const FMantleAssetRef*>& ById, const TArray<FString>& MeshIds);
	void AddNaniteRecordsFor(const TMap<FString, const FMantleAssetRef*>& ById, const TArray<FString>& MeshIds);

	// ---- Streaming (PrepareStreaming) ----

	/** The wanted meshes and materials: who uses what. Streaming sets only. */
	FMantleCellAssetUse CellUse;

	/** The root's asset list, kept so that a cell's meshes can find their Lumen and Nanite entries; by id. */
	TArray<FMantleAssetRef> StreamManifest;
	TMap<FString, const FMantleAssetRef*> StreamManifestById;

	bool bStreaming = false;

	/** Records (by key) that nobody wants any more and that are waiting for a build in flight to settle before they are dropped. */
	TSet<FString> PendingDrops;

	int64 StreamAcquired = 0;
	int64 StreamDropped = 0;
	int64 StreamPumps = 0;
	double StreamPumpSeconds = 0.0;
	double StreamWorstPumpSeconds = 0.0;
	int32 NaniteFilesBuiltCount = 0;
	int32 NaniteFileMisses = 0;
	double StreamSliceSeconds = -1.0;

	/** Meshes (by id) that some cell has needed with collision: a mesh made again is made with it, so it is built again at most once. */
	TSet<FString> MeshesNeedingCollision;

	/** Records (by key) a drop of which is forced past "another build names it": a mesh's own Nanite and Lumen records when the mesh is built again. */
	TSet<FString> ForcedDrops;

	/** The texture records (keys) started for each material id from its answer from the API, so that a material whose build failed before its document was read still lets them go. */
	TMap<FString, TArray<FString>> MaterialTextureKeys;

	/** Mesh records whose collision upgrade waits on a build in flight (by record key). */
	TSet<FString> PendingUpgrades;

	/** The holders a script's demand for an asset no cell has loaded (a spawn of a mesh the manifest lists) uses, one each, counting down from -2: never released while the place stands. */
	int64 NextScriptHolder = -2;

	/** Takes the needs (already counted by CellUse) as records: a mesh or material not here is made; a mesh without collision here that now needs it is upgraded. */
	void StartRecordsFor(const TArray<FMantleCellAssetNeed>& Needs);

	/** Drops the record of a mesh or material nobody wants, with what hangs on it. False when a build is in flight: it is tried again at the next Pump. */
	bool DropRecord(const FString& Key);

	/** Frees a build that no record uses any more (its pin, its request, its queue entries and its object). */
	void EraseBuild(const FBuildKey& Key);

	/** A texture or side-file record that only a dropped record named: dropped with it unless another build names it (a root's record only when bForce). */
	void DropDependentRecord(const FString& Key, bool bForce);

	/** Another build, or a mesh's side-file entry, names this record. */
	bool IsRecordNamed(const FString& Key) const;

	/** The records waiting on a build in flight (drops and collision upgrades), tried again. */
	void ProcessStreamedRecords();

	/** A mesh record built without collision now needs it: the build with collision is started beside the first (its side-file records are made again). False when a build is in flight. */
	bool UpgradeToCollision(FRecord& Record);

	/** The record's build with collision has settled: the record points to it and the first build goes, or, if it failed, the first stays and the upgrade is marked failed. False while it is in flight. */
	bool FinishUpgrade(FRecord& Record);

	/** The answer of a streaming set for an id no live set has, when the root's manifest lists it: starts it for a script (and says Loading), or NotPrepared. */
	EMantleLiveAssetState OnDemandStateOf(const FString& Id, EAssetKind Kind);

	/** #149: a mesh id to its Lumen side file's record key and the mesh's upload hash the file must name. */
	TMap<FString, TPair<FString, FString>> MeshLumen;

	/**
	 * A record for the Nanite file of every mesh record so far whose manifest entry names one baked for exactly this engine build, and
	 * MeshNanite for ApplyResolved; only where Settings.bLoadNanite. A file for another build is never downloaded (the mesh draws plain).
	 */
	void AddNaniteRecords(const FMantlePlace& Place, const TSet<FString>* OnlyMeshes = nullptr);

	/** The Nanite heads and tables held now, and the most held at once (what a mesh has yet to read: no page of any file is held). */
	int64 NaniteHeldBytes = 0;
	int64 NaniteHeldPeakBytes = 0;
	/** Nanite files fetched from the store (path only) that were already stored: how many, the sum and worst of the time each took (the first use of a file in a process hashes all of it), and their bytes. */
	int32 NaniteCachedFetches = 0;
	double NaniteCachedFetchMs = 0.0;
	double NaniteCachedFetchWorstMs = 0.0;
	int64 NaniteCachedFetchBytes = 0;
	/** The Nanite root pages the set's meshes have attached now, against FMantleNaniteRootPages::BudgetPages. */
	int64 NaniteRootPagesAttached = 0;

	/** Frees a Nanite build's head and table (counted in NaniteHeldBytes). */
	void ReleaseNaniteFile(FBuild& NaniteBuild);

	/** A mesh id to its Nanite file's record key and the mesh's upload hash the file must name. */
	TMap<FString, TPair<FString, FString>> MeshNanite;

	/**
	 * Frees a Ready Nanite file's inflated bytes once no mesh that names it still has to read them (one not resolved yet, or not past
	 * Building, does). Current is the mesh build that just finished, built or not; called whenever a mesh settles (built, reused, failed)
	 * and when the file itself becomes Ready, so a file whose mesh never builds is not held.
	 */
	void DropNaniteFileIfUnused(const FString& NaniteRecordKey, const FBuildKey& Current);
	static TSharedRef<FMantlePlaceAssets> PrepareWith(const FMantlePlace& Place, const FMantlePlaceAssetSettings& Settings,
		FMantlePlaceAssetsDone OnDone, bool bManifestOnly);
	/** GetLiveMeshState / GetLiveMaterialState across the live sets, newest first. */
	static EMantleLiveAssetState LiveStateOf(const FString& Id, EAssetKind Kind, FString* OutError);
	/** One record's state in this set. */
	EMantleLiveAssetState StateOf(const FRecord& Record, FString* OutError) const;
	FRecord& EnsureRecord(const FString& Id, EAssetKind Kind, EMantleTextureRole Role = EMantleTextureRole::Color);
	void FailRecord(FRecord& Record, const FString& Error);

	void FlushResolves();
	void OnResolved(const TArray<FString>& Ids, const FMantleAssetResolveResult& Resolved);
	void ApplyResolved(FRecord& Record, const FMantleUploadedAsset& Asset);
	void QueueTexturesOf(const FString& MaterialId, const TMap<FString, FString>& SlotTextures);

	void StartFetches();
	void OnFetched(const FBuildKey& Key, const FMantleAssetResult& Fetched);
	void StartDecodes();
	void OnDecoded(const FBuildKey& Key, TSharedPtr<FMantleStaticMeshData> MeshV2, TSharedPtr<FMantlePreparedStaticMesh> Prepared, TSharedPtr<FMantleRuntimeMesh> MeshV1,
		TSharedPtr<FMantleDecodedTexture> Texture, TSharedPtr<FMantlePlaceMaterialDesc> Material, TSharedPtr<const FMantleSoundData> Sound,
		TSharedPtr<FMantleSkinnedMesh> Skinned, TSharedPtr<const FMantleAnimClip> Clip, TSharedPtr<const FMantleHeightfield> Heightfield,
		TSharedPtr<const FMantleTerrainWeights> TerrainWeights, TSharedPtr<FMantleVehicleAsset> Vehicle, TSharedPtr<FMantleWeaponAsset> Weapon, TSharedPtr<FMantleWheelAsset> Wheel, const FString& Error);
	/** A vehicle's materials, from its document, as material records (off a dedicated server). */
	void QueueMaterialsOf(const FString& VehicleId, const TArray<FString>& MaterialIds);
	/** A vehicle's liveries, from its document, as color texture records (off a dedicated server). */
	void QueueLiveriesOf(const FString& VehicleId, const TArray<FString>& TextureIds);
	/** Item 2: a vehicle's part meshes, from its document, as mesh records without collision (off a dedicated server). */
	void QueueMeshesOf(const FString& VehicleId, const TArray<FString>& MeshIds);
	/**
	 * A vehicle's sounds (its horn's), from its document, as sound records (off a dedicated server, which hears
	 * nothing): they never hold the place up, as no sound does.
	 */
	void QueueSoundsOf(const FString& VehicleId, const TArray<FString>& SoundIds);
	/**
	 * What a weapon's document names. Its skeletal mesh and attachment meshes on every machine (a dedicated server
	 * builds the weapon definition too, and the definition needs them); its materials and sounds off a dedicated server.
	 */
	void QueueWeaponPartsOf(const FString& WeaponId, const FMantleWeaponAsset& Named);
	/**
	 * What a wheel's document names: its static mesh (no collision) and materials, on a machine that renders only; a dedicated
	 * server keeps the document alone. Returns the records the wheel waits for (empty off a rendering machine).
	 */
	TArray<FString> QueueWheelPartsOf(const FString& WheelId, const FMantleWheelAsset& Named);
	void QueueBuild(const FBuildKey& Key);
	void PromoteMaterials();
	void RunBuilds();
	/**
	 * A mesh whose Nanite file is big has it read on a worker before the build (the read is a copy of all of it). True when the build can go ahead now: nothing to read off the
	 * game thread, or the read is done; false while it is still running (the build is tried again next frame).
	 */
	bool NaniteReadReady(FBuild& Build);
	/** The root bytes of the Nanite data a mesh build will register (its file's header), 0 when it will register none. Cached on the build. */
	int64 NaniteRootBytesOf(FBuild& Build);
	void BuildOne(FBuild& Build);
	void OnCollisionCooked(const FBuildKey& Key, bool bSucceeded);
	void MarkReady(FBuild& Build, UObject* Object);
	void FailBuild(FBuild& Build, const FString& Error);
	void ReleasePending(FBuild& Build);

	void DrainCompletions();
	void CheckComplete();
	void LogProgress();

	/** Whether every sound record has failed or finished its build. */
	bool AllSoundsSettled() const;

	/** Adds the settled sounds to Result (once), and says so in the log after OnDone has fired. */
	void SettleSounds();

	/** Calls the WhenLiveSoundSettles callbacks whose sound has settled. */
	void NotifySoundWaiters();

	/** The mesh or material record for an id (textures are not looked up by id alone). */
	const FRecord* FindRecord(const FString& Id, EAssetKind Kind) const;
	UObject* FindObject(const FString& Id, EAssetKind Kind, FString* OutError) const;

	/** What Record (an id's record of Kind, or null) built, or null with the reason. */
	UObject* ObjectOf(const FRecord* Record, const FString& Id, EAssetKind Kind, FString* OutError) const;

	FMantlePlaceAssetSettings Settings;
	TSharedRef<FMantleAssetStore> Store;
	TSharedRef<IMantleAssetResolver> Resolver;
	TSharedRef<FCompletionQueue, ESPMode::ThreadSafe> Completions;
	FMantlePlaceAssetsDone OnDone;

	TMap<FString, FRecord> Records;
	TMap<FBuildKey, FBuild> Builds;

	/** Every answer seen, by id, including ones not asked for (a material's textures). */
	TMap<FString, FMantleUploadedAsset> ResolvedCache;

	/** Materials resolved whose cookInfo names textures, turned into texture records at the next Pump. */
	TArray<TPair<FString, TMap<FString, FString>>> PendingMaterialTextures;

	/**
	 * A vehicle reused from an earlier set is never decoded here, so what its document names (its materials
	 * and liveries) is taken from the built vehicle and turned into records at the next Pump, as a decoded one's are.
	 */
	struct FPendingVehicleLooks
	{
		FString VehicleId;
		TArray<FString> MaterialIds;
		TArray<FString> LiveryIds;
		/** Item 2: its parts' meshes. */
		TArray<FString> MeshIds;
		/** Its horn's sound. */
		TArray<FString> SoundIds;
	};
	TArray<FPendingVehicleLooks> PendingVehicleLooks;

	/** Meshes that built but whose collision could not be cooked: reported with the result. */
	TArray<FMantleParseError> CollisionErrors;

	int64 DownloadedBytes = 0;

	TArray<FBuildKey> FetchQueue;

	/** Sounds wait here until everything else has started downloading: they never hold the place up. */
	TArray<FBuildKey> SoundFetchQueue;

	TArray<FBuildKey> DecodeQueue;
	TArray<FBuildKey> BuildQueue;
	TArray<FBuildKey> WaitingMaterials;

	int32 ResolvesInFlight = 0;
	int32 FetchesInFlight = 0;
	int32 DecodesInFlight = 0;
	int64 PendingBuildBytes = 0;

	double StartSeconds = 0.0;
	double NextProgressLogAt = 0.0;
	/** Game-thread time spent in BuildOne, for the TIMING line at completion. */
	int32 GameThreadBuilds = 0;
	double GameThreadBuildSeconds = 0.0;
	double LongestBuildSeconds = 0.0;
	FString LongestBuildId;
	bool bStarted = false;
	bool bCompleted = false;
	bool bSoundsSettled = false;

	/** WhenLiveSoundSettles callbacks by sound id, waiting for it to arrive. */
	TMap<FString, TArray<FSoundSettled>> SoundWaiters;

	FMantlePlaceAssetsResult Result;
	FTSTicker::FDelegateHandle TickerHandle;
};
