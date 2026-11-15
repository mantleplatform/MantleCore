// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCellLoader.h"

#include "MantleApiClient.h"
#include "MantleRuntime.h"

#include "Async/TaskGraphInterfaces.h"
#include "HAL/PlatformProcess.h"
#include "Tasks/Task.h"

/** What a read task hands back: the index or one file's content, or why it could not be read. */
struct FMantleCellLoader::FDecoded
{
	bool bSuccess = false;
	FString Error;
	FMantleCellIndex Index;
	FMantleCellContent Content;
	TArray<FMantleParseError> Errors;
};

FMantleCellLoaderSettings FMantleCellLoaderSettings::FromAssetSettings(const FMantlePlaceAssetSettings& Assets)
{
	FMantleCellLoaderSettings Settings;
	Settings.Store = Assets.Store;
	Settings.Resolver = Assets.Resolver;
	Settings.MaxResolveAttempts = Assets.MaxResolveAttempts;
	Settings.MaxConcurrentResolves = Assets.MaxConcurrentResolves;
	Settings.bAutoPump = Assets.bAutoPump;
	return Settings;
}

// ---- Lifetime -----------------------------------------------------------------------------------------

FMantleCellLoader::FMantleCellLoader(const FMantleCellLoaderSettings& InSettings, FMantleCellsDone InOnDone)
	: Settings(InSettings)
	, Store(InSettings.Store.IsValid() ? InSettings.Store.ToSharedRef() : FMantleAssetStore::Get().AsShared())
	, Resolver(InSettings.Resolver.IsValid() ? InSettings.Resolver.ToSharedRef() : FMantlePlaceAssets::CreateApiResolver(IsRunningDedicatedServer()))
	, Completions(MakeShared<FCompletionQueue, ESPMode::ThreadSafe>())
	, OnDone(MoveTemp(InOnDone))
{
}

FMantleCellLoader::~FMantleCellLoader()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	}
	for (FFile& File : Files)
	{
		if (File.RequestId != 0)
		{
			Store->Cancel(File.RequestId);
			File.RequestId = 0;
		}
	}
}

TSharedRef<FMantleCellLoader> FMantleCellLoader::Load(const FMantlePlace& Root, const FMantleCellLoaderSettings& Settings, FMantleCellsDone OnDone)
{
	check(IsInGameThread());
	TSharedRef<FMantleCellLoader> Loader = MakeShareable(new FMantleCellLoader(Settings, MoveTemp(OnDone)));
	Loader->Start(Root);
	if (Loader->Settings.bAutoPump)
	{
		const TWeakPtr<FMantleCellLoader> Weak = Loader;
		Loader->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak](float)
		{
			if (const TSharedPtr<FMantleCellLoader> Pinned = Weak.Pin())
			{
				Pinned->Pump();
				return !Pinned->bFinished;
			}
			return false;
		}));
	}
	return Loader;
}

TSharedRef<FMantleCellLoader> FMantleCellLoader::Open(const FMantlePlace& Root, const FMantleCellLoaderSettings& Settings, FMantleCellIndexReady OnIndex, const FMantleCellIndex* KnownIndex)
{
	check(IsInGameThread());
	TSharedRef<FMantleCellLoader> Loader = MakeShareable(new FMantleCellLoader(Settings, FMantleCellsDone()));
	Loader->bStream = true;
	Loader->OnIndexReady = MoveTemp(OnIndex);
	Loader->Start(Root);
	if (KnownIndex && !Loader->bFailed)
	{
		// As good as read: the index file is not fetched, and its cells' files are known at once.
		Loader->Files[0].State = EFileState::Ready;
		Loader->OnIndexRead(FMantleCellIndex(*KnownIndex));
	}
	if (Loader->Settings.bAutoPump)
	{
		const TWeakPtr<FMantleCellLoader> Weak = Loader;
		Loader->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak](float)
		{
			if (const TSharedPtr<FMantleCellLoader> Pinned = Weak.Pin())
			{
				Pinned->Pump();
				return !Pinned->bFinished;
			}
			return false;
		}));
	}
	return Loader;
}

