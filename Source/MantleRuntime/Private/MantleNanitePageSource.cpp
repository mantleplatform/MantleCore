// Copyright Joshua Eger. All Rights Reserved.
#include "MantleNanitePageSource.h"

#include "Containers/Ticker.h"
#include "Engine/StaticMesh.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/CriticalSection.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "IO/IoDispatcher.h"
#include "IO/IoDispatcherBackend.h"
#include "MantleAssetStore.h"
#include "MantleNaniteSideFile.h"
#include "Misc/ScopeLock.h"
#include "Rendering/NaniteResources.h"
#include "Tasks/Task.h"
#include "UObject/WeakObjectPtrTemplates.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleNanitePages, Log, All);

#if WITH_DEV_AUTOMATION_TESTS
TFunction<void()> FMantleNanitePageSource::GTestBeforeRead;
#endif

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleNanitePageSourcePrivate
{
	/** A chunk id of ours: 12 bytes, the chunk type BulkData in the last, the magic "MNPG" in bytes 0 to 3 and a counter in bytes 4 to 11 less the last (an id of the engine's own never starts with it). */
	constexpr uint32 ChunkMagic = 0x47504E4D; // 'M' 'N' 'P' 'G' as little-endian bytes
	constexpr int64 LogEveryPages = 512;
	constexpr float StatsSeconds = 1.0f;
	constexpr int32 PruneEveryTicks = 5;

	/** What a packaged game's file handle keeps for each open file (FAsyncBufferedFileReaderWindows: two 64 KB buffers); the editor's handle keeps none. */
	constexpr int64 OpenFileBufferBytes =
#if WITH_EDITOR
		0;
#else
		2 * 64 * 1024;
#endif

	FIoChunkId MakeChunkId(const uint32 Counter)
	{
		uint8 Id[12] = {};
		FMemory::Memcpy(Id, &ChunkMagic, sizeof(uint32));
		FMemory::Memcpy(Id + 4, &Counter, sizeof(uint32));
		Id[11] = static_cast<uint8>(EIoChunkType::BulkData);
		FIoChunkId ChunkId;
		ChunkId.Set(Id, sizeof(Id));
		return ChunkId;
	}

	bool TryGetCounter(const FIoChunkId& ChunkId, uint32& OutCounter)
	{
		const uint8* Id = ChunkId.GetData();
		uint32 Magic = 0;
		FMemory::Memcpy(&Magic, Id, sizeof(uint32));
		if (Magic != ChunkMagic || ChunkId.GetChunkType() != EIoChunkType::BulkData)
		{
			return false;
		}
		FMemory::Memcpy(&OutCounter, Id + 4, sizeof(uint32));
		return true;
	}

	void UpdateMax(TAtomic<int64>& Max, const int64 Value)
	{
		int64 Current = Max.Load();
		while (Value > Current && !Max.CompareExchange(Current, Value))
		{
		}
	}
}

struct FMantleNanitePageSource::FState
{
	/** One downloaded file, shared by every mesh that names it. The handle is opened by the task that reads and closed least recently used first (at most MaxOpenFiles open). */
	struct FFile
	{
		FString Path;
		int64 ExpectedSize = 0;
		FCriticalSection Lock;
		TSharedPtr<IFileHandle> Handle;
		bool bLoggedUnreadable = false;
	};

	/** What a mesh's store is, shared with the reads in flight on it: kept alive by them after the store itself is dropped. */
	struct FCore
	{
		FCore(const FString& InPath, const TSharedRef<const FMantleNaniteTable>& InTable, const TSharedRef<FFile>& InFile)
			: Path(InPath), Table(InTable), File(InFile)
		{
		}

		FString Path;
		TSharedRef<const FMantleNaniteTable> Table;
		TSharedRef<FFile> File;
		TSharedPtr<FMantleAssetStore> AssetStore;
		FMantleContentHash Hash;
		bool bPinned = false;
		TAtomic<int32> ReadsInFlight{ 0 };
	};

	struct FStore
	{
		TSharedRef<FCore> Core;
		TWeakObjectPtr<UStaticMesh> Mesh;
	};

	enum class EResult : uint8 { Pending, Ok, Refused, Unreadable, InflateFailed, Skipped };

