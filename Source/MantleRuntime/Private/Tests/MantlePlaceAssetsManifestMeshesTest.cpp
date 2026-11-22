// Copyright Joshua Eger. All Rights Reserved.
#include "Misc/AutomationTest.h"

#include "Misc/ConfigCacheIni.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MantleApiClient.h"
#include "MantleAssetStore.h"
#include "MantleCellLoader.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceParser.h"
#include "Serialization/MemoryWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

// Server plan step 2 (faster ready): a dedicated server fetches and cooks every mesh the place's manifest lists while its cells are read
// (FMantlePlaceAssets::PrepareManifestMeshes), and the place's own set, started once the cells are in, finds those meshes built instead
// of downloading, reading and cooking them again. Also the cell loader's in-flight limits from the game ini ([Mantle.CellLoader]).

// Named, not anonymous: this module builds as a unity build.
namespace MantlePlaceAssetsManifestMeshesTestPrivate
{
	const TCHAR* const UsedMesh = TEXT("ast_u00000000000000000000000000000e01");
	const TCHAR* const SpareMesh = TEXT("ast_u00000000000000000000000000000e02");
	const TCHAR* const SomeMaterial = TEXT("ast_u00000000000000000000000000000e03");

	/** A box as an MRM1 mesh, its corners at +-Half cm: another Half is other bytes, so the two meshes never stand in for each other. */
	TArray<uint8> MakeMrm1Box(const float Half)
	{
		TArray<uint8> Bytes;
		FMemoryWriter Ar(Bytes, true);
		uint8 Magic[4] = { 'M', 'R', 'M', '1' };
		Ar.Serialize(Magic, 4);
		uint32 Version = 1, Flags = 0, Vertices = 8, Indices = 36;
		Ar << Version << Flags << Vertices << Indices;
		float Bounds[6] = { -Half, -Half, -Half, Half, Half, Half };
		for (float& Value : Bounds)
		{
			Ar << Value;
		}
		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			float X = (Corner & 1) ? Half : -Half, Y = (Corner & 2) ? Half : -Half, Z = (Corner & 4) ? Half : -Half;
			Ar << X << Y << Z;
		}
		const uint32 Triangles[12][3] = {
			{ 0, 2, 1 }, { 1, 2, 3 }, { 4, 5, 6 }, { 5, 7, 6 }, { 0, 1, 4 }, { 1, 5, 4 },
			{ 2, 6, 3 }, { 3, 6, 7 }, { 0, 4, 2 }, { 2, 4, 6 }, { 1, 3, 5 }, { 3, 7, 5 } };
		for (const uint32 (&Triangle)[3] : Triangles)
		{
			for (uint32 Index : Triangle)
			{
				Ar << Index;
			}
		}
		return Bytes;
	}

	/** The fake API: resolves the two meshes with the cooked hash of their bytes, writes down every id it is asked about. */
	class FMeshResolver : public IMantleAssetResolver
	{
	public:
		TMap<FString, TArray<uint8>> Meshes;
		TArray<FString> Asked;

		virtual void Resolve(const TArray<FString>& Ids, TFunction<void(const FMantleAssetResolveResult&)> OnDone) override
		{
			Asked.Append(Ids);
			FMantleAssetResolveResult Result;
			Result.bSuccess = true;
			for (const FString& Id : Ids)
			{
				const TArray<uint8>* Bytes = Meshes.Find(Id);
				if (!Bytes)
				{
					Result.Missing.Add(Id);
					continue;
				}
				FMantleUploadedAsset Asset;
				Asset.Id = Id;
				Asset.Hash = FString(TEXT("sha256:")) + FString::ChrN(64, TEXT('e'));
				Asset.Kind = TEXT("mesh");
				Asset.Status = TEXT("approved");
				Asset.CookState = TEXT("passed");
				Asset.ModerationState = TEXT("passed");
				Asset.Url = TEXT("test://") + Id;
				Asset.CookedHash = FMantleContentHash::OfBytes(*Bytes).ToString();
				Asset.CookedBytes = Bytes->Num();
				Result.Assets.Add(Asset);
			}
			OnDone(Result);
		}
	};

	/** The fake transport: serves the resolver's meshes by url and counts the downloads. */
	class FCountingFetcher : public IMantleAssetFetcher
	{
	public:
		TSharedPtr<FMeshResolver> Source;
		int32 Downloads = 0;

		virtual void Fetch(const FString& Url, const TSharedRef<FArchive>& Sink, TFunction<void(const FMantleAssetFetchResult&)> OnDone) override
		{
			FMantleAssetFetchResult Result;
			Result.bConnected = true;
			Result.StatusCode = 404;
			if (const TArray<uint8>* Bytes = Source->Meshes.Find(Url.RightChop(7)))
			{
				++Downloads;
				Sink->Serialize(const_cast<uint8*>(Bytes->GetData()), Bytes->Num());
				Result.StatusCode = 200;
			}
			OnDone(Result);
		}
	};

	/** A root with a manifest of two meshes and a material: one entity uses the first mesh (with collision); the second is only listed. */
	FMantlePlace MakeRoot()
	{
		const FString Hash = FString(TEXT("sha256:")) + FString::ChrN(64, TEXT('e'));
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_earlymesh01\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Early Meshes\",\"authorId\":\"usr_4b1d\"},\"settings\":{},\"assets\":["
				 "{\"id\":\"%s\",\"kind\":\"StaticMesh\",\"hash\":\"%s\",\"bytes\":100},"
				 "{\"id\":\"%s\",\"kind\":\"StaticMesh\",\"hash\":\"%s\",\"bytes\":100},"
				 "{\"id\":\"%s\",\"kind\":\"Material\",\"hash\":\"%s\",\"bytes\":10}],"
				 "\"entities\":[{\"id\":\"ent_used\",\"type\":\"StaticMesh\",\"parent\":null,"
				 "\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{\"mesh\":\"%s\"}}]}"),
			UsedMesh, *Hash, SpareMesh, *Hash, SomeMaterial, *Hash, UsedMesh);
		const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
		check(Parsed.bSuccess);
		return Parsed.Place;
	}

	/** A dedicated server's settings (meshes only, for collision) on a store of its own, pumped by hand. */
	FMantlePlaceAssetSettings ServerSettings(const TSharedRef<FMeshResolver>& Resolver, const TSharedRef<FCountingFetcher>& Fetcher, const FString& StoreName)
	{
		FMantleAssetStoreSettings StoreSettings;
		StoreSettings.RootDir = FPaths::AutomationTransientDir() / StoreName;
		StoreSettings.MaxAttempts = 1;
		FMantlePlaceAssetSettings Settings;
		Settings.bAutoPump = false;
		Settings.bRenderAssets = false;
		Settings.bLoadNanite = false;
		Settings.bAsyncCollisionCook = false;
		Settings.Resolver = Resolver;
		Settings.Store = FMantleAssetStore::Create(StoreSettings, Fetcher);
		return Settings;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceAssetsManifestMeshesTest,
	"Mantle.Runtime.PlaceAssets.ManifestMeshesEarly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceAssetsManifestMeshesTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceAssetsManifestMeshesTestPrivate;
	const FString Run = FGuid::NewGuid().ToString();
	const FMantlePlace Root = MakeRoot();

	// Boxes of sizes unique to this run, so no set from an earlier run (or another test) has built the same bytes.
	const float Size = 40.0f + static_cast<float>(GetTypeHash(Run) % 1000) / 100.0f;
	const TSharedRef<FMeshResolver> Resolver = MakeShared<FMeshResolver>();
	Resolver->Meshes.Add(UsedMesh, MakeMrm1Box(Size));
	Resolver->Meshes.Add(SpareMesh, MakeMrm1Box(Size + 20.0f));

	// The early set: every mesh in the manifest, the one no entity uses too, and nothing that is not a mesh.
	const TSharedRef<FCountingFetcher> EarlyFetcher = MakeShared<FCountingFetcher>();
	EarlyFetcher->Source = Resolver;
	const TSharedRef<FMantlePlaceAssets> Early = FMantlePlaceAssets::PrepareManifestMeshes(Root,
		ServerSettings(Resolver, EarlyFetcher, TEXT("ManifestMeshesEarly") / Run / TEXT("Early")), FMantlePlaceAssetsDone());
	TestTrue(TEXT("the early set completes"), Early->WaitUntilComplete(60.0f));
	TestTrue(*FString::Printf(TEXT("the early set built everything: %s"), *Early->GetResult().ErrorSummary()), Early->GetResult().bSuccess);
	TestEqual(TEXT("it built both meshes, the unused one too"), Early->GetResult().Meshes, 2);
	TestEqual(TEXT("it downloaded both"), EarlyFetcher->Downloads, 2);
	TestFalse(TEXT("it never asked about the material (a server builds none)"), Resolver->Asked.Contains(FString(SomeMaterial)));

	// The place's own set, started once the early one is done, on an empty store of its own: the mesh an entity uses is reused, not downloaded.
	Resolver->Asked.Reset();
	const TSharedRef<FCountingFetcher> PlaceFetcher = MakeShared<FCountingFetcher>();
	PlaceFetcher->Source = Resolver;
	const TSharedRef<FMantlePlaceAssets> Place = FMantlePlaceAssets::Prepare(Root,
		ServerSettings(Resolver, PlaceFetcher, TEXT("ManifestMeshesEarly") / Run / TEXT("Place")), FMantlePlaceAssetsDone());
	TestTrue(TEXT("the place's set completes"), Place->WaitUntilComplete(60.0f));
	TestTrue(*FString::Printf(TEXT("the place's set has its mesh: %s"), *Place->GetResult().ErrorSummary()), Place->GetResult().bSuccess);
	TestTrue(TEXT("it asked about the mesh its entity uses"), Resolver->Asked.Contains(FString(UsedMesh)));
	TestEqual(TEXT("and downloaded nothing: the early set had built it"), PlaceFetcher->Downloads, 0);
	FString Error;
	TestNotNull(*FString::Printf(TEXT("the mesh is live: %s"), *Error), FMantlePlaceAssets::FindLiveMesh(UsedMesh, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellLoaderSettingsFromIniTest,
	"Mantle.Runtime.Place.Cells.LoaderSettingsFromIni",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellLoaderSettingsFromIniTest::RunTest(const FString& Parameters)
{
	if (!TestNotNull(TEXT("config"), GConfig))
	{
		return false;
	}
	const TCHAR* Section = TEXT("Mantle.CellLoader");
	const TCHAR* Keys[] = { TEXT("MaxConcurrentResolves"), TEXT("MaxOutstandingFetches"), TEXT("MaxConcurrentDecodes"), TEXT("MaxPendingMB") };
	// What the game ini has now, put back at the end.
	TMap<FString, FString> Before;
	for (const TCHAR* Key : Keys)
	{
		FString Value;
		if (GConfig->GetString(Section, Key, Value, GGameIni))
		{
			Before.Add(Key, Value);
		}
	}
	ON_SCOPE_EXIT
	{
		for (const TCHAR* Key : Keys)
		{
			if (const FString* Value = Before.Find(Key))
			{
				GConfig->SetString(Section, Key, **Value, GGameIni);
			}
			else
			{
				GConfig->RemoveKey(Section, Key, GGameIni);
			}
		}
	};

	FMantlePlaceAssetSettings Assets;
	Assets.MaxResolveAttempts = 5;
	Assets.bAutoPump = false;

	GConfig->SetInt(Section, TEXT("MaxConcurrentResolves"), 7, GGameIni);
	GConfig->SetInt(Section, TEXT("MaxOutstandingFetches"), 33, GGameIni);
	GConfig->SetInt(Section, TEXT("MaxConcurrentDecodes"), 3, GGameIni);
	GConfig->SetInt(Section, TEXT("MaxPendingMB"), 64, GGameIni);
	const FMantleCellLoaderSettings Set = FMantleCellLoaderSettings::ForThisProcess(Assets);
	TestEqual(TEXT("resolves from the ini"), Set.MaxConcurrentResolves, 7);
	TestEqual(TEXT("fetches from the ini"), Set.MaxOutstandingFetches, 33);
	TestEqual(TEXT("reads from the ini"), Set.MaxConcurrentDecodes, 3);
	TestEqual(TEXT("pending bytes from the ini"), Set.MaxPendingBytes, static_cast<int64>(64) * 1024 * 1024);
	TestEqual(TEXT("the asset settings still give the resolve attempts"), Set.MaxResolveAttempts, 5);
	TestFalse(TEXT("and the pumping"), Set.bAutoPump);

	// Nothing set, or nothing above 0: the code's own limits (what FromAssetSettings gives).
	for (const TCHAR* Key : Keys)
	{
		GConfig->RemoveKey(Section, Key, GGameIni);
	}
	GConfig->SetInt(Section, TEXT("MaxConcurrentDecodes"), 0, GGameIni);
	const FMantleCellLoaderSettings Plain = FMantleCellLoaderSettings::FromAssetSettings(Assets);
	const FMantleCellLoaderSettings Unset = FMantleCellLoaderSettings::ForThisProcess(Assets);
	TestEqual(TEXT("unset resolves are the code's"), Unset.MaxConcurrentResolves, Plain.MaxConcurrentResolves);
	TestEqual(TEXT("unset fetches are the code's"), Unset.MaxOutstandingFetches, Plain.MaxOutstandingFetches);
	TestEqual(TEXT("a read limit of 0 is ignored"), Unset.MaxConcurrentDecodes, Plain.MaxConcurrentDecodes);
	TestEqual(TEXT("unset pending bytes are the code's"), Unset.MaxPendingBytes, Plain.MaxPendingBytes);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
