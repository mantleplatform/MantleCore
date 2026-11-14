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
		return !Place.CellIndexId.IsEmpty() && !Place.bCellsMerged;
	}

	/** Starts loading the cells of Root. OnDone fires once on the game thread, from a later Pump, never from inside Load. Keep the result until then. */
	static TSharedRef<FMantleCellLoader> Load(const FMantlePlace& Root, const FMantleCellLoaderSettings& Settings, FMantleCellsDone OnDone);

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

	enum class EFileState : uint8 { Waiting, Resolving, Resolved, Fetching, Fetched, Decoding, Ready };

	/** One asset the load needs: the index (CellIndex INDEX_NONE) or a cell file. */
	struct FFile
	{
		FString Id;
		int32 CellIndex = INDEX_NONE;
		int64 ExpectedBytes = 0;
		EFileState State = EFileState::Waiting;
		int32 ResolveAttempts = 0;
		double NextResolveAt = 0.0;
		bool bReresolved = false;
		FMantleStoredAssetRef Ref;
		uint64 RequestId = 0;
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
	void OnFetched(int32 FileIndex, const FMantleAssetResult& Fetched);
	void StartDecodes();
	void OnDecoded(int32 FileIndex, FDecoded& Decoded);
	void OnIndexRead(FMantleCellIndex&& Index);
	void CheckComplete();
	void Finish();

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
