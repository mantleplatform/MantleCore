// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCellArea.h"
#include "MantleCollisionGate.h"
#include "MantleReflectionProbe.h"

#include "Engine/World.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The collision gate's rule, the cell area answers it and the reflection probes read, and the gate against a world whose streamer is a fake.

namespace MantleCollisionGateTestPrivate
{
	FMantleCellAreaState State(const int32 Cells, const int32 Built, const int32 Failed = 0, const int32 Pending = 0, const bool bUnknown = false, const bool bStale = false)
	{
		FMantleCellAreaState Result;
		Result.Cells = Cells;
		Result.Built = Built;
		Result.Failed = Failed;
		Result.Pending = Pending;
		Result.bUnknown = bUnknown;
		Result.bPlanStale = bStale;
		return Result;
	}

	/** The same with the indices of the cells that stand (a probe's question). */
	FMantleCellAreaState StateIds(const int32 Cells, const TArray<int32>& Standing, const int32 Pending = 0, const bool bStale = false)
	{
		FMantleCellAreaState Result = State(Cells, Standing.Num(), 0, Pending, false, bStale);
		Result.BuiltCellIds = Standing;
		return Result;
	}

	/** A game world for one test, destroyed with it; the fake streamer answers the gate's questions from Stands. */
	struct FGateWorld
	{
		UWorld* World = nullptr;
		bool bStands = true;
		/** False: the world builds no cells for the question (a place that streams nothing): the resolver says so. */
		bool bBuildsCells = true;
		int32 Asks = 0;
		FMantleCellArea::FResolver Before;

		FGateWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(*FString::Printf(TEXT("MantleGateTestWorld_%s"), *FGuid::NewGuid().ToString())));
			Before = FMantleCellArea::Resolver;
			FMantleCellArea::Resolver = [this](UWorld*, const FMantleCellAreaQuery& Query, FMantleCellAreaState& OutState, uint64& OutSerial)
			{
				++Asks;
				OutState = State(2, bStands ? 2 : 1);
				OutSerial = 1;
				return bBuildsCells && Query.bSolidOnly;
			};
		}

		~FGateWorld()
		{
			FMantleCellArea::Resolver = Before;
			if (World)
			{
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
				if (World->IsRooted())
				{
					World->RemoveFromRoot();
				}
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCollisionGateRuleTest,
	"Mantle.Runtime.CollisionGate.Rule",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCollisionGateRuleTest::RunTest(const FString& Parameters)
{
	// The radius: the body's own size, 500 cm of margin, and half a second of travel.
	TestEqual(TEXT("a still body of 100: 100 + 500"), FMantleCollisionGate::RadiusCm(100.0, 0.0), 600.0);
	TestEqual(TEXT("one at 2000 cm/s: 100 + 500 + 1000"), FMantleCollisionGate::RadiusCm(100.0, 2000.0), 1600.0);
	TestEqual(TEXT("a negative speed and size count as none"), FMantleCollisionGate::RadiusCm(-50.0, -10.0), 500.0);

	// The rule: closes at once when the ground does not stand; opens only after it has stood 0.5 s in a row.
	double Since = -1.0;
	bool bClosed = false;
	bClosed = FMantleCollisionGate::NextClosed(bClosed, true, 0.0, Since);
	TestFalse(TEXT("ground stands: open"), bClosed);
	bClosed = FMantleCollisionGate::NextClosed(bClosed, false, 1.0, Since);
	TestTrue(TEXT("the ground goes: closed at once"), bClosed);
	TestTrue(TEXT("and the run of standing is forgotten"), Since < 0.0);
	bClosed = FMantleCollisionGate::NextClosed(bClosed, true, 2.0, Since);
	TestTrue(TEXT("the ground is back at t = 2.0: still closed"), bClosed);
	TestEqual(TEXT("and the run began at 2.0"), Since, 2.0);
	bClosed = FMantleCollisionGate::NextClosed(bClosed, true, 2.4, Since);
	TestTrue(TEXT("0.4 s of ground: still closed"), bClosed);
	bClosed = FMantleCollisionGate::NextClosed(bClosed, false, 2.45, Since);
	TestTrue(TEXT("the ground blinks out: closed, the run starts again"), bClosed && Since < 0.0);
	bClosed = FMantleCollisionGate::NextClosed(bClosed, true, 3.0, Since);
	bClosed = FMantleCollisionGate::NextClosed(bClosed, true, 3.49, Since);
	TestTrue(TEXT("0.49 s of ground: closed"), bClosed);
	bClosed = FMantleCollisionGate::NextClosed(bClosed, true, 3.5, Since);
	TestFalse(TEXT("0.5 s of ground: open"), bClosed);
	bClosed = FMantleCollisionGate::NextClosed(bClosed, true, 3.6, Since);
	TestFalse(TEXT("and it stays open while the ground stands"), bClosed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellAreaStateTest,
	"Mantle.Runtime.CollisionGate.AreaAnswers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellAreaStateTest::RunTest(const FString& Parameters)
{
	using namespace MantleCollisionGateTestPrivate;

	// A body's question (solid cells): every cell asked about must stand. A cell that can never be read does not, and neither does an index not read yet.
	TestTrue(TEXT("two of two stand"), State(2, 2).AllStand());
	TestFalse(TEXT("one of two does not"), State(2, 1).AllStand());
	TestFalse(TEXT("a failed cell is not standing: the body keeps following the server"), State(2, 1, /*Failed*/ 1).AllStand());
	TestTrue(TEXT("no solid cell near it at all: nothing to wait for"), State(0, 0).AllStand());
	TestFalse(TEXT("an index not read yet: nothing stands"), State(0, 0, 0, 0, /*bUnknown*/ true).AllStand());
	TestFalse(TEXT("an unknown answer with cells: no"), State(2, 2, 0, 0, true).AllStand());
	// A plan that is stale does not matter to a body: the cells it needs stand or they do not.
	TestTrue(TEXT("a stale plan, all standing: stands"), State(2, 2, 0, 0, false, /*bStale*/ true).AllStand());

	// A probe's question: nothing near is still on its way and at least one stands (or there is no cell near it: its place is the root's), on a plan that is known.
	TestTrue(TEXT("three cells, all standing: ready"), State(3, 3).ReadyToCapture());
	TestTrue(TEXT("a failed neighbour does not hold it back"), State(3, 2, 1).ReadyToCapture());
	TestFalse(TEXT("one still on its way: not ready"), State(3, 2, 0, 1).ReadyToCapture());
	TestFalse(TEXT("cells near it and none standing: not ready (never a capture before its cells came)"), State(3, 0).ReadyToCapture());
	TestFalse(TEXT("all of them failed: nothing stands, not ready"), State(2, 0, 2).ReadyToCapture());
	TestTrue(TEXT("no cell near it: ready"), State(0, 0).ReadyToCapture());
	TestFalse(TEXT("a plan that is stale: not yet"), State(3, 3, 0, 0, false, true).ReadyToCapture());
	TestFalse(TEXT("an index not read: not ready"), State(0, 0, 0, 0, true).ReadyToCapture());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleProbeRecaptureTest,
	"Mantle.Runtime.CollisionGate.ProbeReachAndRecapture",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleProbeRecaptureTest::RunTest(const FString& Parameters)
{
	using namespace MantleCollisionGateTestPrivate;

	// A box reaches its largest half extent, a sphere its radius.
	FMantleReflectionProbeProps Box;
	Box.Shape = EMantleReflectionProbeShape::Box;
	Box.HalfExtent = FVector(1000.0, 2500.0, 400.0);
	TestEqual(TEXT("a box reaches 2500"), FMantleReflectionProbes::ReachCm(Box), 2500.0);
	FMantleReflectionProbeProps Sphere;
	Sphere.Shape = EMantleReflectionProbeShape::Sphere;
	Sphere.InfluenceRadius = 1800.0;
	Sphere.HalfExtent = FVector(9999.0);
	TestEqual(TEXT("a sphere reaches its radius, whatever its box says"), FMantleReflectionProbes::ReachCm(Sphere), 1800.0);

	// A cell that no capture saw has been built, with everything near ready: capture again. The cells are named by index; its captures have seen cells 4 and 5.
	const TSet<int32> Seen({ 4, 5 });
	TestTrue(TEXT("4 and 5 were seen, 4, 5 and 6 stand: again (6 is new)"), FMantleReflectionProbes::WantsRecapture(Seen, StateIds(3, { 4, 5, 6 })));
	TestFalse(TEXT("4 and 5 were seen, 4 and 5 stand: no"), FMantleReflectionProbes::WantsRecapture(Seen, StateIds(2, { 4, 5 })));
	TestFalse(TEXT("cell 5 was taken down (only 4 stands): its picture keeps what it had"), FMantleReflectionProbes::WantsRecapture(Seen, StateIds(2, { 4 })));
	TestTrue(TEXT("a different cell standing in place of cell 5 is new"), FMantleReflectionProbes::WantsRecapture(Seen, StateIds(2, { 4, 7 })));
	TestFalse(TEXT("6 is new but another cell is still on its way: wait for it"), FMantleReflectionProbes::WantsRecapture(Seen, StateIds(4, { 4, 5, 6 }, /*Pending*/ 1)));
	TestFalse(TEXT("a plan that is stale: wait"), FMantleReflectionProbes::WantsRecapture(Seen, StateIds(3, { 4, 5, 6 }, 0, /*bStale*/ true)));
	TestFalse(TEXT("a world with no cells has none to come"), FMantleReflectionProbes::WantsRecapture(Seen, State(0, 0)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCollisionGateWorldTest,
	"Mantle.Runtime.CollisionGate.AsksTheStreamer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCollisionGateWorldTest::RunTest(const FString& Parameters)
{
	using namespace MantleCollisionGateTestPrivate;
	FGateWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	FMantleCollisionGate Gate;
	const FVector At(1000.0, 2000.0, 300.0);

	// A world that builds no cells for the question (a place that streams nothing; a dedicated server merges every cell): the ground always stands, the gate never closes.
	Fixture.bBuildsCells = false;
	Fixture.bStands = false;
	TestFalse(TEXT("no cells in this world: no change"), Gate.Update(Fixture.World, At, 100.0, 0.0, TEXT("prop"), TEXT("p")));
	TestFalse(TEXT("and never closed"), Gate.IsClosed());
	TestEqual(TEXT("asked once, and the answer was that there are no cells"), Fixture.Asks, 1);
	Fixture.bBuildsCells = true;
	Fixture.Asks = 0;

	// As a client: ground standing keeps it open; the ground going closes it on the next ask (0.1 s on), at once and once.
	Fixture.World->TimeSeconds = 10.0;
	Fixture.bStands = true;
	TestFalse(TEXT("ground stands: open, no change"), Gate.UpdateWhenClient(Fixture.World, At, 100.0, 0.0, TEXT("prop"), TEXT("p")));
	TestEqual(TEXT("asked once"), Fixture.Asks, 1);
	Fixture.bStands = false;
	TestFalse(TEXT("the ground went but the last ask is under 0.1 s old: not asked again"), Gate.UpdateWhenClient(Fixture.World, At, 100.0, 0.0, TEXT("prop"), TEXT("p")));
	TestEqual(TEXT("so one ask"), Fixture.Asks, 1);
	Fixture.World->TimeSeconds = 10.11;
	TestTrue(TEXT("0.11 s on: asked, closed, changed"), Gate.UpdateWhenClient(Fixture.World, At, 100.0, 0.0, TEXT("prop"), TEXT("p")));
	TestTrue(TEXT("closed"), Gate.IsClosed());
	Fixture.World->TimeSeconds = 10.22;
	TestFalse(TEXT("still no ground: no change"), Gate.UpdateWhenClient(Fixture.World, At, 100.0, 0.0, TEXT("prop"), TEXT("p")));

	// A body that moved 2 m is asked at once, even inside the interval.
	const int32 AsksBefore = Fixture.Asks;
	Gate.UpdateWhenClient(Fixture.World, At + FVector(250.0, 0.0, 0.0), 100.0, 0.0, TEXT("prop"), TEXT("p"));
	TestEqual(TEXT("it moved 250 cm: asked again in the same instant"), Fixture.Asks, AsksBefore + 1);

	// The ground is back at 10.3; it stays closed until it has stood for 0.5 s of asks, then opens, changed once.
	Fixture.bStands = true;
	Fixture.World->TimeSeconds = 10.33;
	TestFalse(TEXT("ground back: closed still (the run of standing begins)"), Gate.UpdateWhenClient(Fixture.World, At, 100.0, 0.0, TEXT("prop"), TEXT("p")));
	TestTrue(TEXT("closed"), Gate.IsClosed());
	Fixture.World->TimeSeconds = 10.6;
	TestFalse(TEXT("0.27 s of ground: closed"), Gate.UpdateWhenClient(Fixture.World, At, 100.0, 0.0, TEXT("prop"), TEXT("p")));
	Fixture.World->TimeSeconds = 10.84;
	TestTrue(TEXT("0.51 s of ground: opens"), Gate.UpdateWhenClient(Fixture.World, At, 100.0, 0.0, TEXT("prop"), TEXT("p")));
	TestFalse(TEXT("open"), Gate.IsClosed());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