void FMantleCellLoader::Start(const FMantlePlace& Root)
{
	StartSeconds = FPlatformTime::Seconds();
	bStarted = true;

	const FMantleAssetRef* Entry = Root.Assets.FindByPredicate([&Root](const FMantleAssetRef& Asset)
	{
		return Asset.Kind == EMantleAssetKind::PlaceCellIndex && Asset.Id == Root.CellIndexId;
	});
	if (Root.CellIndexId.IsEmpty() || !Entry)
	{
		Fail(TEXT("/cells/index"), TEXT("the place does not list its cell index among its assets"));
		return;
	}
	RootAssets = MakeShared<TMap<FString, EMantleAssetKind>, ESPMode::ThreadSafe>(FMantlePlaceCells::RootAssetKinds(Root));

	FFile& IndexFile = Files.AddDefaulted_GetRef();
	IndexFile.Id = Root.CellIndexId;
	IndexFile.ExpectedBytes = Entry->Bytes;
	FileByIdMap.Add(IndexFile.Id, 0);
	FileOrder.Add(0);
}

// ---- Pump ---------------------------------------------------------------------------------------------

void FMantleCellLoader::DrainCompletions()
{
	TArray<FCompletion> Items;
	{
		FScopeLock Guard(&Completions->Lock);
		Items = MoveTemp(Completions->Items);
		Completions->Items.Reset();
	}
	for (FCompletion& Item : Items)
	{
		Item(*this);
	}
}

void FMantleCellLoader::Pump()
{
	check(IsInGameThread());
	if (!bStarted || bFinished)
	{
		return;
	}
	const TSharedRef<FMantleCellLoader> KeepAlive = AsShared();
	DrainCompletions();
	if (!bFailed && !bComplete)
	{
		FlushResolves();
		StartFetches();
		StartDecodes();
		if (!bStream)
		{
			CheckComplete();
		}
	}
	if (bStream && bIndexReadyPending && !bFailed)
	{
		bIndexReadyPending = false;
		OnIndexReady.ExecuteIfBound();
	}
	// A stream never completes: it ends when it is dropped or its index cannot be read.
	if (bFailed || (!bStream && bComplete))
	{
		Finish();
	}
}

bool FMantleCellLoader::WaitUntilComplete(const float TimeoutSeconds)
{
	const TSharedRef<FMantleCellLoader> KeepAlive = AsShared();
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	while (!bFinished)
	{
		if (FPlatformTime::Seconds() > Deadline)
		{
			return false;
		}
		Store->Pump();
		Pump();
		FPlatformProcess::Sleep(0.001f);
	}
	return true;
}

void FMantleCellLoader::Finish()
{
	bFinished = true;
	Result.bSuccess = bComplete && !bFailed;
	Result.Seconds = FPlatformTime::Seconds() - StartSeconds;
	if (!Result.bSuccess)
	{
		Result.Cells.Reset();
		// A failed load stops everything still in flight.
		for (FFile& File : Files)
		{
			if (File.RequestId != 0)
			{
				Store->Cancel(File.RequestId);
				File.RequestId = 0;
			}
		}
		UE_LOG(LogMantle, Error, TEXT("MANTLE_PLACE_CELLS FAIL after %.2f s:\n%s"), Result.Seconds, *Result.ErrorSummary());
	}
	else
	{
		int64 Entities = 0;
		int64 Instances = 0;
		for (const FMantleCellContent& Cell : Result.Cells)
		{
			Entities += Cell.Entities.Num();
			Instances += Cell.Instances;
		}
		UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_CELLS READY: %d cells, %d files (%.1f MB packed, %.1f MB downloaded), %lld entities, %lld instances, %.2f s"),
			Result.Index.Cells.Num(), Result.Files, Result.Bytes / (1024.0 * 1024.0), Result.DownloadedBytes / (1024.0 * 1024.0), Entities, Instances, Result.Seconds);
	}
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	const TSharedRef<FMantleCellLoader> KeepAlive = AsShared();
	OnDone.ExecuteIfBound(Result);
}

void FMantleCellLoader::Fail(const FString& Path, const FString& Message)
{
	if (!bFailed)
	{
		Result.Errors.Add(FMantleParseError{ Path, Message });
	}
	bFailed = true;
}

FString FMantleCellLoader::Describe(const FFile& File) const
{
	if (File.IsIndex())
	{
		return FString::Printf(TEXT("cell index %s"), *File.Id);
	}
	return FString::Printf(TEXT("cell %s file %s"), *Index.Cells[File.CellIndex].KeyString(), *File.Id);
}

