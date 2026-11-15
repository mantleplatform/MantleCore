// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "MantleAssetStore.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceCells.h"
#include "MantlePlaceTypes.h"

/** The cell loader's knobs. FromAssetSettings takes the place assets' store and resolver, so one place is fetched through one store. */
struct MANTLERUNTIME_API FMantleCellLoaderSettings
{
	/** Where bytes come from; the process's store when null. */
	TSharedPtr<FMantleAssetStore> Store;

	/** How ids resolve; the API's when null (the server form on a dedicated server). */
	TSharedPtr<IMantleAssetResolver> Resolver;

	/** Resolve requests in flight at once (each takes up to FMantleApiClient::MaxIdsPerResolve ids). */
	int32 MaxConcurrentResolves = 2;

	/** Attempts per id before a resolve failure is final. */
	int32 MaxResolveAttempts = 3;

	/** Store requests outstanding at once (the store itself caps concurrent downloads). */
	int32 MaxOutstandingFetches = 16;

	/** Cell files read (unpacked and parsed) on background tasks at once. A cell file is at most 8 MiB of JSON, so each can briefly hold a few hundred MB. */
	int32 MaxConcurrentDecodes = 2;

	/** Downloaded bytes waiting to be read; no new download starts above it. */
	int64 MaxPendingBytes = 128ll * 1024 * 1024;

	/** Pump from the core ticker every frame. Tests turn it off and pump by hand. */
	bool bAutoPump = true;

	/** The store, resolver and resolve attempts of Assets, which a place's own asset set also uses. */
	static FMantleCellLoaderSettings FromAssetSettings(const FMantlePlaceAssetSettings& Assets);
};

/** What loading a place's cells came to. */
struct MANTLERUNTIME_API FMantleCellLoadResult
{
	bool bSuccess = false;

	FMantleCellIndex Index;

	/** One per index cell, in the index's order, each holding its files' entities in file order. Empty when bSuccess is false. */
	TArray<FMantleCellContent> Cells;

	/** What went wrong, one line per problem: Path names the cell or file. */
	TArray<FMantleParseError> Errors;

	/** Cell files read (the index not counted), their packed bytes, and how many of those came over the network. */
	int32 Files = 0;
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

/** The result is not const: the receiver may move the cells out of it (FMantlePlaceCells::Merge does), as the loader keeps nothing of them. */
DECLARE_DELEGATE_OneParam(FMantleCellsDone, FMantleCellLoadResult&);

/** The cell index has been read (stream mode); the loader keeps it, GetIndex() has it. */
DECLARE_DELEGATE(FMantleCellIndexReady);

/**
 * Loads a place's cells: the cell index its "cells.index" names, then every cell file the index lists. Stage 1 of large places has one
 * rule, "every cell wanted", so the loader reads them all; the result (FMantleCellContent per cell) is what FMantlePlaceCells::Merge puts into the
 * place before its assets are prepared and it is built. Later stages choose which cells to want by distance.
 *
 *   resolve   Ids to {kind, url, cookedHash, cookedBytes} in batches (IMantleAssetResolver, the place assets' own): the index first, then every file.
 *   fetch     Through FMantleAssetStore: verified against the cooked BLAKE3, coalesced, cached across joins.
 *   read      On background tasks: the container (size checked before anything is allocated), the UTF-8 JSON, the entities
 *             (FMantlePlaceCells), so the game thread never reads a document.
 *
 * Any problem fails the whole load (a place with a cell missing is not the place that was published) with the cell and file named.
 * Game thread, like the store it drives. Log marker: MANTLE_PLACE_CELLS.
 */
class MANTLERUNTIME_API FMantleCellLoader : public TSharedFromThis<FMantleCellLoader>
{
public:
	UE_NONCOPYABLE(FMantleCellLoader);

	/** True when Place names cells that have not been added to it yet. */
	static bool NeedsCells(const FMantlePlace& Place)
	{
		return !Place.CellIndexId.IsEmpty() && !Place.bCellsMerged && !Place.bCellsStreamed;
	}

