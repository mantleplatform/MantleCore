// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAssetStore.h"
#include "MantleCellArea.h"
#include "MantleCellStreamer.h"
#include "MantleStreamBudget.h"
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
		/** Cells whose assets are still on their way even while bReady (a first visit's queue, one cell at a time). */
		TSet<int32> SlowCells;
		/** True: every look at the progress mark finds the downloads moved (bytes still arriving). */
		bool bDownloadsMoving = false;
		mutable int64 Mark = 0;

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
			if (!bReady || SlowCells.Contains(CellIndex))
			{
				OutPending.Add(TEXT("ast_cube"));
				return;
			}
			OutFailed = Failing;
		}

		virtual int64 GetProgressMark() const override
		{
			if (bDownloadsMoving)
			{
				++Mark;
			}
			return Mark;
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

		/** #189: how near full texture detail reaches, as the live budget last said (FMantlePlaceAssets::SetTextureDetailReach). */
		virtual void SetTextureDetailReach(double NearCm) override { TextureReach = NearCm; }
		double TextureReach = 15000.0;
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
		/** The owner's join goes on when the first hold ends (UMantleCellStreamSubsystem::OnHoldChanged): later holds wait for HoldRadiusCm. False: the test says when. */
		bool bJoinDoneOnFirstRelease = true;
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
			Streamer->SetHoldChangedCallback([this](const bool bHolding)
			{
				HoldCalls.Add(bHolding);
				if (!bHolding && bJoinDoneOnFirstRelease)
				{
					Streamer->NoteJoinDone();
				}
			});
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
		Settings.AssetIdleTimeoutSeconds = 0.05;
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
	FMantleCellStreamerJoinWaitsForNearbyAreaTest,
	"Mantle.Runtime.CellStream.Streamer.JoinWaitsForNearbyArea",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerJoinWaitsForNearbyAreaTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	// City Sample (#189): the join let the player in once the cells within 100 m stood, and the buildings that draw from farther popped in over a black view. The join is behind the
	// loading screen, so it waits for every cell the spot wants within JoinHoldRadiusCm (600 m; this row's cells all draw from 150 m, so all of them); a teleport afterwards
	// (no screen) waits for the 100 m round the player, as before. Last, a join radius shorter than a wanted cell's distance leaves that cell to stream in after the screen.
	FFixture Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	Fixture.bJoinDoneOnFirstRelease = false;
	Fixture.CellAssets = MakeShared<FFakeCellAssets>();
	// From x = 1 m cells 0, 1 and 2 are wanted (each within its 150 m drawDistance); cell 2 (128 to 192 m) is 127 m off, past the 100 m a teleport waits for. Its download is slow.
	Fixture.CellAssets->SlowCells.Add(2);
	Fixture.StartStreamer(SteadySettings());
	for (int32 Tick = 0; Tick < 2000 && !(Fixture.Built(0) && Fixture.Built(1)); ++Tick)
	{
		Fixture.Tick();
	}
	for (int32 Tick = 0; Tick < 30; ++Tick)
	{
		Fixture.Tick();
	}
	TestTrue(TEXT("cells 0 and 1 stand"), Fixture.Built(0) && Fixture.Built(1));
	TestTrue(TEXT("the 100 m round the spot stands"), Fixture.Streamer->IsAreaBuilt(Fixture.View, 10000.0));
	TestFalse(TEXT("cell 2 waits for its download"), Fixture.Built(2));
	TestTrue(TEXT("so the join still holds: it waits for the cells near the spot, not just 100 m"), Fixture.Streamer->IsHolding());
	const FMantleCellStreamer::FAreaCount Waiting = Fixture.Streamer->CountHoldArea();
	TestEqual(TEXT("the join's area is the three wanted cells"), Waiting.Cells, 3);
	TestEqual(TEXT("two of which stand"), Waiting.Done, 2);

	// The download arrives: cell 2 is built and the join is let go.
	Fixture.CellAssets->SlowCells.Reset();
	if (!TestTrue(TEXT("the join settles"), Fixture.Settle()))
	{
		return false;
	}
	TestTrue(TEXT("cell 2 stands"), Fixture.Built(2));
	TestTrue(TEXT("the join's hold began and ended once"), Fixture.HoldCalls.Num() == 2 && Fixture.HoldCalls[0] && !Fixture.HoldCalls[1]);
	TestFalse(TEXT("nothing more to hold for here"), Fixture.Streamer->NeedsJoinHold());

	// The server's character arrives 49 m from the spot the join was built for: less than a teleport, so nothing holds by itself, but cell 3 (192 m, 142 m from there) is in its
	// first view. The owner (UMantleCellStreamSubsystem::TickTerrainHold) asks, and begins the join's hold again.
	Fixture.View = FVector(5000.0, 100.0, 100.0);
	Fixture.Tick();
	TestFalse(TEXT("49 m is no teleport: nothing holds"), Fixture.Streamer->IsHolding());
	TestTrue(TEXT("but the join's view there does not stand"), Fixture.Streamer->NeedsJoinHold());
	Fixture.Streamer->BeginHold(TEXT("join"));
	if (!TestTrue(TEXT("the join settles at the character"), Fixture.Settle()))
	{
		return false;
	}
	TestTrue(TEXT("cell 3 stands"), Fixture.Built(3));
	TestFalse(TEXT("and nothing more is needed"), Fixture.Streamer->NeedsJoinHold());

	// The owner's join goes on. A teleport to x = 700 m wants cells 8 (512 to 576 m, 124 m off) to 11, and waits only for 9 to 11, the ones within 100 m: cell 8's slow download
	// does not hold the player, who can see the world.
	Fixture.Streamer->NoteJoinDone();
	TestEqual(TEXT("a teleport's hold is 100 m"), Fixture.Streamer->HoldRadiusNow(), 10000.0);
	Fixture.CellAssets->SlowCells.Add(8);
	Fixture.View = FVector(70000.0, 100.0, 100.0);
	Fixture.Tick();
	TestTrue(TEXT("the teleport holds"), Fixture.Streamer->IsHolding());
	for (int32 Tick = 0; Tick < 3000 && Fixture.Streamer->IsHolding(); ++Tick)
	{
		Fixture.Tick();
	}
	TestFalse(TEXT("and lets go"), Fixture.Streamer->IsHolding());
	TestTrue(TEXT("once cells 9 to 11 stand"), Fixture.Built(9) && Fixture.Built(10) && Fixture.Built(11));
	TestFalse(TEXT("without waiting for cell 8"), Fixture.Built(8));

	// The join's radius is a limit too (City Sample: its 1 km cells farther than 600 m stream in after the screen). Scaled to this row: a join radius of 120 m does not wait for
	// cell 2, 127 m off, however slow its download.
	{
		FFixture Near;
		Near.bJoinDoneOnFirstRelease = false;
		Near.CellAssets = MakeShared<FFakeCellAssets>();
		Near.CellAssets->SlowCells.Add(2);
		FMantleCellStreamSettings Settings = SteadySettings();
		Settings.JoinHoldRadiusCm = 12000.0;
		Near.StartStreamer(Settings);
		for (int32 Tick = 0; Tick < 2000 && Near.Streamer->IsHolding(); ++Tick)
		{
			Near.Tick();
		}
		TestFalse(TEXT("a 120 m join is let go"), Near.Streamer->IsHolding());
		TestTrue(TEXT("once cells 0 and 1 stand"), Near.Built(0) && Near.Built(1));
		TestFalse(TEXT("with cell 2 still on its way"), Near.Built(2));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerHoldWaitsWhileDownloadsMoveTest,
	"Mantle.Runtime.CellStream.Streamer.HoldWaitsWhileDownloadsMove",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerHoldWaitsWhileDownloadsMoveTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	// City Sample (#189): the join gave up at a fixed 60 s with 1 cell of 68 standing while files were still arriving at 2,000 a minute, and cells waiting on that queue were built
	// without hundreds of their meshes at a fixed 90 s. Now both wait as long as the downloads move, and give up only once they stand still.
	FFixture Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	Fixture.CellAssets = MakeShared<FFakeCellAssets>();
	Fixture.CellAssets->bReady = false;
	Fixture.CellAssets->bDownloadsMoving = true;
	FMantleCellStreamSettings Settings = SteadySettings();
	Settings.HoldIdleTimeoutSeconds = 0.3;
	Settings.AssetIdleTimeoutSeconds = 0.3;
	Settings.ProgressSampleSeconds = 0.02;
	Fixture.StartStreamer(Settings);

	// Five times the idle timeouts, with bytes arriving all the while: the join holds and no cell gives up on its assets.
	const double Start = FPlatformTime::Seconds();
	while (FPlatformTime::Seconds() - Start < 1.5)
	{
		Fixture.Tick();
	}
	TestTrue(TEXT("the join still holds while the downloads move"), Fixture.Streamer->IsHolding());
	TestEqual(TEXT("and no cell gave up on its assets"), Fixture.Streamer->GetStats().AssetWaitTimeouts, 0);

	// The downloads stand still: the cells give up on what has not come (and are built without it), and the join is let go.
	Fixture.CellAssets->bDownloadsMoving = false;
	const double Stopped = FPlatformTime::Seconds();
	double LetGoAfter = -1.0;
	while (FPlatformTime::Seconds() - Stopped < 3.0 && LetGoAfter < 0.0)
	{
		Fixture.Tick();
		LetGoAfter = Fixture.Streamer->IsHolding() ? -1.0 : FPlatformTime::Seconds() - Stopped;
	}
	TestTrue(TEXT("the join is let go once nothing moves"), LetGoAfter >= 0.0);
	// The last movement was seen up to one sample (0.02 s) before the downloads stopped.
	TestTrue(TEXT("not before the idle timeout"), LetGoAfter >= 0.3 - 0.05);
	TestEqual(TEXT("each of the three cells gave up on its assets once"), Fixture.Streamer->GetStats().AssetWaitTimeouts, 3);
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
		Settings.HoldIdleTimeoutSeconds = 0.3;
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

// ---- Far sets (stage 3a) ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// The same row of twelve cells (64 m squares, drawDistance 150 m, so each reaches 450 m with a twin) with far sets on. Each cell file holds three cubes and a list of 250 instances, all with drawDistance 150 m:
// each has a twin (the list keeps every second instance: 125). The meshes are the engine's own (no Nanite needed: bRequireNanite is off here; the real-mesh rule is for uploaded ones).

namespace MantleCellStreamerTestPrivate
{
	constexpr int32 FarHolderBaseForTest = 1 << 30;

	FMantleCellStreamSettings FarSettings()
	{
		FMantleCellStreamSettings Settings = SteadySettings();
		Settings.FarSets.bEnabled = true;
		Settings.FarSets.bRequireNanite = false;
		Settings.FarSets.ThinnedStride = 2;
		return Settings;
	}

	/** Ticks (auditing the twins after every tick) until Done says so or MaxTicks pass; the audit's double draws and gaps are added to the two counters. */
	bool TickAuditing(FFixture& Fixture, const TFunction<bool()>& Done, const int32 MaxTicks, int32& InOutDoubleDraws, int32& InOutGaps)
	{
		for (int32 Tick = 0; Tick < MaxTicks; ++Tick)
		{
			Fixture.Tick();
			const FMantleFarAudit Audit = Fixture.Streamer->AuditFarSets(TEXT("test"));
			InOutDoubleDraws += Audit.DoubleDraws;
			InOutGaps += Audit.Gaps;
			if (Done())
			{
				return true;
			}
		}
		return false;
	}

	bool FarStands(const FFixture& Fixture, const int32 Cell, const int32 Shown)
	{
		const FMantleCellStreamer::FFarSetView View = Fixture.Streamer->GetFarSetView(Cell);
		return View.bExists && View.State == 3 && View.Built == View.Entities && View.Shown == Shown;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleFarSetStandsPastTheDrawDistanceTest,
	"Mantle.Runtime.FarSet.Streamer.StandsPastTheDrawDistance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFarSetStandsPastTheDrawDistanceTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	FFixture Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	Fixture.CellAssets = MakeShared<FFakeCellAssets>();
	Fixture.StartStreamer(FarSettings());
	int32 DoubleDraws = 0;
	int32 Gaps = 0;

	// From x = 1 m: cells 0, 1 and 2 are within 150 m and are the real flow's. A twin reaches 450 m from its cell's box: cells 3 to 7 (cell 7 starts at 448 m, 447 m off) are far-only; cell 8 (512 m) is not.
	// A twin is built for a cell more than half its drawDistance (75 m) away: cell 2 (127 m) has one, cells 0 and 1 (0 and 63 m) do not yet.
	const bool bFarBuilt = TickAuditing(Fixture, [&Fixture]()
	{
		return FarStands(Fixture, 2, 0) && FarStands(Fixture, 3, 4) && FarStands(Fixture, 4, 4) && FarStands(Fixture, 5, 4) && FarStands(Fixture, 6, 4) && FarStands(Fixture, 7, 4)
			&& Fixture.Streamer->IsCellBuilt(0) && Fixture.Streamer->IsCellBuilt(1) && Fixture.Streamer->IsCellBuilt(2);
	}, 3000, DoubleDraws, Gaps);
	if (!TestTrue(TEXT("the real cells stand and the twins of cells 2 to 7 are built"), bFarBuilt))
	{
		for (int32 Cell = 0; Cell < CellCount; ++Cell)
		{
			const FMantleCellStreamer::FFarSetView View = Fixture.Streamer->GetFarSetView(Cell);
			AddInfo(FString::Printf(TEXT("cell %d: exists %d state %d entities %d built %d shown %d wanted %d"), Cell, View.bExists, View.State, View.Entities, View.Built, View.Shown, View.bWanted));
		}
		return false;
	}
	// Cell 2 is wanted and stands: its twin is built and not drawn (the real entities draw). Cells 3 to 7 are not wanted: their twins are drawn. Cell 8 and up have none.
	TestTrue(TEXT("cell 2's twin is hidden: the real cell is there"), Fixture.Streamer->GetFarSetView(2).bWanted);
	TestFalse(TEXT("cell 3 is not wanted"), Fixture.Streamer->GetFarSetView(3).bWanted);
	TestFalse(TEXT("cells 0 and 1 have a twin made but not built (they are inside half the drawDistance)"), Fixture.Streamer->GetFarSetView(0).Built > 0 || Fixture.Streamer->GetFarSetView(1).Built > 0);
	TestTrue(TEXT("cell 0 and 1 have one made"), Fixture.Streamer->GetFarSetView(0).bExists && Fixture.Streamer->GetFarSetView(1).bExists);
	for (int32 Cell = 8; Cell < CellCount; ++Cell)
	{
		TestFalse(FString::Printf(TEXT("cell %d is past the reach"), Cell), Fixture.Streamer->GetFarSetView(Cell).bExists);
	}
	// Four actors a real cell and four a twin: three real cells, six twins (cells 2 to 7). Instances: 750 real, and every second instance of the list in each twin: 125 x 6.
	TestEqual(TEXT("actors"), Fixture.StreamedActors(), 3 * ActorsPerCell + 6 * 4);
	TestEqual(TEXT("instances"), Fixture.CountInstances(), 750 + 6 * 125);
	TestEqual(TEXT("six twins built"), Fixture.Streamer->GetStats().FarSetsBuiltEver, static_cast<int64>(6));
	TestEqual(TEXT("twin entities built"), Fixture.Streamer->GetStats().FarEntitiesBuilt, static_cast<int64>(24));
	TestEqual(TEXT("twin instances built"), Fixture.Streamer->GetStats().FarInstancesBuilt, static_cast<int64>(6 * 125));
	// The far cells' assets are held under the far holder, one holder a twin that is being built or stands (cells 2 to 7); the real cells' under theirs.
	for (int32 Cell = 2; Cell <= 7; ++Cell)
	{
		TestTrue(FString::Printf(TEXT("cell %d's twin holds its assets"), Cell), Fixture.CellAssets->Held.Contains(FarHolderBaseForTest + Cell));
	}
	TestFalse(TEXT("cell 0's twin asked for none"), Fixture.CellAssets->Held.Contains(FarHolderBaseForTest + 0));

	// Far away (1 km past the last cell): every real cell goes, the twins of cells 8 to 11 (from 230 to 424 m: inside 450 m) are the whole picture, and the twins of cells that are now past 450 x 1.2 = 540 m are let go.
	Fixture.View = FVector(100000.0, 100.0, 100.0);
	const bool bMoved = TickAuditing(Fixture, [&Fixture]()
	{
		return FarStands(Fixture, 8, 4) && FarStands(Fixture, 9, 4) && FarStands(Fixture, 10, 4) && FarStands(Fixture, 11, 4)
			&& !Fixture.Streamer->GetFarSetView(0).bExists && !Fixture.Streamer->GetFarSetView(6).bExists
			&& Fixture.Streamer->GetStats().CellsBuilt == 0 && Fixture.Streamer->GetStats().CellsRemoving == 0;
	}, 4000, DoubleDraws, Gaps);
	if (!TestTrue(TEXT("the view's new place has the far ring of cells 8 to 11"), bMoved))
	{
		for (int32 Cell = 0; Cell < CellCount; ++Cell)
		{
			const FMantleCellStreamer::FFarSetView View = Fixture.Streamer->GetFarSetView(Cell);
			AddInfo(FString::Printf(TEXT("cell %d: exists %d state %d entities %d built %d shown %d wanted %d"), Cell, View.bExists, View.State, View.Entities, View.Built, View.Shown, View.bWanted));
		}
		return false;
	}
	TestFalse(TEXT("a twin past its release distance is gone with its assets: cell 6"), Fixture.CellAssets->Held.Contains(FarHolderBaseForTest + 6));
	TestTrue(TEXT("a standing one holds them: cell 9"), Fixture.CellAssets->Held.Contains(FarHolderBaseForTest + 9));
	TestEqual(TEXT("no double draw at any tick"), DoubleDraws, 0);
	TestEqual(TEXT("no gap at any tick"), Gaps, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleFarSetHandOffTest,
	"Mantle.Runtime.FarSet.Streamer.HandOffLeavesNoGap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFarSetHandOffTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	FFixture Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	Fixture.CellAssets = MakeShared<FFakeCellAssets>();
	Fixture.View = FVector(100000.0, 100.0, 100.0);
	Fixture.StartStreamer(FarSettings());
	int32 DoubleDraws = 0;
	int32 Gaps = 0;

	// From 100 m past the last cell (x = 1,000 m): nothing is the real flow's; cells 8 to 11 are far-only, drawn.
	if (!TestTrue(TEXT("the far ring stands"), TickAuditing(Fixture, [&Fixture]()
	{
		return FarStands(Fixture, 8, 4) && FarStands(Fixture, 9, 4) && FarStands(Fixture, 10, 4) && FarStands(Fixture, 11, 4);
	}, 4000, DoubleDraws, Gaps)))
	{
		return false;
	}
	TestEqual(TEXT("no real cell stands"), Fixture.Streamer->GetStats().CellsBuilt, 0);
	TestEqual(TEXT("nothing was handed off yet"), Fixture.Streamer->GetStats().FarHandOffs, static_cast<int64>(0));

	// Into cell 11 (x = 760 m): cells 9, 10 and 11 are now within 150 m (cell 9 ends at 640 m, 120 m off) and come as real cells; their twins are drawn until each real entity stands,
	// and each is hidden in the step its real entity stands: audited after every tick, never twice and never none.
	Fixture.View = FVector(76000.0, 100.0, 100.0);
	const bool bBuilt = TickAuditing(Fixture, [&Fixture]()
	{
		return Fixture.Streamer->IsCellBuilt(9) && Fixture.Streamer->IsCellBuilt(10) && Fixture.Streamer->IsCellBuilt(11) && !Fixture.Streamer->IsHolding();
	}, 4000, DoubleDraws, Gaps);
	TestTrue(TEXT("the real cells 9, 10 and 11 stand"), bBuilt);
	TestEqual(TEXT("each of the twelve real entities handed its twin off"), Fixture.Streamer->GetStats().FarHandOffs, static_cast<int64>(12));
	for (int32 Cell = 9; Cell <= 11; ++Cell)
	{
		const FMantleCellStreamer::FFarSetView View = Fixture.Streamer->GetFarSetView(Cell);
		TestTrue(FString::Printf(TEXT("cell %d is wanted"), Cell), View.bWanted);
		TestEqual(FString::Printf(TEXT("cell %d's twins are all hidden"), Cell), View.Shown, 0);
	}
	TestEqual(TEXT("cell 8 (more than 150 m off, so not wanted) keeps drawing its twin"), Fixture.Streamer->GetFarSetView(8).bWanted ? -1 : Fixture.Streamer->GetFarSetView(8).Shown, 4);

	// Back out to 1 km: the real cells are released after their twins have been drawn (a cell that stands but is not wanted draws its twin), and the audit stays clean all the way.
	Fixture.View = FVector(100000.0, 100.0, 100.0);
	const bool bBack = TickAuditing(Fixture, [&Fixture]()
	{
		return Fixture.Streamer->GetStats().CellsBuilt == 0 && Fixture.Streamer->GetStats().CellsRemoving == 0
			&& FarStands(Fixture, 9, 4) && FarStands(Fixture, 10, 4) && FarStands(Fixture, 11, 4);
	}, 4000, DoubleDraws, Gaps);
	TestTrue(TEXT("the real cells are gone and the twins are all that is drawn"), bBack);
	TestEqual(TEXT("no double draw at any tick"), DoubleDraws, 0);
	TestEqual(TEXT("no gap at any tick"), Gaps, 0);
	const FMantleFarAudit Last = Fixture.Streamer->AuditFarSets(TEXT("end"));
	TestTrue(TEXT("the last audit looked at the twins"), Last.Entities >= 16);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleFarSetSolidStreamerTest,
	"Mantle.Runtime.FarSet.Streamer.NotForTheSolidStreamer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFarSetSolidStreamerTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	FFixture Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	// A standalone game's solid streamer keeps every solid cell and draws nothing by distance: it makes no twins even with the switch on (it is the picture streamer's).
	FMantleCellStreamSettings Settings = FarSettings();
	Settings.Entities = EMantleCellEntities::SolidOnly;
	Settings.bKeepEveryCell = true;
	Settings.HoldIdleTimeoutSeconds = 600.0;
	Fixture.StartStreamer(Settings);
	int32 DoubleDraws = 0;
	int32 Gaps = 0;
	TestTrue(TEXT("every cell stands"), TickAuditing(Fixture, [&Fixture]() { return Fixture.Streamer->IsEverythingBuilt(); }, 4000, DoubleDraws, Gaps));
	for (int32 Cell = 0; Cell < CellCount; ++Cell)
	{
		TestFalse(FString::Printf(TEXT("cell %d has no far set"), Cell), Fixture.Streamer->GetFarSetView(Cell).bExists);
	}
	TestEqual(TEXT("none made"), Fixture.Streamer->GetStats().FarSetsMade, static_cast<int64>(0));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleFarSetChangedEntityTest,
	"Mantle.Runtime.FarSet.Streamer.ChangedEntitiesHaveNoTwin",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFarSetChangedEntityTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	FFixture Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	Fixture.CellAssets = MakeShared<FFakeCellAssets>();
	Fixture.StartStreamer(FarSettings());
	// What the script world says: an entity a script or the server changed here (destroyed, hidden, moved, given another look) has no far copy. Two are changed from the start: a cube of cell 4
	// and the list of cell 6.
	TSet<FString> Changed;
	Changed.Add(TEXT("ent_c4_0"));
	Changed.Add(TEXT("ent_ism_c6"));
	Fixture.Streamer->SetEntityChangedQuery([&Changed](const FString& EntityId) { return Changed.Contains(EntityId); });
	// The serial the script world keeps: it moves when something is changed, and the twins are asked about only then.
	uint64 Serial = 1;
	Fixture.Streamer->SetEntityChangeSerialQuery([&Serial]() { return Serial; });
	int32 DoubleDraws = 0;
	int32 Gaps = 0;

	// From x = 1 m: the real cells are 0 to 2; cells 3 to 7 are far-only. Cells 4 and 6 have three twins each, the others four; none of the changed entities has one.
	const bool bFar = TickAuditing(Fixture, [&Fixture]()
	{
		return FarStands(Fixture, 2, 0) && FarStands(Fixture, 3, 4) && FarStands(Fixture, 4, 3) && FarStands(Fixture, 5, 4) && FarStands(Fixture, 6, 3) && FarStands(Fixture, 7, 4)
			&& Fixture.Streamer->IsCellBuilt(0) && Fixture.Streamer->IsCellBuilt(1) && Fixture.Streamer->IsCellBuilt(2);
	}, 3000, DoubleDraws, Gaps);
	if (!TestTrue(TEXT("the twins of cells 2 to 7 stand, without the changed entities"), bFar))
	{
		return false;
	}
	TestEqual(TEXT("cell 4 has three twins"), Fixture.Streamer->GetFarSetView(4).Entities, 3);
	TestEqual(TEXT("cell 6 has three twins"), Fixture.Streamer->GetFarSetView(6).Entities, 3);
	// Twelve real actors, and twins: cells 2, 3, 5 and 7 four each, cells 4 and 6 three each.
	TestEqual(TEXT("actors"), Fixture.StreamedActors(), 3 * ActorsPerCell + 4 * 4 + 2 * 3);
	// Real 750, and the lists' twins of cells 2, 3, 4, 5 and 7 (cell 6's list is changed): 5 x 125.
	TestEqual(TEXT("instances"), Fixture.CountInstances(), 750 + 5 * 125);
	TestEqual(TEXT("two were left out when the sets were made"), Fixture.Streamer->GetStats().FarEntitiesChanged, static_cast<int64>(2));

	// Nothing changed since the sets were made: thirty more ticks (a plan every quarter second of them) look at no twin.
	const int64 ScansBefore = Fixture.Streamer->GetStats().FarChangeScans;
	for (int32 Tick = 0; Tick < 30; ++Tick)
	{
		Fixture.Tick(0.1);
	}
	TestEqual(TEXT("no change, no look at the twins"), Fixture.Streamer->GetStats().FarChangeScans, ScansBefore);

	// The list of cell 5 is changed while its twin stands drawn: the twin is taken down at the next look (a plan is made every quarter second), and the audit stays clean.
	Changed.Add(TEXT("ent_ism_c5"));
	++Serial;
	const bool bTaken = TickAuditing(Fixture, [&Fixture]()
	{
		const FMantleCellStreamer::FFarSetView View = Fixture.Streamer->GetFarSetView(5);
		return View.bExists && View.Built == 3 && View.Shown == 3;
	}, 600, DoubleDraws, Gaps);
	TestTrue(TEXT("cell 5's list lost its twin"), bTaken);
	TestEqual(TEXT("one actor fewer"), Fixture.StreamedActors(), 3 * ActorsPerCell + 4 * 4 + 2 * 3 - 1);
	TestEqual(TEXT("125 instances fewer"), Fixture.CountInstances(), 750 + 4 * 125);
	TestEqual(TEXT("three counted"), Fixture.Streamer->GetStats().FarEntitiesChanged, static_cast<int64>(3));
	TestEqual(TEXT("one look more for the change"), Fixture.Streamer->GetStats().FarChangeScans, ScansBefore + 1);
	TestEqual(TEXT("no double draw at any tick"), DoubleDraws, 0);
	TestEqual(TEXT("no gap at any tick"), Gaps, 0);
	return true;
}

// ---- Live budgets (stage 8): a place over the limits loads less far, and never refuses ----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerBudgetLoadsLessFarTest,
	"Mantle.Runtime.CellStream.Streamer.BudgetLoadsLessFar",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerBudgetLoadsLessFarTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	// The view stands at 220 m: cells 1 to 5 are within 150 m of it (cell 0 is 156 m away), 250 instances each, 1,250 in all.
	TSharedRef<int64> InstanceLimit = MakeShared<int64>(800);
	FFixture Fixture;
	Fixture.View = FVector(22000.0, 100.0, 100.0);
	FMantleCellStreamSettings Settings = SteadySettings();
	// The fixture's cells draw 150 m: the default 200 m floor would keep them whole.
	Settings.BudgetTuning.MinReachCm = 3000.0;
	Settings.BudgetLimits = [InstanceLimit]()
	{
		FMantleBudgetLimits Limits;
		Limits.Instances = *InstanceLimit;
		return Limits;
	};
	Fixture.StartStreamer(Settings);
	if (!TestTrue(TEXT("the join settles"), Fixture.Settle()))
	{
		return false;
	}
	TestTrue(TEXT("the join built the five cells round the view"), Fixture.Built(1) && Fixture.Built(2) && Fixture.Built(3) && Fixture.Built(4) && Fixture.Built(5));

	// Ten seconds: 1,250 against 800 is a pressure of 1.5625, so the ladder falls 0.1 every half second until the cells that stand fit.
	for (int32 Tick = 0; Tick < 600; ++Tick)
	{
		Fixture.Tick();
	}
	Fixture.Settle();
	const FMantleCellStreamStats& Stats = Fixture.Streamer->GetStats();
	TestTrue(TEXT("the pressure was seen: 1,250 of 800"), FMath::IsNearlyEqual(Stats.BudgetWorstPressure, 1.5625, 1.0e-6));
	TestTrue(TEXT("the ladder came down"), Stats.BudgetLowestLadder < 0.5);
	TestTrue(TEXT("the cells at the edge were let go: cells 1 and 5 are 92 m and 100 m from the view"), !Fixture.Built(1) && !Fixture.Built(5));
	TestTrue(TEXT("the cells nearest the view stand: 2, 3 and 4"), Fixture.Built(2) && Fixture.Built(3) && Fixture.Built(4));
	TestTrue(TEXT("what stands fits: 750 of 800"), Stats.BudgetInstances == 750 && Stats.BudgetPressure < 1.0);
	// The reach never goes below the floor (here 30 m, in play 200 m, which is wider than the join's 100 m): the ground round the view stands.
	TestTrue(TEXT("the ground within 30 m of the view stands"), Fixture.Streamer->IsAreaBuilt(Fixture.View, 3000.0));
	TestEqual(TEXT("nothing failed or was refused"), Stats.CellsFailed, 0);

	// The limit is lifted (a player raises the graphics setting): the ladder climbs 0.02 every half second and the cells come back.
	*InstanceLimit = 1000000;
	for (int32 Tick = 0; Tick < 1800; ++Tick)
	{
		Fixture.Tick();
	}
	Fixture.Settle();
	TestTrue(TEXT("the ladder is back at the top"), FMath::IsNearlyEqual(Fixture.Streamer->GetStats().BudgetLadder, 1.0, 1.0e-6));
	TestTrue(TEXT("and the five cells stand again"), Fixture.Built(1) && Fixture.Built(2) && Fixture.Built(3) && Fixture.Built(4) && Fixture.Built(5));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerBudgetFollowsReadMemoryTest,
	"Mantle.Runtime.CellStream.Streamer.BudgetFollowsReadMemory",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerBudgetFollowsReadMemoryTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	// BudgetLoadsLessFar's place (1,250 instances against 800), but the memory and the card the instances stand in for are read, and they are fine: nothing is let go.
	// City Sample's buildings went missing this way (122,554 solid things against 117,554, memory at 0.61). Then the memory itself goes over, and the ladder falls.
	constexpr int64 GB = 1024ll * 1024 * 1024;
	TSharedRef<int64> MemoryBytes = MakeShared<int64>(5 * GB);
	FFixture Fixture;
	Fixture.View = FVector(22000.0, 100.0, 100.0);
	FMantleCellStreamSettings Settings = SteadySettings();
	Settings.BudgetTuning.MinReachCm = 3000.0;
	Settings.BudgetTuning.MemoryLowerEverySeconds = 0.5;
	Settings.BudgetLimits = []()
	{
		FMantleBudgetLimits Limits;
		Limits.Instances = 800;
		Limits.SolidThings = 1;
		Limits.ProcessMemoryBytes = 10 * GB;
		return Limits;
	};
	Settings.BudgetMemoryBytes = [MemoryBytes]() { return *MemoryBytes; };
	Settings.BudgetVideoMemory = [](int64& OutUsedBytes, int64& OutBudgetBytes)
	{
		OutUsedBytes = 4 * GB;
		OutBudgetBytes = 16 * GB;
	};
	Fixture.StartStreamer(Settings);
	if (!TestTrue(TEXT("the join settles"), Fixture.Settle()))
	{
		return false;
	}
	for (int32 Tick = 0; Tick < 600; ++Tick)
	{
		Fixture.Tick();
	}
	Fixture.Settle();
	{
		const FMantleCellStreamStats& Stats = Fixture.Streamer->GetStats();
		TestTrue(TEXT("the counts were over (1,250 of 800) but read memory (0.5) decides: the ladder stays at the top"), FMath::IsNearlyEqual(Stats.BudgetLowestLadder, 1.0, 1.0e-6));
		TestTrue(TEXT("the pressure seen is the memory's and the card's (0.5 at most)"), Stats.BudgetWorstPressure <= 0.5 + 1.0e-6);
		TestTrue(TEXT("all five cells stand"), Fixture.Built(1) && Fixture.Built(2) && Fixture.Built(3) && Fixture.Built(4) && Fixture.Built(5));
	}

	// The process's memory goes over its cap: the memory itself lowers the ladder.
	*MemoryBytes = 12 * GB;
	for (int32 Tick = 0; Tick < 600; ++Tick)
	{
		Fixture.Tick();
	}
	TestTrue(TEXT("memory over its cap brings the ladder down"), Fixture.Streamer->GetStats().BudgetLowestLadder < 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerBudgetDoesNotCycleTest,
	"Mantle.Runtime.CellStream.Streamer.BudgetDoesNotRebuildACellThatDoesNotFit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerBudgetDoesNotCycleTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	// A limit of 950: five cells (1,250) are over it, three (750, a pressure of 0.79) are under 0.8 where the ladder wants to rise. A fourth cell (1,000) would be over again, so a rise
	// that brought it back would build it, find the limit passed, let it go and want it again. The ladder must see that coming and hold.
	FFixture Fixture;
	Fixture.View = FVector(22000.0, 100.0, 100.0);
	FMantleCellStreamSettings Settings = SteadySettings();
	Settings.BudgetTuning.MinReachCm = 3000.0;
	Settings.BudgetLimits = []()
	{
		FMantleBudgetLimits Limits;
		Limits.Instances = 950;
		return Limits;
	};
	Fixture.StartStreamer(Settings);
	if (!TestTrue(TEXT("the join settles"), Fixture.Settle()))
	{
		return false;
	}
	const int64 BuiltAtStart = Fixture.Streamer->GetStats().CellsBuiltEver;
	TestEqual(TEXT("five cells were built for the join"), BuiltAtStart, static_cast<int64>(5));
	for (int32 Tick = 0; Tick < 1800; ++Tick)
	{
		Fixture.Tick();
	}
	Fixture.Settle();
	const FMantleCellStreamStats& Stats = Fixture.Streamer->GetStats();
	TestTrue(TEXT("cells 2, 3 and 4 stand"), Fixture.Built(2) && Fixture.Built(3) && Fixture.Built(4));
	TestTrue(TEXT("cells 1 and 5 were let go and not wanted back"), !Fixture.Built(1) && !Fixture.Built(5));
	TestEqual(TEXT("no cell was built again in 30 s"), Stats.CellsBuiltEver, BuiltAtStart);
	TestTrue(TEXT("the ladder stays low: a fourth cell would need a reach of 92 m (a scale of 0.61)"), Stats.BudgetLadder < 0.3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerBudgetTexturesTest,
	"Mantle.Runtime.CellStream.Streamer.BudgetCountsTextures",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerBudgetTexturesTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	// #189: a texture-heavy place. Each standing cell holds 1 GB of textures (what FMantlePlaceAssets::GetLiveTextureBytes would say) against a limit of 3.8 GB, with every count far
	// under its limit. Five cells (5 GB) are over it, so the ladder falls until three stand (3 GB, 0.79). That is under 0.8, where the ladder wants to rise, but a fourth cell is
	// projected at the standing cells' average (1 GB: 4 GB, 1.05), so it holds rather than build the cell, find the limit passed and let it go again, round and round.
	constexpr int64 GB = 1024ll * 1024 * 1024;
	FFixture Fixture;
	Fixture.View = FVector(22000.0, 100.0, 100.0);
	FMantleCellStreamSettings Settings = SteadySettings();
	Settings.BudgetTuning.MinReachCm = 3000.0;
	Settings.BudgetLimits = []()
	{
		FMantleBudgetLimits Limits;
		Limits.Instances = 100000;
		Limits.SolidThings = 100000;
		Limits.TextureBytes = 3 * GB + GB * 4 / 5;
		return Limits;
	};
	FFixture* const FixturePtr = &Fixture;
	Settings.BudgetTextureBytes = [FixturePtr]()
	{
		return FixturePtr->Streamer.IsValid() ? static_cast<int64>(FixturePtr->Streamer->GetStats().CellsBuilt) * GB : int64(0);
	};
	Fixture.StartStreamer(Settings);
	if (!TestTrue(TEXT("the join settles"), Fixture.Settle()))
	{
		return false;
	}
	const int64 BuiltAtStart = Fixture.Streamer->GetStats().CellsBuiltEver;
	TestEqual(TEXT("five cells were built for the join"), BuiltAtStart, static_cast<int64>(5));
	for (int32 Tick = 0; Tick < 1800; ++Tick)
	{
		Fixture.Tick();
	}
	Fixture.Settle();
	const FMantleCellStreamStats& Stats = Fixture.Streamer->GetStats();
	TestTrue(TEXT("the ladder fell on the textures alone"), Stats.BudgetLowestLadder < 1.0);
	TestTrue(TEXT("cells 2, 3 and 4 stand"), Fixture.Built(2) && Fixture.Built(3) && Fixture.Built(4));
	TestTrue(TEXT("cells 1 and 5 were let go and not wanted back"), !Fixture.Built(1) && !Fixture.Built(5));
	TestEqual(TEXT("3 GB of textures counted at the last look"), Stats.BudgetTextureBytes, 3 * GB);
	TestEqual(TEXT("no cell was built again in 30 s"), Stats.CellsBuiltEver, BuiltAtStart);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerBudgetTextureDetailTest,
	"Mantle.Runtime.CellStream.Streamer.BudgetShortensTextureDetail",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerBudgetTextureDetailTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	// #189, City Sample's join: textures over their limit even with the ladder at 0. Each standing cell holds 0.2 GB of low copies and 1 GB of full files when full detail reaches
	// its whole 150 m, less with the circle's area (reach / 150 m, squared). The limit is 2 GB. Five cells (6 GB) take the ladder to 0, and the three cells round the view
	// (3.6 GB) are still over: below the ladder, the full-detail reach comes down, 150 m to 112.5 m (2.29 GB) to 84.4 m (1.55 GB), and there it holds (0.77: a step back up
	// would be 1.37 by its projection). The join's hold waits only for what the ladder still wants. With the limit lifted the reach comes back to 150 m before the ladder rises.
	constexpr int64 GB = 1024ll * 1024 * 1024;
	TSharedRef<int64> TextureLimit = MakeShared<int64>(2 * GB);
	FFixture Fixture;
	Fixture.View = FVector(22000.0, 100.0, 100.0);
	Fixture.CellAssets = MakeShared<FFakeCellAssets>();
	FMantleCellStreamSettings Settings = SteadySettings();
	Settings.BudgetTuning.MinReachCm = 3000.0;
	Settings.BudgetLimits = [TextureLimit]()
	{
		FMantleBudgetLimits Limits;
		Limits.Instances = 100000;
		Limits.SolidThings = 100000;
		Limits.TextureBytes = *TextureLimit;
		return Limits;
	};
	FFixture* const FixturePtr = &Fixture;
	Settings.BudgetTextureBytes = [FixturePtr]()
	{
		if (!FixturePtr->Streamer.IsValid())
		{
			return int64(0);
		}
		const double Share = FMath::Square(FixturePtr->CellAssets->TextureReach / 15000.0);
		return static_cast<int64>(FixturePtr->Streamer->GetStats().CellsBuilt * (0.2 + Share) * static_cast<double>(GB));
	};
	Fixture.StartStreamer(Settings);
	if (!TestTrue(TEXT("the join settles"), Fixture.Settle()))
	{
		return false;
	}
	TestTrue(TEXT("the join waited for no more than the ladder wants: the ground round the view stands"), Fixture.Streamer->IsAreaBuilt(Fixture.View, 3000.0));
	for (int32 Tick = 0; Tick < 1200; ++Tick)
	{
		Fixture.Tick();
	}
	Fixture.Settle();
	{
		const FMantleCellStreamStats& Stats = Fixture.Streamer->GetStats();
		TestEqual(TEXT("the ladder is at the bottom"), Stats.BudgetLadder, 0.0);
		TestTrue(TEXT("cells 2, 3 and 4 stand"), Fixture.Built(2) && Fixture.Built(3) && Fixture.Built(4));
		TestEqual(TEXT("full detail came down two steps, to 84.4 m"), Fixture.CellAssets->TextureReach, 8437.5);
		TestEqual(TEXT("the stats say the same"), Stats.BudgetTextureDetailCm, 8437.5);
		TestTrue(TEXT("the textures fit at the last look"), Stats.BudgetTextureBytes <= *TextureLimit && Stats.BudgetPressure < 1.0);
	}

	// Ten more seconds at the same use: it holds (no cycle between 84 m and 112 m).
	for (int32 Tick = 0; Tick < 600; ++Tick)
	{
		Fixture.Tick();
		if (Fixture.CellAssets->TextureReach != 8437.5)
		{
			break;
		}
	}
	TestEqual(TEXT("it holds at 84.4 m"), Fixture.CellAssets->TextureReach, 8437.5);

	// The limit is lifted: full detail comes back a step every 5 s, and the ladder waits for it.
	*TextureLimit = 100 * GB;
	double ReachWhenTheLadderRose = -1.0;
	for (int32 Tick = 0; Tick < 1800; ++Tick)
	{
		Fixture.Tick();
		if (ReachWhenTheLadderRose < 0.0 && Fixture.Streamer->GetStats().BudgetLadder > 0.0)
		{
			ReachWhenTheLadderRose = Fixture.CellAssets->TextureReach;
		}
	}
	TestEqual(TEXT("full detail reaches its whole 150 m again"), Fixture.CellAssets->TextureReach, 15000.0);
	TestEqual(TEXT("the ladder rose only once it had"), ReachWhenTheLadderRose, 15000.0);
	TestEqual(TEXT("nothing failed or was refused"), Fixture.Streamer->GetStats().CellsFailed, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerBudgetWatchesTheCardTest,
	"Mantle.Runtime.CellStream.Streamer.BudgetWatchesTheCard",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerBudgetWatchesTheCardTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	// #189: the card as the driver sees it, faked. The place's textures are well inside their own share (a limit of 100 GB), but the driver gives the game 10 GB, so the pressure is 1
	// at 9 GB, and the rest of the game (avatars, vehicles, Lumen, other programs: Others) uses its part. Each standing cell's textures are 0.2 GB of low copies and 1 GB of full
	// files over the whole 150 m, less with the circle's area. Memory, the card's included, lowers things every half second here (MemoryLowerEverySeconds; 10 s in play).
	//   6 GB of others: five cells (12 GB, 1.33) take the ladder to 0; three (9.6 GB, 1.07) are still over, so full detail comes in to 112.5 m (8.29 GB, 0.92) and holds.
	//   A crowded server, 7.5 GB of others: 9.79 GB (1.09) -> 84.4 m (1.006) -> 63.3 m (8.63 GB, 0.96), and holds.
	//   3 GB of others: 4.13 GB (0.46), and the textures' growth would still fit, so full detail comes back to 150 m, and only then does the ladder rise.
	constexpr int64 GB = 1024ll * 1024 * 1024;
	TSharedRef<double> Others = MakeShared<double>(6.0);
	FFixture Fixture;
	Fixture.View = FVector(22000.0, 100.0, 100.0);
	Fixture.CellAssets = MakeShared<FFakeCellAssets>();
	FMantleCellStreamSettings Settings = SteadySettings();
	Settings.BudgetTuning.MinReachCm = 3000.0;
	Settings.BudgetTuning.MemoryLowerEverySeconds = 0.5;
	Settings.BudgetLimits = []()
	{
		FMantleBudgetLimits Limits;
		Limits.Instances = 100000;
		Limits.SolidThings = 100000;
		Limits.TextureBytes = 100 * GB;
		return Limits;
	};
	FFixture* const FixturePtr = &Fixture;
	auto TexturesGB = [FixturePtr]()
	{
		const double Share = FMath::Square(FixturePtr->CellAssets->TextureReach / 15000.0);
		return FixturePtr->Streamer.IsValid() ? FixturePtr->Streamer->GetStats().CellsBuilt * (0.2 + Share) : 0.0;
	};
	Settings.BudgetTextureBytes = [TexturesGB]() { return static_cast<int64>(TexturesGB() * static_cast<double>(GB)); };
	Settings.BudgetVideoMemory = [TexturesGB, Others](int64& OutUsedBytes, int64& OutBudgetBytes)
	{
		OutUsedBytes = static_cast<int64>((*Others + TexturesGB()) * static_cast<double>(GB));
		OutBudgetBytes = 10 * GB;
	};
	Fixture.StartStreamer(Settings);
	if (!TestTrue(TEXT("the join settles"), Fixture.Settle()))
	{
		return false;
	}
	for (int32 Tick = 0; Tick < 1200; ++Tick)
	{
		Fixture.Tick();
	}
	Fixture.Settle();
	{
		const FMantleCellStreamStats& Stats = Fixture.Streamer->GetStats();
		TestEqual(TEXT("the card took the ladder to the bottom"), Stats.BudgetLadder, 0.0);
		TestTrue(TEXT("cells 2, 3 and 4 stand"), Fixture.Built(2) && Fixture.Built(3) && Fixture.Built(4));
		TestEqual(TEXT("full detail came in one step, to 112.5 m"), Fixture.CellAssets->TextureReach, 11250.0);
		TestTrue(TEXT("the card fits at the last look (0.92)"), Stats.BudgetVideoPressure > 0.9 && Stats.BudgetVideoPressure < 1.0);
		TestEqual(TEXT("the driver's budget was read"), Stats.BudgetVideoBudgetBytes, 10 * GB);
	}

	// A crowded server: the rest of the game takes 1.5 GB more.
	*Others = 7.5;
	for (int32 Tick = 0; Tick < 600; ++Tick)
	{
		Fixture.Tick();
	}
	TestEqual(TEXT("a crowded server takes full detail in to 63.3 m"), Fixture.CellAssets->TextureReach, 6328.125);
	TestTrue(TEXT("and the card fits again"), Fixture.Streamer->GetStats().BudgetVideoPressure < 1.0);

	// The crowd leaves.
	*Others = 3.0;
	double ReachWhenTheLadderRose = -1.0;
	for (int32 Tick = 0; Tick < 1800; ++Tick)
	{
		Fixture.Tick();
		if (ReachWhenTheLadderRose < 0.0 && Fixture.Streamer->GetStats().BudgetLadder > 0.0)
		{
			ReachWhenTheLadderRose = Fixture.CellAssets->TextureReach;
		}
	}
	TestEqual(TEXT("full detail reaches its whole 150 m again"), Fixture.CellAssets->TextureReach, 15000.0);
	TestEqual(TEXT("the ladder rose only once it had"), ReachWhenTheLadderRose, 15000.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerPoolGivesWayFirstTest,
	"Mantle.Runtime.CellStream.Streamer.PoolGivesWayFirst",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerPoolGivesWayFirstTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	// #189: streamed textures sit in the engine's pool, which the budget sizes. The card, faked: the driver gives the game 10 GB (a cushion of VideoUseShare of it), the textures'
	// share is 8 GB, 0.5 GB of them are built whole, and the streamed ones would fill 6 GB of pool. The engine keeps no more in the pool than the pool's size.
	//   3.9 GB of others: 10.4 GB on the card, over the cushion: the pool gives way (to about 4.6 GB, where it fits) and the ladder and full-detail radius stay where they are.
	//   9.5 GB of others: the pool gives way to its floor, and only then does the card lower the ladder and the radius.
	constexpr int64 GB = 1024ll * 1024 * 1024;
	TSharedRef<double> Others = MakeShared<double>(3.9);
	TSharedRef<int64> Pool = MakeShared<int64>(0);
	FFixture Fixture;
	Fixture.View = FVector(22000.0, 100.0, 100.0);
	Fixture.CellAssets = MakeShared<FFakeCellAssets>();
	FMantleCellStreamSettings Settings = SteadySettings();
	Settings.BudgetTuning.MinReachCm = 3000.0;
	Settings.BudgetTuning.MemoryLowerEverySeconds = 0.5;
	Settings.BudgetLimits = []()
	{
		FMantleBudgetLimits Limits;
		Limits.Instances = 100000;
		Limits.SolidThings = 100000;
		Limits.TextureBytes = 8 * GB;
		return Limits;
	};
	Settings.BudgetTextureBytes = []() { return GB / 2; };
	Settings.ApplyTexturePool = [Pool](const int64 Bytes) { *Pool = Bytes; };
	Settings.BudgetVideoMemory = [Others, Pool](int64& OutUsedBytes, int64& OutBudgetBytes)
	{
		const int64 Streamed = *Pool > 0 ? FMath::Min(*Pool, 6 * GB) : 0;
		OutUsedBytes = static_cast<int64>(*Others * static_cast<double>(GB)) + GB / 2 + Streamed;
		OutBudgetBytes = 10 * GB;
	};
	Fixture.StartStreamer(Settings);
	if (!TestTrue(TEXT("the join settles"), Fixture.Settle()))
	{
		return false;
	}
	const double LadderAtStart = Fixture.Streamer->GetStats().BudgetLadder;
	int64 SmallestPool = MAX_int64;
	for (int32 Tick = 0; Tick < 600; ++Tick)
	{
		Fixture.Tick();
		// The join can settle before the budget's first look (every StepSeconds), so the pool may still be unset (0) here.
		if (*Pool > 0)
		{
			SmallestPool = FMath::Min(SmallestPool, *Pool);
		}
	}
	{
		const FMantleCellStreamStats& Stats = Fixture.Streamer->GetStats();
		AddInfo(FString::Printf(TEXT("pool %.2f GB, smallest %.2f GB"), *Pool / static_cast<double>(GB), SmallestPool / static_cast<double>(GB)));
		TestTrue(TEXT("the pool was set"), *Pool > 0);
		TestEqual(TEXT("the stats say the pool"), Stats.BudgetTexturePoolBytes, *Pool);
		TestTrue(TEXT("the pool gave way below what the share leaves (7.5 GB)"), *Pool < 7 * GB + GB / 2);
		TestTrue(TEXT("and stayed above its floor"), SmallestPool > FMantleStreamBudget::MinTexturePoolBytes);
		TestEqual(TEXT("the ladder did not move"), Stats.BudgetLadder, LadderAtStart);
		TestEqual(TEXT("full detail still reaches 150 m"), Fixture.CellAssets->TextureReach, 15000.0);
	}

	// The rest of the game takes 5.5 GB more: no pool fits, so it goes to its floor first, then the ladder and the radius come in.
	*Others = 9.5;
	int64 PoolWhenTheLadderMoved = -1;
	double ReachWhenTheLadderMoved = -1.0;
	for (int32 Tick = 0; Tick < 1200; ++Tick)
	{
		Fixture.Tick();
		if (PoolWhenTheLadderMoved < 0 && Fixture.Streamer->GetStats().BudgetLadder < LadderAtStart)
		{
			PoolWhenTheLadderMoved = *Pool;
			ReachWhenTheLadderMoved = Fixture.CellAssets->TextureReach;
		}
	}
	TestEqual(TEXT("the pool was at its floor when the ladder first moved"), PoolWhenTheLadderMoved, FMantleStreamBudget::MinTexturePoolBytes);
	TestEqual(TEXT("and full detail still reached 150 m then"), ReachWhenTheLadderMoved, 15000.0);
	TestEqual(TEXT("then the card took the ladder to the bottom"), Fixture.Streamer->GetStats().BudgetLadder, 0.0);
	TestTrue(TEXT("and full detail came in"), Fixture.CellAssets->TextureReach < 15000.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerJoinHoldFollowsTheLadderTest,
	"Mantle.Runtime.CellStream.Streamer.JoinHoldFollowsTheLadder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerJoinHoldFollowsTheLadderTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	// #189: the join's hold waits only for what the ladder still wants. Cells 1 and 5 (92 m and 100 m from the view) never get their assets, while the downloads keep moving, so
	// no wait gives up on them. 1,250 instances against 800 take the ladder down during the hold; once their reach no longer takes in cells 1 and 5 they are let go, and the
	// hold ends because what is left of its area stands, not because anything timed out.
	FFixture Fixture;
	Fixture.View = FVector(22000.0, 100.0, 100.0);
	Fixture.CellAssets = MakeShared<FFakeCellAssets>();
	Fixture.CellAssets->bDownloadsMoving = true;
	Fixture.CellAssets->SlowCells.Add(1);
	Fixture.CellAssets->SlowCells.Add(5);
	FMantleCellStreamSettings Settings = SteadySettings();
	Settings.BudgetTuning.MinReachCm = 3000.0;
	Settings.BudgetLimits = []()
	{
		FMantleBudgetLimits Limits;
		Limits.Instances = 800;
		Limits.SolidThings = 100000;
		return Limits;
	};
	Fixture.StartStreamer(Settings);
	TestTrue(TEXT("the join holds"), Fixture.Streamer->IsHolding());
	bool bEnded = false;
	for (int32 Tick = 0; Tick < 1200 && !bEnded; ++Tick)
	{
		Fixture.Tick();
		bEnded = !Fixture.Streamer->IsHolding();
	}
	TestTrue(TEXT("the hold ended within 20 s"), bEnded);
	const FMantleCellStreamStats& Stats = Fixture.Streamer->GetStats();
	TestTrue(TEXT("the ladder came down during the hold"), Stats.BudgetLowestLadder < 0.5);
	TestTrue(TEXT("cells 2, 3 and 4 stand"), Fixture.Built(2) && Fixture.Built(3) && Fixture.Built(4));
	TestTrue(TEXT("cells 1 and 5 were not waited for"), !Fixture.Built(1) && !Fixture.Built(5));
	TestEqual(TEXT("no asset wait gave up"), Stats.AssetWaitTimeouts, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerBudgetWithinLimitsTest,
	"Mantle.Runtime.CellStream.Streamer.BudgetWithinLimitsChangesNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerBudgetWithinLimitsTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	// 1,250 instances against a limit of 1,500 (0.83: between 0.8 and 1, so the ladder neither falls nor rises): the five cells stand and the ladder stays at the top.
	FFixture Fixture;
	Fixture.View = FVector(22000.0, 100.0, 100.0);
	FMantleCellStreamSettings Settings = SteadySettings();
	Settings.BudgetTuning.MinReachCm = 3000.0;
	Settings.BudgetLimits = []()
	{
		FMantleBudgetLimits Limits;
		Limits.Instances = 1500;
		Limits.SolidThings = 100000;
		return Limits;
	};
	Fixture.StartStreamer(Settings);
	Fixture.Settle();
	for (int32 Tick = 0; Tick < 600; ++Tick)
	{
		Fixture.Tick();
	}
	const FMantleCellStreamStats& Stats = Fixture.Streamer->GetStats();
	TestTrue(TEXT("the budget was looked at"), Stats.BudgetLooks >= 10);
	TestEqual(TEXT("the ladder never moved"), Stats.BudgetLowestLadder, 1.0);
	TestTrue(TEXT("five cells stand"), Fixture.Built(1) && Fixture.Built(2) && Fixture.Built(3) && Fixture.Built(4) && Fixture.Built(5));
	TestEqual(TEXT("1,250 instances counted"), Stats.BudgetInstances, static_cast<int64>(1250));
	TestEqual(TEXT("and 1,265 solid things (253 a cell)"), Stats.BudgetSolidThings, static_cast<int64>(1265));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamerBudgetNeverLimitsKeepEveryCellTest,
	"Mantle.Runtime.CellStream.Streamer.BudgetNeverLimitsTheSolidStreamer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamerBudgetNeverLimitsKeepEveryCellTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamerTestPrivate;
	// A streamer that keeps every cell (a standalone game's solid things, as on a server) is not limited, whatever the limits say: collision stands.
	FFixture Fixture;
	FMantleCellStreamSettings Settings = SteadySettings();
	Settings.bKeepEveryCell = true;
	Settings.HoldIdleTimeoutSeconds = 600.0;
	Settings.BudgetLimits = []()
	{
		FMantleBudgetLimits Limits;
		Limits.Instances = 1;
		Limits.SolidThings = 1;
		return Limits;
	};
	Fixture.StartStreamer(Settings);
	if (!TestTrue(TEXT("every cell is built"), Fixture.Settle(6000)))
	{
		return false;
	}
	for (int32 Tick = 0; Tick < 120; ++Tick)
	{
		Fixture.Tick();
	}
	TestEqual(TEXT("all twelve cells stand"), Fixture.Streamer->GetStats().CellsBuilt, CellCount);
	TestEqual(TEXT("the budget was never looked at"), Fixture.Streamer->GetStats().BudgetLooks, 0);
	TestEqual(TEXT("the ladder is at the top"), Fixture.Streamer->GetStats().BudgetLadder, 1.0);
	return true;
}


#endif // WITH_DEV_AUTOMATION_TESTS
