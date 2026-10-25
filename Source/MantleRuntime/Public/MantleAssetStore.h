// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Hash/Blake3.h"
#include "Misc/DateTime.h"
#include "Misc/Timespan.h"
#include "Tasks/Task.h"

#include <atomic>

class FArchive;
class FMantleAssetStore;
class FMantleAssetSink;

/**
 * The content address of some cooked bytes: "blake3:" and 64 lowercase hex digits.
 *
 * BLAKE3 only. It is the standard PLACE_SCHEMA.md names for the backend, it is in the engine's
 * Core module (FBlake3), and it runs on every platform the client and the dedicated server ship
 * on. The schema's other algorithms (sha256, sha1, md5) are refused by name rather than trusted
 * unverified: a store that skips verification for some hashes is a store that can be handed
 * arbitrary bytes under a borrowed name.
 */
struct MANTLERUNTIME_API FMantleContentHash
{
	FBlake3Hash Digest;

	/** Reads "blake3:<64 lowercase hex>". OutError says what was wrong, for the log. */
	static bool Parse(const FString& Text, FMantleContentHash& OutHash, FString* OutError = nullptr);

	/** The hash of these bytes. */
	static FMantleContentHash OfBytes(const void* Data, uint64 Size);
	static FMantleContentHash OfBytes(TConstArrayView<uint8> Bytes) { return OfBytes(Bytes.GetData(), Bytes.Num()); }

	/** False for the zero hash a failed Parse leaves behind. */
	bool IsValid() const { return !Digest.IsZero(); }

	/** "blake3:<hex>", the form manifests carry. */
	FString ToString() const;

	/** The 64 hex digits alone: the file name in the store. */
	FString ToHex() const;

	bool operator==(const FMantleContentHash& Other) const { return Digest == Other.Digest; }
	bool operator!=(const FMantleContentHash& Other) const { return Digest != Other.Digest; }

	friend uint32 GetTypeHash(const FMantleContentHash& Hash) { return GetTypeHash(Hash.Digest); }
};

/**
 * One asset the store can fetch: what the bytes must hash to, and where to get them.
 *
 * Hash and Bytes describe the object that is downloaded (the cooked asset), never the creator's
 * upload: the store verifies the bytes it receives against Hash, and refuses a response larger
 * than Bytes.
 */
struct MANTLERUNTIME_API FMantleStoredAssetRef
{
	FMantleContentHash Hash;

	/** The download URL: GET /assets/:id's url, or a manifest's. Empty means "only if cached". */
	FString Url;

	/** Size of the cooked object. 0 when unknown; the store's MaxAssetBytes cap applies instead. */
	int64 Bytes = 0;
};

/** Whether a caller wants the bytes in memory or only a verified file on disk. */
enum class EMantleAssetLoad : uint8
{
	/** Ensure the file is in the store and verified; Result.Bytes stays null. Pin it before use. */
	PathOnly,

	/** Also read the verified bytes into memory. Every waiter on one hash shares one buffer. */
	Bytes,
};

/** What a request got. Delivered on the game thread. */
struct MANTLERUNTIME_API FMantleAssetResult
{
	bool bSuccess = false;

	FMantleContentHash Hash;

	/** The file in the store. Valid while the asset is pinned (see FMantleAssetStore::Pin). */
	FString Path;

	/** Filled for EMantleAssetLoad::Bytes. Shared, read-only, safe to hand to a worker thread. */
	TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Bytes;

	/** True when no download was needed. */
	bool bFromCache = false;

	/** Size of the stored file. */
	int64 SizeBytes = 0;

	/** The last HTTP status on failure, or 0 when the request never got an answer. A 403 or 404
	    usually means the URL expired or the asset was taken down: re-resolve, then ask again. */
	int32 StatusCode = 0;

	/** Why it failed, for the log and for whoever builds a player-facing message from it. */
	FString Error;
};

DECLARE_DELEGATE_OneParam(FMantleAssetDone, const FMantleAssetResult&);

/** What one fetch attempt came back with. */
struct MANTLERUNTIME_API FMantleAssetFetchResult
{
	/** True when a response arrived, whatever its status. */
	bool bConnected = false;

	int32 StatusCode = 0;

	FString Error;
};

/**
 * The transport under the store: the real one is HTTP, tests supply a fake.
 *
 * Fetch is called on the game thread. It streams the response body into Sink (from any thread,
 * in order), then calls OnDone exactly once (from any thread). Sink's GetError() turns true when
 * the store refuses more bytes (the response outgrew the asset's size); a fetcher should stop
 * writing then, and the HTTP one does, because the engine's HTTP layer checks it.
 */
class MANTLERUNTIME_API IMantleAssetFetcher
{
public:
	virtual ~IMantleAssetFetcher() = default;

