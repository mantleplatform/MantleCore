// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAssetStore.h"
#include "MantleCellArea.h"
#include "MantleCellStreamer.h"
#include "MantlePhysicsProp.h"
#include "MantlePlaceCells.h"
#include "MantlePlaceParser.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/Compression.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"

#if WITH_DEV_AUTOMATION_TESTS

// The cell streamer in a world: a row of twelve cells 64 m apart (each holding three cubes and a list of 250 solid instances), read through the real loader
// from a fake API and transport, built into a real world. The view moves along the row: the cells within drawDistance of it are built and nothing else is
// fetched, a cell is released only well after the view leaves it, a jump holds until the new spot stands, the frame's budget bounds the work, and a cell that cannot
// be read does not hold anyone.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleCellStreamerTestPrivate
{
	constexpr int32 CellCount = 12;
	constexpr double CellSize = 6400.0;
	constexpr double DrawDistance = 15000.0;
	constexpr int32 ActorsPerCell = 4; // three cubes and the list of 250 (built as three pieces of one component)

	FString TestId(const uint32 Number)
	{
		return FString::Printf(TEXT("ast_u%032x"), Number);
	}

	const FString IndexId = TestId(1);
	FString FileId(const int32 Cell) { return TestId(0x100 + Cell); }

	TArray<uint8> Pack(const char* Magic, const FString& Json)
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

	/** One cell file: three cubes and an InstancedMesh of 250 solid instances anchored at the cell's start. */
	FString CellFile(const int32 Cell, const bool bPicture = false)
	{
		const int64 MinX = static_cast<int64>(Cell * CellSize);
		FString Entities;
		for (int32 Cube = 0; Cube < 3; ++Cube)
		{
			Entities += FString::Printf(
				TEXT("{\"id\":\"ent_c%d_%d\",\"type\":\"StaticMesh\",\"transform\":{\"location\":[%lld,100,100],\"rotation\":[0,0,0],\"scale\":[1,1,1]},"
					 "\"properties\":{\"mesh\":\"ast_cube\",\"collision\":\"blocking\",\"drawDistance\":15000}},"),
				Cell, Cube, MinX + 500 + Cube * 1000);
		}
		FString Instances;
		for (int32 Instance = 0; Instance < 250; ++Instance)
		{
			Instances += FString::Printf(TEXT("%s[%d,%d,0,0,0,0,1,1,1]"), Instance > 0 ? TEXT(",") : TEXT(""), (Instance % 50) * 100, (Instance / 50) * 100 + 2000);
		}
		Entities += FString::Printf(
			TEXT("{\"id\":\"ent_ism_c%d\",\"type\":\"InstancedMesh\",\"transform\":{\"location\":[%lld,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},"
				 "\"properties\":{\"mesh\":\"ast_cube\",\"collision\":\"blocking\",\"drawDistance\":15000,\"instances\":[%s]}}"),
			Cell, MinX, *Instances);
		if (bPicture)
		{
			// A model with no collision: only to be looked at.
			Entities += FString::Printf(
				TEXT(",{\"id\":\"ent_pic_c%d\",\"type\":\"StaticMesh\",\"transform\":{\"location\":[%lld,3000,100],\"rotation\":[0,0,0],\"scale\":[1,1,1]},"
					 "\"properties\":{\"mesh\":\"ast_cube\",\"collision\":\"none\",\"drawDistance\":15000}}"),
				Cell, MinX + 200);
		}
		return FString::Printf(TEXT("{\"version\":1,\"entities\":[%s]}"), *Entities);
	}

	/** A cell file with one picture (no collision) and nothing solid. */
	FString PictureOnlyCellFile(const int32 Cell)
	{
		return FString::Printf(
			TEXT("{\"version\":1,\"entities\":[{\"id\":\"ent_pic_only_c%d\",\"type\":\"StaticMesh\",\"transform\":{\"location\":[%lld,3000,100],\"rotation\":[0,0,0],\"scale\":[1,1,1]},"
				 "\"properties\":{\"mesh\":\"ast_cube\",\"collision\":\"none\",\"drawDistance\":15000}}]}"),
			Cell, static_cast<int64>(Cell * CellSize) + 200);
	}

	FString IndexText(const TSet<int32>& PictureOnly = TSet<int32>())
	{
		FString Cells;
		for (int32 Cell = 0; Cell < CellCount; ++Cell)
		{
			const bool bPictureOnly = PictureOnly.Contains(Cell);
			Cells += FString::Printf(
				TEXT("%s{\"level\":0,\"x\":%d,\"y\":0,\"bounds\":{\"min\":[%.1f,0,0],\"max\":[%.1f,6400,500]},\"drawDistance\":15000,"
					 "\"files\":[{\"id\":\"%s\",\"hash\":\"sha256:dddd\",\"bytes\":10}],\"counts\":{\"entities\":%d,\"instances\":%d,\"solid\":%d}}"),
				Cell > 0 ? TEXT(",") : TEXT(""), Cell, Cell * CellSize, (Cell + 1) * CellSize, *FileId(Cell), bPictureOnly ? 1 : 4, bPictureOnly ? 0 : 250, bPictureOnly ? 0 : 253);
		}
		return FString::Printf(TEXT("{\"version\":1,\"grid\":{\"levels\":[{\"cellSize\":6400,\"maxDrawDistance\":15000}]},\"cells\":[%s]}"), *Cells);
	}

	struct FServed
	{
		FString Kind;
		TArray<uint8> Bytes;
	};

	class FResolver : public IMantleAssetResolver
	{
	public:
		TMap<FString, FServed> Assets;
		TSet<FString> Unknown;
		TSet<FString> Asked;

		virtual void Resolve(const TArray<FString>& Ids, TFunction<void(const FMantleAssetResolveResult&)> OnDone) override
		{
			FMantleAssetResolveResult Result;
			Result.bSuccess = true;
			for (const FString& Id : Ids)
			{
				Asked.Add(Id);
				const FServed* Served = Assets.Find(Id);
				if (!Served || Unknown.Contains(Id))
				{
					Result.Missing.Add(Id);
					continue;
				}
				FMantleUploadedAsset Asset;
				Asset.Id = Id;
				Asset.Kind = Served->Kind;
				Asset.Status = TEXT("approved");
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

	class FFetcher : public IMantleAssetFetcher
	{
	public:
		TSharedPtr<FResolver> Source;

		virtual void Fetch(const FString& Url, const TSharedRef<FArchive>& Sink, TFunction<void(const FMantleAssetFetchResult&)> OnDone) override
		{
			FMantleAssetFetchResult Result;
			Result.bConnected = true;
			Result.StatusCode = 404;
			if (const FServed* Served = Source->Assets.Find(Url.RightChop(7)))
			{
				Sink->Serialize(const_cast<uint8*>(Served->Bytes.GetData()), Served->Bytes.Num());
				Result.StatusCode = 200;
			}
			OnDone(Result);
		}
	};

	/** Where a cell's assets come from, for a test: what was asked for and given back, and what it says (all ready, none yet, or some failed). */
	class FFakeCellAssets : public IMantleCellAssets
	{
	public:
		TArray<int32> Acquired;
		TArray<int32> Released;
		TSet<int32> Held;
		/** False: every asset is still on its way. */
		bool bReady = true;
		/** Ids that fail once the assets are ready. */
		TArray<FString> Failing;

		virtual void Acquire(int32 CellIndex, const TArray<FMantleEntity>& Entities) override
		{
			Acquired.Add(CellIndex);
			Held.Add(CellIndex);
			EntitiesAsked += Entities.Num();
		}

		virtual void GetState(int32 CellIndex, TArray<FString>& OutPending, TArray<FString>& OutFailed) override
		{
			OutPending.Reset();
			OutFailed.Reset();
			if (!bReady)
			{
				OutPending.Add(TEXT("ast_cube"));
				return;
			}
			OutFailed = Failing;
		}

		virtual void Release(int32 CellIndex) override
		{
			if (Held.Remove(CellIndex) > 0)
			{
				Released.Add(CellIndex);
			}
		}

		virtual void SetBuildSlice(double Seconds) override
		{
			LastSlice = Seconds;
			LargestSlice = FMath::Max(LargestSlice, Seconds);
		}

		double LastSlice = -1.0;
		double LargestSlice = 0.0;

		int32 EntitiesAsked = 0;
	};

	/** A world, the fake service, and a streamer over them, ticked by hand. */
	struct FFixture
	{
		UWorld* World = nullptr;
		TSharedRef<FResolver> Resolver = MakeShared<FResolver>();
		TSharedRef<FFetcher> Fetcher = MakeShared<FFetcher>();
		TSharedPtr<FMantleAssetStore> Store;
		FMantlePlace Root;
		TSharedPtr<FMantleCellStreamer> Streamer;
		/** Given to the streamer when it starts (a test that has the cells' assets come from somewhere). */
		TSharedPtr<FFakeCellAssets> CellAssets;
		FVector View = FVector(100.0, 100.0, 100.0);
		FVector Velocity = FVector::ZeroVector;
		int32 BaselineActors = 0;
		TArray<bool> HoldCalls;
		bool bProviderHasView = true;
		/** The extra views the provider gives (the camera, when it is not at the player). */
		TArray<FMantleCellStreamer::FExtraView> Extras;
		/** What the streamer told about entities: ids of the pieces' entities as they stood, and as they were released. */
		TArray<FString> BuiltIds;
		TArray<FString> ReleasedIds;
		/** A second streamer on the same world and service (a standalone game's solid streamer), ticked with the first. */
		TSharedPtr<FMantleCellStreamer> Other;
		TArray<bool> OtherHoldCalls;

		/** PictureOnly: cells whose file holds one picture and nothing solid (the index says so). */
		explicit FFixture(const bool bPictures = false, const TSet<int32>& PictureOnly = TSet<int32>())
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(*FString::Printf(TEXT("MantleCellStreamerTestWorld_%s"), *FGuid::NewGuid().ToString())));
			Fetcher->Source = Resolver;
			Resolver->Assets.Add(IndexId, FServed{ TEXT("cellindex"), Pack("MCI1", IndexText(PictureOnly)) });
			for (int32 Cell = 0; Cell < CellCount; ++Cell)
			{
				Resolver->Assets.Add(FileId(Cell), FServed{ TEXT("cell"), Pack("MCF1", PictureOnly.Contains(Cell) ? PictureOnlyCellFile(Cell) : CellFile(Cell, bPictures)) });
			}
			const FString Json = FString::Printf(
				TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_cellstream01\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
					 "\"metadata\":{\"name\":\"Row\",\"authorId\":\"usr_4b1d\"},\"settings\":{},"
					 "\"assets\":[{\"id\":\"%s\",\"kind\":\"PlaceCellIndex\",\"hash\":\"sha256:cccc\",\"bytes\":50}],"
					 "\"cells\":{\"index\":\"%s\"},\"entities\":[]}"),
				*IndexId, *IndexId);
			const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
			check(Parsed.bSuccess);
			Root = Parsed.Place;
			Root.bCellsStreamed = true;
			FMantleAssetStoreSettings StoreSettings;
			StoreSettings.RootDir = FPaths::AutomationTransientDir() / FString::Printf(TEXT("CellStreamer_%s"), *FGuid::NewGuid().ToString());
			StoreSettings.bAutoPump = false;
			Store = FMantleAssetStore::Create(StoreSettings, Fetcher);
			BaselineActors = CountActors();
		}

		~FFixture()
		{
			if (Other.IsValid())
			{
				Other->Stop();
			}
			if (Streamer.IsValid())
			{
				Streamer->Stop();
			}
			if (World)
			{
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
				if (World->IsRooted())
				{
					World->RemoveFromRoot();
				}
			}
		}

		int32 CountActors() const
		{
			int32 Count = 0;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				++Count;
			}
			return Count;
		}

		int32 StreamedActors() const { return CountActors() - BaselineActors; }

		/** The instances of every instanced mesh in the world. */
		int32 CountInstances() const
		{
			int32 Count = 0;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (const UInstancedStaticMeshComponent* Instances = It->FindComponentByClass<UInstancedStaticMeshComponent>())
				{
					Count += Instances->GetInstanceCount();
				}
			}
			return Count;
		}

		void StartStreamer(FMantleCellStreamSettings Settings)
		{
			Settings.bAutoTick = false;
			FMantleCellLoaderSettings LoaderSettings;
			LoaderSettings.bAutoPump = false;
			LoaderSettings.Resolver = Resolver;
			LoaderSettings.Store = Store;
			LoaderSettings.MaxResolveAttempts = 2;
			Streamer = FMantleCellStreamer::Start(World, Root, nullptr, LoaderSettings,
				[this](FVector& OutLocation)
				{
					OutLocation = View;
					return bProviderHasView;
				},
				View, Settings);
			if (CellAssets.IsValid())
			{
				Streamer->SetCellAssets(CellAssets);
			}
			Streamer->SetEntityCallbacks(
				[this](const FString& EntityId, const FString&, AActor*) { BuiltIds.Add(EntityId); },
				[this](const FString& EntityId) { ReleasedIds.Add(EntityId); });
			Streamer->SetExtraViewsProvider([this](TArray<FMantleCellStreamer::FExtraView>& OutViews) { OutViews = Extras; });
			// The join's hold began inside Start, before the callback could be set: the owner reads it once, as UMantleCellStreamSubsystem does.
			Streamer->SetHoldChangedCallback([this](const bool bHolding) { HoldCalls.Add(bHolding); });
			HoldCalls.Add(Streamer->IsHolding());
		}

		void StartOther(FMantleCellStreamSettings Settings)
		{
			Settings.bAutoTick = false;
			FMantleCellLoaderSettings LoaderSettings;
			LoaderSettings.bAutoPump = false;
			LoaderSettings.Resolver = Resolver;
			LoaderSettings.Store = Store;
			LoaderSettings.MaxResolveAttempts = 2;
			Other = FMantleCellStreamer::Start(World, Root, nullptr, LoaderSettings,
				[this](FVector& OutLocation)
				{
					OutLocation = View;
					return bProviderHasView;
				},
				View, Settings);
			Other->SetHoldChangedCallback([this](const bool bHolding) { OtherHoldCalls.Add(bHolding); });
			OtherHoldCalls.Add(Other->IsHolding());
		}

		/** One tick of the streamer (at 60 frames a second unless told otherwise), the view moving by Velocity as it would in that time. */
		void Tick(const double DeltaSeconds = 1.0 / 60.0)
		{
			View += Velocity * DeltaSeconds;
			Store->Pump();
			Streamer->Tick(DeltaSeconds);
			if (Other.IsValid())
			{
				Other->Tick(DeltaSeconds);
			}
			// The cell files are read on worker tasks: give them real time between ticks.
			FPlatformProcess::Sleep(0.001f);
		}

		/** Ticks until the streamer has nothing left to read, build or take down (or 2,000 ticks). */
		bool Settle(const int32 MaxTicks = 2000)
		{
			int32 SettledTicks = 0;
			for (int32 Tick = 0; Tick < MaxTicks; ++Tick)
			{
				this->Tick();
				const FMantleCellStreamStats& S = Streamer->GetStats();
				SettledTicks = (S.CellsPending == 0 && S.CellsLoading == 0 && S.CellsAssetWait == 0 && S.CellsReady == 0 && S.CellsRemoving == 0 && !Streamer->IsHolding()) ? SettledTicks + 1 : 0;
				if (SettledTicks >= 5)
				{
					return true;
				}
			}
			return false;
		}

		bool Built(const int32 Cell) const { return Streamer->IsCellBuilt(Cell); }
	};

	FMantleCellStreamSettings SteadySettings()
	{
		FMantleCellStreamSettings Settings;
		// No look-ahead: these tests move the view by jumps and look at where it stands.
		Settings.LookAheadSeconds = 0.0;
		Settings.HoldRadiusCm = 10000.0;
		return Settings;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerFollowsTheViewTest,
	"Mantle.Runtime.CellStream.Streamer.FollowsTheView",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerFollowsTheViewTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	FFixture Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	Fixture.StartStreamer(SteadySettings());
	TestTrue(TEXT("the join holds from the start"), Fixture.Streamer->IsHolding());

	// From x = 1 m: cells whose box is within 150 m, which are cells 0, 1 and 2 (their boxes start at 0, 64 and 128 m); cell 3 starts at 192 m.
	if (!TestTrue(TEXT("the streamer settles"), Fixture.Settle()))
	{
		AddInfo(FString::Printf(TEXT("pending %d loading %d ready %d"), Fixture.Streamer->GetStats().CellsPending, Fixture.Streamer->GetStats().CellsLoading, Fixture.Streamer->GetStats().CellsReady));
		return false;
	}
	TestEqual(TEXT("the join's hold began and ended"), Fixture.HoldCalls.Num(), 2);
	TestTrue(TEXT("true then false"), Fixture.HoldCalls.Num() == 2 && Fixture.HoldCalls[0] && !Fixture.HoldCalls[1]);
	TestTrue(TEXT("cells 0, 1 and 2 stand"), Fixture.Built(0) && Fixture.Built(1) && Fixture.Built(2));
	TestFalse(TEXT("cell 3 does not"), Fixture.Built(3));
	TestEqual(TEXT("three cells"), Fixture.Streamer->GetStats().CellsBuilt, 3);
	TestEqual(TEXT("four actors a cell: three cubes and the list, whose three pieces went into one component"), Fixture.StreamedActors(), 3 * ActorsPerCell);
	TestEqual(TEXT("eighteen pieces built"), Fixture.Streamer->GetStats().UnitsBuilt, static_cast<int64>(18));
	TestEqual(TEXT("with 750 instances"), Fixture.Streamer->GetStats().InstancesBuilt, static_cast<int64>(750));
	// The list of 250 was built in three steps, and ends as one component with every instance: 250 in each of the three cells.
	TestEqual(TEXT("every instance of every list is in its component"), Fixture.CountInstances(), 750);
	// Each entity was told once as it stood, the list by its own id (not by a piece's): three cubes and the list in each of the three cells.
	TestEqual(TEXT("twelve entities stood"), Fixture.BuiltIds.Num(), 12);
	TestEqual(TEXT("the list of cell 0 once, by its id"), Fixture.BuiltIds.FilterByPredicate([](const FString& Id) { return Id == TEXT("ent_ism_c0"); }).Num(), 1);
	TestTrue(TEXT("nothing was released yet"), Fixture.ReleasedIds.IsEmpty());

	// Nothing of the far cells was asked for: the files of cells 3 and up were never resolved.
	for (int32 Cell = 3; Cell < CellCount; ++Cell)
	{
		TestFalse(FString::Printf(TEXT("cell %d's file was never asked for"), Cell), Fixture.Resolver->Asked.Contains(FileId(Cell)));
	}

	// Moving 220 m on (a jump, so the streamer holds meanwhile; the cells it releases are the same): cell 0 is now 156 m off, past its 150 m but inside the 180 m it is kept to. It stays; cell 5 (320 m) is 100 m away and comes.
	Fixture.View = FVector(22000.0, 100.0, 100.0);
	Fixture.Settle();
	TestTrue(TEXT("cell 0 is kept inside the hysteresis band"), Fixture.Built(0));
	TestTrue(TEXT("cell 5 is built"), Fixture.Built(5));

	// 30 m farther: cell 0 is 186 m off, past 180 m. It comes down and its actors with it.
	Fixture.View = FVector(25000.0, 100.0, 100.0);
	Fixture.Settle();
	TestFalse(TEXT("cell 0 is released"), Fixture.Built(0));
	TestTrue(TEXT("cell 1 (122 m off) is kept"), Fixture.Built(1));
	// From 250 m: cells 1 (ends at 128 m) to 6 (starts at 384 m, 134 m off) are wanted; 7 starts at 448 m, 198 m off.
	TestTrue(TEXT("cell 6 is built"), Fixture.Built(6));
	TestFalse(TEXT("cell 7 is not"), Fixture.Built(7));
	TestEqual(TEXT("six cells stand, with their actors"), Fixture.StreamedActors(), 6 * ActorsPerCell);
	TestTrue(TEXT("taking cell 0 down removed its actors"), Fixture.Streamer->GetStats().ActorsRemoved >= ActorsPerCell);
	TestEqual(TEXT("and told of its four entities, the list by its id"), Fixture.ReleasedIds.Num(), 4);
	TestTrue(TEXT("one of them cell 0's list"), Fixture.ReleasedIds.Contains(TEXT("ent_ism_c0")));
	TestEqual(TEXT("one cell released"), Fixture.Streamer->GetStats().CellsReleasedEver, static_cast<int64>(Fixture.Streamer->GetStats().CellsBuiltEver - Fixture.Streamer->GetStats().CellsBuilt));

	// Stop takes down everything it made.
	Fixture.Streamer->Stop();
	TestEqual(TEXT("nothing of it is left"), Fixture.StreamedActors(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerNoThrashTest,
	"Mantle.Runtime.CellStream.Streamer.NoThrash",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerNoThrashTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	FFixture Fixture;
	Fixture.StartStreamer(SteadySettings());
	Fixture.View = FVector(21000.0, 100.0, 100.0);
	if (!TestTrue(TEXT("the join settles at 210 m"), Fixture.Settle()))
	{
		return false;
	}
	// From 210 m: cell 0 (ends at 64 m) is 146 m off and wanted, and cells 0 to 5 stand.
	TestEqual(TEXT("six cells stand"), Fixture.Streamer->GetStats().CellsBuilt, 6);
	const int64 BuiltBefore = Fixture.Streamer->GetStats().CellsBuiltEver;
	const int64 ReleasedBefore = Fixture.Streamer->GetStats().CellsReleasedEver;

	// A view that wanders along the edge of cell 0's draw distance (150 m past 64 m is 214 m; it is kept until 180 m past, which is 244 m) goes between 210 m and 230 m forty times:
	// no cell is built again, none is released, and the work is the same cells all the time.
	for (int32 Pass = 0; Pass < 40; ++Pass)
	{
		Fixture.View.X = (Pass % 2 == 0) ? 23000.0 : 21000.0;
		Fixture.Settle();
		TestTrue(TEXT("cell 0 stays"), Fixture.Built(0));
	}
	TestEqual(TEXT("no cell was built again"), Fixture.Streamer->GetStats().CellsBuiltEver, BuiltBefore);
	TestEqual(TEXT("and none was released"), Fixture.Streamer->GetStats().CellsReleasedEver, ReleasedBefore);

	// Past the band it goes, once, and a view that comes back to the band's inside edge does not rebuild it until it is wanted again: at 250 m cell 0 (186 m off) is released.
	Fixture.View.X = 25000.0;
	Fixture.Settle();
	TestFalse(TEXT("cell 0 is released past 180 m"), Fixture.Built(0));
	TestEqual(TEXT("one cell was released"), Fixture.Streamer->GetStats().CellsReleasedEver, ReleasedBefore + 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerHoldTest,
	"Mantle.Runtime.CellStream.Streamer.TeleportHolds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerHoldTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	FFixture Fixture;
	Fixture.StartStreamer(SteadySettings());
	if (!TestTrue(TEXT("the join settles"), Fixture.Settle()))
	{
		return false;
	}
	Fixture.HoldCalls.Reset();

	// The player is moved 450 m: not travel, a teleport. The next tick holds, and the hold lasts until the cells within 100 m of the new spot stand.
	Fixture.View = FVector(70000.0, 100.0, 100.0);
	Fixture.Tick();
	TestTrue(TEXT("held at once"), Fixture.Streamer->IsHolding());
	TestTrue(TEXT("and told"), Fixture.HoldCalls.Num() == 1 && Fixture.HoldCalls[0]);
	TestFalse(TEXT("the spot's cell does not stand yet"), Fixture.Streamer->IsAreaBuilt(Fixture.View, 10000.0));
	TestTrue(TEXT("the cells settle"), Fixture.Settle());
	TestFalse(TEXT("the hold ended"), Fixture.Streamer->IsHolding());
	TestTrue(TEXT("and was told so"), Fixture.HoldCalls.Num() == 2 && !Fixture.HoldCalls[1]);
	TestTrue(TEXT("the cell that holds the spot (cell 10, 640 to 704 m) stands"), Fixture.Built(10));
	TestTrue(TEXT("and the area round it"), Fixture.Streamer->IsAreaBuilt(Fixture.View, 10000.0));

	// A hold with a cell that can never be read does not wait for it.
	FFixture Broken;
	Broken.Resolver->Unknown.Add(FileId(0));
	Broken.StartStreamer(SteadySettings());
	AddExpectedError(TEXT("MANTLE_PLACE_CELLS FAIL"), EAutomationExpectedErrorFlags::Contains, 1);
	TestTrue(TEXT("a missing cell: the streamer still settles"), Broken.Settle());
	TestFalse(TEXT("and the hold is over"), Broken.Streamer->IsHolding());
	TestFalse(TEXT("the cell is not built"), Broken.Built(0));
	TestTrue(TEXT("the others are"), Broken.Built(1) && Broken.Built(2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerExtraViewsTest,
	"Mantle.Runtime.CellStream.Streamer.ExtraViews",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerExtraViewsTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	FFixture Fixture;
	Fixture.StartStreamer(SteadySettings());
	if (!TestTrue(TEXT("the join settles"), Fixture.Settle()))
	{
		return false;
	}
	// The player stands at x = 1 m: cells 0, 1 and 2 stand, and the join's hold began and ended.
	TestEqual(TEXT("cells 0 to 2 stand"), Fixture.Streamer->GetStats().CellsBuilt, 3);
	TestEqual(TEXT("one hold, begun and ended"), Fixture.HoldCalls.Num(), 2);

	// The camera is 700 m away (x = 70,000 cm, in cell 10 which spans 64,000 to 70,400) while the player stays. Within 150 m of that point: cell 8 (51,200 to 57,600, 12,400 cm off),
	// 9, 10 and 11 (70,400, 400 cm off); cell 7 ends at 51,200 and is 18,800 off. So cells 8 to 11 are built as well, and the player's cells stand.
	Fixture.Extras = { FMantleCellStreamer::FExtraView{ 1, FVector(70000.0, 100.0, 100.0) } };
	TestTrue(TEXT("the cells settle"), Fixture.Settle());
	TestTrue(TEXT("the camera's cells 8 to 11 are built"), Fixture.Built(8) && Fixture.Built(9) && Fixture.Built(10) && Fixture.Built(11));
	TestFalse(TEXT("cell 7 is not"), Fixture.Built(7));
	TestTrue(TEXT("the player's cells 0 to 2 are still there"), Fixture.Built(0) && Fixture.Built(1) && Fixture.Built(2));
	TestEqual(TEXT("seven cells stand"), Fixture.Streamer->GetStats().CellsBuilt, 7);
	TestEqual(TEXT("and nothing was held for the camera"), Fixture.HoldCalls.Num(), 2);

	// The camera jumps 500 m to x = 20,000 (cell 3, 19,200 to 25,600) in one tick: that is no teleport, because the player did not move. Wanted from it: cells 0 to 5
	// (cell 5 spans 32,000 to 38,400, 12,000 off; cell 6 starts at 38,400, 18,400 off). Cells 8 to 11 are 31,200 cm and more from it and 51,100 from the player: past 18,000, released.
	Fixture.Extras = { FMantleCellStreamer::FExtraView{ 1, FVector(20000.0, 100.0, 100.0) } };
	Fixture.Tick();
	TestFalse(TEXT("a jump of the camera holds nothing"), Fixture.Streamer->IsHolding());
	TestTrue(TEXT("the cells settle"), Fixture.Settle());
	TestEqual(TEXT("the camera's new place is built: cells 0 to 5"), Fixture.Streamer->GetStats().CellsBuilt, 6);
	TestTrue(TEXT("cell 3 and cell 5 stand"), Fixture.Built(3) && Fixture.Built(5));
	TestFalse(TEXT("the old camera place is taken down"), Fixture.Built(8) || Fixture.Built(11));
	TestEqual(TEXT("still no hold"), Fixture.HoldCalls.Num(), 2);

	// The camera comes back to the player (or goes): what only it wanted is released. Cell 3 (19,200) is 19,100 cm from the player, past the 18,000 a 15,000 cm cell is kept to.
	Fixture.Extras.Reset();
	TestTrue(TEXT("the cells settle"), Fixture.Settle());
	TestEqual(TEXT("the player's three cells are left"), Fixture.Streamer->GetStats().CellsBuilt, 3);
	TestEqual(TEXT("with their actors"), Fixture.StreamedActors(), 3 * ActorsPerCell);

	// An extra view next to the player (here 5 m off) adds nothing.
	Fixture.Extras = { FMantleCellStreamer::FExtraView{ 1, FVector(600.0, 100.0, 100.0) } };
	TestTrue(TEXT("the cells settle"), Fixture.Settle());
	TestEqual(TEXT("still three"), Fixture.Streamer->GetStats().CellsBuilt, 3);

	// The player itself jumps while a camera stays: that holds, as before, until the cells round the player stand.
	Fixture.Extras.Reset();
	Fixture.View = FVector(70000.0, 100.0, 100.0);
	Fixture.Tick();
	TestTrue(TEXT("the player's jump holds"), Fixture.Streamer->IsHolding());
	TestTrue(TEXT("the cells settle"), Fixture.Settle());
	TestTrue(TEXT("and the spot's cell stands"), Fixture.Built(10));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerLongFrameTest,
	"Mantle.Runtime.CellStream.Streamer.LongFrame",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerLongFrameTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	FFixture Fixture;
	Fixture.StartStreamer(SteadySettings());
	if (!TestTrue(TEXT("the join settles"), Fixture.Settle()))
	{
		return false;
	}
	Fixture.HoldCalls.Reset();

	// A 0.5 s hitch at 240 m/s moves the player 120 m (12,000 cm): that is travel (a half second allows 150 m), not a teleport.
	Fixture.View.X += 12000.0;
	Fixture.Tick(0.5);
	TestFalse(TEXT("travel over a long frame does not hold"), Fixture.Streamer->IsHolding());
	TestTrue(TEXT("and nothing was told"), Fixture.HoldCalls.IsEmpty());

	// 200 m in the same half second is more than anything travels at: a teleport.
	Fixture.View.X += 20000.0;
	Fixture.Tick(0.5);
	TestTrue(TEXT("a jump too big for the frame holds"), Fixture.Streamer->IsHolding());
	TestTrue(TEXT("the cells settle"), Fixture.Settle());

	// And a normal frame still catches 101 m.
	Fixture.HoldCalls.Reset();
	Fixture.View.X += 10100.0;
	Fixture.Tick();
	TestTrue(TEXT("101 m in a 60 fps frame is a teleport"), Fixture.Streamer->IsHolding());
	TestTrue(TEXT("the cells settle"), Fixture.Settle());

	// Stop leaves nothing that reads the index it let go of.
	Fixture.Streamer->Stop();
	TestFalse(TEXT("no index after Stop"), Fixture.Streamer->IsIndexReady());
	TestFalse(TEXT("so no area is built"), Fixture.Streamer->IsAreaBuilt(Fixture.View, 10000.0));
	TestEqual(TEXT("and nothing of the cells is left"), Fixture.StreamedActors(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerStandaloneTest,
	"Mantle.Runtime.CellStream.Streamer.SolidAndPictures",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerStandaloneTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	// A standalone game is the server and the player in one process: one streamer builds every solid thing of every cell and keeps it, the other builds the pictures by distance.
	// Each cell here holds three cubes and a list of 250 instances with collision (four actors) and one model with none (one actor).
	FFixture Fixture(/*bPictures*/ true);
	FMantleCellStreamSettings Pictures = SteadySettings();
	Pictures.Entities = EMantleCellEntities::PicturesOnly;
	Fixture.StartStreamer(Pictures);
	FMantleCellStreamSettings Solid = SteadySettings();
	Solid.Entities = EMantleCellEntities::SolidOnly;
	Solid.bKeepEveryCell = true;
	Fixture.StartOther(Solid);
	TestTrue(TEXT("both hold at the join"), Fixture.Streamer->IsHolding() && Fixture.Other->IsHolding());

	const auto Wait = [&Fixture]()
	{
		for (int32 Tick = 0; Tick < 3000; ++Tick)
		{
			Fixture.Tick();
			const FMantleCellStreamStats& S = Fixture.Streamer->GetStats();
			if (S.CellsPending == 0 && S.CellsLoading == 0 && S.CellsReady == 0 && S.CellsRemoving == 0 && !Fixture.Streamer->IsHolding()
				&& Fixture.Other->IsEverythingBuilt() && !Fixture.Other->IsHolding())
			{
				return true;
			}
		}
		return false;
	};
	if (!TestTrue(TEXT("both settle"), Wait()))
	{
		return false;
	}
	// Every one of the 12 cells has its four solid actors: 48. The pictures are those of cells 0 to 2 (within 150 m of the view at x = 1 m): 3. Nothing else stands.
	TestEqual(TEXT("every cell's solid things stand"), Fixture.Other->GetStats().CellsBuilt, CellCount);
	TestEqual(TEXT("the pictures of the three cells near the view"), Fixture.Streamer->GetStats().CellsBuilt, 3);
	TestEqual(TEXT("48 solid actors and 3 pictures"), Fixture.StreamedActors(), 12 * 4 + 3);
	TestEqual(TEXT("every solid instance is in its component: 250 in each of 12 cells"), Fixture.CountInstances(), 12 * 250);

	// A teleport to cell 10 holds the player for the pictures only: the solid streamer waits for nothing, and the solid things of the cells left behind stay.
	Fixture.HoldCalls.Reset();
	Fixture.OtherHoldCalls.Reset();
	Fixture.View = FVector(70000.0, 100.0, 100.0);
	Fixture.Tick();
	TestTrue(TEXT("the pictures' streamer holds"), Fixture.Streamer->IsHolding());
	TestFalse(TEXT("the solid streamer does not"), Fixture.Other->IsHolding());
	TestTrue(TEXT("the cells settle"), Wait());
	TestTrue(TEXT("nothing was told to the solid streamer's owner"), Fixture.OtherHoldCalls.IsEmpty());
	// Pictures of cells 8 to 11 (within 150 m of 700 m) now; those of cells 0 to 2 are taken down. All 48 solid actors still stand.
	TestEqual(TEXT("the pictures follow the view"), Fixture.StreamedActors(), 12 * 4 + 4);
	TestEqual(TEXT("and the solid things are all still there"), Fixture.CountInstances(), 12 * 250);
	TestEqual(TEXT("every cell's solid things stand still"), Fixture.Other->GetStats().CellsBuilt, CellCount);
	TestEqual(TEXT("the solid streamer released nothing"), Fixture.Other->GetStats().CellsReleasedEver, static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerBudgetTest,
	"Mantle.Runtime.CellStream.Streamer.BudgetAndLookAhead",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerBudgetTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;

	// A budget of nothing still does one piece a tick, and no more: the work is spread over frames however little time they give it.
	{
		FFixture Fixture;
		FMantleCellStreamSettings Settings = SteadySettings();
		Settings.BaseBudgetSeconds = 0.0;
		Settings.HoldBudgetSeconds = 0.0;
		Fixture.StartStreamer(Settings);
		int64 Last = 0;
		int64 MostInATick = 0;
		for (int32 Tick = 0; Tick < 300; ++Tick)
		{
			Fixture.Tick();
			const int64 Now = Fixture.Streamer->GetStats().UnitsBuilt;
			MostInATick = FMath::Max(MostInATick, Now - Last);
			Last = Now;
		}
		TestEqual(TEXT("never more than one piece a tick"), MostInATick, static_cast<int64>(1));
		TestEqual(TEXT("and all eighteen pieces in the end"), Fixture.Streamer->GetStats().UnitsBuilt, static_cast<int64>(18));
	}

	// Look-ahead: a view standing at 1 m asks for cells 0 to 2; the same view driving east at 40 m/s has a path 160 m long, so the cell 320 m on is read before it arrives.
	{
		FFixture Standing;
		Standing.StartStreamer(SteadySettings());
		Standing.Settle();
		TestFalse(TEXT("standing: cell 5's file is not asked for"), Standing.Resolver->Asked.Contains(FileId(5)));
	}
	{
		FFixture Driving;
		FMantleCellStreamSettings Settings;
		Settings.HoldRadiusCm = 10000.0;
		Driving.StartStreamer(Settings);
		Driving.Settle();
		Driving.Velocity = FVector(4000.0, 0.0, 0.0);
		// A second of driving at 40 m/s moves the view 40 m to x = 41 m; the smoothed velocity is 98% of 40 m/s by then, so the path ends 157 m on at x = 198 m, and cell 5 (from 320 m) is 122 m from its end.
		for (int32 Tick = 0; Tick < 60; ++Tick)
		{
			Driving.Tick();
		}
		TestTrue(TEXT("the view's speed is read"), Driving.Streamer->GetView().Velocity.X > 2500.0);
		TestTrue(TEXT("driving: cell 5, 280 m ahead of the view, is already asked for"), Driving.Resolver->Asked.Contains(FileId(5)));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerKeepAllPlansOnceTest,
	"Mantle.Runtime.CellStream.Streamer.KeepAllPlansOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerKeepAllPlansOnceTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	// A streamer that keeps every cell wants the same cells wherever the view is: once everything of its kind stands there is nothing to plan, and a moving view must not
	// make it go through every cell's wants again (a big place has a quarter of a million of them).
	{
		FFixture Fixture;
		FMantleCellStreamSettings Solid = SteadySettings();
		Solid.Entities = EMantleCellEntities::SolidOnly;
		Solid.bKeepEveryCell = true;
		Fixture.StartStreamer(Solid);
		if (!TestTrue(TEXT("every cell's solid things stand"), Fixture.Settle()))
		{
			return false;
		}
		TestEqual(TEXT("all twelve cells"), Fixture.Streamer->GetStats().CellsBuilt, CellCount);
		TestTrue(TEXT("and it knows it"), Fixture.Streamer->IsEverythingBuilt());
		const int32 PlansWhenSettled = Fixture.Streamer->GetStats().PlansRun;
		TestTrue(TEXT("it planned at least once"), PlansWhenSettled > 0);

		// The view runs the length of the row and jumps: every one of those would ask for a new plan.
		Fixture.Velocity = FVector(6000.0, 0.0, 0.0);
		for (int32 Tick = 0; Tick < 200; ++Tick)
		{
			Fixture.Tick();
		}
		Fixture.Velocity = FVector::ZeroVector;
		Fixture.View = FVector(70000.0, 100.0, 100.0);
		for (int32 Tick = 0; Tick < 60; ++Tick)
		{
			Fixture.Tick();
		}
		TestEqual(TEXT("nothing was planned again"), Fixture.Streamer->GetStats().PlansRun, PlansWhenSettled);
		TestTrue(TEXT("everything still stands"), Fixture.Streamer->IsEverythingBuilt());
		TestEqual(TEXT("and none of it was released"), Fixture.Streamer->GetStats().CellsReleasedEver, static_cast<int64>(0));
	}

	// The same view makes a streamer that follows it plan again and again.
	{
		FFixture Fixture;
		Fixture.StartStreamer(SteadySettings());
		if (!TestTrue(TEXT("the join settles"), Fixture.Settle()))
		{
			return false;
		}
		const int32 PlansWhenSettled = Fixture.Streamer->GetStats().PlansRun;
		Fixture.Velocity = FVector(6000.0, 0.0, 0.0);
		for (int32 Tick = 0; Tick < 60; ++Tick)
		{
			Fixture.Tick();
		}
		TestTrue(TEXT("a streamer that follows the view plans as it moves"), Fixture.Streamer->GetStats().PlansRun > PlansWhenSettled + 5);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerDestroyedListTest,
	"Mantle.Runtime.CellStream.Streamer.DestroyedListStaysQuiet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerDestroyedListTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	// A long solid list is built as one piece after another into the actor the first made. A script that destroys the entity does it the moment its first piece stands (the
	// owner of the streamer's piece callback applies what the server said); the pieces after it have nowhere to go, and that is not an error.
	FFixture Fixture;
	Fixture.StartStreamer(SteadySettings());
	TArray<TWeakObjectPtr<AActor>> Destroyed;
	Fixture.Streamer->SetEntityCallbacks(
		[&Fixture, &Destroyed](const FString& EntityId, const FString&, AActor* Piece)
		{
			Fixture.BuiltIds.Add(EntityId);
			if (EntityId == TEXT("ent_ism_c0"))
			{
				Destroyed.Add(Piece);
				Piece->Destroy();
			}
		},
		[&Fixture](const FString& EntityId) { Fixture.ReleasedIds.Add(EntityId); });
	if (!TestTrue(TEXT("the join settles"), Fixture.Settle()))
	{
		return false;
	}
	const FMantleCellStreamStats& Stats = Fixture.Streamer->GetStats();
	TestEqual(TEXT("the list was destroyed once, at its first piece"), Destroyed.Num(), 1);
	TestEqual(TEXT("its other two pieces were skipped"), Stats.PiecesSkippedDestroyed, 2);
	TestEqual(TEXT("and that is no error"), Stats.BuildErrors, 0);
	// Cells 0 to 2 are built: three lists of 250 instances, but the first is gone.
	TestEqual(TEXT("the other lists were built whole"), Fixture.CountInstances(), 2 * 250);

	// The cell is released and built again: the list comes back, and is destroyed again at its first piece, with the same quiet.
	Fixture.View = FVector(70000.0, 100.0, 100.0);
	TestTrue(TEXT("the view moves away and settles"), Fixture.Settle());
	Fixture.View = FVector(100.0, 100.0, 100.0);
	TestTrue(TEXT("and comes back"), Fixture.Settle());
	TestEqual(TEXT("the list was destroyed again"), Destroyed.Num(), 2);
	TestEqual(TEXT("and still no error"), Fixture.Streamer->GetStats().BuildErrors, 0);
	TestEqual(TEXT("with its other pieces skipped again"), Fixture.Streamer->GetStats().PiecesSkippedDestroyed, 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerJoinWorkTest,
	"Mantle.Runtime.CellStream.Streamer.JoinWorkCountsEveryStreamer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerJoinWorkTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	// The join screen counts a change in the work done as progress. A standalone game's solid streamer does most of the join's work: its files read and pieces built count.
	FFixture Fixture(/*bPictures*/ true);
	FMantleCellStreamSettings Pictures = SteadySettings();
	Pictures.Entities = EMantleCellEntities::PicturesOnly;
	Fixture.StartStreamer(Pictures);
	FMantleCellStreamSettings Solid = SteadySettings();
	Solid.Entities = EMantleCellEntities::SolidOnly;
	Solid.bKeepEveryCell = true;
	Fixture.StartOther(Solid);
	TestEqual(TEXT("nothing is done yet"), Fixture.Other->GetJoinWork(), static_cast<int64>(0));
	int64 Last = 0;
	bool bRose = false;
	for (int32 Tick = 0; Tick < 600 && !(Fixture.Other->IsEverythingBuilt() && !Fixture.Other->IsHolding()); ++Tick)
	{
		Fixture.Tick();
		const int64 Now = Fixture.Other->GetJoinWork();
		bRose = bRose || Now > Last;
		Last = Now;
	}
	TestTrue(TEXT("the solid streamer's work rose while it built"), bRose);
	TestEqual(TEXT("it is its pieces built and its files read"), Fixture.Other->GetJoinWork(), Fixture.Other->GetStats().UnitsBuilt + Fixture.Other->GetFilesRead());
	TestEqual(TEXT("every one of the twelve cells' four solid pieces (three cubes and a list of three) was built"), Fixture.Other->GetStats().UnitsBuilt, static_cast<int64>(12 * 6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerAssetsGateTheBuildTest,
	"Mantle.Runtime.CellStream.Streamer.AssetsGateTheBuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerAssetsGateTheBuildTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	FFixture Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	Fixture.CellAssets = MakeShared<FFakeCellAssets>();
	Fixture.CellAssets->bReady = false;
	Fixture.StartStreamer(SteadySettings());

	// From x = 1 m the cells within 150 m are 0, 1 and 2. Their files are read and their assets asked for; their assets do not come.
	for (int32 Tick = 0; Tick < 300 && Fixture.CellAssets->Acquired.Num() < 3; ++Tick)
	{
		Fixture.Tick();
	}
	for (int32 Tick = 0; Tick < 100; ++Tick)
	{
		Fixture.Tick();
	}
	TestEqual(TEXT("each of the three cells asked once"), Fixture.CellAssets->Acquired.Num(), 3);
	TestEqual(TEXT("and they are cells 0, 1 and 2"), Fixture.CellAssets->Held.Num() == 3 && Fixture.CellAssets->Held.Contains(0) && Fixture.CellAssets->Held.Contains(1) && Fixture.CellAssets->Held.Contains(2), true);
	TestEqual(TEXT("with their six solid pieces (three cubes and a list of three) each"), Fixture.CellAssets->EntitiesAsked, 18);
	TestEqual(TEXT("the three wait for their assets"), Fixture.Streamer->GetStats().CellsAssetWait, 3);
	TestEqual(TEXT("nothing is built"), Fixture.StreamedActors(), 0);
	TestEqual(TEXT("no piece was built"), Fixture.Streamer->GetStats().UnitsBuilt, static_cast<int64>(0));
	TestTrue(TEXT("the join still holds: the area does not stand"), Fixture.Streamer->IsHolding());
	TestEqual(TEXT("and they count as the backlog"), Fixture.Streamer->GetStats().CellsPending, 3);
	// The assets' builds and the pieces share the join hold's 8 ms: while a cell waits for its assets they get half of it.
	TestTrue(TEXT("the assets are given half the hold budget, 4 ms, while a cell waits"), FMath::IsNearlyEqual(Fixture.CellAssets->LastSlice, 0.004, 1e-9));

	// The assets arrive: the cells are built, the hold ends, nothing is given back.
	Fixture.CellAssets->bReady = true;
	if (!TestTrue(TEXT("the streamer settles"), Fixture.Settle()))
	{
		return false;
	}
	TestTrue(TEXT("cells 0, 1 and 2 stand"), Fixture.Built(0) && Fixture.Built(1) && Fixture.Built(2));
	TestEqual(TEXT("four actors a cell"), Fixture.StreamedActors(), 3 * ActorsPerCell);
	TestEqual(TEXT("the hold ended"), Fixture.Streamer->IsHolding(), false);
	TestEqual(TEXT("none was asked for twice"), Fixture.CellAssets->Acquired.Num(), 3);
	TestEqual(TEXT("and none was given back while they stand"), Fixture.CellAssets->Released.Num(), 0);
	TestEqual(TEXT("a cell waited for them, so the longest wait is on the record"), Fixture.Streamer->GetStats().LongestAssetWaitSeconds > 0.0, true);
	TestTrue(TEXT("with nothing waiting the assets get only their idle slice, 0.5 ms"), FMath::IsNearlyEqual(Fixture.CellAssets->LastSlice, 0.0005, 1e-9));
	TestTrue(TEXT("and never more than half of the budget of a tick"), Fixture.CellAssets->LargestSlice <= 0.004 + 1e-9);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerAssetsGoWithTheCellTest,
	"Mantle.Runtime.CellStream.Streamer.AssetsGoWithTheCell",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerAssetsGoWithTheCellTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	FFixture Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	Fixture.CellAssets = MakeShared<FFakeCellAssets>();
	Fixture.StartStreamer(SteadySettings());
	if (!TestTrue(TEXT("the streamer settles"), Fixture.Settle()))
	{
		return false;
	}
	TestEqual(TEXT("cells 0, 1 and 2 stand and hold their assets"), Fixture.CellAssets->Held.Num() == 3 && Fixture.CellAssets->Held.Contains(0) && Fixture.CellAssets->Held.Contains(2), true);

	// 600 m on, past every released cell's hysteresis: the three come down, and give their assets back each once, after their actors are gone.
	Fixture.View = FVector(60000.0, 100.0, 100.0);
	if (!TestTrue(TEXT("the streamer settles at the far end"), Fixture.Settle()))
	{
		return false;
	}
	TestFalse(TEXT("cell 0 is gone"), Fixture.Built(0));
	TestEqual(TEXT("cells 0, 1 and 2 gave their assets back, each once"), Fixture.CellAssets->Released.Num(), 3);
	TestTrue(TEXT("exactly those"), Fixture.CellAssets->Released.Contains(0) && Fixture.CellAssets->Released.Contains(1) && Fixture.CellAssets->Released.Contains(2));
	// What stands now holds, and nothing else does: every cell asked once, and the held ones are the built ones.
	TestEqual(TEXT("the cells that stand hold their assets"), Fixture.CellAssets->Held.Num(), Fixture.Streamer->GetStats().CellsBuilt);
	TestEqual(TEXT("asked once for each cell read, given back once for each that went"), Fixture.CellAssets->Acquired.Num() - Fixture.CellAssets->Released.Num(), Fixture.CellAssets->Held.Num());

	// Stopping gives back whatever still stands.
	const int32 StillHeld = Fixture.CellAssets->Held.Num();
	const int32 ReleasedBefore = Fixture.CellAssets->Released.Num();
	Fixture.Streamer->Stop();
	TestEqual(TEXT("stopping gives every cell's assets back"), Fixture.CellAssets->Released.Num() - ReleasedBefore, StillHeld);
	TestEqual(TEXT("and none is held"), Fixture.CellAssets->Held.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerMissingAssetsTest,
	"Mantle.Runtime.CellStream.Streamer.MissingAssets",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerMissingAssetsTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	// A mesh that could not be had: the cells that use it are built without what uses it, say so once, and never wait for it.
	{
		FFixture Fixture;
		if (!TestNotNull(TEXT("a world"), Fixture.World))
		{
			return false;
		}
		Fixture.CellAssets = MakeShared<FFakeCellAssets>();
		Fixture.CellAssets->Failing.Add(TEXT("ast_cube"));
		Fixture.StartStreamer(SteadySettings());
		if (!TestTrue(TEXT("the streamer settles with the mesh failed"), Fixture.Settle()))
		{
			return false;
		}
		TestTrue(TEXT("the three cells stand (the join does not hold)"), Fixture.Built(0) && Fixture.Built(1) && Fixture.Built(2) && !Fixture.Streamer->IsHolding());
		TestEqual(TEXT("with nothing in them"), Fixture.StreamedActors(), 0);
		TestEqual(TEXT("every one of their six pieces was left out: three cells of six"), Fixture.Streamer->GetStats().EntitiesWithoutMesh, static_cast<int64>(18));
		TestEqual(TEXT("the asset is named once, however many cells use it"), Fixture.Streamer->GetStats().AssetsMissing, 1);
		TestEqual(TEXT("no build error: nothing was built for it"), Fixture.Streamer->GetStats().BuildErrors, 0);
		TestEqual(TEXT("the cells gave their assets back at once, having nothing to build"), Fixture.CellAssets->Released.Num(), 3);
	}
	// An asset that never arrives: after the wait the cell is built without it.
	{
		FFixture Fixture;
		if (!TestNotNull(TEXT("a world"), Fixture.World))
		{
			return false;
		}
		Fixture.CellAssets = MakeShared<FFakeCellAssets>();
		Fixture.CellAssets->bReady = false;
		FMantleCellStreamSettings Settings = SteadySettings();
		Settings.AssetWaitTimeoutSeconds = 0.05;
		Fixture.StartStreamer(Settings);
		const double Start = FPlatformTime::Seconds();
		while (FPlatformTime::Seconds() - Start < 2.0 && Fixture.Streamer->GetStats().AssetWaitTimeouts < 3)
		{
			Fixture.Tick();
		}
		TestEqual(TEXT("each of the three cells gave up on it once"), Fixture.Streamer->GetStats().AssetWaitTimeouts, 3);
		TestTrue(TEXT("and the join was let go"), Fixture.Settle() && !Fixture.Streamer->IsHolding());
		TestEqual(TEXT("with nothing built"), Fixture.StreamedActors(), 0);
		TestEqual(TEXT("the asset is named once"), Fixture.Streamer->GetStats().AssetsMissing, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerQueryAreaTest,
	"Mantle.Runtime.CellStream.Streamer.QueryAreaSolidAndPending",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerQueryAreaTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	FFixture Fixture;
	Fixture.StartStreamer(SteadySettings());
	if (!TestTrue(TEXT("the join settles"), Fixture.Settle()))
	{
		return false;
	}
	FMantleCellAreaQuery Here;
	Here.Point = Fixture.View;
	Here.RadiusCm = 300.0;
	Here.bSolidOnly = true;

	// Standing at x = 1 m in cell 0 (0 to 64 m): the one cell within 3 m of the player stands.
	FMantleCellAreaState Under = Fixture.Streamer->QueryArea(Here);
	TestEqual(TEXT("one cell near the player"), Under.Cells, 1);
	TestEqual(TEXT("and it stands"), Under.Built, 1);
	TestTrue(TEXT("every cell asked about stands"), Under.AllStand());
	TestTrue(TEXT("a probe there may capture"), Fixture.Streamer->QueryArea(Here).ReadyToCapture());
	const uint64 SerialStanding = Fixture.Streamer->GetAreaSerial();

	// Cell 10 (640 to 704 m) was never wanted: it is a cell near a point there, and it does not stand, and is not on its way.
	FMantleCellAreaQuery Far = Here;
	Far.Point = FVector(67000.0, 100.0, 100.0);
	const FMantleCellAreaState FarState = Fixture.Streamer->QueryArea(Far);
	TestEqual(TEXT("one cell near the far point"), FarState.Cells, 1);
	TestEqual(TEXT("it does not stand"), FarState.Built, 0);
	TestEqual(TEXT("and is not wanted"), FarState.Pending, 0);
	TestFalse(TEXT("so a body there has no ground"), FarState.AllStand());
	TestFalse(TEXT("and a probe there may not capture (its cells have not come)"), FarState.ReadyToCapture());

	// Released behind the player: the cell under the old spot no longer stands, and the answer's serial moved.
	Fixture.View = FVector(25000.0, 100.0, 100.0);
	Fixture.Settle();
	TestFalse(TEXT("cell 0 was released"), Fixture.Built(0));
	const FMantleCellAreaState Released = Fixture.Streamer->QueryArea(Here);
	TestEqual(TEXT("it is still a cell near the old spot"), Released.Cells, 1);
	TestFalse(TEXT("but its ground is gone"), Released.AllStand());
	TestTrue(TEXT("the serial moved with the cells"), Fixture.Streamer->GetAreaSerial() != SerialStanding);

	// A cell that can never be read does not stand: a body on it keeps following the server, and a probe is not held back by it.
	FFixture Broken;
	Broken.Resolver->Unknown.Add(FileId(0));
	Broken.StartStreamer(SteadySettings());
	AddExpectedError(TEXT("MANTLE_PLACE_CELLS FAIL"), EAutomationExpectedErrorFlags::Contains, 1);
	Broken.Settle();
	FMantleCellAreaQuery OnBroken = Here;
	OnBroken.Point = Broken.View;
	const FMantleCellAreaState Failed = Broken.Streamer->QueryArea(OnBroken);
	TestEqual(TEXT("the broken cell is counted"), Failed.Cells, 1);
	TestEqual(TEXT("as failed"), Failed.Failed, 1);
	TestFalse(TEXT("it does not stand"), Failed.AllStand());
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerOwnGroundTest,
	"Mantle.Runtime.CellStream.Streamer.OwnGroundHold",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerOwnGroundTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	// The view walks toward cell 3 (19,200 to 25,600 cm) while that cell's assets do not come: within 300 cm of it, with nothing built there, the player is held (a join
	// or a teleport holds the same way); when the assets come the cell is built and the hold ends. Cells 0 to 2 stand from the start (cell 2 ends at 19,200).
	{
		FFixture Fixture;
		Fixture.CellAssets = MakeShared<FFakeCellAssets>();
		FMantleCellStreamSettings Settings = SteadySettings();
		Fixture.StartStreamer(Settings);
		if (!TestTrue(TEXT("the join settles"), Fixture.Settle()))
		{
			return false;
		}
		TestFalse(TEXT("not holding"), Fixture.Streamer->IsHolding());
		TestTrue(TEXT("cell 2 stands"), Fixture.Built(2));
		TestFalse(TEXT("cell 3 does not (nothing near enough wanted it)"), Fixture.Built(3));
		Fixture.CellAssets->bReady = false;
		// 40 m/s: 188 m is 4.7 s, 280 ticks. The streamer wants cell 3 once it is within its draw distance (150 m) and waits for its assets.
		Fixture.Velocity = FVector(4000.0, 0.0, 0.0);
		bool bHeldBeforeTheCell = false;
		int32 Ticks = 0;
		for (; Ticks < 600 && Fixture.View.X < 19200.0 - 350.0; ++Ticks)
		{
			Fixture.Tick();
			bHeldBeforeTheCell = bHeldBeforeTheCell || Fixture.Streamer->IsHolding();
		}
		TestFalse(TEXT("350 cm before the cell: not held yet (the cell is more than 300 cm away)"), bHeldBeforeTheCell);
		// Walk on to 100 cm short of the cell: within 300 cm of the unbuilt solid cell. The hold begins on the first tick that sees it.
		for (int32 More = 0; More < 20 && !Fixture.Streamer->IsHolding(); ++More)
		{
			Fixture.Tick();
		}
		Fixture.Velocity = FVector::ZeroVector;
		TestTrue(TEXT("within 300 cm of a solid cell that is not built: the player is held"), Fixture.Streamer->IsHolding());
		TestFalse(TEXT("and that cell is not built"), Fixture.Built(3));
		// Its assets come: it is built, and the hold ends.
		Fixture.CellAssets->bReady = true;
		bool bBuilt = false;
		for (int32 More = 0; More < 2000 && !bBuilt; ++More)
		{
			Fixture.Tick();
			bBuilt = Fixture.Built(3) && !Fixture.Streamer->IsHolding();
		}
		TestTrue(TEXT("the cell is built and the hold has ended"), bBuilt);
	}

	// A hold that times out lets the player go and is not begun again on the same unbuilt cell; leaving and coming back holds again.
	{
		FFixture Fixture;
		Fixture.CellAssets = MakeShared<FFakeCellAssets>();
		FMantleCellStreamSettings Settings = SteadySettings();
		Settings.HoldTimeoutSeconds = 0.3;
		Fixture.StartStreamer(Settings);
		if (!TestTrue(TEXT("the join settles"), Fixture.Settle()))
		{
			return false;
		}
		Fixture.CellAssets->bReady = false;
		Fixture.Velocity = FVector(4000.0, 0.0, 0.0);
		bool bHeld = false;
		for (int32 Tick = 0; Tick < 800 && !bHeld; ++Tick)
		{
			Fixture.Tick();
			bHeld = Fixture.Streamer->IsHolding();
		}
		Fixture.Velocity = FVector::ZeroVector;
		TestTrue(TEXT("held"), bHeld);
		bool bLetGo = false;
		for (int32 Tick = 0; Tick < 2000 && !bLetGo; ++Tick)
		{
			Fixture.Tick();
			bLetGo = !Fixture.Streamer->IsHolding();
		}
		TestTrue(TEXT("the timeout lets the player go"), bLetGo);
		bool bHeldAgain = false;
		for (int32 Tick = 0; Tick < 300; ++Tick)
		{
			Fixture.Tick();
			bHeldAgain = bHeldAgain || Fixture.Streamer->IsHolding();
		}
		TestFalse(TEXT("and it is not held again on the same cell"), bHeldAgain);
		// Out of its reach, onto a cell that stands, the give-up is forgotten; back on the unbuilt cell, held again.
		Fixture.Velocity = FVector(-4000.0, 0.0, 0.0);
		for (int32 Tick = 0; Tick < 800 && Fixture.View.X > 3000.0; ++Tick)
		{
			Fixture.Tick();
		}
		Fixture.Velocity = FVector(4000.0, 0.0, 0.0);
		bool bHeldOnReturn = false;
		for (int32 Tick = 0; Tick < 800 && !bHeldOnReturn; ++Tick)
		{
			Fixture.Tick();
			bHeldOnReturn = Fixture.Streamer->IsHolding();
		}
		Fixture.Velocity = FVector::ZeroVector;
		TestTrue(TEXT("coming back to the unbuilt cell holds again"), bHeldOnReturn);
	}

	// A cell that can never be read does not hold the player: the place stands without it.
	{
		FFixture Fixture;
		Fixture.Resolver->Unknown.Add(FileId(3));
		AddExpectedError(TEXT("MANTLE_PLACE_CELLS FAIL"), EAutomationExpectedErrorFlags::Contains, 1);
		Fixture.StartStreamer(SteadySettings());
		if (!TestTrue(TEXT("the join settles"), Fixture.Settle()))
		{
			return false;
		}
		Fixture.Velocity = FVector(4000.0, 0.0, 0.0);
		bool bHeld = false;
		for (int32 Tick = 0; Tick < 400 && Fixture.View.X < 19200.0 - 100.0; ++Tick)
		{
			Fixture.Tick();
			bHeld = bHeld || Fixture.Streamer->IsHolding();
		}
		Fixture.Velocity = FVector::ZeroVector;
		for (int32 Tick = 0; Tick < 120; ++Tick)
		{
			Fixture.Tick();
			bHeld = bHeld || Fixture.Streamer->IsHolding();
		}
		TestFalse(TEXT("a failed cell under the player holds nothing"), bHeld);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerSolidFilterTest,
	"Mantle.Runtime.CellStream.Streamer.SolidQuestionFiltersCells",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerSolidFilterTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	// Cell 1 (6,400 to 12,800 cm) holds one picture and nothing solid. A solid question counts only cells with something solid in them; an every-cell question counts it.
	{
		FFixture Fixture(/*bPictures*/ false, TSet<int32>({ 1 }));
		Fixture.StartStreamer(SteadySettings());
		if (!TestTrue(TEXT("the join settles"), Fixture.Settle()))
		{
			return false;
		}
		TestTrue(TEXT("the picture-only cell is built"), Fixture.Built(1));
		FMantleCellAreaQuery InCell1;
		InCell1.Point = FVector(9600.0, 100.0, 100.0);
		InCell1.RadiusCm = 300.0;
		InCell1.bSolidOnly = true;
		const FMantleCellAreaState Solid = Fixture.Streamer->QueryArea(InCell1);
		TestEqual(TEXT("solid question: no cell near it holds anything solid"), Solid.Cells, 0);
		TestTrue(TEXT("so there is nothing to wait for"), Solid.AllStand());
		InCell1.bSolidOnly = false;
		const FMantleCellAreaState All = Fixture.Streamer->QueryArea(InCell1);
		TestEqual(TEXT("every-cell question: that cell is one"), All.Cells, 1);
		TestEqual(TEXT("and it stands"), All.Built, 1);
		// Asked for, the cells that stand are named by index.
		InCell1.bWantCellIds = true;
		const FMantleCellAreaState Named = Fixture.Streamer->QueryArea(InCell1);
		TestEqual(TEXT("named: one cell stands"), Named.BuiltCellIds.Num(), 1);
		TestTrue(TEXT("and it is cell 1"), Named.BuiltCellIds.Contains(1));
		InCell1.bWantCellIds = false;
		TestEqual(TEXT("not named unless asked"), Fixture.Streamer->QueryArea(InCell1).BuiltCellIds.Num(), 0);
	}

	// The solid pieces of a cell stand while its pictures are still coming: with a budget of one piece a tick, there is a moment when a solid question says "stands" and an
	// every-cell question says it does not.
	{
		FFixture Fixture(/*bPictures*/ true);
		FMantleCellStreamSettings Settings = SteadySettings();
		Settings.BaseBudgetSeconds = 0.0;
		Settings.HoldBudgetSeconds = 0.0;
		Settings.FastBudgetSeconds = 0.0;
		Fixture.StartStreamer(Settings);
		FMantleCellAreaQuery Here;
		Here.Point = Fixture.View;
		Here.RadiusCm = 300.0;
		bool bSolidStoodFirst = false;
		bool bEverStoodWhole = false;
		for (int32 Tick = 0; Tick < 3000 && !bEverStoodWhole; ++Tick)
		{
			Fixture.Tick();
			Here.bSolidOnly = true;
			const FMantleCellAreaState Solid = Fixture.Streamer->QueryArea(Here);
			Here.bSolidOnly = false;
			const FMantleCellAreaState All = Fixture.Streamer->QueryArea(Here);
			if (Solid.Cells == 1 && Solid.AllStand() && All.Cells == 1 && All.Built == 0 && All.Pending == 1)
			{
				bSolidStoodFirst = true;
			}
			bEverStoodWhole = All.Built == 1;
		}
		TestTrue(TEXT("a solid question was answered \"stands\" while the cell's pictures were still coming"), bSolidStoodFirst);
		TestTrue(TEXT("and the cell did stand whole in the end"), bEverStoodWhole);
	}
	return true;
}


namespace MantleCellStreamerTestPrivate
{
	/** The cell area questions answered by one fixture's streamer, as a player's machine answers them; the resolvers are put back when it goes. */
	struct FStreamerAsAreaOwner
	{
		FMantleCellArea::FResolver SavedResolver;
		FMantleCellArea::FTakeDownResolver SavedTakeDown;

		explicit FStreamerAsAreaOwner(FFixture& Fixture)
		{
			SavedResolver = FMantleCellArea::Resolver;
			SavedTakeDown = FMantleCellArea::TakeDownResolver;
			FMantleCellArea::Resolver = [&Fixture](UWorld*, const FMantleCellAreaQuery& Query, FMantleCellAreaState& OutState, uint64& OutSerial)
			{
				OutState = Fixture.Streamer->QueryArea(Query);
				OutSerial = Fixture.Streamer->GetAreaSerial();
				return true;
			};
			FMantleCellArea::TakeDownResolver = [&Fixture](UWorld*, bool, uint64& OutSerial)
			{
				OutSerial = Fixture.Streamer->GetTakeDownSerial();
				return true;
			};
		}

		~FStreamerAsAreaOwner()
		{
			FMantleCellArea::Resolver = SavedResolver;
			FMantleCellArea::TakeDownResolver = SavedTakeDown;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerGateBodyTest,
	"Mantle.Runtime.CellStream.Streamer.GateStopsABodyWhenItsGroundGoes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerGateBodyTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	// A real resting body (a client's copy of a physics prop) over a real streamer: the cell under it is released, and in that very frame the body stops simulating and
	// stands at the server's pose; the cell comes back, and after the wait the body simulates again, from the server's pose, asleep as the server's was.
	FFixture Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	Fixture.StartStreamer(SteadySettings());
	if (!TestTrue(TEXT("the join settles"), Fixture.Settle()))
	{
		return false;
	}
	FStreamerAsAreaOwner Owner(Fixture);
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("the engine's cube"), Cube))
	{
		return false;
	}
	const FVector ServerPose(1000.0, 100.0, 300.0);
	AMantlePhysicsProp* Prop = Fixture.World->SpawnActor<AMantlePhysicsProp>(ServerPose, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the prop"), Prop))
	{
		return false;
	}
	UStaticMeshComponent* Mesh = Prop->GetStaticMeshComponent();
	Mesh->SetStaticMesh(Cube);
	FMantlePhysicsProps Physics;
	Physics.bSimulate = true;
	Prop->InitFromPlace(TEXT("ent_resting"), Physics);
	Prop->BeginSimulation();
	if (!TestTrue(TEXT("the prop simulates (the test world makes its body)"), Mesh->IsSimulatingPhysics()))
	{
		Prop->Destroy();
		return false;
	}
	// A client's copy: not the authority, adopted, and the server says it rests (asleep) at ServerPose.
	Prop->SetRole(ROLE_SimulatedProxy);
	Prop->MarkAdoptedForTest();
	FRepMovement& Replicated = Prop->GetReplicatedMovement_Mutable();
	Replicated.Location = ServerPose;
	Replicated.Rotation = FRotator::ZeroRotator;
	Replicated.LinearVelocity = FVector::ZeroVector;
	Replicated.bRepPhysics = true;
	Replicated.bSimulatedPhysicSleep = true;

	const double FrameSeconds = 1.0 / 60.0;
	auto Frame = [&]()
	{
		Fixture.World->TimeSeconds += FrameSeconds;
		Fixture.Tick(FrameSeconds);
		Prop->UpdateCollisionGate();
	};

	// Ground stands: the gate stays open, the body simulates.
	for (int32 Index = 0; Index < 30; ++Index)
	{
		Frame();
	}
	TestFalse(TEXT("ground stands: the gate is open"), Prop->IsCollisionGateClosed());
	TestTrue(TEXT("and the body simulates"), Mesh->IsSimulatingPhysics());

	// The view jumps 450 m away (a teleport, which holds the player and lets the streamer take the old cells down). The gate must close in the frame the streamer hands the
	// cell to its take-down, not up to a tenth of a second (six frames) later.
	const uint64 TakeDownBefore = Fixture.Streamer->GetTakeDownSerial();
	Fixture.View = FVector(45000.0, 100.0, 100.0);
	int32 ReleaseFrame = -1;
	int32 CloseFrame = -1;
	for (int32 Index = 0; Index < 600 && CloseFrame < 0; ++Index)
	{
		Frame();
		if (ReleaseFrame < 0 && Fixture.Streamer->GetTakeDownSerial() != TakeDownBefore)
		{
			ReleaseFrame = Index;
		}
		if (Prop->IsCollisionGateClosed())
		{
			CloseFrame = Index;
		}
	}
	TestTrue(TEXT("the cell under the body was handed to its take-down"), ReleaseFrame >= 0);
	TestTrue(TEXT("the gate closed"), CloseFrame >= 0);
	TestEqual(TEXT("in the same frame (the ask interval is six frames)"), CloseFrame, ReleaseFrame);
	TestFalse(TEXT("the body no longer simulates"), Mesh->IsSimulatingPhysics());
	TestTrue(TEXT("and stands at the server's pose"), Prop->GetActorLocation().Equals(ServerPose, 0.1));

	// The ground comes back (the view returns; the cell is built again): the gate opens after the 0.5 s wait, not before.
	Fixture.View = FVector(100.0, 100.0, 100.0);
	for (int32 Index = 0; Index < 600 && !Fixture.Built(0); ++Index)
	{
		Frame();
	}
	TestTrue(TEXT("cell 0 is built again"), Fixture.Built(0));
	TestTrue(TEXT("held shut the moment it stood: the wait has not passed"), Prop->IsCollisionGateClosed());
	TestFalse(TEXT("still not simulating"), Mesh->IsSimulatingPhysics());
	int32 FramesToOpen = 0;
	while (Prop->IsCollisionGateClosed() && FramesToOpen < 240)
	{
		Frame();
		++FramesToOpen;
	}
	TestFalse(TEXT("the gate opened"), Prop->IsCollisionGateClosed());
	TestTrue(TEXT("the body simulates again"), Mesh->IsSimulatingPhysics());
	TestTrue(TEXT("from the server's pose"), Prop->GetActorLocation().Equals(ServerPose, 0.1));
	TestFalse(TEXT("asleep, as the server's body is"), Mesh->RigidBodyIsAwake());
	Prop->Destroy();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
