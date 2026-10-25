// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAssetStore.h"

#include "MantlePreviewLaunch.h"
#include "MantleRuntime.h"

#include "Async/Async.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/Archive.h"

// Helpers carry a MantleAssetStore prefix: Unity builds put this file and its test in one
// translation unit, where two anonymous namespaces defining the same name collide.
namespace
{
	const TCHAR* const MantleAssetStoreHashPrefix = TEXT("blake3:");

	int32 MantleAssetStoreNibble(TCHAR Character)
	{
		if (Character >= TEXT('0') && Character <= TEXT('9'))
		{
			return Character - TEXT('0');
		}
		if (Character >= TEXT('a') && Character <= TEXT('f'))
		{
			return 10 + (Character - TEXT('a'));
		}
		return -1;
	}

	IPlatformFile& MantleAssetStoreFiles()
	{
		return FPlatformFileManager::Get().GetPlatformFile();
	}

	/**
	 * Hashes a file, optionally keeping its bytes. False when it cannot be opened or read, or is
	 * larger than MaxBytes. Reads in chunks when the bytes are not wanted, so verifying a 60 MB
	 * mesh for a path-only caller never holds it all in memory.
	 */
	bool MantleAssetStoreHashFile(const FString& Path, int64 MaxBytes, FBlake3Hash& OutHash, int64& OutSize,
		TArray<uint8>* OutBytes)
	{
		TUniquePtr<IFileHandle> Handle(MantleAssetStoreFiles().OpenRead(*Path));
		if (!Handle)
		{
			return false;
		}

		const int64 Size = Handle->Size();
		if (Size < 0 || Size > MaxBytes)
		{
			return false;
		}

		FBlake3 Hasher;
		if (OutBytes)
		{
			OutBytes->SetNumUninitialized(static_cast<int32>(Size));
			if (Size > 0 && !Handle->Read(OutBytes->GetData(), Size))
			{
				OutBytes->Reset();
				return false;
			}
			Hasher.Update(OutBytes->GetData(), static_cast<uint64>(Size));
		}
		else
		{
			TArray<uint8> Chunk;
			Chunk.SetNumUninitialized(static_cast<int32>(FMath::Min<int64>(FMath::Max<int64>(Size, 1), 1024 * 1024)));
			int64 Remaining = Size;
			while (Remaining > 0)
			{
				const int64 Step = FMath::Min<int64>(Remaining, Chunk.Num());
				if (!Handle->Read(Chunk.GetData(), Step))
				{
					return false;
				}
				Hasher.Update(Chunk.GetData(), static_cast<uint64>(Step));
				Remaining -= Step;
			}
		}

		OutHash = Hasher.Finalize();
		OutSize = Size;
		return true;
	}

	FMantleAssetStoreSettings MantleAssetStoreNormalize(FMantleAssetStoreSettings Settings)
	{
		if (Settings.RootDir.IsEmpty())
		{
			Settings.RootDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Mantle"), TEXT("Assets"));
		}
		Settings.RootDir = FPaths::ConvertRelativePathToFull(Settings.RootDir);
		FPaths::NormalizeDirectoryName(Settings.RootDir);

		Settings.DiskBudgetBytes = FMath::Max<int64>(Settings.DiskBudgetBytes, 0);
		Settings.EvictionLowWater = FMath::Clamp(Settings.EvictionLowWater, 0.0, 1.0);
		Settings.MaxConcurrentDownloads = FMath::Max(Settings.MaxConcurrentDownloads, 1);
		Settings.MaxAttempts = FMath::Max(Settings.MaxAttempts, 1);
		// TArray<uint8> indexes with int32; a larger asset could not be handed to anyone anyway.
		Settings.MaxAssetBytes = FMath::Clamp<int64>(Settings.MaxAssetBytes, 1, MAX_int32);
		return Settings;
	}

	bool MantleAssetStoreIsRetryable(const FMantleAssetFetchResult& Result)
	{
		if (!Result.bConnected)
		{
			return true;
		}
		const int32 Status = Result.StatusCode;
		return Status >= 500 || Status == 408 || Status == 429;
	}
}

// ---------------------------------------------------------------------------------------------
// FMantleContentHash

bool FMantleContentHash::Parse(const FString& Text, FMantleContentHash& OutHash, FString* OutError)
{
	OutHash = FMantleContentHash();

	auto Refuse = [OutError](const FString& Why)
	{
		if (OutError)
		{
			*OutError = Why;
		}
		return false;
	};

	if (!Text.StartsWith(MantleAssetStoreHashPrefix, ESearchCase::CaseSensitive))
	{
		int32 Colon = INDEX_NONE;
		if (Text.FindChar(TEXT(':'), Colon))
		{
			return Refuse(FString::Printf(
				TEXT("'%s' uses %s; the asset store verifies blake3 only."), *Text, *Text.Left(Colon)));
		}
		return Refuse(FString::Printf(TEXT("'%s' is not a content hash (blake3:<64 hex digits>)."), *Text));
	}

	const FString Hex = Text.Mid(FCString::Strlen(MantleAssetStoreHashPrefix));
	if (Hex.Len() != 64)
	{
		return Refuse(FString::Printf(TEXT("'%s' has %d hex digits; blake3 has 64."), *Text, Hex.Len()));
	}

	FBlake3Hash::ByteArray Bytes;
	for (int32 Index = 0; Index < 32; ++Index)
	{
		const int32 High = MantleAssetStoreNibble(Hex[Index * 2]);
		const int32 Low = MantleAssetStoreNibble(Hex[Index * 2 + 1]);
		if (High < 0 || Low < 0)
		{
			// Lowercase only, as the schema says: the digits become a file name, and a
			// case-insensitive file system would otherwise hold two spellings of one hash.
			return Refuse(FString::Printf(TEXT("'%s' is not lowercase hex."), *Text));
		}
		Bytes[Index] = static_cast<uint8>((High << 4) | Low);
	}

	OutHash.Digest = FBlake3Hash(Bytes);
	if (!OutHash.IsValid())
	{
		return Refuse(TEXT("The all-zero hash is not a content address."));
	}
	return true;
}

FMantleContentHash FMantleContentHash::OfBytes(const void* Data, uint64 Size)
{
	FMantleContentHash Hash;
	Hash.Digest = FBlake3::HashBuffer(Data, Size);
	return Hash;
}