	/** One claimed request. The request object is touched only on the dispatcher's thread (Resolve, Cancel, GetCompleted); a worker works on Target and nothing else of it. */
	struct FJob
	{
		FIoRequestImpl* Request = nullptr;
		TSharedPtr<FCore> Core;
		int32 Piece = 0;
		uint8* Target = nullptr;
		uint32 Raw = 0;
		double ResolvedAt = 0.0;
		double StartedAt = 0.0;
		double DoneAt = 0.0;
		TAtomic<bool> bSkip{ false };
		EResult Result = EResult::Pending;
	};

	mutable FCriticalSection Lock;
	TMap<uint32, FStore> Stores;
	TMap<FString, TWeakPtr<FFile>> Files;
	TArray<TSharedRef<FCore>> Retired;
	uint32 NextCounter = 1;
	bool bMounted = false;
	FTSTicker::FDelegateHandle PruneHandle;
	int32 TicksSincePrune = 0;

	FCriticalSection QueueLock;
	TArray<TSharedRef<FJob>> Queue;
	int32 ActiveWorkers = 0;

	FCriticalSection DoneLock;
	TArray<TSharedRef<FJob>> Done;

	mutable FCriticalSection LruLock;
	TArray<TWeakPtr<FFile>> Open;

	TSharedPtr<const FIoDispatcherBackendContext> BackendContext;

	// The log's own memory of the last line (game thread).
	int64 LastLoggedPages = 0;
	int64 LastSumAnswerUs = 0;
	int64 LastSumQueueUs = 0;

	TAtomic<int64> PagesServed{ 0 };
	TAtomic<int64> BytesServed{ 0 };
	TAtomic<int64> Refused{ 0 };
	TAtomic<int64> FileUnreadable{ 0 };
	TAtomic<int64> InflateFailed{ 0 };
	TAtomic<int64> Cancelled{ 0 };
	TAtomic<int32> InFlight{ 0 };
	TAtomic<int64> SumAnswerUs{ 0 };
	TAtomic<int64> SumQueueUs{ 0 };
	TAtomic<int64> MaxAnswerUs{ 0 };
	TAtomic<int64> MaxQueueUs{ 0 };
	TAtomic<int64> IntervalMaxAnswerUs{ 0 };
	TAtomic<int64> IntervalMaxQueueUs{ 0 };
};

namespace MantleNanitePageSourcePrivate
{
	using FState = FMantleNanitePageSource::FState;

	void WakeDispatcher(FState& State)
	{
		if (State.BackendContext.IsValid())
		{
			State.BackendContext->WakeUpDispatcherThreadDelegate.Execute();
		}
	}

	/** A finished job (done, failed or skipped) goes on the list the dispatcher's thread collects, then the dispatcher is woken. The bytes of a read are written before this. */
	void PushDone(FState& State, const TSharedRef<FState::FJob>& Job)
	{
		{
			FScopeLock ScopeLock(&State.DoneLock);
			State.Done.Add(Job);
		}
		WakeDispatcher(State);
	}

	/**
	 * Notes that this file's handle was used (and, when it was just opened, that one more is open): the least recently used are closed past MaxOpenFiles. A read still running on a closed
	 * handle keeps its own reference to it, so closing is only dropping the file's. Locks are never held together.
	 */
	void TouchOpenFile(FState& State, const TSharedRef<FState::FFile>& File, const bool bNewlyOpened)
	{
		TArray<TSharedPtr<FState::FFile>> Victims;
		{
			FScopeLock ScopeLock(&State.LruLock);
			for (int32 Index = State.Open.Num() - 1; Index >= 0; --Index)
			{
				const TSharedPtr<FState::FFile> Entry = State.Open[Index].Pin();
				if (!Entry.IsValid() || Entry.Get() == &File.Get())
				{
					State.Open.RemoveAt(Index);
				}
			}
			State.Open.Add(File);
			if (bNewlyOpened)
			{
				while (State.Open.Num() > FMantleNanitePageSource::MaxOpenFiles)
				{
					Victims.Add(State.Open[0].Pin());
					State.Open.RemoveAt(0);
				}
			}
		}
		for (const TSharedPtr<FState::FFile>& Victim : Victims)
		{
			if (Victim.IsValid())
			{
				FScopeLock ScopeLock(&Victim->Lock);
				Victim->Handle.Reset();
			}
		}
	}

