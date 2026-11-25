// Copyright Joshua Eger. All Rights Reserved.
#include "Misc/AutomationTest.h"

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Engine/Texture.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "MantleApiClient.h"
#include "MantleAssetStore.h"
#include "MantlePlaceAssets.h"
#include "MantlePlaceParser.h"
#include "Serialization/MemoryWriter.h"

#if WITH_DEV_AUTOMATION_TESTS

// The minimap's picture (#167): its tiles are Texture assets that no entity uses, so nothing downloads while the minimap is off. Neither the
// join's set (Prepare) nor the manifest-only pass (PrepareManifestOnly) takes them; they are asked for after the place is running
// (PrepareTextures) and then answer FindLiveTexture like any uploaded texture.

// Named, not anonymous: this module builds as a unity build.
namespace MantlePlaceAssetsMinimapTilesTestPrivate
{
	const TCHAR* const UsedMesh = TEXT("ast_u00000000000000000000000000000a01");
	const TCHAR* const SpareMesh = TEXT("ast_u00000000000000000000000000000a02");
	const TCHAR* const TileOne = TEXT("ast_u00000000000000000000000000000b01");
	const TCHAR* const TileTwo = TEXT("ast_u00000000000000000000000000000b02");

	/** A box as an MRM1 mesh, its corners at +-Half cm: another Half is other bytes, so two meshes never stand in for each other. */
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

