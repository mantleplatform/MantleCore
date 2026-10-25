// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHeightfieldReader.h"
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"
#include "MantleTerrainGrass.h"
#include "MantleTerrainSurface.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Terrain grass: the parser's rules for a Terrain's grass[], and the pure generator that decides which
// instances a cell holds. No network, no world.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleTerrainGrassTestPrivate
{
	/** One grass entry: the four keys it needs, then whatever Extra adds (each starting with a comma). */
	FString GrassEntry(const FString& Map, const FString& Channel, const FString& Mesh, const FString& Density, const FString& Extra = FString())
	{
		return FString::Printf(TEXT("{\"map\":\"%s\",\"channel\":\"%s\",\"mesh\":\"%s\",\"density\":%s%s}"), *Map, *Channel, *Mesh, *Density, *Extra);
	}

	FString GoodEntry(const FString& Extra = FString())
	{
		return GrassEntry(TEXT("map_a"), TEXT("r"), TEXT("mesh_a"), TEXT("2"), Extra);
	}

	/** A Terrain entity with one layer and the grass array Grass (the text between the brackets). */
	FString TerrainEntity(const FString& Grass)
	{
		return FString::Printf(
			TEXT("{\"id\":\"t\",\"type\":\"Terrain\",\"name\":\"t\",\"parent\":null,")
				TEXT("\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},")
				TEXT("\"properties\":{\"heightfield\":\"hf_a\",\"layers\":[{\"material\":\"mat_a\"}],\"grass\":[%s]}}"),
			*Grass);
	}

	/** A complete place around Entities (comma-separated entity objects) and the manifest array Assets. */
	FString PlaceJson(const FString& Entities, const FString& Assets = TEXT("[]"))
	{
		return FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_grass_test\",\"revision\":{\"number\":1,\"state\":\"draft\"},")
				TEXT("\"metadata\":{\"name\":\"Grass test\",\"authorId\":\"usr_test\"},\"settings\":{},\"assets\":%s,\"entities\":[%s]}"),
			*Assets, *Entities);
	}

	FString GrassPath(const int32 Entry, const TCHAR* Key = nullptr)
	{
		const FString Base = FString::Printf(TEXT("/entities/0/properties/grass/%d"), Entry);
		return Key ? Base + TEXT("/") + Key : Base;
	}

	bool HasErrorAt(const FMantleParseResult& Result, const FString& Path)
	{
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error)
		{
			return Error.Path.Equals(Path, ESearchCase::CaseSensitive);
		});
	}

	/** A heightfield of Size x Size samples, SpacingCm apart, whose height rises SlopePerSample cm for each sample along X. */
	FMantleHeightfield MakeField(const int32 Size, const float SpacingCm, const uint16 SlopePerSample)
	{
		FMantleHeightfield Field;
		Field.Width = Size;
		Field.Height = Size;
		Field.SpacingX = SpacingCm;
		Field.SpacingY = SpacingCm;
		Field.HeightScale = 1.0f;
		Field.HeightOffset = 0.0f;
		Field.Heights.SetNumUninitialized(Size * Size);
		for (int32 Y = 0; Y < Size; ++Y)
		{
			for (int32 X = 0; X < Size; ++X)
			{
				Field.Heights[Y * Size + X] = static_cast<uint16>(X * SlopePerSample);
			}
		}
		return Field;
	}

	/** A Size x Size grass map whose every texel is Color, pixels kept. */
	FMantleTerrainWeights MakeMap(const int32 Size, const FColor& Color)
	{
		TArray<FColor> Pixels;
		Pixels.Init(Color, Size * Size);
		FMantleTerrainWeights Map;
		FMantleTerrainWeights::FromPixels(Size, Size, Pixels, Map, /*bKeepPixels*/ true);
		return Map;
	}

	FMantleTerrainGrass MakeEntry(const double Density)
	{
		FMantleTerrainGrass Entry;
		Entry.Density = Density;
		Entry.Channel = 0;
		return Entry;
	}

	bool SameInstances(const FMantleGrassCellInstances& A, const FMantleGrassCellInstances& B)
	{
		if (A.Instances.Num() != B.Instances.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Instances.Num(); ++Index)
		{
			if (!A.Instances[Index].Equals(B.Instances[Index], 0.0))
			{
				return false;
			}
		}
		return true;
	}
}

