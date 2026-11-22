// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAssetStore.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

// #189, the disk cache: the budget is sized from the drive (a quarter of the free space, 8 to 64 GB; Joshua, 2026-11-22), rises
// for a big place while the drive keeps 10 GB free, and is remembered across restarts; unpinning evicts nothing (leaving City
// Sample evicted it down to 4 GB within a minute, so a rejoin downloaded it all again); a new file makes room least recently
// used first; and Clear deletes what nothing is using.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleAssetStoreBudgetTestPrivate
{
	constexpr int64 GiB = 1024ll * 1024 * 1024;
	constexpr int64 MiB = 1024ll * 1024;

	class FNoFetcher final : public IMantleAssetFetcher
	{
	public:
		virtual void Fetch(const FString&, const TSharedRef<FArchive>&, TFunction<void(const FMantleAssetFetchResult&)> OnDone) override
		{
			FMantleAssetFetchResult Result;
			Result.bConnected = true;
			Result.StatusCode = 404;
			OnDone(Result);
		}
	};

	/** A file of Size bytes, all Fill, written under Dir; its ref for Seed. */
	FMantleStoredAssetRef WriteSource(const FString& Dir, const uint8 Fill, const int32 Size, FString& OutPath)
	{
		TArray<uint8> Bytes;
		Bytes.Init(Fill, Size);
		OutPath = Dir / FString::Printf(TEXT("source_%d.bin"), Fill);
		FFileHelper::SaveArrayToFile(Bytes, *OutPath);
		FMantleStoredAssetRef Ref;
		Ref.Hash = FMantleContentHash::OfBytes(Bytes);
		Ref.Bytes = Size;
		return Ref;
	}

	/** Seeds Ref from Path and pumps the store until it has answered. */
	bool SeedAndWait(FMantleAssetStore& Store, const FMantleStoredAssetRef& Ref, const FString& Path)
	{
		bool bStored = false;
		Store.Seed(Ref, Path, FMantleAssetDone::CreateLambda([&bStored](const FMantleAssetResult& Result) { bStored = Result.bSuccess; }));
		Store.WaitUntilIdle(30.0f);
		return bStored;
	}

	TSharedRef<FMantleAssetStore> MakeStore(const FString& Root, const TFunctionRef<void(FMantleAssetStoreSettings&)> Adjust)
	{
		FMantleAssetStoreSettings Settings;
		Settings.RootDir = Root;
		Settings.bAutoPump = false;
		Adjust(Settings);
		const TSharedRef<FMantleAssetStore> Store = FMantleAssetStore::Create(Settings, MakeShared<FNoFetcher>());
		Store->WaitUntilIdle(30.0f);
		return Store;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleAssetStoreBudgetRuleTest,
	"Mantle.Runtime.AssetStore.BudgetFromTheDrive",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleAssetStoreBudgetRuleTest::RunTest(const FString& Parameters)
{
	using namespace MantleAssetStoreBudgetTestPrivate;
	const FMantleAssetStoreSettings Auto;
	auto Budget = [&Auto](const int64 Free, const int64 Stored, const int64 Chosen = 0, const int64 Demand = 0)
	{
		return FMantleAssetStore::ComputeBudget(Auto, Free, Stored, Chosen, Demand);
	};
	TestEqual(TEXT("sized from the drive by default"), Auto.DiskBudgetBytes, int64(0));
	TestEqual(TEXT("a quarter of 100 GB free"), Budget(100 * GiB, 0), 25 * GiB);
	TestEqual(TEXT("its own files count as room it can use (no shrinking as it fills)"), Budget(60 * GiB, 40 * GiB), 25 * GiB);
	TestEqual(TEXT("at most 64 GB on a big drive"), Budget(1000 * GiB, 0), 64 * GiB);
	TestEqual(TEXT("at least 8 GB"), Budget(20 * GiB, 0), 8 * GiB);
	TestEqual(TEXT("but the drive keeps 10 GB free"), Budget(15 * GiB, 0), 5 * GiB);
	TestEqual(TEXT("and nothing is kept on a nearly full drive"), Budget(6 * GiB, 0), int64(0));
	TestEqual(TEXT("a player's size replaces the quarter"), Budget(1000 * GiB, 0, 128 * GiB), 128 * GiB);
	TestEqual(TEXT("a player's size is held to the free-space floor too"), Budget(50 * GiB, 0, 128 * GiB), 40 * GiB);
	TestEqual(TEXT("a big place raises it past 64 GB"), Budget(1000 * GiB, 0, 0, 100 * GiB), 100 * GiB);
	TestEqual(TEXT("as far as the drive allows"), Budget(50 * GiB, 20 * GiB, 0, 100 * GiB), 60 * GiB);
	TestEqual(TEXT("a small place leaves the quarter as it is"), Budget(100 * GiB, 0, 0, 2 * GiB), 25 * GiB);
	TestEqual(TEXT("the drive not read yet: the smallest size"), Budget(-1, 0), 8 * GiB);
	TestEqual(TEXT("the drive not read yet: a place's need still counts"), Budget(-1, 0, 0, 30 * GiB), 30 * GiB);

	FMantleAssetStoreSettings Fixed;
	Fixed.DiskBudgetBytes = 3 * GiB;
	TestEqual(TEXT("a fixed budget (a tool's, a test's, the ini's) is used as it is"),
		FMantleAssetStore::ComputeBudget(Fixed, 1000 * GiB, 0, 128 * GiB, 100 * GiB), 3 * GiB);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleAssetStoreEvictionTest,
	"Mantle.Runtime.AssetStore.EvictionAndClear",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleAssetStoreEvictionTest::RunTest(const FString& Parameters)
{
	using namespace MantleAssetStoreBudgetTestPrivate;
	AddExpectedError(TEXT("over the 150-byte budget"), EAutomationExpectedErrorFlags::Contains, 0);
	const FString Run = FPaths::AutomationTransientDir() / TEXT("AssetStoreBudget") / FGuid::NewGuid().ToString();
	const FString Sources = Run / TEXT("Sources");
	IFileManager::Get().MakeDirectory(*Sources, true);
	FString PathA, PathB, PathC, PathD, PathE;
	const FMantleStoredAssetRef A = WriteSource(Sources, 1, 100, PathA);
	const FMantleStoredAssetRef B = WriteSource(Sources, 2, 100, PathB);
	const FMantleStoredAssetRef C = WriteSource(Sources, 3, 100, PathC);
	const FMantleStoredAssetRef D = WriteSource(Sources, 4, 100, PathD);
	const FMantleStoredAssetRef E = WriteSource(Sources, 5, 100, PathE);

	{
		// A 150-byte budget. A and B are pinned while they arrive (a place using them), so they stay over it.
		const TSharedRef<FMantleAssetStore> Store = MakeStore(Run / TEXT("Small"), [](FMantleAssetStoreSettings& S) { S.DiskBudgetBytes = 150; });
		Store->Pin(A.Hash);
		Store->Pin(B.Hash);
		TestTrue(TEXT("A is stored"), SeedAndWait(*Store, A, PathA));
		TestTrue(TEXT("B is stored"), SeedAndWait(*Store, B, PathB));

		// The place lets them go (leaving it): nothing is evicted then.
		Store->Unpin(A.Hash);
		Store->Unpin(B.Hash);
		Store->WaitUntilIdle(30.0f);
		TestTrue(TEXT("unpinning evicts nothing: A is still stored"), Store->Contains(A.Hash));
		TestTrue(TEXT("nor B"), Store->Contains(B.Hash));
		TestFalse(TEXT("and neither is pinned"), Store->IsPinned(A.Hash) || Store->IsPinned(B.Hash));

		// A new file needs the room: the least recently used go first, until the store is back under the budget.
		TestTrue(TEXT("C is stored"), SeedAndWait(*Store, C, PathC));
		TestFalse(TEXT("A, the oldest, made room"), Store->Contains(A.Hash));
		TestFalse(TEXT("then B"), Store->Contains(B.Hash));
		TestTrue(TEXT("the new file stays"), Store->Contains(C.Hash));
	}

	{
		// Clear deletes what nothing is using and keeps what is pinned.
		const TSharedRef<FMantleAssetStore> Store = MakeStore(Run / TEXT("Clear"), [](FMantleAssetStoreSettings& S) { S.DiskBudgetBytes = 1 * MiB; });
		Store->Pin(D.Hash);
		TestTrue(TEXT("D is stored"), SeedAndWait(*Store, D, PathD));
		TestTrue(TEXT("E is stored"), SeedAndWait(*Store, E, PathE));
		int64 Freed = -1;
		Store->ClearUnused([&Freed](const int64 Bytes) { Freed = Bytes; });
		Store->WaitUntilIdle(30.0f);
		TestEqual(TEXT("Clear freed E's 100 bytes"), Freed, int64(100));
		TestFalse(TEXT("E is gone"), Store->Contains(E.Hash));
		TestTrue(TEXT("D, pinned, stays"), Store->Contains(D.Hash));
		TestEqual(TEXT("the usage line says what is left"), Store->GetUsage().StoredBytes, int64(100));
		Store->Unpin(D.Hash);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleAssetStorePlaceDemandTest,
	"Mantle.Runtime.AssetStore.BudgetRisesForAPlace",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleAssetStorePlaceDemandTest::RunTest(const FString& Parameters)
{
	using namespace MantleAssetStoreBudgetTestPrivate;
	const FString Root = FPaths::AutomationTransientDir() / TEXT("AssetStoreDemand") / FGuid::NewGuid().ToString();
	// Sized from the drive, but held to 1 MiB so the numbers do not depend on this PC's free space.
	auto Small = [](FMantleAssetStoreSettings& S)
	{
		S.DiskBudgetMinBytes = 1 * MiB;
		S.DiskBudgetMaxBytes = 1 * MiB;
		S.MinFreeDiskBytes = 0;
	};
	{
		const TSharedRef<FMantleAssetStore> Store = MakeStore(Root, Small);
		TestEqual(TEXT("the drive's size, held to 1 MiB"), Store->GetBudgetBytes(), 1 * MiB);
		Store->SetPlaceDemand(300 * MiB);
		Store->WaitUntilIdle(30.0f);
		TestEqual(TEXT("a place that needs 300 MiB raises it"), Store->GetBudgetBytes(), 300 * MiB);
		Store->SetPlaceDemand(10 * MiB);
		Store->WaitUntilIdle(30.0f);
		TestEqual(TEXT("a smaller place after it keeps the big place's room (its files stay for a rejoin)"), Store->GetBudgetBytes(), 300 * MiB);
	}
	{
		const TSharedRef<FMantleAssetStore> Store = MakeStore(Root, Small);
		TestEqual(TEXT("the raised size survives a restart"), Store->GetBudgetBytes(), 300 * MiB);
		Store->ClearUnused(nullptr);
		Store->WaitUntilIdle(30.0f);
		TestEqual(TEXT("Clear forgets it"), Store->GetBudgetBytes(), 1 * MiB);
	}
	{
		const TSharedRef<FMantleAssetStore> Store = MakeStore(Root, Small);
		TestEqual(TEXT("and it stays forgotten"), Store->GetBudgetBytes(), 1 * MiB);
		Store->SetChosenBudget(2 * MiB);
		Store->WaitUntilIdle(30.0f);
		TestEqual(TEXT("a player's size replaces the drive's"), Store->GetBudgetBytes(), 2 * MiB);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