	/** Starts loading the cells of Root. OnDone fires once on the game thread, from a later Pump, never from inside Load. Keep the result until then. */
	static TSharedRef<FMantleCellLoader> Load(const FMantlePlace& Root, const FMantleCellLoaderSettings& Settings, FMantleCellsDone OnDone);

	/**
	 * Stream mode (stage 2): reads the cell index only, then cell files only for the cells SetWanted names, one cell at a time, and hands each cell's
	 * entities over with TakeCell. Nothing is read twice while a cell stays wanted, and nothing is kept for a cell nobody wants. The loader never
	 * finishes by itself: dropping it stops everything in flight. KnownIndex (when the caller has already read it, as the join's asset pass did) saves
	 * fetching and reading it again. OnIndex fires once from a Pump (or from this call's first Pump when KnownIndex is given), never from inside Open.
	 * A problem with one cell file fails that cell only (MANTLE_PLACE_CELLS FAIL, IsCellFailed); a problem with the index fails the loader (HasFailed).
	 */
	static TSharedRef<FMantleCellLoader> Open(const FMantlePlace& Root, const FMantleCellLoaderSettings& Settings, FMantleCellIndexReady OnIndex, const FMantleCellIndex* KnownIndex = nullptr);

	/** Stream mode: the cells to read, most urgent first. Cells not named are dropped (a download in flight is cancelled, a cell read but not taken is freed). */
	void SetWanted(TConstArrayView<int32> CellsMostUrgentFirst);

	/** Stream mode: moves a finished cell's entities out (in file order). False while any of its files is still on its way, or when it failed. The cell stops being wanted. */
	bool TakeCell(int32 CellIndex, FMantleCellContent& OutContent);

	/** Stream mode: this cell's files are all read and waiting to be taken. */
	bool IsCellReady(int32 CellIndex) const;

	/** Stream mode: this cell can never be read (a file was rejected, missing, or unreadable); why is in Describe lines of the log and FailureOf. */
	bool IsCellFailed(int32 CellIndex) const { return CellStreams.IsValidIndex(CellIndex) && CellStreams[CellIndex].bFailed; }

	/** Stream mode: the cell index, once OnIndex has fired. */
	const FMantleCellIndex& GetIndex() const { return Index; }
	bool IsIndexReady() const { return bIndexRead; }

	/** Stream mode: the index failed to load or be read. */
	bool HasFailed() const { return bFailed; }
	const FMantleCellLoadResult& GetStreamResult() const { return Result; }

	/** Stream mode: cell files fetched and read so far, and how many of their bytes came over the network. */
	int32 GetFilesReadTotal() const { return FilesReadTotal; }
	/** Requests to the store not answered yet (a cancelled one is off the count at once). */
	int32 GetFetchesInFlight() const { return FetchesInFlight; }

	~FMantleCellLoader();

	/** Delivers finished work, starts resolves, downloads and reads. The core ticker calls it when bAutoPump is set. */
	void Pump();

	/** Pumps the store and this loader until OnDone has fired or the time runs out. For tools and tests, never a running game. */
	bool WaitUntilComplete(float TimeoutSeconds);

	/** OnDone has fired. */
	bool IsComplete() const { return bFinished; }

	/** Valid once complete. */
	const FMantleCellLoadResult& GetResult() const { return Result; }

	/** Cell files (not the index) read so far and in all; the total is known once the index is read. */
	int32 GetFilesRead() const { return FilesReady; }
	int32 GetFilesTotal() const { return FMath::Max(Files.Num() - 1, 0); }

private:
	struct FDecoded;

	using FCompletion = TFunction<void(FMantleCellLoader&)>;

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

	enum class EFileState : uint8 { Waiting, Resolving, Resolved, Fetching, Fetched, Decoding, Ready, Failed };

	/** Stream mode, per cell: whether it is wanted, where its files are in Files, and whether it can never be read. */
	struct FCellStream
	{
		int32 FirstFile = 0;
		int32 FileCount = 0;
		bool bWanted = false;
		bool bFailed = false;
	};