FString FMantleContentHash::ToHex() const
{
	static const TCHAR* const Digits = TEXT("0123456789abcdef");
	FString Hex;
	Hex.Reserve(64);
	for (const uint8 Byte : Digest.GetBytes())
	{
		Hex.AppendChar(Digits[Byte >> 4]);
		Hex.AppendChar(Digits[Byte & 0x0F]);
	}
	return Hex;
}

FString FMantleContentHash::ToString() const
{
	return MantleAssetStoreHashPrefix + ToHex();
}

// ---------------------------------------------------------------------------------------------
// FMantleAssetSink: the partial file a download streams into, hashed as it goes.

class FMantleAssetSink final : public FArchive
{
public:
	FMantleAssetSink(const FString& InPath, int64 InMaxBytes)
		: Path(InPath)
		, MaxBytes(InMaxBytes)
	{
		SetIsSaving(true);
		SetIsPersistent(false);
	}

	virtual void Serialize(void* Data, int64 Num) override
	{
		if (Num <= 0)
		{
			return;
		}

		FScopeLock Guard(&Mutex);
		if (IsError())
		{
			return;
		}
		if (bClosed)
		{
			SetError();
			return;
		}
		if (Written + Num > MaxBytes)
		{
			// Stops the engine's HTTP layer too: it checks GetError() after every write.
			bTooLarge = true;
			SetError();
			return;
		}
		if (!EnsureOpenLocked() || !Handle->Write(static_cast<const uint8*>(Data), Num))
		{
			bWriteFailed = true;
			SetError();
			return;
		}

		Hasher.Update(Data, static_cast<uint64>(Num));
		Written += Num;
		BytesWritten.store(Written, std::memory_order_relaxed);
	}

	virtual FString GetArchiveName() const override
	{
		return TEXT("FMantleAssetSink");
	}

	/** Closes the file. False when anything could not be written. An empty body still leaves
	    an (empty) file, because an empty asset is a legitimate content address. */
	bool Finish(FBlake3Hash& OutHash, int64& OutSize)
	{
		FScopeLock Guard(&Mutex);
		if (!bClosed)
		{
			bClosed = true;
			if (!bTooLarge && !bWriteFailed && !EnsureOpenLocked())
			{
				bWriteFailed = true;
			}
			if (Handle && !Handle->Flush())
			{
				bWriteFailed = true;
			}
			Handle.Reset();
		}
		OutHash = Hasher.Finalize();
		OutSize = Written;
		return !bWriteFailed && !bTooLarge;
	}

	bool WasTooLarge() const
	{
		FScopeLock Guard(&Mutex);
		return bTooLarge;
	}

	int64 GetBytesWritten() const
	{
		return BytesWritten.load(std::memory_order_relaxed);
	}

	const FString& GetPath() const
	{
		return Path;
	}

	int64 GetMaxBytes() const
	{
		return MaxBytes;
	}

private:
	/** Opened on the first write, which is on the fetcher's thread, so the game thread that
	    starts a download never touches the disk. */
	bool EnsureOpenLocked()
	{
		if (Handle)
		{
			return true;
		}
		IPlatformFile& Files = MantleAssetStoreFiles();
		Handle.Reset(Files.OpenWrite(*Path));
		if (!Handle)
		{
			// Saved/ can be removed under a running process; make the folder once and retry.
			Files.CreateDirectoryTree(*FPaths::GetPath(Path));
			Handle.Reset(Files.OpenWrite(*Path));
		}
		return Handle.IsValid();
	}

	const FString Path;
	const int64 MaxBytes;

	mutable FCriticalSection Mutex;
	TUniquePtr<IFileHandle> Handle;
	FBlake3 Hasher;
	int64 Written = 0;
	std::atomic<int64> BytesWritten{0};
	bool bClosed = false;
	bool bTooLarge = false;
	bool bWriteFailed = false;
};

// ---------------------------------------------------------------------------------------------
// The HTTP fetcher

namespace
{
	class FMantleHttpAssetFetcher final : public IMantleAssetFetcher
	{
	public:
		virtual void Fetch(const FString& Url, const TSharedRef<FArchive>& Sink,
			TFunction<void(const FMantleAssetFetchResult&)> OnDone) override
		{
			if (MantlePreviewLaunch::IsRequested())
			{
				// Offline preview: no production requests. Counted, and answered off the caller's stack, as a failed download is.
				FMantlePreviewStatus::Get().NoteBlockedRequest(TEXT("asset download"));
				AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [OnDone]()
				{
					// A refusal the store reads as final: a "connected" 403, since a failed connection is retried.
					FMantleAssetFetchResult Result;
					Result.bConnected = true;
					Result.StatusCode = 403;
					Result.Error = TEXT("preview_offline");
					OnDone(Result);
				});
				return;
			}
			const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
			Request->SetURL(Url);
			Request->SetVerb(TEXT("GET"));
			// No Authorization header on purpose: these are CDN URLs, often on another host, and
			// the player's Mantle token must never travel to one.
			Request->SetHeader(TEXT("Accept"), TEXT("application/octet-stream"));
			// Large meshes on slow connections take a while; a stalled one is caught by the
			// activity timeout long before the overall one.
			Request->SetTimeout(600.0f);
			Request->SetActivityTimeout(30.0f);

			// Stream to disk, so N concurrent downloads never hold N assets in memory. An HTTP
			// implementation without streaming falls back to copying the body once at the end.
			const bool bStreams = Request->SetResponseBodyReceiveStream(Sink);

			// Completion work (hashing, renaming) goes to a background task anyway; there is no
			// reason to wait for the game thread's HTTP tick to hand it over.
			Request->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnHttpThread);

			// The store counts on exactly one OnDone. Some HTTP implementations also fire the
			// completion delegate when ProcessRequest itself fails, so both paths go through this.
			const TSharedRef<std::atomic<bool>> bAnswered = MakeShared<std::atomic<bool>>(false);

			Request->OnProcessRequestComplete().BindLambda(
				[Sink, bStreams, OnDone, bAnswered](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
				{
					if (bAnswered->exchange(true))
					{
						return;
					}
					FMantleAssetFetchResult Result;
					Result.bConnected = bConnected && Response.IsValid();
					Result.StatusCode = Response.IsValid() ? Response->GetResponseCode() : 0;
					if (!Result.bConnected)
					{
						Result.Error = TEXT("no response");
					}
					else if (!bStreams)
					{
						TArray<uint8> Content = Response->GetContent();
						if (Content.Num() > 0)
						{
							Sink->Serialize(Content.GetData(), Content.Num());
						}
					}
					OnDone(Result);
				});

			if (!Request->ProcessRequest() && !bAnswered->exchange(true))
			{
				FMantleAssetFetchResult Result;
				Result.Error = TEXT("the request could not be started");
				OnDone(Result);
			}
		}
	};

	TSharedPtr<FMantleAssetStore> GMantleDefaultAssetStore;
}