	virtual void Fetch(
		const FString& Url, const TSharedRef<FArchive>& Sink,
		TFunction<void(const FMantleAssetFetchResult&)> OnDone) = 0;
};

/** The store's knobs. FromConfig reads [Mantle.AssetStore] in the game ini over these defaults. */
struct MANTLERUNTIME_API FMantleAssetStoreSettings
{
	/** Saved/Mantle/Assets by default. */
	FString RootDir;

	/** Total bytes of unpinned assets kept on disk. Pinned assets may take it over; see Pin. */
	int64 DiskBudgetBytes = 4ll * 1024 * 1024 * 1024;

	/** Eviction frees down to this fraction of the budget, so a full store does not evict on
	    every single download. */
	double EvictionLowWater = 0.9;

	/** Downloads in flight at once, across every caller. */
	int32 MaxConcurrentDownloads = 6;

	/** Attempts per asset, across its URLs. A corrupt download counts as a failed attempt. */
	int32 MaxAttempts = 3;

	/** Hard ceiling on one asset, whatever a manifest claims. TArray<uint8> is int32-sized. */
	int64 MaxAssetBytes = 512ll * 1024 * 1024;

	/** A partial download older than this is a leftover from a crash and is deleted at startup.
	    Younger ones may belong to another process sharing Saved/ (a local -server and -game). */
	FTimespan StalePartialAge = FTimespan::FromHours(1.0);

	/** Pump from the core ticker every frame. Tests turn it off and pump by hand. */
	bool bAutoPump = true;

	static FMantleAssetStoreSettings FromConfig();
};

/**
 * The content-addressed disk store for uploaded assets: F7 (#341), used by the place loader
 * (F8) and the avatar cosmetic cache (A3.1). Lives in MantleRuntime because the place builder
 * is here, and client/Source may depend on MantleCore but never the reverse.
 *
 * Files live at <RootDir>/<64 hex digits of the BLAKE3 hash>. Nothing is ever read from the
 * store without its hash being checked: a download is hashed as it streams to a partial file
 * and only renamed into place when it matches, and a cached file is re-hashed when it is read
 * into memory (and once per process for path-only callers). A mismatch is corruption: the file
 * is deleted and fetched again.
 *
 * Built for many players joining places with hundreds of assets:
 * - One download per hash however many callers ask (coalescing), and at most
 *   MaxConcurrentDownloads at once.
 * - Disk work (scan, hash, read, rename, delete) runs on background tasks; the game thread only
 *   takes a lock, starts HTTP requests and delivers callbacks.
 * - Writes are atomic: stream to <RootDir>/.partial/<hex>.<guid>.part, verify, rename.
 * - A disk budget with LRU eviction (least recently used first; file timestamps carry recency
 *   across restarts) that never evicts a pinned asset or one being read or fetched.
 * - No UI and no platform-specific API: it runs the same in a Linux dedicated server.
 *
 * The public API is for the game thread; callbacks arrive there too, one Pump later.
 */
class MANTLERUNTIME_API FMantleAssetStore : public TSharedFromThis<FMantleAssetStore>
{
public:
	/** The process's store, created on first use with FromConfig settings and the HTTP fetcher. */
	static FMantleAssetStore& Get();

	/** Drops the process's store. Called at module shutdown. */
	static void ShutdownDefault();

	/** A store of its own: tests, tools. Starts the background scan of RootDir immediately. */
	static TSharedRef<FMantleAssetStore> Create(
		const FMantleAssetStoreSettings& InSettings, TSharedRef<IMantleAssetFetcher> InFetcher);

	/** The engine-HTTP fetcher. Never sends the Mantle sign-in token: asset URLs are CDN URLs. */
	static TSharedRef<IMantleAssetFetcher> CreateHttpFetcher();

	~FMantleAssetStore();

	/**
	 * Asks for one asset. OnDone fires on the game thread exactly once, unless cancelled.
	 * Returns an id for Cancel. A hash already being resolved or downloaded gains a waiter
	 * instead of a second request.
	 */
	uint64 Request(const FMantleStoredAssetRef& Ref, EMantleAssetLoad Load, FMantleAssetDone OnDone);

	/** Stops OnDone from firing. A download nobody waits on any more still finishes if it has
	    started (the bytes are likely wanted again); a queued one is dropped. */
	void Cancel(uint64 RequestId);

	/**
	 * Puts a local copy of an asset into the store, as if it had been downloaded: the client's
	 * Builtin bundle, a pre-warmed cache (#429). SourceFile is read on a
	 * background task after the startup scan, checked against Ref.Hash (and Ref.Bytes when set)
	 * and written in like a finished download; bytes that do not match are refused and nothing
	 * is stored. A hash already stored is left alone (bFromCache). Ref.Url is not used, and
	 * SourceFile is never changed. OnDone fires on the game thread exactly once. Pin the hash
	 * first when it must survive eviction.
	 */
	void Seed(const FMantleStoredAssetRef& Ref, const FString& SourceFile, FMantleAssetDone OnDone);

