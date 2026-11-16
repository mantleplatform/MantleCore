// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHeightfieldReader.h"
#include "MantleTerrainBuilder.h"

#include "MeshDescription.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Terrain tiles (stage 4): which Terrains touch which, the normal that reads across a seam, and the skirt that hangs from a shared border. Pure data: no world, no network.
// A tile is a Terrain that shares its border samples with the next one, so each test cuts one made-up surface into pieces and compares the pieces with the whole.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleTerrainTilesTestPrivate
{
	/** A Width x Height field at 100 cm spacing and 1 cm per height unit whose sample (X, Y) is Surface(X, Y), where (X, Y) is counted from (OffsetX, OffsetY) in a bigger surface. */
	TSharedPtr<FMantleHeightfield> MakeTile(const int32 Width, const int32 Height, const int32 OffsetX, const int32 OffsetY, TFunctionRef<int32(int32, int32)> Surface)
	{
		TSharedPtr<FMantleHeightfield> Field = MakeShared<FMantleHeightfield>();
		Field->Width = Width;
		Field->Height = Height;
		Field->SpacingX = 100.0f;
		Field->SpacingY = 100.0f;
		Field->HeightScale = 1.0f;
		Field->HeightOffset = 0.0f;
		Field->Heights.SetNumUninitialized(Width * Height);
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				Field->Heights[Y * Width + X] = static_cast<uint16>(Surface(OffsetX + X, OffsetY + Y));
			}
		}
		return Field;
	}

	FMantleTerrainPlacement Place(const TSharedPtr<FMantleHeightfield>& Field, const FVector& Location, const double YawDegrees = 0.0)
	{
		FMantleTerrainPlacement Placement;
		Placement.World = FTransform(FRotator(0.0, YawDegrees, 0.0), Location);
		Placement.Field = Field;
		return Placement;
	}

	int32 Triangles(const FMantleTerrainPreparedChunk& Chunk, const int32 Lod)
	{
		return Chunk.Descriptions.IsValidIndex(Lod) ? Chunk.Descriptions[Lod].Triangles().Num() : -1;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainTilesAdjacencyTest,
	"Mantle.Runtime.TerrainTiles.Adjacency",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainTilesAdjacencyTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainTilesTestPrivate;

	// One surface g(X, Y) = X * X + 3 * Y cut into 5 x 5 tiles of 4 quads (400 cm) that share their border samples: A at the origin, B to its right, C above it, D above B.
	const auto Surface = [](const int32 X, const int32 Y) { return X * X + 3 * Y; };
	const TSharedPtr<FMantleHeightfield> A = MakeTile(5, 5, 0, 0, Surface);
	const TSharedPtr<FMantleHeightfield> B = MakeTile(5, 5, 4, 0, Surface);
	const TSharedPtr<FMantleHeightfield> C = MakeTile(5, 5, 0, 4, Surface);
	const TSharedPtr<FMantleHeightfield> D = MakeTile(5, 5, 4, 4, Surface);

	TArray<FMantleTerrainEdgeNeighbors> Out;
	{
		const TArray<FMantleTerrainPlacement> Grid = { Place(A, FVector(0, 0, 0)), Place(B, FVector(400, 0, 0)), Place(C, FVector(0, 400, 0)), Place(D, FVector(400, 400, 0)) };
		FMantleTerrainAdjacency::Find(Grid, Out);
		if (!TestEqual(TEXT("one entry per terrain"), Out.Num(), 4))
		{
			return false;
		}
		// A: B on the right, C above. B: A on the left, D above. C: D on the right, A below. D: C on the left, B below. No diagonal.
		TestTrue(TEXT("A has B on its right"), Out[0].Field[1].Get() == B.Get());
		TestTrue(TEXT("A has C above"), Out[0].Field[3].Get() == C.Get());
		TestFalse(TEXT("A has nothing on its left"), Out[0].Has(0));
		TestFalse(TEXT("A has nothing below"), Out[0].Has(2));
		TestTrue(TEXT("B has A on its left"), Out[1].Field[0].Get() == A.Get());
		TestTrue(TEXT("B has D above"), Out[1].Field[3].Get() == D.Get());
		TestFalse(TEXT("B has nothing on its right"), Out[1].Has(1));
		TestTrue(TEXT("C has D on its right"), Out[2].Field[1].Get() == D.Get());
		TestTrue(TEXT("C has A below"), Out[2].Field[2].Get() == A.Get());
		TestTrue(TEXT("D has C on its left"), Out[3].Field[0].Get() == C.Get());
		TestTrue(TEXT("D has B below"), Out[3].Field[2].Get() == B.Get());
		TestFalse(TEXT("D has nothing on its right"), Out[3].Has(1));
		TestFalse(TEXT("D has nothing above"), Out[3].Has(3));
	}
	{
		// A gap of 5 cm is no seam; half a cm is (the tolerance is 1 cm); the order in the list does not matter.
		const TArray<FMantleTerrainPlacement> Gap = { Place(B, FVector(405, 0, 0)), Place(A, FVector(0, 0, 0)) };
		FMantleTerrainAdjacency::Find(Gap, Out);
		TestFalse(TEXT("5 cm apart does not touch"), Out[0].HasAny() || Out[1].HasAny());
		const TArray<FMantleTerrainPlacement> Near = { Place(B, FVector(400.5, 0, 0)), Place(A, FVector(0, 0, 0)) };
		FMantleTerrainAdjacency::Find(Near, Out);
		TestTrue(TEXT("half a cm apart touches: A is B's left"), Out[0].Field[0].Get() == A.Get());
		TestTrue(TEXT("and B is A's right"), Out[1].Field[1].Get() == B.Get());
	}
	{
		// Borders at different heights are no seam: B raised 5 cm in the world, or B's own heights 5 units higher along the border.
		const TArray<FMantleTerrainPlacement> Raised = { Place(A, FVector(0, 0, 0)), Place(B, FVector(400, 0, 5)) };
		FMantleTerrainAdjacency::Find(Raised, Out);
		TestFalse(TEXT("a step of 5 cm at the border is no seam"), Out[0].HasAny());
		const TSharedPtr<FMantleHeightfield> Stepped = MakeTile(5, 5, 4, 0, [](const int32 X, const int32 Y) { return X * X + 3 * Y + 5; });
		const TArray<FMantleTerrainPlacement> Step = { Place(A, FVector(0, 0, 0)), Place(Stepped, FVector(400, 0, 0)) };
		FMantleTerrainAdjacency::Find(Step, Out);
		TestFalse(TEXT("heights 5 cm higher along the border are no seam"), Out[0].HasAny());
	}
	{
		// A border with a different number of samples is no seam.
		const TSharedPtr<FMantleHeightfield> Taller = MakeTile(5, 6, 4, 0, Surface);
		const TArray<FMantleTerrainPlacement> Mismatch = { Place(A, FVector(0, 0, 0)), Place(Taller, FVector(400, 0, 0)) };
		FMantleTerrainAdjacency::Find(Mismatch, Out);
		TestFalse(TEXT("5 samples against 6 along the border is no seam"), Out[0].HasAny());
	}
	{
		// Turned a quarter turn about Z, the pair still meets: B's origin is A's right border turned with it, (0, 400). Turned differently, it does not.
		const TArray<FMantleTerrainPlacement> Turned = { Place(A, FVector(0, 0, 0), 90.0), Place(B, FVector(0, 400, 0), 90.0) };
		FMantleTerrainAdjacency::Find(Turned, Out);
		TestTrue(TEXT("both turned 90 degrees still meet"), Out[0].Field[1].Get() == B.Get() && Out[1].Field[0].Get() == A.Get());
		const TArray<FMantleTerrainPlacement> Mixed = { Place(A, FVector(0, 0, 0), 90.0), Place(B, FVector(0, 400, 0), 0.0) };
		FMantleTerrainAdjacency::Find(Mixed, Out);
		TestFalse(TEXT("turned differently does not"), Out[0].HasAny() || Out[1].HasAny());
	}
	{
		// Border points that coincide in the world are not enough: the two frames must agree too, or the builder would read the neighbor's heights and spacing wrongly.
		// (a) B stands 100 cm higher with a height offset of -100: every border sample is where A's is, but B's local heights are 100 lower than A's.
		const TSharedPtr<FMantleHeightfield> Lowered = MakeTile(5, 5, 4, 0, Surface);
		Lowered->HeightOffset = -100.0f;
		const TArray<FMantleTerrainPlacement> Offset = { Place(A, FVector(0, 0, 0)), Place(Lowered, FVector(400, 0, 100)) };
		FMantleTerrainAdjacency::Find(Offset, Out);
		TestFalse(TEXT("(a) a step in height between the origins, made up by the height offset, is no seam"), Out[0].HasAny() || Out[1].HasAny());
		// (b) B has 50 cm between samples across the border (and 100 along it): the border points still coincide, but the two slopes would not mean the same.
		const TSharedPtr<FMantleHeightfield> Finer = MakeTile(5, 5, 4, 0, Surface);
		Finer->SpacingX = 50.0f;
		const TArray<FMantleTerrainPlacement> Spacing = { Place(A, FVector(0, 0, 0)), Place(Finer, FVector(400, 0, 0)) };
		FMantleTerrainAdjacency::Find(Spacing, Out);
		TestFalse(TEXT("(b) 50 cm against 100 cm across the border is no seam"), Out[0].HasAny() || Out[1].HasAny());
		// (c) Flat ground, B turned 10 degrees about its own left border line (the Y axis through its origin): the border points coincide, the surfaces do not continue.
		const auto Flat = [](const int32, const int32) { return 0; };
		const TSharedPtr<FMantleHeightfield> FlatA = MakeTile(5, 5, 0, 0, Flat);
		const TSharedPtr<FMantleHeightfield> FlatB = MakeTile(5, 5, 4, 0, Flat);
		FMantleTerrainPlacement Tilted = Place(FlatB, FVector(400, 0, 0));
		Tilted.World = FTransform(FRotator(10.0, 0.0, 0.0), FVector(400, 0, 0));
		const TArray<FMantleTerrainPlacement> Tilt = { Place(FlatA, FVector(0, 0, 0)), Tilted };
		FMantleTerrainAdjacency::Find(Tilt, Out);
		TestFalse(TEXT("(c) turned about the border line is no seam"), Out[0].HasAny() || Out[1].HasAny());
		// The same pair untilted is a seam, so (c) tests the turn and nothing else.
		const TArray<FMantleTerrainPlacement> Level = { Place(FlatA, FVector(0, 0, 0)), Place(FlatB, FVector(400, 0, 0)) };
		FMantleTerrainAdjacency::Find(Level, Out);
		TestTrue(TEXT("the same flat pair, not turned, is a seam"), Out[0].Field[1].Get() == FlatB.Get() && Out[1].Field[0].Get() == FlatA.Get());
	}
	{
		// A terrain with no field, and a lone terrain, touch nothing.
		const TArray<FMantleTerrainPlacement> Missing = { Place(A, FVector(0, 0, 0)), Place(nullptr, FVector(400, 0, 0)) };
		FMantleTerrainAdjacency::Find(Missing, Out);
		TestEqual(TEXT("two entries"), Out.Num(), 2);
		TestFalse(TEXT("a missing field touches nothing"), Out[0].HasAny() || Out[1].HasAny());
		FMantleTerrainAdjacency::Find(TArray<FMantleTerrainPlacement>(), Out);
		TestEqual(TEXT("no terrains, no entries"), Out.Num(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainTilesNormalTest,
	"Mantle.Runtime.TerrainTiles.NormalAcrossSeam",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainTilesNormalTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainTilesTestPrivate;

	// Heights x * x (cm) along X, 100 cm between samples. The whole surface has 9 samples across; left tile L is samples 0 to 4, right tile R samples 4 to 8 (sample 4 is in both).
	const auto Parabola = [](const int32 X, const int32) { return X * X; };
	const TSharedPtr<FMantleHeightfield> Left = MakeTile(5, 3, 0, 0, Parabola);
	const TSharedPtr<FMantleHeightfield> Right = MakeTile(5, 3, 4, 0, Parabola);
	const TSharedPtr<FMantleHeightfield> Whole = MakeTile(9, 3, 0, 0, Parabola);

	FMantleTerrainEdgeNeighbors RightEdges;
	RightEdges.Field[0] = Left;
	FMantleTerrainEdgeNeighbors LeftEdges;
	LeftEdges.Field[1] = Right;

	const auto ExpectNormal = [this](const TCHAR* What, const FVector3f& Actual, const float DzDx, const float DzDy)
	{
		const FVector3f Expected = FVector3f(-DzDx, -DzDy, 1.0f).GetSafeNormal();
		TestNearlyEqual(*FString::Printf(TEXT("%s: x"), What), Actual.X, Expected.X, 1e-5f);
		TestNearlyEqual(*FString::Printf(TEXT("%s: y"), What), Actual.Y, Expected.Y, 1e-5f);
		TestNearlyEqual(*FString::Printf(TEXT("%s: z"), What), Actual.Z, Expected.Z, 1e-5f);
	};

	// The whole surface at sample 4: (5 * 5 - 3 * 3) / (2 * 100) = 0.08, no slope along Y.
	ExpectNormal(TEXT("the whole surface at the seam"), FMantleTerrainBuilder::SampleNormal(*Whole, 4, 1), 0.08f, 0.0f);
	// R's sample 0 is that sample: with L beside it the normal is the same; with no neighbor it is one-sided, (25 - 16) / 100 = 0.09.
	ExpectNormal(TEXT("R's first column with L beside it"), FMantleTerrainBuilder::SampleNormal(*Right, 0, 1, &RightEdges), 0.08f, 0.0f);
	ExpectNormal(TEXT("R's first column alone"), FMantleTerrainBuilder::SampleNormal(*Right, 0, 1), 0.09f, 0.0f);
	// L's last column is the same sample from the other side: R's sample 1 (25) against L's sample 3 (9).
	ExpectNormal(TEXT("L's last column with R beside it"), FMantleTerrainBuilder::SampleNormal(*Left, 4, 1, &LeftEdges), 0.08f, 0.0f);
	ExpectNormal(TEXT("L's last column alone: (16 - 9) / 100"), FMantleTerrainBuilder::SampleNormal(*Left, 4, 1), 0.07f, 0.0f);
	// An interior sample does not read the neighbor: R's sample 2 is surface sample 6, (49 - 25) / 200 = 0.12.
	ExpectNormal(TEXT("an interior sample with edges given"), FMantleTerrainBuilder::SampleNormal(*Right, 2, 1, &RightEdges), 0.12f, 0.0f);
	ExpectNormal(TEXT("an interior sample, no edges"), FMantleTerrainBuilder::SampleNormal(*Right, 2, 1), 0.12f, 0.0f);
	// A side that has no neighbor stays one-sided even when another side has one: R's corner (0, 0) has L, nothing below, so the slope along Y is 0 and along X reads L.
	ExpectNormal(TEXT("R's corner: L beside it, nothing below"), FMantleTerrainBuilder::SampleNormal(*Right, 0, 0, &RightEdges), 0.08f, 0.0f);

	// The same across a horizontal seam: heights y * y, bottom tile samples 0 to 4 in Y, top tile 4 to 8. The top tile's bottom row with the bottom tile below it reads (25 - 9) / 200 = 0.08 along Y.
	const auto Ridge = [](const int32, const int32 Y) { return Y * Y; };
	const TSharedPtr<FMantleHeightfield> Bottom = MakeTile(3, 5, 0, 0, Ridge);
	const TSharedPtr<FMantleHeightfield> Top = MakeTile(3, 5, 0, 4, Ridge);
	FMantleTerrainEdgeNeighbors TopEdges;
	TopEdges.Field[2] = Bottom;
	FMantleTerrainEdgeNeighbors BottomEdges;
	BottomEdges.Field[3] = Top;
	ExpectNormal(TEXT("the top tile's bottom row with the bottom tile below it"), FMantleTerrainBuilder::SampleNormal(*Top, 1, 0, &TopEdges), 0.0f, 0.08f);
	ExpectNormal(TEXT("the top tile's bottom row alone"), FMantleTerrainBuilder::SampleNormal(*Top, 1, 0), 0.0f, 0.09f);
	ExpectNormal(TEXT("the bottom tile's top row with the top tile above it"), FMantleTerrainBuilder::SampleNormal(*Bottom, 1, 4, &BottomEdges), 0.0f, 0.08f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainTilesSkirtTest,
	"Mantle.Runtime.TerrainTiles.SkirtOnSharedBorder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainTilesSkirtTest::RunTest(const FString& Parameters)
{
	using namespace MantleTerrainTilesTestPrivate;

	// One chunk of 16 quads (17 samples a side), 5 LODs (steps 1, 2, 4, 8, 16). With no neighbor a lone chunk has no skirt: LOD 0 is 2 * 16 * 16 = 512 triangles, LOD 1 is 2 * 8 * 8 = 128.
	// A skirt is 4 triangles a segment of the border (two quads, one for each side): LOD 0 has 16 segments on a side, LOD 1 has 8.
	const TSharedPtr<FMantleHeightfield> Field = MakeTile(17, 17, 0, 0, [](const int32 X, const int32 Y) { return 7 * X + 3 * Y + (X * Y) % 5; });
	const TSharedPtr<FMantleHeightfield> Other = MakeTile(17, 17, 16, 0, [](const int32 X, const int32 Y) { return 7 * X + 3 * Y + (X * Y) % 5; });

	FMantleTerrainBuildParams Params;
	Params.ChunkQuads = 16;
	Params.MaxLODs = 5;

	FMantleTerrainPreparedChunk Alone;
	FMantleTerrainBuilder::PrepareChunk(*Field, Params, 0, 0, EMantleTerrainChunkDetail::Full, Alone);
	if (!TestEqual(TEXT("five LODs"), Alone.Descriptions.Num(), 5))
	{
		return false;
	}
	TestEqual(TEXT("alone: LOD 0 is 512 triangles"), Triangles(Alone, 0), 512);
	TestEqual(TEXT("alone: LOD 1 is 128"), Triangles(Alone, 1), 128);

	FMantleTerrainEdgeNeighbors Right;
	Right.Field[1] = Other;
	Params.Neighbors = MakeShared<FMantleTerrainEdgeNeighbors>(Right);
	FMantleTerrainPreparedChunk WithRight;
	FMantleTerrainBuilder::PrepareChunk(*Field, Params, 0, 0, EMantleTerrainChunkDetail::Full, WithRight);
	TestEqual(TEXT("a neighbor on the right: LOD 0 gains 16 x 4 = 64 skirt triangles"), Triangles(WithRight, 0), 512 + 64);
	TestEqual(TEXT("LOD 1 gains 8 x 4 = 32"), Triangles(WithRight, 1), 128 + 32);

	FMantleTerrainEdgeNeighbors Three;
	Three.Field[0] = Other;
	Three.Field[1] = Other;
	Three.Field[3] = Other;
	Params.Neighbors = MakeShared<FMantleTerrainEdgeNeighbors>(Three);
	FMantleTerrainPreparedChunk WithThree;
	FMantleTerrainBuilder::PrepareChunk(*Field, Params, 0, 0, EMantleTerrainChunkDetail::Full, WithThree);
	TestEqual(TEXT("neighbors left, right and above: three skirts of 64"), Triangles(WithThree, 0), 512 + 3 * 64);

	// The far chunk (the coarsest LOD that has a quad: step 16, one quad = 2 triangles) gets the same skirts: one segment a side, 4 triangles.
	FMantleTerrainPreparedChunk FarAlone;
	Params.Neighbors.Reset();
	FMantleTerrainBuilder::PrepareChunk(*Field, Params, 0, 0, EMantleTerrainChunkDetail::Far, FarAlone);
	TestEqual(TEXT("far chunk alone: 2 triangles"), Triangles(FarAlone, 0), 2);
	FarAlone = FMantleTerrainPreparedChunk();
	Params.Neighbors = MakeShared<FMantleTerrainEdgeNeighbors>(Right);
	FMantleTerrainBuilder::PrepareChunk(*Field, Params, 0, 0, EMantleTerrainChunkDetail::Far, FarAlone);
	TestEqual(TEXT("far chunk with a neighbor on the right: 2 + 4"), Triangles(FarAlone, 0), 6);

	// A terrain's own chunks meet as before: a 33 by 17 sample terrain of two chunks, no neighbors, has a skirt on the inner edge only.
	const TSharedPtr<FMantleHeightfield> Two = MakeTile(33, 17, 0, 0, [](const int32 X, const int32 Y) { return 7 * X + 3 * Y; });
	Params.Neighbors.Reset();
	FMantleTerrainPreparedChunk FirstOfTwo;
	FMantleTerrainBuilder::PrepareChunk(*Two, Params, 0, 0, EMantleTerrainChunkDetail::Full, FirstOfTwo);
	TestEqual(TEXT("the first of two chunks has a skirt on its right edge only: 512 + 64"), Triangles(FirstOfTwo, 0), 576);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