// ---------------------------------------------------------------------------------------------
// Settings

FMantleAssetStoreSettings FMantleAssetStoreSettings::FromConfig()
{
	FMantleAssetStoreSettings Settings;
	if (GConfig)
	{
		const TCHAR* const Section = TEXT("Mantle.AssetStore");
		int32 BudgetMB = 0;
		if (GConfig->GetInt(Section, TEXT("DiskBudgetMB"), BudgetMB, GGameIni) && BudgetMB > 0)
		{
			Settings.DiskBudgetBytes = static_cast<int64>(BudgetMB) * 1024 * 1024;
		}
		int32 Concurrent = 0;
		if (GConfig->GetInt(Section, TEXT("MaxConcurrentDownloads"), Concurrent, GGameIni) && Concurrent > 0)
		{
			Settings.MaxConcurrentDownloads = Concurrent;
		}
	}
	return Settings;
}

// ---------------------------------------------------------------------------------------------
// FMantleAssetStore: lifetime

FMantleAssetStore& FMantleAssetStore::Get()
{
	if (!GMantleDefaultAssetStore.IsValid())
	{
		GMantleDefaultAssetStore = Create(FMantleAssetStoreSettings::FromConfig(), CreateHttpFetcher());
	}
	return *GMantleDefaultAssetStore;
}

void FMantleAssetStore::ShutdownDefault()
{
	if (GMantleDefaultAssetStore.IsValid())
	{
		if (GMantleDefaultAssetStore->TickerHandle.IsValid())
		{
			FTSTicker::RemoveTicker(GMantleDefaultAssetStore->TickerHandle);
			GMantleDefaultAssetStore->TickerHandle.Reset();
		}
		GMantleDefaultAssetStore.Reset();
	}
}

TSharedRef<IMantleAssetFetcher> FMantleAssetStore::CreateHttpFetcher()
{
	return MakeShared<FMantleHttpAssetFetcher>();
}

TSharedRef<FMantleAssetStore> FMantleAssetStore::Create(
	const FMantleAssetStoreSettings& InSettings, TSharedRef<IMantleAssetFetcher> InFetcher)
{
	TSharedRef<FMantleAssetStore> Store = MakeShareable(new FMantleAssetStore(InSettings, InFetcher));
	Store->StartScan();

	if (Store->Settings.bAutoPump)
	{
		const TWeakPtr<FMantleAssetStore> Weak = Store;
		Store->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Weak](float)
			{
				if (const TSharedPtr<FMantleAssetStore> Pinned = Weak.Pin())
				{
					Pinned->Pump();
					return true;
				}
				return false;
			}));
	}
	return Store;
}

FMantleAssetStore::FMantleAssetStore(
	const FMantleAssetStoreSettings& InSettings, TSharedRef<IMantleAssetFetcher> InFetcher)
	: Settings(MantleAssetStoreNormalize(InSettings))
	, Fetcher(InFetcher)
{
}

FMantleAssetStore::~FMantleAssetStore()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(TickerHandle);
	}
}

FString FMantleAssetStore::PathFor(const FMantleContentHash& Hash) const
{
	return FPaths::Combine(Settings.RootDir, Hash.ToHex());
}

FString FMantleAssetStore::PartialDir() const
{
	return FPaths::Combine(Settings.RootDir, TEXT(".partial"));
}

void FMantleAssetStore::LaunchTask(const TCHAR* DebugName, TUniqueFunction<void()> Body, bool bAfterScan)
{
	{
		FScopeLock Guard(&Lock);
		++PendingTasks;
	}

	TSharedRef<FMantleAssetStore> Self = AsShared();
	auto Run = [Self, Work = MoveTemp(Body)]() mutable
	{
		Work();
		FScopeLock Guard(&Self->Lock);
		--Self->PendingTasks;
	};

	if (bAfterScan)
	{
		UE::Tasks::Launch(DebugName, MoveTemp(Run), UE::Tasks::Prerequisites(ScanTask),
			UE::Tasks::ETaskPriority::BackgroundNormal);
	}
	else
	{
		UE::Tasks::Launch(DebugName, MoveTemp(Run), UE::Tasks::ETaskPriority::BackgroundNormal);
	}
}

void FMantleAssetStore::Post(TFunction<void()>&& Completion)
{
	FScopeLock Guard(&CompletionLock);
	Completions.Add(MoveTemp(Completion));
}

// ---------------------------------------------------------------------------------------------
// Startup scan

void FMantleAssetStore::StartScan()
{
	TSharedRef<FMantleAssetStore> Self = AsShared();
	ScanTask = UE::Tasks::Launch(TEXT("MantleAssetStoreScan"),
		[Self]() { Self->Scan(); },
		UE::Tasks::ETaskPriority::BackgroundNormal);
}

