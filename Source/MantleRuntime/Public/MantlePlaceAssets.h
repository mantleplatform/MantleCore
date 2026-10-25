// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "MantleApiClient.h"
#include "MantleAssetStore.h"
#include "MantleHeightfieldReader.h"
#include "MantlePlaceMaterial.h"
#include "MantlePlaceTypes.h"
#include "MantleRuntimeMesh.h"
#include "MantleRuntimeTextureBuilder.h"
#include "MantleSoundReader.h"
#include "MantleStaticMeshReader.h"
#include "MantleVehicleAsset.h"
#include "MantleWeaponAsset.h"
#include "UObject/GCObject.h"
#include "UObject/ObjectPtr.h"

class UMaterialInterface;
class UMantleBuiltVehicle;
class UMantleBuiltWeapon;
class UObject;
class USkeletalMesh;
class UStaticMesh;
class UTexture;
struct FMantleAnimClip;
struct FMantleSkinnedMesh;
struct FMantleTerrainWeights;

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
	 * sound, a skeletal mesh or a clip (#469: a skeletal mesh has no collision).
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

	/** Where bytes come from; the process's store when null. */
	TSharedPtr<FMantleAssetStore> Store;

	/** How ids resolve; the API's when null (the server form when bRenderAssets is off on a dedicated server). */
	TSharedPtr<IMantleAssetResolver> Resolver;

	/**
	 * #462: FMantlePlaceAssets::PrepareManifestOnly takes at most this many of the manifest's
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
	 * #462: this process's settings for PrepareManifestOnly: ForThisProcess at a gentler pace (one
	 * resolve, four downloads and two decodes at a time, a 2 ms build slice), since it runs while
	 * the place is already being played.
	 */
	static FMantlePlaceAssetSettings ForManifestOnly();
};