	/** The file's handle, opened now if it is not open (on the calling task, never on a game or dispatcher thread). Null, with the reason, when it cannot be opened or is not the size its table says. */
	TSharedPtr<IFileHandle> GetHandle(FState& State, const TSharedRef<FState::FFile>& File, FString& OutReason)
	{
		TSharedPtr<IFileHandle> Handle;
		bool bNewlyOpened = false;
		{
			FScopeLock ScopeLock(&File->Lock);
			if (!File->Handle.IsValid())
			{
				IFileHandle* const Raw = FPlatformFileManager::Get().GetPlatformFile().OpenRead(*File->Path);
				if (!Raw)
				{
					OutReason = TEXT("could not be opened");
					return nullptr;
				}
				TSharedPtr<IFileHandle> Opened = MakeShareable(Raw);
				if (Opened->Size() != File->ExpectedSize)
				{
					OutReason = FString::Printf(TEXT("is %lld bytes, its table says %lld"), Opened->Size(), File->ExpectedSize);
					return nullptr;
				}
				File->Handle = Opened;
				bNewlyOpened = true;
			}
			Handle = File->Handle;
		}
		TouchOpenFile(State, File, bNewlyOpened);
		return Handle;
	}

	/** Reads one piece's packed bytes and inflates it into the job's target. Runs on a worker (or inline when the process has no threads). Never touches the request. */
	void ProcessJob(FState& State, FState::FJob& Job, TArray<uint8>& Scratch)
	{
		Job.StartedAt = FPlatformTime::Seconds();
#if WITH_DEV_AUTOMATION_TESTS
		if (FMantleNanitePageSource::GTestBeforeRead)
		{
			FMantleNanitePageSource::GTestBeforeRead();
		}
#endif
		if (Job.bSkip)
		{
			Job.Result = FState::EResult::Skipped;
			return;
		}
		FState::FCore& Core = *Job.Core;
		const FMantleNaniteTable::FPiece& Piece = Core.Table->Pieces[Job.Piece];
		Scratch.SetNumUninitialized(static_cast<int32>(Piece.Packed), EAllowShrinking::No);

		// A read that fails is tried once more after a moment on a fresh handle (a scanner or a backup can hold a file for a few milliseconds); then it fails fast, and the engine's own retries run.
		FString Reason;
		bool bRead = false;
		for (int32 Attempt = 0; Attempt < 2 && !bRead; ++Attempt)
		{
			if (Attempt > 0)
			{
				FPlatformProcess::Sleep(0.01f);
			}
			const TSharedPtr<IFileHandle> Handle = GetHandle(State, Core.File, Reason);
			if (!Handle.IsValid())
			{
				continue;
			}
			bRead = Handle->ReadAt(Scratch.GetData(), Piece.Packed, Piece.PackedStart);
			if (!bRead)
			{
				Reason = TEXT("could not be read");
				FScopeLock ScopeLock(&Core.File->Lock);
				if (Core.File->Handle == Handle)
				{
					Core.File->Handle.Reset();
				}
			}
		}
		if (!bRead)
		{
			Job.Result = FState::EResult::Unreadable;
			bool bLog = false;
			{
				FScopeLock ScopeLock(&Core.File->Lock);
				bLog = !Core.File->bLoggedUnreadable;
				Core.File->bLoggedUnreadable = true;
			}
			if (bLog)
			{
				UE_LOG(LogMantleNanitePages, Warning, TEXT("MANTLE_NANITE_PAGES the file %s %s (page piece %d)"), *Core.Path, *Reason, Job.Piece);
			}
			return;
		}
		if (Job.bSkip)
		{
			Job.Result = FState::EResult::Skipped;
			return;
		}
		FString InflateReason;
		if (!FMantleNaniteSideFile::InflatePiece(TConstArrayView<uint8>(Scratch.GetData(), static_cast<int32>(Piece.Packed)), Job.Target, Job.Raw, &InflateReason))
		{
			Job.Result = FState::EResult::InflateFailed;
			UE_LOG(LogMantleNanitePages, Warning, TEXT("MANTLE_NANITE_PAGES piece %d of %s %s"), Job.Piece, *Core.Path, *InflateReason);
			return;
		}
		Job.Result = FState::EResult::Ok;
	}