void FMantleAssetStore::Scan()
{
	IPlatformFile& Files = MantleAssetStoreFiles();
	Files.CreateDirectoryTree(*Settings.RootDir);
	Files.CreateDirectoryTree(*PartialDir());

	struct FFound
	{
		FMantleContentHash Hash;
		int64 Size = 0;
		FDateTime Modified;
	};
	TArray<FFound> Found;

	// Only 64-hex names are the store's; anything else in the folder is left alone.
	Files.IterateDirectoryStat(*Settings.RootDir,
		[&Found](const TCHAR* Name, const FFileStatData& Stat)
		{
			if (Stat.bIsDirectory)
			{
				return true;
			}
			FMantleContentHash Hash;
			if (FMantleContentHash::Parse(MantleAssetStoreHashPrefix + FPaths::GetCleanFilename(Name), Hash))
			{
				Found.Add({Hash, Stat.FileSize, Stat.ModificationTime});
			}
			return true;
		});

	// Partial files from a crash. Only old ones: a young one may be another process's download
	// in progress, since a local -server and -game share Saved/.
	const FDateTime Cutoff = FDateTime::UtcNow() - Settings.StalePartialAge;
	TArray<FString> Stale;
	Files.IterateDirectoryStat(*PartialDir(),
		[&Stale, Cutoff](const TCHAR* Name, const FFileStatData& Stat)
		{
			if (!Stat.bIsDirectory && Stat.ModificationTime < Cutoff)
			{
				Stale.Add(Name);
			}
			return true;
		});
	for (const FString& Path : Stale)
	{
		Files.DeleteFile(*Path);
	}

	// A file's timestamp is its last use (hits touch it), so recency survives a restart.
	Found.Sort([](const FFound& A, const FFound& B) { return A.Modified < B.Modified; });

	int32 Count = 0;
	int64 Bytes = 0;
	{
		FScopeLock Guard(&Lock);
		for (const FFound& File : Found)
		{
			if (Index.Contains(File.Hash))
			{
				continue;
			}
			FEntry& Entry = Index.Add(File.Hash);
			Entry.Size = File.Size;
			Entry.UseSerial = NextUseSerial++;
			StoredBytes += File.Size;
		}
		Count = Index.Num();
		Bytes = StoredBytes;
	}

	UE_LOG(LogMantle, Log, TEXT("Asset store at %s: %d assets, %lld bytes (budget %lld)%s"),
		*Settings.RootDir, Count, Bytes, Settings.DiskBudgetBytes,
		Stale.Num() > 0 ? *FString::Printf(TEXT(", removed %d stale partial downloads"), Stale.Num()) : TEXT(""));

	// A smaller budget than last run takes effect before anything is requested.
	EvictToBudget();
	bScanComplete.store(true);
}

// ---------------------------------------------------------------------------------------------
// Requests

uint64 FMantleAssetStore::Request(const FMantleStoredAssetRef& Ref, EMantleAssetLoad Load, FMantleAssetDone OnDone)
{
	const bool bWantsBytes = Load == EMantleAssetLoad::Bytes;

	uint64 RequestId = 0;
	TSharedPtr<FJob> NewJob;
	FString Refusal;
	{
		FScopeLock Guard(&Lock);
		RequestId = NextRequestId++;
		RequestHashes.Add(RequestId, Ref.Hash);

		if (!Ref.Hash.IsValid())
		{
			Refusal = TEXT("The asset has no valid content hash.");
		}
		else if (Ref.Bytes > Settings.MaxAssetBytes)
		{
			Refusal = FString::Printf(TEXT("The asset is %lld bytes; the store accepts at most %lld."),
				Ref.Bytes, Settings.MaxAssetBytes);
		}
		else if (TSharedRef<FJob>* Existing = Jobs.Find(Ref.Hash))
		{
			// Coalesced: one resolve and one download per hash, however many callers ask.
			FJob& Job = Existing->Get();
			Job.Waiters.Add({RequestId, bWantsBytes, OnDone});
			if (!Ref.Url.IsEmpty())
			{
				Job.Urls.AddUnique(Ref.Url);
			}
			if (Job.ExpectedBytes <= 0 && Ref.Bytes > 0)
			{
				Job.ExpectedBytes = Ref.Bytes;
			}
			return RequestId;
		}
		else
		{
			NewJob = MakeShared<FJob>();
			NewJob->Hash = Ref.Hash;
			if (!Ref.Url.IsEmpty())
			{
				NewJob->Urls.Add(Ref.Url);
			}
			NewJob->ExpectedBytes = Ref.Bytes;
			NewJob->Waiters.Add({RequestId, bWantsBytes, OnDone});
			Jobs.Add(Ref.Hash, NewJob.ToSharedRef());
		}
	}

	if (!Refusal.IsEmpty())
	{
		FMantleAssetResult Result;
		Result.Hash = Ref.Hash;
		Result.Error = Refusal;
		UE_LOG(LogMantle, Warning, TEXT("Asset store: refused %s: %s"), *Ref.Hash.ToString(), *Refusal);
		const TWeakPtr<FMantleAssetStore> Weak = AsShared();
		Post([Weak, RequestId, OnDone, Result]()
		{
			const TSharedPtr<FMantleAssetStore> Self = Weak.Pin();
			if (Self && Self->TakeRequest(RequestId))
			{
				OnDone.ExecuteIfBound(Result);
			}
		});
		return RequestId;
	}

	const TSharedRef<FJob> Job = NewJob.ToSharedRef();
	LaunchTask(TEXT("MantleAssetStoreResolve"), [this, Job]() { Resolve(Job); }, /*bAfterScan*/ true);
	return RequestId;
}

bool FMantleAssetStore::TakeRequest(uint64 RequestId)
{
	FScopeLock Guard(&Lock);
	return RequestHashes.Remove(RequestId) > 0;
}

void FMantleAssetStore::Cancel(uint64 RequestId)
{
	FScopeLock Guard(&Lock);
	FMantleContentHash Hash;
	if (!RequestHashes.RemoveAndCopyValue(RequestId, Hash))
	{
		return;
	}

	TSharedRef<FJob>* Found = Jobs.Find(Hash);
	if (!Found)
	{
		return;
	}
	FJob& Job = Found->Get();
	Job.Waiters.RemoveAll([RequestId](const FWaiter& Waiter) { return Waiter.RequestId == RequestId; });

	// A queued download nobody wants any more is dropped (its queue entry is skipped when it
	// comes up). One already resolving or downloading finishes: the bytes are likely wanted again.
	if (Job.Waiters.Num() == 0 && Job.State == EJobState::Queued)
	{
		Jobs.Remove(Hash);
	}
}