void FMantleCellLoader::FailFile(const FFile& File, const FString& Message)
{
	if (bStream && !File.IsIndex())
	{
		// One cell file that cannot be had loses its cell, not the place: the streamer builds the rest and the hold does not wait for it.
		FailCell(File.CellIndex, File, Message);
		return;
	}
	Fail(File.IsIndex() ? FString(TEXT("/cells/index")) : FString::Printf(TEXT("cell %s"), *Index.Cells[File.CellIndex].KeyString()),
		FString::Printf(TEXT("%s %s"), *Describe(File), *Message));
}

// ---- Resolve ------------------------------------------------------------------------------------------

void FMantleCellLoader::FlushResolves()
{
	const double Now = FPlatformTime::Seconds();
	while (ResolvesInFlight < Settings.MaxConcurrentResolves)
	{
		TArray<FString> Ids;
		for (const int32 FileIndex : FileOrder)
		{
			const FFile& File = Files[FileIndex];
			if (File.State != EFileState::Waiting || File.NextResolveAt > Now)
			{
				continue;
			}
			Ids.Add(File.Id);
			if (Ids.Num() >= FMantleApiClient::MaxIdsPerResolve)
			{
				break;
			}
		}
		if (Ids.IsEmpty())
		{
			return;
		}
		for (const FString& Id : Ids)
		{
			FFile& File = Files[FileByIdMap.FindChecked(Id)];
			File.State = EFileState::Resolving;
			++File.ResolveAttempts;
		}
		++ResolvesInFlight;
		const TSharedRef<FCompletionQueue, ESPMode::ThreadSafe> Queue = Completions;
		Resolver->Resolve(Ids, [Queue, Ids](const FMantleAssetResolveResult& Resolved)
		{
			Queue->Push([Ids, Resolved](FMantleCellLoader& Self) { Self.OnResolved(Ids, Resolved); });
		});
	}
}

void FMantleCellLoader::OnResolved(const TArray<FString>& Ids, const FMantleAssetResolveResult& Resolved)
{
	--ResolvesInFlight;
	const double Now = FPlatformTime::Seconds();

	TMap<FString, const FMantleUploadedAsset*> ById;
	for (const FMantleUploadedAsset& Asset : Resolved.Assets)
	{
		ById.Add(Asset.Id, &Asset);
	}
	const TSet<FString> Missing(Resolved.Missing);

	for (const FString& Id : Ids)
	{
		const int32* Slot = FileByIdMap.Find(Id);
		if (!Slot || Files[*Slot].State != EFileState::Resolving)
		{
			continue;
		}
		FFile& File = Files[*Slot];
		if (const FMantleUploadedAsset* const* Asset = ById.Find(Id))
		{
			ApplyResolved(File, **Asset);
		}
		else if (Missing.Contains(Id))
		{
			FailFile(File, TEXT("does not exist (it may have been taken down)"));
		}
		else
		{
			FString Why = !Resolved.bSuccess ? Resolved.Error : Resolved.Failed.FindRef(Id);
			if (Why.IsEmpty())
			{
				Why = TEXT("the API did not answer for it");
			}
			if (File.ResolveAttempts >= Settings.MaxResolveAttempts)
			{
				FailFile(File, FString::Printf(TEXT("could not be resolved: %s"), *Why));
			}
			else
			{
				File.State = EFileState::Waiting;
				File.NextResolveAt = Now + static_cast<double>(File.ResolveAttempts);
				UE_LOG(LogMantle, Verbose, TEXT("MANTLE_PLACE_CELLS retrying %s: %s"), *Describe(File), *Why);
			}
		}
	}
}

void FMantleCellLoader::ApplyResolved(FFile& File, const FMantleUploadedAsset& Asset)
{
	const TCHAR* Expected = File.IsIndex() ? TEXT("cellindex") : TEXT("cell");
	if (!Asset.Kind.Equals(Expected, ESearchCase::CaseSensitive))
	{
		FailFile(File, FString::Printf(TEXT("is a %s, not a %s"), *Asset.Kind, Expected));
		return;
	}
	if (Asset.IsRejected())
	{
		FailFile(File, Asset.CookError.IsEmpty() ? FString(TEXT("was rejected")) : FString::Printf(TEXT("was rejected: %s"), *Asset.CookError));
		return;
	}
	if (!Asset.IsApproved())
	{
		FailFile(File, FString::Printf(TEXT("is not approved (status %s)"), *Asset.Status));
		return;
	}
	if (Asset.Url.IsEmpty())
	{
		FailFile(File, TEXT("has no download URL"));
		return;
	}
	FMantleContentHash Hash;
	FString HashError;
	if (!FMantleContentHash::Parse(Asset.CookedHash, Hash, &HashError))
	{
		FailFile(File, FString::Printf(TEXT("has no cooked hash to verify its download against (%s)"), *HashError));
		return;
	}
	File.Ref.Hash = Hash;
	File.Ref.Url = Asset.Url;
	File.Ref.Bytes = FMath::Max<int64>(Asset.CookedBytes, 0);
	File.ResolvedAt = FPlatformTime::Seconds();
	File.State = EFileState::Resolved;
}