	/** After ProcessJob: the numbers, then the job goes to the dispatcher (done list, wake). Nothing of the target is touched after PushDone: the engine reuses its slot as soon as it sees the request done. */
	void FinishJob(const TSharedRef<FState>& State, const TSharedRef<FState::FJob>& Job)
	{
		Job->DoneAt = FPlatformTime::Seconds();
		switch (Job->Result)
		{
		case FState::EResult::Ok:
		{
			const int64 AnswerUs = static_cast<int64>((Job->DoneAt - Job->ResolvedAt) * 1.0e6);
			const int64 QueueUs = static_cast<int64>((Job->StartedAt - Job->ResolvedAt) * 1.0e6);
			State->SumAnswerUs += AnswerUs;
			State->SumQueueUs += QueueUs;
			UpdateMax(State->MaxAnswerUs, AnswerUs);
			UpdateMax(State->MaxQueueUs, QueueUs);
			UpdateMax(State->IntervalMaxAnswerUs, AnswerUs);
			UpdateMax(State->IntervalMaxQueueUs, QueueUs);
			State->BytesServed += Job->Raw;
			const int64 Pages = ++State->PagesServed;
			if (Pages == 1 || Pages % LogEveryPages == 0)
			{
				UE_LOG(LogMantleNanitePages, Log, TEXT("MANTLE_NANITE_PAGES served %lld pages, %lld bytes, refused %lld"), Pages, State->BytesServed.Load(), State->Refused.Load());
			}
			break;
		}
		case FState::EResult::Unreadable:
			++State->FileUnreadable;
			break;
		case FState::EResult::InflateFailed:
			++State->InflateFailed;
			break;
		case FState::EResult::Skipped:
			++State->Cancelled;
			break;
		default:
			break;
		}
		PushDone(*State, Job);
		// The read is over for the pin's purpose: the job still names the store until the dispatcher collects it, but nothing reads the file for it any more.
		--Job->Core->ReadsInFlight;
		--State->InFlight;
	}

	/** A worker task: takes jobs from the one queue until it is empty. At most FMantleNanitePageSource::MaxReadsInFlight of these run. */
	void DrainQueue(const TSharedRef<FState>& State)
	{
		TArray<uint8> Scratch;
		for (;;)
		{
			TSharedPtr<FState::FJob> Job;
			{
				FScopeLock ScopeLock(&State->QueueLock);
				if (State->Queue.IsEmpty())
				{
					--State->ActiveWorkers;
					return;
				}
				Job = State->Queue[0];
				State->Queue.RemoveAt(0);
			}
			ProcessJob(*State, *Job, Scratch);
			FinishJob(State, Job.ToSharedRef());
		}
	}

	/** The dispatcher's side: claims a read and queues it; the workers answer. Modeled on the engine's FPackageResourceIoBackend, with the rules of gates\fable-nanite-residency-3.check.md. */
	class FPageBackend final : public IIoDispatcherBackend
	{
	public:
		explicit FPageBackend(const TSharedRef<FState>& InState)
			: State(InState)
		{
		}

		virtual void Initialize(TSharedRef<const FIoDispatcherBackendContext> Context) override
		{
			State->BackendContext = Context;
		}

		virtual void ResolveIoRequests(FIoRequestList Requests, FIoRequestList& OutUnresolved) override
		{
			while (FIoRequestImpl* Request = Requests.PopHead())
			{
				if (!Resolve(Request))
				{
					OutUnresolved.AddTail(Request);
				}
			}
		}

		virtual FIoRequestImpl* GetCompletedIoRequests() override
		{
			TArray<TSharedRef<FState::FJob>> Items;
			{
				FScopeLock ScopeLock(&State->DoneLock);
				Swap(Items, State->Done);
			}
			FIoRequestImpl* Head = nullptr;
			FIoRequestImpl* Tail = nullptr;
			for (const TSharedRef<FState::FJob>& Job : Items)
			{
				FIoRequestImpl* const Request = Job->Request;
				Request->BackendData = nullptr;
				if (Job->Result != FState::EResult::Ok)
				{
					// Does not change a request that was cancelled (the dispatcher already marked it, and reports it cancelled).
					Request->SetFailed();
				}
				Request->NextRequest = nullptr;
				if (Tail)
				{
					Tail->NextRequest = Request;
				}
				else
				{
					Head = Request;
				}
				Tail = Request;
			}
			return Head;
		}

		virtual void CancelIoRequest(FIoRequestImpl* Request) override
		{
			// Never completes the request here: a worker may be writing into the engine's slot. A read that has not started is skipped; the request still goes through the done list.
			if (FState::FJob* const Job = static_cast<FState::FJob*>(Request->BackendData))
			{
				Job->bSkip = true;
			}
		}

