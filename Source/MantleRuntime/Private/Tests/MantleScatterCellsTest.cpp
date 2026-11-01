// Copyright Joshua Eger. All Rights Reserved.
#include "MantleRuntime.h"
#include "MantlePlaceLimits.h"
#include "MantleScatter.h"
#include "MantleScatterCells.h"
#include "MantleScatterVolume.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Scatter cells (#189): a Scatter of more than 200,000 instances is read cell by cell. A cell built alone must be exactly the whole stream's
// instances inside it (spacing included, across a border), in any order, with the Foliage tier's prefix a subset, and a server's queries
// (a box, the whole hash) must give what a whole build gives.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleScatterCellsTestPrivate
{
	/** 400 m x 400 m at density 0.5 is 80,000 candidates: a handful of cells, fast to read whole. */
	FMantleScatterParams CellParams()
	{
		FMantleScatterParams P;
		P.Seed = 189;
		P.HalfExtent = FVector(20000.0, 20000.0, 0.0);
		P.Density = 0.5;
		P.MinScale = 0.7;
		P.MaxScale = 1.4;
		P.Alignment = EMantleScatterAlignment::Tilted;
		P.MaxTiltDegrees = 9.0;
		P.DrawDistance = 5000.0;
		P.Meshes = { { TEXT("ast_cone"), 4, {} }, { TEXT("ast_sphere"), 2, {} }, { TEXT("ast_cube"), 1, {} } };
		return P;
	}

	/** A sloped, bumpy ground with a hole and a banded paint layer, all functions of X-Y so any thread gets the same answer. */
	FMantleScatterGroundFn BumpyGround()
	{
		return [](const int64 XMm, const int64 YMm, FMantleScatterGround& Out) -> bool
		{
			// A hole 40 m across, 100 m east of the centre.
			if (XMm > 1000000 && XMm < 1400000 && FMath::Abs(YMm) < 200000)
			{
				return false;
			}
			const double X = static_cast<double>(XMm) * 0.1;
			const double Y = static_cast<double>(YMm) * 0.1;
			const double Height = 0.15 * X + 80.0 * FMath::Sin(X * 0.002) + 50.0 * FMath::Cos(Y * 0.003);
			// The slope of that height: dz/dx and dz/dy, as a unit normal.
			const double Dx = 0.15 + 80.0 * 0.002 * FMath::Cos(X * 0.002);
			const double Dy = -50.0 * 0.003 * FMath::Sin(Y * 0.003);
			const FVector Normal = FVector(-Dx, -Dy, 1.0).GetSafeNormal();
			Out.ZMm = static_cast<int64>(FMath::FloorToDouble(Height * 10.0 + 0.5));
			Out.WorldZCm = Height;
			Out.NormalZ = Normal.Z;
			Out.LocalNormalX = Normal.X;
			Out.LocalNormalY = Normal.Y;
			Out.LocalNormalZ = Normal.Z;
			Out.LayerWeight = static_cast<int32>((XMm / 800) & 255);
			return true;
		};
	}

	/** The instances of Whole that lie in Cell of Index, in order. */
	TArray<FMantleScatterInstance> InCell(const TArray<FMantleScatterInstance>& All, const FMantleScatterCellIndex& Index, const FIntPoint& Cell)
	{
		TArray<FMantleScatterInstance> Out;
		for (const FMantleScatterInstance& Instance : All)
		{
			if (Index.CellAt(Instance.X, Instance.Y) == Cell)
			{
				Out.Add(Instance);
			}
		}
		return Out;
	}

	/** Whether Subset's instances all appear in Full, in order. */
	bool IsOrderedSubset(const TArray<FMantleScatterInstance>& Subset, const TArray<FMantleScatterInstance>& Full)
	{
		int32 At = 0;
		for (const FMantleScatterInstance& Instance : Subset)
		{
			while (At < Full.Num() && Full[At] != Instance)
			{
				++At;
			}
			if (At >= Full.Num())
			{
				return false;
			}
			++At;
		}
		return true;
	}

	/**
	 * Reads every cell of Params alone, forwards and then backwards (each with a fresh read, as two machines would), and holds each to the whole
	 * stream's instances inside it. Also the sum, the parallel whole read and the hash. Returns the whole stream's count.
	 */
	int32 CheckCellsMatchWhole(FAutomationTestBase& Test, const TCHAR* Name, const FMantleScatterParams& P, const FMantleScatterGenerateOptions& Options)
	{
		FString Error;
		TArray<FMantleScatterInstance> All;
		if (!Test.TestTrue(*FString::Printf(TEXT("%s: whole generates (%s)"), Name, *Error), FMantleScatter::Generate(P, All, Error, Options)))
		{
			return 0;
		}
		const TSharedPtr<const FMantleScatterCellIndex> Index = FMantleScatter::BuildCellIndex(P, Error);
		if (!Test.TestTrue(*FString::Printf(TEXT("%s: index builds (%s)"), Name, *Error), Index.IsValid()))
		{
			return 0;
		}
		Test.TestTrue(*FString::Printf(TEXT("%s: the cell side is at least the spacing"), Name),
			Index->CellMm >= static_cast<int64>(FMath::RoundToInt64(P.MinSpacing * 10.0)));

		int32 Total = 0;
		int32 Wrong = 0;
		TArray<TArray<FMantleScatterInstance>> Forwards;
		Forwards.SetNum(Index->NumCells());
		for (int32 Number = 0; Number < Index->NumCells(); ++Number)
		{
			const FIntPoint Cell(Number % Index->CellsX, Number / Index->CellsX);
			if (!FMantleScatter::GenerateCell(P, *Index, Cell, Forwards[Number], Error, Options))
			{
				Test.AddError(FString::Printf(TEXT("%s: cell %d,%d did not generate: %s"), Name, Cell.X, Cell.Y, *Error));
				return 0;
			}
			if (Forwards[Number] != InCell(All, *Index, Cell))
			{
				++Wrong;
			}
			Total += Forwards[Number].Num();
		}
		Test.TestEqual(*FString::Printf(TEXT("%s: cells that differ from the same region of the whole build"), Name), Wrong, 0);
		Test.TestEqual(*FString::Printf(TEXT("%s: the cells hold every instance, once"), Name), Total, All.Num());

		// Backwards, each on a freshly built index: the same cells, whatever was read before.
		const TSharedPtr<const FMantleScatterCellIndex> Fresh = FMantleScatter::BuildCellIndex(P, Error);
		int32 Different = 0;
		for (int32 Number = Fresh->NumCells() - 1; Number >= 0; --Number)
		{
			TArray<FMantleScatterInstance> Again;
			FMantleScatter::GenerateCell(P, *Fresh, FIntPoint(Number % Fresh->CellsX, Number / Fresh->CellsX), Again, Error, Options);
			Different += Again != Forwards[Number] ? 1 : 0;
		}
		Test.TestEqual(*FString::Printf(TEXT("%s: cells built in the other order differ"), Name), Different, 0);

		TArray<FMantleScatterInstance> ByCells;
		FMantleScatter::GenerateAllByCells(P, *Index, ByCells, Error, Options);
		Test.TestEqual(*FString::Printf(TEXT("%s: the parallel read is the whole stream (hash)"), Name),
			FMantleScatter::HashText(FMantleScatter::Hash(ByCells)), FMantleScatter::HashText(FMantleScatter::Hash(All)));
		return All.Num();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterCellsMatchWholeTest,
	"Mantle.Runtime.Scatter.Cells.EachCellIsTheWholeBuildsRegion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterCellsMatchWholeTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterCellsTestPrivate;

	FMantleScatterGenerateOptions None;
	FMantleScatterGenerateOptions OnGround;
	OnGround.Ground = BumpyGround();

	const int32 Plain = CheckCellsMatchWhole(*this, TEXT("box"), CellParams(), None);
	TestTrue(TEXT("a box of 80,000 candidates keeps most"), Plain > 79000);

	FMantleScatterParams Excluded = CellParams();
	Excluded.Exclusions = { { FVector2D(-5000.0, 3000.0), FVector2D(2500.0, 1500.0) } };
	Excluded.PathExclusions = { { { FVector2D(-20000.0, -20000.0), FVector2D(20000.0, 20000.0) }, 800.0 } };
	const int32 WithHoles = CheckCellsMatchWhole(*this, TEXT("exclusions"), Excluded, None);
	TestTrue(TEXT("exclusions drop some"), WithHoles < Plain);

	FMantleScatterParams Disc = CellParams();
	Disc.Shape = EMantleScatterShape::Disc;
	Disc.Radius = 18000.0;
	CheckCellsMatchWhole(*this, TEXT("disc"), Disc, None);

	FMantleScatterParams Path = CellParams();
	Path.Shape = EMantleScatterShape::Path;
	Path.Path = { FVector2D(-18000.0, -9000.0), FVector2D(0.0, 6000.0), FVector2D(18000.0, -2000.0) };
	Path.Width = 6000.0;
	Path.Density = 1.0;
	CheckCellsMatchWhole(*this, TEXT("path"), Path, None);

	FMantleScatterParams Clustered = CellParams();
	Clustered.ClusterCount = 6;
	Clustered.ClusterRadius = 3000.0;
	CheckCellsMatchWhole(*this, TEXT("clusters"), Clustered, None);

	FMantleScatterParams Ground = CellParams();
	Ground.Placement = EMantleScatterPlacement::Terrain;
	Ground.Alignment = EMantleScatterAlignment::Ground;
	Ground.MaxSlopeDegrees = 30.0;
	Ground.MinSlopeDegrees = 2.0;
	Ground.MinHeight = -500.0;
	Ground.PaintLayer = 0;
	Ground.MinLayerWeight = 0.2;
	Ground.MaxLayerWeight = 0.9;
	const int32 OnTerrain = CheckCellsMatchWhole(*this, TEXT("terrain"), Ground, OnGround);
	TestTrue(*FString::Printf(TEXT("the terrain filters drop some and keep some (%d of %d)"), OnTerrain, Plain), OnTerrain > 1000 && OnTerrain < Plain);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterCellsSpacingTest,
	"Mantle.Runtime.Scatter.Cells.SpacingIsExactAcrossBorders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterCellsSpacingTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterCellsTestPrivate;

	FMantleScatterGenerateOptions None;
	FMantleScatterGenerateOptions OnGround;
	OnGround.Ground = BumpyGround();

	// Far more candidates than the spacing leaves room for, so every cell's border is crowded and a kept instance's fate hangs on a chain of
	// neighbours through several cells: the case a per-cell spacing pass gets wrong.
	for (const double Spacing : { 150.0, 600.0, 2500.0, 15000.0 })
	{
		FMantleScatterParams P = CellParams();
		P.MinSpacing = Spacing;
		P.Density = Spacing > 10000.0 ? 0.25 : 0.5;
		const FString Name = FString::Printf(TEXT("spacing %g cm"), Spacing);
		const int32 Kept = CheckCellsMatchWhole(*this, *Name, P, None);

		// And what the spacing means: no two kept instances closer than it, exactly this apart allowed.
		FString Error;
		TArray<FMantleScatterInstance> All;
		FMantleScatter::Generate(P, All, Error, None);
		const int64 SpacingMm = FMath::RoundToInt64(Spacing * 10.0);
		TMap<uint64, TArray<int32>> Grid;
		int32 TooClose = 0;
		for (int32 At = 0; At < All.Num(); ++At)
		{
			const int64 Gx = FMath::FloorToInt64(static_cast<double>(All[At].X) / static_cast<double>(SpacingMm));
			const int64 Gy = FMath::FloorToInt64(static_cast<double>(All[At].Y) / static_cast<double>(SpacingMm));
			for (int64 Sx = -1; Sx <= 1; ++Sx)
			{
				for (int64 Sy = -1; Sy <= 1; ++Sy)
				{
					if (const TArray<int32>* Cell = Grid.Find((static_cast<uint64>(static_cast<uint32>(Gx + Sx)) << 32) | static_cast<uint32>(Gy + Sy)))
					{
						for (const int32 Other : *Cell)
						{
							const int64 Dx = All[At].X - All[Other].X;
							const int64 Dy = All[At].Y - All[Other].Y;
							TooClose += Dx * Dx + Dy * Dy < SpacingMm * SpacingMm ? 1 : 0;
						}
					}
				}
			}
			Grid.FindOrAdd((static_cast<uint64>(static_cast<uint32>(Gx)) << 32) | static_cast<uint32>(Gy)).Add(At);
		}
		TestEqual(*FString::Printf(TEXT("%s: pairs closer than the spacing"), *Name), TooClose, 0);
		TestTrue(*FString::Printf(TEXT("%s: the spacing thins the field (%d kept)"), *Name, Kept), Kept > 1 && Kept < 80000);
	}

	// The same on terrain with a hole and the other filters: an instance a filter dropped never keeps its neighbours away, in a cell or out of it.
	FMantleScatterParams Ground = CellParams();
	Ground.Placement = EMantleScatterPlacement::Terrain;
	Ground.MaxSlopeDegrees = 25.0;
	Ground.MinSpacing = 800.0;
	CheckCellsMatchWhole(*this, TEXT("terrain with spacing"), Ground, OnGround);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterCellsThinningTest,
	"Mantle.Runtime.Scatter.Cells.WideSpacingIsThinned",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterCellsThinningTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterCellsTestPrivate;

	FMantleScatterGenerateOptions None;
	FString Error;
	const int32 Target = FMantlePlaceLimits::ScatterSpacingCandidates;

	// 320,000 candidates (400 m x 400 m at density 2) with a spacing of 50 m: 5,000 candidates a spacing square, thinned to about 64 of them, which
	// is 160,000 / 2,500 squares x 64 = 4,096 in all.
	FMantleScatterParams Wide = CellParams();
	Wide.Density = 2.0;
	Wide.MinSpacing = 5000.0;
	TestEqual(TEXT("the Scatter counts, it is not refused"), FMantleScatter::CountFor(Wide, Error), static_cast<int64>(320000));
	const TSharedPtr<const FMantleScatterCellIndex> WideIndex = FMantleScatter::BuildCellIndex(Wide, Error);
	if (!TestTrue(*FString::Printf(TEXT("its index builds (%s)"), *Error), WideIndex.IsValid()))
	{
		return false;
	}
	TestTrue(*FString::Printf(TEXT("only about %d candidates a spacing square stay (%d of 320,000)"), Target, WideIndex->NumSlots()),
		WideIndex->NumSlots() > 2800 && WideIndex->NumSlots() < 5600);
	// Still a deterministic field: a cell alone is the whole build's region, in any order, and the parallel read is the whole stream.
	const int32 Thinned = CheckCellsMatchWhole(*this, TEXT("thinned wide spacing"), Wide, None);
	// 160,000 m2 holds about 45 discs of a 50 m spacing packed at random, and 64 candidates a square is enough to reach that.
	TestTrue(*FString::Printf(TEXT("the spacing still leaves a field (%d)"), Thinned), Thinned > 20 && Thinned < 120);

	// The same Scatter with a spacing it holds comfortably (150 cm is 4.5 a square) is not thinned: every candidate is there.
	FMantleScatterParams Close = Wide;
	Close.MinSpacing = 150.0;
	TSharedPtr<const FMantleScatterCellIndex> CloseIndex = FMantleScatter::BuildCellIndex(Close, Error);
	TestTrue(TEXT("a close spacing keeps every candidate"), CloseIndex.IsValid() && CloseIndex->NumSlots() == 320000);
	// And one built whole (up to 200,000) is never thinned, however wide the spacing.
	FMantleScatterParams Small = CellParams();
	Small.MinSpacing = 5000.0;
	TSharedPtr<const FMantleScatterCellIndex> SmallIndex = FMantleScatter::BuildCellIndex(Small, Error);
	TestTrue(TEXT("a Scatter built whole is not thinned"), SmallIndex.IsValid() && SmallIndex->NumSlots() == 80000);

	// The review's case: a 1 km box at density 2 with a spacing of 1 km. Every read stays small and the whole stream is the same as a whole build's.
	FMantleScatterParams Huge = CellParams();
	Huge.HalfExtent = FVector(50000.0, 50000.0, 0.0);
	Huge.Density = 2.0;
	Huge.MinSpacing = 100000.0;
	Huge.DrawDistance = 10000.0;
	TestEqual(TEXT("2,000,000 candidates count"), FMantleScatter::CountFor(Huge, Error), static_cast<int64>(2000000));
	const double Start = FPlatformTime::Seconds();
	const TSharedPtr<const FMantleScatterCellIndex> HugeIndex = FMantleScatter::BuildCellIndex(Huge, Error);
	if (!TestTrue(TEXT("the huge index builds"), HugeIndex.IsValid()))
	{
		return false;
	}
	TArray<FMantleScatterInstance> ByCells;
	TestTrue(TEXT("every cell reads"), FMantleScatter::GenerateAllByCells(Huge, *HugeIndex, ByCells, Error, None));
	const double Seconds = FPlatformTime::Seconds() - Start;
	TArray<FMantleScatterInstance> Whole;
	TestTrue(TEXT("the whole build generates"), FMantleScatter::Generate(Huge, Whole, Error, None));
	TestTrue(*FString::Printf(TEXT("the index and every cell took %.2f s"), Seconds), Seconds < 10.0);
	TestTrue(*FString::Printf(TEXT("about %d candidates remain (%d)"), Target, HugeIndex->NumSlots()), HugeIndex->NumSlots() >= 1 && HugeIndex->NumSlots() < 400);
	TestEqual(TEXT("the cells' hash is the whole build's"), FMantleScatter::HashText(FMantleScatter::Hash(ByCells)), FMantleScatter::HashText(FMantleScatter::Hash(Whole)));
	TestTrue(TEXT("at least one instance stands"), ByCells.Num() >= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterCellsPrefixTest,
	"Mantle.Runtime.Scatter.Cells.TiersKeepAPrefix",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterCellsPrefixTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterCellsTestPrivate;

	FMantleScatterGenerateOptions OnGround;
	OnGround.Ground = BumpyGround();
	FMantleScatterParams P = CellParams();
	P.Placement = EMantleScatterPlacement::Terrain;
	P.MinSpacing = 500.0;
	FString Error;
	const int64 Count = FMantleScatter::CountFor(P, Error);

	TArray<FMantleScatterInstance> Full;
	FMantleScatter::Generate(P, Full, Error, OnGround);
	TArray<FMantleScatterInstance> Previous;
	for (const double Share : { 0.3, 0.6, 1.0 })
	{
		FMantleScatterGenerateOptions Tier = OnGround;
		Tier.MaxCandidates = static_cast<int64>(FMath::FloorToDouble(static_cast<double>(Count) * Share + 0.5));
		const FString Name = FString::Printf(TEXT("tier %g"), Share);
		CheckCellsMatchWhole(*this, *Name, P, Tier);

		TArray<FMantleScatterInstance> Cut;
		const TSharedPtr<const FMantleScatterCellIndex> Index = FMantleScatter::BuildCellIndex(P, Error);
		FMantleScatter::GenerateAllByCells(P, *Index, Cut, Error, Tier);
		TestTrue(*FString::Printf(TEXT("%s: a subset of the full stream, in order"), *Name), IsOrderedSubset(Cut, Full));
		TestTrue(*FString::Printf(TEXT("%s: a subset of the lower tier's"), *Name), Previous.Num() == 0 || IsOrderedSubset(Previous, Cut));
		Previous = Cut;
	}
	TestEqual(TEXT("the top tier is the whole stream"), Previous.Num(), Full.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterCellsQueriesTest,
	"Mantle.Runtime.Scatter.Cells.BoxQueriesMatchTheWholeBuild",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterCellsQueriesTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterCellsTestPrivate;

	FMantleScatterGenerateOptions OnGround;
	OnGround.Ground = BumpyGround();
	for (const bool bSpacing : { false, true })
	{
		FMantleScatterParams P = CellParams();
		P.Placement = EMantleScatterPlacement::Terrain;
		P.MinSlopeDegrees = 0.5;
		P.MinSpacing = bSpacing ? 700.0 : 0.0;
		FString Error;
		TArray<FMantleScatterInstance> All;
		FMantleScatter::Generate(P, All, Error, OnGround);
		const TSharedPtr<const FMantleScatterCellIndex> Index = FMantleScatter::BuildCellIndex(P, Error);
		if (!TestTrue(TEXT("index"), Index.IsValid()))
		{
			return false;
		}

		struct FQueryBox { int64 MinX, MinY, MaxX, MaxY; };
		const TArray<FQueryBox> Boxes = {
			{ -1000000, -1000000, -800000, -800000 },            // inside one cell
			{ -100000, -100000, 100000, 100000 },                // across the middle: several cells' corners
			{ Index->OriginX + Index->CellMm - 40000, 0, Index->OriginX + Index->CellMm + 40000, 300000 }, // astride a border
			{ -50000000, -50000000, -40000000, -40000000 },      // off the grid: nothing
			{ -3000000000ll, -3000000000ll, 3000000000ll, 3000000000ll }, // all of it
		};
		for (int32 BoxIndex = 0; BoxIndex < Boxes.Num(); ++BoxIndex)
		{
			const FQueryBox& Box = Boxes[BoxIndex];
			TArray<FMantleScatterInstance> Expected;
			for (const FMantleScatterInstance& Instance : All)
			{
				if (Instance.X >= Box.MinX && Instance.X <= Box.MaxX && Instance.Y >= Box.MinY && Instance.Y <= Box.MaxY)
				{
					Expected.Add(Instance);
				}
			}
			TArray<FMantleScatterInstance> Got;
			TestTrue(TEXT("the box reads"), FMantleScatter::GenerateInBox(P, *Index, Box.MinX, Box.MinY, Box.MaxX, Box.MaxY, Got, Error, OnGround));
			TestTrue(*FString::Printf(TEXT("box %d (spacing %d) is the whole build's instances in it (%d)"), BoxIndex, bSpacing ? 1 : 0, Expected.Num()), Got == Expected);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterCellsLimitsTest,
	"Mantle.Runtime.Scatter.Cells.LimitsAndTheDrawDistanceRule",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterCellsLimitsTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterCellsTestPrivate;

	FString Error;
	FMantleScatterParams P = CellParams();
	P.HalfExtent = FVector(5000.0, 5000.0, 0.0); // 100 m x 100 m: density 20 is 200,000
	P.Density = 20.0;
	P.DrawDistance = 0.0;
	TestEqual(TEXT("200,000 is built whole, with no draw distance"), FMantleScatter::CountFor(P, Error), static_cast<int64>(200000));
	TestFalse(TEXT("200,000 does not use cells"), FMantleScatter::UsesCells(P));
	P.Density = 20.0001;
	TestEqual(TEXT("200,001 needs a draw distance"), FMantleScatter::CountFor(P, Error), static_cast<int64>(-1));
	TestTrue(TEXT("and says so"), Error.Contains(TEXT("drawDistance")));
	P.DrawDistance = 3000.0;
	TestEqual(TEXT("with one it counts"), FMantleScatter::CountFor(P, Error), static_cast<int64>(200001));
	TestTrue(TEXT("and uses cells"), FMantleScatter::UsesCells(P));
	P.Density = 200.0;
	TestEqual(TEXT("2,000,000 is the per-Scatter cap"), FMantleScatter::CountFor(P, Error), static_cast<int64>(2000000));
	P.Density = 200.0001;
	TestEqual(TEXT("2,000,001 is refused"), FMantleScatter::CountFor(P, Error), static_cast<int64>(-1));
	TestTrue(TEXT("with the cap in the message"), Error.Contains(TEXT("at most 2000000")));

	// A wide minSpacing is never refused: a Scatter built in cells thins its candidates instead (WideSpacingIsThinned).
	P.Density = 200.0;
	P.MinSpacing = 57.0;
	TestEqual(TEXT("57 cm at density 200 counts"), FMantleScatter::CountFor(P, Error), static_cast<int64>(2000000));
	P.MinSpacing = 100000.0;
	TestEqual(TEXT("so does the widest spacing"), FMantleScatter::CountFor(P, Error), static_cast<int64>(2000000));
	P.MinSpacing = 0.0;
	P.Density = 20.0001;

	// A stepped path stays whole: its cap is the whole cap, and cells refuse it.
	FMantleScatterParams Step = CellParams();
	Step.Shape = EMantleScatterShape::Path;
	Step.Path = { FVector2D(-1000000.0, 0.0), FVector2D(1000000.0, 0.0) };
	Step.Width = 100.0;
	Step.PathStep = 10.0;
	TestEqual(TEXT("200,001 steps are refused even with a draw distance"), FMantleScatter::CountFor(Step, Error), static_cast<int64>(-1));
	Step.PathStep = 11.0;
	TestEqual(TEXT("181,819 steps pass"), FMantleScatter::CountFor(Step, Error), static_cast<int64>(181819));
	TestFalse(TEXT("a cell index of a stepped path is refused"), FMantleScatter::BuildCellIndex(Step, Error).IsValid());

	// The place total is the same 2,000,000 for the parser and a script's change.
	FMantleScatterParams Half = CellParams();
	Half.HalfExtent = FVector(50000.0, 50000.0, 0.0); // 1 km x 1 km
	Half.Density = 1.0;                                // 1,000,000
	TArray<const FMantleScatterParams*> All = { &Half, &Half };
	int32 Bad = INDEX_NONE;
	TestTrue(TEXT("two of 1,000,000 fit a place"), FMantleScatter::CheckPlaceTotals(All, Bad, Error));
	FMantleScatterParams One = CellParams();
	One.HalfExtent = FVector(500.0, 500.0, 0.0);
	One.Density = 1.0; // 100
	All.Add(&One);
	TestFalse(TEXT("one more instance past them does not"), FMantleScatter::CheckPlaceTotals(All, Bad, Error));
	TestEqual(TEXT("at the third"), Bad, 2);
	TestTrue(TEXT("with the message"), Error.Contains(TEXT("a place holds at most 2000000")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterCellsMeasureTest,
	"Mantle.Runtime.Scatter.Cells.Measure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterCellsMeasureTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterCellsTestPrivate;

	// What a big Scatter costs, logged (MANTLE_SCATTER_CELLS_MEASURE): the index for 2,000,000 candidates, one ordinary cell, every cell on every core.
	FMantleScatterParams P = CellParams();
	P.HalfExtent = FVector(70700.0, 70700.0, 0.0); // about 1.4 km x 1.4 km
	P.Density = 1.0;                                // about 2,000,000
	P.MinSpacing = 60.0;
	FString Error;
	const int64 Count = FMantleScatter::CountFor(P, Error);
	if (!TestTrue(*FString::Printf(TEXT("counts (%s)"), *Error), Count > 1900000 && Count <= 2000000))
	{
		return false;
	}
	FMantleScatterGenerateOptions OnGround;
	OnGround.Ground = BumpyGround();
	P.Placement = EMantleScatterPlacement::Terrain;

	double Start = FPlatformTime::Seconds();
	const TSharedPtr<const FMantleScatterCellIndex> Index = FMantleScatter::BuildCellIndex(P, Error);
	const double IndexMs = (FPlatformTime::Seconds() - Start) * 1000.0;
	if (!TestTrue(TEXT("index"), Index.IsValid()))
	{
		return false;
	}
	TArray<FMantleScatterInstance> Cell;
	const FIntPoint Middle(Index->CellsX / 2, Index->CellsY / 2);
	Start = FPlatformTime::Seconds();
	FMantleScatter::GenerateCell(P, *Index, Middle, Cell, Error, OnGround);
	const double CellMs = (FPlatformTime::Seconds() - Start) * 1000.0;
	uint32 Begin = 0, End = 0;
	Index->SlotRange(Middle, Begin, End);

	TArray<FMantleScatterInstance> All;
	Start = FPlatformTime::Seconds();
	FMantleScatter::GenerateAllByCells(P, *Index, All, Error, OnGround);
	const double AllMs = (FPlatformTime::Seconds() - Start) * 1000.0;

	UE_LOG(LogMantle, Display,
		TEXT("MANTLE_SCATTER_CELLS_MEASURE candidates=%lld slots=%d grid=%dx%d cell_m=%.1f index_ms=%.1f index_mb=%.1f cell_candidates=%u cell_kept=%d cell_ms=%.2f all_kept=%d all_by_cells_ms=%.1f"),
		static_cast<long long>(Count), Index->NumSlots(), Index->CellsX, Index->CellsY, static_cast<double>(Index->CellMm) / 1000.0, IndexMs,
		static_cast<double>(Index->MemoryBytes()) / (1024.0 * 1024.0), End - Begin, Cell.Num(), CellMs, All.Num(), AllMs);
	TestTrue(TEXT("the cell has instances"), Cell.Num() > 0);
	return true;
}

// ---- The component, in a game world ------------------------------------------------------------------------------------------------

namespace MantleScatterCellsTestPrivate
{
	struct FCvarGuard
	{
		IConsoleVariable* Variable = nullptr;
		int32 Saved = 0;
		FCvarGuard(const TCHAR* Name, const int32 Value)
		{
			Variable = IConsoleManager::Get().FindConsoleVariable(Name);
			if (Variable)
			{
				Saved = Variable->GetInt();
				Variable->Set(Value, ECVF_SetByCode);
			}
		}
		~FCvarGuard()
		{
			if (Variable)
			{
				Variable->Set(Saved, ECVF_SetByCode);
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScatterCellsComponentTest,
	"Mantle.Runtime.Scatter.Cells.ComponentBudgetHysteresisAndServerQueries",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScatterCellsComponentTest::RunTest(const FString& Parameters)
{
	using namespace MantleScatterCellsTestPrivate;

	struct FCleanup
	{
		UWorld* World = nullptr;
		uint64 Frame = GFrameCounter;
		~FCleanup()
		{
			if (World)
			{
				World->BeginTearingDown();
				World->EndPlay(EEndPlayReason::Quit);
				World->DestroyWorld(false);
				GEngine->DestroyWorldContext(World);
			}
			GFrameCounter = Frame;
		}
	} Cleanup;
	if (!TestNotNull(TEXT("engine"), GEngine))
	{
		return false;
	}
	Cleanup.World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("MantleScatterCellsWorld"));
	UWorld* World = Cleanup.World;
	if (!TestNotNull(TEXT("world"), World))
	{
		return false;
	}
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL());
	World->GetWorldSettings()->NotifyBeginPlay();
	World->CopyGameState(World->SpawnActor<AGameModeBase>(), nullptr);

	// A Scatter of 320,000 instances (400 m x 400 m at 2), drawn to 60 m: built in cells, a few thousand instances at a time.
	FMantleScatterParams P = CellParams();
	P.Density = 2.0;
	P.DrawDistance = 6000.0;
	FString Error;
	if (!TestEqual(TEXT("320,000 instances"), FMantleScatter::CountFor(P, Error), static_cast<int64>(320000)))
	{
		return false;
	}

	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("the engine's cube"), Cube))
	{
		return false;
	}
	AMantleScatterVolume* Volume = World->SpawnActor<AMantleScatterVolume>();
	if (!TestNotNull(TEXT("volume"), Volume))
	{
		return false;
	}
	Volume->ApplyProps(P);
	for (FMantleScatterMeshSettings& Entry : Volume->Meshes)
	{
		Entry.Mesh = Cube;
	}
	TestTrue(TEXT("the volume uses cells"), Volume->UsesCells());

	// A server's view: no component, queries on demand, and the same answers as a whole build.
	TArray<FMantleScatterInstance> Whole;
	FMantleScatter::Generate(P, Whole, Error);
	const TArray<FMantleScatterInstance>* Full = Volume->GenerateFullCached(Error);
	if (!TestNotNull(TEXT("the volume reads its whole stream"), Full))
	{
		return false;
	}
	TestEqual(TEXT("the volume's hash is the whole build's"), FMantleScatter::HashText(Volume->GetFullHash()), FMantleScatter::HashText(FMantleScatter::Hash(Whole)));
	TArray<FMantleScatterInstance> InArea;
	TestTrue(TEXT("an area reads"), Volume->GenerateInArea(-1500.0, -1500.0, 2500.0, 500.0, InArea, Error));
	TArray<FMantleScatterInstance> InAreaWhole;
	for (const FMantleScatterInstance& Instance : Whole)
	{
		if (Instance.X * 0.1 >= -1500.1 && Instance.X * 0.1 <= 2500.1 && Instance.Y * 0.1 >= -1500.1 && Instance.Y * 0.1 <= 500.1)
		{
			InAreaWhole.Add(Instance);
		}
	}
	// The volume's area is a superset by at most a millimetre of slack; every instance of the exact area is in it, in order.
	TestTrue(TEXT("the area holds the whole build's instances in it"), IsOrderedSubset(InAreaWhole, InArea) && InArea.Num() >= InAreaWhole.Num() && InArea.Num() <= InAreaWhole.Num() + 8);

	// Cells around a view: nothing stands before the first scan, then some do, within the caps.
	FCvarGuard MaxLive(TEXT("mantle.Scatter.MaxLiveInstances"), 40000);
	FCvarGuard PerFrame(TEXT("mantle.Scatter.MaxInstancesPerFrame"), 6000);
	if (!TestTrue(TEXT("the Scatter builds"), Volume->RefreshScatter(Error)))
	{
		return false;
	}
	UMantleScatterCellsComponent* Cells = Volume->GetCellsComponent();
	if (!TestNotNull(TEXT("a game builds it in cells"), Cells))
	{
		return false;
	}
	TestEqual(TEXT("nothing stands yet"), Volume->GetInstanceCount(), 0);
	Cells->SetViewOverride(FVector(-15000.0, -15000.0, 0.0));

	auto Tick = [World]() { ++GFrameCounter; World->Tick(LEVELTICK_All, 1.0f / 60.0f); };
	int64 MostInAFrame = 0;
	int64 Last = 0;
	auto SettleFor = [&](const double Seconds)
	{
		// At least a few scans' worth (a scan is every 0.2 s), so a move of the view is looked at before "settled" is believed.
		const double Soonest = FPlatformTime::Seconds() + 0.7;
		const double Until = FPlatformTime::Seconds() + Seconds;
		while (FPlatformTime::Seconds() < Until && (FPlatformTime::Seconds() < Soonest || !(Cells->IsSettled() && Cells->GetLiveInstances() == Last)))
		{
			Last = Cells->GetLiveInstances();
			Tick();
			MostInAFrame = FMath::Max(MostInAFrame, Cells->GetLiveInstances() - Last);
			FPlatformProcess::Sleep(0.005f);
		}
	};
	SettleFor(20.0);
	TestTrue(TEXT("the index was built"), Cells->IsIndexReady());
	const int64 Near = Cells->GetLiveInstances();
	TestTrue(TEXT("instances stand near the view"), Near > 0);
	TestTrue(TEXT("never more than the live cap"), Near <= 40000 && UMantleScatterCellsComponent::GetWorldLiveInstances() <= 40000);
	// A cell is a few thousand instances, so a frame's budget of 6,000 admits one or two, far under the Scatter's 320,000.
	TestTrue(*FString::Printf(TEXT("a frame added at most the budget plus one cell (most %lld)"), static_cast<long long>(MostInAFrame)), MostInAFrame <= 6000 + 20000);
	TestTrue(TEXT("far fewer than the whole Scatter stand"), Near < 100000);

	// Hysteresis: a step that keeps the same cells in reach changes nothing; leaving them far behind releases them.
	const int32 CellsBefore = Cells->GetLiveCells();
	Cells->SetViewOverride(FVector(-15000.0 + 300.0, -15000.0 + 300.0, 0.0));
	SettleFor(5.0);
	TestTrue(TEXT("a small step keeps the cells standing"), Cells->GetLiveCells() >= CellsBefore - 1);
	Cells->SetViewOverride(FVector(15000.0, 15000.0, 0.0));
	SettleFor(20.0);
	TestTrue(TEXT("far away, the old cells are released and new ones stand"), Cells->GetLiveInstances() > 0 && Cells->GetLiveInstances() <= 40000);
	TestTrue(TEXT("the budget counts what stands"), UMantleScatterCellsComponent::GetWorldLiveInstances() == Cells->GetLiveInstances());

	// A cell too big for the live cap is read once, not again every scan: its size is remembered until that many instances fit.
	IConsoleVariable* LiveCap = IConsoleManager::Get().FindConsoleVariable(TEXT("mantle.Scatter.MaxLiveInstances"));
	if (!TestNotNull(TEXT("the live cap variable"), LiveCap))
	{
		return false;
	}
	LiveCap->Set(50, ECVF_SetByCode);
	Cells->SetViewOverride(FVector(0.0, 0.0, 0.0));
	auto RunFor = [&](const double Seconds)
	{
		const double Until = FPlatformTime::Seconds() + Seconds;
		while (FPlatformTime::Seconds() < Until)
		{
			Tick();
			FPlatformProcess::Sleep(0.005f);
		}
	};
	RunFor(2.0);
	const int32 BuiltOnce = Cells->GetBuiltTotal();
	TestEqual(TEXT("cells over a 50-instance cap do not stand"), Cells->GetLiveInstances(), static_cast<int64>(0));
	RunFor(2.0);
	TestEqual(TEXT("and are not read again every scan"), Cells->GetBuiltTotal(), BuiltOnce);
	LiveCap->Set(40000, ECVF_SetByCode);
	RunFor(10.0);
	TestTrue(TEXT("once they fit they are asked for again and stand"), Cells->GetLiveInstances() > 0);

	// The mesh components belong to the actor, which outlives the cells component: clearing the Scatter takes them down with it.
	auto CountMeshComponents = [Volume]()
	{
		TArray<UInstancedStaticMeshComponent*> Meshes;
		Volume->GetComponents(Meshes);
		int32 Standing = 0;
		for (const UInstancedStaticMeshComponent* Mesh : Meshes)
		{
			Standing += IsValid(Mesh) ? 1 : 0;
		}
		return Standing;
	};
	TestTrue(TEXT("cells stand as mesh components on the actor"), CountMeshComponents() > 0);
	Volume->ClearScatter();
	TestEqual(TEXT("clearing the Scatter releases its share of the budget"), UMantleScatterCellsComponent::GetWorldLiveInstances(), static_cast<int64>(0));
	TestEqual(TEXT("and leaves no mesh component on the actor"), CountMeshComponents(), 0);

	// Building the Scatter again (a settings change does this) stands only the new cells, not the new ones on top of the old.
	TestTrue(TEXT("the Scatter builds again"), Volume->RefreshScatter(Error));
	UMantleScatterCellsComponent* Again = Volume->GetCellsComponent();
	if (!TestNotNull(TEXT("a new cells component"), Again))
	{
		return false;
	}
	Again->SetViewOverride(FVector(0.0, 0.0, 0.0));
	RunFor(10.0);
	int64 Standing = 0;
	{
		TArray<UInstancedStaticMeshComponent*> Meshes;
		Volume->GetComponents(Meshes);
		for (const UInstancedStaticMeshComponent* Mesh : Meshes)
		{
			Standing += IsValid(Mesh) ? Mesh->GetInstanceCount() : 0;
		}
	}
	TestEqual(TEXT("the mesh components hold exactly the instances the budget counts"), Standing, UMantleScatterCellsComponent::GetWorldLiveInstances());
	Volume->ClearScatter();
	TestEqual(TEXT("clearing again leaves nothing counted"), UMantleScatterCellsComponent::GetWorldLiveInstances(), static_cast<int64>(0));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
