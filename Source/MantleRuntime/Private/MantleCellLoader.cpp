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
		CheckComplete();
	}
	if (bFailed || bComplete)
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
		for (const FFile& File : Files)
		{
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
	File.State = EFileState::Resolved;
}

// ---- Fetch --------------------------------------------------------------------------------------------

void FMantleCellLoader::StartFetches()
{
	for (int32 FileIndex = 0; FileIndex < Files.Num() && FetchesInFlight < Settings.MaxOutstandingFetches && PendingBytes < Settings.MaxPendingBytes; ++FileIndex)
	{
		FFile& File = Files[FileIndex];
		if (File.State != EFileState::Resolved)
		{
			continue;
		}
		File.State = EFileState::Fetching;
		++FetchesInFlight;
		const TSharedRef<FCompletionQueue, ESPMode::ThreadSafe> Queue = Completions;
		File.RequestId = Store->Request(File.Ref, EMantleAssetLoad::Bytes, FMantleAssetDone::CreateLambda(
			[Queue, FileIndex](const FMantleAssetResult& Fetched)
			{
				Queue->Push([FileIndex, Fetched](FMantleCellLoader& Self) { Self.OnFetched(FileIndex, Fetched); });
			}));
	}
}

void FMantleCellLoader::OnFetched(const int32 FileIndex, const FMantleAssetResult& Fetched)
{
	--FetchesInFlight;
	FFile& File = Files[FileIndex];
	if (File.State != EFileState::Fetching)
	{
		return;
	}
	File.RequestId = 0;
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
	File.State = EFileState::Ready;
	if (File.IsIndex())
	{
		OnIndexRead(MoveTemp(Decoded.Index));
		return;
	}
	File.Content = MoveTemp(Decoded.Content);
	++FilesReady;
}

void FMantleCellLoader::OnIndexRead(FMantleCellIndex&& InIndex)
{
	Index = MoveTemp(InIndex);
	bIndexRead = true;
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