		virtual void UpdatePriorityForIoRequest(FIoRequestImpl* Request) override
		{
		}

		virtual bool DoesChunkExist(const FIoChunkId& ChunkId) const override
		{
			return FindSize(ChunkId) >= 0;
		}

		virtual TIoStatusOr<uint64> GetSizeForChunk(const FIoChunkId& ChunkId) const override
		{
			const int64 Size = FindSize(ChunkId);
			if (Size < 0)
			{
				return FIoStatus(EIoErrorCode::NotFound);
			}
			return static_cast<uint64>(Size);
		}

		virtual TIoStatusOr<FIoMappedRegion> OpenMapped(const FIoChunkId& ChunkId, const FIoReadOptions& Options) override
		{
			return FIoStatus(EIoErrorCode::NotFound);
		}

		virtual const TCHAR* GetName() const override
		{
			return TEXT("MantleNanitePages");
		}

		virtual void Shutdown() override
		{
			// The dispatcher's thread is gone by now and every request has finished (its exit loop waits for them); a read task may still be on its way out, just after it handed its job over
			// and woke the dispatcher: wait for the last of them, so none wakes a dispatcher that no longer exists. A bounded wait (a task that cannot start is not waited for forever).
			const double Limit = FPlatformTime::Seconds() + 5.0;
			for (;;)
			{
				{
					FScopeLock ScopeLock(&State->QueueLock);
					if (State->ActiveWorkers == 0)
					{
						break;
					}
				}
				if (FPlatformTime::Seconds() > Limit)
				{
					break;
				}
				FPlatformProcess::Sleep(0.001f);
			}
			State->BackendContext.Reset();
		}

	private:
		int64 FindSize(const FIoChunkId& ChunkId) const
		{
			uint32 Counter = 0;
			if (!TryGetCounter(ChunkId, Counter))
			{
				return -1;
			}
			FScopeLock ScopeLock(&State->Lock);
			const FState::FStore* Store = State->Stores.Find(Counter);
			return Store ? Store->Core->Table->StreamedBytes : -1;
		}

		/** A request that cannot be answered goes the same way as one that can: through the done list, failed. */
		bool Refuse(FIoRequestImpl* Request)
		{
			const TSharedRef<FState::FJob> Job = MakeShared<FState::FJob>();
			Job->Request = Request;
			Job->Result = FState::EResult::Refused;
			Request->BackendData = &Job.Get();
			++State->Refused;
			PushDone(*State, Job);
			return true;
		}

		bool Resolve(FIoRequestImpl* Request)
		{
			uint32 Counter = 0;
			if (!TryGetCounter(Request->ChunkId, Counter))
			{
				return false;
			}
			// The engine asks for exactly one page: the page's own offset and size (NaniteStreamingManager.cpp, Batch.Read). Anything else is not a page of this file's container.
			const FIoReadOptions& Options = Request->Options;
			const uint64 ReadSize = Options.GetSize();
			TSharedPtr<FState::FCore> Core;
			int32 Piece = INDEX_NONE;
			{
				// The read is counted while the lock is still held: the prune (game thread, same lock) that retires a store and gives its file's pin back sees it, so a read never starts on an unpinned file.
				FScopeLock ScopeLock(&State->Lock);
				if (const FState::FStore* Store = State->Stores.Find(Counter))
				{
					Core = Store->Core;
					Piece = ReadSize == ~uint64(0) ? INDEX_NONE : Core->Table->FindPage(Options.GetOffset(), ReadSize);
					if (Piece != INDEX_NONE)
					{
						++Core->ReadsInFlight;
					}
				}
			}
			if (!Core.IsValid())
			{
				// One of ours, but its mesh is gone: it fails here (a pruned store's read must end, not wait).
				return Refuse(Request);
			}
			if (Piece == INDEX_NONE)
			{
				UE_LOG(LogMantleNanitePages, Warning, TEXT("MANTLE_NANITE_PAGES refused a read of %llu bytes at %llu: it is not one page of the mesh's streamed block (%lld bytes)"),
					ReadSize, Options.GetOffset(), Core->Table->StreamedBytes);
				return Refuse(Request);
			}

			// The buffer is the engine's own slot (Options.GetTargetVa()), wrapped; or a new one when the caller gave none. Made here, on the dispatcher's thread, so the worker only writes bytes.
			Request->CreateBuffer(ReadSize);
			const TSharedRef<FState::FJob> Job = MakeShared<FState::FJob>();
			Job->Request = Request;
			Job->Core = Core;
			Job->Piece = Piece;
			Job->Target = Request->GetBuffer().GetData();
			Job->Raw = static_cast<uint32>(ReadSize);
			Job->ResolvedAt = FPlatformTime::Seconds();
			Request->BackendData = &Job.Get();
			++State->InFlight;

			if (!FPlatformProcess::SupportsMultithreading())
			{
				// No threads (the dispatcher runs single-threaded and ignores cancels): read and inflate here.
				TArray<uint8> Scratch;
				ProcessJob(*State, *Job, Scratch);
				FinishJob(State, Job);
				return true;
			}

			bool bLaunch = false;
			{
				FScopeLock ScopeLock(&State->QueueLock);
				State->Queue.Add(Job);
				if (State->ActiveWorkers < FMantleNanitePageSource::MaxReadsInFlight)
				{
					++State->ActiveWorkers;
					bLaunch = true;
				}
			}
			if (bLaunch)
			{
				const TSharedRef<FState> Held = State;
				UE::Tasks::Launch(TEXT("MantleNanitePageRead"), [Held]() { DrainQueue(Held); }, UE::Tasks::ETaskPriority::BackgroundNormal);
			}
			return true;
		}