// ---- Fetch --------------------------------------------------------------------------------------------

void FMantleCellLoader::StartFetches()
{
	const double Now = FPlatformTime::Seconds();
	for (int32 OrderIndex = 0; OrderIndex < FileOrder.Num() && FetchesInFlight < Settings.MaxOutstandingFetches && PendingBytes < Settings.MaxPendingBytes; ++OrderIndex)
	{
		const int32 FileIndex = FileOrder[OrderIndex];
		FFile& File = Files[FileIndex];
		if (File.State != EFileState::Resolved)
		{
			continue;
		}
		// A signed URL does not last: a file that waited a long while for its turn is resolved again first.
		if (bStream && Now - File.ResolvedAt > UrlLifetimeSeconds)
		{
			File.State = EFileState::Waiting;
			File.NextResolveAt = 0.0;
			continue;
		}
		File.State = EFileState::Fetching;
		++FetchesInFlight;
		const uint32 Serial = ++File.FetchSerial;
		const TSharedRef<FCompletionQueue, ESPMode::ThreadSafe> Queue = Completions;
		File.RequestId = Store->Request(File.Ref, EMantleAssetLoad::Bytes, FMantleAssetDone::CreateLambda(
			[Queue, FileIndex, Serial](const FMantleAssetResult& Fetched)
			{
				Queue->Push([FileIndex, Serial, Fetched](FMantleCellLoader& Self) { Self.OnFetched(FileIndex, Serial, Fetched); });
			}));
	}
}

void FMantleCellLoader::OnFetched(const int32 FileIndex, const uint32 FetchSerial, const FMantleAssetResult& Fetched)
{
	FFile& File = Files[FileIndex];
	if (File.State != EFileState::Fetching || File.FetchSerial != FetchSerial)
	{
		// The request was cancelled by DropFile, which took it off the count already (and the file may be fetching again, under a newer serial): this answer is not for it.
		return;
	}
	--FetchesInFlight;
	File.RequestId = 0;
	// Off the count and no longer fetching: a failure below drops the cell's files (DropFile), and must not take this one off the count again.
	File.State = EFileState::Resolved;
	if (!Fetched.bSuccess)
	{
		// A 403 or 404 from the CDN usually means the signed URL expired: resolve once more for a fresh one.
		if ((Fetched.StatusCode == 403 || Fetched.StatusCode == 404) && !File.bReresolved)
		{
			File.bReresolved = true;
			File.State = EFileState::Waiting;
			File.NextResolveAt = 0.0;
			UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_CELLS %s answered %d; resolving it again for a fresh URL"), *Describe(File), Fetched.StatusCode);
			return;
		}
		FailFile(File, FString::Printf(TEXT("could not be downloaded: %s"), *Fetched.Error));
		return;
	}
	if (!Fetched.bFromCache)
	{
		Result.DownloadedBytes += Fetched.SizeBytes;
	}
	if (!File.IsIndex())
	{
		Result.Bytes += Fetched.SizeBytes;
	}
	if (File.ExpectedBytes > 0 && File.ExpectedBytes != Fetched.SizeBytes)
	{
		// The index's bytes are what the plugin's upload returned for this file; the store has already verified the download against the cooked hash.
		UE_LOG(LogMantle, Warning, TEXT("MANTLE_PLACE_CELLS %s is %lld bytes; the place says %lld"), *Describe(File), Fetched.SizeBytes, File.ExpectedBytes);
	}
	File.Bytes = Fetched.Bytes;
	File.HeldBytes = Fetched.SizeBytes;
	PendingBytes += File.HeldBytes;
	File.State = EFileState::Fetched;
	DecodeQueue.Add(FileIndex);
}

// ---- Read ---------------------------------------------------------------------------------------------