	/** One asset the load needs: the index (CellIndex INDEX_NONE) or a cell file. */
	struct FFile
	{
		FString Id;
		int32 CellIndex = INDEX_NONE;
		int64 ExpectedBytes = 0;
		EFileState State = EFileState::Waiting;
		int32 ResolveAttempts = 0;
		double NextResolveAt = 0.0;
		/** When the URL was resolved: a signed URL does not last, so a file waiting a long time is resolved again. */
		double ResolvedAt = 0.0;
		bool bReresolved = false;
		FMantleStoredAssetRef Ref;
		uint64 RequestId = 0;
		/** Counts this file's fetches: an answer that carries an older number is for a request that was cancelled (the file was dropped, maybe asked for again) and is ignored. */
		uint32 FetchSerial = 0;
		TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Bytes;
		int64 HeldBytes = 0;
		FMantleCellContent Content;

		bool IsIndex() const { return CellIndex == INDEX_NONE; }
	};

	FMantleCellLoader(const FMantleCellLoaderSettings& InSettings, FMantleCellsDone InOnDone);

	void Start(const FMantlePlace& Root);
	void DrainCompletions();
	void FlushResolves();
	void OnResolved(const TArray<FString>& Ids, const FMantleAssetResolveResult& Resolved);
	void ApplyResolved(FFile& File, const FMantleUploadedAsset& Asset);
	void StartFetches();
	void OnFetched(int32 FileIndex, uint32 FetchSerial, const FMantleAssetResult& Fetched);
	void StartDecodes();
	void OnDecoded(int32 FileIndex, FDecoded& Decoded);
	void OnIndexRead(FMantleCellIndex&& Index);
	void CheckComplete();
	void Finish();

	/** Stream mode. */
	void BuildCellStreams();
	void RebuildFileOrder();
	void DropFile(FFile& File);
	void FailCell(int32 CellIndex, const FFile& File, const FString& Message);

	/** Fails the whole load with what went wrong (once; the first problem is the one reported). */
	void Fail(const FString& Path, const FString& Message);
	void FailFile(const FFile& File, const FString& Message);
	FString Describe(const FFile& File) const;

	const FMantleCellLoaderSettings Settings;
	const TSharedRef<FMantleAssetStore> Store;
	const TSharedRef<IMantleAssetResolver> Resolver;
	const TSharedRef<FCompletionQueue, ESPMode::ThreadSafe> Completions;
	FMantleCellsDone OnDone;

	/** The root's asset ids by kind, shared with the read tasks. */
	TSharedPtr<const TMap<FString, EMantleAssetKind>, ESPMode::ThreadSafe> RootAssets;

	/** [0] is the index; the cell files follow once it is read, grouped by cell in the index's order. */
	TArray<FFile> Files;
	TMap<FString, int32> FileByIdMap;
	TArray<int32> DecodeQueue;

	/** The order files are resolved, fetched and read in: all of them in index order (every cell wanted), or in stream mode the index first, then the wanted cells' files. */
	TArray<int32> FileOrder;
	TArray<FCellStream> CellStreams;
	/** Stream mode: the cells SetWanted last named that are still wanted, most urgent first. */
	TArray<int32> WantedCells;
	FMantleCellIndexReady OnIndexReady;
	bool bStream = false;
	bool bIndexReadyPending = false;
	int32 FilesReadTotal = 0;

	/** A resolved URL older than this is resolved again before it is used (signed URLs do not last). */
	static constexpr double UrlLifetimeSeconds = 600.0;

	FMantleCellIndex Index;
	FMantleCellLoadResult Result;

	int32 ResolvesInFlight = 0;
	int32 FetchesInFlight = 0;
	int32 DecodesInFlight = 0;
	int32 FilesReady = 0;
	int64 PendingBytes = 0;

	bool bStarted = false;
	bool bIndexRead = false;
	bool bFailed = false;
	bool bComplete = false;
	bool bFinished = false;

	double StartSeconds = 0.0;
	FTSTicker::FDelegateHandle TickerHandle;
};
