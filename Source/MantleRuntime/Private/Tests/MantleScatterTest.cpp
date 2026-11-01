// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"
#include "MantleScatter.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Scatter (#530, #169): the generator's counts, determinism and filters, and what the parser reads and refuses. The golden
// numbers come from an independent model of the generator (a script outside the engine), so a change to the maths shows here.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleScatterTestPrivate
{
	FMantleScatterParams BoxParams()
	{
		FMantleScatterParams P;
		P.Seed = 530;
		P.HalfExtent = FVector(500.0, 300.0, 0.0);
		P.Density = 0.5;
		P.MinScale = 0.8;
		P.MaxScale = 1.2;
		P.Meshes = { { TEXT("ast_cone"), 4, {} }, { TEXT("ast_sphere"), 2, {} }, { TEXT("ast_cube"), 1, {} } };
		return P;
	}

	TArray<FMantleScatterInstance> GenerateOk(FAutomationTestBase& Test, const FMantleScatterParams& P,
		const FMantleScatterGenerateOptions& Options = FMantleScatterGenerateOptions())
	{
		TArray<FMantleScatterInstance> Out;
		FString Error;
		Test.TestTrue(FString::Printf(TEXT("generates (%s)"), *Error), FMantleScatter::Generate(P, Out, Error, Options));
		return Out;
	}

	/** A place of one Scatter whose properties are Properties (the text between the braces), drawing from ast_cube. */
	FMantleParseResult ParseScatter(const FString& Properties)
	{
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_scatter001\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Field\",\"authorId\":\"usr_4b1d\"},\"settings\":{},"
				 "\"assets\":[{\"id\":\"ast_cube\",\"kind\":\"StaticMesh\",\"hash\":\"md5:0123456789abcdef\",\"bytes\":1024}],"
				 "\"entities\":[{\"id\":\"ent_scatter\",\"type\":\"Scatter\",\"parent\":null,"
				 "\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{"
				 "\"halfExtent\":[1000,1000,0],\"density\":0.1,\"meshes\":[{\"mesh\":\"ast_cube\"}]%s}}]}"),
			*Properties);
		return FMantlePlaceParser::ParseFromString(Json);
	}

	bool HasErrorAt(const FMantleParseResult& Result, const FString& Property)
	{
		const FString Path = TEXT("/entities/0/properties/") + Property;
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error) { return Error.Path == Path; });
	}

	const FMantleScatterParams* ScatterOf(const FMantleParseResult& Result)
	{
		return Result.Place.Entities.Num() == 1 ? Result.Place.Entities[0].Props.TryGet<FMantleScatterParams>() : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterBoxGoldenTest,
	"Mantle.Runtime.Scatter.Generator.BoxIsUnchanged",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterBoxGoldenTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterTestPrivate;

	// What the generator gave before shapes, clusters and terrain existed: a box scatter must stay bit for bit the same.
	FMantleScatterParams P = BoxParams();
	TArray<FMantleScatterInstance> Instances = GenerateOk(*this, P);
	TestEqual(TEXT("box count"), Instances.Num(), 30);
	TestEqual(TEXT("box hash"), FMantleScatter::HashText(FMantleScatter::Hash(Instances)), FString(TEXT("42261fcbeefd8dd6")));

	FMantleScatterParams Tilted;
	Tilted.Seed = 7;
	Tilted.HalfExtent = FVector(400.0, 400.0, 50.0);
	Tilted.Density = 1.0;
	Tilted.MinScale = 0.5;
	Tilted.MaxScale = 2.0;
	Tilted.Alignment = EMantleScatterAlignment::Tilted;
	Tilted.MaxTiltDegrees = 15.0;
	Tilted.Meshes = { { TEXT("ast_cone"), 1, {} }, { TEXT("ast_sphere"), 1, {} } };
	Instances = GenerateOk(*this, Tilted);
	TestEqual(TEXT("tilted count"), Instances.Num(), 64);
	TestEqual(TEXT("tilted hash"), FMantleScatter::HashText(FMantleScatter::Hash(Instances)), FString(TEXT("180527a65a08b414")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterCountsTest,
	"Mantle.Runtime.Scatter.Generator.CountsFollowTheFormulas",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterCountsTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterTestPrivate;

	FString Error;
	FMantleScatterParams Disc = BoxParams();
	Disc.Shape = EMantleScatterShape::Disc;
	Disc.Radius = 1000.0;
	Disc.Density = 0.1;
	TestEqual(TEXT("disc r 10 m, density 0.1"), FMantleScatter::CountFor(Disc, Error), static_cast<int64>(31));
	Disc.Radius = 1234.5;
	Disc.Density = 0.37;
	TestEqual(TEXT("disc r 12.345 m, density 0.37"), FMantleScatter::CountFor(Disc, Error), static_cast<int64>(177));

	FMantleScatterParams Path = BoxParams();
	Path.Shape = EMantleScatterShape::Path;
	Path.Path = { FVector2D(0.0, 0.0), FVector2D(1000.0, 0.0), FVector2D(1000.0, 500.0) };
	Path.Width = 200.0;
	Path.Density = 0.2;
	TestEqual(TEXT("path 15 m by 2 m, density 0.2"), FMantleScatter::CountFor(Path, Error), static_cast<int64>(6));
	Path.Path = { FVector2D(0.0, 0.0), FVector2D(300.0, 400.0) };
	Path.Width = 100.5;
	Path.Density = 1.0;
	TestEqual(TEXT("path 5 m by 1.005 m, density 1"), FMantleScatter::CountFor(Path, Error), static_cast<int64>(5));

	Path.Path = { FVector2D(0.0, 0.0) };
	TestEqual(TEXT("a path of one point is refused"), FMantleScatter::CountFor(Path, Error), static_cast<int64>(-1));
	Disc.Radius = 0.0;
	TestEqual(TEXT("a disc without a radius is refused"), FMantleScatter::CountFor(Disc, Error), static_cast<int64>(-1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterShapesTest,
	"Mantle.Runtime.Scatter.Generator.ShapesExclusionsAndClusters",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterShapesTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterTestPrivate;

	// A disc: every instance inside its radius.
	FMantleScatterParams Disc = BoxParams();
	Disc.Shape = EMantleScatterShape::Disc;
	Disc.Radius = 1000.0;
	Disc.Density = 5.0;
	TArray<FMantleScatterInstance> Instances = GenerateOk(*this, Disc);
	TestEqual(TEXT("disc count"), Instances.Num(), 1571);
	bool bInside = true;
	for (const FMantleScatterInstance& Instance : Instances)
	{
		const int64 X = Instance.X;
		const int64 Y = Instance.Y;
		bInside &= X * X + Y * Y <= static_cast<int64>(10000) * 10000;
	}
	TestTrue(TEXT("disc: every instance is inside the radius"), bInside);
	TestEqual(TEXT("disc: the same parameters give the same hash"), FMantleScatter::Hash(Instances), FMantleScatter::Hash(GenerateOk(*this, Disc)));

	// A path: every instance within half the width of its line (a straight east-west strip here).
	FMantleScatterParams Path = BoxParams();
	Path.Shape = EMantleScatterShape::Path;
	Path.Path = { FVector2D(-1000.0, 0.0), FVector2D(1000.0, 0.0) };
	Path.Width = 400.0;
	Path.Density = 20.0;
	Instances = GenerateOk(*this, Path);
	TestEqual(TEXT("path count"), Instances.Num(), 1600);
	bool bInStrip = true;
	for (const FMantleScatterInstance& Instance : Instances)
	{
		bInStrip &= Instance.X >= -10000 && Instance.X <= 10000 && FMath::Abs(Instance.Y) <= 2000;
	}
	TestTrue(TEXT("path: every instance is on the strip"), bInStrip);

	// An exclusion drops what is inside it and keeps the rest exactly as it was.
	FMantleScatterParams Box = BoxParams();
	Box.HalfExtent = FVector(1000.0, 1000.0, 0.0);
	Box.Density = 1.0;
	const TArray<FMantleScatterInstance> Whole = GenerateOk(*this, Box);
	FMantleScatterExclusion Exclusion;
	Exclusion.Center = FVector2D(0.0, 0.0);
	Exclusion.HalfExtent = FVector2D(500.0, 500.0);
	Box.Exclusions.Add(Exclusion);
	const TArray<FMantleScatterInstance> Holed = GenerateOk(*this, Box);
	int32 Expected = 0;
	for (const FMantleScatterInstance& Instance : Whole)
	{
		if (!(FMath::Abs(Instance.X) <= 5000 && FMath::Abs(Instance.Y) <= 5000))
		{
			TestTrue(TEXT("an exclusion keeps the others untouched"), Holed.Contains(Instance));
			++Expected;
		}
	}
	TestEqual(TEXT("an exclusion drops exactly what is inside it"), Holed.Num(), Expected);
	TestTrue(TEXT("an exclusion drops some"), Holed.Num() < Whole.Num());

	// Clusters: instances gather about a few centres, and the same parameters give the same ones.
	FMantleScatterParams Clustered = BoxParams();
	Clustered.HalfExtent = FVector(5000.0, 5000.0, 0.0);
	Clustered.Density = 0.1;
	Clustered.ClusterCount = 3;
	Clustered.ClusterRadius = 300.0;
	Instances = GenerateOk(*this, Clustered);
	TestEqual(TEXT("clustered count"), Instances.Num(), 1000);
	TestEqual(TEXT("clusters are deterministic"), FMantleScatter::Hash(Instances), FMantleScatter::Hash(GenerateOk(*this, Clustered)));
	// Three clusters of radius 3 m cannot reach across a 100 m box: some pair of instances is far apart and many are close.
	int32 Close = 0;
	for (int32 Index = 1; Index < Instances.Num(); ++Index)
	{
		const int64 Dx = Instances[Index].X - Instances[0].X;
		const int64 Dy = Instances[Index].Y - Instances[0].Y;
		Close += (Dx * Dx + Dy * Dy <= static_cast<int64>(6000) * 6000) ? 1 : 0;
	}
	TestTrue(TEXT("clustered: about a third of the instances stand in the first one's cluster"), Close > 100 && Close < 900);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterTerrainAndTierTest,
	"Mantle.Runtime.Scatter.Generator.TerrainAndFoliageTier",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterTerrainAndTierTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterTestPrivate;

	FMantleScatterParams P = BoxParams();
	P.HalfExtent = FVector(1000.0, 1000.0, 0.0);
	P.Density = 1.0;
	P.Placement = EMantleScatterPlacement::Terrain;

	// Without ground nothing stands.
	TestEqual(TEXT("terrain placement without ground drops everything"), GenerateOk(*this, P).Num(), 0);

	// Flat ground at 12.3456 cm (rounded by the caller to 123 mm), a hole where X is above 5 m: those are dropped.
	FMantleScatterGenerateOptions Options;
	Options.Ground = [](const int64 XMm, const int64 YMm, FMantleScatterGround& Out) -> bool
	{
		if (XMm > 5000)
		{
			return false;
		}
		Out.ZMm = 123;
		Out.NormalZ = 1.0;
		Out.WorldZCm = 12.3;
		return true;
	};
	TArray<FMantleScatterInstance> OnGround = GenerateOk(*this, P, Options);
	TestTrue(TEXT("some stand"), OnGround.Num() > 0);
	bool bAllOnGround = true;
	for (const FMantleScatterInstance& Instance : OnGround)
	{
		bAllOnGround &= Instance.Z == 123 && Instance.X <= 5000;
	}
	TestTrue(TEXT("every instance is on the ground and none is over the hole"), bAllOnGround);

	// The slope filter: ground steeper than 30 degrees (normal up 0.5) keeps none, flatter keeps all.
	Options.Ground = [](const int64 XMm, const int64 YMm, FMantleScatterGround& Out) -> bool
	{
		Out.ZMm = 0;
		Out.NormalZ = XMm < 0 ? 0.5 : 1.0;
		Out.WorldZCm = 0.0;
		return true;
	};
	P.MaxSlopeDegrees = 30.0;
	OnGround = GenerateOk(*this, P, Options);
	bool bOnlyFlat = OnGround.Num() > 0;
	for (const FMantleScatterInstance& Instance : OnGround)
	{
		bOnlyFlat &= Instance.X >= 0;
	}
	TestTrue(TEXT("the slope filter drops the steep side only"), bOnlyFlat);

	// The height filters use the surface's world height.
	P.MaxSlopeDegrees = 90.0;
	Options.Ground = [](const int64 XMm, const int64 YMm, FMantleScatterGround& Out) -> bool
	{
		Out.ZMm = 0;
		Out.NormalZ = 1.0;
		Out.WorldZCm = static_cast<double>(XMm) * 0.1;
		return true;
	};
	P.MinHeight = -200.0;
	P.MaxHeight = 300.0;
	OnGround = GenerateOk(*this, P, Options);
	bool bInBand = OnGround.Num() > 0;
	for (const FMantleScatterInstance& Instance : OnGround)
	{
		bInBand &= Instance.X >= -2000 && Instance.X <= 3000;
	}
	TestTrue(TEXT("the height filters keep the band"), bInBand);

	// The Foliage tier keeps a prefix of each stream: a lower tier is a subset of a higher one.
	FMantleScatterParams Box = BoxParams();
	Box.HalfExtent = FVector(2000.0, 2000.0, 0.0);
	Box.Density = 1.0;
	const TArray<FMantleScatterInstance> Full = GenerateOk(*this, Box);
	FMantleScatterGenerateOptions Thin;
	Thin.MaxCandidates = Full.Num() * 3 / 10;
	const TArray<FMantleScatterInstance> Low = GenerateOk(*this, Box, Thin);
	TestEqual(TEXT("a tier keeps its share of the stream"), Low.Num(), Full.Num() * 3 / 10);
	bool bSubset = true;
	for (int32 Index = 0; Index < Low.Num(); ++Index)
	{
		bSubset &= Low[Index] == Full[Index];
	}
	TestTrue(TEXT("a lower tier is a subset of a higher one"), bSubset);

	TestEqual(TEXT("Low keeps 30%"), FMantleScatter::FoliageTierDensity(0), 0.3);
	TestEqual(TEXT("Epic keeps all"), FMantleScatter::FoliageTierDensity(3), 1.0);
	TestEqual(TEXT("Epic draws the whole distance"), FMantleScatter::FoliageTierDistance(3), 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterPlaceCapsTest,
	"Mantle.Runtime.Scatter.Caps.PlaceTotalsAreSharedByParserAndScripts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterPlaceCapsTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterTestPrivate;

	// 40 instances each (20 m x 20 m at 0.1); 5000 of them would be 200000, 51 colliding ones pass 2000.
	FMantleScatterParams Forty = BoxParams();
	Forty.HalfExtent = FVector(1000.0, 1000.0, 0.0);
	Forty.Density = 0.1;
	FMantleScatterParams Colliding = Forty;
	Colliding.bCollision = true;

	TArray<const FMantleScatterParams*> All;
	for (int32 Index = 0; Index < 50; ++Index)
	{
		All.Add(&Colliding);
	}
	int32 Bad = INDEX_NONE;
	FString Error;
	TestTrue(TEXT("exactly 2000 colliding instances are fine"), FMantleScatter::CheckPlaceTotals(All, Bad, Error));
	All.Add(&Colliding);
	TestFalse(TEXT("the 2001st colliding instance crosses the cap"), FMantleScatter::CheckPlaceTotals(All, Bad, Error));
	TestEqual(TEXT("at the Scatter that crosses it"), Bad, 50);
	TestTrue(TEXT("with the parser's message"), Error.Contains(TEXT("colliding scatter instances; a place holds at most 2000")));

	// Non-colliding scatters do not count towards the colliding cap, only towards the place total.
	All.Reset();
	for (int32 Index = 0; Index < 51; ++Index)
	{
		All.Add(&Forty);
	}
	TestTrue(TEXT("51 visual Scatters are within 200000"), FMantleScatter::CheckPlaceTotals(All, Bad, Error));
	FMantleScatterParams Huge = BoxParams();
	Huge.HalfExtent = FVector(10000.0, 10000.0, 0.0);
	Huge.Density = 5.0; // 200000
	All = { &Huge, &Forty };
	TestFalse(TEXT("200000 plus 40 passes the place total"), FMantleScatter::CheckPlaceTotals(All, Bad, Error));
	TestEqual(TEXT("at the second"), Bad, 1);
	TestTrue(TEXT("with its message"), Error.Contains(TEXT("scatter instances; a place holds at most 200000")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterRoundedFiltersTest,
	"Mantle.Runtime.Scatter.Generator.FiltersCompareRoundedNumbers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterRoundedFiltersTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterTestPrivate;

	FMantleScatterParams P = BoxParams();
	P.HalfExtent = FVector(1000.0, 1000.0, 0.0);
	P.Density = 1.0;
	P.Placement = EMantleScatterPlacement::Terrain;
	P.MinHeight = 100.0; // 1000 mm

	// A surface 0.04 mm under the limit (and 0.04 mm over) rounds to the limit's millimetre: kept both times, however the float came out.
	FMantleScatterGenerateOptions Options;
	for (const double HeightCm : { 99.996, 100.004 })
	{
		Options.Ground = [HeightCm](const int64, const int64, FMantleScatterGround& Out) -> bool
		{
			Out.ZMm = 0;
			Out.NormalZ = 1.0;
			Out.WorldZCm = HeightCm;
			return true;
		};
		TestTrue(TEXT("a height within half a millimetre of the limit is on the limit"), GenerateOk(*this, P, Options).Num() > 0);
	}
	// 0.6 mm under is a different millimetre: dropped.
	Options.Ground = [](const int64, const int64, FMantleScatterGround& Out) -> bool
	{
		Out.ZMm = 0;
		Out.NormalZ = 1.0;
		Out.WorldZCm = 99.94;
		return true;
	};
	TestEqual(TEXT("a height a millimetre under the limit is dropped"), GenerateOk(*this, P, Options).Num(), 0);

	// A normal within half a millionth of the threshold is on it (kept); a clearly steeper one is dropped.
	P.MinHeight.Reset();
	P.MaxSlopeDegrees = 60.0; // cos = 0.5
	for (const double NormalZ : { 0.4999996, 0.5000004 })
	{
		Options.Ground = [NormalZ](const int64, const int64, FMantleScatterGround& Out) -> bool
		{
			Out.ZMm = 0;
			Out.NormalZ = NormalZ;
			Out.WorldZCm = 0.0;
			return true;
		};
		TestTrue(TEXT("a normal within half a millionth of the slope limit is kept"), GenerateOk(*this, P, Options).Num() > 0);
	}
	Options.Ground = [](const int64, const int64, FMantleScatterGround& Out) -> bool
	{
		Out.ZMm = 0;
		Out.NormalZ = 0.49;
		Out.WorldZCm = 0.0;
		return true;
	};
	TestEqual(TEXT("a steeper normal is dropped"), GenerateOk(*this, P, Options).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterPaintLayerTest,
	"Mantle.Runtime.Scatter.Generator.PaintLayerFilter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterPaintLayerTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterTestPrivate;

	FMantleScatterParams P = BoxParams();
	P.HalfExtent = FVector(1000.0, 1000.0, 0.0);
	P.Density = 1.0;
	P.Placement = EMantleScatterPlacement::Terrain;

	// The layer weighs 230 of 255 (about 0.902) on the east half (X >= 0) and 25 (about 0.098) on the west half.
	FMantleScatterGenerateOptions Options;
	Options.Ground = [](const int64 XMm, const int64, FMantleScatterGround& Out) -> bool
	{
		Out.ZMm = 0;
		Out.NormalZ = 1.0;
		Out.WorldZCm = 0.0;
		Out.LayerWeight = XMm >= 0 ? 230 : 25;
		return true;
	};
	const TArray<FMantleScatterInstance> Unfiltered = GenerateOk(*this, P, Options);

	// Filter off (no paint layer): the weight is never read, every instance stays.
	TestTrue(TEXT("some stand without the filter"), Unfiltered.Num() > 0);
	const uint64 UnfilteredHash = FMantleScatter::Hash(Unfiltered);

	// At least 0.9: the east half only. 0.9 x 255 = 229.5, so 230 passes and 229 would not.
	P.PaintLayer = 1;
	P.MinLayerWeight = 0.9;
	TArray<FMantleScatterInstance> Kept = GenerateOk(*this, P, Options);
	bool bEastOnly = Kept.Num() > 0 && Kept.Num() < Unfiltered.Num();
	for (const FMantleScatterInstance& Instance : Kept)
	{
		bEastOnly &= Instance.X >= 0;
	}
	TestTrue(TEXT("a minimum keeps the heavy side only"), bEastOnly);
	TestEqual(TEXT("the instances that stay are the unfiltered ones, unchanged"), Kept.Num(),
		Unfiltered.FilterByPredicate([](const FMantleScatterInstance& I) { return I.X >= 0; }).Num());
	TestEqual(TEXT("the filtered hash is stable"), FMantleScatter::Hash(GenerateOk(*this, P, Options)), FMantleScatter::Hash(Kept));
	TestTrue(TEXT("the filter changes the hash"), FMantleScatter::Hash(Kept) != UnfilteredHash);

	// At most 0.5: the west half only.
	P.MinLayerWeight.Reset();
	P.MaxLayerWeight = 0.5;
	Kept = GenerateOk(*this, P, Options);
	bool bWestOnly = Kept.Num() > 0 && Kept.Num() < Unfiltered.Num();
	for (const FMantleScatterInstance& Instance : Kept)
	{
		bWestOnly &= Instance.X < 0;
	}
	TestTrue(TEXT("a maximum keeps the light side only"), bWestOnly);

	// A band that holds neither side keeps none; one that holds both keeps all.
	P.MinLayerWeight = 0.4;
	P.MaxLayerWeight = 0.6;
	TestEqual(TEXT("a band between the two weights keeps none"), GenerateOk(*this, P, Options).Num(), 0);
	P.MinLayerWeight = 0.0;
	P.MaxLayerWeight = 1.0;
	TestEqual(TEXT("0 to 1 keeps every instance"), GenerateOk(*this, P, Options).Num(), Unfiltered.Num());

	// The threshold is exact in whole numbers: weight 230 passes a minimum m when 230 x 1,000,000 >= (m in millionths) x 255.
	P.MinLayerWeight = 0.901961; // 901961 x 255 = 230,000,055: just above 230,000,000, so 230 is dropped
	TestEqual(TEXT("a minimum a hair above a byte's weight drops that byte"), GenerateOk(*this, P, Options).Num() > 0 ? 1 : 0, 0);
	P.MinLayerWeight = 0.90196; // 901960 x 255 = 229,999,800: below 230,000,000, so 230 passes
	TestTrue(TEXT("a minimum a hair below a byte's weight keeps that byte"), GenerateOk(*this, P, Options).Num() > 0);

	// A layer the terrain lacks weighs 0 (the ground function's call): a minimum above 0 keeps none, a maximum keeps all.
	Options.Ground = [](const int64, const int64, FMantleScatterGround& Out) -> bool
	{
		Out.ZMm = 0;
		Out.NormalZ = 1.0;
		Out.WorldZCm = 0.0;
		Out.LayerWeight = 0;
		return true;
	};
	P.MinLayerWeight = 0.5;
	P.MaxLayerWeight.Reset();
	TestEqual(TEXT("a missing layer fails a minimum"), GenerateOk(*this, P, Options).Num(), 0);
	P.MinLayerWeight.Reset();
	P.MaxLayerWeight = 0.5;
	TestTrue(TEXT("a missing layer passes a maximum"), GenerateOk(*this, P, Options).Num() > 0);

	// Box placement ignores the layer (the parser refuses the keys there), and a bad layer is refused by Generate.
	FMantleScatterParams Box = BoxParams();
	TArray<FMantleScatterInstance> Out;
	FString Error;
	Box.PaintLayer = 8;
	Box.MinLayerWeight = 0.5;
	TestFalse(TEXT("a layer past 7 is refused"), FMantleScatter::Generate(Box, Out, Error));
	Box.PaintLayer = 2;
	Box.MinLayerWeight.Reset();
	TestFalse(TEXT("a layer with no limit is refused"), FMantleScatter::Generate(Box, Out, Error));
	Box.PaintLayer = -1;
	Box.MaxLayerWeight = 0.5;
	TestFalse(TEXT("a limit with no layer is refused"), FMantleScatter::Generate(Box, Out, Error));

	// ToJson writes the keys back to the same parameters.
	P.PaintLayer = 3;
	P.MinLayerWeight = 0.25;
	P.MaxLayerWeight = 0.75;
	const TSharedRef<FJsonObject> Json = FMantleScatter::ToJson(P);
	TestEqual(TEXT("paintLayer is written"), static_cast<int32>(Json->GetNumberField(TEXT("paintLayer"))), 3);
	TestEqual(TEXT("minLayerWeight is written"), Json->GetNumberField(TEXT("minLayerWeight")), 0.25);
	TestEqual(TEXT("maxLayerWeight is written"), Json->GetNumberField(TEXT("maxLayerWeight")), 0.75);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterParserTest,
	"Mantle.Runtime.Scatter.Parser.ReadsAndRefusesTheNewKeys",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterParserTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterTestPrivate;

	FMantleParseResult Result = ParseScatter(TEXT(""));
	TestTrue(TEXT("the required keys alone parse"), Result.bSuccess);
	const FMantleScatterParams* Plain = ScatterOf(Result);
	const bool bCollisionOffByDefault = Plain && !Plain->bCollision;
	const bool bNoPaintLayerByDefault = Plain && Plain->PaintLayer == -1 && !Plain->MinLayerWeight.IsSet() && !Plain->MaxLayerWeight.IsSet();
	TestTrue(TEXT("defaults: box placement, box shape, no clusters, never culled"),
		Plain && Plain->Placement == EMantleScatterPlacement::Box && Plain->Shape == EMantleScatterShape::Box
		&& Plain->ClusterCount == 0 && Plain->DrawDistance == 0.0 && Plain->MaxSlopeDegrees == 90.0);

	Result = ParseScatter(TEXT(",\"drawDistance\":12000,\"placement\":\"terrain\",\"maxSlopeDegrees\":35,\"minHeight\":-50,\"maxHeight\":900,"
		"\"exclusions\":[{\"center\":[100,-200],\"halfExtent\":[50,60]}],\"clusterCount\":12,\"clusterRadius\":400"));
	TestTrue(TEXT("every terrain key parses"), Result.bSuccess);
	const FMantleScatterParams* Terrain = ScatterOf(Result);
	TestTrue(TEXT("terrain keys are read"),
		Terrain && Terrain->DrawDistance == 12000.0 && Terrain->Placement == EMantleScatterPlacement::Terrain
		&& Terrain->MaxSlopeDegrees == 35.0 && Terrain->MinHeight.Get(0.0) == -50.0 && Terrain->MaxHeight.Get(0.0) == 900.0
		&& Terrain->Exclusions.Num() == 1 && Terrain->Exclusions[0].HalfExtent.Y == 60.0 && Terrain->ClusterCount == 12
		&& Terrain->ClusterRadius == 400.0);

	Result = ParseScatter(TEXT(",\"shape\":\"disc\",\"radius\":800"));
	TestTrue(TEXT("a disc parses"), Result.bSuccess && ScatterOf(Result) && ScatterOf(Result)->Radius == 800.0);
	Result = ParseScatter(TEXT(",\"shape\":\"path\",\"path\":[[0,0],[500,0],[500,500]],\"width\":300"));
	TestTrue(TEXT("a path parses"), Result.bSuccess && ScatterOf(Result) && ScatterOf(Result)->Path.Num() == 3 && ScatterOf(Result)->Width == 300.0);

	TestTrue(TEXT("a slope with box placement is refused"), HasErrorAt(ParseScatter(TEXT(",\"maxSlopeDegrees\":10")), TEXT("maxSlopeDegrees")));
	Result = ParseScatter(TEXT(",\"placement\":\"terrain\",\"paintLayer\":5,\"minLayerWeight\":0.9,\"maxLayerWeight\":1"));
	const FMantleScatterParams* Painted = ScatterOf(Result);
	TestTrue(TEXT("the paint-layer keys parse"), Result.bSuccess && Painted && Painted->PaintLayer == 5
		&& Painted->MinLayerWeight.Get(-1.0) == 0.9 && Painted->MaxLayerWeight.Get(-1.0) == 1.0);
	TestTrue(TEXT("no paint layer by default"), bNoPaintLayerByDefault);
	TestTrue(TEXT("a paint layer on box placement is refused"), HasErrorAt(ParseScatter(TEXT(",\"paintLayer\":1,\"minLayerWeight\":0.5")), TEXT("paintLayer")));
	TestTrue(TEXT("a paint layer past 7 is refused"),
		HasErrorAt(ParseScatter(TEXT(",\"placement\":\"terrain\",\"paintLayer\":8,\"minLayerWeight\":0.5")), TEXT("paintLayer")));
	TestTrue(TEXT("a paint layer without a limit is refused"),
		HasErrorAt(ParseScatter(TEXT(",\"placement\":\"terrain\",\"paintLayer\":1")), TEXT("paintLayer")));
	TestTrue(TEXT("a limit without a paint layer is refused"),
		HasErrorAt(ParseScatter(TEXT(",\"placement\":\"terrain\",\"maxLayerWeight\":0.5")), TEXT("maxLayerWeight")));
	TestTrue(TEXT("a layer weight past 1 is refused"),
		HasErrorAt(ParseScatter(TEXT(",\"placement\":\"terrain\",\"paintLayer\":1,\"minLayerWeight\":1.5")), TEXT("minLayerWeight")));
	TestTrue(TEXT("layer min above max is refused"),
		HasErrorAt(ParseScatter(TEXT(",\"placement\":\"terrain\",\"paintLayer\":1,\"minLayerWeight\":0.9,\"maxLayerWeight\":0.2")), TEXT("maxLayerWeight")));
	TestTrue(TEXT("min above max is refused"),
		HasErrorAt(ParseScatter(TEXT(",\"placement\":\"terrain\",\"minHeight\":10,\"maxHeight\":5")), TEXT("maxHeight")));
	TestTrue(TEXT("a disc without a radius is refused"), HasErrorAt(ParseScatter(TEXT(",\"shape\":\"disc\"")), TEXT("radius")));
	TestTrue(TEXT("a radius on a box is refused"), HasErrorAt(ParseScatter(TEXT(",\"radius\":100")), TEXT("radius")));
	TestTrue(TEXT("a path of one point is refused"),
		HasErrorAt(ParseScatter(TEXT(",\"shape\":\"path\",\"path\":[[0,0]],\"width\":100")), TEXT("path")));
	TestTrue(TEXT("a path without a width is refused"),
		HasErrorAt(ParseScatter(TEXT(",\"shape\":\"path\",\"path\":[[0,0],[1,1]]")), TEXT("width")));
	TestTrue(TEXT("clusterCount alone is refused"), HasErrorAt(ParseScatter(TEXT(",\"clusterCount\":4")), TEXT("clusterRadius")));
	TestTrue(TEXT("a draw distance of 0 is refused"), HasErrorAt(ParseScatter(TEXT(",\"drawDistance\":0")), TEXT("drawDistance")));

	Result = ParseScatter(TEXT(",\"collision\":true"));
	TestTrue(TEXT("collision parses"), Result.bSuccess && ScatterOf(Result) && ScatterOf(Result)->bCollision);
	TestTrue(TEXT("collision is off by default"), bCollisionOffByDefault);
	TestTrue(TEXT("a collision that is not a boolean is refused"), HasErrorAt(ParseScatter(TEXT(",\"collision\":\"yes\"")), TEXT("collision")));
	// halfExtent 1000 x 1000 cm at density 0.1 makes 40 instances: 51 such Scatters pass the place's 2000 colliding.
	{
		FString Entities;
		for (int32 Index = 0; Index < 51; ++Index)
		{
			Entities += (Index ? TEXT(",") : TEXT("")) + FString::Printf(
				TEXT("{\"id\":\"ent_scatter_%d\",\"type\":\"Scatter\",\"parent\":null,\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},"
					 "\"properties\":{\"halfExtent\":[1000,1000,0],\"density\":0.1,\"collision\":true,\"meshes\":[{\"mesh\":\"ast_cube\"}]}}"), Index);
		}
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_scatter002\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Field\",\"authorId\":\"usr_4b1d\"},\"settings\":{},"
				 "\"assets\":[{\"id\":\"ast_cube\",\"kind\":\"StaticMesh\",\"hash\":\"md5:0123456789abcdef\",\"bytes\":1024}],\"entities\":[%s]}"), *Entities);
		const FMantleParseResult Over = FMantlePlaceParser::ParseFromString(Json);
		TestTrue(TEXT("past 2000 colliding instances in a place is refused at the Scatter that crosses it"),
			Over.Errors.ContainsByPredicate([](const FMantleParseError& Error) { return Error.Path == TEXT("/entities/50/properties/density"); }));
	}

	FString Exclusions;
	for (int32 Index = 0; Index < 17; ++Index)
	{
		Exclusions += (Index ? TEXT(",") : TEXT("")) + FString(TEXT("{\"center\":[0,0],\"halfExtent\":[10,10]}"));
	}
	TestTrue(TEXT("17 exclusions are refused"), HasErrorAt(ParseScatter(TEXT(",\"exclusions\":[") + Exclusions + TEXT("]")), TEXT("exclusions")));
	return true;
}

// Minimum slope and ground alignment (#188).
namespace MantleScatterTestPrivate
{
	/** Flat-looking ground at height 0 whose surface normal (the same in world and scatter space) is Normal. */
	FMantleScatterGenerateOptions SlopeOptions(const FVector& Normal)
	{
		FMantleScatterGenerateOptions Options;
		Options.Ground = [Normal](const int64, const int64, FMantleScatterGround& Out) -> bool
		{
			Out.ZMm = 0;
			Out.NormalZ = Normal.Z;
			Out.LocalNormalX = Normal.X;
			Out.LocalNormalY = Normal.Y;
			Out.LocalNormalZ = Normal.Z;
			Out.WorldZCm = 0.0;
			return true;
		};
		return Options;
	}

	FMantleScatterParams TerrainParams()
	{
		FMantleScatterParams P = BoxParams();
		P.HalfExtent = FVector(1000.0, 1000.0, 0.0);
		P.Density = 1.0;
		P.Placement = EMantleScatterPlacement::Terrain;
		return P;
	}

	double DegreesBetween(const FVector& A, const FVector& B)
	{
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(A.GetSafeNormal(), B.GetSafeNormal()), -1.0, 1.0)));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterMinSlopeTest,
	"Mantle.Runtime.Scatter.Generator.MinSlopeFilter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterMinSlopeTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterTestPrivate;

	FMantleScatterParams P = TerrainParams();

	// The west half (X < 0) is a 60 degree slope (normal up 0.5), the east half is flat.
	FMantleScatterGenerateOptions Options;
	Options.Ground = [](const int64 XMm, const int64, FMantleScatterGround& Out) -> bool
	{
		Out.ZMm = 0;
		Out.NormalZ = XMm < 0 ? 0.5 : 1.0;
		Out.WorldZCm = 0.0;
		return true;
	};
	const TArray<FMantleScatterInstance> Everything = GenerateOk(*this, P, Options);
	TestTrue(TEXT("some stand without a limit"), Everything.Num() > 0);

	// A minimum of 0 is the default: nothing is dropped, and the instances are the ones the generator always made.
	P.MinSlopeDegrees = 0.0;
	TestEqual(TEXT("a minimum of 0 filters nothing"), FMantleScatter::Hash(GenerateOk(*this, P, Options)), FMantleScatter::Hash(Everything));

	// A minimum of 30 keeps the steep (west) side only, and those instances are the unfiltered ones, unchanged.
	P.MinSlopeDegrees = 30.0;
	TArray<FMantleScatterInstance> Kept = GenerateOk(*this, P, Options);
	TestTrue(TEXT("the instances that stay are the unfiltered west ones"),
		Kept == Everything.FilterByPredicate([](const FMantleScatterInstance& I) { return I.X < 0; }));
	TestTrue(TEXT("a minimum keeps some and drops some"), Kept.Num() > 0 && Kept.Num() < Everything.Num());
	TestEqual(TEXT("the filtered hash is stable"), FMantleScatter::Hash(GenerateOk(*this, P, Options)), FMantleScatter::Hash(Kept));

	// With a maximum on top, a band that holds 60 degrees keeps the west, one that holds neither side keeps none.
	P.MaxSlopeDegrees = 70.0;
	TestEqual(TEXT("a 30 to 70 degree band keeps the 60 degree side"), GenerateOk(*this, P, Options).Num(), Kept.Num());
	P.MinSlopeDegrees = 10.0;
	P.MaxSlopeDegrees = 50.0;
	TestEqual(TEXT("a 10 to 50 degree band holds neither side"), GenerateOk(*this, P, Options).Num(), 0);

	// A slope on the limit is kept both ways: min = max = 60 keeps the 60 degree side (cos 60 = 0.5 to the millionth) and drops flat.
	P.MinSlopeDegrees = 60.0;
	P.MaxSlopeDegrees = 60.0;
	TestEqual(TEXT("a slope on the limit is kept"), GenerateOk(*this, P, Options).Num(), Kept.Num());

	// A normal within half a millionth of the threshold is on it; a clearly flatter one is dropped, a clearly steeper one kept.
	P.MaxSlopeDegrees = 90.0;
	for (const double NormalZ : { 0.4999996, 0.5000004 })
	{
		TestTrue(TEXT("a normal within half a millionth of the minimum is kept"), GenerateOk(*this, P, SlopeOptions(FVector(FMath::Sqrt(1.0 - NormalZ * NormalZ), 0.0, NormalZ))).Num() > 0);
	}
	TestEqual(TEXT("flatter ground than the minimum is dropped"), GenerateOk(*this, P, SlopeOptions(FVector(0.0, FMath::Sin(FMath::DegreesToRadians(59.0)), FMath::Cos(FMath::DegreesToRadians(59.0))))).Num(), 0);
	TestTrue(TEXT("steeper ground than the minimum is kept"), GenerateOk(*this, P, SlopeOptions(FVector(0.0, FMath::Sin(FMath::DegreesToRadians(61.0)), FMath::Cos(FMath::DegreesToRadians(61.0))))).Num() > 0);

	// A minimum of 90 keeps a vertical wall (normal up 0) and nothing less.
	P.MinSlopeDegrees = 90.0;
	TestTrue(TEXT("a wall is kept by a minimum of 90"), GenerateOk(*this, P, SlopeOptions(FVector(1.0, 0.0, 0.0))).Num() > 0);
	TestEqual(TEXT("a steep slope is dropped by a minimum of 90"),
		GenerateOk(*this, P, SlopeOptions(FVector(FMath::Sin(FMath::DegreesToRadians(89.0)), 0.0, FMath::Cos(FMath::DegreesToRadians(89.0))))).Num(), 0);

	// Box placement ignores the limit (the parser refuses the key there); the numbers are checked.
	FMantleScatterParams Box = BoxParams();
	TArray<FMantleScatterInstance> Out;
	FString Error;
	Box.MinSlopeDegrees = 45.0;
	TestTrue(TEXT("a minimum on box placement filters nothing"), FMantleScatter::Generate(Box, Out, Error) && Out.Num() == 30);
	Box.MinSlopeDegrees = -1.0;
	TestFalse(TEXT("a negative minimum is refused"), FMantleScatter::Generate(Box, Out, Error));
	Box.MinSlopeDegrees = 91.0;
	TestFalse(TEXT("a minimum past 90 is refused"), FMantleScatter::Generate(Box, Out, Error));
	P.MinSlopeDegrees = 60.0;
	P.MaxSlopeDegrees = 30.0;
	TestFalse(TEXT("a minimum above the maximum is refused"), FMantleScatter::Generate(P, Out, Error));
	TestTrue(TEXT("with a message naming both"), Error.Contains(TEXT("min slope")) && Error.Contains(TEXT("max slope")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterGroundAlignmentTest,
	"Mantle.Runtime.Scatter.Generator.GroundAlignment",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterGroundAlignmentTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterTestPrivate;

	FMantleScatterParams P = TerrainParams();
	P.Density = 2.0;
	P.MaxTiltDegrees = 20.0;

	// Existing alignments never read the normal: on sloped ground Upright and Tilted give the box's instances, only the height differs.
	const FVector Slope(0.6, -0.48, 0.64);
	FMantleScatterParams Plain = P;
	Plain.Alignment = EMantleScatterAlignment::Tilted;
	const TArray<FMantleScatterInstance> TiltedOnSlope = GenerateOk(*this, Plain, SlopeOptions(Slope));
	const TArray<FMantleScatterInstance> TiltedOnFlat = GenerateOk(*this, Plain, SlopeOptions(FVector::UpVector));
	TestTrue(TEXT("tilted instances ignore the slope"), TiltedOnSlope == TiltedOnFlat);

	// Ground on flat ground is exactly Tilted (the same instances, the same hash), whatever the tilt.
	FMantleScatterParams Ground = P;
	Ground.Alignment = EMantleScatterAlignment::Ground;
	TestTrue(TEXT("ground on flat ground is tilted"), GenerateOk(*this, Ground, SlopeOptions(FVector::UpVector)) == TiltedOnFlat);
	Ground.MaxTiltDegrees = 0.0;
	FMantleScatterParams UprightParams = P;
	UprightParams.MaxTiltDegrees = 0.0;
	TestTrue(TEXT("ground with no tilt on flat ground is upright"),
		GenerateOk(*this, Ground, SlopeOptions(FVector::UpVector)) == GenerateOk(*this, UprightParams, SlopeOptions(FVector::UpVector)));

	// On a slope with no tilt, each instance's up is the ground normal, and it is the flat instance turned onto the slope.
	const TArray<FVector> Normals = {
		Slope.GetSafeNormal(), FVector(0.5, 0.0, 0.8660254).GetSafeNormal(), FVector(-0.3, 0.2, 0.9).GetSafeNormal(),
		FVector(0.0, 1.0, 0.1).GetSafeNormal(), FVector(1.0, 0.0, 0.0), FVector(0.0, -1.0, 0.0), FVector(-0.7071068, 0.7071068, 0.0),
		FVector(0.9999, 0.0, 0.01).GetSafeNormal(), FVector(0.0, 0.01, 0.99995).GetSafeNormal() };
	Ground.MaxTiltDegrees = 0.0;
	FMantleScatterParams FlatReference = Ground;
	FlatReference.Alignment = EMantleScatterAlignment::Tilted;
	const TArray<FMantleScatterInstance> FlatInstances = GenerateOk(*this, FlatReference, SlopeOptions(FVector::UpVector));
	for (const FVector& Normal : Normals)
	{
		const TArray<FMantleScatterInstance> Aligned = GenerateOk(*this, Ground, SlopeOptions(Normal));
		TestEqual(*FString::Printf(TEXT("a slope %s keeps every instance"), *Normal.ToCompactString()), Aligned.Num(), FlatInstances.Num());
		double WorstUp = 0.0;
		double WorstTurn = 0.0;
		bool bOthersUnchanged = true;
		for (int32 Index = 0; Index < Aligned.Num() && Index < FlatInstances.Num(); ++Index)
		{
			const FQuat Got = FMantleScatter::ToTransform(Aligned[Index]).GetRotation();
			WorstUp = FMath::Max(WorstUp, DegreesBetween(Got.GetAxisZ(), Normal));
			// The flat turn, then the shortest turn from straight up onto the slope.
			const FQuat Flat = FMantleScatter::ToTransform(FlatInstances[Index]).GetRotation();
			const FQuat Expected = FQuat::FindBetweenNormals(FVector::UpVector, Normal) * Flat;
			WorstTurn = FMath::Max(WorstTurn, FMath::RadiansToDegrees(Got.AngularDistance(Expected)));
			bOthersUnchanged &= Aligned[Index].X == FlatInstances[Index].X && Aligned[Index].Y == FlatInstances[Index].Y
				&& Aligned[Index].Z == FlatInstances[Index].Z && Aligned[Index].Scale == FlatInstances[Index].Scale
				&& Aligned[Index].Mesh == FlatInstances[Index].Mesh;
		}
		TestTrue(TEXT("position, scale and mesh are the flat instance's"), bOthersUnchanged);
		TestTrue(*FString::Printf(TEXT("up follows the slope %s (worst %.4f degrees off)"), *Normal.ToCompactString(), WorstUp), WorstUp < 0.03);
		TestTrue(*FString::Printf(TEXT("the turn is the flat turn carried onto the slope %s (worst %.4f degrees off)"), *Normal.ToCompactString(), WorstTurn),
			WorstTurn < 0.05);
	}

	// The turn about the normal is random: the instances' forward axes spread all the way round it.
	{
		const FVector Normal = Normals[0];
		const TArray<FMantleScatterInstance> Aligned = GenerateOk(*this, Ground, SlopeOptions(Normal));
		const FVector Across = FVector::CrossProduct(Normal, FVector::UpVector).GetSafeNormal();
		const FVector Along = FVector::CrossProduct(Across, Normal);
		int32 Quadrants[4] = { 0, 0, 0, 0 };
		for (const FMantleScatterInstance& Instance : Aligned)
		{
			const FVector Forward = FMantleScatter::ToTransform(Instance).GetRotation().GetAxisX();
			const double X = FVector::DotProduct(Forward, Across);
			const double Y = FVector::DotProduct(Forward, Along);
			++Quadrants[(X >= 0.0 ? 0 : 1) + (Y >= 0.0 ? 0 : 2)];
		}
		TestTrue(TEXT("instances turn every way about the normal"), Quadrants[0] > 20 && Quadrants[1] > 20 && Quadrants[2] > 20 && Quadrants[3] > 20);
	}

	// With a tilt, up leans off the normal by at most the tilt, and by about that much for some instances.
	Ground.MaxTiltDegrees = 20.0;
	for (const FVector& Normal : { Normals[0], Normals[4], Normals[6] })
	{
		const TArray<FMantleScatterInstance> Leaning = GenerateOk(*this, Ground, SlopeOptions(Normal));
		double Worst = 0.0;
		for (const FMantleScatterInstance& Instance : Leaning)
		{
			Worst = FMath::Max(Worst, DegreesBetween(FMantleScatter::ToTransform(Instance).GetRotation().GetAxisZ(), Normal));
		}
		TestTrue(*FString::Printf(TEXT("the lean off %s stays within the tilt (worst %.3f)"), *Normal.ToCompactString(), Worst), Worst <= 20.0 * 1.4143 + 0.05);
		TestTrue(*FString::Printf(TEXT("some instances lean off %s (worst %.3f)"), *Normal.ToCompactString(), Worst), Worst > 10.0);
		TestEqual(TEXT("the aligned instances are stable"), FMantleScatter::Hash(GenerateOk(*this, Ground, SlopeOptions(Normal))), FMantleScatter::Hash(Leaning));
	}

	// The slope filters read the world up component, the alignment the scatter-space normal: each on its own.
	{
		FMantleScatterGenerateOptions Split;
		Split.Ground = [](const int64, const int64, FMantleScatterGround& Out) -> bool
		{
			Out.ZMm = 0;
			Out.NormalZ = 1.0;
			Out.LocalNormalX = 1.0;
			Out.LocalNormalY = 0.0;
			Out.LocalNormalZ = 0.0;
			Out.WorldZCm = 0.0;
			return true;
		};
		const TArray<FMantleScatterInstance> Turned = GenerateOk(*this, Ground, Split);
		TestEqual(TEXT("a flat world normal does not stop alignment"), Turned.Num(), FlatInstances.Num());
		TestTrue(TEXT("the scatter-space normal decides the turn"),
			Turned.Num() > 0 && DegreesBetween(FMantleScatter::ToTransform(Turned[0]).GetRotation().GetAxisZ(), FVector(1.0, 0.0, 0.0)) <= 20.1);
	}

	// A normal that is not unit length or has no up (a ground function's own choice) still gives a clean turn.
	{
		Ground.MaxTiltDegrees = 0.0;
		const TArray<FMantleScatterInstance> Long = GenerateOk(*this, Ground, SlopeOptions(Normals[0] * 3.0));
		const TArray<FMantleScatterInstance> Unit = GenerateOk(*this, Ground, SlopeOptions(Normals[0]));
		TestTrue(TEXT("the normal is normalised"), Long == Unit);
		const TArray<FMantleScatterInstance> Upside = GenerateOk(*this, Ground, SlopeOptions(FVector(0.0, 0.0, -1.0)));
		TestTrue(TEXT("a normal pointing down leaves the instances as they were"), Upside == FlatInstances);
	}

	// Ground alignment needs a Terrain; a bad combination is refused and ToJson writes both new keys back.
	FMantleScatterParams Box = BoxParams();
	Box.Alignment = EMantleScatterAlignment::Ground;
	TArray<FMantleScatterInstance> Out;
	FString Error;
	TestFalse(TEXT("ground alignment on box placement is refused"), FMantleScatter::Generate(Box, Out, Error));
	TestTrue(TEXT("with a message naming terrain"), Error.Contains(TEXT("terrain")));

	Ground.MinSlopeDegrees = 40.0;
	const TSharedRef<FJsonObject> Json = FMantleScatter::ToJson(Ground);
	TestEqual(TEXT("alignment is written"), Json->GetStringField(TEXT("alignment")), FString(TEXT("ground")));
	TestEqual(TEXT("minSlopeDegrees is written"), Json->GetNumberField(TEXT("minSlopeDegrees")), 40.0);
	FMantleScatterParams Back;
	TArray<FMantleParseError> Errors;
	TestTrue(TEXT("the written keys read back"), FMantlePlaceParser::ParseScatterProperties(Json, Back, Errors));
	TestTrue(TEXT("to the same settings"), Back.Alignment == EMantleScatterAlignment::Ground && Back.MinSlopeDegrees == 40.0 && Back.MaxTiltDegrees == 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterGroundAlignmentGoldenTest,
	"Mantle.Runtime.Scatter.Generator.GroundAlignmentIsStable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterGroundAlignmentGoldenTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterTestPrivate;

	// The numbers come out of integer maths only (no libm), so they are fixed: a slope that tilts three ways across the area.
	FMantleScatterParams P = TerrainParams();
	P.Seed = 188;
	P.Density = 0.5;
	P.Alignment = EMantleScatterAlignment::Ground;
	P.MaxTiltDegrees = 12.0;
	P.MinSlopeDegrees = 20.0;
	FMantleScatterGenerateOptions Options;
	Options.Ground = [](const int64 XMm, const int64 YMm, FMantleScatterGround& Out) -> bool
	{
		const FVector Normal = XMm < -3000 ? FVector(0.6, -0.48, 0.64) : (YMm < 0 ? FVector(-0.8, 0.0, 0.6) : FVector(0.0, 0.9701425, 0.2425356));
		Out.ZMm = 0;
		Out.NormalZ = Normal.Z;
		Out.LocalNormalX = Normal.X;
		Out.LocalNormalY = Normal.Y;
		Out.LocalNormalZ = Normal.Z;
		Out.WorldZCm = 0.0;
		return true;
	};
	const TArray<FMantleScatterInstance> Instances = GenerateOk(*this, P, Options);
	TestEqual(TEXT("count"), Instances.Num(), 0);
	TestEqual(TEXT("hash"), FMantleScatter::HashText(FMantleScatter::Hash(Instances)), FString(TEXT("")));
	if (Instances.Num() > 0)
	{
		const FMantleScatterInstance& First = Instances[0];
		AddInfo(FString::Printf(TEXT("first: %d %d %d yaw %d pitch %d roll %d"), First.X, First.Y, First.Z, First.Yaw, First.Pitch, First.Roll));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterSlopeParserTest,
	"Mantle.Runtime.Scatter.Parser.MinSlopeAndGroundAlignment",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterSlopeParserTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterTestPrivate;

	const FMantleScatterParams* Plain = nullptr;
	FMantleParseResult Result = ParseScatter(TEXT(""));
	Plain = ScatterOf(Result);
	TestTrue(TEXT("defaults: no minimum slope, upright"),
		Plain && Plain->MinSlopeDegrees == 0.0 && Plain->Alignment == EMantleScatterAlignment::Upright);

	Result = ParseScatter(TEXT(",\"placement\":\"terrain\",\"minSlopeDegrees\":50,\"maxSlopeDegrees\":85,\"alignment\":\"ground\",\"maxTilt\":5"));
	const FMantleScatterParams* Cliffs = ScatterOf(Result);
	TestTrue(TEXT("the minimum slope and ground alignment parse"), Result.bSuccess && Cliffs && Cliffs->MinSlopeDegrees == 50.0
		&& Cliffs->MaxSlopeDegrees == 85.0 && Cliffs->Alignment == EMantleScatterAlignment::Ground && Cliffs->MaxTiltDegrees == 5.0);

	Result = ParseScatter(TEXT(",\"placement\":\"terrain\",\"minSlopeDegrees\":60,\"maxSlopeDegrees\":60"));
	TestTrue(TEXT("a minimum equal to the maximum parses"), Result.bSuccess);
	Result = ParseScatter(TEXT(",\"placement\":\"terrain\",\"minSlopeDegrees\":30"));
	TestTrue(TEXT("a minimum alone parses"), Result.bSuccess && ScatterOf(Result) && ScatterOf(Result)->MaxSlopeDegrees == 90.0);

	TestTrue(TEXT("a minimum slope on box placement is refused"), HasErrorAt(ParseScatter(TEXT(",\"minSlopeDegrees\":30")), TEXT("minSlopeDegrees")));
	TestTrue(TEXT("ground alignment on box placement is refused"), HasErrorAt(ParseScatter(TEXT(",\"alignment\":\"ground\"")), TEXT("alignment")));
	TestTrue(TEXT("ground alignment on an explicit box is refused"),
		HasErrorAt(ParseScatter(TEXT(",\"placement\":\"box\",\"alignment\":\"ground\"")), TEXT("alignment")));
	TestTrue(TEXT("a minimum past 90 is refused"), HasErrorAt(ParseScatter(TEXT(",\"placement\":\"terrain\",\"minSlopeDegrees\":91")), TEXT("minSlopeDegrees")));
	TestTrue(TEXT("a negative minimum is refused"), HasErrorAt(ParseScatter(TEXT(",\"placement\":\"terrain\",\"minSlopeDegrees\":-1")), TEXT("minSlopeDegrees")));
	TestTrue(TEXT("a minimum above the maximum is refused at the maximum"),
		HasErrorAt(ParseScatter(TEXT(",\"placement\":\"terrain\",\"minSlopeDegrees\":60,\"maxSlopeDegrees\":30")), TEXT("maxSlopeDegrees")));
	TestTrue(TEXT("an unknown alignment is refused"), HasErrorAt(ParseScatter(TEXT(",\"alignment\":\"sideways\"")), TEXT("alignment")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