void FMantleAssetStore::Resolve(TSharedRef<FJob> Job)
{
	const FMantleContentHash Hash = Job->Hash;
	const FString Path = PathFor(Hash);

	bool bPresent = false;
	bool bVerified = false;
	bool bWantsBytes = false;
	{
		FScopeLock Guard(&Lock);
		if (FEntry* Entry = Index.Find(Hash))
		{
			bPresent = true;
			bVerified = Entry->bVerified;
			++Entry->Readers;
		}
		for (const FWaiter& Waiter : Job->Waiters)
		{
			bWantsBytes |= Waiter.bWantsBytes;
		}
	}

	if (bPresent)
	{
		// Bytes going into memory are always re-hashed (BLAKE3 outruns the disk); a path-only
		// hit is hashed once per process, the first time it is used.
		TSharedPtr<TArray<uint8>, ESPMode::ThreadSafe> Bytes;
		bool bRead = true;
		bool bMatches = true;
		int64 ReadSize = 0;
		if (bWantsBytes || !bVerified)
		{
			if (bWantsBytes)
			{
				Bytes = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>();
			}
			FBlake3Hash Actual;
			bRead = MantleAssetStoreHashFile(Path, Settings.MaxAssetBytes, Actual, ReadSize, Bytes.Get());
			bMatches = bRead && Actual == Hash.Digest;
		}

		if (bMatches)
		{
			int64 Size = ReadSize;
			{
				FScopeLock Guard(&Lock);
				// Our reader count keeps eviction away; only this job's hash could remove it.
				if (FEntry* Entry = Index.Find(Hash))
				{
					--Entry->Readers;
					Entry->bVerified = true;
					Entry->UseSerial = NextUseSerial++;
					if (ReadSize > 0 && ReadSize != Entry->Size)
					{
						StoredBytes += ReadSize - Entry->Size;
						Entry->Size = ReadSize;
					}
					Size = Entry->Size;
				}
			}
			// Recency for the next process's scan. Best effort: another process may hold the file.
			MantleAssetStoreFiles().SetTimeStamp(*Path, FDateTime::UtcNow());
			Deliver(Job, /*bFromCache*/ true, Size, Bytes);
			return;
		}

		// Corrupt or gone: forget it, delete it, and fetch it again.
		{
			FScopeLock Guard(&Lock);
			if (const FEntry* Entry = Index.Find(Hash))
			{
				StoredBytes -= Entry->Size;
				Index.Remove(Hash);
			}
			MantleAssetStoreFiles().DeleteFile(*Path);
		}
		if (bRead)
		{
			UE_LOG(LogMantle, Warning, TEXT("Asset store: %s on disk does not match its hash; deleted, fetching it again"),
				*Hash.ToString());
		}
		else
		{
			UE_LOG(LogMantle, Warning, TEXT("Asset store: %s could not be read from disk; fetching it again"),
				*Hash.ToString());
		}
	}

	bool bHasUrl = false;
	{
		FScopeLock Guard(&Lock);
		bHasUrl = Job->Urls.Num() > 0;
	}
	if (!bHasUrl)
	{
		Fail(Job, TEXT("The asset is not in the store and no URL was given to fetch it from."), 0);
		return;
	}
	EnqueueDownload(Job);
}

void FMantleAssetStore::EnqueueDownload(const TSharedRef<FJob>& Job)
{
	FScopeLock Guard(&Lock);
	Job->State = EJobState::Queued;
	DownloadQueue.Add(Job->Hash);
}

// ---------------------------------------------------------------------------------------------
// The game-thread pump