/** #462: where an uploaded id stands in the live sets, for a caller that can wait for it (a script's spawn). */
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

	/** Creator vehicles built (#435). */
	int32 Vehicles = 0;

	/** Uploaded sounds decoded (#324). Filled in once they settle (FMantlePlaceAssets::AreSoundsSettled). */
	int32 Sounds = 0;

	/** Uploaded skeletal meshes built and clips decoded (#469). */
	int32 SkeletalMeshes = 0;
	int32 Clips = 0;
	/** Terrain heightfields decoded (#466). The server needs them too: they are the terrain's collision. */
	int32 Heightfields = 0;

	/** Dedicated server: material slots it did not resolve (nothing renders there). */
	int32 SkippedMaterials = 0;

	/** Dedicated server: uploaded sounds it did not resolve (nothing is heard there). */
	int32 SkippedSounds = 0;

	/** Dedicated server: uploaded skeletal meshes and clips it did not resolve (nothing renders there, #469). */
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
 * builder spawns anything: Fab F8 (#342).
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
 * A Decal's textures (#472) are records of their own, read in the decal's roles, and built like a
 * material's. A dedicated server resolves and builds meshes only, and skips every material (and so
 * every texture) and every sound. While the set lives, FMantleAssetLibrary::LoadMesh/LoadMaterial/
 * LoadTexture answer its ids (the newest live set first), which is how the unchanged
 * FMantlePlaceBuilder finds them. A failed asset is an error naming its id, reported in the result
 * and logged, never a silent stand-in.
 *
 * Sounds (#324, a SoundEmitter's uploaded sound, #459, any sound the manifest lists, which scripts
 * may play through Sounds:Play, and #546, a creator vehicle's horn) take the same resolve, fetch and decode path (MAU1
 * read on a background task) but never hold up the place: OnDone fires once everything else has
 * settled, sounds download after every other asset has started, and the builder binds each emitter
 * when its sound arrives (WhenLiveSoundSettles). Nothing is built on the game thread for a sound:
 * each emitter plays the shared decoded samples through its own UMantleSoundWave. The set keeps
 * pumping itself until its sounds have settled too.
 *
 * Skeletal meshes and clips (#469, a SkeletalMesh entity's uploaded mesh and clip) hold the place up like
 * meshes: an MSK1 place skeletal mesh (role 3) is read on a background task and built on the game thread
 * as a USkeletalMesh on a transient skeleton of its own bones; a MAN1 place clip is read on a background
 * task and its decoded keys are the build (nothing is made on the game thread; each entity samples the
 * shared clip). A dedicated server resolves neither: nothing renders there.
 *
 * A Terrain (#466) adds three uses: its heightfield (MHF1, read on a background task; the decoded
 * grid is the build, as a sound's samples are, and the place builder turns it into chunk meshes),
 * which a dedicated server needs as well because it is the terrain's collision; its weightmap, a
 * texture read as linear data; and each layer's material. The last two are skipped on a server. When
 * its layers name surfaces (#596), the weightmap is also read on the CPU, on every net mode, into
 * the heaviest layer per texel (TerrainWeights), which is what a hit on the terrain resolves against.
 * Its grass (#628, drawn only) takes its meshes and materials like a Scatter's, and its grass maps as
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
	 * A SoundEmitter's sound that the live set answers (#324): an uploaded id the built-in library
	 * does not know. A library entry wins, so a sound id the library resolves (tests register
	 * ast_u-shaped ids there) builds exactly as it did before uploaded sounds existed.
	 */
	static bool IsUploadedSoundId(const FString& Id);

	/**
	 * True when an entity of the place uses an uploaded mesh, material, (a Decal's, #472) texture or (a
	 * VehicleSpawn's, #435) vehicle, a Terrain (#466, whose heightfield is always uploaded), or (off a
	 * dedicated server, which never hears or draws one) an uploaded sound, skeletal mesh or clip (#469), or
	 * the manifest lists an uploaded sound (one scripts may play, #459) or vehicle (a script can spawn it),
	 * or (off a dedicated server) a UI document shows an uploaded image (#556 item 5):
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
	 * #462: true when the place's manifest lists uploaded meshes (and, when bRenderAssets, uploaded
	 * materials) that no entity uses: assets a script can spawn (Assets:Spawn) or swap onto an
	 * entity (Entity:SetMaterial). Prepare leaves those out, so they need PrepareManifestOnly. A
	 * vehicle's part meshes and materials (#435) are Prepare's, not these: they count as used once a
	 * live set has built the vehicle, so ask only after the place's own set has completed.
	 */
	static bool HasManifestOnlyAssets(const FMantlePlace& Place, bool bRenderAssets);

	/**
	 * #462: prepares the manifest-only assets HasManifestOnlyAssets describes, exactly as Prepare
	 * does the entities' (meshes with collision; materials and their textures only when
	 * Settings.bRenderAssets), up to Settings.MaxManifestOnlyAssets and MaxManifestOnlyBytes. Start
	 * it once the place is built, never before: the place and the join never wait for it. While it
	 * loads, GetLiveMeshState / GetLiveMaterialState say Loading for its ids.
	 */
	static TSharedRef<FMantlePlaceAssets> PrepareManifestOnly(const FMantlePlace& Place, const FMantlePlaceAssetSettings& Settings,
		FMantlePlaceAssetsDone OnDone);

	/**
	 * #462: PrepareManifestOnly with ForManifestOnly settings for a place this process has just
	 * built, or null when the place has no manifest-only assets. Logs a summary when it completes.
	 * Keep the result exactly as long as the place's own set.
	 */
	static TSharedPtr<FMantlePlaceAssets> PrepareManifestOnlyForThisProcess(const FMantlePlace& Place);

	/** The API resolver: the server route on a dedicated server (bServer), else the batch route with a per-id fallback. */
	static TSharedRef<IMantleAssetResolver> CreateApiResolver(bool bServer);

	/** The built mesh for an uploaded id in the newest live set that has it; null with the reason otherwise. */
	static UStaticMesh* FindLiveMesh(const FString& Id, FString& OutError);

	/** The built material for an uploaded id, as FindLiveMesh. */
	static UMaterialInterface* FindLiveMaterial(const FString& Id, FString& OutError);

	/** The built texture an entity names directly (a Decal's, #472), read with Role, as FindLiveMesh. */
	static UTexture* FindLiveTexture(const FString& Id, EMantleTextureRole Role, FString& OutError);

	/**
	 * #488: where an uploaded texture read with Role stands in the newest live set that has it, as GetLiveMeshState: a
	 * vehicle's livery (read as colour) is prepared with its vehicle off a dedicated server, so a car can wait for it.
	 */
	static EMantleLiveAssetState GetLiveTextureState(const FString& Id, EMantleTextureRole Role, FString* OutError = nullptr);

	/** The built skeletal mesh (on its own skeleton) for an uploaded id, as FindLiveMesh (#469). */
	static USkeletalMesh* FindLiveSkeletalMesh(const FString& Id, FString& OutError);

	/**
	 * The decoded place clip for an uploaded id, as FindLiveMesh (#469); shared by every entity playing it.
	 * bOutStillLoading, when given, says the null is only because it has not arrived yet (a script's
	 * Entity:PlayClip of a manifest-only clip, script API 1.18, waits for it).
	 */
	static TSharedPtr<const FMantleAnimClip> FindLiveClip(const FString& Id, FString& OutError, bool* bOutStillLoading = nullptr);

	/**
	 * The decoded sound for an uploaded id in the newest live set that has it (#324); null with the
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
	 * The built creator vehicle for an uploaded Vehicle id (#435) in the newest live set that has it; null
	 * with the reason otherwise. Every vehicle a place's VehicleSpawns use or its manifest lists is prepared
	 * with the place, on the server (for its physics) and on clients.
	 */
	static UMantleBuiltVehicle* FindLiveVehicle(const FString& Id, FString& OutError);

	/** #435: where an uploaded vehicle id stands in the newest live set that has it, as GetLiveMeshState. */
	static EMantleLiveAssetState GetLiveVehicleState(const FString& Id, FString* OutError = nullptr);

	/**
	 * The read creator weapon (#789 stage 4) for an uploaded Weapon id in the newest live set that has it: the document text
	 * and the ids it names. Null with the reason otherwise. Every weapon a place's WeaponSpawns use or its manifest lists is
	 * prepared with the place on every machine, with the skeletal mesh and attachment meshes the document names (and, off a
	 * dedicated server, its materials and sounds): FindLiveSkeletalMesh / FindLiveMesh / FindLiveMaterial / FindLiveSound
	 * answer for them. An attachment id is one of FindLiveSkeletalMesh or FindLiveMesh, whichever the API says it is.
	 */
	static UMantleBuiltWeapon* FindLiveWeapon(const FString& Id, FString& OutError);

	/** #789: where an uploaded weapon id stands in the newest live set that has it, as GetLiveMeshState. */
	static EMantleLiveAssetState GetLiveWeaponState(const FString& Id, FString* OutError = nullptr);

	/** #462: where an uploaded mesh id stands in the newest live set that has it; OutError says why when it failed or was never prepared. */
	static EMantleLiveAssetState GetLiveMeshState(const FString& Id, FString* OutError = nullptr);

	/** #462: the same for an uploaded material id. */
	static EMantleLiveAssetState GetLiveMaterialState(const FString& Id, FString* OutError = nullptr);

	/** #789: the same for an uploaded skeletal mesh id (a weapon's mesh, or an attachment the API says is one). */
	static EMantleLiveAssetState GetLiveSkeletalMeshState(const FString& Id, FString* OutError = nullptr);

	/** The decoded heightfield for an uploaded id in the newest live set that has it (#466); null with the reason otherwise. */
	static TSharedPtr<const FMantleHeightfield> FindLiveHeightfield(const FString& Id, FString& OutError);

	/**
	 * A Terrain weightmap's heaviest layer per texel (#596) for an uploaded id in the newest live set
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

	/** The decoded sound for an uploaded id; null with the reason otherwise, bOutStillLoading when it has not arrived yet. */
	TSharedPtr<const FMantleSoundData> FindSound(const FString& Id, FString* OutError = nullptr, bool* bOutStillLoading = nullptr) const;

	/** The decoded heightfield for an uploaded id (#466); null with the reason otherwise. */
	TSharedPtr<const FMantleHeightfield> FindHeightfield(const FString& Id, FString* OutError = nullptr) const;

	/** A Terrain weightmap's heaviest layers for an uploaded id (#596); null with the reason otherwise. */
	TSharedPtr<const FMantleTerrainWeights> FindTerrainWeights(const FString& Id, FString* OutError = nullptr) const;

	// FGCObject
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("FMantlePlaceAssets"); }

private:
	/**
	 * TerrainWeights (#596) is an uploaded Texture (the API's kind) read on the CPU into a Terrain's
	 * heaviest layer per texel, beside (not instead of) the GPU texture a client builds from the same id.
	 */
	enum class EAssetKind : uint8 { Mesh, Material, Texture, Sound, SkeletalMesh, AnimClip, Heightfield, Vehicle, TerrainWeights, Weapon };

	using FSoundSettled = TFunction<void(TSharedPtr<const FMantleSoundData>, const FString&)>;

	enum class EBuildState : uint8 { Queued, Fetching, Fetched, Decoding, WaitingForTextures, Building, CookingCollision, Ready, Failed };

	/** One distinct thing to build: a cooked hash and what it is used as. */
	struct FBuildKey
	{
		FMantleContentHash Hash;
		EAssetKind Kind = EAssetKind::Mesh;

		/** Meshes: 1 with collision. Textures: the EMantleTextureRole. Terrain weights: 1 keeps the pixels (#628). Materials: 0. */
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
		TSharedPtr<FMantleStaticMeshData> MeshV2;
		TSharedPtr<FMantleRuntimeMesh> MeshV1;
		TSharedPtr<FMantleDecodedTexture> Texture;
		TSharedPtr<FMantlePlaceMaterialDesc> Material;

		/** Sounds: the decoded samples, which are what a sound build is (kept while the set lives). */
		TSharedPtr<const FMantleSoundData> Sound;

		/** Skeletal meshes (#469): the decoded MSK1, until the game thread builds it. */
		TSharedPtr<FMantleSkinnedMesh> Skinned;

		/** Clips (#469): the decoded keys, which are what a clip build is (kept while the set lives). */
		TSharedPtr<const FMantleAnimClip> Clip;

		/** Heightfields (#466): the decoded grid, likewise the build; the place builder makes the meshes. */
		TSharedPtr<const FMantleHeightfield> Heightfield;

		/** Terrain weights (#596): the heaviest layer per texel, likewise the build; the weightmap's pixels are dropped once read. */
		TSharedPtr<const FMantleTerrainWeights> TerrainWeights;

		/** Vehicles (#435): the read MVH1, until the game thread builds its UMantleBuiltVehicle. */
		TSharedPtr<FMantleVehicleAsset> Vehicle;

		/** Weapons (#789): the read MWP1, until the game thread wraps it in its UMantleBuiltWeapon. */
		TSharedPtr<FMantleWeaponAsset> Weapon;

		/** Materials: the texture record per slot, a layer blend's included (#533; empty when the slot has none). */
		FString TextureRecords[MantlePlaceMaterialTextureCount];

		TObjectPtr<UObject> Object;

		bool IsTerminal() const { return State == EBuildState::Ready || State == EBuildState::Failed; }
	};

	/** One uploaded id in one use (a texture used as colour and as data is two records). */
	struct FRecord
	{
		FString Id;
		EAssetKind Kind = EAssetKind::Mesh;

		/** Textures: the role it is read with. */
		EMantleTextureRole Role = EMantleTextureRole::Color;

		/** Meshes: an entity collides with it. */
		bool bCollision = false;

		/** Terrain weights: a grass map (#628), whose four channels are read too, so the decode keeps the pixels. */
		bool bKeepPixels = false;

		/**
		 * Meshes (#789): a weapon's attachment, which the API may say is a skeletal mesh instead. When it does, ApplyResolved
		 * swaps this record for a skeletal mesh one.
		 */
		bool bMeshOrSkeletal = false;

		bool bResolving = false;
		bool bResolved = false;
		int32 ResolveAttempts = 0;
		double NextResolveAt = 0.0;

		/** Re-resolved after a 403/404 download (an expired URL). */
		bool bReresolved = false;

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
	/** #462: PrepareManifestOnly's Start: the manifest's uploaded meshes and materials no entity or built vehicle uses, within the caps. */
	void StartManifestOnly(const FMantlePlace& Place);
	static TSharedRef<FMantlePlaceAssets> PrepareWith(const FMantlePlace& Place, const FMantlePlaceAssetSettings& Settings,
		FMantlePlaceAssetsDone OnDone, bool bManifestOnly);
	/** #462: GetLiveMeshState / GetLiveMaterialState across the live sets, newest first. */
	static EMantleLiveAssetState LiveStateOf(const FString& Id, EAssetKind Kind, FString* OutError);
	/** #462: one record's state in this set. */
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
	void OnDecoded(const FBuildKey& Key, TSharedPtr<FMantleStaticMeshData> MeshV2, TSharedPtr<FMantleRuntimeMesh> MeshV1,
		TSharedPtr<FMantleDecodedTexture> Texture, TSharedPtr<FMantlePlaceMaterialDesc> Material, TSharedPtr<const FMantleSoundData> Sound,
		TSharedPtr<FMantleSkinnedMesh> Skinned, TSharedPtr<const FMantleAnimClip> Clip, TSharedPtr<const FMantleHeightfield> Heightfield,
		TSharedPtr<const FMantleTerrainWeights> TerrainWeights, TSharedPtr<FMantleVehicleAsset> Vehicle, TSharedPtr<FMantleWeaponAsset> Weapon, const FString& Error);
	/** #435: a vehicle's materials, from its document, as material records (off a dedicated server). */
	void QueueMaterialsOf(const FString& VehicleId, const TArray<FString>& MaterialIds);
	/** #488: a vehicle's liveries, from its document, as colour texture records (off a dedicated server). */
	void QueueLiveriesOf(const FString& VehicleId, const TArray<FString>& TextureIds);
	/** #435 item 2: a vehicle's part meshes, from its document, as mesh records without collision (off a dedicated server). */
	void QueueMeshesOf(const FString& VehicleId, const TArray<FString>& MeshIds);
	/**
	 * #546: a vehicle's sounds (its horn's), from its document, as sound records (off a dedicated server, which hears
	 * nothing): they never hold the place up, as no sound does.
	 */
	void QueueSoundsOf(const FString& VehicleId, const TArray<FString>& SoundIds);
	/**
	 * #789: what a weapon's document names. Its skeletal mesh and attachment meshes on every machine (a dedicated server
	 * builds the weapon definition too, and the definition needs them); its materials and sounds off a dedicated server.
	 */
	void QueueWeaponPartsOf(const FString& WeaponId, const FMantleWeaponAsset& Named);
	void QueueBuild(const FBuildKey& Key);
	void PromoteMaterials();
	void RunBuilds();
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
	 * #435/#488: a vehicle reused from an earlier set is never decoded here, so what its document names (its materials
	 * and liveries) is taken from the built vehicle and turned into records at the next Pump, as a decoded one's are.
	 */
	struct FPendingVehicleLooks
	{
		FString VehicleId;
		TArray<FString> MaterialIds;
		TArray<FString> LiveryIds;
		/** #435 item 2: its parts' meshes. */
		TArray<FString> MeshIds;
		/** #546: its horn's sound. */
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
	/** Game-thread time spent in BuildOne (#126), for the TIMING line at completion. */
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
