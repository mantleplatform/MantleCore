// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHeightfieldReader.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"
#include "MantleSurface.h"
#include "MantleTerrainSurface.h"

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

// Terrain with up to eight layers: the parser's weightmap2 rules, and the surface lookup over two weightmaps.
// No network, no world.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleTerrainLayersTestPrivate
{
	/** A Terrain entity with LayerCount layers and Extra after them (each key starting with a comma). */
	FString TerrainEntity(const int32 LayerCount, const FString& Extra)
	{
		TArray<FString> Layers;
		for (int32 Index = 0; Index < LayerCount; ++Index)
		{
			Layers.Add(TEXT("{\"material\":\"mat_a\"}"));
		}
		return FString::Printf(
			TEXT("{\"id\":\"t\",\"type\":\"Terrain\",\"name\":\"t\",\"parent\":null,")
				TEXT("\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},")
				TEXT("\"properties\":{\"heightfield\":\"hf_a\",\"layers\":[%s]%s}}"),
			*FString::Join(Layers, TEXT(",")), *Extra);
	}

	FString PlaceJson(const FString& Entity)
	{
		return FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_layers_test\",\"revision\":{\"number\":1,\"state\":\"draft\"},")
				TEXT("\"metadata\":{\"name\":\"Layers test\",\"authorId\":\"usr_test\"},\"settings\":{},\"assets\":[],\"entities\":[%s]}"),
			*Entity);
	}

	bool HasErrorAt(const FMantleParseResult& Result, const FString& Path)
	{
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error)
		{
			return Error.Path.Equals(Path, ESearchCase::CaseSensitive);
		});
	}

	TSharedRef<FMantleTerrainWeights> MakeWeights(const int32 Width, const TArray<FColor>& Pixels)
	{
		TSharedRef<FMantleTerrainWeights> Weights = MakeShared<FMantleTerrainWeights>();
		FMantleTerrainWeights::FromPixels(Width, 1, Pixels, *Weights);
		return Weights;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainLayersParserTest,
	"Mantle.Runtime.Parser.TerrainLayers8",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainLayersParserTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainLayersTestPrivate;

	TestEqual(TEXT("a Terrain may have eight layers"), FMantlePlaceLimits::MaxTerrainLayers, 8);

	// Eight layers with both weightmaps parse, and the second is kept.
	const FMantleParseResult Eight = FMantlePlaceParser::ParseFromString(
		PlaceJson(TerrainEntity(8, TEXT(",\"weightmap\":\"wm_a\",\"weightmap2\":\"wm_b\""))));
	if (!TestTrue(TEXT("eight layers with a weightmap2 parse"), Eight.bSuccess))
	{
		AddError(Eight.ErrorSummary());
		return false;
	}
	const FMantleTerrainProps* Terrain = Eight.Place.Entities.Num() == 1 ? Eight.Place.Entities[0].Props.TryGet<FMantleTerrainProps>() : nullptr;
	if (!TestNotNull(TEXT("the terrain"), Terrain))
	{
		return false;
	}
	TestEqual(TEXT("eight layers"), Terrain->Layers.Num(), 8);
	TestEqual(TEXT("the weightmap"), Terrain->Weightmap, FString(TEXT("wm_a")));
	TestEqual(TEXT("the weightmap2"), Terrain->Weightmap2, FString(TEXT("wm_b")));

	// Five layers is the first to need the second map.
	TestTrue(TEXT("five layers with a weightmap2 parse"),
		FMantlePlaceParser::ParseFromString(PlaceJson(TerrainEntity(5, TEXT(",\"weightmap\":\"wm_a\",\"weightmap2\":\"wm_b\"")))).bSuccess);

	// Up to four layers need no weightmap2, and a Terrain without one has none.
	const FMantleParseResult Four = FMantlePlaceParser::ParseFromString(PlaceJson(TerrainEntity(4, TEXT(",\"weightmap\":\"wm_a\""))));
	TestTrue(TEXT("four layers without a weightmap2 parse"), Four.bSuccess);
	TestTrue(TEXT("and have none"), Four.Place.Entities.Num() == 1 && Four.Place.Entities[0].Props.Get<FMantleTerrainProps>().Weightmap2.IsEmpty());

	// More than four layers without it: reported at the missing weightmap2.
	const FMantleParseResult Missing = FMantlePlaceParser::ParseFromString(PlaceJson(TerrainEntity(5, TEXT(",\"weightmap\":\"wm_a\""))));
	TestFalse(TEXT("five layers without a weightmap2 are refused"), Missing.bSuccess);
	TestTrue(TEXT("at the missing weightmap2"), HasErrorAt(Missing, TEXT("/entities/0/properties/weightmap2")));

	// A weightmap2 with no layer past the fourth: reported at the weightmap2.
	const FMantleParseResult Surplus = FMantlePlaceParser::ParseFromString(
		PlaceJson(TerrainEntity(4, TEXT(",\"weightmap\":\"wm_a\",\"weightmap2\":\"wm_b\""))));
	TestFalse(TEXT("a weightmap2 on four layers is refused"), Surplus.bSuccess);
	TestTrue(TEXT("at the weightmap2"), HasErrorAt(Surplus, TEXT("/entities/0/properties/weightmap2")));

	// Nine layers are past the limit, whatever the maps.
	const FMantleParseResult Nine = FMantlePlaceParser::ParseFromString(
		PlaceJson(TerrainEntity(9, TEXT(",\"weightmap\":\"wm_a\",\"weightmap2\":\"wm_b\""))));
	TestFalse(TEXT("nine layers are refused"), Nine.bSuccess);
	TestTrue(TEXT("at the layers"), HasErrorAt(Nine, TEXT("/entities/0/properties/layers")));

	// An empty id and a wrong type.
	const FMantleParseResult Empty = FMantlePlaceParser::ParseFromString(
		PlaceJson(TerrainEntity(5, TEXT(",\"weightmap\":\"wm_a\",\"weightmap2\":\"\""))));
	TestFalse(TEXT("a weightmap2 naming no texture is refused"), Empty.bSuccess);
	TestTrue(TEXT("at the weightmap2"), HasErrorAt(Empty, TEXT("/entities/0/properties/weightmap2")));
	const FMantleParseResult Number = FMantlePlaceParser::ParseFromString(
		PlaceJson(TerrainEntity(5, TEXT(",\"weightmap\":\"wm_a\",\"weightmap2\":7"))));
	TestFalse(TEXT("a weightmap2 that is not a string is refused"), Number.bSuccess);
	TestTrue(TEXT("at the weightmap2"), HasErrorAt(Number, TEXT("/entities/0/properties/weightmap2")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainLayersSurfaceTest,
	"Mantle.Runtime.TerrainSurface.EightLayers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainLayersSurfaceTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainLayersTestPrivate;

	// Two texels across. The first map: layer 0 weighs 50 on the left (and nothing else), layer 1 weighs 255 on the right.
	// The second map: layer 5 (G) weighs 200 on the left, layer 7 (A) weighs 40 on the right.
	const TSharedRef<FMantleTerrainWeights> First = MakeWeights(2, { FColor(50, 0, 0, 0), FColor(0, 255, 0, 0) });
	const TSharedRef<FMantleTerrainWeights> Second = MakeWeights(2, { FColor(0, 200, 0, 0), FColor(0, 0, 0, 40) });
	TestEqual(TEXT("the first map's heaviest weight is kept"), First->DominantWeightAt(0.25, 0.5), 50);
	TestEqual(TEXT("(the right texel's)"), First->DominantWeightAt(0.75, 0.5), 255);
	TestEqual(TEXT("an empty map weighs nothing"), FMantleTerrainWeights().DominantWeightAt(0.5, 0.5), 0);

	TestEqual(TEXT("left, the second map's layer 5 outweighs the first's layer 0"),
		FMantleTerrainWeights::HeaviestLayerOf(*First, &Second.Get(), 0.25, 0.5), 5);
	TestEqual(TEXT("right, the first map's layer 1 outweighs the second's layer 7"),
		FMantleTerrainWeights::HeaviestLayerOf(*First, &Second.Get(), 0.75, 0.5), 1);
	TestEqual(TEXT("without a second map it is the first's heaviest"), FMantleTerrainWeights::HeaviestLayerOf(*First, nullptr, 0.25, 0.5), 0);
	const FMantleTerrainWeights Empty;
	TestEqual(TEXT("(an empty second map the same)"), FMantleTerrainWeights::HeaviestLayerOf(*First, &Empty, 0.25, 0.5), 0);

	// A tie goes to the lower layer, the first map's; alpha of the second map is layer 7.
	const TSharedRef<FMantleTerrainWeights> TieFirst = MakeWeights(1, { FColor(0, 0, 100, 0) });
	const TSharedRef<FMantleTerrainWeights> TieSecond = MakeWeights(1, { FColor(0, 0, 0, 100) });
	TestEqual(TEXT("a tie goes to the first map's layer"), FMantleTerrainWeights::HeaviestLayerOf(*TieFirst, &TieSecond.Get(), 0.5, 0.5), 2);
	const TSharedRef<FMantleTerrainWeights> HeavierSecond = MakeWeights(1, { FColor(0, 0, 0, 101) });
	TestEqual(TEXT("one more and the second map's alpha is layer 7"),
		FMantleTerrainWeights::HeaviestLayerOf(*TieFirst, &HeavierSecond.Get(), 0.5, 0.5), 7);

	// The surface a hit resolves to: layer 5 and layer 7 name their own surfaces.
	FMantleHeightfield Field;
	Field.Width = 3;
	Field.Height = 2;
	Field.SpacingX = 100.0f;
	Field.SpacingY = 100.0f;
	const TArray<EMantleSurface> Surfaces = { EMantleSurface::Grass, EMantleSurface::Dirt, EMantleSurface::Sand, EMantleSurface::Mud,
		EMantleSurface::Snow, EMantleSurface::Ice, EMantleSurface::Stone, EMantleSurface::Brick };

	UMantleTerrainSurfaceComponent* Component = NewObject<UMantleTerrainSurfaceComponent>(GetTransientPackage());
	Component->Init(Field, Surfaces, First, Second);
	TestEqual(TEXT("a hit on the left is layer 5's surface"), Component->SurfaceAtLocal(50.0, 50.0), EMantleSurface::Ice);
	TestEqual(TEXT("a hit on the right is layer 1's"), Component->SurfaceAtLocal(150.0, 50.0), EMantleSurface::Dirt);

	// Layer 7 where the second map's alpha wins: the first map has nothing there.
	const TSharedRef<FMantleTerrainWeights> Bare = MakeWeights(2, { FColor(0, 0, 0, 0), FColor(0, 0, 0, 0) });
	Component->Init(Field, Surfaces, Bare, Second);
	TestEqual(TEXT("the second map's alpha on the right is layer 7's surface"), Component->SurfaceAtLocal(150.0, 50.0), EMantleSurface::Brick);

	// A second map that could not be read leaves the first's layers, and a terrain of up to four layers has none.
	Component->Init(Field, Surfaces, First, nullptr);
	TestEqual(TEXT("without the second map the left is layer 0's surface"), Component->SurfaceAtLocal(50.0, 50.0), EMantleSurface::Grass);
	TestEqual(TEXT("and the right layer 1's"), Component->SurfaceAtLocal(150.0, 50.0), EMantleSurface::Dirt);

	// A weighted layer with no surface behind it (a four-layer terrain with a stray second map) is layer 0's.
	Component->Init(Field, { EMantleSurface::Grass, EMantleSurface::Dirt }, First, Second);
	TestEqual(TEXT("a layer past the surfaces falls back to layer 0's"), Component->SurfaceAtLocal(50.0, 50.0), EMantleSurface::Grass);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