	/** A Size x Size opaque PNG of one grey: what a tile is, apart from its cook (the builder reads a PNG as it reads a cooked file). */
	TArray<uint8> MakePng(const int32 Size, const uint8 Grey)
	{
		IImageWrapperModule& Module = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(EImageFormat::PNG);
		TArray<FColor> Pixels;
		Pixels.Init(FColor(Grey, Grey, Grey, 255), Size * Size);
		Wrapper->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Size, Size, ERGBFormat::BGRA, 8);
		const TArray64<uint8> Compressed = Wrapper->GetCompressed();
		return TArray<uint8>(Compressed.GetData(), static_cast<int32>(Compressed.Num()));
	}

	/** The fake API: answers the meshes and the tiles it was given, and writes down every id it is asked about. */
	class FTileResolver : public IMantleAssetResolver
	{
	public:
		TMap<FString, TArray<uint8>> Files;
		TMap<FString, FString> Kinds;
		TArray<FString> Asked;

		virtual void Resolve(const TArray<FString>& Ids, TFunction<void(const FMantleAssetResolveResult&)> OnDone) override
		{
			Asked.Append(Ids);
			FMantleAssetResolveResult Result;
			Result.bSuccess = true;
			for (const FString& Id : Ids)
			{
				const TArray<uint8>* Bytes = Files.Find(Id);
				if (!Bytes)
				{
					Result.Missing.Add(Id);
					continue;
				}
				FMantleUploadedAsset Asset;
				Asset.Id = Id;
				Asset.Hash = FString(TEXT("sha256:")) + FString::ChrN(64, TEXT('e'));
				Asset.Kind = Kinds.FindRef(Id);
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

	/** The fake transport: serves the resolver's files by url and counts the downloads. */
	class FTileFetcher : public IMantleAssetFetcher
	{
	public:
		TSharedPtr<FTileResolver> Source;
		int32 Downloads = 0;

		virtual void Fetch(const FString& Url, const TSharedRef<FArchive>& Sink, TFunction<void(const FMantleAssetFetchResult&)> OnDone) override
		{
			FMantleAssetFetchResult Result;
			Result.bConnected = true;
			Result.StatusCode = 404;
			if (const TArray<uint8>* Bytes = Source->Files.Find(Url.RightChop(7)))
			{
				++Downloads;
				Sink->Serialize(const_cast<uint8*>(Bytes->GetData()), Bytes->Num());
				Result.StatusCode = 200;
			}
			OnDone(Result);
		}
	};

	/**
	 * A place whose entity uses one mesh, whose manifest also lists a spare mesh nothing uses and the two picture tiles, with the minimap on.
	 * Without the mesh entries it is a place whose only uploaded assets are the tiles.
	 */
	FMantlePlace MakePlace(const bool bWithMeshes)
	{
		const FString Hash = FString(TEXT("sha256:")) + FString::ChrN(64, TEXT('e'));
		FString Assets = FString::Printf(
			TEXT("{\"id\":\"%s\",\"kind\":\"Texture\",\"hash\":\"%s\",\"bytes\":100},{\"id\":\"%s\",\"kind\":\"Texture\",\"hash\":\"%s\",\"bytes\":100}"),
			TileOne, *Hash, TileTwo, *Hash);
		FString Entities;
		if (bWithMeshes)
		{
			Assets += FString::Printf(
				TEXT(",{\"id\":\"%s\",\"kind\":\"StaticMesh\",\"hash\":\"%s\",\"bytes\":100},{\"id\":\"%s\",\"kind\":\"StaticMesh\",\"hash\":\"%s\",\"bytes\":100}"),
				UsedMesh, *Hash, SpareMesh, *Hash);
			Entities = FString::Printf(
				TEXT("{\"id\":\"ent_used\",\"type\":\"StaticMesh\",\"parent\":null,"
					 "\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{\"mesh\":\"%s\"}}"),
				UsedMesh);
		}
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_minimaptiles\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Minimap Tiles\",\"authorId\":\"usr_4b1d\"},"
				 "\"settings\":{\"minimap\":{\"enabled\":true,\"image\":{\"tiles\":[[\"%s\",\"%s\"]],"
				 "\"bounds\":{\"minX\":-100000,\"maxX\":100000,\"minY\":-50000,\"maxY\":50000}}}},"
				 "\"assets\":[%s],\"entities\":[%s]}"),
			TileOne, TileTwo, *Assets, *Entities);
		const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
		check(Parsed.bSuccess);
		return Parsed.Place;
	}

	/** Settings on a store of their own, pumped by hand. bRender is a client's (textures are built); not, a dedicated server's. */
	FMantlePlaceAssetSettings MakeSettings(const TSharedRef<FTileResolver>& Resolver, const TSharedRef<FTileFetcher>& Fetcher, const FString& StoreName, const bool bRender)
	{
		FMantleAssetStoreSettings StoreSettings;
		StoreSettings.RootDir = FPaths::AutomationTransientDir() / StoreName;
		StoreSettings.MaxAttempts = 1;
		FMantlePlaceAssetSettings Settings;
		Settings.bAutoPump = false;
		Settings.bRenderAssets = bRender;
		Settings.bLoadNanite = false;
		Settings.bAsyncCollisionCook = false;
		Settings.Resolver = Resolver;
		Settings.Store = FMantleAssetStore::Create(StoreSettings, Fetcher);
		return Settings;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceAssetsMinimapTilesTest,
	"Mantle.Runtime.PlaceAssets.MinimapTilesOnDemand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceAssetsMinimapTilesTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceAssetsMinimapTilesTestPrivate;
	const FString Run = FGuid::NewGuid().ToString();

	// A place whose only uploaded assets are the tiles has nothing for the join or the manifest-only pass, on a client or a server.
	const FMantlePlace TileOnly = MakePlace(false);
	TestTrue(TEXT("the parsed place has a picture of two tiles"), TileOnly.Settings.Minimap.Image.IsSet() && TileOnly.Settings.Minimap.Image.Tiles[0].Num() == 2);
	TestEqual(TEXT("the tiles are in the manifest"), TileOnly.Assets.Num(), 2);
	TestFalse(TEXT("no entity uses a tile: the join takes no asset"), FMantlePlaceAssets::HasUploadedAssets(TileOnly));
	TestFalse(TEXT("and the manifest-only pass takes none on a client"), FMantlePlaceAssets::HasManifestOnlyAssets(TileOnly, true));
	TestFalse(TEXT("nor on a server"), FMantlePlaceAssets::HasManifestOnlyAssets(TileOnly, false));

	// A place with a mesh in use and a spare one: the join takes the used mesh, the manifest-only pass the spare, and neither takes a tile.
	const FMantlePlace Place = MakePlace(true);
	const TSharedRef<FTileResolver> Resolver = MakeShared<FTileResolver>();
	const float Size = 40.0f + static_cast<float>(GetTypeHash(Run) % 1000) / 100.0f;
	Resolver->Files.Add(UsedMesh, MakeMrm1Box(Size));
	Resolver->Kinds.Add(UsedMesh, TEXT("mesh"));
	Resolver->Files.Add(SpareMesh, MakeMrm1Box(Size + 20.0f));
	Resolver->Kinds.Add(SpareMesh, TEXT("mesh"));
	// Tiles of other greys each run, so no earlier run's built texture stands in for them.
	const int32 Shade = static_cast<int32>(GetTypeHash(Run) % 100);
	Resolver->Files.Add(TileOne, MakePng(16, static_cast<uint8>(40 + Shade)));
	Resolver->Kinds.Add(TileOne, TEXT("texture"));
	Resolver->Files.Add(TileTwo, MakePng(16, static_cast<uint8>(120 + Shade)));
	Resolver->Kinds.Add(TileTwo, TEXT("texture"));

	TestTrue(TEXT("the place with a mesh in use has uploaded assets"), FMantlePlaceAssets::HasUploadedAssets(Place));
	TestTrue(TEXT("and a spare mesh for the manifest-only pass"), FMantlePlaceAssets::HasManifestOnlyAssets(Place, false));

	const TSharedRef<FTileFetcher> JoinFetcher = MakeShared<FTileFetcher>();
	JoinFetcher->Source = Resolver;
	{
		const TSharedRef<FMantlePlaceAssets> Join = FMantlePlaceAssets::Prepare(Place, MakeSettings(Resolver, JoinFetcher, TEXT("MinimapTiles") / Run / TEXT("Join"), false), FMantlePlaceAssetsDone());
		TestTrue(TEXT("the join's set completes"), Join->WaitUntilComplete(60.0f));
		TestTrue(*FString::Printf(TEXT("the join's set has its mesh: %s"), *Join->GetResult().ErrorSummary()), Join->GetResult().bSuccess);
		TestTrue(TEXT("the join asked about the mesh an entity uses"), Resolver->Asked.Contains(UsedMesh));
		TestFalse(TEXT("and about no tile"), Resolver->Asked.Contains(TileOne) || Resolver->Asked.Contains(TileTwo));

		Resolver->Asked.Reset();
		const TSharedRef<FMantlePlaceAssets> Manifest = FMantlePlaceAssets::PrepareManifestOnly(Place,
			MakeSettings(Resolver, JoinFetcher, TEXT("MinimapTiles") / Run / TEXT("Manifest"), false), FMantlePlaceAssetsDone());
		TestTrue(TEXT("the manifest-only set completes"), Manifest->WaitUntilComplete(60.0f));
		TestTrue(TEXT("the manifest-only pass asked about the spare mesh"), Resolver->Asked.Contains(SpareMesh));
		TestFalse(TEXT("and about no tile"), Resolver->Asked.Contains(TileOne) || Resolver->Asked.Contains(TileTwo));

		FString Error;
		TestTrue(TEXT("so a tile is not prepared"), FMantlePlaceAssets::GetLiveTextureState(TileOne, EMantleTextureRole::Color, &Error) == EMantleLiveAssetState::NotPrepared);
		TestNull(TEXT("and there is no texture to find"), FMantlePlaceAssets::FindLiveTexture(TileOne, EMantleTextureRole::Color, Error));
	}

	// A machine that draws nothing never takes a tile on demand either.
	{
		Resolver->Asked.Reset();
		const TSharedRef<FTileFetcher> ServerFetcher = MakeShared<FTileFetcher>();
		ServerFetcher->Source = Resolver;
		const TSharedRef<FMantlePlaceAssets> Server = FMantlePlaceAssets::PrepareTextures({ TileOne, TileTwo },
			MakeSettings(Resolver, ServerFetcher, TEXT("MinimapTiles") / Run / TEXT("Server"), false), FMantlePlaceAssetsDone());
		TestTrue(TEXT("the server's texture set completes at once"), Server->WaitUntilComplete(60.0f));
		TestEqual(TEXT("it asked about nothing"), Resolver->Asked.Num(), 0);
		TestEqual(TEXT("and downloaded nothing"), ServerFetcher->Downloads, 0);
	}

	if (IsRunningDedicatedServer())
	{
		AddInfo(TEXT("A dedicated server builds no textures: the on-demand request is not run."));
		return true;
	}

	// The minimap turns on: the tiles are asked for now, and become live textures.
	Resolver->Asked.Reset();
	const TSharedRef<FTileFetcher> TileFetcher = MakeShared<FTileFetcher>();
	TileFetcher->Source = Resolver;
	// A duplicate id is one request.
	TSharedPtr<FMantlePlaceAssets> Tiles = FMantlePlaceAssets::PrepareTextures({ TileOne, TileTwo, TileOne },
		MakeSettings(Resolver, TileFetcher, TEXT("MinimapTiles") / Run / TEXT("Tiles"), true), FMantlePlaceAssetsDone());
	FString Error;
	TestTrue(TEXT("a tile is loading as soon as it is asked for"), FMantlePlaceAssets::GetLiveTextureState(TileOne, EMantleTextureRole::Color, &Error) == EMantleLiveAssetState::Loading);
	TestTrue(TEXT("the tiles' set completes"), Tiles->WaitUntilComplete(60.0f));
	TestTrue(*FString::Printf(TEXT("every tile built: %s"), *Tiles->GetResult().ErrorSummary()), Tiles->GetResult().bSuccess);
	TestEqual(TEXT("two textures"), Tiles->GetResult().Textures, 2);
	TestEqual(TEXT("the API was asked about the two ids once"), Resolver->Asked.Num(), 2);
	TestEqual(TEXT("and each was downloaded once"), TileFetcher->Downloads, 2);
	for (const FString& Tile : { TileOne, TileTwo })
	{
		TestTrue(*FString::Printf(TEXT("%s is ready"), *Tile), FMantlePlaceAssets::GetLiveTextureState(Tile, EMantleTextureRole::Color, &Error) == EMantleLiveAssetState::Ready);
		const UTexture* Texture = FMantlePlaceAssets::FindLiveTexture(Tile, EMantleTextureRole::Color, Error);
		TestNotNull(*FString::Printf(TEXT("%s is a live texture (%s)"), *Tile, *Error), Texture);
		if (Texture)
		{
			TestEqual(TEXT("the size of the tile"), static_cast<int32>(Texture->GetSurfaceWidth()), 16);
		}
	}

	// The place ends: the set goes, and the tiles with it.
	Tiles.Reset();
	TestTrue(TEXT("with the set gone a tile is not prepared"), FMantlePlaceAssets::GetLiveTextureState(TileOne, EMantleTextureRole::Color, &Error) == EMantleLiveAssetState::NotPrepared);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
