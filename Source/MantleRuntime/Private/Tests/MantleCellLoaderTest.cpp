// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAssetStore.h"
#include "MantleCellLoader.h"
#include "MantlePlaceCells.h"
#include "MantlePlaceParser.h"

#include "Misc/AutomationTest.h"
#include "Misc/Compression.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

// The cell loader: a place whose scenery lives in cell files is read through the real loader (resolve, store, read tasks) with a fake API and
// a fake transport, and the cells merge into the place. Every way a cell can fail fails the load, naming the cell and the file.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleCellLoaderTestPrivate
{
	FString LoaderTestId(const uint32 Number)
	{
		return FString::Printf(TEXT("ast_u%032x"), Number);
	}

	const FString LoaderMesh = LoaderTestId(1);
	const FString LoaderMaterial = LoaderTestId(2);
	const FString LoaderIndexId = LoaderTestId(3);
	const FString LoaderFileA1 = LoaderTestId(0x10);
	const FString LoaderFileA2 = LoaderTestId(0x11);
	const FString LoaderFileB = LoaderTestId(0x12);
	const FString LoaderFileC = LoaderTestId(0x13);

	TArray<uint8> LoaderPack(const char* Magic, const FString& Json)
	{
		const FTCHARToUTF8 Utf8(*Json);
		TArray<uint8> Packed;
		Packed.SetNumUninitialized(FCompression::CompressMemoryBound(NAME_Zlib, Utf8.Length()));
		int32 PackedSize = Packed.Num();
		verify(FCompression::CompressMemory(NAME_Zlib, Packed.GetData(), PackedSize, Utf8.Get(), Utf8.Length(), COMPRESS_BiasMemory));
		const uint32 RawBytes = static_cast<uint32>(Utf8.Length());
		TArray<uint8> Container;
		Container.Append(reinterpret_cast<const uint8*>(Magic), 4);
		Container.Append(reinterpret_cast<const uint8*>(&RawBytes), sizeof(RawBytes));
		Container.Append(Packed.GetData(), PackedSize);
		return Container;
	}

	FString LoaderStaticMesh(const FString& EntityId, const FString& MeshId = LoaderMesh)
	{
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"type\":\"StaticMesh\",\"transform\":{\"location\":[100,200,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},"
				 "\"properties\":{\"mesh\":\"%s\",\"materials\":[\"%s\"],\"drawDistance\":6000}}"),
			*EntityId, *MeshId, *LoaderMaterial);
	}

	FString LoaderInstanced(const FString& EntityId, const TCHAR* Collision)
	{
		return FString::Printf(
			TEXT("{\"id\":\"%s\",\"type\":\"InstancedMesh\",\"transform\":{\"location\":[80012,-16034,410],\"rotation\":[0,0,0],\"scale\":[1,1,1]},"
				 "\"properties\":{\"mesh\":\"%s\",\"drawDistance\":9000,\"collision\":\"%s\",\"instances\":[[1,2,3,0,90,0,1,1,1],[4,5,6,0,0,0,1,1,1],[7,8,9,0,0,0,1,1,1]]}}"),
			*EntityId, *LoaderMesh, Collision);
	}

	FString LoaderCellFile(const FString& Entities)
	{
		return FString::Printf(TEXT("{\"version\":1,\"entities\":[%s]}"), *Entities);
	}

	FString LoaderFileRef(const FString& Id)
	{
		return FString::Printf(TEXT("{\"id\":\"%s\",\"hash\":\"sha256:dddd\",\"bytes\":10}"), *Id);
	}

	/** Three cells: (0,0,0) with two files, (0,1,0) with one, and (1,0,0) with one. */
	const FString LoaderIndexText = FString::Printf(
		TEXT("{\"version\":1,\"grid\":{\"levels\":[{\"cellSize\":6400,\"maxDrawDistance\":15000},{\"cellSize\":25600,\"maxDrawDistance\":60000}]},\"cells\":["
			 "{\"level\":0,\"x\":0,\"y\":0,\"bounds\":{\"min\":[0,0,0],\"max\":[100,100,100]},\"drawDistance\":9000,\"files\":[%s,%s],\"counts\":{\"entities\":3,\"instances\":3,\"solid\":5}},"
			 "{\"level\":0,\"x\":1,\"y\":0,\"bounds\":{\"min\":[6400,0,0],\"max\":[6500,100,100]},\"drawDistance\":6000,\"files\":[%s],\"counts\":{\"entities\":1,\"instances\":0,\"solid\":1}},"
			 "{\"level\":1,\"x\":0,\"y\":0,\"bounds\":{\"min\":[0,0,0],\"max\":[100,100,100]},\"drawDistance\":9000,\"files\":[%s],\"counts\":{\"entities\":1,\"instances\":3,\"solid\":0}}]}"),
		*LoaderFileRef(LoaderFileA1), *LoaderFileRef(LoaderFileA2), *LoaderFileRef(LoaderFileB), *LoaderFileRef(LoaderFileC));

	/** One asset the fake API knows and the fake transport serves. */
	struct FLoaderServed
	{
		FString Kind;
		TArray<uint8> Bytes;
		FString Status = TEXT("approved");
		bool bServe = true;
	};

	class FLoaderResolver : public IMantleAssetResolver
	{
	public:
		TMap<FString, FLoaderServed> Assets;
		TArray<TArray<FString>> Batches;
		TSet<FString> Unknown;
		TSet<FString> Flaky;
		int32 Asks(const FString& Id) const
		{
			int32 Count = 0;
			for (const TArray<FString>& Batch : Batches)
			{
				Count += Batch.Contains(Id) ? 1 : 0;
			}
			return Count;
		}

		virtual void Resolve(const TArray<FString>& Ids, TFunction<void(const FMantleAssetResolveResult&)> OnDone) override
		{
			Batches.Add(Ids);
			FMantleAssetResolveResult Result;
			Result.bSuccess = true;
			for (const FString& Id : Ids)
			{
				const FLoaderServed* Served = Assets.Find(Id);
				if (!Served || Unknown.Contains(Id))
				{
					Result.Missing.Add(Id);
					continue;
				}
				FMantleUploadedAsset Asset;
				Asset.Id = Id;
				Asset.Kind = Served->Kind;
				Asset.Status = Served->Status;
				Asset.CookState = TEXT("passed");
				Asset.ModerationState = TEXT("passed");
				Asset.Url = TEXT("test://") + Id;
				Asset.CookedHash = FMantleContentHash::OfBytes(Served->Bytes).ToString();
				Asset.CookedBytes = Served->Bytes.Num();
				Result.Assets.Add(Asset);
			}
			OnDone(Result);
		}
	};

	class FLoaderFetcher : public IMantleAssetFetcher
	{
	public:
		TSharedPtr<FLoaderResolver> Source;
		TMap<FString, int32> RefuseFirst;

		virtual void Fetch(const FString& Url, const TSharedRef<FArchive>& Sink, TFunction<void(const FMantleAssetFetchResult&)> OnDone) override
		{
			FMantleAssetFetchResult Result;
			Result.bConnected = true;
			Result.StatusCode = 404;
			const FString Id = Url.RightChop(7);
			const FLoaderServed* Served = Source->Assets.Find(Id);
			if (int32* Left = RefuseFirst.Find(Id); Left && *Left > 0)
			{
				--*Left;
				Result.StatusCode = 403;
			}
			else if (Served && Served->bServe)
			{
				Sink->Serialize(const_cast<uint8*>(Served->Bytes.GetData()), Served->Bytes.Num());
				Result.StatusCode = 200;
			}
			OnDone(Result);
		}
	};

	/** The root place and the fake API/transport that serve its cells. */
	struct FLoaderFixture
	{
		FMantlePlace Root;
		TSharedRef<FLoaderResolver> Resolver = MakeShared<FLoaderResolver>();
		TSharedRef<FLoaderFetcher> Fetcher = MakeShared<FLoaderFetcher>();

		FLoaderFixture()
		{
			const FString Json = FString::Printf(
				TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_cellload01\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
					 "\"metadata\":{\"name\":\"Loader\",\"authorId\":\"usr_4b1d\"},\"settings\":{},"
					 "\"assets\":[{\"id\":\"%s\",\"kind\":\"StaticMesh\",\"hash\":\"sha256:aaaa\",\"bytes\":100},"
					 "{\"id\":\"%s\",\"kind\":\"Material\",\"hash\":\"sha256:bbbb\",\"bytes\":100},"
					 "{\"id\":\"%s\",\"kind\":\"PlaceCellIndex\",\"hash\":\"sha256:cccc\",\"bytes\":50}],"
					 "\"cells\":{\"index\":\"%s\"},"
					 "\"entities\":[{\"id\":\"ent_root_1\",\"type\":\"StaticMesh\",\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},"
					 "\"properties\":{\"mesh\":\"%s\"}}]}"),
				*LoaderMesh, *LoaderMaterial, *LoaderIndexId, *LoaderIndexId, *LoaderMesh);
			const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
			check(Parsed.bSuccess);
			Root = Parsed.Place;
			Fetcher->Source = Resolver;
			Serve(LoaderIndexId, TEXT("cellindex"), LoaderPack("MCI1", LoaderIndexText));
			Serve(LoaderFileA1, TEXT("cell"), LoaderPack("MCF1", LoaderCellFile(LoaderStaticMesh(TEXT("ent_a1")) + TEXT(",") + LoaderStaticMesh(TEXT("ent_a2")))));
			Serve(LoaderFileA2, TEXT("cell"), LoaderPack("MCF1", LoaderCellFile(LoaderInstanced(TEXT("ent_ism_a3"), TEXT("blocking")))));
			Serve(LoaderFileB, TEXT("cell"), LoaderPack("MCF1", LoaderCellFile(LoaderStaticMesh(TEXT("ent_b1")))));
			Serve(LoaderFileC, TEXT("cell"), LoaderPack("MCF1", LoaderCellFile(LoaderInstanced(TEXT("ent_ism_c1"), TEXT("none")))));
		}

		void Serve(const FString& Id, const TCHAR* Kind, TArray<uint8> Bytes)
		{
			FLoaderServed& Served = Resolver->Assets.FindOrAdd(Id);
			Served.Kind = Kind;
			Served.Bytes = MoveTemp(Bytes);
		}

		/** Loads the cells through a store of its own; returns whether the load finished in time. */
		bool Load(FMantleCellLoadResult& Out, int32* OutDoneCalls = nullptr)
		{
			FMantleAssetStoreSettings StoreSettings;
			StoreSettings.RootDir = FPaths::AutomationTransientDir() / FString::Printf(TEXT("CellLoader_%s"), *FGuid::NewGuid().ToString());
			StoreSettings.bAutoPump = false;
			FMantleCellLoaderSettings Settings;
			Settings.bAutoPump = false;
			Settings.Resolver = Resolver;
			Settings.Store = FMantleAssetStore::Create(StoreSettings, Fetcher);
			Settings.MaxResolveAttempts = 2;
			int32 DoneCalls = 0;
			const TSharedRef<FMantleCellLoader> Loader = FMantleCellLoader::Load(Root, Settings, FMantleCellsDone::CreateLambda([&DoneCalls](const FMantleCellLoadResult&) { ++DoneCalls; }));
			const bool bDone = Loader->WaitUntilComplete(30.0f);
			if (OutDoneCalls)
			{
				*OutDoneCalls = DoneCalls;
			}
			Out = Loader->GetResult();
			return bDone;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellLoaderLoadsTest,
	"Mantle.Runtime.Place.Cells.Loader",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellLoaderLoadsTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellLoaderTestPrivate;
	FLoaderFixture Fixture;
	TestTrue(TEXT("a place that names cells needs them"), FMantleCellLoader::NeedsCells(Fixture.Root));

	FMantleCellLoadResult Result;
	int32 DoneCalls = 0;
	if (!TestTrue(TEXT("the load finishes"), Fixture.Load(Result, &DoneCalls)) || !TestTrue(TEXT("and succeeds"), Result.bSuccess))
	{
		AddInfo(Result.ErrorSummary());
		return false;
	}
	TestEqual(TEXT("OnDone fired once"), DoneCalls, 1);
	TestEqual(TEXT("three cells"), Result.Cells.Num(), 3);
	TestEqual(TEXT("four cell files"), Result.Files, 4);
	TestTrue(TEXT("the cells come in the index's order"), Result.Cells[0].CellIndex == 0 && Result.Cells[1].CellIndex == 1 && Result.Cells[2].CellIndex == 2);

	// The index is asked for alone first (its files are not known until it is read), then every file together.
	TestEqual(TEXT("two resolve calls"), Fixture.Resolver->Batches.Num(), 2);
	TestTrue(TEXT("the first asks for the index only"), Fixture.Resolver->Batches[0].Num() == 1 && Fixture.Resolver->Batches[0][0] == LoaderIndexId);
	TestEqual(TEXT("the second asks for the four files"), Fixture.Resolver->Batches[1].Num(), 4);

	// A cell's files are one unit, read in file order.
	TestEqual(TEXT("cell 0 holds three entities"), Result.Cells[0].Entities.Num(), 3);
	TestEqual(TEXT("first file's first"), Result.Cells[0].Entities[0].Id, FString(TEXT("ent_a1")));
	TestEqual(TEXT("first file's second"), Result.Cells[0].Entities[1].Id, FString(TEXT("ent_a2")));
	TestEqual(TEXT("second file's"), Result.Cells[0].Entities[2].Id, FString(TEXT("ent_ism_a3")));
	TestEqual(TEXT("cell 0's instances"), Result.Cells[0].Instances, static_cast<int64>(3));
	TestEqual(TEXT("cell 0's solid: two StaticMeshes and three blocking instances"), Result.Cells[0].Solid, static_cast<int64>(5));
	TestEqual(TEXT("cell 2's solid: instances with no collision"), Result.Cells[2].Solid, static_cast<int64>(0));

	// Merged into the place: the root's entity, then the cells'.
	FMantlePlace Place = Fixture.Root;
	TArray<FMantleParseError> Errors;
	TestTrue(TEXT("the cells merge into the place"), FMantlePlaceCells::Merge(Place, Result.Index, Result.Cells, Errors));
	TestEqual(TEXT("six entities in all"), Place.Entities.Num(), 6);
	TestEqual(TEXT("the last is the third cell's"), Place.Entities.Last().Id, FString(TEXT("ent_ism_c1")));
	TestFalse(TEXT("nothing more to load"), FMantleCellLoader::NeedsCells(Place));

	// A place with no cells needs none.
	TestFalse(TEXT("a place without cells needs none"), FMantleCellLoader::NeedsCells(FMantlePlace()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellLoaderFailuresTest,
	"Mantle.Runtime.Place.Cells.LoaderFailures",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellLoaderFailuresTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellLoaderTestPrivate;

	// A failed load logs one error (MANTLE_PLACE_CELLS FAIL), and the engine one for each stream it cannot inflate.
	AddExpectedError(TEXT("MANTLE_PLACE_CELLS FAIL"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("Failed to uncompress memory"), EAutomationExpectedErrorFlags::Contains, 0);

	// Runs the load of a fixture changed by Change and expects it to fail with a message holding Mentions (and, where given, the file's id).
	const auto Fails = [this](const TCHAR* What, const TFunctionRef<void(FLoaderFixture&)> Change, const FString& Mentions)
	{
		FLoaderFixture Fixture;
		Change(Fixture);
		FMantleCellLoadResult Result;
		TestTrue(FString::Printf(TEXT("%s: the load finishes"), What), Fixture.Load(Result));
		TestFalse(FString::Printf(TEXT("%s: it fails"), What), Result.bSuccess);
		const FString Summary = Result.ErrorSummary();
		TestTrue(FString::Printf(TEXT("%s: the message names it (%s)"), What, *Summary), Summary.Contains(Mentions));
		TestTrue(FString::Printf(TEXT("%s: no cells come back"), What), Result.Cells.IsEmpty());
		return Fixture.Resolver->Batches.Num();
	};

	Fails(TEXT("a damaged cell file"), [](FLoaderFixture& Fixture)
	{
		Fixture.Resolver->Assets[LoaderFileA2].Bytes[Fixture.Resolver->Assets[LoaderFileA2].Bytes.Num() / 2] ^= 0x5A;
	}, LoaderFileA2);
	Fails(TEXT("a cell file that is not a container"), [](FLoaderFixture& Fixture)
	{
		Fixture.Serve(LoaderFileB, TEXT("cell"), TArray<uint8>({ 'n', 'o', 'p', 'e', 0, 0, 0, 0 }));
	}, TEXT("MCF1"));
	Fails(TEXT("a cell file that names an asset the place does not list"), [](FLoaderFixture& Fixture)
	{
		Fixture.Serve(LoaderFileB, TEXT("cell"), LoaderPack("MCF1", LoaderCellFile(LoaderStaticMesh(TEXT("ent_b1"), LoaderTestId(0x99)))));
	}, TEXT("not in the place's assets"));
	Fails(TEXT("a cell file with another version"), [](FLoaderFixture& Fixture)
	{
		Fixture.Serve(LoaderFileB, TEXT("cell"), LoaderPack("MCF1", LoaderCellFile(LoaderStaticMesh(TEXT("ent_b1"))).Replace(TEXT("\"version\":1"), TEXT("\"version\":2"))));
	}, TEXT("version 2"));
	Fails(TEXT("a cell file the API does not know"), [](FLoaderFixture& Fixture)
	{
		Fixture.Resolver->Unknown.Add(LoaderFileC);
	}, TEXT("does not exist"));
	Fails(TEXT("a cell file the transport does not serve"), [](FLoaderFixture& Fixture)
	{
		Fixture.Resolver->Assets[LoaderFileB].bServe = false;
	}, TEXT("could not be downloaded"));
	Fails(TEXT("a cell file that is another kind of asset"), [](FLoaderFixture& Fixture)
	{
		Fixture.Resolver->Assets[LoaderFileB].Kind = TEXT("mesh");
	}, TEXT("is a mesh, not a cell"));
	Fails(TEXT("a rejected cell file"), [](FLoaderFixture& Fixture)
	{
		Fixture.Resolver->Assets[LoaderFileC].Status = TEXT("rejected");
	}, TEXT("rejected"));
	Fails(TEXT("a cell file that is not approved yet"), [](FLoaderFixture& Fixture)
	{
		Fixture.Resolver->Assets[LoaderFileC].Status = TEXT("processing");
	}, TEXT("not approved"));
	Fails(TEXT("a cell index that is another kind of asset"), [](FLoaderFixture& Fixture)
	{
		Fixture.Resolver->Assets[LoaderIndexId].Kind = TEXT("cell");
	}, TEXT("is a cell, not a cellindex"));
	Fails(TEXT("a cell index that is a cell file"), [](FLoaderFixture& Fixture)
	{
		Fixture.Serve(LoaderIndexId, TEXT("cellindex"), LoaderPack("MCF1", LoaderCellFile(LoaderStaticMesh(TEXT("ent_b1")))));
	}, TEXT("MCI1"));
	Fails(TEXT("a cell index the API does not know"), [](FLoaderFixture& Fixture)
	{
		Fixture.Resolver->Unknown.Add(LoaderIndexId);
	}, LoaderIndexId);
	Fails(TEXT("a cell index with another version"), [](FLoaderFixture& Fixture)
	{
		Fixture.Serve(LoaderIndexId, TEXT("cellindex"), LoaderPack("MCI1", LoaderIndexText.Replace(TEXT("\"version\":1"), TEXT("\"version\":9"))));
	}, TEXT("version 9"));
	Fails(TEXT("a cell index that lists its own id as a file"), [](FLoaderFixture& Fixture)
	{
		Fixture.Serve(LoaderIndexId, TEXT("cellindex"), LoaderPack("MCI1", LoaderIndexText.Replace(*LoaderFileC, *LoaderIndexId)));
	}, TEXT("the cell index itself"));

	// A signed URL that expired (a 403 once) is resolved again, once, and the load goes on.
	{
		FLoaderFixture Fixture;
		Fixture.Fetcher->RefuseFirst.Add(LoaderFileB, 1);
		FMantleCellLoadResult Result;
		TestTrue(TEXT("an expired URL: the load finishes"), Fixture.Load(Result));
		TestTrue(TEXT("and succeeds"), Result.bSuccess);
		TestEqual(TEXT("the file was resolved twice"), Fixture.Resolver->Asks(LoaderFileB), 2);
		TestEqual(TEXT("and the others once"), Fixture.Resolver->Asks(LoaderFileA1), 1);
	}
	// Twice is not a flaky URL, it is a refusal.
	{
		FLoaderFixture Fixture;
		Fixture.Fetcher->RefuseFirst.Add(LoaderFileB, 2);
		FMantleCellLoadResult Result;
		TestTrue(TEXT("a file refused twice: the load finishes"), Fixture.Load(Result));
		TestFalse(TEXT("and fails"), Result.bSuccess);
		TestEqual(TEXT("after resolving it twice"), Fixture.Resolver->Asks(LoaderFileB), 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellLoaderStreamTest,
	"Mantle.Runtime.Place.Cells.LoaderStream",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellLoaderStreamTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellLoaderTestPrivate;
	FLoaderFixture Fixture;

	FMantleAssetStoreSettings StoreSettings;
	StoreSettings.RootDir = FPaths::AutomationTransientDir() / FString::Printf(TEXT("CellLoaderStream_%s"), *FGuid::NewGuid().ToString());
	StoreSettings.bAutoPump = false;
	FMantleCellLoaderSettings Settings;
	Settings.bAutoPump = false;
	Settings.Resolver = Fixture.Resolver;
	Settings.Store = FMantleAssetStore::Create(StoreSettings, Fixture.Fetcher);
	Settings.MaxResolveAttempts = 2;

	int32 IndexCalls = 0;
	const TSharedRef<FMantleCellLoader> Loader = FMantleCellLoader::Open(Fixture.Root, Settings, FMantleCellIndexReady::CreateLambda([&IndexCalls]() { ++IndexCalls; }));
	const auto PumpUntil = [&](const TFunction<bool()>& Done)
	{
		const double Deadline = FPlatformTime::Seconds() + 30.0;
		while (!Done())
		{
			if (FPlatformTime::Seconds() > Deadline)
			{
				return false;
			}
			Settings.Store->Pump();
			Loader->Pump();
			FPlatformProcess::Sleep(0.001f);
		}
		return true;
	};

	// Only the index is read until a cell is asked for.
	if (!TestTrue(TEXT("the index is read"), PumpUntil([&]() { return Loader->IsIndexReady() && IndexCalls > 0; })))
	{
		return false;
	}
	TestEqual(TEXT("the index callback fired once"), IndexCalls, 1);
	TestEqual(TEXT("three cells"), Loader->GetIndex().Cells.Num(), 3);
	TestEqual(TEXT("only the index was resolved"), Fixture.Resolver->Batches.Num(), 1);
	TestFalse(TEXT("no cell is ready"), Loader->IsCellReady(1));

	// Cell 1 (one file) is asked for: only its file is resolved and fetched, nothing of the others.
	Loader->SetWanted(TArray<int32>{ 1 });
	if (!TestTrue(TEXT("cell 1 is read"), PumpUntil([&]() { return Loader->IsCellReady(1); })))
	{
		return false;
	}
	TestEqual(TEXT("one more resolve"), Fixture.Resolver->Batches.Num(), 2);
	TestTrue(TEXT("for cell 1's file alone"), Fixture.Resolver->Batches[1].Num() == 1 && Fixture.Resolver->Batches[1][0] == LoaderFileB);
	TestEqual(TEXT("cell 0's files never asked for"), Fixture.Resolver->Asks(LoaderFileA1) + Fixture.Resolver->Asks(LoaderFileA2), 0);
	FMantleCellContent Taken;
	TestTrue(TEXT("cell 1 is taken"), Loader->TakeCell(1, Taken));
	TestEqual(TEXT("its one entity"), Taken.Entities.Num(), 1);
	TestEqual(TEXT("its id"), Taken.Entities[0].Id, FString(TEXT("ent_b1")));
	TestEqual(TEXT("it says which cell"), Taken.CellIndex, 1);
	TestFalse(TEXT("taken, it is not ready again"), Loader->IsCellReady(1));
	FMantleCellContent Again;
	TestFalse(TEXT("and cannot be taken twice"), Loader->TakeCell(1, Again));

	// Two cells, the one with two files first: its files are one unit, in file order.
	Loader->SetWanted(TArray<int32>{ 0, 2 });
	if (!TestTrue(TEXT("cells 0 and 2 are read"), PumpUntil([&]() { return Loader->IsCellReady(0) && Loader->IsCellReady(2); })))
	{
		return false;
	}
	FMantleCellContent Zero;
	TestTrue(TEXT("cell 0 is taken"), Loader->TakeCell(0, Zero));
	TestEqual(TEXT("three entities from two files"), Zero.Entities.Num(), 3);
	TestEqual(TEXT("in file order"), Zero.Entities[2].Id, FString(TEXT("ent_ism_a3")));
	TestEqual(TEXT("counted as the index counts"), Zero.Solid, static_cast<int64>(5));

	// Not wanted any more: dropped, not taken. Wanted again, it is fetched from the store without being resolved a second time (its URL is still good).
	Loader->SetWanted(TArray<int32>());
	TestFalse(TEXT("a cell nobody wants is not handed over"), Loader->IsCellReady(2) && Loader->TakeCell(2, Again));
	const int32 ResolvesBefore = Fixture.Resolver->Batches.Num();
	Loader->SetWanted(TArray<int32>{ 1 });
	TestTrue(TEXT("cell 1 comes back"), PumpUntil([&]() { return Loader->IsCellReady(1); }));
	TestEqual(TEXT("without resolving it again"), Fixture.Resolver->Batches.Num(), ResolvesBefore);
	TestTrue(TEXT("and can be taken"), Loader->TakeCell(1, Again));

	// A cell that cannot be had loses that cell only: its file is unknown to the API, the cell next to it reads as before.
	{
		FLoaderFixture Broken;
		Broken.Resolver->Unknown.Add(LoaderFileC);
		FMantleCellLoaderSettings BrokenSettings = Settings;
		BrokenSettings.Resolver = Broken.Resolver;
		BrokenSettings.Store = FMantleAssetStore::Create(StoreSettings, Broken.Fetcher);
		const TSharedRef<FMantleCellLoader> Other = FMantleCellLoader::Open(Broken.Root, BrokenSettings, FMantleCellIndexReady());
		const auto PumpOther = [&](const TFunction<bool()>& Done)
		{
			const double Deadline = FPlatformTime::Seconds() + 30.0;
			while (!Done() && FPlatformTime::Seconds() < Deadline)
			{
				BrokenSettings.Store->Pump();
				Other->Pump();
				FPlatformProcess::Sleep(0.001f);
			}
			return Done();
		};
		TestTrue(TEXT("the index is read"), PumpOther([&]() { return Other->IsIndexReady(); }));
		Other->SetWanted(TArray<int32>{ 0, 2 });
		AddExpectedError(TEXT("MANTLE_PLACE_CELLS FAIL"), EAutomationExpectedErrorFlags::Contains, 1);
		TestTrue(TEXT("cell 2 fails and cell 0 is read"), PumpOther([&]() { return Other->IsCellFailed(2) && Other->IsCellReady(0); }));
		TestFalse(TEXT("the loader does not fail"), Other->HasFailed());
		TestFalse(TEXT("a failed cell is never ready"), Other->IsCellReady(2));
		FMantleCellContent InBroken;
		TestFalse(TEXT("and cannot be taken"), Other->TakeCell(2, InBroken));
		TestTrue(TEXT("while the other is"), Other->TakeCell(0, InBroken));
	}

	// A caller that has the index already does not fetch it again.
	{
		FLoaderFixture Second;
		FMantleCellLoaderSettings SecondSettings = Settings;
		SecondSettings.Resolver = Second.Resolver;
		SecondSettings.Store = FMantleAssetStore::Create(StoreSettings, Second.Fetcher);
		int32 Calls = 0;
		const FMantleCellIndex Known = Loader->GetIndex();
		const TSharedRef<FMantleCellLoader> Quick = FMantleCellLoader::Open(Second.Root, SecondSettings, FMantleCellIndexReady::CreateLambda([&Calls]() { ++Calls; }), &Known);
		TestEqual(TEXT("not from inside Open"), Calls, 0);
		Quick->Pump();
		TestEqual(TEXT("from its first Pump"), Calls, 1);
		TestEqual(TEXT("the index is known"), Quick->GetIndex().Cells.Num(), 3);
		TestEqual(TEXT("and was not asked for"), Second.Resolver->Batches.Num(), 0);
		Quick->SetWanted(TArray<int32>{ 1 });
		const double Deadline = FPlatformTime::Seconds() + 30.0;
		while (!Quick->IsCellReady(1) && FPlatformTime::Seconds() < Deadline)
		{
			SecondSettings.Store->Pump();
			Quick->Pump();
			FPlatformProcess::Sleep(0.001f);
		}
		TestTrue(TEXT("a cell reads from a known index"), Quick->IsCellReady(1));
	}

	// A cell dropped while its file is being fetched comes off the count once, even when the store's answer was already on its way: the answer that
	// arrives for it afterwards is not taken for a fetch of the file again, and the count never goes below what is really in flight.
	{
		FLoaderFixture Third;
		FMantleCellLoaderSettings ThirdSettings = Settings;
		ThirdSettings.Resolver = Third.Resolver;
		ThirdSettings.Store = FMantleAssetStore::Create(StoreSettings, Third.Fetcher);
		const TSharedRef<FMantleCellLoader> Dropping = FMantleCellLoader::Open(Third.Root, ThirdSettings, FMantleCellIndexReady());
		const double Deadline = FPlatformTime::Seconds() + 30.0;
		while (!Dropping->IsIndexReady() && FPlatformTime::Seconds() < Deadline)
		{
			ThirdSettings.Store->Pump();
			Dropping->Pump();
			FPlatformProcess::Sleep(0.001f);
		}
		Dropping->SetWanted(TArray<int32>{ 1 });
		// Until the request is out (its one file resolved, then fetched), and no further: the answer is not taken up by the loader.
		while (Dropping->GetFetchesInFlight() == 0 && !Dropping->IsCellReady(1) && FPlatformTime::Seconds() < Deadline)
		{
			Dropping->Pump();
			FPlatformProcess::Sleep(0.001f);
		}
		TestEqual(TEXT("one fetch in flight"), Dropping->GetFetchesInFlight(), 1);
		// The store answers (its pump runs the fake transport); the loader has not looked at the answer yet.
		for (int32 Pass = 0; Pass < 50; ++Pass)
		{
			ThirdSettings.Store->Pump();
			FPlatformProcess::Sleep(0.002f);
		}
		Dropping->SetWanted(TArray<int32>());
		TestEqual(TEXT("dropped: off the count"), Dropping->GetFetchesInFlight(), 0);
		for (int32 Pass = 0; Pass < 20; ++Pass)
		{
			ThirdSettings.Store->Pump();
			Dropping->Pump();
			FPlatformProcess::Sleep(0.002f);
		}
		TestEqual(TEXT("the late answer did not take it off again"), Dropping->GetFetchesInFlight(), 0);
		TestFalse(TEXT("and the dropped cell was not delivered"), Dropping->IsCellReady(1));
		// Wanted again, it is fetched and read as if nothing had happened.
		Dropping->SetWanted(TArray<int32>{ 1 });
		while (!Dropping->IsCellReady(1) && FPlatformTime::Seconds() < Deadline)
		{
			ThirdSettings.Store->Pump();
			Dropping->Pump();
			FPlatformProcess::Sleep(0.001f);
		}
		TestTrue(TEXT("wanted again, it is read"), Dropping->IsCellReady(1));
		TestEqual(TEXT("and nothing is in flight"), Dropping->GetFetchesInFlight(), 0);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