		TSharedRef<FState> State;
	};

	/** Drops the stores whose mesh is gone; gives back the pins of dropped stores no read still uses. Game thread. */
	int32 PruneState(FState& State)
	{
		TArray<TSharedRef<FState::FCore>> ToUnpin;
		int32 Dropped = 0;
		int32 Left = 0;
		{
			FScopeLock ScopeLock(&State.Lock);
			TArray<uint32> Gone;
			for (const TPair<uint32, FState::FStore>& Pair : State.Stores)
			{
				if (!Pair.Value.Mesh.IsValid())
				{
					Gone.Add(Pair.Key);
				}
			}
			for (const uint32 Counter : Gone)
			{
				State.Retired.Add(State.Stores.FindChecked(Counter).Core);
				State.Stores.Remove(Counter);
			}
			Dropped = Gone.Num();
			Left = State.Stores.Num();
			for (int32 Index = State.Retired.Num() - 1; Index >= 0; --Index)
			{
				if (State.Retired[Index]->ReadsInFlight.Load() == 0)
				{
					ToUnpin.Add(State.Retired[Index]);
					State.Retired.RemoveAt(Index);
				}
			}
			for (auto It = State.Files.CreateIterator(); It; ++It)
			{
				if (!It.Value().IsValid())
				{
					It.RemoveCurrent();
				}
			}
		}
		for (const TSharedRef<FState::FCore>& Core : ToUnpin)
		{
			if (Core->bPinned && Core->AssetStore.IsValid())
			{
				Core->AssetStore->Unpin(Core->Hash);
			}
		}
		if (Dropped > 0)
		{
			UE_LOG(LogMantleNanitePages, Log, TEXT("MANTLE_NANITE_PAGES released %d store(s) of collected meshes, %d left"), Dropped, Left);
		}
		return Dropped;
	}
}

FMantleNanitePageSource::FMantleNanitePageSource()
	: State(MakeShared<FState>())
{
}

FMantleNanitePageSource& FMantleNanitePageSource::Get()
{
	static FMantleNanitePageSource* Source = new FMantleNanitePageSource();
	return *Source;
}

bool FMantleNanitePageSource::IsMantleChunk(const FIoChunkId& ChunkId)
{
	uint32 Counter = 0;
	return MantleNanitePageSourcePrivate::TryGetCounter(ChunkId, Counter);
}

