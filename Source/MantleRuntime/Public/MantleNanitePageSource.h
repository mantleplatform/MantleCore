// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "IO/IoChunkId.h"

class FMantleAssetStore;
class UStaticMesh;
struct FMantleContentHash;
struct FMantleNaniteTable;

/** What the page source has done, since the process began (or since ResetStats): the proof run and the tests read these. */
struct FMantleNanitePageStats
{
	/** Streamed pages (reads) answered, and their bytes. */
	int64 PagesServed = 0;
	int64 BytesServed = 0;

	/** Reads of a chunk this source holds that were not exactly one page of it, or of a chunk whose mesh is gone (failed). */
	int64 Refused = 0;

	/** Reads that failed because the file was gone, could not be opened, was not the size its table says, or could not be read (after one retry). */
	int64 FileUnreadable = 0;

	/** Reads whose piece did not inflate to exactly what its table says. */
	int64 InflateFailed = 0;

	/** Reads the engine cancelled (they still finished once, and the request is reported cancelled). */
	int64 Cancelled = 0;

	/** Meshes whose streamed pages are held now. */
	int32 Stores = 0;

	/** Page bytes held in memory by the source: 0 by design (a read goes from the file to the engine's buffer). */
	int64 PageBytesHeld = 0;

	/** What the stores' tables hold, in bytes (the only thing the source keeps per mesh). */
	int64 TableBytes = 0;

	/** Files held open for reading now (at most MaxOpenFiles), and the engine's read buffers they hold (128 KB each in a packaged game, 0 in the editor). */
	int32 OpenFiles = 0;
	int64 OpenFileBufferBytes = 0;

	/** Asset-store pins held for the stores now (a store's file is pinned for as long as its mesh lives and a read of it is running). */
	int32 PinsHeld = 0;

	/** Reads waiting or running now. */
	int32 ReadsInFlight = 0;

	/** Time from the request reaching the source to its answer, and from the request to its read starting (queue wait): the mean and the worst of every page served, in milliseconds. */
	double MeanAnswerMs = 0.0;
	double WorstAnswerMs = 0.0;
	double MeanQueueMs = 0.0;
	double WorstQueueMs = 0.0;
};

/**
 * Where the engine reads the streamed pages of a place's Nanite meshes from. A mesh built at runtime has no package to stream from; the engine's own request for a page is a
 * read of (chunk, offset, size) through its I/O dispatcher, so this is a backend of that dispatcher (IIoDispatcherBackend, mounted once) that answers a read by reading the
 * page's piece from the mesh's downloaded file (MNZ2, contracts\nanite-v2.md) and inflating it straight into the buffer the engine handed over. It holds no page bytes: a store is
 * the file's path, the container's table and a pin on the file. See gates\fable-nanite-residency-3.check.md for the rules the backend follows.
 *
 * - The dispatcher's thread only claims a request and queues it; one queue is drained by at most MaxReadsInFlight tasks (UE::Tasks), which read and inflate. Every claimed request
 *   finishes exactly once, bytes written before it is marked done. A worker never touches the request object; the dispatcher's thread applies its result. A cancel only sets a flag.
 * - A file is opened by the task that needs it (one handle per file, shared by the meshes that name it, at most MaxOpenFiles open, least recently used closed first).
 * - A store lives as long as its mesh: Prune drops the ones whose mesh has been collected and gives the file's pin back once no read of it is running. A chunk id that is not one of ours (not the "MNPG" magic) is handed back to the dispatcher for the next
 *   backend; one of ours whose store is gone fails at once (it never waits). A dedicated server never registers a store, so it never mounts the backend.
 */
class MANTLERUNTIME_API FMantleNanitePageSource
{
public:
	/** The stores and counters (private to the source; named here so the dispatcher's backend can hold it). */
	struct FState;

	/** At most this many reads run at once (the engine's own decodes share the background workers). */
	static constexpr int32 MaxReadsInFlight = 4;

	/** At most this many files are open for reading at once. */
	static constexpr int32 MaxOpenFiles = 64;

	/** The one source. */
	static FMantleNanitePageSource& Get();

	/**
	 * The largest streamed page the engine can be sure to stage: its staging ring is r.Nanite.Streaming.MaxPendingPages (read now) x NANITE_ESTIMATED_MAX_PAGE_DISK_SIZE bytes and never
	 * resets its offsets, so only about half the ring is guaranteed to be free in one piece (NaniteStreamingManager.cpp FRingBufferAllocator): ring / 2 - 1. A mesh with a page past
	 * this one would stop all Nanite streaming (the engine never stages it), so FMantleNaniteSideFile::Read refuses it. A page between the estimate and this one waits its turn, as the engine's own do.
	 */
	static int64 MaxStreamedPageBytes();

	/**
	 * Holds the file at Path (its table, no bytes) as Mesh's streamed pages and returns the chunk id a read of them names. Mounts the backend on the first call. Game thread. The table
	 * needs at least one streamed page (else an invalid chunk id comes back and nothing is held). The file is opened later, by the first read. A read is exactly one page.
	 */
	FIoChunkId Register(const FString& Path, const TSharedRef<const FMantleNaniteTable>& Table, UStaticMesh* Mesh);

	/** As Register, and pins Hash in AssetStore for as long as the store (and a read of it) lives, so the file is never evicted under a mesh that is still drawn. Game thread. */
	FIoChunkId Register(const FString& Path, const TSharedRef<const FMantleNaniteTable>& Table, UStaticMesh* Mesh, const TSharedPtr<FMantleAssetStore>& AssetStore, const FMantleContentHash& Hash);

	/** Drops the stores whose mesh is gone (collected), returns how many, and gives back the pins of dropped stores that have no read running. Game thread. Also runs every few seconds once the backend is mounted, and on every Register. */
	int32 Prune();

	/** Whether the chunk id is one this source made (its magic), whether or not a store still holds it. */
	static bool IsMantleChunk(const FIoChunkId& ChunkId);

	FMantleNanitePageStats GetStats() const;

	/** Tests: the counters back to 0 (the stores stay). */
	void ResetStats();

	/** The size of a chunk's streamed block, or -1 when none holds it (a pruned or unknown chunk). */
	int64 GetStoreSize(const FIoChunkId& ChunkId) const;

#if WITH_DEV_AUTOMATION_TESTS
	/** Tests: called by a read task after it took the request and before it reads (to hold a read while a test cancels or prunes). Not a switch of behavior. */
	static TFunction<void()> GTestBeforeRead;
#endif

private:
	FMantleNanitePageSource();

	TSharedRef<FState> State;
};