void FMantleCellLoader::StartDecodes()
{
	while (!DecodeQueue.IsEmpty() && DecodesInFlight < Settings.MaxConcurrentDecodes)
	{
		const int32 FileIndex = DecodeQueue[0];
		DecodeQueue.RemoveAt(0);
		FFile& File = Files[FileIndex];
		if (File.State != EFileState::Fetched)
		{
			continue;
		}
		File.State = EFileState::Decoding;
		++DecodesInFlight;

		const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Bytes = MoveTemp(File.Bytes);
		File.Bytes.Reset();
		const bool bIsIndex = File.IsIndex();
		const TSharedPtr<const TMap<FString, EMantleAssetKind>, ESPMode::ThreadSafe> Assets = RootAssets;
		const TSharedRef<FCompletionQueue, ESPMode::ThreadSafe> Queue = Completions;
		UE::Tasks::Launch(TEXT("MantleCellRead"), [Queue, Bytes, FileIndex, bIsIndex, Assets]()
		{
			const TSharedRef<FDecoded, ESPMode::ThreadSafe> Decoded = MakeShared<FDecoded, ESPMode::ThreadSafe>();
			TArray<uint8> Raw;
			FString Text;
			FString Reason;
			const TConstArrayView<uint8> View = Bytes.IsValid() ? TConstArrayView<uint8>(*Bytes) : TConstArrayView<uint8>();
			if (!FMantlePlaceCells::Unpack(View, bIsIndex ? FMantlePlaceCells::IndexMagic : FMantlePlaceCells::CellMagic,
					bIsIndex ? FMantlePlaceCells::MaxIndexInflatedBytes : FMantlePlaceCells::MaxCellInflatedBytes, Raw, &Reason)
				|| !FMantlePlaceCells::ToText(Raw, Text, &Reason))
			{
				Decoded->Error = Reason;
			}
			else
			{
				// Raw is the inflated JSON, up to 64 MiB; it is not needed once it is text.
				Raw.Empty();
				if (bIsIndex)
				{
					FString IndexError;
					Decoded->bSuccess = FMantlePlaceCells::ReadIndex(Text, Decoded->Index, IndexError);
					Decoded->Error = IndexError;
				}
				else
				{
					Decoded->bSuccess = FMantlePlaceCells::ReadCellFile(Text, *Assets, Decoded->Content, Decoded->Errors);
					if (!Decoded->bSuccess)
					{
						TArray<FString> Lines;
						for (const FMantleParseError& Error : Decoded->Errors)
						{
							Lines.Add(Error.ToString());
						}
						Decoded->Error = FString::Join(Lines, TEXT("; "));
					}
				}
			}
			Queue->Push([Decoded, FileIndex](FMantleCellLoader& Self) { Self.OnDecoded(FileIndex, *Decoded); });
		});
	}
}

void FMantleCellLoader::OnDecoded(const int32 FileIndex, FDecoded& Decoded)
{
	--DecodesInFlight;
	FFile& File = Files[FileIndex];
	PendingBytes -= File.HeldBytes;
	File.HeldBytes = 0;
	if (File.State != EFileState::Decoding)
	{
		return;
	}
	if (!Decoded.bSuccess)
	{
		FailFile(File, FString::Printf(TEXT("could not be read: %s"), *Decoded.Error));
		return;
	}
	if (bStream && !File.IsIndex() && !CellStreams[File.CellIndex].bWanted)
	{
		// Nobody wants this cell any more (it was dropped while its file was being read): the read is thrown away, the URL is still good.
		File.State = EFileState::Resolved;
		return;
	}
	File.State = EFileState::Ready;
	if (File.IsIndex())
	{
		OnIndexRead(MoveTemp(Decoded.Index));
		return;
	}
	File.Content = MoveTemp(Decoded.Content);
	++FilesReady;
	++FilesReadTotal;
}