void FMantleAssetStore::Pump()
{
	struct FStart
	{
		TSharedRef<FJob> Job;
		TSharedRef<FMantleAssetSink> Sink;
		FString Url;
	};
	TArray<FStart> Starts;
	{
		FScopeLock Guard(&Lock);
		int32 Consumed = 0;
		while (ActiveDownloads < Settings.MaxConcurrentDownloads && Consumed < DownloadQueue.Num())
		{
			const FMantleContentHash Hash = DownloadQueue[Consumed++];
			TSharedRef<FJob>* Found = Jobs.Find(Hash);
			if (!Found || (*Found)->State != EJobState::Queued || (*Found)->Urls.Num() == 0)
			{
				continue; // cancelled, or a duplicate entry for a job already started
			}

			const TSharedRef<FJob> Job = *Found;
			Job->State = EJobState::Downloading;
			++ActiveDownloads;

			const FString PartialPath = FPaths::Combine(PartialDir(), FString::Printf(TEXT("%s.%s.part"),
				*Hash.ToHex(), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
			const int64 Limit = Job->ExpectedBytes > 0 ? Job->ExpectedBytes : Settings.MaxAssetBytes;
			const TSharedRef<FMantleAssetSink> Sink = MakeShared<FMantleAssetSink>(PartialPath, Limit);
			Job->Sink = Sink;

			Starts.Add({Job, Sink, Job->Urls[Job->UrlIndex % Job->Urls.Num()]});
		}
		if (Consumed > 0)
		{
			DownloadQueue.RemoveAt(0, Consumed, EAllowShrinking::No);
		}
	}

	// Outside the lock: a fetcher may complete synchronously, and completion takes the lock.
	const TWeakPtr<FMantleAssetStore> Weak = AsShared();
	for (const FStart& Start : Starts)
	{
		UE_LOG(LogMantle, Verbose, TEXT("Asset store: downloading %s"), *Start.Job->Hash.ToString());
		const TSharedRef<FJob> Job = Start.Job;
		const TSharedRef<FMantleAssetSink> Sink = Start.Sink;
		Fetcher->Fetch(Start.Url, Sink, [Weak, Job, Sink](const FMantleAssetFetchResult& Result)
		{
			const TSharedPtr<FMantleAssetStore> Self = Weak.Pin();
			if (!Self)
			{
				// The store is gone (shutdown). Leave nothing half-written behind.
				FBlake3Hash Ignored;
				int64 IgnoredSize = 0;
				Sink->Finish(Ignored, IgnoredSize);
				MantleAssetStoreFiles().DeleteFile(*Sink->GetPath());
				return;
			}
			FMantleAssetStore* Store = Self.Get();
			Store->LaunchTask(TEXT("MantleAssetStoreFinalize"),
				[Store, Job, Sink, Result]() { Store->Finalize(Job, Sink, Result); },
				/*bAfterScan*/ false);
		});
	}

	TArray<TFunction<void()>> Ready;
	{
		FScopeLock Guard(&CompletionLock);
		Ready = MoveTemp(Completions);
		Completions.Reset();
	}
	for (TFunction<void()>& Completion : Ready)
	{
		Completion();
	}
}

bool FMantleAssetStore::IsIdle() const
{
	if (!bScanComplete.load())
	{
		return false;
	}
	{
		FScopeLock Guard(&Lock);
		if (Jobs.Num() > 0 || PendingTasks > 0 || ActiveDownloads > 0)
		{
			return false;
		}
	}
	FScopeLock Guard(&CompletionLock);
	return Completions.Num() == 0;
}

bool FMantleAssetStore::WaitUntilIdle(float TimeoutSeconds)
{
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	for (;;)
	{
		Pump();
		if (IsIdle())
		{
			return true;
		}
		if (FPlatformTime::Seconds() >= Deadline)
		{
			return false;
		}
		FPlatformProcess::Sleep(0.001f);
	}
}

// ---------------------------------------------------------------------------------------------
// Finishing a download

void FMantleAssetStore::Finalize(
	const TSharedRef<FJob>& Job, const TSharedRef<FMantleAssetSink>& Sink, const FMantleAssetFetchResult& Result)
{
	IPlatformFile& Files = MantleAssetStoreFiles();
	const FMantleContentHash Hash = Job->Hash;

	FBlake3Hash Actual;
	int64 Size = 0;
	const bool bWritten = Sink->Finish(Actual, Size);
	const bool bHttpOk = Result.bConnected && Result.StatusCode >= 200 && Result.StatusCode < 300;

	FString Error;
	bool bRetryable = true;
	if (Sink->WasTooLarge())
	{
		// Checked first: refusing the bytes is what made the HTTP layer report a failure.
		Error = FString::Printf(TEXT("the response is larger than the asset's %lld bytes"), Sink->GetMaxBytes());
		bRetryable = false;
	}
	else if (!bHttpOk)
	{
		Error = Result.bConnected
			? FString::Printf(TEXT("HTTP %d"), Result.StatusCode)
			: (Result.Error.IsEmpty() ? FString(TEXT("no response")) : Result.Error);
		bRetryable = MantleAssetStoreIsRetryable(Result);
	}
	else if (!bWritten)
	{
		Error = FString::Printf(TEXT("could not write %s"), *Sink->GetPath());
	}
	else if (Actual != Hash.Digest)
	{
		FMantleContentHash Got;
		Got.Digest = Actual;
		Error = FString::Printf(TEXT("the downloaded bytes hash to %s"), *Got.ToString());
	}

	if (!Error.IsEmpty())
	{
		Files.DeleteFile(*Sink->GetPath());

		bool bRetry = false;
		int32 Attempt = 0;
		{
			FScopeLock Guard(&Lock);
			--ActiveDownloads;
			Job->Sink.Reset();
			Job->LastStatus = Result.StatusCode;
			Job->LastError = Error;
			Attempt = ++Job->Attempts;
			if (bRetryable && Job->Attempts < Settings.MaxAttempts)
			{
				// The next URL, when a caller supplied more than one.
				++Job->UrlIndex;
				Job->State = EJobState::Queued;
				DownloadQueue.Add(Hash);
				bRetry = true;
			}
		}

		UE_LOG(LogMantle, Warning, TEXT("Asset store: download of %s failed (attempt %d of %d): %s%s"),
			*Hash.ToString(), Attempt, Settings.MaxAttempts, *Error, bRetry ? TEXT("; retrying") : TEXT(""));
		if (!bRetry)
		{
			Fail(Job, Error, Result.StatusCode);
		}
		return;
	}

	// Verified: rename into place. Under the lock, so eviction's deletes (also under it) can
	// never interleave with this rename for the same name.
	const FString FinalPath = PathFor(Hash);
	bool bMoved = false;
	{
		FScopeLock Guard(&Lock);
		--ActiveDownloads;
		Job->Sink.Reset();
		Job->State = EJobState::Finalizing;

		// Windows' rename refuses to replace; a leftover file under this name is either an
		// evicted copy not deleted yet or another process's. Both are replaceable.
		if (Files.FileExists(*FinalPath))
		{
			Files.DeleteFile(*FinalPath);
		}
		bMoved = Files.MoveFile(*FinalPath, *Sink->GetPath());
	}

	if (!bMoved)
	{
		Files.DeleteFile(*Sink->GetPath());

		// Another process holding its own verified copy open (Windows) blocks both the delete
		// and the rename. Its copy is as good as ours if it hashes right.
		FBlake3Hash Existing;
		int64 ExistingSize = 0;
		if (!MantleAssetStoreHashFile(FinalPath, Settings.MaxAssetBytes, Existing, ExistingSize, nullptr)
			|| Existing != Hash.Digest)
		{
			Fail(Job, FString::Printf(TEXT("could not move the download into %s"), *FinalPath), 0);
			return;
		}
		Size = ExistingSize;
	}

	{
		FScopeLock Guard(&Lock);
		FEntry& Entry = Index.FindOrAdd(Hash);
		StoredBytes += Size - Entry.Size;
		Entry.Size = Size;
		Entry.UseSerial = NextUseSerial++;
		Entry.bVerified = true;
	}

	UE_LOG(LogMantle, Log, TEXT("Asset store: stored %s (%lld bytes)"), *Hash.ToString(), Size);

	// This hash cannot be a victim: its job is still registered until Deliver.
	EvictToBudget();
	Deliver(Job, /*bFromCache*/ false, Size, nullptr);
}

void FMantleAssetStore::Deliver(const TSharedRef<FJob>& Job, bool bFromCache, int64 Size,
	TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Bytes)
{
	const FMantleContentHash Hash = Job->Hash;

	TArray<FWaiter> Waiters;
	bool bNeedBytes = false;
	{
		FScopeLock Guard(&Lock);
		if (const TSharedRef<FJob>* Registered = Jobs.Find(Hash); Registered && &Registered->Get() == &Job.Get())
		{
			Jobs.Remove(Hash);
		}
		Waiters = MoveTemp(Job->Waiters);
		Job->Waiters.Reset();

		bool bAnyWantsBytes = false;
		for (const FWaiter& Waiter : Waiters)
		{
			bAnyWantsBytes |= Waiter.bWantsBytes;
		}
		// A reader keeps eviction off the file while it is read back outside the lock.
		if (bAnyWantsBytes && !Bytes.IsValid())
		{
			if (FEntry* Entry = Index.Find(Hash))
			{
				++Entry->Readers;
				bNeedBytes = true;
			}
		}
	}

	FString ReadError;
	if (bNeedBytes)
	{
		// Downloads stream to disk, so a caller wanting bytes reads the fresh file back (from the
		// OS cache, normally). Verified again, like every read into memory.
		TSharedPtr<TArray<uint8>, ESPMode::ThreadSafe> Loaded = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>();
		FBlake3Hash Actual;
		int64 ReadSize = 0;
		const bool bRead = MantleAssetStoreHashFile(PathFor(Hash), Settings.MaxAssetBytes, Actual, ReadSize, Loaded.Get());
		{
			FScopeLock Guard(&Lock);
			if (FEntry* Entry = Index.Find(Hash))
			{
				--Entry->Readers;
			}
		}
		if (bRead && Actual == Hash.Digest)
		{
			Bytes = TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>(Loaded);
		}
		else
		{
			ReadError = FString::Printf(TEXT("%s could not be read back from the store."), *Hash.ToString());
		}
	}
	else if (!Bytes.IsValid())
	{
		for (const FWaiter& Waiter : Waiters)
		{
			if (Waiter.bWantsBytes)
			{
				ReadError = FString::Printf(TEXT("%s left the store before it could be read."), *Hash.ToString());
				break;
			}
		}
	}

	if (!bFromCache || !ReadError.IsEmpty())
	{
		UE_LOG(LogMantle, Verbose, TEXT("Asset store: delivering %s to %d callers%s"),
			*Hash.ToString(), Waiters.Num(), ReadError.IsEmpty() ? TEXT("") : *(TEXT(" (") + ReadError + TEXT(")")));
	}

	FMantleAssetResult Base;
	Base.bSuccess = true;
	Base.Hash = Hash;
	Base.Path = PathFor(Hash);
	Base.bFromCache = bFromCache;
	Base.SizeBytes = Size;

	const TWeakPtr<FMantleAssetStore> Weak = AsShared();
	Post([Weak, Delivered = MoveTemp(Waiters), Base, Bytes, ReadError]()
	{
		const TSharedPtr<FMantleAssetStore> Self = Weak.Pin();
		if (!Self)
		{
			return;
		}
		for (const FWaiter& Waiter : Delivered)
		{
			// Cancelled after delivery was posted: stay silent, as Cancel promises.
			if (!Self->TakeRequest(Waiter.RequestId))
			{
				continue;
			}
			FMantleAssetResult Result = Base;
			if (Waiter.bWantsBytes)
			{
				if (Bytes.IsValid())
				{
					Result.Bytes = Bytes;
				}
				else
				{
					Result.bSuccess = false;
					Result.Error = ReadError;
				}
			}
			Waiter.OnDone.ExecuteIfBound(Result);
		}
	});
}

void FMantleAssetStore::Fail(const TSharedRef<FJob>& Job, const FString& Error, int32 StatusCode)
{
	const FMantleContentHash Hash = Job->Hash;
	TArray<FWaiter> Waiters;
	{
		FScopeLock Guard(&Lock);
		if (const TSharedRef<FJob>* Registered = Jobs.Find(Hash); Registered && &Registered->Get() == &Job.Get())
		{
			Jobs.Remove(Hash);
		}
		Waiters = MoveTemp(Job->Waiters);
		Job->Waiters.Reset();
	}

	UE_LOG(LogMantle, Warning, TEXT("Asset store: could not get %s: %s"), *Hash.ToString(), *Error);

	FMantleAssetResult Result;
	Result.Hash = Hash;
	Result.Path = PathFor(Hash);
	Result.StatusCode = StatusCode;
	Result.Error = Error;

	const TWeakPtr<FMantleAssetStore> Weak = AsShared();
	Post([Weak, Failed = MoveTemp(Waiters), Result]()
	{
		const TSharedPtr<FMantleAssetStore> Self = Weak.Pin();
		if (!Self)
		{
			return;
		}
		for (const FWaiter& Waiter : Failed)
		{
			if (Self->TakeRequest(Waiter.RequestId))
			{
				Waiter.OnDone.ExecuteIfBound(Result);
			}
		}
	});
}

// ---------------------------------------------------------------------------------------------
// Seeding from a local copy

void FMantleAssetStore::Seed(const FMantleStoredAssetRef& Ref, const FString& SourceFile, FMantleAssetDone OnDone)
{
	const TWeakPtr<FMantleAssetStore> Weak = AsShared();
	LaunchTask(TEXT("MantleAssetStoreSeed"), [this, Weak, Ref, SourceFile, OnDone]()
	{
		const FMantleAssetResult Result = SeedFromFile(Ref, SourceFile);
		Post([Weak, OnDone, Result]()
		{
			if (Weak.Pin())
			{
				OnDone.ExecuteIfBound(Result);
			}
		});
	}, /*bAfterScan*/ true);
}

FMantleAssetResult FMantleAssetStore::SeedFromFile(const FMantleStoredAssetRef& Ref, const FString& SourceFile)
{
	IPlatformFile& Files = MantleAssetStoreFiles();
	const FMantleContentHash Hash = Ref.Hash;
	FMantleAssetResult Result;
	Result.Hash = Hash;
	Result.Path = PathFor(Hash);

	if (!Hash.IsValid())
	{
		Result.Error = TEXT("The asset has no valid content hash.");
		return Result;
	}
	{
		FScopeLock Guard(&Lock);
		if (const FEntry* Entry = Index.Find(Hash))
		{
			// Stored already (a download, or a previous run's seed): verified when it is read, like any hit.
			Result.bSuccess = true;
			Result.bFromCache = true;
			Result.SizeBytes = Entry->Size;
			return Result;
		}
	}

	// Read once, verify the bytes in memory, write those same bytes: nothing unverified reaches the store.
	TArray<uint8> Bytes;
	FBlake3Hash Actual;
	int64 Size = 0;
	if (!MantleAssetStoreHashFile(SourceFile, Settings.MaxAssetBytes, Actual, Size, &Bytes))
	{
		Result.Error = FString::Printf(TEXT("%s could not be read (or is larger than %lld bytes)"), *SourceFile, Settings.MaxAssetBytes);
	}
	else if (Ref.Bytes > 0 && Size != Ref.Bytes)
	{
		Result.Error = FString::Printf(TEXT("%s is %lld bytes, not the %lld its manifest says"), *SourceFile, Size, Ref.Bytes);
	}
	else if (Actual != Hash.Digest)
	{
		FMantleContentHash Got;
		Got.Digest = Actual;
		Result.Error = FString::Printf(TEXT("%s hashes to %s, not %s"), *SourceFile, *Got.ToString(), *Hash.ToString());
	}
	if (!Result.Error.IsEmpty())
	{
		UE_LOG(LogMantle, Warning, TEXT("Asset store: refused to seed %s: %s"), *Hash.ToString(), *Result.Error);
		return Result;
	}

	const FString PartialPath = FPaths::Combine(PartialDir(), FString::Printf(TEXT("%s.%s.part"),
		*Hash.ToHex(), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	bool bWritten = false;
	{
		TUniquePtr<IFileHandle> Handle(Files.OpenWrite(*PartialPath));
		if (!Handle)
		{
			Files.CreateDirectoryTree(*PartialDir());
			Handle.Reset(Files.OpenWrite(*PartialPath));
		}
		bWritten = Handle.IsValid() && (Bytes.Num() == 0 || Handle->Write(Bytes.GetData(), Bytes.Num())) && Handle->Flush();
	}
	if (!bWritten)
	{
		Files.DeleteFile(*PartialPath);
		Result.Error = FString::Printf(TEXT("could not write %s"), *PartialPath);
		UE_LOG(LogMantle, Warning, TEXT("Asset store: could not seed %s: %s"), *Hash.ToString(), *Result.Error);
		return Result;
	}

	// Into place under the lock, as Finalize does: a download of the same hash may have landed meanwhile.
	bool bMoved = false;
	bool bRaced = false;
	{
		FScopeLock Guard(&Lock);
		if (const FEntry* Existing = Index.Find(Hash))
		{
			bRaced = true;
			Size = Existing->Size;
		}
		else
		{
			const FString FinalPath = PathFor(Hash);
			if (Files.FileExists(*FinalPath))
			{
				Files.DeleteFile(*FinalPath);
			}
			bMoved = Files.MoveFile(*FinalPath, *PartialPath);
			if (bMoved)
			{
				FEntry& Entry = Index.Add(Hash);
				Entry.Size = Size;
				Entry.UseSerial = NextUseSerial++;
				Entry.bVerified = true;
				StoredBytes += Size;
			}
		}
	}
	if (!bMoved)
	{
		Files.DeleteFile(*PartialPath);
	}
	if (!bMoved && !bRaced)
	{
		Result.Error = FString::Printf(TEXT("could not move the seed into %s"), *PathFor(Hash));
		UE_LOG(LogMantle, Warning, TEXT("Asset store: could not seed %s: %s"), *Hash.ToString(), *Result.Error);
		return Result;
	}

	if (bMoved)
	{
		UE_LOG(LogMantle, Log, TEXT("Asset store: seeded %s (%lld bytes) from %s"), *Hash.ToString(), Size, *SourceFile);
		EvictToBudget();
	}
	Result.bSuccess = true;
	Result.bFromCache = bRaced;
	Result.SizeBytes = Size;
	return Result;
}

// ---------------------------------------------------------------------------------------------
// Pins, eviction, queries

void FMantleAssetStore::Pin(const FMantleContentHash& Hash)
{
	FScopeLock Guard(&Lock);
	++Pins.FindOrAdd(Hash);
}

void FMantleAssetStore::Unpin(const FMantleContentHash& Hash)
{
	bool bOverBudget = false;
	{
		FScopeLock Guard(&Lock);
		int32* Count = Pins.Find(Hash);
		if (!Count)
		{
			return;
		}
		if (--(*Count) <= 0)
		{
			Pins.Remove(Hash);
		}
		bOverBudget = StoredBytes > Settings.DiskBudgetBytes;
	}
	if (bOverBudget)
	{
		// Deleting files is disk work; never on the game thread.
		LaunchTask(TEXT("MantleAssetStoreEvict"), [this]() { EvictToBudget(); }, /*bAfterScan*/ true);
	}
}

bool FMantleAssetStore::IsEvictableLocked(const FMantleContentHash& Hash, const FEntry& Entry) const
{
	return Entry.Readers == 0 && !Pins.Contains(Hash) && !Jobs.Contains(Hash);
}

void FMantleAssetStore::EvictToBudget()
{
	TArray<FMantleContentHash> Victims;
	int64 Remaining = 0;
	{
		FScopeLock Guard(&Lock);
		if (StoredBytes <= Settings.DiskBudgetBytes)
		{
			bWarnedOverBudget = false;
			return;
		}

		TArray<TPair<int64, FMantleContentHash>> Candidates;
		for (const TPair<FMantleContentHash, FEntry>& Pair : Index)
		{
			if (IsEvictableLocked(Pair.Key, Pair.Value))
			{
				Candidates.Emplace(Pair.Value.UseSerial, Pair.Key);
			}
		}
		Candidates.Sort([](const TPair<int64, FMantleContentHash>& A, const TPair<int64, FMantleContentHash>& B)
		{
			return A.Key < B.Key;
		});

		const int64 Target = static_cast<int64>(static_cast<double>(Settings.DiskBudgetBytes) * Settings.EvictionLowWater);
		for (const TPair<int64, FMantleContentHash>& Candidate : Candidates)
		{
			if (StoredBytes <= Target)
			{
				break;
			}
			StoredBytes -= Index.FindChecked(Candidate.Value).Size;
			Index.Remove(Candidate.Value);
			Victims.Add(Candidate.Value);
		}

		Remaining = StoredBytes;
		if (StoredBytes > Settings.DiskBudgetBytes && !bWarnedOverBudget)
		{
			bWarnedOverBudget = true;
			UE_LOG(LogMantle, Warning,
				TEXT("Asset store: %lld bytes in use or pinned, over the %lld-byte budget; nothing more can be evicted"),
				StoredBytes, Settings.DiskBudgetBytes);
		}
	}

	IPlatformFile& Files = MantleAssetStoreFiles();
	for (const FMantleContentHash& Victim : Victims)
	{
		// One file per lock, so the game thread is never held behind a long run of deletes. A
		// download of the same hash that finished meanwhile is back in the index: keep its file.
		FScopeLock Guard(&Lock);
		if (!Index.Contains(Victim))
		{
			Files.DeleteFile(*PathFor(Victim));
		}
	}

	if (Victims.Num() > 0)
	{
		UE_LOG(LogMantle, Log, TEXT("Asset store: evicted %d assets; %lld bytes stored"), Victims.Num(), Remaining);
	}
}

bool FMantleAssetStore::Contains(const FMantleContentHash& Hash) const
{
	FScopeLock Guard(&Lock);
	return Index.Contains(Hash);
}

int64 FMantleAssetStore::GetInFlightBytes(const FMantleContentHash& Hash) const
{
	FScopeLock Guard(&Lock);
	const TSharedRef<FJob>* Found = Jobs.Find(Hash);
	if (!Found || !(*Found)->Sink.IsValid())
	{
		return 0;
	}
	return (*Found)->Sink->GetBytesWritten();
}
