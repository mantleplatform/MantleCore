// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCoverage.h"
#include "MantleHeightfieldReader.h"
#include "MantleLandscapeMaterialReader.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceLimits.h"
#include "MantleTerrainExport.h"
#include "MantleTerrainGrassExport.h"

#include "Misc/AutomationTest.h"
#include "Misc/SecureHash.h"

#if WITH_DEV_AUTOMATION_TESTS

// Terrain tiles (stage 4, terrain-tiles-format.md): a landscape larger than one heightfield is cut at its own spacing into tiles of
// 2,048 quads that share their border samples, each a Terrain of its own, and one of at most 2,049 samples a side exports as it did.
// Everything here runs on sources built in code, as the other terrain tests do.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleTerrainTilesTestPrivate
{
	/** A height for sample (X, Y) that differs from its neighbours', so a tile cut from the wrong place is noticed. */
	uint16 HeightAt(const int32 X, const int32 Y)
	{
		return static_cast<uint16>((static_cast<uint32>(X) * 7919u + static_cast<uint32>(Y) * 104729u + static_cast<uint32>(X) * static_cast<uint32>(Y) * 31u) & 0xFFFFu);
	}

	FMantleTerrainSource MakeSource(const int32 Width, const int32 Height)
	{
		FMantleTerrainSource Source;
		Source.Width = Width;
		Source.Height = Height;
		Source.Heights.SetNumUninitialized(Width * Height);
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				Source.Heights[Y * Width + X] = HeightAt(X, Y);
			}
		}
		Source.Transform = FTransform(FQuat::Identity, FVector(1000.0, 2000.0, 300.0), FVector::OneVector);
		Source.LandscapePath = FSoftObjectPath(TEXT("/Game/Maps/Big.Big:PersistentLevel.Landscape1"));
		return Source;
	}

	FMantleLandscapeLayerLook MakeFlatLook(const TCHAR* Layer, const FLinearColor Color)
	{
		FMantleLandscapeLayerLook Look;
		Look.Layer = Layer;
		Look.bHasFlatColor = true;
		Look.FlatColor = Color;
		Look.bHasAverage = true;
		Look.Average = Color;
		return Look;
	}

	/** The first 24 lowercase hex digits of the MD5 of Text, as the id rule says. */
	FString Md5First24(const FString& Text)
	{
		FMD5 Md5;
		const FTCHARToUTF8 Utf8(*Text);
		Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		uint8 Digest[16];
		Md5.Final(Digest);
		FString Hex;
		for (const uint8 Byte : Digest)
		{
			Hex += FString::Printf(TEXT("%02x"), Byte);
		}
		return Hex.Left(24);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainTilesPlanTest,
	"Mantle.Editor.Terrain.Tiles.Plan",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainTilesPlanTest::RunTest(const FString& Parameters)
{
	// The edge sizes: tiles are 2,048 quads, cut from sample (0, 0), the last taking what is left.
	struct FCase { int32 Samples; int32 Tiles; int32 LastWidth; };
	const FCase Cases[] = { { 2, 1, 2 }, { 2048, 1, 2048 }, { 2049, 1, 2049 }, { 2050, 2, 2 }, { 4097, 2, 2049 }, { 4098, 3, 2 }, { 6145, 3, 2049 } };
	for (const FCase& Case : Cases)
	{
		const FString Name = FString::Printf(TEXT("%d samples"), Case.Samples);
		const TArray<FMantleTerrainTile> Tiles = FMantleTerrainExport::PlanTiles(Case.Samples, 5);
		TestEqual(*(Name + TEXT(": tile count")), Tiles.Num(), Case.Tiles);
		TestEqual(*(Name + TEXT(": TileCount agrees")), FMantleTerrainExport::TileCount(Case.Samples, 5), Case.Tiles);
		if (Tiles.Num() != Case.Tiles)
		{
			continue;
		}
		TestEqual(*(Name + TEXT(": the last tile's width")), Tiles.Last().Width, Case.LastWidth);
		TestEqual(*(Name + TEXT(": one tile on Y, all five rows")), Tiles.Last().Height, 5);
		// Neighbours share the border sample: each tile starts where the one before it ends.
		int32 Covered = 1;
		for (int32 Index = 0; Index < Tiles.Num(); ++Index)
		{
			TestEqual(*(Name + TEXT(": tile starts at 2048 x its index")), Tiles[Index].First.X, Index * 2048);
			TestTrue(*(Name + TEXT(": a neighbour follows all but the last")), Tiles[Index].bNeighborX == (Index < Tiles.Num() - 1));
			TestFalse(*(Name + TEXT(": none along Y")), Tiles[Index].bNeighborY);
			Covered += Tiles[Index].Width - 1;
		}
		TestEqual(*(Name + TEXT(": together they cover every sample")), Covered, Case.Samples);
	}

	// Both axes: row by row, (tx, ty) with the neighbour flags.
	{
		const TArray<FMantleTerrainTile> Tiles = FMantleTerrainExport::PlanTiles(4098, 2050);
		TestEqual(TEXT("4098 x 2050 is 3 x 2 tiles"), Tiles.Num(), 6);
		if (Tiles.Num() == 6)
		{
			TestTrue(TEXT("row by row"), Tiles[1].Index == FIntPoint(1, 0) && Tiles[3].Index == FIntPoint(0, 1));
			TestTrue(TEXT("tile (2, 1): the 2 x 2 corner"), Tiles[5].Width == 2 && Tiles[5].Height == 2 && Tiles[5].First == FIntPoint(4096, 2048));
			TestTrue(TEXT("tile (1, 0) has neighbours on both axes' sides it can: X and Y"), Tiles[1].bNeighborX && Tiles[1].bNeighborY);
			TestTrue(TEXT("tile (2, 0): only Y"), !Tiles[2].bNeighborX && Tiles[2].bNeighborY);
		}
	}
	// Published samples are the tiles' together (a border sample in both), and a small landscape's are its own.
	TestEqual(TEXT("2049 x 300 publishes its own samples"), FMantleTerrainExport::PublishedSamples(2049, 300), static_cast<int64>(2049) * 300);
	TestEqual(TEXT("4097 x 4097: four full tiles"), FMantleTerrainExport::PublishedSamples(4097, 4097), static_cast<int64>(4) * 2049 * 2049);
	TestEqual(TEXT("2050 x 2050: 2,049 + 2 columns and rows"), FMantleTerrainExport::PublishedSamples(2050, 2050), static_cast<int64>(2051) * 2051);
	TestEqual(TEXT("no ground counts nothing"), FMantleTerrainExport::PublishedSamples(1, 4097), static_cast<int64>(0));
	TestEqual(TEXT("a landscape with nothing loaded is one tile"), FMantleTerrainExport::TileCount(static_cast<const AActor*>(nullptr)), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainTilesBordersTest,
	"Mantle.Editor.Terrain.Tiles.HeightsAndSharedBorders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainTilesBordersTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainTilesTestPrivate;

	// 4,098 x 2,050 samples: 3 x 2 tiles, the last column and row only 2 samples.
	const FMantleTerrainSource Source = MakeSource(4098, 2050);
	const TArray<FMantleTerrainTile> Plan = FMantleTerrainExport::PlanTiles(Source.Width, Source.Height);
	TArray<FMantleTerrainSource> Tiles;
	int64 Mismatches = 0;
	for (const FMantleTerrainTile& Tile : Plan)
	{
		Tiles.Add(FMantleTerrainExport::CutTile(Source, Tile));
		const FMantleTerrainSource& Cut = Tiles.Last();
		if (Cut.Width != Tile.Width || Cut.Height != Tile.Height || Cut.Heights.Num() != Tile.Width * Tile.Height)
		{
			AddError(TEXT("a tile has the size the plan says"));
			return false;
		}
		// Every height is the landscape's own value, bit for bit, at the sample the tile starts from.
		for (int32 Y = 0; Y < Cut.Height; ++Y)
		{
			for (int32 X = 0; X < Cut.Width; ++X)
			{
				Mismatches += Cut.Heights[Y * Cut.Width + X] != Source.Heights[(Tile.First.Y + Y) * Source.Width + Tile.First.X + X] ? 1 : 0;
			}
		}
		// The heightfield published for it is those values, at the landscape's own spacing: nothing is resampled.
		const FMantleHeightfield Field = FMantleTerrainExport::MakeHeightfield(Cut);
		TestTrue(TEXT("its heightfield is its samples"), Field.Width == Cut.Width && Field.Height == Cut.Height && Field.Heights == Cut.Heights);
		TestEqual(TEXT("at the landscape's X spacing"), Field.SpacingX, static_cast<float>(Source.SpacingX));
		TestEqual(TEXT("and Y spacing"), Field.SpacingY, static_cast<float>(Source.SpacingY));
		TestEqual(TEXT("with its height scale"), Field.HeightScale, static_cast<float>(Source.HeightScale));
		TestEqual(TEXT("and offset"), Field.HeightOffset, static_cast<float>(Source.HeightOffset));
		TestTrue(TEXT("and no heightfield bigger than the format holds"), Field.Width <= FMantleHeightfieldReader::MaxSize && Field.Height <= FMantleHeightfieldReader::MaxSize);
		TestTrue(TEXT("the MHF1 file is made"), FMantleHeightfieldReader::Write(Field).Num() > 0);
		// The tile's transform: the landscape's, moved to the tile's first sample (spacing 100 cm).
		TestTrue(TEXT("its transform is moved to its first sample"),
			Cut.Transform.GetLocation().Equals(FVector(1000.0 + Tile.First.X * 100.0, 2000.0 + Tile.First.Y * 100.0, 300.0), 0.001));
		TestTrue(TEXT("its first sample in the landscape's grid"), Cut.FirstSample == Source.FirstSample + Tile.First);
	}
	TestEqual(TEXT("every height of every tile is the landscape's"), Mismatches, static_cast<int64>(0));

	// Neighbours share their border: the last column of a tile is the first column of the next, the last row the first row of the next.
	int32 BorderPairs = 0;
	for (int32 Index = 0; Index < Plan.Num(); ++Index)
	{
		for (int32 Other = 0; Other < Plan.Num(); ++Other)
		{
			const FMantleTerrainSource& A = Tiles[Index];
			const FMantleTerrainSource& B = Tiles[Other];
			if (Plan[Other].Index == Plan[Index].Index + FIntPoint(1, 0))
			{
				++BorderPairs;
				bool bEqual = A.Height == B.Height;
				for (int32 Y = 0; bEqual && Y < A.Height; ++Y)
				{
					bEqual = A.Heights[Y * A.Width + A.Width - 1] == B.Heights[Y * B.Width];
				}
				TestTrue(TEXT("the last column of a tile equals the first column of the next"), bEqual);
			}
			if (Plan[Other].Index == Plan[Index].Index + FIntPoint(0, 1))
			{
				++BorderPairs;
				bool bEqual = A.Width == B.Width;
				for (int32 X = 0; bEqual && X < A.Width; ++X)
				{
					bEqual = A.Heights[(A.Height - 1) * A.Width + X] == B.Heights[X];
				}
				TestTrue(TEXT("the last row of a tile equals the first row of the next"), bEqual);
			}
		}
	}
	TestEqual(TEXT("every neighbouring pair was compared: 2 + 2 along X, 3 along Y"), BorderPairs, 7);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainTilesSmallUnchangedTest,
	"Mantle.Editor.Terrain.Tiles.SmallLandscapeUnchanged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainTilesSmallUnchangedTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainTilesTestPrivate;

	// A landscape of at most 2,049 samples a side is one tile that is all of it, with everything it had.
	for (const FIntPoint Size : { FIntPoint(2049, 300), FIntPoint(300, 2049), FIntPoint(2049, 2049), FIntPoint(17, 9) })
	{
		const FMantleTerrainSource Source = MakeSource(Size.X, Size.Y);
		const TArray<FMantleTerrainTile> Plan = FMantleTerrainExport::PlanTiles(Source.Width, Source.Height);
		if (!TestEqual(TEXT("one tile"), Plan.Num(), 1))
		{
			continue;
		}
		TestTrue(TEXT("all of it"), Plan[0].Width == Size.X && Plan[0].Height == Size.Y && Plan[0].First == FIntPoint::ZeroValue && !Plan[0].bNeighborX && !Plan[0].bNeighborY);
		TestEqual(TEXT("its entity name is the label"), FMantleTerrainExport::TileName(TEXT("Hills"), Plan[0].Index, 1), FString(TEXT("Hills")));
		TestEqual(TEXT("its entity id is the landscape's"), FMantleTerrainExport::TileEntityId(TEXT("ent_0123456789abcdef0123456789abcdef"), Plan[0].Index), FString(TEXT("ent_0123456789abcdef0123456789abcdef")));
		TestTrue(TEXT("its keys have no tile in them"), FMantleTerrainExport::TileSuffix(Plan[0].Index, 1).IsEmpty());
		TestEqual(TEXT("its layers keep the landscape's tiling"), FMantleTerrainExport::TileTiling(300.0, Plan.Num(), Source.SpacingX), 300.0);
		TestEqual(TEXT("it publishes its own samples"), FMantleTerrainExport::PublishedSamples(Size.X, Size.Y), static_cast<int64>(Size.X) * Size.Y);
		TestTrue(TEXT("its transform is the landscape's"), FMantleTerrainExport::TileTransform(Source, Plan[0]).Equals(Source.Transform, 0.0));

		// The same heightfield bytes, cut or not.
		const FMantleTerrainSource Cut = FMantleTerrainExport::CutTile(Source, Plan[0]);
		TestTrue(TEXT("a cut of it has the same heightfield file"),
			FMantleHeightfieldReader::Write(FMantleTerrainExport::MakeHeightfield(Cut)) == FMantleHeightfieldReader::Write(FMantleTerrainExport::MakeHeightfield(Source)));
		TestTrue(TEXT("and the same samples"), Cut.Heights == Source.Heights);
	}
	TestTrue(TEXT("no Check message for a landscape that is one tile"), FMantleTerrainExport::TileCount(2049, 2049) == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainTilesIdsTest,
	"Mantle.Editor.Terrain.Tiles.IdsAndNames",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainTilesIdsTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainTilesTestPrivate;

	const FString Guid = TEXT("0123456789abcdef0123456789abcdef");
	const FString Base = TEXT("ent_") + Guid;
	TestEqual(TEXT("tile (0, 0) keeps the landscape's id"), FMantleTerrainExport::TileEntityId(Base, FIntPoint(0, 0)), Base);

	// The rule: ent_ and the first 24 hex digits of MD5("<guid>|T|<tx>|<ty>").
	TestEqual(TEXT("tile (1, 0)"), FMantleTerrainExport::TileEntityId(Base, FIntPoint(1, 0)), TEXT("ent_") + Md5First24(Guid + TEXT("|T|1|0")));
	TestEqual(TEXT("tile (0, 3)"), FMantleTerrainExport::TileEntityId(Base, FIntPoint(0, 3)), TEXT("ent_") + Md5First24(Guid + TEXT("|T|0|3")));
	TestEqual(TEXT("tile (12, 7)"), FMantleTerrainExport::TileEntityId(Base, FIntPoint(12, 7)), TEXT("ent_") + Md5First24(Guid + TEXT("|T|12|7")));

	// Every id is the schema's (^ent_[a-z0-9_]{1,40}$), different from every other, and the same on another call (a republish keeps them).
	TSet<FString> Seen;
	for (int32 Y = 0; Y < 6; ++Y)
	{
		for (int32 X = 0; X < 6; ++X)
		{
			const FString Id = FMantleTerrainExport::TileEntityId(Base, FIntPoint(X, Y));
			TestEqual(TEXT("stable"), Id, FMantleTerrainExport::TileEntityId(Base, FIntPoint(X, Y)));
			bool bValid = Id.StartsWith(TEXT("ent_")) && Id.Len() - 4 >= 1 && Id.Len() - 4 <= 40;
			for (int32 Index = 4; bValid && Index < Id.Len(); ++Index)
			{
				const TCHAR Char = Id[Index];
				bValid = (Char >= TEXT('a') && Char <= TEXT('z')) || (Char >= TEXT('0') && Char <= TEXT('9')) || Char == TEXT('_');
			}
			TestTrue(*FString::Printf(TEXT("tile (%d, %d)'s id is a schema id"), X, Y), bValid);
			Seen.Add(Id);
		}
	}
	TestEqual(TEXT("36 tiles, 36 different ids"), Seen.Num(), 36);
	TestNotEqual(TEXT("another landscape's tile (1, 0) differs"), FMantleTerrainExport::TileEntityId(TEXT("ent_ffffffffffffffffffffffffffffffff"), FIntPoint(1, 0)), FMantleTerrainExport::TileEntityId(Base, FIntPoint(1, 0)));

	// A landscape whose id came from its label (no guid) gets tile ids from that.
	TestEqual(TEXT("from a label-derived id too"), FMantleTerrainExport::TileEntityId(TEXT("ent_hills"), FIntPoint(1, 1)), TEXT("ent_") + Md5First24(TEXT("hills|T|1|1")));

	// Names: the landscape's name for one tile, else <name>_<tx>_<ty>, within the entity name limit.
	TestEqual(TEXT("one tile keeps the name"), FMantleTerrainExport::TileName(TEXT("Hills"), FIntPoint(0, 0), 1), FString(TEXT("Hills")));
	TestEqual(TEXT("a tile of several"), FMantleTerrainExport::TileName(TEXT("Hills"), FIntPoint(2, 1), 6), FString(TEXT("Hills_2_1")));
	TestEqual(TEXT("its first one too"), FMantleTerrainExport::TileName(TEXT("Hills"), FIntPoint(0, 0), 6), FString(TEXT("Hills_0_0")));
	const FString Long = FString::ChrN(FMantlePlaceLimits::MaxEntityNameLength + 10, TEXT('n'));
	const FString LongName = FMantleTerrainExport::TileName(Long, FIntPoint(10, 3), 20);
	TestEqual(TEXT("a long name is cut so the whole fits"), LongName.Len(), FMantlePlaceLimits::MaxEntityNameLength);
	TestTrue(TEXT("and still ends with the tile"), LongName.EndsWith(TEXT("_10_3")));
	TestEqual(TEXT("a long name of one tile is cut as before"), FMantleTerrainExport::TileName(Long, FIntPoint(0, 0), 1), Long.Left(FMantlePlaceLimits::MaxEntityNameLength));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainTilesTilingTest,
	"Mantle.Editor.Terrain.Tiles.TilingMakesWholeRepeats",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainTilesTilingTest::RunTest(const FString& Parameters)
{
	// One tiling for every tile of a landscape of several tiles, from a full tile's X extent (2,048 quads x spacing).
	const double Cap = FMantlePlaceLimits::MaxTerrainTilingCm;
	// 400 cm repeats 512 times over 204,800 cm: no change.
	TestEqual(TEXT("400 cm over a full tile is 512 repeats, unchanged"), FMantleTerrainExport::TileTiling(400.0, 9, 100.0), 400.0);
	// 300 cm would be 682.67 repeats: 683, a tiling of 299.85 cm.
	const double Stretched = FMantleTerrainExport::TileTiling(300.0, 9, 100.0);
	TestEqual(TEXT("300 cm becomes 683 repeats"), 204800.0 / Stretched, 683.0, 1e-9);
	TestTrue(TEXT("a tiling of 299.85 cm"), FMath::Abs(Stretched - 299.853) < 0.001);
	TestTrue(TEXT("a change of 0.05 percent"), FMath::Abs(Stretched / 300.0 - 1.0) < 0.0006);
	TestEqual(TEXT("the same for two tiles: the number depends on the spacing, not on which tile"), FMantleTerrainExport::TileTiling(300.0, 2, 100.0), Stretched);
	// Other spacing: the extent is 2,048 x spacing along X.
	TestEqual(TEXT("at 50 cm: 102,400 cm, 300 cm is 341 repeats"), FMantleTerrainExport::TileTiling(300.0, 4, 50.0), 102400.0 / 341.0);
	TestEqual(TEXT("at 60 cm (Medieval): 122,880 cm, 2,940 cm is 42 repeats"), FMantleTerrainExport::TileTiling(2940.0, 2, 60.0), 122880.0 / 42.0);
	// Never past the cap a layer's tiling is held to (the backend and the game parser refuse it): at least ceil(extent / cap) repeats.
	for (const double Tiling : { 81920.0, 90000.0, Cap })
	{
		const double Out = FMantleTerrainExport::TileTiling(Tiling, 4, 100.0);
		TestTrue(*FString::Printf(TEXT("%g cm stays within the cap (got %g)"), Tiling, Out), Out > 0.0 && Out <= Cap);
		TestEqual(*FString::Printf(TEXT("%g cm is a whole number of repeats"), Tiling), 204800.0 / Out, FMath::RoundToDouble(204800.0 / Out), 1e-9);
	}
	TestTrue(TEXT("at 60 cm, 100,000 cm is within the cap too"), FMantleTerrainExport::TileTiling(Cap, 4, 60.0) <= Cap);
	TestEqual(TEXT("a tiling beyond the tile is the fewest repeats the cap allows"), FMantleTerrainExport::TileTiling(900000.0, 4, 100.0), 204800.0 / FMath::CeilToDouble(204800.0 / Cap));
	// A one-tile landscape keeps the landscape's tiling, and a layer with none stays so.
	TestEqual(TEXT("a one-tile landscape keeps 300"), FMantleTerrainExport::TileTiling(300.0, 1, 100.0), 300.0);
	TestEqual(TEXT("and keeps one over the tile's extent, even past the cap's repeats"), FMantleTerrainExport::TileTiling(Cap, 1, 100.0), Cap);
	TestEqual(TEXT("a layer with no tiling stays so"), FMantleTerrainExport::TileTiling(0.0, 4, 100.0), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainTilesKeysTest,
	"Mantle.Editor.Terrain.Tiles.UploadKeys",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainTilesKeysTest::RunTest(const FString& Parameters)
{
	// The suffix: "#tx,ty" for a landscape of several tiles, nothing for one (its keys are what they were).
	TestEqual(TEXT("a tile's suffix"), FMantleTerrainExport::TileSuffix(FIntPoint(3, 1), 8), FString(TEXT("#3,1")));
	TestEqual(TEXT("tile (0, 0) of several carries it too"), FMantleTerrainExport::TileSuffix(FIntPoint(0, 0), 2), FString(TEXT("#0,0")));
	TestTrue(TEXT("a one-tile landscape has none"), FMantleTerrainExport::TileSuffix(FIntPoint(0, 0), 1).IsEmpty());

	// Every kind of key ends with it, and the keys of two tiles differ for each kind.
	const FString Path = TEXT("/Game/Maps/Big.Big:PersistentLevel.Landscape1");
	const FString A = FMantleTerrainExport::TileSuffix(FIntPoint(0, 1), 4);
	const FString B = FMantleTerrainExport::TileSuffix(FIntPoint(1, 1), 4);
	TestEqual(TEXT("the heightfield key"), FMantleTerrainExport::HeightfieldKey(nullptr) + A, FString(TEXT("heightfield:#0,1")));
	TestEqual(TEXT("the weightmap key"), FMantleTerrainExport::WeightmapKey(nullptr) + B, FString(TEXT("weightmap:#1,1")));
	TestEqual(TEXT("the second weightmap key from the path"), FMantleTerrainExport::Weightmap2KeyForPath(Path + A), TEXT("weightmap2:") + Path + TEXT("#0,1"));
	TestEqual(TEXT("the holes key from the path"), FMantleTerrainExport::HolesKeyForPath(Path + B), TEXT("terrainholes:") + Path + TEXT("#1,1"));
	TestEqual(TEXT("a grass map's key"), FMantleTerrainGrassExport::GrassMapKey(nullptr, 2, A), FString(TEXT("grassmap::2#0,1")));
	TestEqual(TEXT("without a tile it is what it was"), FMantleTerrainGrassExport::GrassMapKey(nullptr, 2), FString(TEXT("grassmap::2")));
	TestNotEqual(TEXT("two tiles' grass maps are two assets"), FMantleTerrainGrassExport::GrassMapKey(nullptr, 0, A), FMantleTerrainGrassExport::GrassMapKey(nullptr, 0, B));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainTilesCheckTest,
	"Mantle.Editor.Terrain.Tiles.CheckMessage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainTilesCheckTest::RunTest(const FString& Parameters)
{
	// Check says how many tiles, and no longer that the landscape is resampled.
	const FMantleValidationIssue Issue = FMantleTerrainExport::TilesIssue(TEXT("Big"), 4097, 4097);
	TestEqual(TEXT("under the landscape's label"), Issue.Actor, FString(TEXT("Big")));
	TestEqual(TEXT("a warning, not an error"), Issue.Severity, EMantleIssueSeverity::Warning);
	TestTrue(TEXT("it says the landscape publishes as 4 terrain tiles"), Issue.Problem.Contains(TEXT("This landscape publishes as 4 terrain tiles")));
	TestTrue(TEXT("with the grid"), Issue.Problem.Contains(TEXT("(2 x 2)")));
	TestFalse(TEXT("and nothing about resampling"), Issue.Problem.Contains(TEXT("resampl")) || Issue.Remedy.Contains(TEXT("resampl")) || Issue.Problem.Contains(TEXT("players get")));
	TestTrue(TEXT("a remedy that asks for nothing"), Issue.Remedy.StartsWith(TEXT("Nothing to do")));
	TestTrue(TEXT("4,098 x 2,050 is 6"), FMantleTerrainExport::TilesIssue(TEXT("Big"), 4098, 2050).Problem.Contains(TEXT("publishes as 6 terrain tiles")));

	// Past the place's terrain count, the landscape is left out and the message counts its tiles.
	TArray<TPair<int32, FMantleValidationIssue>> LeftOut;
	const TArray<int64> Samples = { 100, 100, 100 };
	const TArray<FString> Labels = { TEXT("Small"), TEXT("Wide"), TEXT("Tiny") };
	const TArray<int32> Tiles = { 1, FMantlePlaceLimits::MaxTerrainsPerPlace, 1 };
	const TArray<int32> Kept = FMantleTerrainExport::FitTerrains(Samples, Labels, LeftOut, Tiles);
	TestTrue(TEXT("a landscape of as many tiles as the place holds does not fit after another"), Kept == TArray<int32>{ 0, 2 });
	if (TestEqual(TEXT("one is left out"), LeftOut.Num(), 1))
	{
		TestEqual(TEXT("the wide one"), LeftOut[0].Key, 1);
		TestTrue(TEXT("it says how many terrains it is"), LeftOut[0].Value.Problem.Contains(FString::Printf(TEXT("publishes as %d terrain tiles"), FMantlePlaceLimits::MaxTerrainsPerPlace)));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainTilesCutTest,
	"Mantle.Editor.Terrain.Tiles.PaintHolesAndLayersPerTile",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainTilesCutTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainTilesTestPrivate;

	// 2,050 x 4 samples: two tiles along X (2,049 and 2 columns). Two layers, Grass in the first 1,000 columns and Rock past them; one hole, in the second tile only.
	FMantleTerrainSource Source = MakeSource(2050, 4);
	Source.bMaterialRead = true;
	Source.MaterialRead.Layers.Add(MakeFlatLook(TEXT("Grass"), FLinearColor(0.1f, 0.5f, 0.1f)));
	Source.MaterialRead.Layers.Add(MakeFlatLook(TEXT("Rock"), FLinearColor(0.4f, 0.4f, 0.4f)));
	Source.MaterialRead.bFoundLayers = true;
	Source.LayerNames = { TEXT("Grass"), TEXT("Rock") };
	Source.LayerWeights.SetNum(2);
	Source.LayerWeights[0].SetNumZeroed(2050 * 4);
	Source.LayerWeights[1].SetNumZeroed(2050 * 4);
	for (int32 Y = 0; Y < 4; ++Y)
	{
		for (int32 X = 0; X < 2050; ++X)
		{
			Source.LayerWeights[X < 1000 ? 0 : 1][Y * 2050 + X] = 255;
		}
	}
	Source.Holes.SetNumZeroed(2050 * 4);
	Source.Holes[1 * 2050 + 2049] = 1;
	Source.bHasHoles = true;
	Source.HoleSamples = 1;

	const TArray<FMantleTerrainTile> Plan = FMantleTerrainExport::PlanTiles(Source.Width, Source.Height);
	if (!TestEqual(TEXT("two tiles"), Plan.Num(), 2))
	{
		return false;
	}
	TestFalse(TEXT("the first tile has no hole"), FMantleTerrainExport::TileHasHoles(Source, Plan[0]));
	TestTrue(TEXT("the second has the hole"), FMantleTerrainExport::TileHasHoles(Source, Plan[1]));
	const FMantleTerrainSource First = FMantleTerrainExport::CutTile(Source, Plan[0]);
	const FMantleTerrainSource Second = FMantleTerrainExport::CutTile(Source, Plan[1]);
	TestFalse(TEXT("so the first publishes no hole mask"), FMantleTerrainExport::HasHoles(First));
	TestTrue(TEXT("and the second does"), FMantleTerrainExport::HasHoles(Second));
	TestTrue(TEXT("whose mask marks the hole at its own sample (1, 1)"), Second.Holes.Num() == 2 * 4 && Second.Holes[1 * 2 + 1] == 1 && Second.Holes[0] == 0);
	TestEqual(TEXT("the hole mask is a texel a tile sample"), FMantleTerrainExport::MakeHoleMask(Second, FIntPoint(2, 4)).Num(), 8);

	// The same layers in every tile, each with its own part of the paint.
	TestTrue(TEXT("both layers in both tiles"), First.LayerNames == Source.LayerNames && Second.LayerNames == Source.LayerNames);
	TestTrue(TEXT("weights cut to the tile's size"), First.LayerWeights.Num() == 2 && First.LayerWeights[0].Num() == 2049 * 4 && Second.LayerWeights[1].Num() == 2 * 4);
	TestEqual(TEXT("the first tile's Grass is the landscape's at that sample"), static_cast<int32>(First.LayerWeights[0][2 * 2049 + 999]), 255);
	TestEqual(TEXT("and its Rock past column 1,000"), static_cast<int32>(First.LayerWeights[1][2 * 2049 + 1000]), 255);
	TestEqual(TEXT("the second tile is all Rock"), static_cast<int32>(Second.LayerWeights[1][3]), 255);
	TestEqual(TEXT("and has no Grass, though the layer is listed"), static_cast<int32>(Second.LayerWeights[0][3]), 0);

	// Converting makes a heightfield, a weightmap and (second tile) a hole mask for each tile, under keys that end with the tile.
	FMantlePlaceAssetConverter Converter;
	FMantlePlaceAssetSet Out;
	const FString Path = TEXT("/Game/Maps/Big.Big:PersistentLevel.Landscape1");
	TestTrue(TEXT("the landscape converts"), Converter.ConvertTerrainSource(nullptr, Source, TEXT("heightfield:") + Path, TEXT("weightmap:") + Path, TEXT("Big"), Path, Out));
	if (TestEqual(TEXT("one heightfield for each tile"), Out.Heightfields.Num(), 2))
	{
		TestEqual(TEXT("the first under #0,0"), Out.Heightfields[0].Key, TEXT("heightfield:") + Path + TEXT("#0,0"));
		TestEqual(TEXT("the second under #1,0"), Out.Heightfields[1].Key, TEXT("heightfield:") + Path + TEXT("#1,0"));
		TestTrue(TEXT("at the tile's own size"), Out.Heightfields[0].Width == 2049 && Out.Heightfields[0].Height == 4 && Out.Heightfields[1].Width == 2 && Out.Heightfields[1].Height == 4);
		TestEqual(TEXT("each tile's weightmap key"), Out.Heightfields[1].WeightmapKey, TEXT("weightmap:") + Path + TEXT("#1,0"));
		TestNotNull(TEXT("the first weightmap is a texture"), Out.FindTexture(TEXT("weightmap:") + Path + TEXT("#0,0")));
		TestNotNull(TEXT("and the second"), Out.FindTexture(TEXT("weightmap:") + Path + TEXT("#1,0")));
		TestNull(TEXT("the tile with no hole has no hole mask"), Out.FindTexture(TEXT("terrainholes:") + Path + TEXT("#0,0")));
		TestNotNull(TEXT("the tile with the hole does"), Out.FindTexture(TEXT("terrainholes:") + Path + TEXT("#1,0")));
		TestNull(TEXT("no landscape-wide asset is left under the plain key"), Out.FindHeightfield(TEXT("heightfield:") + Path));
		TestTrue(TEXT("the first heightfield is the landscape's first 2,049 columns, read back"),
			[&]()
			{
				FMantleHeightfield Back;
				if (!FMantleHeightfieldReader::Read(Out.Heightfields[0].Bytes, Back) || Back.Width != 2049 || Back.Height != 4)
				{
					return false;
				}
				for (int32 Y = 0; Y < 4; ++Y)
				{
					for (int32 X = 0; X < 2049; ++X)
					{
						if (Back.Heights[Y * 2049 + X] != Source.Heights[Y * 2050 + X])
						{
							return false;
						}
					}
				}
				return true;
			}());
	}
	for (const FMantleValidationIssue& Issue : Out.Issues)
	{
		TestFalse(TEXT("no message says the landscape was resampled"), Issue.Problem.Contains(TEXT("resampled")));
	}

	// A landscape of one tile converts under the keys it always had.
	FMantlePlaceAssetSet Small;
	const FMantleTerrainSource SmallSource = [&Source]()
	{
		FMantleTerrainSource Cut = FMantleTerrainExport::CutTile(Source, FMantleTerrainExport::PlanTiles(Source.Width, Source.Height)[0]);
		return Cut;
	}();
	TestTrue(TEXT("a small landscape converts"), Converter.ConvertTerrainSource(nullptr, SmallSource, TEXT("heightfield:") + Path, TEXT("weightmap:") + Path, TEXT("Small"), Path, Small));
	TestTrue(TEXT("under its plain keys"), Small.FindHeightfield(TEXT("heightfield:") + Path) != nullptr && Small.FindTexture(TEXT("weightmap:") + Path) != nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainTilesFallbackTest,
	"Mantle.Editor.Terrain.Tiles.TooManyForThePlaceFallsBackToResampled",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainTilesFallbackTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainTilesTestPrivate;

	// Every case is built from the place's limits as the synced MantlePlaceLimits.h has them, so it holds for any cap.
	const int64 Cap = FMantlePlaceLimits::MaxTerrainSamplesPerPlace;
	const int32 MaxTerrains = FMantlePlaceLimits::MaxTerrainsPerPlace;
	const int64 OneField = static_cast<int64>(FMantleHeightfieldReader::MaxSize) * FMantleHeightfieldReader::MaxSize;
	const TArray<FString> Labels = { TEXT("Small"), TEXT("Huge") };
	auto Fit = [](TArray<int64> S, TArray<FString> L, TArray<TPair<int32, FMantleValidationIssue>>& Left, TArray<int32> T, TArray<int64> R, TArray<int32>* Res)
	{
		return FMantleTerrainExport::FitTerrains(S, L, Left, T, R, Res);
	};
	TestTrue(TEXT("the limits leave room for these cases"), Cap > 3 * OneField && MaxTerrains > 6);

	// The tiles of the second landscape would take the place past the samples cap: it publishes resampled to one Terrain instead, not left out.
	{
		TArray<TPair<int32, FMantleValidationIssue>> LeftOut;
		TArray<int32> Resampled;
		const TArray<int32> Kept = Fit({ 100, Cap }, Labels, LeftOut, { 1, 4 }, { 100, OneField }, &Resampled);
		TestTrue(TEXT("both publish"), Kept == TArray<int32>{ 0, 1 });
		TestEqual(TEXT("none is left out"), LeftOut.Num(), 0);
		TestTrue(TEXT("the second as one resampled Terrain"), Resampled == TArray<int32>{ 1 });
	}
	// Too many Terrains rather than samples: the same.
	{
		TArray<TPair<int32, FMantleValidationIssue>> LeftOut;
		TArray<int32> Resampled;
		const TArray<int32> Kept = Fit({ 100, 1000 }, Labels, LeftOut, { 1, MaxTerrains }, { 100, 1000 }, &Resampled);
		TestTrue(TEXT("both publish"), Kept == TArray<int32>{ 0, 1 } && LeftOut.Num() == 0);
		TestTrue(TEXT("the second resampled"), Resampled == TArray<int32>{ 1 });
	}
	// One whose tiles fit still tiles.
	{
		TArray<TPair<int32, FMantleValidationIssue>> LeftOut;
		TArray<int32> Resampled;
		const TArray<int32> Kept = Fit({ 100, Cap - 100 }, Labels, LeftOut, { 1, 4 }, { 100, OneField }, &Resampled);
		TestTrue(TEXT("both publish"), Kept == TArray<int32>{ 0, 1 } && LeftOut.Num() == 0);
		TestEqual(TEXT("as tiles: none resampled"), Resampled.Num(), 0);
	}
	// Several compete, in order: the first takes the room as tiles, the second no longer fits as tiles and falls back; a third small one still fits.
	{
		TArray<TPair<int32, FMantleValidationIssue>> LeftOut;
		TArray<int32> Resampled;
		const int64 FirstSamples = Cap - OneField - 100;
		const TArray<int32> Kept = Fit({ FirstSamples, 4 * OneField, 50 }, { TEXT("A"), TEXT("B"), TEXT("C") }, LeftOut, { 2, 4, 1 },
			{ OneField, OneField, 50 }, &Resampled);
		TestTrue(TEXT("all publish"), Kept == TArray<int32>{ 0, 1, 2 } && LeftOut.Num() == 0);
		TestTrue(TEXT("only the second is resampled"), Resampled == TArray<int32>{ 1 });
	}
	// Not even resampled does it fit: left out, as before tiles, with the errors for one Terrain.
	{
		TArray<TPair<int32, FMantleValidationIssue>> LeftOut;
		TArray<int32> Resampled;
		const TArray<int32> Kept = Fit({ Cap - 10, 4 * OneField }, Labels, LeftOut, { 1, 4 }, { Cap - 10, OneField }, &Resampled);
		TestTrue(TEXT("the first publishes"), Kept == TArray<int32>{ 0 });
		TestEqual(TEXT("the second is left out"), LeftOut.Num(), 1);
		TestEqual(TEXT("not as a resampled one"), Resampled.Num(), 0);
		if (LeftOut.Num() == 1)
		{
			TestTrue(TEXT("with the samples error"), LeftOut[0].Value.Problem.Contains(TEXT("left out")));
		}
	}
	// Without a list to report in, a landscape is never kept resampled silently: it is left out as before.
	{
		TArray<TPair<int32, FMantleValidationIssue>> LeftOut;
		const TArray<int32> Kept = Fit({ 100, Cap }, Labels, LeftOut, { 1, 4 }, { 100, OneField }, nullptr);
		TestTrue(TEXT("the first publishes, the second is left out"), Kept == TArray<int32>{ 0 } && LeftOut.Num() == 1);
	}
	// A one-tile landscape left out because the tiles before it used up the Terrains says so, not "number N of at most M landscapes".
	{
		TArray<TPair<int32, FMantleValidationIssue>> LeftOut;
		TArray<int32> Resampled;
		const TArray<int32> Kept = Fit({ 100, 100 }, Labels, LeftOut, { MaxTerrains, 1 }, { 100, 100 }, &Resampled);
		TestTrue(TEXT("the first publishes, the second is left out"), Kept == TArray<int32>{ 0 } && LeftOut.Num() == 1);
		if (LeftOut.Num() == 1)
		{
			TestTrue(TEXT("naming the terrains the earlier tiles took"), LeftOut[0].Value.Problem.Contains(FString::Printf(TEXT("already publish as %d terrains"), MaxTerrains)));
		}
	}

	// Check's words for it.
	FMantleHeightfield Field;
	Field.Width = 2049;
	Field.Height = 1500;
	Field.SpacingX = 120.0f;
	Field.SpacingY = 100.0f;
	const FMantleValidationIssue Issue = FMantleTerrainExport::ResampledIssue(TEXT("Huge"), 4097, 3000, Field);
	TestEqual(TEXT("a warning"), Issue.Severity, EMantleIssueSeverity::Warning);
	TestTrue(TEXT("says why: the place's limits, both of them, from the synced file"),
		Issue.Problem.Contains(TEXT("would take the place past its limits")) && Issue.Problem.Contains(FString::Printf(TEXT("at most %d terrains"), FMantlePlaceLimits::MaxTerrainsPerPlace))
		&& Issue.Problem.Contains(FText::AsNumber(FMantlePlaceLimits::MaxTerrainSamplesPerPlace).ToString()) && Issue.Problem.Contains(TEXT("resampled to 2049 x 1500")));
	TestTrue(TEXT("and one remedy"), Issue.Remedy.Contains(TEXT("fewer samples")) && Issue.Remedy.Contains(TEXT("split the level into more than one place")) && !Issue.Remedy.Contains(TEXT("coarser landscape")));

	// The conversion: one resampled heightfield under the plain keys, as before tiles, and the warning.
	FMantleTerrainSource Source = MakeSource(2050, 3);
	const TArray<FMantleTerrainTile> One = FMantleTerrainExport::PlanTiles(Source.Width, Source.Height, true);
	TestTrue(TEXT("one tile that is all of it"), One.Num() == 1 && One[0].Width == 2050 && One[0].Height == 3 && !One[0].bNeighborX);
	TestEqual(TEXT("it is two tiles when they fit"), FMantleTerrainExport::PlanTiles(Source.Width, Source.Height, false).Num(), 2);
	FMantlePlaceAssetConverter Converter;
	FMantlePlaceAssetSet Out;
	const FString Path = TEXT("/Game/Maps/Big.Big:PersistentLevel.Landscape1");
	TestTrue(TEXT("converts"), Converter.ConvertTerrainSource(nullptr, Source, TEXT("heightfield:") + Path, TEXT("weightmap:") + Path, TEXT("Huge"), Path, Out, /*bResampled*/ true));
	if (TestEqual(TEXT("one heightfield"), Out.Heightfields.Num(), 1))
	{
		TestEqual(TEXT("under the plain key"), Out.Heightfields[0].Key, TEXT("heightfield:") + Path);
		TestTrue(TEXT("resampled to 2,049 wide"), Out.Heightfields[0].Width == 2049 && Out.Heightfields[0].Height == 3);
	}
	bool bWarned = false;
	for (const FMantleValidationIssue& Warning : Out.Issues)
	{
		bWarned = bWarned || Warning.Problem.Contains(TEXT("would take the place past its limits"));
	}
	TestTrue(TEXT("with the warning"), bWarned);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainTilesMoreCasesTest,
	"Mantle.Editor.Terrain.Tiles.RotationSlopeGrassAndMissingSamples",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainTilesMoreCasesTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainTilesTestPrivate;

	// A landscape turned 90 degrees about Z: tile (1, 1)'s first sample is 2,048 x spacing along the turned axes.
	{
		FMantleTerrainSource Source = MakeSource(2050, 2050);
		Source.SpacingX = 50.0;
		Source.SpacingY = 80.0;
		Source.Transform = FTransform(FQuat(FVector::UpVector, UE_PI * 0.5), FVector(1000.0, 2000.0, 300.0), FVector::OneVector);
		const TArray<FMantleTerrainTile> Plan = FMantleTerrainExport::PlanTiles(Source.Width, Source.Height);
		const FMantleTerrainSource Cut = FMantleTerrainExport::CutTile(Source, Plan[3]);
		// Local offset (2048 x 50, 2048 x 80, 0) turned by 90 degrees is (-2048 x 80, 2048 x 50, 0).
		TestTrue(TEXT("tile (1, 1) is moved along the turned axes"),
			Cut.Transform.GetLocation().Equals(FVector(1000.0 - 2048.0 * 80.0, 2000.0 + 2048.0 * 50.0, 300.0), 0.01));
		TestTrue(TEXT("and keeps the rotation"), Cut.Transform.GetRotation().Equals(Source.Transform.GetRotation(), 1e-6));
		TestTrue(TEXT("and scale 1"), Cut.Transform.GetScale3D().Equals(FVector::OneVector));
		TestTrue(TEXT("TileTransform says the same"), FMantleTerrainExport::TileTransform(Source, Plan[3]).Equals(Cut.Transform, 0.01));
	}

	// A slope overlay baked over the whole landscape is what each tile has: its border texels are the whole landscape's there.
	{
		FMantleTerrainSource Source = MakeSource(2050, 4);
		TArray<uint8> Whole;
		Whole.SetNum(2050 * 4);
		for (int32 Index = 0; Index < Whole.Num(); ++Index)
		{
			Whole[Index] = static_cast<uint8>((Index * 37) & 0xFF);
		}
		const TArray<FMantleTerrainTile> Plan = FMantleTerrainExport::PlanTiles(Source.Width, Source.Height);
		const FMantleTerrainSource First = FMantleTerrainExport::CutTile(Source, Plan[0], &Whole);
		const FMantleTerrainSource Second = FMantleTerrainExport::CutTile(Source, Plan[1], &Whole);
		bool bFirst = First.SlopeMask.Num() == 2049 * 4;
		bool bSecond = Second.SlopeMask.Num() == 2 * 4;
		for (int32 Y = 0; bFirst && bSecond && Y < 4; ++Y)
		{
			bFirst = bFirst && First.SlopeMask[Y * 2049 + 2048] == Whole[Y * 2050 + 2048];
			bSecond = bSecond && Second.SlopeMask[Y * 2] == Whole[Y * 2050 + 2048] && Second.SlopeMask[Y * 2 + 1] == Whole[Y * 2050 + 2049];
		}
		TestTrue(TEXT("the first tile's border column is the landscape's bake there"), bFirst);
		TestTrue(TEXT("the second tile's columns are the landscape's bake there, and its first equals the first tile's last"), bSecond);
		Source.SlopeMask = Whole;
		TestTrue(TEXT("a source that already has the whole bake hands it on"), FMantleTerrainExport::BakeSlopeFor(Source, TArray<FMantleTerrainLayerChoice>()) == Whole);
		TestEqual(TEXT("a tile cut without a bake has none"), FMantleTerrainExport::CutTile(Source, Plan[1]).SlopeMask.Num(), 0);
	}

	// Grass maps per tile: a tile's map is made from its own slice of each grass type's weights.
	{
		const int32 Width = 2050;
		const int32 Height = 4;
		TArray<uint8> Weights;
		Weights.Init(10, Width * Height);
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 2048; X < Width; ++X)
			{
				Weights[Y * Width + X] = 200;
			}
		}
		const TArray<FMantleTerrainTile> Plan = FMantleTerrainExport::PlanTiles(Width, Height);
		const TArray<TArray<uint8>> Second = { FMantleTerrainExport::SliceGrid(Weights, Width, Height, Plan[1]) };
		const TArray<TArray<uint8>> First = { FMantleTerrainExport::SliceGrid(Weights, Width, Height, Plan[0]) };
		TestTrue(TEXT("a tile's slice is its size"), Second[0].Num() == 2 * 4 && First[0].Num() == 2049 * 4);
		const TArray<int32> Indices = { 0, INDEX_NONE, INDEX_NONE, INDEX_NONE };
		const TArray<FColor> MapSecond = FMantleTerrainGrassExport::MakeMap(Second, Indices, 2, 4, FIntPoint(2, 4));
		const TArray<FColor> MapFirst = FMantleTerrainGrassExport::MakeMap(First, Indices, 2049, 4, FIntPoint(2048, 4));
		TestTrue(TEXT("the second tile's map is its own weights (200)"), MapSecond.Num() == 8 && MapSecond[0].R == 200 && MapSecond[7].R == 200);
		TestTrue(TEXT("the first tile's map starts with its own weights (10)"), MapFirst.Num() == 2048 * 4 && MapFirst[0].R == 10 && MapFirst[1000].R == 10);
		TestEqual(TEXT("a slice of a grid of the wrong size is empty"), FMantleTerrainExport::SliceGrid(Weights, Width, 3, Plan[1]).Num(), 0);
	}

	// Samples no component covers: a tile counts its own, and its painted holes are the rest.
	{
		FMantleTerrainSource Source = MakeSource(2050, 4);
		TArray<uint8> Covered;
		Covered.Init(1, 2050 * 4);
		Covered[0 * 2050 + 2049] = 0;
		Covered[1 * 2050 + 2049] = 0;
		Covered[2 * 2050 + 10] = 0;
		FMantleTerrainExport::ApplyCoverage(Source, Covered);
		Source.Holes[3 * 2050 + 2048] = 1;
		Source.HoleSamples = 1;
		TestEqual(TEXT("the landscape has three uncovered samples"), Source.MissingSamples, 3);
		const TArray<FMantleTerrainTile> Plan = FMantleTerrainExport::PlanTiles(Source.Width, Source.Height);
		const FMantleTerrainSource First = FMantleTerrainExport::CutTile(Source, Plan[0]);
		const FMantleTerrainSource Second = FMantleTerrainExport::CutTile(Source, Plan[1]);
		TestEqual(TEXT("the first tile has one of them"), First.MissingSamples, 1);
		TestEqual(TEXT("and the painted hole at the shared column (sample 2048, row 3)"), First.HoleSamples, 1);
		TestEqual(TEXT("the second has two"), Second.MissingSamples, 2);
		TestEqual(TEXT("and the same painted hole"), Second.HoleSamples, 1);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