void FMantleCellLoader::OnIndexRead(FMantleCellIndex&& InIndex)
{
	Index = MoveTemp(InIndex);
	bIndexRead = true;
	bIndexReadyPending = bStream;
	int32 FileCount = 0;
	for (const FMantleCellInfo& Cell : Index.Cells)
	{
		FileCount += Cell.Files.Num();
	}
	Files.Reserve(1 + FileCount);
	for (int32 CellIndex = 0; CellIndex < Index.Cells.Num(); ++CellIndex)
	{
		for (const FMantleCellFileRef& Ref : Index.Cells[CellIndex].Files)
		{
			// The reader holds a file to one cell; the index's own id cannot also be a cell file.
			if (FileByIdMap.Contains(Ref.Id))
			{
				Fail(FString::Printf(TEXT("cell %s"), *Index.Cells[CellIndex].KeyString()), FString::Printf(TEXT("lists %s, which is the cell index itself"), *Ref.Id));
				return;
			}
			FileByIdMap.Add(Ref.Id, Files.Num());
			FFile& File = Files.AddDefaulted_GetRef();
			File.Id = Ref.Id;
			File.CellIndex = CellIndex;
			File.ExpectedBytes = Ref.Bytes;
		}
	}
	if (bStream)
	{
		// Files are read only for the cells SetWanted names.
		BuildCellStreams();
		FileOrder.Reset();
		UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_CELLS index (streamed): %d cells in %d level(s), %d files, read when their cell is wanted"), Index.Cells.Num(), Index.Levels.Num(), FileCount);
		return;
	}
	FileOrder.Reset();
	for (int32 FileIndex = 0; FileIndex < Files.Num(); ++FileIndex)
	{
		FileOrder.Add(FileIndex);
	}
	UE_LOG(LogMantle, Log, TEXT("MANTLE_PLACE_CELLS index: %d cells in %d level(s), %d files to fetch"), Index.Cells.Num(), Index.Levels.Num(), FileCount);
}

void FMantleCellLoader::CheckComplete()
{
	if (bComplete || bFailed || !bIndexRead || FilesReady != Files.Num() - 1)
	{
		return;
	}
	Result.Index = MoveTemp(Index);
	Result.Files = FilesReady;
	Result.Cells.Reset();
	Result.Cells.SetNum(Result.Index.Cells.Num());
	for (int32 CellIndex = 0; CellIndex < Result.Cells.Num(); ++CellIndex)
	{
		Result.Cells[CellIndex].CellIndex = CellIndex;
	}
	// The files are in the index's order, so a cell's files are consecutive and its entities keep file order.
	for (int32 FileIndex = 1; FileIndex < Files.Num(); ++FileIndex)
	{
		FFile& File = Files[FileIndex];
		FMantleCellContent& Cell = Result.Cells[File.CellIndex];
		Cell.Entities.Append(MoveTemp(File.Content.Entities));
		Cell.Instances += File.Content.Instances;
		Cell.Solid += File.Content.Solid;
		File.Content = FMantleCellContent();
	}
	bComplete = true;
}

// ---- Stream mode --------------------------------------------------------------------------------------

void FMantleCellLoader::BuildCellStreams()
{
	CellStreams.Reset();
	CellStreams.SetNum(Index.Cells.Num());
	// The files follow the index file in the index's order, so a cell's files are consecutive.
	for (int32 FileIndex = 1; FileIndex < Files.Num(); ++FileIndex)
	{
		FCellStream& Stream = CellStreams[Files[FileIndex].CellIndex];
		if (Stream.FileCount == 0)
		{
			Stream.FirstFile = FileIndex;
		}
		++Stream.FileCount;
	}
}

void FMantleCellLoader::RebuildFileOrder()
{
	FileOrder.Reset();
	if (Files.IsEmpty())
	{
		return;
	}
	if (Files[0].State != EFileState::Ready)
	{
		FileOrder.Add(0);
	}
	for (const int32 CellIndex : WantedCells)
	{
		const FCellStream& Stream = CellStreams[CellIndex];
		for (int32 FileIndex = Stream.FirstFile; FileIndex < Stream.FirstFile + Stream.FileCount; ++FileIndex)
		{
			FileOrder.Add(FileIndex);
		}
	}
}

void FMantleCellLoader::DropFile(FFile& File)
{
	switch (File.State)
	{
	case EFileState::Fetching:
		// Cancelled before it can report: nothing answers for it, so the count is taken off here.
		if (File.RequestId != 0)
		{
			Store->Cancel(File.RequestId);
			File.RequestId = 0;
		}
		--FetchesInFlight;
		File.State = EFileState::Resolved;
		break;
	case EFileState::Fetched:
		DecodeQueue.RemoveSingle(static_cast<int32>(&File - Files.GetData()));
		PendingBytes -= File.HeldBytes;
		File.HeldBytes = 0;
		File.Bytes.Reset();
		File.State = EFileState::Resolved;
		break;
	case EFileState::Ready:
		File.Content = FMantleCellContent();
		--FilesReady;
		File.State = EFileState::Resolved;
		break;
	default:
		// Waiting and resolved files cost nothing; one being resolved or read finishes and is dealt with when it arrives (a read for a cell nobody
		// wants is thrown away by OnDecoded).
		break;
	}
}

