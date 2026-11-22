// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCellStreamPlan.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// What the cell streamer decides, worked out by hand: the path a moving view sweeps, the distance to a cell's box, which cells are wanted and released,
// who builds first, the frame's budget, how a long solid list is cut, and which cells a spot is near.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleCellStreamPlanTestPrivate
{
	FMantleCellInfo MakeCell(const int32 Level, const int32 X, const int32 Y, const FVector& Min, const FVector& Max, const double DrawDistance)
	{
		FMantleCellInfo Cell;
		Cell.Level = Level;
		Cell.X = X;
		Cell.Y = Y;
		Cell.Bounds = FBox(Min, Max);
		Cell.DrawDistance = DrawDistance;
		return Cell;
	}

	FMantleCellView View(const FVector& Location, const FVector& Velocity)
	{
		FMantleCellView Result;
		Result.Location = Location;
		Result.Velocity = Velocity;
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamPlanPathTest,
	"Mantle.Runtime.CellStream.Plan.Path",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamPlanPathTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamPlanTestPrivate;
	const FMantleCellStreamSettings Settings;
	FMantleCellStreamPlan::FPath Path;

	// Standing, or slower than 5 m/s (500 cm/s): the view's own spot only.
	FMantleCellStreamPlan::PathOf(View(FVector(10, 20, 30), FVector::ZeroVector), Settings, Path);
	TestEqual(TEXT("standing: one point"), Path.Num(), 1);
	TestTrue(TEXT("at the view"), Path[0].Equals(FVector(10, 20, 30)));
	FMantleCellStreamPlan::PathOf(View(FVector::ZeroVector, FVector(400, 0, 0)), Settings, Path);
	TestEqual(TEXT("4 m/s: still one point"), Path.Num(), 1);

	// 10 m/s east: 1,000 cm/s x 4 s = 4,000 cm ahead, sampled at a third, two thirds and all of it.
	FMantleCellStreamPlan::PathOf(View(FVector(100, 0, 0), FVector(1000, 0, 0)), Settings, Path);
	if (TestEqual(TEXT("10 m/s: four points"), Path.Num(), 4))
	{
		TestTrue(TEXT("the first is the view"), Path[0].Equals(FVector(100, 0, 0)));
		TestTrue(TEXT("a third of 4,000 cm on"), Path[1].Equals(FVector(100.0 + 4000.0 / 3.0, 0, 0), 1e-6));
		TestTrue(TEXT("two thirds on"), Path[2].Equals(FVector(100.0 + 8000.0 / 3.0, 0, 0), 1e-6));
		TestTrue(TEXT("all of it"), Path[3].Equals(FVector(4100, 0, 0), 1e-6));
	}

	// A jet at 100 m/s: 10,000 cm/s x 4 s = 40,000 cm, which is the cap exactly; at 150 m/s (60,000 cm) the cap holds it at 40,000.
	FMantleCellStreamPlan::PathOf(View(FVector::ZeroVector, FVector(0, 15000, 0)), Settings, Path);
	if (TestEqual(TEXT("150 m/s: four points"), Path.Num(), 4))
	{
		TestTrue(TEXT("held to 400 m"), Path[3].Equals(FVector(0, 40000, 0), 1e-6));
	}

	// A dive: 50 m/s with 30 m/s east and 40 m/s down is 5,000 cm/s in direction (0.6, 0, -0.8): 20,000 cm ahead, all three dimensions.
	FMantleCellStreamPlan::PathOf(View(FVector(0, 0, 100000), FVector(3000, 0, -4000)), Settings, Path);
	if (TestEqual(TEXT("a dive: four points"), Path.Num(), 4))
	{
		TestTrue(TEXT("12,000 cm east and 16,000 cm down at the end"), Path[3].Equals(FVector(12000, 0, 84000), 1e-6));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamPlanDistanceTest,
	"Mantle.Runtime.CellStream.Plan.WantedAndReleased",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamPlanDistanceTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamPlanTestPrivate;
	const FMantleCellStreamSettings Settings;
	const FMantleCellInfo Cell = MakeCell(0, 0, 0, FVector(0, 0, 0), FVector(100, 100, 100), 150.0);

	// Distance to the box: zero inside, along an axis, across a corner (3-4-5 style: 100 and 100 give the diagonal), and straight up.
	TestEqual(TEXT("inside"), FMantleCellStreamPlan::DistanceToCell(Cell, FVector(50, 50, 50)), 0.0);
	TestEqual(TEXT("100 cm off one face"), FMantleCellStreamPlan::DistanceToCell(Cell, FVector(200, 50, 50)), 100.0);
	TestTrue(TEXT("past a corner"), FMath::IsNearlyEqual(FMantleCellStreamPlan::DistanceToCell(Cell, FVector(200, 200, 50)), FMath::Sqrt(20000.0), 1e-9));
	TestEqual(TEXT("straight above: 3D"), FMantleCellStreamPlan::DistanceToCell(Cell, FVector(50, 50, 300)), 200.0);

	// Wanted: within drawDistance (150) of some point of the path. 250 is 150 from the face: wanted (at the limit); 251 is not.
	FMantleCellStreamPlan::FPath Standing;
	Standing.Add(FVector(250, 50, 50));
	TestTrue(TEXT("exactly drawDistance away: wanted"), FMantleCellStreamPlan::IsWanted(Cell, Standing));
	Standing[0] = FVector(251, 50, 50);
	TestFalse(TEXT("1 cm farther: not"), FMantleCellStreamPlan::IsWanted(Cell, Standing));
	// A point ahead on the path that is near makes it wanted.
	Standing.Add(FVector(150, 50, 50));
	TestTrue(TEXT("a path point 50 cm from the face"), FMantleCellStreamPlan::IsWanted(Cell, Standing));
	TestEqual(TEXT("its distance is the nearest point's"), FMantleCellStreamPlan::DistanceToCell(Cell, Standing), 50.0);

	// Released: drawDistance x 1.2, or drawDistance + 25 m when that is more. 15,000 cm: 18,000 against 17,500. 6,000 cm: 7,200 against 8,500.
	const FMantleCellInfo Far = MakeCell(0, 0, 0, FVector::ZeroVector, FVector(100, 100, 100), 15000.0);
	const FMantleCellInfo Short = MakeCell(0, 0, 0, FVector::ZeroVector, FVector(100, 100, 100), 6000.0);
	TestEqual(TEXT("15,000 cm releases at 18,000"), FMantleCellStreamPlan::ReleaseDistance(Far, Settings), 18000.0);
	TestEqual(TEXT("6,000 cm releases at 8,500"), FMantleCellStreamPlan::ReleaseDistance(Short, Settings), 8500.0);
	FMantleCellStreamPlan::FPath Away;
	Away.Add(FVector(8500 + 100, 50, 50));
	TestFalse(TEXT("8,500 cm off the face: kept"), FMantleCellStreamPlan::IsReleased(Short, Away, Settings));
	Away[0] = FVector(8501 + 100, 50, 50);
	TestTrue(TEXT("1 cm farther: released"), FMantleCellStreamPlan::IsReleased(Short, Away, Settings));
	// The band is the hysteresis: wanted inside 6,000, kept until 8,500, so a view that wanders along the edge does not rebuild.
	Away[0] = FVector(7000 + 100, 50, 50);
	TestFalse(TEXT("7,000 cm: no longer wanted"), FMantleCellStreamPlan::IsWanted(Short, Away));
	TestFalse(TEXT("but not released either"), FMantleCellStreamPlan::IsReleased(Short, Away, Settings));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamPlanOrderTest,
	"Mantle.Runtime.CellStream.Plan.OrderAndBudget",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamPlanOrderTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamPlanTestPrivate;
	const FMantleCellStreamSettings Settings;

	// Ties: the smaller key, then the nearer to the view, then the lower index.
	using FRank = FMantleCellStreamPlan::FRank;
	const auto Rank = [](const double Key, const double ViewDistance, const int32 Index)
	{
		FRank Result;
		Result.Key = Key;
		Result.ViewDistance = ViewDistance;
		Result.Index = Index;
		return Result;
	};
	TestTrue(TEXT("the smaller key first"), FMantleCellStreamPlan::BuildsBefore(Rank(100.0, 500.0, 7), Rank(200.0, 50.0, 1)));
	TestFalse(TEXT("and not the other way"), FMantleCellStreamPlan::BuildsBefore(Rank(200.0, 50.0, 1), Rank(100.0, 500.0, 7)));
	TestTrue(TEXT("equal key: nearer the view"), FMantleCellStreamPlan::BuildsBefore(Rank(100.0, 50.0, 7), Rank(100.0, 60.0, 1)));
	TestTrue(TEXT("equal both: lower index"), FMantleCellStreamPlan::BuildsBefore(Rank(100.0, 50.0, 1), Rank(100.0, 50.0, 7)));
	TestFalse(TEXT("a cell is not before itself"), FMantleCellStreamPlan::BuildsBefore(Rank(100.0, 50.0, 7), Rank(100.0, 50.0, 7)));

	// Standing, or slower than 5 m/s: nearest first. A cell 35 m behind the view (x from -3,500 to -3,400) and one 40 m ahead of it (from 4,000 to 4,100).
	const FMantleCellInfo Behind = MakeCell(0, -1, 0, FVector(-3500, -50, -50), FVector(-3400, 50, 50), 15000.0);
	const FMantleCellInfo Ahead = MakeCell(0, 1, 0, FVector(4000, -50, -50), FVector(4100, 50, 50), 15000.0);
	{
		const FRank RankBehind = FMantleCellStreamPlan::RankOf(Behind, 0, View(FVector::ZeroVector, FVector::ZeroVector), Settings);
		const FRank RankAhead = FMantleCellStreamPlan::RankOf(Ahead, 1, View(FVector::ZeroVector, FVector::ZeroVector), Settings);
		TestEqual(TEXT("standing: the key is the distance, 3,400 cm"), RankBehind.Key, 3400.0);
		TestEqual(TEXT("and 4,000 cm"), RankAhead.Key, 4000.0);
		TestTrue(TEXT("the nearer is first"), FMantleCellStreamPlan::BuildsBefore(RankBehind, RankAhead));
		const FRank Slow = FMantleCellStreamPlan::RankOf(Ahead, 1, View(FVector::ZeroVector, FVector(400, 0, 0)), Settings);
		TestEqual(TEXT("4 m/s is standing"), Slow.Key, 4000.0);
	}

	// Driving east at 40 m/s: the key is the distance from a point 2,000 cm ahead (4,000 cm/s x half a second). The cell ahead is 2,000 cm off it and the cell behind 5,400:
	// the one in front goes first although it is farther from the view (4,000 against 3,400 cm).
	{
		const FMantleCellView Driving = View(FVector::ZeroVector, FVector(4000, 0, 0));
		const FRank RankBehind = FMantleCellStreamPlan::RankOf(Behind, 0, Driving, Settings);
		const FRank RankAhead = FMantleCellStreamPlan::RankOf(Ahead, 1, Driving, Settings);
		TestEqual(TEXT("ahead: 4,000 less the 2,000 cm shift"), RankAhead.Key, 2000.0);
		TestEqual(TEXT("behind: 3,400 plus the shift"), RankBehind.Key, 5400.0);
		TestEqual(TEXT("the view distance is the tie, unshifted"), RankAhead.ViewDistance, 4000.0);
		TestTrue(TEXT("so the cell in front goes first"), FMantleCellStreamPlan::BuildsBefore(RankAhead, RankBehind));
	}

	// The shift stops at 50 m: 200 m/s (20,000 cm/s) would be 10,000 cm. A cell 200 m ahead (from 20,000 to 20,100) is 15,000 cm from the point 5,000 cm on.
	{
		const FMantleCellInfo Far = MakeCell(0, 3, 0, FVector(20000, -50, -50), FVector(20100, 50, 50), 15000.0);
		const FRank Fast = FMantleCellStreamPlan::RankOf(Far, 3, View(FVector::ZeroVector, FVector(20000, 0, 0)), Settings);
		TestEqual(TEXT("200 m/s shifts by the 5,000 cm cap"), Fast.Key, 15000.0);
	}

	// A dive counts in all three dimensions: 40 m/s straight down from 1,000 m, a cell below (z from 90,000 to 90,100) is 9,900 cm off and 7,900 from the point 2,000 cm lower.
	{
		const FMantleCellInfo Below = MakeCell(0, 0, 0, FVector(-50, -50, 90000), FVector(50, 50, 90100), 15000.0);
		const FRank Dive = FMantleCellStreamPlan::RankOf(Below, 0, View(FVector(0, 0, 100000), FVector(0, 0, -4000)), Settings);
		TestEqual(TEXT("below, diving"), Dive.Key, 7900.0);
		TestEqual(TEXT("9,900 cm from the view"), Dive.ViewDistance, 9900.0);
	}

	// Budget: 2 ms to 15 m/s (1,500 cm/s), then linear to 6 ms at 60 m/s (6,000 cm/s); 8 ms while holding. Halfway is 3,750 cm/s and 4 ms.
	TestEqual(TEXT("standing"), FMantleCellStreamPlan::BudgetSeconds(Settings, 0.0, false), 0.002);
	TestEqual(TEXT("15 m/s"), FMantleCellStreamPlan::BudgetSeconds(Settings, 1500.0, false), 0.002);
	TestTrue(TEXT("halfway: 4 ms"), FMath::IsNearlyEqual(FMantleCellStreamPlan::BudgetSeconds(Settings, 3750.0, false), 0.004, 1e-12));
	TestEqual(TEXT("60 m/s"), FMantleCellStreamPlan::BudgetSeconds(Settings, 6000.0, false), 0.006);
	TestEqual(TEXT("250 m/s"), FMantleCellStreamPlan::BudgetSeconds(Settings, 25000.0, false), 0.006);
	TestEqual(TEXT("holding"), FMantleCellStreamPlan::BudgetSeconds(Settings, 0.0, true), 0.008);

	// Teleport: more than 100 m (10,000 cm) in a frame, or more than 300 m/s (30,000 cm/s) over a long one, whichever is larger. A 60 fps frame allows 500 cm by speed, so the 10,000 cm
	// floor rules: exactly 10,000 is travel, 10,001 is a teleport. A 0.5 s hitch allows 15,000 cm: 200 m/s for 0.5 s is 10,000 cm and is travel, 15,001 is not; a 1 s frame allows 30,000.
	TestFalse(TEXT("exactly 100 m in a frame is travel"), FMantleCellStreamPlan::IsTeleport(FVector(10000, 0, 0), 1.0 / 60.0, Settings));
	TestTrue(TEXT("100 m and a little more in a frame is a teleport"), FMantleCellStreamPlan::IsTeleport(FVector(10001, 0, 0), 1.0 / 60.0, Settings));
	TestFalse(TEXT("200 m/s over a 0.5 s hitch is travel"), FMantleCellStreamPlan::IsTeleport(FVector(10000, 0, 0), 0.5, Settings));
	TestFalse(TEXT("a 0.5 s hitch allows 150 m"), FMantleCellStreamPlan::IsTeleport(FVector(15000, 0, 0), 0.5, Settings));
	TestTrue(TEXT("more than 150 m in 0.5 s is a teleport"), FMantleCellStreamPlan::IsTeleport(FVector(15001, 0, 0), 0.5, Settings));
	TestFalse(TEXT("a 1 s frame allows 300 m, in any direction (3-4-5: 18,000 and 24,000 is 30,000)"), FMantleCellStreamPlan::IsTeleport(FVector(18000, 0, 24000), 1.0, Settings));
	TestTrue(TEXT("and no more"), FMantleCellStreamPlan::IsTeleport(FVector(18001, 0, 24001), 1.0, Settings));

	// A hold and an asset wait give up only when nothing has moved for the idle timeout, counted from the later of when they began and the last progress (City Sample's first visit: minutes of
	// downloads that never stop moving must not give up at a fixed 60 s).
	TestEqual(TEXT("the hold's default: a minute with nothing moving"), Settings.HoldIdleTimeoutSeconds, 60.0);
	TestFalse(TEXT("begun 59 s ago, nothing since: still waiting"), FMantleCellStreamPlan::HasStalled(159.0, 100.0, 0.0, 60.0));
	TestTrue(TEXT("begun 60 s ago, nothing since: stalled"), FMantleCellStreamPlan::HasStalled(160.0, 100.0, 0.0, 60.0));
	TestFalse(TEXT("begun 10 minutes ago, progress 30 s ago: still waiting"), FMantleCellStreamPlan::HasStalled(700.0, 100.0, 670.0, 60.0));
	TestTrue(TEXT("progress 60 s ago: stalled"), FMantleCellStreamPlan::HasStalled(730.0, 100.0, 670.0, 60.0));
	TestFalse(TEXT("progress before the wait began does not shorten it"), FMantleCellStreamPlan::HasStalled(159.0, 100.0, 50.0, 60.0));
	TestTrue(TEXT("the join's radius reaches every cell the spot wants"), Settings.JoinHoldRadiusCm >= UE_DOUBLE_BIG_NUMBER);
	TestEqual(TEXT("a teleport's is 100 m"), Settings.HoldRadiusCm, 10000.0);

	// Velocity smoothing: an eighth of a second of a quarter-second window moves half way.
	const FVector Half = FMantleCellStreamPlan::SmoothVelocity(FVector::ZeroVector, FVector(100, 0, 0), 0.125, Settings);
	TestTrue(TEXT("half way"), Half.Equals(FVector(50, 0, 0), 1e-9));
	const FVector All = FMantleCellStreamPlan::SmoothVelocity(FVector(5, 0, 0), FVector(100, 0, 0), 0.5, Settings);
	TestTrue(TEXT("a long frame: all the way"), All.Equals(FVector(100, 0, 0), 1e-9));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamPlanPiecesTest,
	"Mantle.Runtime.CellStream.Plan.Pieces",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamPlanPiecesTest::RunTest(const FString& Parameters)
{
	const FMantleCellStreamSettings Settings;

	// Up to 100 solid instances is one piece; 101 is two; 888 (S0-M34's 18 ms list) is ceil(888 / 100) = 9; 4,096 is 41.
	TestEqual(TEXT("empty"), FMantleCellStreamPlan::PieceCount(0, true, Settings), 1);
	TestEqual(TEXT("100 solid"), FMantleCellStreamPlan::PieceCount(100, true, Settings), 1);
	TestEqual(TEXT("101 solid"), FMantleCellStreamPlan::PieceCount(101, true, Settings), 2);
	TestEqual(TEXT("888 solid"), FMantleCellStreamPlan::PieceCount(888, true, Settings), 9);
	TestEqual(TEXT("4,096 solid"), FMantleCellStreamPlan::PieceCount(4096, true, Settings), 41);
	// A list with no collision is one piece whatever its length (it holds at most 4,096, about 0.5 ms).
	TestEqual(TEXT("empty visual"), FMantleCellStreamPlan::PieceCount(0, false, Settings), 1);
	TestEqual(TEXT("4,096 visual"), FMantleCellStreamPlan::PieceCount(4096, false, Settings), 1);

	// 888 in 9: 98 each and 6 left over, so the first six hold 99. Pieces 0 to 5 start at 0, 99, 198, 297, 396, 495; piece 6 at 594 with 98; piece 8 at 790 with 98, ending at 888.
	int32 First = 0;
	int32 Count = 0;
	FMantleCellStreamPlan::PieceRange(888, 9, 0, First, Count);
	TestTrue(TEXT("piece 0: 0, 99"), First == 0 && Count == 99);
	FMantleCellStreamPlan::PieceRange(888, 9, 5, First, Count);
	TestTrue(TEXT("piece 5: 495, 99"), First == 495 && Count == 99);
	FMantleCellStreamPlan::PieceRange(888, 9, 6, First, Count);
	TestTrue(TEXT("piece 6: 594, 98"), First == 594 && Count == 98);
	FMantleCellStreamPlan::PieceRange(888, 9, 8, First, Count);
	TestTrue(TEXT("piece 8: 790, 98 (ends at 888)"), First == 790 && Count == 98 && First + Count == 888);
	// Every instance is in exactly one piece.
	int32 Total = 0;
	int32 NextFirst = 0;
	for (int32 Piece = 0; Piece < 9; ++Piece)
	{
		FMantleCellStreamPlan::PieceRange(888, 9, Piece, First, Count);
		TestEqual(FString::Printf(TEXT("piece %d follows the one before"), Piece), First, NextFirst);
		NextFirst = First + Count;
		Total += Count;
	}
	TestEqual(TEXT("the pieces hold all 888"), Total, 888);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellStreamPlanNearTest,
	"Mantle.Runtime.CellStream.Plan.CellsNear",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellStreamPlanNearTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellStreamPlanTestPrivate;

	// Two levels (64 m and 256 m squares). Cells in the index's order: (0,0,0) (0,1,0) (0,2,0) (0,10,10) (1,0,0).
	FMantleCellIndex Index;
	Index.Levels.Add(FMantleCellGridLevel{ 6400, 15000.0 });
	Index.Levels.Add(FMantleCellGridLevel{ 25600, 60000.0 });
	Index.Cells.Add(MakeCell(0, 0, 0, FVector(0, 0, 0), FVector(6400, 6400, 500), 6000.0));
	// A big model hangs 300 cm over the east edge of its square (12,800 to 13,100).
	Index.Cells.Add(MakeCell(0, 1, 0, FVector(6400, 0, 0), FVector(13100, 6400, 500), 6000.0));
	Index.Cells.Add(MakeCell(0, 2, 0, FVector(12800, 0, 0), FVector(19200, 6400, 500), 6000.0));
	Index.Cells.Add(MakeCell(0, 10, 10, FVector(64000, 64000, 0), FVector(70400, 70400, 500), 6000.0));
	Index.Cells.Add(MakeCell(1, 0, 0, FVector(0, 0, 0), FVector(25600, 25600, 900), 6000.0));

	TArray<double> Overhangs;
	FMantleCellStreamPlan::LevelOverhangs(Index, Overhangs);
	if (!TestEqual(TEXT("one overhang a level"), Overhangs.Num(), 2))
	{
		return false;
	}
	// Level 0's widest reach past its square: cell (1,0) ends at 13,100 where its square ends at 12,800: 300. Cell (2,0) starts at 12,800 = its square's start: none.
	TestEqual(TEXT("level 0 overhangs by 300"), Overhangs[0], 300.0);
	TestEqual(TEXT("level 1 does not"), Overhangs[1], 0.0);

	// From (3,200, 3,200, 0) with 6,000 cm reach on both levels: level 0 looks at squares floor((3200 - 6300) / 6400) = -1 to floor((3200 + 6300) / 6400) = 1 in x and y,
	// which holds (0,0,0) and (0,1,0) but not (0,2,0) (x = 2) or (0,10,10); level 1 looks at x and y from -1 to 0, which holds (1,0,0).
	FMantleCellStreamPlan::FPath Path;
	Path.Add(FVector(3200, 3200, 0));
	TArray<int32> Near;
	FMantleCellStreamPlan::CellsNear(Index, Overhangs, TArray<double>{ 6000.0, 6000.0 }, Path, Near);
	TestEqual(TEXT("three candidates"), Near.Num(), 3);
	if (Near.Num() == 3)
	{
		TestTrue(TEXT("cells 0, 1 and 4, in index order"), Near[0] == 0 && Near[1] == 1 && Near[2] == 4);
	}

	// Two points of one path that look at the same squares give each cell once, and a far point finds the far cell: (67,200, 67,200) is in square (10, 10) of level 0.
	Path.Add(FVector(3300, 3200, 0));
	Path.Add(FVector(67200, 67200, 0));
	FMantleCellStreamPlan::CellsNear(Index, Overhangs, TArray<double>{ 3000.0, 3000.0 }, Path, Near);
	// Level 0 (reach 3,300): squares -1 to 1 round the first two points, which holds cells 0 and 1 (not x = 2), and squares 9 to 11 round the third, which holds cell 3; level 1
	// (reach 3,000): square 0 round the first two, which holds cell 4.
	TestEqual(TEXT("four, each once"), Near.Num(), 4);
	TestTrue(TEXT("cells 0, 1, 3 and 4"), Near.Contains(0) && Near.Contains(1) && Near.Contains(3) && Near.Contains(4));
	TestFalse(TEXT("not cell 2 (square x = 2 is out of reach)"), Near.Contains(2));
	TArray<int32> Sorted = Near;
	Sorted.Sort();
	for (int32 At = 1; At < Sorted.Num(); ++At)
	{
		TestTrue(TEXT("no duplicates"), Sorted[At] != Sorted[At - 1]);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
