// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHeightfieldReader.h"
#include "MantlePlaceParser.h"
#include "MantleTerrainBuilder.h"
#include "MantleTerrainSurface.h"

#include "MeshDescription.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Terrain holes: the parser's `holes` field, the grid made from the mask, and the chunk description that leaves
// a hole's quads out in every LOD. No network, no world.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleTerrainHolesTestPrivate
{
	FString PlaceJson(const FString& Extra)
	{
		return FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_holes_test\",\"revision\":{\"number\":1,\"state\":\"draft\"},")
				TEXT("\"metadata\":{\"name\":\"Holes test\",\"authorId\":\"usr_test\"},\"settings\":{},\"assets\":[],\"entities\":[")
				TEXT("{\"id\":\"t\",\"type\":\"Terrain\",\"name\":\"t\",\"parent\":null,")
				TEXT("\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},")
				TEXT("\"properties\":{\"heightfield\":\"hf_a\",\"layers\":[{\"material\":\"mat_a\"}]%s}}]}"),
			*Extra);
	}

	bool HasErrorAt(const FMantleParseResult& Result, const FString& Path)
	{
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error)
		{
			return Error.Path.Equals(Path, ESearchCase::CaseSensitive);
		});
	}

	/** A mask of Width x Height texels, ground (255) except the texels in Holes (x, y), which are 0. */
	FMantleTerrainWeights MakeMask(const int32 Width, const int32 Height, const TArray<FIntPoint>& Holes)
	{
		TArray<FColor> Pixels;
		Pixels.Init(FColor(255, 255, 255, 255), Width * Height);
		for (const FIntPoint& Hole : Holes)
		{
			Pixels[Hole.Y * Width + Hole.X] = FColor(0, 255, 255, 255);
		}
		FMantleTerrainWeights Mask;
		FMantleTerrainWeights::FromPixels(Width, Height, Pixels, Mask, true);
		return Mask;
	}

	FMantleHeightfield MakeField(const int32 Size)
	{
		FMantleHeightfield Field;
		Field.Width = Size;
		Field.Height = Size;
		Field.SpacingX = 100.0f;
		Field.SpacingY = 100.0f;
		Field.HeightScale = 1.0f;
		Field.Heights.Init(0, Size * Size);
		return Field;
	}

	int32 Triangles(const FMantleHeightfield& Field, const int32 Step, const FMantleTerrainHoleGrid* Holes)
	{
		const bool bSkirts[4] = { false, false, false, false };
		FMeshDescription Description;
		FMantleTerrainBuilder::BuildChunkDescription(Field, FIntPoint(0, 0), FIntPoint(Field.Width - 1, Field.Height - 1), Step, 0.0f, bSkirts, Description, Holes);
		return Description.Triangles().Num();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainHolesParserTest,
	"Mantle.Runtime.Parser.TerrainHoles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainHolesParserTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainHolesTestPrivate;

	const FMantleParseResult Good = FMantlePlaceParser::ParseFromString(PlaceJson(TEXT(",\"holes\":\"mask_a\"")));
	if (!TestTrue(TEXT("a Terrain with a hole mask parses"), Good.bSuccess))
	{
		AddError(Good.ErrorSummary());
		return false;
	}
	TestEqual(TEXT("and keeps the id"), Good.Place.Entities[0].Props.Get<FMantleTerrainProps>().Holes, FString(TEXT("mask_a")));
	TestTrue(TEXT("a Terrain without one has none"),
		FMantlePlaceParser::ParseFromString(PlaceJson(FString())).Place.Entities[0].Props.Get<FMantleTerrainProps>().Holes.IsEmpty());

	const FMantleParseResult Empty = FMantlePlaceParser::ParseFromString(PlaceJson(TEXT(",\"holes\":\"\"")));
	TestFalse(TEXT("a hole mask naming no texture is refused"), Empty.bSuccess);
	TestTrue(TEXT("at the holes"), HasErrorAt(Empty, TEXT("/entities/0/properties/holes")));
	const FMantleParseResult Number = FMantlePlaceParser::ParseFromString(PlaceJson(TEXT(",\"holes\":7")));
	TestFalse(TEXT("a hole mask that is not a string is refused"), Number.bSuccess);
	TestTrue(TEXT("at the holes"), HasErrorAt(Number, TEXT("/entities/0/properties/holes")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainHolesGridTest,
	"Mantle.Runtime.TerrainBuilder.Holes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainHolesGridTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainHolesTestPrivate;

	// A mask with no hole makes no grid.
	TestFalse(TEXT("no hole, no grid"), FMantleTerrainHoleGrid::FromMask(MakeMask(5, 5, {}), 5, 5).IsValid());

	// Sample (2, 2) of a 5 x 5 heightfield is a hole: the four quads around it are, and no other.
	const TSharedPtr<const FMantleTerrainHoleGrid> Grid = FMantleTerrainHoleGrid::FromMask(MakeMask(5, 5, { FIntPoint(2, 2) }), 5, 5);
	if (!TestTrue(TEXT("a hole makes a grid"), Grid.IsValid()))
	{
		return false;
	}
	int32 HoleQuads = 0;
	for (int32 Y = 0; Y < 4; ++Y)
	{
		for (int32 X = 0; X < 4; ++X)
		{
			const bool bAround = X >= 1 && X <= 2 && Y >= 1 && Y <= 2;
			HoleQuads += Grid->QuadIsHole(X, Y) ? 1 : 0;
			TestEqual(*FString::Printf(TEXT("quad (%d, %d)"), X, Y), Grid->QuadIsHole(X, Y), bAround);
		}
	}
	TestEqual(TEXT("four quads are holes"), HoleQuads, 4);
	TestTrue(TEXT("a point inside one is over a hole"), Grid->IsHoleAt(1.5, 1.5));
	TestFalse(TEXT("a point in a corner quad is not"), Grid->IsHoleAt(0.5, 0.5));
	TestFalse(TEXT("nor one past the edge"), Grid->IsHoleAt(4.0, 4.0));

	// A mask at half the heightfield's size is read by nearest texel: texel (1, 1) of 3 x 3 is sample (2, 2) of 5 x 5.
	const TSharedPtr<const FMantleTerrainHoleGrid> Coarse = FMantleTerrainHoleGrid::FromMask(MakeMask(3, 3, { FIntPoint(1, 1) }), 5, 5);
	TestTrue(TEXT("a smaller mask still marks the middle"), Coarse.IsValid() && Coarse->QuadIsHole(1, 1) && Coarse->QuadIsHole(2, 2));

	// The chunk description leaves the hole's quads out: 16 quads is 32 triangles, 4 fewer quads is 8 fewer triangles.
	const FMantleHeightfield Field = MakeField(5);
	TestEqual(TEXT("a whole chunk"), Triangles(Field, 1, nullptr), 32);
	TestEqual(TEXT("the hole's quads are left out"), Triangles(Field, 1, Grid.Get()), 24);
	// At LOD 1 (step 2) each of the four quads covers a hole quad, so all of them go.
	TestEqual(TEXT("a coarser LOD's quad covering a hole is left out"), Triangles(Field, 2, Grid.Get()), 0);

	// A hole at a corner leaves most of the chunk.
	const TSharedPtr<const FMantleTerrainHoleGrid> Corner = FMantleTerrainHoleGrid::FromMask(MakeMask(5, 5, { FIntPoint(0, 0) }), 5, 5);
	TestEqual(TEXT("a corner sample takes one quad"), Triangles(Field, 1, Corner.Get()), 30);
	TestEqual(TEXT("and one quad of the four at LOD 1"), Triangles(Field, 2, Corner.Get()), 6);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
