// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceParser.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The minimap's place format (#167): settings.minimap and its picture.
// Mirrors validate.py (check_minimap and the schema) and the backend's validate.go (minimapSettings).

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantlePlaceMinimapParseTestPrivate
{
	const TCHAR* const Bounds = TEXT("{\"minX\":-245000,\"maxX\":210000,\"minY\":-415000,\"maxY\":115000}");

	/** A place with no entities and the given text between the settings braces. */
	FMantleParseResult ParsePlace(const FString& Settings)
	{
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_minimap01\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Minimap\",\"authorId\":\"usr_4b1d\"},\"settings\":{%s},\"assets\":[],\"entities\":[]}"),
			*Settings);
		return FMantlePlaceParser::ParseFromString(Json);
	}

	/** A place whose settings.minimap is the given JSON. */
	FMantleParseResult ParseMinimap(const FString& Minimap)
	{
		return ParsePlace(FString::Printf(TEXT("\"minimap\":%s"), *Minimap));
	}

	/** A place whose settings.minimap.image is the given JSON. */
	FMantleParseResult ParseImage(const FString& Image)
	{
		return ParseMinimap(FString::Printf(TEXT("{\"image\":%s}"), *Image));
	}

	/** An image of the given tile rows (the text between the outer brackets) over Bounds. */
	FMantleParseResult ParseTiles(const FString& Rows)
	{
		return ParseImage(FString::Printf(TEXT("{\"tiles\":[%s],\"bounds\":%s}"), *Rows, Bounds));
	}

	/** One row of Count upload tiles, as JSON. */
	FString Row(int32 Count)
	{
		TArray<FString> Ids;
		for (int32 I = 0; I < Count; ++I)
		{
			Ids.Add(FString::Printf(TEXT("\"ast_u%032d\""), I + 1));
		}
		return TEXT("[") + FString::Join(Ids, TEXT(",")) + TEXT("]");
	}

	/** Rows rows of Columns tiles each, as the text between the tiles' outer brackets. */
	FString Grid(int32 Rows, int32 Columns)
	{
		TArray<FString> Lines;
		for (int32 I = 0; I < Rows; ++I)
		{
			Lines.Add(Row(Columns));
		}
		return FString::Join(Lines, TEXT(","));
	}

	bool HasErrorAt(const FMantleParseResult& Result, const FString& Path)
	{
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error) { return Error.Path == Path; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMinimapSettingsParseTest,
	"Mantle.Runtime.Place.Parser.MinimapSettings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMinimapSettingsParseTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceMinimapParseTestPrivate;

	// A place without the block has no minimap, and every key at the schema's default.
	const FMantleParseResult Default = ParsePlace(FString());
	if (!TestTrue(TEXT("a place without settings.minimap parses"), Default.bSuccess))
	{
		AddError(Default.ErrorSummary());
		return false;
	}
	const FMantleMinimapSettings& Defaults = Default.Place.Settings.Minimap;
	TestFalse(TEXT("off by default"), Defaults.bEnabled);
	TestTrue(TEXT("a circle by default"), Defaults.Shape == EMantleMinimapShape::Circle);
	TestTrue(TEXT("top left by default"), Defaults.Anchor == EMantleMinimapAnchor::TopLeft);
	TestEqual(TEXT("an offset of 16 across by default"), Defaults.Offset.X, 16.0);
	TestEqual(TEXT("an offset of 16 down by default"), Defaults.Offset.Y, 16.0);
	TestEqual(TEXT("200 dp across by default"), Defaults.Size, 200.0);
	TestTrue(TEXT("a rim by default"), Defaults.bRim);
	TestTrue(TEXT("turns with the camera by default"), Defaults.Rotate == EMantleMinimapRotate::Camera);
	TestEqual(TEXT("150 m of radius by default"), Defaults.Range, 150.0);
	TestTrue(TEXT("the full map is allowed by default"), Defaults.bFullMap);
	TestTrue(TEXT("nearby players by default"), Defaults.Players == EMantleMinimapPlayers::Nearby);
	TestFalse(TEXT("no picture by default"), Defaults.Image.IsSet());

	// An empty block keeps every default too.
	const FMantleParseResult Empty = ParseMinimap(TEXT("{}"));
	if (!TestTrue(TEXT("an empty block parses"), Empty.bSuccess))
	{
		AddError(Empty.ErrorSummary());
		return false;
	}
	TestFalse(TEXT("an empty block leaves it off"), Empty.Place.Settings.Minimap.bEnabled);

	// Every key set to something other than its default.
	const FMantleParseResult Set = ParseMinimap(TEXT(
		"{\"enabled\":true,\"shape\":\"square\",\"anchor\":\"bottomRight\",\"offset\":[-16,24.5],\"size\":256,\"rim\":false,"
		"\"rotate\":\"north\",\"range\":300,\"fullMap\":false,\"players\":\"off\"}"));
	if (!TestTrue(TEXT("every key set parses"), Set.bSuccess))
	{
		AddError(Set.ErrorSummary());
		return false;
	}
	const FMantleMinimapSettings& Minimap = Set.Place.Settings.Minimap;
	TestTrue(TEXT("enabled is read"), Minimap.bEnabled);
	TestTrue(TEXT("shape is read"), Minimap.Shape == EMantleMinimapShape::Square);
	TestTrue(TEXT("anchor is read"), Minimap.Anchor == EMantleMinimapAnchor::BottomRight);
	TestEqual(TEXT("offset x is read"), Minimap.Offset.X, -16.0);
	TestEqual(TEXT("offset y is read"), Minimap.Offset.Y, 24.5);
	TestEqual(TEXT("size is read"), Minimap.Size, 256.0);
	TestFalse(TEXT("rim is read"), Minimap.bRim);
	TestTrue(TEXT("rotate is read"), Minimap.Rotate == EMantleMinimapRotate::North);
	TestEqual(TEXT("range is read"), Minimap.Range, 300.0);
	TestFalse(TEXT("fullMap is read"), Minimap.bFullMap);
	TestTrue(TEXT("players is read"), Minimap.Players == EMantleMinimapPlayers::Off);

	// Each anchor reads as its own value, in the schema's order.
	const TCHAR* const Anchors[] = { TEXT("topLeft"), TEXT("topCenter"), TEXT("topRight"), TEXT("centerLeft"), TEXT("centerRight"),
		TEXT("bottomLeft"), TEXT("bottomCenter"), TEXT("bottomRight") };
	for (int32 I = 0; I < static_cast<int32>(UE_ARRAY_COUNT(Anchors)); ++I)
	{
		const FMantleParseResult Result = ParseMinimap(FString::Printf(TEXT("{\"anchor\":\"%s\"}"), Anchors[I]));
		TestTrue(*FString::Printf(TEXT("anchor %s parses"), Anchors[I]), Result.bSuccess);
		TestEqual(*FString::Printf(TEXT("anchor %s is value %d"), Anchors[I], I), static_cast<int32>(Result.Place.Settings.Minimap.Anchor), I);
	}

	// The ranges are inclusive at both ends.
	TestTrue(TEXT("size 96 parses"), ParseMinimap(TEXT("{\"size\":96}")).bSuccess);
	TestTrue(TEXT("size 512 parses"), ParseMinimap(TEXT("{\"size\":512}")).bSuccess);
	TestTrue(TEXT("range 25 parses"), ParseMinimap(TEXT("{\"range\":25}")).bSuccess);
	TestTrue(TEXT("range 2000 parses"), ParseMinimap(TEXT("{\"range\":2000}")).bSuccess);
	TestTrue(TEXT("offsets of -1000 and 1000 parse"), ParseMinimap(TEXT("{\"offset\":[-1000,1000]}")).bSuccess);

	TestTrue(TEXT("size 95 is refused"), HasErrorAt(ParseMinimap(TEXT("{\"size\":95}")), TEXT("/settings/minimap/size")));
	TestTrue(TEXT("size 513 is refused"), HasErrorAt(ParseMinimap(TEXT("{\"size\":513}")), TEXT("/settings/minimap/size")));
	TestTrue(TEXT("a string size is refused"), HasErrorAt(ParseMinimap(TEXT("{\"size\":\"200\"}")), TEXT("/settings/minimap/size")));
	TestTrue(TEXT("range 24 is refused"), HasErrorAt(ParseMinimap(TEXT("{\"range\":24}")), TEXT("/settings/minimap/range")));
	TestTrue(TEXT("range 2001 is refused"), HasErrorAt(ParseMinimap(TEXT("{\"range\":2001}")), TEXT("/settings/minimap/range")));
	TestTrue(TEXT("a string range is refused"), HasErrorAt(ParseMinimap(TEXT("{\"range\":\"150\"}")), TEXT("/settings/minimap/range")));
	TestTrue(TEXT("offset x 1001 is refused"), HasErrorAt(ParseMinimap(TEXT("{\"offset\":[1001,0]}")), TEXT("/settings/minimap/offset/0")));
	TestTrue(TEXT("offset y -1001 is refused"), HasErrorAt(ParseMinimap(TEXT("{\"offset\":[0,-1001]}")), TEXT("/settings/minimap/offset/1")));
	TestTrue(TEXT("an offset of one number is refused"), HasErrorAt(ParseMinimap(TEXT("{\"offset\":[16]}")), TEXT("/settings/minimap/offset")));
	TestTrue(TEXT("an offset of three numbers is refused"), HasErrorAt(ParseMinimap(TEXT("{\"offset\":[1,2,3]}")), TEXT("/settings/minimap/offset")));
	TestTrue(TEXT("an offset that is not an array is refused"), HasErrorAt(ParseMinimap(TEXT("{\"offset\":16}")), TEXT("/settings/minimap/offset")));
	TestTrue(TEXT("a string in the offset is refused"), HasErrorAt(ParseMinimap(TEXT("{\"offset\":[16,\"16\"]}")), TEXT("/settings/minimap/offset/1")));

	TestTrue(TEXT("an unknown anchor is refused"), HasErrorAt(ParseMinimap(TEXT("{\"anchor\":\"middle\"}")), TEXT("/settings/minimap/anchor")));
	TestTrue(TEXT("anchor is case-exact"), HasErrorAt(ParseMinimap(TEXT("{\"anchor\":\"topleft\"}")), TEXT("/settings/minimap/anchor")));
	TestTrue(TEXT("an unknown shape is refused"), HasErrorAt(ParseMinimap(TEXT("{\"shape\":\"hexagon\"}")), TEXT("/settings/minimap/shape")));
	TestTrue(TEXT("shape is case-exact"), HasErrorAt(ParseMinimap(TEXT("{\"shape\":\"Circle\"}")), TEXT("/settings/minimap/shape")));
	TestTrue(TEXT("an unknown rotate is refused"), HasErrorAt(ParseMinimap(TEXT("{\"rotate\":\"player\"}")), TEXT("/settings/minimap/rotate")));
	TestTrue(TEXT("an unknown players is refused"), HasErrorAt(ParseMinimap(TEXT("{\"players\":\"all\"}")), TEXT("/settings/minimap/players")));
	TestTrue(TEXT("a string enabled is refused"), HasErrorAt(ParseMinimap(TEXT("{\"enabled\":\"yes\"}")), TEXT("/settings/minimap/enabled")));
	TestTrue(TEXT("a number rim is refused"), HasErrorAt(ParseMinimap(TEXT("{\"rim\":1}")), TEXT("/settings/minimap/rim")));
	TestTrue(TEXT("a string fullMap is refused"), HasErrorAt(ParseMinimap(TEXT("{\"fullMap\":\"on\"}")), TEXT("/settings/minimap/fullMap")));
	TestTrue(TEXT("an unknown key is refused"), HasErrorAt(ParseMinimap(TEXT("{\"fog\":true}")), TEXT("/settings/minimap/fog")));
	TestTrue(TEXT("a block that is not an object is refused"), HasErrorAt(ParseMinimap(TEXT("\"on\"")), TEXT("/settings/minimap")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMinimapImageParseTest,
	"Mantle.Runtime.Place.Parser.MinimapImage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMinimapImageParseTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceMinimapParseTestPrivate;

	// City Sample's picture: 3 x 3 tiles over its box in cm.
	const FMantleParseResult Good = ParseTiles(Grid(3, 3));
	if (!TestTrue(TEXT("a 3 x 3 picture parses"), Good.bSuccess))
	{
		AddError(Good.ErrorSummary());
		return false;
	}
	const FMantleMinimapImage& Image = Good.Place.Settings.Minimap.Image;
	TestTrue(TEXT("the picture is set"), Image.IsSet());
	if (!TestEqual(TEXT("three rows"), Image.Tiles.Num(), 3) || !TestEqual(TEXT("three tiles in the first row"), Image.Tiles[0].Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("row 0, column 0 is the first id"), Image.Tiles[0][0], FString(TEXT("ast_u00000000000000000000000000000001")));
	TestEqual(TEXT("row 2, column 2 is the last id"), Image.Tiles[2][2], FString(TEXT("ast_u00000000000000000000000000000003")));
	TestEqual(TEXT("minX is read"), Image.MinX, -245000.0);
	TestEqual(TEXT("maxX is read"), Image.MaxX, 210000.0);
	TestEqual(TEXT("minY is read"), Image.MinY, -415000.0);
	TestEqual(TEXT("maxY is read"), Image.MaxY, 115000.0);

	TestTrue(TEXT("one tile parses"), ParseTiles(Grid(1, 1)).bSuccess);
	TestTrue(TEXT("4 x 4 tiles parse"), ParseTiles(Grid(4, 4)).bSuccess);
	TestTrue(TEXT("a built-in id parses: its kind is the backend's to check"), ParseTiles(TEXT("[\"ast_cube\"]")).bSuccess);

	TestTrue(TEXT("5 rows are refused"), HasErrorAt(ParseTiles(Grid(5, 2)), TEXT("/settings/minimap/image/tiles")));
	TestTrue(TEXT("no rows are refused"), HasErrorAt(ParseTiles(FString()), TEXT("/settings/minimap/image/tiles")));
	TestTrue(TEXT("5 tiles in a row are refused"), HasErrorAt(ParseTiles(Grid(1, 5)), TEXT("/settings/minimap/image/tiles/0")));
	TestTrue(TEXT("an empty row is refused"), HasErrorAt(ParseTiles(TEXT("[]")), TEXT("/settings/minimap/image/tiles/0")));
	TestTrue(TEXT("a row that is not an array is refused"), HasErrorAt(ParseTiles(Row(2) + TEXT(",\"ast_cube\"")), TEXT("/settings/minimap/image/tiles/1")));
	TestTrue(TEXT("rows of different lengths are refused"), HasErrorAt(ParseTiles(Row(2) + TEXT(",") + Row(3)), TEXT("/settings/minimap/image/tiles/1")));
	TestTrue(TEXT("a shorter second row is refused"), HasErrorAt(ParseTiles(Row(2) + TEXT(",") + Row(1)), TEXT("/settings/minimap/image/tiles/1")));
	TestTrue(TEXT("a tile that is not an id is refused"), HasErrorAt(ParseTiles(TEXT("[\"ast_u00000000000000000000000000000001\",\"map_0_1.png\"]")),
		TEXT("/settings/minimap/image/tiles/0/1")));
	TestTrue(TEXT("a tile in capitals is refused"), HasErrorAt(ParseTiles(TEXT("[\"AST_U1\"]")), TEXT("/settings/minimap/image/tiles/0/0")));
	TestTrue(TEXT("a tile that is not a string is refused"), HasErrorAt(ParseTiles(TEXT("[7]")), TEXT("/settings/minimap/image/tiles/0/0")));

	TestTrue(TEXT("an image that is not an object is refused"), HasErrorAt(ParseImage(TEXT("\"map.png\"")), TEXT("/settings/minimap/image")));
	TestTrue(TEXT("an image without bounds is refused"), HasErrorAt(ParseImage(FString::Printf(TEXT("{\"tiles\":[%s]}"), *Grid(1, 1))),
		TEXT("/settings/minimap/image/bounds")));
	TestTrue(TEXT("an image without tiles is refused"), HasErrorAt(ParseImage(FString::Printf(TEXT("{\"bounds\":%s}"), Bounds)),
		TEXT("/settings/minimap/image/tiles")));
	TestTrue(TEXT("an unknown image key is refused"), HasErrorAt(ParseImage(FString::Printf(TEXT("{\"tiles\":[%s],\"bounds\":%s,\"scale\":1}"), *Grid(1, 1), Bounds)),
		TEXT("/settings/minimap/image/scale")));
	TestTrue(TEXT("bounds missing a side are refused"), HasErrorAt(
		ParseImage(FString::Printf(TEXT("{\"tiles\":[%s],\"bounds\":{\"minX\":0,\"maxX\":1,\"minY\":0}}"), *Grid(1, 1))), TEXT("/settings/minimap/image/bounds/maxY")));
	TestTrue(TEXT("bounds with an unknown side are refused"), HasErrorAt(
		ParseImage(FString::Printf(TEXT("{\"tiles\":[%s],\"bounds\":{\"minX\":0,\"maxX\":1,\"minY\":0,\"maxY\":1,\"top\":0}}"), *Grid(1, 1))),
		TEXT("/settings/minimap/image/bounds/top")));
	TestTrue(TEXT("maxX below minX is refused"), HasErrorAt(
		ParseImage(FString::Printf(TEXT("{\"tiles\":[%s],\"bounds\":{\"minX\":10,\"maxX\":-10,\"minY\":0,\"maxY\":1}}"), *Grid(1, 1))), TEXT("/settings/minimap/image/bounds")));
	TestTrue(TEXT("maxY equal to minY is refused"), HasErrorAt(
		ParseImage(FString::Printf(TEXT("{\"tiles\":[%s],\"bounds\":{\"minX\":0,\"maxX\":1,\"minY\":5,\"maxY\":5}}"), *Grid(1, 1))), TEXT("/settings/minimap/image/bounds")));
	TestTrue(TEXT("a bound that is not a number is refused"), HasErrorAt(
		ParseImage(FString::Printf(TEXT("{\"tiles\":[%s],\"bounds\":{\"minX\":\"west\",\"maxX\":1,\"minY\":0,\"maxY\":1}}"), *Grid(1, 1))),
		TEXT("/settings/minimap/image/bounds/minX")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