int64 FMantleNanitePageSource::MaxStreamedPageBytes()
{
	// The engine's staging ring (NaniteStreamingManager.cpp, FRingBufferAllocator): r.Nanite.Streaming.MaxPendingPages x NANITE_ESTIMATED_MAX_PAGE_DISK_SIZE bytes, read when the engine starts.
	int64 MaxPendingPages = 128;
	if (const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Nanite.Streaming.MaxPendingPages")))
	{
		MaxPendingPages = FMath::Max(1, Variable->GetInt());
	}
	const int64 Ring = MaxPendingPages * static_cast<int64>(NANITE_ESTIMATED_MAX_PAGE_DISK_SIZE);
	// Free() never resets the offsets, so an empty ring sits anywhere in it; a block of Size fits from every position only when Size <= Ring / 2 (and a block of the whole ring never fits).
	return Ring / 2 - 1;
}

FIoChunkId FMantleNanitePageSource::Register(const FString& Path, const TSharedRef<const FMantleNaniteTable>& Table, UStaticMesh* Mesh)
{
	return Register(Path, Table, Mesh, nullptr, FMantleContentHash());
}

FIoChunkId FMantleNanitePageSource::Register(const FString& Path, const TSharedRef<const FMantleNaniteTable>& Table, UStaticMesh* Mesh,
	const TSharedPtr<FMantleAssetStore>& AssetStore, const FMantleContentHash& Hash)
{
	check(IsInGameThread());
	using namespace MantleNanitePageSourcePrivate;
	if (Path.IsEmpty() || Table->PageCount() < 1 || Table->StreamedBytes <= 0 || !Mesh)
	{
		return FIoChunkId::InvalidChunkId;
	}
	PruneState(*State);
	FIoChunkId ChunkId;
	bool bMountNow = false;
	TSharedPtr<FState::FCore> Core;
	{
		FScopeLock ScopeLock(&State->Lock);
		TSharedPtr<FState::FFile> File = State->Files.FindOrAdd(Path).Pin();
		if (!File.IsValid() || File->ExpectedSize != Table->FileSize)
		{
			File = MakeShared<FState::FFile>();
			File->Path = Path;
			File->ExpectedSize = Table->FileSize;
			State->Files.Add(Path, File);
		}
		Core = MakeShared<FState::FCore>(Path, Table, File.ToSharedRef());
		Core->AssetStore = AssetStore;
		Core->Hash = Hash;
		const uint32 Counter = State->NextCounter++;
		State->Stores.Add(Counter, FState::FStore{ Core.ToSharedRef(), TWeakObjectPtr<UStaticMesh>(Mesh) });
		ChunkId = MakeChunkId(Counter);
		bMountNow = !State->bMounted;
		State->bMounted = true;
	}
	if (AssetStore.IsValid() && Hash.IsValid())
	{
		// The file must outlive the mesh's draw: the asset set's own pin goes with its builds, and a mesh lives on after them.
		AssetStore->Pin(Hash);
		Core->bPinned = true;
	}
	if (bMountNow)
	{
		FIoDispatcher::Get().Mount(MakeShared<FPageBackend>(State));
		const TSharedRef<FState> Held = State;
		State->PruneHandle = FTSTicker::GetCoreTicker().AddTicker(TEXT("MantleNanitePageSourceStats"), StatsSeconds, [Held](float)
		{
			// Said every second while pages are being served, so the log shows when the detail of a mesh has all arrived (the count stops growing).
			const int64 Pages = Held->PagesServed.Load();
			if (Pages != Held->LastLoggedPages)
			{
				const int64 Delta = Pages - Held->LastLoggedPages;
				const int64 SumAnswerUs = Held->SumAnswerUs.Load();
				const int64 SumQueueUs = Held->SumQueueUs.Load();
				UE_LOG(LogMantleNanitePages, Log, TEXT("MANTLE_NANITE_PAGES now %lld pages served (%lld more in the last %.0f s), %lld bytes, refused %lld, unreadable %lld, inflate failed %lld, cancelled %lld; "
					"request to answer %.2f ms mean, %.2f ms worst; waiting for a read %.2f ms mean, %.2f ms worst (this interval)"),
					Pages, Delta, StatsSeconds, Held->BytesServed.Load(), Held->Refused.Load(), Held->FileUnreadable.Load(), Held->InflateFailed.Load(), Held->Cancelled.Load(),
					(SumAnswerUs - Held->LastSumAnswerUs) / 1000.0 / static_cast<double>(Delta), Held->IntervalMaxAnswerUs.Exchange(0) / 1000.0,
					(SumQueueUs - Held->LastSumQueueUs) / 1000.0 / static_cast<double>(Delta), Held->IntervalMaxQueueUs.Exchange(0) / 1000.0);
				Held->LastLoggedPages = Pages;
				Held->LastSumAnswerUs = SumAnswerUs;
				Held->LastSumQueueUs = SumQueueUs;
				FMantleNanitePageStats Stats = FMantleNanitePageSource::Get().GetStats();
				UE_LOG(LogMantleNanitePages, Log, TEXT("MANTLE_NANITE_PAGES memory: page bytes held %lld, tables %.1f KB for %d stores, open files %d (%.1f KB of read buffers), pins %d, reads in flight %d"),
					Stats.PageBytesHeld, Stats.TableBytes / 1024.0, Stats.Stores, Stats.OpenFiles, Stats.OpenFileBufferBytes / 1024.0, Stats.PinsHeld, Stats.ReadsInFlight);
			}
			if (++Held->TicksSincePrune < PruneEveryTicks)
			{
				return true;
			}
			Held->TicksSincePrune = 0;
			PruneState(*Held);
			return true;
		});
	}
	return ChunkId;
}

int32 FMantleNanitePageSource::Prune()
{
	return MantleNanitePageSourcePrivate::PruneState(*State);
}

FMantleNanitePageStats FMantleNanitePageSource::GetStats() const
{
	FMantleNanitePageStats Stats;
	Stats.PagesServed = State->PagesServed.Load();
	Stats.BytesServed = State->BytesServed.Load();
	Stats.Refused = State->Refused.Load();
	Stats.FileUnreadable = State->FileUnreadable.Load();
	Stats.InflateFailed = State->InflateFailed.Load();
	Stats.Cancelled = State->Cancelled.Load();
	Stats.ReadsInFlight = State->InFlight.Load();
	if (Stats.PagesServed > 0)
	{
		Stats.MeanAnswerMs = State->SumAnswerUs.Load() / 1000.0 / static_cast<double>(Stats.PagesServed);
		Stats.MeanQueueMs = State->SumQueueUs.Load() / 1000.0 / static_cast<double>(Stats.PagesServed);
	}
	Stats.WorstAnswerMs = State->MaxAnswerUs.Load() / 1000.0;
	Stats.WorstQueueMs = State->MaxQueueUs.Load() / 1000.0;
	{
		FScopeLock ScopeLock(&State->Lock);
		Stats.Stores = State->Stores.Num();
		for (const TPair<uint32, FState::FStore>& Pair : State->Stores)
		{
			Stats.TableBytes += Pair.Value.Core->Table->GetAllocatedBytes();
			Stats.PinsHeld += Pair.Value.Core->bPinned ? 1 : 0;
		}
		for (const TSharedRef<FState::FCore>& Core : State->Retired)
		{
			Stats.TableBytes += Core->Table->GetAllocatedBytes();
			Stats.PinsHeld += Core->bPinned ? 1 : 0;
		}
	}
	{
		TArray<TSharedPtr<FState::FFile>> Files;
		{
			FScopeLock ScopeLock(&State->LruLock);
			for (const TWeakPtr<FState::FFile>& Entry : State->Open)
			{
				Files.Add(Entry.Pin());
			}
		}
		for (const TSharedPtr<FState::FFile>& File : Files)
		{
			if (File.IsValid())
			{
				FScopeLock ScopeLock(&File->Lock);
				Stats.OpenFiles += File->Handle.IsValid() ? 1 : 0;
			}
		}
		Stats.OpenFileBufferBytes = static_cast<int64>(Stats.OpenFiles) * MantleNanitePageSourcePrivate::OpenFileBufferBytes;
	}
	return Stats;
}

void FMantleNanitePageSource::ResetStats()
{
	State->PagesServed = 0;
	State->BytesServed = 0;
	State->Refused = 0;
	State->FileUnreadable = 0;
	State->InflateFailed = 0;
	State->Cancelled = 0;
	State->SumAnswerUs = 0;
	State->SumQueueUs = 0;
	State->MaxAnswerUs = 0;
	State->MaxQueueUs = 0;
	State->IntervalMaxAnswerUs = 0;
	State->IntervalMaxQueueUs = 0;
	State->LastLoggedPages = 0;
	State->LastSumAnswerUs = 0;
	State->LastSumQueueUs = 0;
}

int64 FMantleNanitePageSource::GetStoreSize(const FIoChunkId& ChunkId) const
{
	uint32 Counter = 0;
	if (!MantleNanitePageSourcePrivate::TryGetCounter(ChunkId, Counter))
	{
		return -1;
	}
	FScopeLock ScopeLock(&State->Lock);
	const FState::FStore* Store = State->Stores.Find(Counter);
	return Store ? Store->Core->Table->StreamedBytes : -1;
}