// ---- Parser -------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainGrassParserAcceptsTest,
	"Mantle.Runtime.Parser.TerrainGrass.Accepts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainGrassParserAcceptsTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainGrassTestPrivate;

	// Only the four required keys: every other value is the schema's default.
	const FMantleParseResult Minimal = FMantlePlaceParser::ParseFromString(PlaceJson(TerrainEntity(GoodEntry())));
	if (!TestTrue(TEXT("a Terrain with minimal grass parses"), Minimal.bSuccess))
	{
		AddError(Minimal.ErrorSummary());
		return false;
	}
	const FMantleTerrainProps* Terrain = Minimal.Place.Entities.Num() == 1 ? Minimal.Place.Entities[0].Props.TryGet<FMantleTerrainProps>() : nullptr;
	if (!TestNotNull(TEXT("the terrain"), Terrain) || !TestEqual(TEXT("one grass entry"), Terrain->Grass.Num(), 1))
	{
		return false;
	}
	const FMantleTerrainGrass& Defaults = Terrain->Grass[0];
	TestEqual(TEXT("map"), Defaults.Map, FString(TEXT("map_a")));
	TestEqual(TEXT("channel r is 0"), Defaults.Channel, 0);
	TestEqual(TEXT("mesh"), Defaults.Mesh, FString(TEXT("mesh_a")));
	TestEqual(TEXT("density"), Defaults.Density, 2.0);
	TestEqual(TEXT("no material overrides"), Defaults.Materials.Num(), 0);
	TestEqual(TEXT("default scale min"), Defaults.MinScale, 1.0);
	TestEqual(TEXT("default scale max"), Defaults.MaxScale, 1.0);
	TestTrue(TEXT("aligns to the surface by default"), Defaults.bAlignToSurface);
	TestTrue(TEXT("random yaw by default"), Defaults.bRandomYaw);
	TestEqual(TEXT("default cull start"), Defaults.CullStart, 0.0);
	TestEqual(TEXT("default cull end"), Defaults.CullEnd, 10000.0);
	TestFalse(TEXT("casts no shadow by default"), Defaults.bCastShadow);

	// Every key, and every channel.
	const FString Full = GrassEntry(TEXT("map_b"), TEXT("a"), TEXT("mesh_b"), TEXT("100"),
		TEXT(",\"materials\":[\"mat_a\",null],\"scale\":[0.5,2.5],\"alignToSurface\":false,\"randomYaw\":false,")
		TEXT("\"cullDistance\":[1000,50000],\"castShadow\":true"));
	const FMantleParseResult Complete = FMantlePlaceParser::ParseFromString(PlaceJson(TerrainEntity(Full)));
	if (!TestTrue(TEXT("a grass entry with every key parses"), Complete.bSuccess))
	{
		AddError(Complete.ErrorSummary());
		return false;
	}
	const FMantleTerrainGrass& Grass = Complete.Place.Entities[0].Props.Get<FMantleTerrainProps>().Grass[0];
	TestEqual(TEXT("channel a is 3"), Grass.Channel, 3);
	TestEqual(TEXT("density at the limit"), Grass.Density, FMantlePlaceLimits::MaxTerrainGrassDensity);
	if (TestEqual(TEXT("two material slots"), Grass.Materials.Num(), 2))
	{
		TestEqual(TEXT("slot 0"), Grass.Materials[0], FString(TEXT("mat_a")));
		TestTrue(TEXT("slot 1 is unset"), Grass.Materials[1].IsEmpty());
	}
	TestEqual(TEXT("scale min"), Grass.MinScale, 0.5);
	TestEqual(TEXT("scale max"), Grass.MaxScale, 2.5);
	TestFalse(TEXT("align"), Grass.bAlignToSurface);
	TestFalse(TEXT("yaw"), Grass.bRandomYaw);
	TestEqual(TEXT("cull start"), Grass.CullStart, 1000.0);
	TestEqual(TEXT("cull end at the limit"), Grass.CullEnd, FMantlePlaceLimits::MaxTerrainGrassCullDistance);
	TestTrue(TEXT("shadow"), Grass.bCastShadow);

	for (const TCHAR* Channel : { TEXT("g"), TEXT("b") })
	{
		TestTrue(FString::Printf(TEXT("channel %s parses"), Channel),
			FMantlePlaceParser::ParseFromString(PlaceJson(TerrainEntity(GrassEntry(TEXT("m"), Channel, TEXT("x"), TEXT("1"))))).bSuccess);
	}

	// No grass key is the Terrain it always was; the limits are what the schema says.
	const FString Bare = TEXT("{\"id\":\"t\",\"type\":\"Terrain\",\"name\":\"t\",\"parent\":null,")
		TEXT("\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},")
		TEXT("\"properties\":{\"heightfield\":\"hf_a\",\"layers\":[{\"material\":\"mat_a\"}]}}");
	const FMantleParseResult Plain = FMantlePlaceParser::ParseFromString(PlaceJson(Bare));
	TestTrue(TEXT("a Terrain without grass parses"), Plain.bSuccess);
	TestEqual(TEXT("and has none"), Plain.Place.Entities.Num() == 1 ? Plain.Place.Entities[0].Props.Get<FMantleTerrainProps>().Grass.Num() : -1, 0);
	TestEqual(TEXT("max grass entries"), FMantlePlaceLimits::MaxTerrainGrass, 32);
	TestEqual(TEXT("max grass maps"), FMantlePlaceLimits::MaxTerrainGrassMaps, 4);

	// Exactly the permitted number of entries, and of distinct maps, parses.
	TArray<FString> ThirtyTwo;
	for (int32 Index = 0; Index < FMantlePlaceLimits::MaxTerrainGrass; ++Index)
	{
		ThirtyTwo.Add(GrassEntry(FString::Printf(TEXT("map_%d"), Index % FMantlePlaceLimits::MaxTerrainGrassMaps), TEXT("r"), TEXT("m"), TEXT("1")));
	}
	const FMantleParseResult AtLimit = FMantlePlaceParser::ParseFromString(PlaceJson(TerrainEntity(FString::Join(ThirtyTwo, TEXT(",")))));
	TestTrue(TEXT("32 entries over 4 maps parse"), AtLimit.bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainGrassParserRefusesTest,
	"Mantle.Runtime.Parser.TerrainGrass.Refuses",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainGrassParserRefusesTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainGrassTestPrivate;

	// The base place parses, so a refusal below is for the case alone.
	const FMantleParseResult Base = FMantlePlaceParser::ParseFromString(PlaceJson(TerrainEntity(GoodEntry())));
	if (!TestTrue(TEXT("the base place parses before any case mutates it"), Base.bSuccess))
	{
		AddError(Base.ErrorSummary());
		return false;
	}

	// Refused at Path, and nowhere that is not grass.
	const auto ExpectRefused = [this](const TCHAR* Case, const FString& Json, const FString& Path)
	{
		const FMantleParseResult Result = FMantlePlaceParser::ParseFromString(Json);
		TestFalse(FString::Printf(TEXT("%s is refused"), Case), Result.bSuccess);
		TestTrue(FString::Printf(TEXT("%s is reported at %s (got: %s)"), Case, *Path, *Result.ErrorSummary()), HasErrorAt(Result, Path));
		for (const FMantleParseError& Error : Result.Errors)
		{
			TestTrue(FString::Printf(TEXT("%s: error '%s' concerns the grass"), Case, *Error.ToString()),
				Error.Path.StartsWith(TEXT("/entities/0/properties/grass")));
		}
	};
	const auto Refused = [&ExpectRefused](const TCHAR* Case, const FString& Entry, const FString& Path)
	{
		ExpectRefused(Case, PlaceJson(TerrainEntity(Entry)), Path);
	};

	Refused(TEXT("an unknown key"), GoodEntry(TEXT(",\"bogus\":1")), GrassPath(0, TEXT("bogus")));
	Refused(TEXT("no map"), TEXT("{\"channel\":\"r\",\"mesh\":\"m\",\"density\":1}"), GrassPath(0, TEXT("map")));
	Refused(TEXT("an empty map"), GrassEntry(TEXT(""), TEXT("r"), TEXT("m"), TEXT("1")), GrassPath(0, TEXT("map")));
	Refused(TEXT("no channel"), TEXT("{\"map\":\"a\",\"mesh\":\"m\",\"density\":1}"), GrassPath(0, TEXT("channel")));
	Refused(TEXT("a capital channel"), GrassEntry(TEXT("a"), TEXT("R"), TEXT("m"), TEXT("1")), GrassPath(0, TEXT("channel")));
	Refused(TEXT("an unknown channel"), GrassEntry(TEXT("a"), TEXT("x"), TEXT("m"), TEXT("1")), GrassPath(0, TEXT("channel")));
	Refused(TEXT("no mesh"), TEXT("{\"map\":\"a\",\"channel\":\"r\",\"density\":1}"), GrassPath(0, TEXT("mesh")));
	Refused(TEXT("an empty mesh"), GrassEntry(TEXT("a"), TEXT("r"), TEXT(""), TEXT("1")), GrassPath(0, TEXT("mesh")));
	Refused(TEXT("no density"), TEXT("{\"map\":\"a\",\"channel\":\"r\",\"mesh\":\"m\"}"), GrassPath(0, TEXT("density")));
	Refused(TEXT("zero density"), GrassEntry(TEXT("a"), TEXT("r"), TEXT("m"), TEXT("0")), GrassPath(0, TEXT("density")));
	Refused(TEXT("negative density"), GrassEntry(TEXT("a"), TEXT("r"), TEXT("m"), TEXT("-1")), GrassPath(0, TEXT("density")));
	Refused(TEXT("density past the limit"), GrassEntry(TEXT("a"), TEXT("r"), TEXT("m"), TEXT("100.5")), GrassPath(0, TEXT("density")));
	Refused(TEXT("scale out of order"), GoodEntry(TEXT(",\"scale\":[2,1]")), GrassPath(0, TEXT("scale")));
	Refused(TEXT("scale below the minimum"), GoodEntry(TEXT(",\"scale\":[0,1]")), GrassPath(0, TEXT("scale/0")));
	Refused(TEXT("scale above the maximum"), GoodEntry(TEXT(",\"scale\":[1,65.536]")), GrassPath(0, TEXT("scale/1")));
	Refused(TEXT("one scale number"), GoodEntry(TEXT(",\"scale\":[1]")), GrassPath(0, TEXT("scale")));
	Refused(TEXT("cull distances out of order"), GoodEntry(TEXT(",\"cullDistance\":[500,100]")), GrassPath(0, TEXT("cullDistance")));
	Refused(TEXT("cull distances equal"), GoodEntry(TEXT(",\"cullDistance\":[100,100]")), GrassPath(0, TEXT("cullDistance")));
	Refused(TEXT("a cull end of 0"), GoodEntry(TEXT(",\"cullDistance\":[0,0]")), GrassPath(0, TEXT("cullDistance")));
	Refused(TEXT("a cull end past the limit"), GoodEntry(TEXT(",\"cullDistance\":[0,50001]")), GrassPath(0, TEXT("cullDistance/1")));
	Refused(TEXT("a negative cull start"), GoodEntry(TEXT(",\"cullDistance\":[-1,100]")), GrassPath(0, TEXT("cullDistance/0")));
	Refused(TEXT("a material that is not an id or null"), GoodEntry(TEXT(",\"materials\":[3]")), GrassPath(0, TEXT("materials/0")));
	{
		TArray<FString> Slots;
		for (int32 Slot = 0; Slot <= FMantlePlaceLimits::MaxMaterialSlots; ++Slot)
		{
			Slots.Add(TEXT("null"));
		}
		Refused(TEXT("too many material slots"), GoodEntry(TEXT(",\"materials\":[") + FString::Join(Slots, TEXT(",")) + TEXT("]")), GrassPath(0, TEXT("materials")));
	}

	// An error in a later entry is reported at that entry.
	Refused(TEXT("a bad second entry"), GoodEntry() + TEXT(",") + GrassEntry(TEXT("a"), TEXT("q"), TEXT("m"), TEXT("1")), GrassPath(1, TEXT("channel")));

	// More than 32 entries, at the array.
	{
		TArray<FString> TooMany;
		for (int32 Index = 0; Index <= FMantlePlaceLimits::MaxTerrainGrass; ++Index)
		{
			TooMany.Add(GoodEntry());
		}
		Refused(TEXT("33 entries"), FString::Join(TooMany, TEXT(",")), TEXT("/entities/0/properties/grass"));
	}

	// More than four distinct maps, reported at the first entry that makes a fifth. Ids are case-sensitive, so
	// 'a' and 'A' are two maps; repeating a map costs nothing.
	{
		const FString Entries = GrassEntry(TEXT("a"), TEXT("r"), TEXT("m"), TEXT("1")) + TEXT(",") + GrassEntry(TEXT("A"), TEXT("r"), TEXT("m"), TEXT("1"))
			+ TEXT(",") + GrassEntry(TEXT("b"), TEXT("g"), TEXT("m"), TEXT("1")) + TEXT(",") + GrassEntry(TEXT("a"), TEXT("g"), TEXT("m"), TEXT("1"))
			+ TEXT(",") + GrassEntry(TEXT("B"), TEXT("r"), TEXT("m"), TEXT("1")) + TEXT(",") + GrassEntry(TEXT("c"), TEXT("r"), TEXT("m"), TEXT("1"));
		Refused(TEXT("five distinct maps"), Entries, GrassPath(5, TEXT("map")));
	}

	// A grass map the manifest lists as something else than a texture; a texture, or an id it does not list, is fine.
	const FString MeshListed = TEXT("[{\"id\":\"map_a\",\"kind\":\"StaticMesh\",\"hash\":\"h\",\"bytes\":1}]");
	ExpectRefused(TEXT("a map listed as a mesh"), PlaceJson(TerrainEntity(GoodEntry()), MeshListed), GrassPath(0, TEXT("map")));
	const FString TextureListed = TEXT("[{\"id\":\"map_a\",\"kind\":\"Texture\",\"hash\":\"h\",\"bytes\":1}]");
	TestTrue(TEXT("a map listed as a texture parses"), FMantlePlaceParser::ParseFromString(PlaceJson(TerrainEntity(GoodEntry()), TextureListed)).bSuccess);
	return true;
}

