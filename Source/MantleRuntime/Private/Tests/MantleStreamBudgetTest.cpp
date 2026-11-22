// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCellStreamPlan.h"
#include "MantleFarSetPlan.h"
#include "MantlePlaceAssets.h"
#include "MantleStreamBudget.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The live budgets (stage 8 of large places), worked out by hand: the limits for each graphics setting and machine, the pressure, the ladder and the two reach scales it gives, and how the
// cell plan and the far-set plan use them. The streamer's own behaviour with them is in MantleCellStreamerTest.cpp (Mantle.Runtime.CellStream.Streamer.Budget*).

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleStreamBudgetTestPrivate
{
	constexpr int64 GiB = 1024ll * 1024 * 1024;

	FMantleCellInfo CellAt(const double MinX, const double MaxX, const double DrawDistance)
	{
		FMantleCellInfo Cell;
		Cell.Bounds = FBox(FVector(MinX, 0.0, 0.0), FVector(MaxX, 6400.0, 500.0));
		Cell.DrawDistance = DrawDistance;
		return Cell;
	}

	FMantleCellStreamPlan::FPath PathAt(const FVector& Point)
	{
		FMantleCellStreamPlan::FPath Path;
		Path.Add(Point);
		return Path;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStreamBudgetLimitsTest,
	"Mantle.Runtime.StreamBudget.Limits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStreamBudgetLimitsTest::RunTest(const FString& Parameters)
{
	using namespace MantleStreamBudgetTestPrivate;

	TestEqual(TEXT("below Low clamps to Low"), FMantleStreamBudget::ClampLevel(-1), 0);
	TestEqual(TEXT("Epic is 3"), FMantleStreamBudget::ClampLevel(3), 3);
	TestEqual(TEXT("Cinematic (4) counts as Epic"), FMantleStreamBudget::ClampLevel(4), 3);

	// The machine the numbers were measured on: 16 GB of video memory and 32 GB of RAM: factor 1.
	TestEqual(TEXT("the measured machine"), FMantleStreamBudget::MachineFactor({ 16 * GiB, 32 * GiB }), 1.0);
	TestEqual(TEXT("a bigger machine does not go over 1"), FMantleStreamBudget::MachineFactor({ 24 * GiB, 64 * GiB }), 1.0);
	// An 8 GB card on a big PC: 8 / 16 = 0.5.
	TestEqual(TEXT("an 8 GB card halves every count"), FMantleStreamBudget::MachineFactor({ 8 * GiB, 64 * GiB }), 0.5);
	// 12 GB card (0.75) with 24 GB of RAM (0.75): 0.75.
	TestEqual(TEXT("12 GB and 24 GB: three quarters"), FMantleStreamBudget::MachineFactor({ 12 * GiB, 24 * GiB }), 0.75);
	// 16 GB card with 16 GB RAM: min(1, 0.5) = 0.5. A 4 GB card would be 0.25: held at the floor of 0.5.
	TestEqual(TEXT("16 GB of RAM is the smaller: half"), FMantleStreamBudget::MachineFactor({ 16 * GiB, 16 * GiB }), 0.5);
	TestEqual(TEXT("under half is held at half"), FMantleStreamBudget::MachineFactor({ 4 * GiB, 64 * GiB }), 0.5);
	TestEqual(TEXT("an unknown machine counts as big enough"), FMantleStreamBudget::MachineFactor({ 0, 0 }), 1.0);
	TestEqual(TEXT("an unknown card on 24 GB of RAM: 24 / 32"), FMantleStreamBudget::MachineFactor({ -1, 24 * GiB }), 0.75);

	// The table, at factor 1.
	const FMantleBudgetLimits Low = FMantleStreamBudget::LimitsFor(0, 1.0);
	TestEqual(TEXT("Low instances"), Low.Instances, static_cast<int64>(275000));
	TestEqual(TEXT("Low solid things"), Low.SolidThings, static_cast<int64>(30000));
	TestEqual(TEXT("Low far instances"), Low.FarInstances, static_cast<int64>(50000));
	TestEqual(TEXT("Low Nanite pages (256 MB of 32 KB pages)"), Low.NaniteRootPages, static_cast<int64>(8192));
	TestEqual(TEXT("Low process memory share"), Low.ProcessMemoryShare, 0.55);
	const FMantleBudgetLimits Epic = FMantleStreamBudget::LimitsFor(3, 1.0);
	TestEqual(TEXT("Epic instances"), Epic.Instances, static_cast<int64>(1100000));
	TestEqual(TEXT("Epic solid things"), Epic.SolidThings, static_cast<int64>(120000));
	TestEqual(TEXT("Epic far instances"), Epic.FarInstances, static_cast<int64>(400000));
	TestEqual(TEXT("Epic Nanite pages: the budget the asset set always had"), Epic.NaniteRootPages, FMantleNaniteRootPages::BudgetPages);
	TestEqual(TEXT("Epic process memory share"), Epic.ProcessMemoryShare, 0.70);
	TestEqual(TEXT("each setting is a quarter step: Balanced instances"), FMantleStreamBudget::LimitsFor(1, 1.0).Instances, static_cast<int64>(550000));
	TestEqual(TEXT("High solid things"), FMantleStreamBudget::LimitsFor(2, 1.0).SolidThings, static_cast<int64>(90000));
	TestEqual(TEXT("a level past Epic reads as Epic"), FMantleStreamBudget::LimitsFor(9, 1.0).Instances, static_cast<int64>(1100000));

	// Scaled by the machine: Balanced at 0.75 is 412,500 instances, 45,000 solid, 75,000 far, 12,288 pages; the memory share is not scaled.
	const FMantleBudgetLimits Scaled = FMantleStreamBudget::LimitsFor(1, 0.75);
	TestEqual(TEXT("instances x 0.75"), Scaled.Instances, static_cast<int64>(412500));
	TestEqual(TEXT("solid x 0.75"), Scaled.SolidThings, static_cast<int64>(45000));
	TestEqual(TEXT("far x 0.75"), Scaled.FarInstances, static_cast<int64>(75000));
	TestEqual(TEXT("pages x 0.75"), Scaled.NaniteRootPages, static_cast<int64>(12288));
	TestEqual(TEXT("the memory share is the machine's own measure"), Scaled.ProcessMemoryShare, 0.60);
	// Epic on an 8 GB card: half of everything.
	TestEqual(TEXT("Epic on an 8 GB card: 550,000 instances"), FMantleStreamBudget::LimitsFor(3, 0.5).Instances, static_cast<int64>(550000));
	TestEqual(TEXT("and 18,432 pages"), FMantleStreamBudget::LimitsFor(3, 0.5).NaniteRootPages, static_cast<int64>(18432));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStreamBudgetPressureTest,
	"Mantle.Runtime.StreamBudget.Pressure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStreamBudgetPressureTest::RunTest(const FString& Parameters)
{
	const FMantleBudgetLimits Epic = FMantleStreamBudget::LimitsFor(3, 1.0);
	FMantleBudgetUse Use;
	TestEqual(TEXT("nothing standing: no pressure"), FMantleStreamBudget::Pressure(Epic, Use), 0.0);

	Use.Instances = 550000;
	TestEqual(TEXT("550,000 of 1,100,000 instances: 0.5"), FMantleStreamBudget::Pressure(Epic, Use), 0.5);
	Use.SolidThings = 150000;
	TestEqual(TEXT("150,000 of 120,000 solid things: 1.25 is the highest"), FMantleStreamBudget::Pressure(Epic, Use), 1.25);
	Use.FarInstances = 100000;
	Use.NaniteRootPages = 18432;
	TestEqual(TEXT("far 0.25 and Nanite 0.5 are lower: still 1.25"), FMantleStreamBudget::Pressure(Epic, Use), 1.25);
	Use.NaniteRootPages = 73728;
	TestEqual(TEXT("73,728 of 36,864 pages: 2"), FMantleStreamBudget::Pressure(Epic, Use), 2.0);

	FMantleBudgetUse MemoryOnly;
	MemoryOnly.ProcessMemoryShare = 0.35;
	TestEqual(TEXT("35% of the RAM against 70%: 0.5"), FMantleStreamBudget::Pressure(Epic, MemoryOnly), 0.5);
	TestEqual(TEXT("the memory pressure alone"), FMantleStreamBudget::MemoryPressure(Epic, MemoryOnly), 0.5);
	TestEqual(TEXT("and it is not in the count pressure"), FMantleStreamBudget::CountPressure(Epic, MemoryOnly), 0.0);
	MemoryOnly.Instances = 1100000;
	TestEqual(TEXT("the instances alone are 1 and the memory 0.5: the count pressure is 1"), FMantleStreamBudget::CountPressure(Epic, MemoryOnly), 1.0);
	MemoryOnly.ProcessMemoryShare = 0.0;
	TestEqual(TEXT("an unknown memory share is not counted: only the instances (1) press"), FMantleStreamBudget::Pressure(Epic, MemoryOnly), 1.0);
	MemoryOnly.Instances = 0;
	TestEqual(TEXT("and with no counts either, nothing presses"), FMantleStreamBudget::Pressure(Epic, MemoryOnly), 0.0);

	// No limits at all (the default: every test that does not ask): nothing is over.
	FMantleBudgetUse Huge;
	Huge.Instances = 900000000;
	Huge.SolidThings = 900000000;
	Huge.ProcessMemoryShare = 0.99;
	TestEqual(TEXT("unlimited limits never press"), FMantleStreamBudget::Pressure(FMantleBudgetLimits(), Huge), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStreamBudgetLadderTest,
	"Mantle.Runtime.StreamBudget.Ladder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStreamBudgetLadderTest::RunTest(const FString& Parameters)
{
	const FMantleBudgetTuning Tuning;
	TestEqual(TEXT("over budget: down 0.1"), FMantleStreamBudget::NextLadder(1.0, 1.5, Tuning), 0.9);
	TestEqual(TEXT("never below 0"), FMantleStreamBudget::NextLadder(0.05, 2.0, Tuning), 0.0);
	TestEqual(TEXT("pressure of exactly 1 holds"), FMantleStreamBudget::NextLadder(0.7, 1.0, Tuning), 0.7);
	TestEqual(TEXT("between 0.8 and 1 holds"), FMantleStreamBudget::NextLadder(0.7, 0.9, Tuning), 0.7);
	TestEqual(TEXT("pressure of exactly 0.8 holds"), FMantleStreamBudget::NextLadder(0.7, 0.8, Tuning), 0.7);
	TestEqual(TEXT("under 0.8: up 0.02"), FMantleStreamBudget::NextLadder(0.5, 0.5, Tuning), 0.52);
	TestEqual(TEXT("never above 1"), FMantleStreamBudget::NextLadder(0.99, 0.0, Tuning), 1.0);

	// The two scales. The far sets' reach goes first: 1 at ladder 1, half at 0.75, none from 0.5 down, while the cells' reach is still whole.
	TestEqual(TEXT("far scale at 1"), FMantleStreamBudget::FarScale(1.0), 1.0);
	TestEqual(TEXT("far scale at 0.75"), FMantleStreamBudget::FarScale(0.75), 0.5);
	TestEqual(TEXT("far scale at 0.625"), FMantleStreamBudget::FarScale(0.625), 0.25);
	TestEqual(TEXT("far scale at 0.5: none"), FMantleStreamBudget::FarScale(0.5), 0.0);
	TestEqual(TEXT("far scale at 0.25: none"), FMantleStreamBudget::FarScale(0.25), 0.0);
	TestEqual(TEXT("cell scale at 1"), FMantleStreamBudget::NearScale(1.0, Tuning), 1.0);
	TestEqual(TEXT("cell scale at 0.5: still whole while the far sets are already gone"), FMantleStreamBudget::NearScale(0.5, Tuning), 1.0);
	TestEqual(TEXT("cell scale at 0.25: halfway from 1 to 0.25"), FMantleStreamBudget::NearScale(0.25, Tuning), 0.625);
	TestEqual(TEXT("cell scale at 0: the least"), FMantleStreamBudget::NearScale(0.0, Tuning), 0.25);

	// Steady pressure of 1.5 from the top: ten steps (5 s of looks) to the bottom, the far sets gone after five, the cells' reach untouched until then.
	double Ladder = 1.0;
	for (int32 Step = 0; Step < 5; ++Step)
	{
		Ladder = FMantleStreamBudget::NextLadder(Ladder, 1.5, Tuning);
	}
	TestTrue(TEXT("after 5 steps the ladder is 0.5"), FMath::IsNearlyEqual(Ladder, 0.5, 1.0e-9));
	TestEqual(TEXT("far sets gone"), FMantleStreamBudget::FarScale(Ladder), 0.0);
	TestEqual(TEXT("cells whole"), FMantleStreamBudget::NearScale(Ladder, Tuning), 1.0);
	for (int32 Step = 0; Step < 5; ++Step)
	{
		Ladder = FMantleStreamBudget::NextLadder(Ladder, 1.5, Tuning);
	}
	TestTrue(TEXT("after 10 steps the ladder is 0"), FMath::IsNearlyEqual(Ladder, 0.0, 1.0e-9));
	TestEqual(TEXT("cells at their least"), FMantleStreamBudget::NearScale(Ladder, Tuning), 0.25);
	// Back up at 0.02 a step: 25 steps (12.5 s) from 0 to 0.5, 50 in all.
	for (int32 Step = 0; Step < 50; ++Step)
	{
		Ladder = FMantleStreamBudget::NextLadder(Ladder, 0.1, Tuning);
	}
	TestTrue(TEXT("50 steps up is the top again"), FMath::IsNearlyEqual(Ladder, 1.0, 1.0e-9));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStreamBudgetReachTest,
	"Mantle.Runtime.StreamBudget.Reach",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStreamBudgetReachTest::RunTest(const FString& Parameters)
{
	using namespace MantleStreamBudgetTestPrivate;
	// A 600 m cell, floor 200 m.
	TestEqual(TEXT("scale 1: the drawDistance"), FMantleStreamBudget::ScaledReach(60000.0, 1.0, 20000.0), 60000.0);
	TestEqual(TEXT("scale 0.5: 300 m"), FMantleStreamBudget::ScaledReach(60000.0, 0.5, 20000.0), 30000.0);
	TestEqual(TEXT("scale 0.25 is 150 m, held at the 200 m floor"), FMantleStreamBudget::ScaledReach(60000.0, 0.25, 20000.0), 20000.0);
	TestEqual(TEXT("a cell that draws only 120 m is never cut below it, floor or not"), FMantleStreamBudget::ScaledReach(12000.0, 0.25, 20000.0), 12000.0);
	TestEqual(TEXT("scale above 1 does not reach past the drawDistance"), FMantleStreamBudget::ScaledReach(60000.0, 2.0, 20000.0), 60000.0);

	// The plan: a 600 m cell whose bounds start 250 m from the view.
	const FMantleCellInfo Cell = CellAt(25000.0, 31400.0, 60000.0);
	const FMantleCellStreamPlan::FPath Path = PathAt(FVector(0.0, 100.0, 100.0));
	FMantleCellStreamSettings Settings;
	TestTrue(TEXT("whole reach: wanted from 250 m"), FMantleCellStreamPlan::IsWanted(Cell, Path, Settings));
	TestEqual(TEXT("whole reach: 600 m"), FMantleCellStreamPlan::ReachOf(Cell, Settings), 60000.0);
	TestEqual(TEXT("release at 720 m (x 1.2)"), FMantleCellStreamPlan::ReleaseDistance(Cell, Settings), 72000.0);
	Settings.ReachScale = 0.5;
	TestTrue(TEXT("scale 0.5: 300 m, still wanted from 250 m"), FMantleCellStreamPlan::IsWanted(Cell, Path, Settings));
	TestEqual(TEXT("release at 360 m"), FMantleCellStreamPlan::ReleaseDistance(Cell, Settings), 36000.0);
	Settings.ReachScale = 0.25;
	TestEqual(TEXT("scale 0.25: the 200 m floor"), FMantleCellStreamPlan::ReachOf(Cell, Settings), 20000.0);
	TestFalse(TEXT("not wanted from 250 m"), FMantleCellStreamPlan::IsWanted(Cell, Path, Settings));
	TestEqual(TEXT("release at the larger of 240 m and 225 m"), FMantleCellStreamPlan::ReleaseDistance(Cell, Settings), 24000.0);
	TestTrue(TEXT("a cell standing 250 m away is let go"), FMantleCellStreamPlan::IsReleased(Cell, Path, Settings));
	// A view 100 m from the cell is inside the floor at any scale: the walls round the player always stand.
	const FMantleCellStreamPlan::FPath Near = PathAt(FVector(25000.0 - 10000.0, 100.0, 100.0));
	TestTrue(TEXT("100 m from the cell: wanted at the least reach"), FMantleCellStreamPlan::IsWanted(Cell, Near, Settings));
	TestFalse(TEXT("and not released"), FMantleCellStreamPlan::IsReleased(Cell, Near, Settings));
	// The two-argument one is the whole reach whatever the settings say.
	TestTrue(TEXT("the plain IsWanted is unchanged"), FMantleCellStreamPlan::IsWanted(Cell, Path));

	// The far sets: reach 3 x 600 m = 1,800 m (the cap is 2 km); the cell is 3.5 km from this view.
	FMantleFarSetSettings Far;
	Far.bEnabled = true;
	const FMantleCellInfo FarCell = CellAt(350000.0, 356400.0, 60000.0);
	TestFalse(TEXT("3,500 m is past the reach of 1,800 m"), FMantleFarSetPlan::IsWanted(FarCell, Path, Far));
	const FMantleCellInfo MidCell = CellAt(120000.0, 126400.0, 60000.0);
	TestTrue(TEXT("1,200 m is inside it"), FMantleFarSetPlan::IsWanted(MidCell, Path, Far));
	TestEqual(TEXT("the twin is made with the whole reach"), FMantleFarSetPlan::ReachOf(MidCell, Far), 180000.0);
	Far.ReachScale = 0.5;
	TestEqual(TEXT("half scale: 900 m live"), FMantleFarSetPlan::LiveReachOf(MidCell, Far), 90000.0);
	TestEqual(TEXT("the twin is still made with the whole reach"), FMantleFarSetPlan::ReachOf(MidCell, Far), 180000.0);
	TestFalse(TEXT("1,200 m is now past it"), FMantleFarSetPlan::IsWanted(MidCell, Path, Far));
	const FMantleCellInfo CloserCell = CellAt(80000.0, 86400.0, 60000.0);
	TestTrue(TEXT("800 m is inside"), FMantleFarSetPlan::IsWanted(CloserCell, Path, Far));
	Far.ReachScale = 0.0;
	TestFalse(TEXT("scale 0 wants none, not even the cell the view stands in"), FMantleFarSetPlan::IsWanted(CellAt(-100.0, 6300.0, 60000.0), Path, Far));
	// Released: with scale 0.5 the live reach is 900 m, release at the larger of 1,080 m and 925 m.
	Far.ReachScale = 0.5;
	TestFalse(TEXT("a twin 1,000 m away is kept (inside 1,080 m)"), FMantleFarSetPlan::IsReleased(CellAt(100000.0, 106400.0, 60000.0), Path, Far));
	TestTrue(TEXT("one 1,100 m away is let go"), FMantleFarSetPlan::IsReleased(CellAt(110000.0, 116400.0, 60000.0), Path, Far));

	// A twin is built from half the cell's drawDistance (300 m), and not at all once a budget has given the far sets up (the real cells' reach shrinks only after that).
	Far.ReachScale = 1.0;
	const FMantleCellInfo Close = CellAt(25000.0, 31400.0, 60000.0);
	TestFalse(TEXT("250 m from the cell is not past 300 m: no twin yet"), FMantleFarSetPlan::NeedsBuilding(Close, Path, Far));
	const FMantleCellInfo Past = CellAt(40000.0, 46400.0, 60000.0);
	TestTrue(TEXT("400 m from the cell is past it"), FMantleFarSetPlan::NeedsBuilding(Past, Path, Far));
	Far.ReachScale = 0.0;
	TestFalse(TEXT("far sets given up: none is built, wherever the view is"), FMantleFarSetPlan::NeedsBuilding(Past, Path, Far));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStreamBudgetTexturesAndMemoryCapTest,
	"Mantle.Runtime.StreamBudget.TexturesAndMemoryCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStreamBudgetTexturesAndMemoryCapTest::RunTest(const FString& Parameters)
{
	using namespace MantleStreamBudgetTestPrivate;
	// #189 (the client big-map plan, 4.3): the place's textures are a count, a share of the card by setting, and the process has a cap in bytes beside its share of RAM.
	TestEqual(TEXT("Low: a quarter of the card"), FMantleStreamBudget::TextureShareOfVideo(0), 0.25);
	TestEqual(TEXT("Epic: half of it"), FMantleStreamBudget::TextureShareOfVideo(3), 0.5);
	const FMantleBudgetLimits Big = FMantleStreamBudget::LimitsFor(3, FMantleBudgetMachine{ 16 * GiB, 64 * GiB }, 1.0);
	TestEqual(TEXT("Epic on a 16 GB card: 8 GB of textures"), Big.TextureBytes, 8 * GiB);
	TestEqual(TEXT("and 24 GB for the process on 64 GB of RAM"), Big.ProcessMemoryBytes, 24 * GiB);
	TestEqual(TEXT("the counts are the ones LimitsFor gives"), Big.Instances, FMantleStreamBudget::LimitsFor(3, 1.0).Instances);
	const FMantleBudgetLimits Minimum = FMantleStreamBudget::LimitsFor(1, FMantleBudgetMachine{ 8 * GiB, 16 * GiB }, 1.0);
	TestEqual(TEXT("Joshua's minimum PC on Balanced: a third of an 8 GB card"), Minimum.TextureBytes, static_cast<int64>(8.0 * GiB * 0.33));
	TestEqual(TEXT("and 9 GB for the process"), Minimum.ProcessMemoryBytes, 9 * GiB);
	TestEqual(TEXT("32 GB of RAM: 17 GB"), FMantleStreamBudget::ProcessMemoryCapBytes(32 * GiB), 17 * GiB);
	TestEqual(TEXT("24 GB: half way between 9 and 17"), FMantleStreamBudget::ProcessMemoryCapBytes(24 * GiB), 13 * GiB);
	TestEqual(TEXT("8 GB: in proportion below 16"), FMantleStreamBudget::ProcessMemoryCapBytes(8 * GiB), static_cast<int64>(4.5 * GiB));
	TestEqual(TEXT("128 GB: on at 64 GB's slope"), FMantleStreamBudget::ProcessMemoryCapBytes(128 * GiB), 38 * GiB);
	TestEqual(TEXT("unknown RAM: no cap"), FMantleStreamBudget::ProcessMemoryCapBytes(0), MAX_int64);
	const FMantleBudgetLimits NoCard = FMantleStreamBudget::LimitsFor(3, FMantleBudgetMachine{ 0, 32 * GiB }, 1.0);
	TestEqual(TEXT("no card known (a server): textures are not limited"), NoCard.TextureBytes, MAX_int64);
	TestEqual(TEXT("-MantleBudgetScale scales the textures' limit with the counts"),
		FMantleStreamBudget::LimitsFor(3, FMantleBudgetMachine{ 16 * GiB, 64 * GiB }, 0.5).TextureBytes, 4 * GiB);

	// Pressure: textures are a count; the byte cap and the share are memory, and the higher of them counts.
	FMantleBudgetLimits Limits;
	Limits.TextureBytes = 8 * GiB;
	Limits.ProcessMemoryShare = 0.7;
	Limits.ProcessMemoryBytes = 24 * GiB;
	FMantleBudgetUse Use;
	Use.TextureBytes = 12 * GiB;
	TestEqual(TEXT("12 GB of textures against 8: a count pressure of 1.5"), FMantleStreamBudget::CountPressure(Limits, Use), 1.5);
	Use.ProcessMemoryShare = 0.35;
	Use.ProcessMemoryBytes = 30 * GiB;
	TestEqual(TEXT("30 GB against the 24 GB cap beats half the share"), FMantleStreamBudget::MemoryPressure(Limits, Use), 1.25);
	Use.ProcessMemoryBytes = 0;
	TestEqual(TEXT("bytes unknown: the share alone"), FMantleStreamBudget::MemoryPressure(Limits, Use), 0.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleStreamBudgetTextureDetailTest,
	"Mantle.Runtime.StreamBudget.TextureDetailReach",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleStreamBudgetTextureDetailTest::RunTest(const FString& Parameters)
{
	// #189: below the ladder. Full texture detail reaches 150 m until textures are over their limit with the ladder already at 0; then x0.75 a step, to 25 m at the least.
	const FMantleBudgetTuning Tuning;
	auto Next = [&Tuning](const double Reach, const double Ladder, const double Textures, const double Pressure, const double Video = 0.0, const double TexturesOfVideo = 0.0)
	{
		return FMantleStreamBudget::NextTextureDetailReach(Reach, Ladder, Textures, Video, TexturesOfVideo, Pressure, Tuning);
	};
	TestEqual(TEXT("over, but the ladder can still fall: the cells go first"), Next(15000.0, 0.3, 1.5, 1.5), 15000.0);
	TestEqual(TEXT("over at ladder 0: 150 m to 112.5 m"), Next(15000.0, 0.0, 1.2, 1.2), 11250.0);
	TestEqual(TEXT("the textures' own pressure decides, not another count's"), Next(15000.0, 0.0, 0.5, 1.5), 15000.0);
	TestEqual(TEXT("never under 25 m"), Next(3000.0, 0.0, 3.0, 3.0), 2500.0);
	TestEqual(TEXT("between 0.8 and 1 it holds"), Next(8437.5, 0.0, 0.9, 0.9), 8437.5);
	// Back up only if the textures would fit with the circle's area grown by 1 / 0.75^2 (1.78): 0.77 would be 1.37, so it holds; 0.5 would be 0.89, so it rises.
	TestEqual(TEXT("just under 0.8 after a step down: it holds rather than cycle"), Next(8437.5, 0.0, 0.77, 0.77), 8437.5);
	TestEqual(TEXT("room for the step: 84 m to 112.5 m"), Next(8437.5, 0.0, 0.5, 0.5), 11250.0);
	TestEqual(TEXT("never past 150 m"), Next(14000.0, 0.0, 0.1, 0.1), 15000.0);
	TestEqual(TEXT("whole already: stays whole"), Next(15000.0, 1.0, 0.1, 0.1), 15000.0);
	// The card: over at ladder 0 with the textures inside their own share (a crowded server) shortens it too; back up only if the card would fit the textures' growth.
	TestEqual(TEXT("the card over at ladder 0: 150 m to 112.5 m"), Next(15000.0, 0.0, 0.5, 1.1, 1.1, 0.3), 11250.0);
	TestEqual(TEXT("the card over, ladder still above 0: the cells go first"), Next(15000.0, 0.2, 0.5, 1.1, 1.1, 0.3), 15000.0);
	TestEqual(TEXT("card at 0.7 with textures 0.4 of it: 0.7 + 0.4 x 0.78 = 1.01, it holds"), Next(8437.5, 0.0, 0.3, 0.7, 0.7, 0.4), 8437.5);
	TestEqual(TEXT("card at 0.5 with textures 0.4 of it: 0.81, it rises"), Next(8437.5, 0.0, 0.3, 0.5, 0.5, 0.4), 11250.0);

	// The card's pressure: what the driver says is used over 90% of the budget it gives; nothing when it cannot say.
	FMantleBudgetLimits Limits;
	FMantleBudgetUse Card;
	TestEqual(TEXT("an unknown card does not press"), FMantleStreamBudget::VideoPressure(Limits, Card), 0.0);
	Card.VideoBudgetBytes = 10000;
	Card.VideoUsedBytes = 9000;
	TestEqual(TEXT("9 of a 10 budget is 1 at the 90% cushion"), FMantleStreamBudget::VideoPressure(Limits, Card), 1.0);
	Card.VideoUsedBytes = 4500;
	TestEqual(TEXT("4.5 of 10 is 0.5"), FMantleStreamBudget::VideoPressure(Limits, Card), 0.5);
	TestEqual(TEXT("and it is part of the pressure"), FMantleStreamBudget::Pressure(Limits, Card), 0.5);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