	/** Reference-counted: eviction never removes a pinned hash. Pinning a hash that is not
	    stored yet is allowed, and protects it once it arrives. */
	void Pin(const FMantleContentHash& Hash);
	void Unpin(const FMantleContentHash& Hash);

	/** True when the hash is stored (not re-verified here). */
	bool Contains(const FMantleContentHash& Hash) const;

	/** Bytes received so far for a hash being downloaded, else 0. */
	int64 GetInFlightBytes(const FMantleContentHash& Hash) const;

	/** Where a hash's file lives, whether or not it is stored. */
	FString PathFor(const FMantleContentHash& Hash) const;

	const FMantleAssetStoreSettings& GetSettings() const { return Settings; }

	/** Starts queued downloads and delivers finished callbacks. The core ticker calls it every
	    frame when bAutoPump is set. Game thread only. */
	void Pump();

	/** Pumps until nothing is queued, in flight or undelivered, or the time runs out. For tools,
	    tests and shutdown, never a running game. Returns whether it went idle. */
	bool WaitUntilIdle(float TimeoutSeconds);

private:
	struct FWaiter
	{
		uint64 RequestId = 0;
		bool bWantsBytes = false;
		FMantleAssetDone OnDone;
	};

	enum class EJobState : uint8 { Resolving, Queued, Downloading, Finalizing };

	struct FJob
	{
		FMantleContentHash Hash;
		TArray<FString> Urls;
		int64 ExpectedBytes = 0;
		TArray<FWaiter> Waiters;
		EJobState State = EJobState::Resolving;
		int32 Attempts = 0;
		int32 UrlIndex = 0;
		int32 LastStatus = 0;
		FString LastError;
		TSharedPtr<FMantleAssetSink> Sink;
	};

	struct FEntry
	{
		int64 Size = 0;
		int64 UseSerial = 0;
		int32 Readers = 0;
		bool bVerified = false;
	};

	FMantleAssetStore(const FMantleAssetStoreSettings& InSettings, TSharedRef<IMantleAssetFetcher> InFetcher);

	void StartScan();
	void Scan();
	void Resolve(TSharedRef<FJob> Job);
	void EnqueueDownload(const TSharedRef<FJob>& Job);
	void Finalize(const TSharedRef<FJob>& Job, const TSharedRef<FMantleAssetSink>& Sink,
		const FMantleAssetFetchResult& Result);
	void Deliver(const TSharedRef<FJob>& Job, bool bFromCache, int64 Size,
		TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Bytes);
	void Fail(const TSharedRef<FJob>& Job, const FString& Error, int32 StatusCode);
	/** Seed's background half. */
	FMantleAssetResult SeedFromFile(const FMantleStoredAssetRef& Ref, const FString& SourceFile);
	void EvictToBudget();
	void Post(TFunction<void()>&& Completion);

	/** Runs Body on a background task (after the startup scan when asked), counted for IsIdle. */
	void LaunchTask(const TCHAR* DebugName, TUniqueFunction<void()> Body, bool bAfterScan);

	/** Consumes a request id; false when it was cancelled (or already delivered). */
	bool TakeRequest(uint64 RequestId);
	FString PartialDir() const;
	bool IsIdle() const;

	/** Lock held: whether eviction may remove this hash now. */
	bool IsEvictableLocked(const FMantleContentHash& Hash, const FEntry& Entry) const;

	const FMantleAssetStoreSettings Settings;
	const TSharedRef<IMantleAssetFetcher> Fetcher;

	/** Guards everything below it. File renames and deletes of stored files also happen under
	    it, one file at a time, so a delete can never race a rename of the same hash. */
	mutable FCriticalSection Lock;
	TMap<FMantleContentHash, FEntry> Index;
	TMap<FMantleContentHash, TSharedRef<FJob>> Jobs;
	TMap<FMantleContentHash, int32> Pins;
	TMap<uint64, FMantleContentHash> RequestHashes;
	TArray<FMantleContentHash> DownloadQueue;
	int64 StoredBytes = 0;
	int64 NextUseSerial = 1;
	uint64 NextRequestId = 1;
	int32 ActiveDownloads = 0;
	int32 PendingTasks = 0;
	bool bWarnedOverBudget = false;

	mutable FCriticalSection CompletionLock;
	TArray<TFunction<void()>> Completions;

	UE::Tasks::FTask ScanTask;
	std::atomic<bool> bScanComplete{false};
	FTSTicker::FDelegateHandle TickerHandle;
};