// ---- Generator ----------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainGrassDeterministicTest,
	"Mantle.Runtime.TerrainGrass.Deterministic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainGrassDeterministicTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainGrassTestPrivate;

	// 129 samples 100 cm apart: a 128 m terrain, four 32 m cells across.
	const FMantleHeightfield Field = MakeField(129, 100.0f, 7);
	const FMantleTerrainWeights Map = MakeMap(4, FColor(255, 255, 255, 255));
	const FMantleTerrainGrass Entry = MakeEntry(0.5);

	FMantleGrassCellInstances First, Second, OtherCell, OtherEntry;
	FMantleTerrainGrassGenerator::GenerateCell(Entry, 2, FIntPoint(1, 2), Field, Map, 1.0, First);
	FMantleTerrainGrassGenerator::GenerateCell(Entry, 2, FIntPoint(1, 2), Field, Map, 1.0, Second);
	TestTrue(TEXT("the cell grows something"), First.Instances.Num() > 0);
	TestTrue(TEXT("the same cell gives the same instances"), SameInstances(First, Second));

	// Another cell, or another entry's cell, is a different draw (the counts agree: only the draws differ).
	FMantleTerrainGrassGenerator::GenerateCell(Entry, 2, FIntPoint(2, 2), Field, Map, 1.0, OtherCell);
	FMantleTerrainGrassGenerator::GenerateCell(Entry, 3, FIntPoint(1, 2), Field, Map, 1.0, OtherEntry);
	TestFalse(TEXT("a neighbouring cell differs"), SameInstances(First, OtherCell));
	TestFalse(TEXT("the same cell of another entry differs"), SameInstances(First, OtherEntry));
	TestTrue(TEXT("cell seeds differ by cell"),
		FMantleTerrainGrassGenerator::CellSeed(2, FIntPoint(1, 2)) != FMantleTerrainGrassGenerator::CellSeed(2, FIntPoint(2, 1)));
	TestTrue(TEXT("cell seeds differ by entry"),
		FMantleTerrainGrassGenerator::CellSeed(2, FIntPoint(1, 2)) != FMantleTerrainGrassGenerator::CellSeed(3, FIntPoint(1, 2)));

	// Every instance is inside its own cell, and carries what the cell is called.
	TestTrue(TEXT("the cell is recorded"), First.Cell == FIntPoint(1, 2));
	TestEqual(TEXT("the entry is recorded"), First.Entry, 2);
	TestEqual(TEXT("the origin is the cell's corner"), First.Origin, FVector(3200.0, 6400.0, 0.0));
	bool bInside = true;
	for (const FTransform& Instance : First.Instances)
	{
		const FVector Location = Instance.GetLocation();
		bInside = bInside && Location.X >= 0.0 && Location.X <= 3200.0 && Location.Y >= 0.0 && Location.Y <= 3200.0;
	}
	TestTrue(TEXT("every instance is inside its cell"), bInside);

	// A density scale below 1 keeps a prefix of the same candidates, so it never grows more.
	FMantleGrassCellInstances Half;
	FMantleTerrainGrassGenerator::GenerateCell(Entry, 2, FIntPoint(1, 2), Field, Map, 0.5, Half);
	TestTrue(TEXT("half the density scale grows fewer"), Half.Instances.Num() < First.Instances.Num());
	bool bPrefix = Half.Instances.Num() <= First.Instances.Num();
	for (int32 Index = 0; bPrefix && Index < Half.Instances.Num(); ++Index)
	{
		bPrefix = First.Instances.ContainsByPredicate([&Half, Index](const FTransform& Candidate) { return Candidate.Equals(Half.Instances[Index], 0.0); });
	}
	TestTrue(TEXT("and every one of them is one of the full density's"), bPrefix);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainGrassWeightTest,
	"Mantle.Runtime.TerrainGrass.WeightScalesCount",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainGrassWeightTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainGrassTestPrivate;

	const FMantleHeightfield Field = MakeField(129, 100.0f, 0);
	const FMantleTerrainGrass Entry = MakeEntry(2.0);
	FMantleGrassCellInstances Cell;

	// Weight 1 keeps every candidate: density x area, 2 per square metre over a 32 m cell.
	FMantleTerrainGrassGenerator::GenerateCell(Entry, 0, FIntPoint(1, 1), Field, MakeMap(2, FColor(255, 255, 255, 255)), 1.0, Cell);
	TestEqual(TEXT("a cell asks for density x area candidates"), Cell.Candidates, 2048);
	TestEqual(TEXT("and at weight 1 keeps them all"), Cell.Instances.Num(), 2048);

	// Weight 0 grows nothing, on every channel.
	for (int32 Channel = 0; Channel < 4; ++Channel)
	{
		FMantleTerrainGrass OnChannel = Entry;
		OnChannel.Channel = Channel;
		FMantleTerrainGrassGenerator::GenerateCell(OnChannel, 0, FIntPoint(1, 1), Field, MakeMap(2, FColor(0, 0, 0, 0)), 1.0, Cell);
		TestEqual(FString::Printf(TEXT("weight 0 grows nothing on channel %d"), Channel), Cell.Instances.Num(), 0);
	}

	// Only the entry's own channel counts: red full and green empty.
	FMantleTerrainGrass Green = Entry;
	Green.Channel = 1;
	FMantleTerrainGrassGenerator::GenerateCell(Green, 0, FIntPoint(1, 1), Field, MakeMap(2, FColor(255, 0, 255, 255)), 1.0, Cell);
	TestEqual(TEXT("the other channels do not grow it"), Cell.Instances.Num(), 0);

	// Half weight keeps about half.
	FMantleTerrainGrassGenerator::GenerateCell(Entry, 0, FIntPoint(1, 1), Field, MakeMap(2, FColor(128, 0, 0, 0)), 1.0, Cell);
	const double Expected = 2048.0 * 128.0 / 255.0;
	TestTrue(FString::Printf(TEXT("weight 128/255 keeps about %.0f of 2048 (got %d)"), Expected, Cell.Instances.Num()),
		FMath::Abs(Cell.Instances.Num() - Expected) < Expected * 0.1);

	// A map weighted by position: left half full, right half empty (a 4 x 1 strip of texels stretched over the terrain).
	TArray<FColor> Strip = { FColor(255, 0, 0, 0), FColor(255, 0, 0, 0), FColor(0, 0, 0, 0), FColor(0, 0, 0, 0) };
	FMantleTerrainWeights Halves;
	FMantleTerrainWeights::FromPixels(4, 1, Strip, Halves, true);
	FMantleTerrainGrassGenerator::GenerateCell(Entry, 0, FIntPoint(0, 0), Field, Halves, 1.0, Cell);
	TestTrue(TEXT("the left cell grows"), Cell.Instances.Num() > 1500);
	FMantleTerrainGrassGenerator::GenerateCell(Entry, 0, FIntPoint(3, 0), Field, Halves, 1.0, Cell);
	TestEqual(TEXT("the right cell grows nothing"), Cell.Instances.Num(), 0);

	// A cell wholly past the terrain's edge grows nothing, and a density scale of 0 grows nothing.
	FMantleTerrainGrassGenerator::GenerateCell(Entry, 0, FIntPoint(9, 9), Field, MakeMap(2, FColor(255, 255, 255, 255)), 1.0, Cell);
	TestEqual(TEXT("past the terrain's edge"), Cell.Instances.Num(), 0);
	FMantleTerrainGrassGenerator::GenerateCell(Entry, 0, FIntPoint(1, 1), Field, MakeMap(2, FColor(255, 255, 255, 255)), 0.0, Cell);
	TestEqual(TEXT("density scale 0"), Cell.Instances.Num(), 0);

	// A map without its pixels grows nothing.
	FMantleTerrainWeights NoPixels;
	FMantleTerrainWeights::FromPixels(2, 2, TArray<FColor>({ FColor::White, FColor::White, FColor::White, FColor::White }), NoPixels, false);
	FMantleTerrainGrassGenerator::GenerateCell(Entry, 0, FIntPoint(1, 1), Field, NoPixels, 1.0, Cell);
	TestEqual(TEXT("a map that kept no pixels"), Cell.Instances.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainGrassPlacementTest,
	"Mantle.Runtime.TerrainGrass.Placement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainGrassPlacementTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainGrassTestPrivate;

	// A ramp rising 0.2 cm for each cm along X: 20 cm per 100 cm sample.
	const FMantleHeightfield Ramp = MakeField(129, 100.0f, 20);
	const FMantleTerrainWeights Map = MakeMap(2, FColor(255, 255, 255, 255));

	// The height is the terrain's, and aligned instances stand on its normal.
	FMantleTerrainGrass Aligned = MakeEntry(1.0);
	Aligned.bAlignToSurface = true;
	Aligned.bRandomYaw = true;
	FMantleGrassCellInstances Cell;
	FMantleTerrainGrassGenerator::GenerateCell(Aligned, 0, FIntPoint(1, 1), Ramp, Map, 1.0, Cell);
	if (!TestTrue(TEXT("the cell grows"), Cell.Instances.Num() > 100))
	{
		return false;
	}
	const FVector Normal = FVector(-0.2, 0.0, 1.0).GetSafeNormal();
	double WorstHeight = 0.0;
	double WorstTilt = 1.0;
	for (const FTransform& Instance : Cell.Instances)
	{
		const double WorldX = Cell.Origin.X + Instance.GetLocation().X;
		WorstHeight = FMath::Max(WorstHeight, FMath::Abs(Instance.GetLocation().Z - WorldX * 0.2));
		WorstTilt = FMath::Min(WorstTilt, FVector::DotProduct(Instance.GetRotation().GetUpVector(), Normal));
	}
	TestTrue(FString::Printf(TEXT("every instance stands on the surface (off by at most %.3f cm)"), WorstHeight), WorstHeight < 0.5);
	TestTrue(FString::Printf(TEXT("and on its normal (worst dot %.4f)"), WorstTilt), WorstTilt > 0.999);

	// Unaligned and without yaw: no rotation at all, and scale within the range.
	FMantleTerrainGrass Upright = MakeEntry(1.0);
	Upright.bAlignToSurface = false;
	Upright.bRandomYaw = false;
	Upright.MinScale = 0.5;
	Upright.MaxScale = 2.0;
	FMantleTerrainGrassGenerator::GenerateCell(Upright, 0, FIntPoint(1, 1), Ramp, Map, 1.0, Cell);
	bool bIdentity = Cell.Instances.Num() > 0;
	bool bScaled = true;
	bool bVaried = false;
	for (const FTransform& Instance : Cell.Instances)
	{
		bIdentity = bIdentity && Instance.GetRotation().Equals(FQuat::Identity, 1.0e-6);
		const FVector Scale = Instance.GetScale3D();
		bScaled = bScaled && Scale.X >= 0.5 - 1.0e-9 && Scale.X <= 2.0 + 1.0e-9 && Scale.Equals(FVector(Scale.X), 1.0e-9);
		bVaried = bVaried || !FMath::IsNearlyEqual(Scale.X, Cell.Instances[0].GetScale3D().X, 0.01);
	}
	TestTrue(TEXT("unaligned, yawless instances are not turned"), bIdentity);
	TestTrue(TEXT("scale is uniform and within the range"), bScaled);
	TestTrue(TEXT("and varies"), bVaried);

	// Random yaw alone turns about up only.
	FMantleTerrainGrass Yawed = Upright;
	Yawed.bRandomYaw = true;
	FMantleTerrainGrassGenerator::GenerateCell(Yawed, 0, FIntPoint(1, 1), Ramp, Map, 1.0, Cell);
	bool bUp = Cell.Instances.Num() > 0;
	bool bTurned = false;
	for (const FTransform& Instance : Cell.Instances)
	{
		bUp = bUp && Instance.GetRotation().GetUpVector().Equals(FVector::UpVector, 1.0e-6);
		bTurned = bTurned || !Instance.GetRotation().Equals(FQuat::Identity, 0.01);
	}
	TestTrue(TEXT("a random yaw stays upright"), bUp);
	TestTrue(TEXT("and turns"), bTurned);

	// The surface on the triangles the meshes draw: a corner is its sample, and a point on the ramp is on the ramp.
	float Z = 0.0f;
	FVector3f SurfaceNormal = FVector3f::UpVector;
	TestTrue(TEXT("a surface"), FMantleTerrainGrassGenerator::SurfaceAt(Ramp, 300.0, 700.0, Z, SurfaceNormal));
	TestTrue(TEXT("a sample's height"), FMath::IsNearlyEqual(Z, 60.0f, 0.01f));
	FMantleTerrainGrassGenerator::SurfaceAt(Ramp, 350.0, 720.0, Z, SurfaceNormal);
	TestTrue(TEXT("a point between samples on the ramp"), FMath::IsNearlyEqual(Z, 70.0f, 0.01f));
	TestTrue(TEXT("the ramp's normal"), SurfaceNormal.Equals(FVector3f(-0.2f, 0.0f, 1.0f).GetSafeNormal(), 0.001f));
	FMantleTerrainGrassGenerator::SurfaceAt(Ramp, -50.0, 1.0e9, Z, SurfaceNormal);
	TestTrue(TEXT("clamped to the terrain"), FMath::IsNearlyEqual(Z, 0.0f, 0.01f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainGrassCellsTest,
	"Mantle.Runtime.TerrainGrass.Cells",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainGrassCellsTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainGrassTestPrivate;

	// 32 m cells, halved while one would ask for more than 16384 candidates, never below 4 m.
	TestEqual(TEXT("a sparse entry's cell"), FMantleTerrainGrassGenerator::CellSizeCm(MakeEntry(0.1)), 3200.0);
	TestEqual(TEXT("10 a square metre keeps 32 m"), FMantleTerrainGrassGenerator::CellSizeCm(MakeEntry(10.0)), 3200.0);
	TestEqual(TEXT("16 a square metre keeps 32 m"), FMantleTerrainGrassGenerator::CellSizeCm(MakeEntry(16.0)), 3200.0);
	TestEqual(TEXT("17 a square metre halves it"), FMantleTerrainGrassGenerator::CellSizeCm(MakeEntry(17.0)), 1600.0);
	TestEqual(TEXT("the densest entry's cell"), FMantleTerrainGrassGenerator::CellSizeCm(MakeEntry(FMantlePlaceLimits::MaxTerrainGrassDensity)), 800.0);
	for (const double Density : { 0.001, 1.0, 10.0, 17.0, 50.0, FMantlePlaceLimits::MaxTerrainGrassDensity })
	{
		TestTrue(FString::Printf(TEXT("density %g stays within the candidate cap"), Density),
			FMantleTerrainGrassGenerator::CandidateCount(MakeEntry(Density), 1.0) <= FMantleTerrainGrassGenerator::MaxCandidatesPerCell);
	}
	TestEqual(TEXT("a 32 m cell at 2 a square metre"), FMantleTerrainGrassGenerator::CandidateCount(MakeEntry(2.0), 1.0), 2048);
	TestEqual(TEXT("at half the density scale"), FMantleTerrainGrassGenerator::CandidateCount(MakeEntry(2.0), 0.5), 1024);
	TestEqual(TEXT("at density scale 0"), FMantleTerrainGrassGenerator::CandidateCount(MakeEntry(2.0), 0.0), 0);

	const FMantleHeightfield Field = MakeField(129, 100.0f, 0);
	TestTrue(TEXT("a 128 m terrain is four 32 m cells across"), FMantleTerrainGrassGenerator::CellCount(Field, 3200.0) == FIntPoint(4, 4));
	TestTrue(TEXT("and sixteen 8 m cells"), FMantleTerrainGrassGenerator::CellCount(Field, 800.0) == FIntPoint(16, 16));
	TestTrue(TEXT("a partial cell counts"), FMantleTerrainGrassGenerator::CellCount(Field, 3000.0) == FIntPoint(5, 5));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainGrassMapSamplingTest,
	"Mantle.Runtime.TerrainGrass.MapSampling",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainGrassMapSamplingTest::RunTest(const FString& Parameters)
{
	// Two texels across: red 0 on the left, 255 on the right. Texel centres are at 0.25 and 0.75 of the terrain.
	TArray<FColor> Pixels = { FColor(0, 10, 0, 0), FColor(255, 20, 0, 200) };
	FMantleTerrainWeights Map;
	if (!TestTrue(TEXT("the map builds"), FMantleTerrainWeights::FromPixels(2, 1, Pixels, Map, true)))
	{
		return false;
	}
	TestTrue(TEXT("and keeps its pixels"), Map.HasPixels());
	TestEqual(TEXT("left of the first centre clamps"), Map.SampleChannel(0, 0.0, 0.5), 0.0);
	TestEqual(TEXT("at the first centre"), Map.SampleChannel(0, 0.25, 0.5), 0.0);
	TestTrue(TEXT("halfway is half"), FMath::IsNearlyEqual(Map.SampleChannel(0, 0.5, 0.5), 0.5, 1.0e-9));
	TestEqual(TEXT("at the last centre"), Map.SampleChannel(0, 0.75, 0.5), 1.0);
	TestEqual(TEXT("past the last centre clamps"), Map.SampleChannel(0, 1.0, 0.5), 1.0);
	TestTrue(TEXT("green is its own channel"), FMath::IsNearlyEqual(Map.SampleChannel(1, 0.75, 0.5), 20.0 / 255.0, 1.0e-9));
	TestTrue(TEXT("alpha is the fourth"), FMath::IsNearlyEqual(Map.SampleChannel(3, 0.5, 0.5), 100.0 / 255.0, 1.0e-9));
	TestEqual(TEXT("a channel that does not exist"), Map.SampleChannel(4, 0.5, 0.5), 0.0);

	// A map that kept no pixels samples as nothing, and still names its dominant layer as before.
	FMantleTerrainWeights Without;
	FMantleTerrainWeights::FromPixels(2, 1, Pixels, Without);
	TestFalse(TEXT("pixels are kept only when asked for"), Without.HasPixels());
	TestEqual(TEXT("and sample as 0"), Without.SampleChannel(0, 0.75, 0.5), 0.0);
	TestEqual(TEXT("while the dominant layer is read as before"), Without.DominantLayerAt(0.9, 0.5), 0);
	TestEqual(TEXT("(green weighs most on the left)"), Without.DominantLayerAt(0.1, 0.5), 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