void FMantleCellLoader::SetWanted(const TConstArrayView<int32> CellsMostUrgentFirst)
{
	check(bStream);
	if (!bIndexRead)
	{
		return;
	}
	TSet<int32> Now;
	Now.Reserve(CellsMostUrgentFirst.Num());
	for (const int32 CellIndex : CellsMostUrgentFirst)
	{
		if (CellStreams.IsValidIndex(CellIndex) && !CellStreams[CellIndex].bFailed)
		{
			Now.Add(CellIndex);
		}
	}
	for (const int32 CellIndex : WantedCells)
	{
		if (Now.Contains(CellIndex))
		{
			continue;
		}
		FCellStream& Stream = CellStreams[CellIndex];
		Stream.bWanted = false;
		for (int32 FileIndex = Stream.FirstFile; FileIndex < Stream.FirstFile + Stream.FileCount; ++FileIndex)
		{
			DropFile(Files[FileIndex]);
		}
	}
	WantedCells.Reset();
	for (const int32 CellIndex : CellsMostUrgentFirst)
	{
		if (Now.Contains(CellIndex))
		{
			CellStreams[CellIndex].bWanted = true;
			WantedCells.Add(CellIndex);
			Now.Remove(CellIndex);
		}
	}
	RebuildFileOrder();
}

bool FMantleCellLoader::IsCellReady(const int32 CellIndex) const
{
	if (!CellStreams.IsValidIndex(CellIndex) || CellStreams[CellIndex].bFailed)
	{
		return false;
	}
	const FCellStream& Stream = CellStreams[CellIndex];
	for (int32 FileIndex = Stream.FirstFile; FileIndex < Stream.FirstFile + Stream.FileCount; ++FileIndex)
	{
		if (Files[FileIndex].State != EFileState::Ready)
		{
			return false;
		}
	}
	return Stream.FileCount > 0;
}

bool FMantleCellLoader::TakeCell(const int32 CellIndex, FMantleCellContent& OutContent)
{
	check(bStream);
	if (!IsCellReady(CellIndex))
	{
		return false;
	}
	FCellStream& Stream = CellStreams[CellIndex];
	OutContent = FMantleCellContent();
	OutContent.CellIndex = CellIndex;
	for (int32 FileIndex = Stream.FirstFile; FileIndex < Stream.FirstFile + Stream.FileCount; ++FileIndex)
	{
		FFile& File = Files[FileIndex];
		OutContent.Entities.Append(MoveTemp(File.Content.Entities));
		OutContent.Instances += File.Content.Instances;
		OutContent.Solid += File.Content.Solid;
		File.Content = FMantleCellContent();
		File.State = EFileState::Resolved;
		--FilesReady;
	}
	Stream.bWanted = false;
	WantedCells.RemoveSingle(CellIndex);
	RebuildFileOrder();
	return true;
}

void FMantleCellLoader::FailCell(const int32 CellIndex, const FFile& Failing, const FString& Message)
{
	FCellStream& Stream = CellStreams[CellIndex];
	if (Stream.bFailed)
	{
		return;
	}
	const FString Line = FString::Printf(TEXT("%s %s"), *Describe(Failing), *Message);
	UE_LOG(LogMantle, Error, TEXT("MANTLE_PLACE_CELLS FAIL cell %s: %s"), *Index.Cells[CellIndex].KeyString(), *Line);
	Result.Errors.Add(FMantleParseError{ FString::Printf(TEXT("cell %s"), *Index.Cells[CellIndex].KeyString()), Line });
	Stream.bFailed = true;
	Stream.bWanted = false;
	WantedCells.RemoveSingle(CellIndex);
	for (int32 FileIndex = Stream.FirstFile; FileIndex < Stream.FirstFile + Stream.FileCount; ++FileIndex)
	{
		FFile& File = Files[FileIndex];
		DropFile(File);
		// A file still being resolved or read has an answer coming that will find it failed and do nothing.
		File.State = EFileState::Failed;
	}
	RebuildFileOrder();
}
