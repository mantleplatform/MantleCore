// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCells.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceInstancing.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Cells (large places, stage 1; BP\contracts\cells-format.md): the exporter's planner, cell files, index and root pointer, on built
// places with no level and no network. Expected numbers are worked out by hand from the contract's default grid:
// level 0 is 6,400 cm cells for content drawn up to 15,000 cm, level 1 is 25,600 cm to 60,000, level 2 is 102,400 cm to 250,000.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleCellsTestPrivate
{
	const TCHAR* const MeshA = TEXT("ast_u0123456789abcdef0123456789abcdef");
	const TCHAR* const MeshB = TEXT("ast_u11111111111111111111111111111111");

	FMantleEntity MakeMesh(const FString& Id, const TCHAR* Mesh, const FVector& Location, const double DrawDistance = 9000.0)
	{
		FMantleStaticMeshProps Props;
		Props.Mesh = Mesh;
		Props.DrawDistance = DrawDistance;
		FMantleEntity Entity;
		Entity.Id = Id;
		Entity.Name = Id;
		Entity.Type = EMantleEntityType::StaticMesh;
		Entity.Transform = FTransform(FRotator(0.0, 10.0, 0.0), Location, FVector::OneVector);
		Entity.Props.Set<FMantleStaticMeshProps>(MoveTemp(Props));
		return Entity;
	}

	FMantleEntity MakeGroup(const FString& Id, const FVector& Location, const FString& Parent = FString())
	{
		FMantleEntity Entity;
		Entity.Id = Id;
		Entity.Name = Id;
		Entity.Type = EMantleEntityType::Group;
		Entity.Parent = Parent;
		Entity.Transform = FTransform(FRotator::ZeroRotator, Location, FVector::OneVector);
		return Entity;
	}

	FMantleInstancedMeshGroup MakeGroupOf(const TCHAR* Mesh, const double DrawDistance, const TArray<FVector>& Locations)
	{
		FMantleInstancedMeshGroup Group;
		Group.Mesh.Mesh = Mesh;
		Group.Mesh.DrawDistance = DrawDistance;
		Group.MeshName = TEXT("Rock");
		for (const FVector& Location : Locations)
		{
			Group.WorldInstances.Add(FTransform(FRotator(0.0, 45.0, 0.0), Location, FVector(1.0, 1.0, 1.0)));
		}
		return Group;
	}

	/** The cell at (Level, X, Y), or null. */
	const FMantlePlannedCell* FindCell(const FMantleCellPlan& Plan, const int32 Level, const int32 X, const int32 Y)
	{
		return Plan.Cells.FindByPredicate([&](const FMantlePlannedCell& Cell) { return Cell.Key.Level == Level && Cell.Key.X == X && Cell.Key.Y == Y; });
	}

	bool HasEntity(const FMantlePlace& Place, const FString& Id)
	{
		return Place.Entities.ContainsByPredicate([&Id](const FMantleEntity& Entity) { return Entity.Id == Id; });
	}

	/** Every cell's document, joined: what determinism is compared on. */
	FString AllDocuments(const FMantleCellPlan& Plan)
	{
		FString All;
		for (const FMantlePlannedCell& Cell : Plan.Cells)
		{
			All += Cell.Key.ToString() + TEXT("\n") + FMantleCellFiles::CellDocument(Cell.Entities) + TEXT("\n");
		}
		return All;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellGridTest,
	"Mantle.Editor.Cells.Grid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellGridTest::RunTest(const FString& Parameters)
{
	const FMantleCellGrid Grid = FMantleCellGrid::Default();
	TestTrue(TEXT("the default grid follows the format's rules"), Grid.IsValid());
	TestEqual(TEXT("three levels"), Grid.Levels.Num(), 3);
	TestEqual(TEXT("the top draw distance is 2.5 km"), Grid.TopMaxDrawDistance(), static_cast<int64>(250000));

	// The smallest level whose maxDrawDistance is at least the draw distance (4.1).
	TestEqual(TEXT("100 m is level 0"), Grid.LevelFor(10000.0), 0);
	TestEqual(TEXT("exactly 150 m is level 0"), Grid.LevelFor(15000.0), 0);
	TestEqual(TEXT("150.01 m is level 1"), Grid.LevelFor(15000.01), 1);
	TestEqual(TEXT("600 m is level 1"), Grid.LevelFor(60000.0), 1);
	TestEqual(TEXT("2.5 km is level 2"), Grid.LevelFor(250000.0), 2);
	TestEqual(TEXT("farther than the top level is none"), Grid.LevelFor(250001.0), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("no draw distance is none"), Grid.LevelFor(0.0), static_cast<int32>(INDEX_NONE));

	// floor(pivot / cellSize), negatives included, and a pivot on an edge belongs to the cell that starts there.
	const FMantleCellKey Origin = Grid.KeyFor(0, 0.0, 0.0);
	TestTrue(TEXT("the origin is cell (0,0)"), Origin.X == 0 && Origin.Y == 0);
	const FMantleCellKey Edge = Grid.KeyFor(0, 6400.0, 12800.0);
	TestTrue(TEXT("an edge is the next cell"), Edge.X == 1 && Edge.Y == 2);
	const FMantleCellKey Just = Grid.KeyFor(0, 6399.99, -0.01);
	TestTrue(TEXT("just below an edge, and just below zero"), Just.X == 0 && Just.Y == -1);
	const FMantleCellKey Big = Grid.KeyFor(2, -204800.0, 307200.0);
	TestTrue(TEXT("level 2 is 1,024 m cells"), Big.Level == 2 && Big.X == -2 && Big.Y == 3);

	// The rules the format sets on a grid.
	FMantleCellGrid Bad = Grid;
	Bad.Levels[1].CellSize = Bad.Levels[0].CellSize;
	TestFalse(TEXT("cell sizes must rise"), Bad.IsValid());
	Bad = Grid;
	Bad.Levels[0].CellSize = 999;
	TestFalse(TEXT("a cell under 10 m is refused"), Bad.IsValid());
	Bad = Grid;
	Bad.Levels[2].MaxDrawDistance = 1000001;
	TestFalse(TEXT("a draw distance past the cap is refused"), Bad.IsValid());
	Bad = FMantleCellGrid();
	TestFalse(TEXT("no levels is refused"), Bad.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellPlannerRulesTest,
	"Mantle.Editor.Cells.PlannerRules",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellPlannerRulesTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellsTestPrivate;
	FMantlePlace Place;
	TArray<FMantleInstancedMeshGroup> Groups;
	FMantleCellPlanInput Input;
	Input.bFoldStaticMeshes = false;

	// Eligible: drawn up to 90 m, in cell (0,0) of level 0, and one drawn up to 400 m in level 1.
	Place.Entities.Add(MakeMesh(TEXT("ent_near"), MeshA, FVector(100.0, 200.0, 50.0), 9000.0));
	Place.Entities.Add(MakeMesh(TEXT("ent_mid"), MeshA, FVector(30000.0, -100.0, 0.0), 40000.0));
	// Stay in the root, each for its own reason.
	Place.Entities.Add(MakeMesh(TEXT("ent_nodistance"), MeshA, FVector(1.0, 1.0, 0.0), 0.0));
	Place.Entities.Add(MakeMesh(TEXT("ent_toofar"), MeshA, FVector(1.0, 1.0, 0.0), 300000.0));
	FMantleEntity Tagged = MakeMesh(TEXT("ent_tagged"), MeshA, FVector(2.0, 2.0, 0.0));
	Tagged.Tags.Add(TEXT("door"));
	Place.Entities.Add(Tagged);
	FMantleEntity Dynamic = MakeMesh(TEXT("ent_dynamic"), MeshA, FVector(3.0, 3.0, 0.0));
	Dynamic.bDynamic = true;
	Place.Entities.Add(Dynamic);
	FMantleEntity Physical = MakeMesh(TEXT("ent_physics"), MeshA, FVector(4.0, 4.0, 0.0));
	Physical.Props.Get<FMantleStaticMeshProps>().Physics = FMantlePhysicsProps();
	Place.Entities.Add(Physical);
	Place.Entities.Add(MakeMesh(TEXT("ent_seat"), MeshA, FVector(5.0, 5.0, 0.0)));
	FMantleSeat Seat;
	Seat.Entity = TEXT("ent_seat");
	Place.Seats.Add(Seat);
	Place.Entities.Add(MakeMesh(TEXT("Gate"), MeshA, FVector(6.0, 6.0, 0.0)));
	Input.ScriptLiterals.Add(TEXT("Gate"));
	Place.Entities.Add(MakeMesh(TEXT("ent_unloaded"), MeshA, FVector(7.0, 7.0, 0.0)));
	Input.NotSpatiallyLoaded.Add(TEXT("ent_unloaded"));
	FMantleEntity Light;
	Light.Id = TEXT("ent_light");
	Light.Type = EMantleEntityType::PointLight;
	Place.Entities.Add(Light);
	// A parent of something stays; so does the child, because its parent is not a plain Group.
	Place.Entities.Add(MakeMesh(TEXT("ent_parent"), MeshA, FVector(8.0, 8.0, 0.0)));
	FMantleEntity Child = MakeMesh(TEXT("ent_child"), MeshA, FVector(0.0, 0.0, 10.0));
	Child.Parent = TEXT("ent_parent");
	Place.Entities.Add(Child);

	const FMantleCellPlan Plan = FMantleCellPlanner::Plan(Place, Groups, Input);
	TestEqual(TEXT("two cells"), Plan.Cells.Num(), 2);
	const FMantlePlannedCell* Near = FindCell(Plan, 0, 0, 0);
	const FMantlePlannedCell* Mid = FindCell(Plan, 1, 1, -1);
	if (!TestNotNull(TEXT("level 0 cell (0,0)"), Near) || !TestNotNull(TEXT("level 1 cell (1,-1): 30000/25600 and -100/25600"), Mid))
	{
		return false;
	}
	TestEqual(TEXT("ent_near is in it"), Near->Entities.Num(), 1);
	TestEqual(TEXT("with its id"), Near->Entities[0].Id, FString(TEXT("ent_near")));
	TestEqual(TEXT("the cell's draw distance"), Near->DrawDistance, 9000.0);
	TestEqual(TEXT("level 1's"), Mid->DrawDistance, 40000.0);

	for (const TCHAR* Stays : { TEXT("ent_nodistance"), TEXT("ent_toofar"), TEXT("ent_tagged"), TEXT("ent_dynamic"), TEXT("ent_physics"), TEXT("ent_seat"),
		TEXT("Gate"), TEXT("ent_unloaded"), TEXT("ent_light"), TEXT("ent_parent"), TEXT("ent_child") })
	{
		TestTrue(FString::Printf(TEXT("%s stays in the root"), Stays), HasEntity(Place, Stays));
	}
	TestFalse(TEXT("ent_near left"), HasEntity(Place, TEXT("ent_near")));

	const FMantleCellReport& Report = Plan.Report;
	TestEqual(TEXT("entities in cells"), Report.CellEntities, static_cast<int64>(2));
	TestEqual(TEXT("cells at level 0"), Report.CellsPerLevel[0], 1);
	TestEqual(TEXT("cells at level 1"), Report.CellsPerLevel[1], 1);
	TestEqual(TEXT("one has no draw distance"), Report.StayCounts[static_cast<int32>(EMantleCellStay::NoDrawDistance)], static_cast<int64>(1));
	TestEqual(TEXT("one is drawn too far"), Report.StayCounts[static_cast<int32>(EMantleCellStay::DrawDistanceTooFar)], static_cast<int64>(1));
	TestEqual(TEXT("one is tagged"), Report.StayCounts[static_cast<int32>(EMantleCellStay::Tagged)], static_cast<int64>(1));
	TestEqual(TEXT("one is movable"), Report.StayCounts[static_cast<int32>(EMantleCellStay::Dynamic)], static_cast<int64>(1));
	TestEqual(TEXT("one has physics"), Report.StayCounts[static_cast<int32>(EMantleCellStay::Physics)], static_cast<int64>(1));
	TestEqual(TEXT("a seat and a parent are named"), Report.StayCounts[static_cast<int32>(EMantleCellStay::Named)], static_cast<int64>(2));
	TestEqual(TEXT("one is a script's literal"), Report.StayCounts[static_cast<int32>(EMantleCellStay::ScriptLiteral)], static_cast<int64>(1));
	TestEqual(TEXT("one is not spatially loaded"), Report.StayCounts[static_cast<int32>(EMantleCellStay::NotSpatiallyLoaded)], static_cast<int64>(1));
	TestEqual(TEXT("the light and nothing else is another type"), Report.StayCounts[static_cast<int32>(EMantleCellStay::OtherType)], static_cast<int64>(1));
	TestEqual(TEXT("the child's parent is not plain"), Report.StayCounts[static_cast<int32>(EMantleCellStay::ParentNotPlain)], static_cast<int64>(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellPlannerGroupsTest,
	"Mantle.Editor.Cells.PlainGroupsAreFlattened",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellPlannerGroupsTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellsTestPrivate;
	FMantlePlace Place;
	TArray<FMantleInstancedMeshGroup> Groups;
	FMantleCellPlanInput Input;
	Input.bFoldStaticMeshes = false;

	// A house: Group at (1000, 2000, 0) turned 90 degrees, holding a wall at (100, 0, 0) relative to it. The wall's world location is the
	// group's location plus (100,0,0) turned 90 degrees about Z, which is (0, 100, 0): (1000, 2100, 0).
	FMantleEntity House = MakeGroup(TEXT("grp_house"), FVector(1000.0, 2000.0, 0.0));
	House.Transform.SetRotation(FQuat(FRotator(0.0, 90.0, 0.0)));
	Place.Entities.Add(House);
	FMantleEntity Wall = MakeMesh(TEXT("ent_wall"), MeshA, FVector(100.0, 0.0, 0.0));
	Wall.Parent = TEXT("grp_house");
	Wall.Transform.SetRotation(FQuat::Identity);
	Place.Entities.Add(Wall);
	// A group that something else names stays, and so does what hangs under it.
	Place.Entities.Add(MakeGroup(TEXT("grp_named"), FVector::ZeroVector));
	FMantleEntity Under = MakeMesh(TEXT("ent_under"), MeshA, FVector(50.0, 0.0, 0.0));
	Under.Parent = TEXT("grp_named");
	Place.Entities.Add(Under);
	FMantleSeat Seat;
	Seat.Entity = TEXT("grp_named");
	Place.Seats.Add(Seat);
	// A group under a plain group (nested) with a mesh in it.
	Place.Entities.Add(MakeGroup(TEXT("grp_outer"), FVector(0.0, 12800.0, 0.0)));
	Place.Entities.Add(MakeGroup(TEXT("grp_inner"), FVector(10.0, 0.0, 0.0), TEXT("grp_outer")));
	FMantleEntity Deep = MakeMesh(TEXT("ent_deep"), MeshA, FVector(5.0, 0.0, 0.0));
	Deep.Parent = TEXT("grp_inner");
	Place.Entities.Add(Deep);

	const FMantleCellPlan Plan = FMantleCellPlanner::Plan(Place, Groups, Input);

	const FMantlePlannedCell* WallCell = FindCell(Plan, 0, 0, 0);
	if (!TestNotNull(TEXT("the wall's cell"), WallCell))
	{
		return false;
	}
	const FMantleEntity* WallEntity = WallCell->Entities.FindByPredicate([](const FMantleEntity& Entity) { return Entity.Id == TEXT("ent_wall"); });
	if (!TestNotNull(TEXT("the wall is in it"), WallEntity))
	{
		return false;
	}
	TestTrue(TEXT("its parent is gone"), WallEntity->Parent.IsEmpty());
	TestTrue(TEXT("its world X is 1000"), FMath::IsNearlyEqual(WallEntity->Transform.GetLocation().X, 1000.0, 0.01));
	TestTrue(TEXT("its world Y is 2100"), FMath::IsNearlyEqual(WallEntity->Transform.GetLocation().Y, 2100.0, 0.01));
	TestTrue(TEXT("it faces the way the group faces"), FMath::IsNearlyEqual(WallEntity->Transform.Rotator().Yaw, 90.0, 0.01));
	TestFalse(TEXT("the emptied group is dropped"), HasEntity(Place, TEXT("grp_house")));
	TestEqual(TEXT("three groups were dropped: the house, the inner and the outer"), Plan.Report.DroppedGroups, 3);

	TestTrue(TEXT("the named group stays"), HasEntity(Place, TEXT("grp_named")));
	TestTrue(TEXT("and what hangs under it"), HasEntity(Place, TEXT("ent_under")));

	// The nested pair: the mesh moves to a cell by its world position (10 + 5, 12800), the inner group goes, then the outer.
	TestFalse(TEXT("the inner group is dropped"), HasEntity(Place, TEXT("grp_inner")));
	TestFalse(TEXT("then the outer group"), HasEntity(Place, TEXT("grp_outer")));
	const FMantlePlannedCell* DeepCell = FindCell(Plan, 0, 0, 2);
	TestNotNull(TEXT("12,800 / 6,400 = row 2"), DeepCell);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellPlannerFlatTransformTest,
	"Mantle.Editor.Cells.ShearedGroupsStayInTheRoot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellPlannerFlatTransformTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellsTestPrivate;
	FMantlePlace Place;
	TArray<FMantleInstancedMeshGroup> Groups;
	FMantleCellPlanInput Input;
	Input.bFoldStaticMeshes = false;

	// A group scaled (1, 3, 1) holding a mesh turned 30 degrees: the composition is a shear, which a transform cannot say.
	FMantleEntity Skewed = MakeGroup(TEXT("grp_skewed"), FVector::ZeroVector);
	Skewed.Transform.SetScale3D(FVector(1.0, 3.0, 1.0));
	Place.Entities.Add(Skewed);
	FMantleEntity Turned = MakeMesh(TEXT("ent_turned"), MeshA, FVector(10.0, 0.0, 0.0));
	Turned.Parent = TEXT("grp_skewed");
	Turned.Transform.SetRotation(FQuat(FRotator(0.0, 30.0, 0.0)));
	Place.Entities.Add(Turned);
	// The same group holding a mesh that is not turned: still exact.
	FMantleEntity Straight = MakeMesh(TEXT("ent_straight"), MeshA, FVector(10.0, 0.0, 0.0));
	Straight.Parent = TEXT("grp_skewed");
	Straight.Transform.SetRotation(FQuat::Identity);
	Place.Entities.Add(Straight);
	// A group scaled (-1, 1, 1): a mirror, which stays too.
	FMantleEntity Mirror = MakeGroup(TEXT("grp_mirror"), FVector::ZeroVector);
	Mirror.Transform.SetScale3D(FVector(-1.0, 1.0, 1.0));
	Place.Entities.Add(Mirror);
	FMantleEntity Mirrored = MakeMesh(TEXT("ent_mirrored"), MeshA, FVector(10.0, 0.0, 0.0));
	Mirrored.Parent = TEXT("grp_mirror");
	Place.Entities.Add(Mirrored);

	const FMantleCellPlan Plan = FMantleCellPlanner::Plan(Place, Groups, Input);
	TestTrue(TEXT("the sheared mesh stays"), HasEntity(Place, TEXT("ent_turned")));
	TestTrue(TEXT("with its group"), HasEntity(Place, TEXT("grp_skewed")));
	TestTrue(TEXT("the mirrored mesh stays"), HasEntity(Place, TEXT("ent_mirrored")));
	TestFalse(TEXT("the unturned mesh under the stretched group moves"), HasEntity(Place, TEXT("ent_straight")));
	TestEqual(TEXT("two could not be flattened"), Plan.Report.StayCounts[static_cast<int32>(EMantleCellStay::TransformNotFlat)], static_cast<int64>(2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellPlannerInstancesTest,
	"Mantle.Editor.Cells.InstancesAreSplitByCellAndFolded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellPlannerInstancesTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellsTestPrivate;
	FMantlePlace Place;
	FMantleCellPlanInput Input;

	// Four identical meshes in one cell fold into one instance set (MinGroupSize); three in another cell do not (counted per cell).
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Place.Entities.Add(MakeMesh(FString::Printf(TEXT("ent_a%d"), Index), MeshA, FVector(100.0 * Index, 50.0, 0.0)));
	}
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Place.Entities.Add(MakeMesh(FString::Printf(TEXT("ent_b%d"), Index), MeshA, FVector(6400.0 + 100.0 * Index, 50.0, 0.0)));
	}
	// An instanced group with instances in both cells and one in the root's far corner (draw distance 0: never moved).
	TArray<FMantleInstancedMeshGroup> Groups;
	Groups.Add(MakeGroupOf(MeshB, 9000.0, { FVector(10.0, 10.0, 0.0), FVector(20.0, 30.0, 0.0), FVector(6500.0, 10.0, 0.0), FVector(-10.0, 10.0, 0.0) }));
	Groups.Add(MakeGroupOf(MeshB, 0.0, { FVector(10.0, 10.0, 0.0) }));
	// Painted over a Mantle-unloaded host: stays.
	Groups[0].RootOnlyInstances.Add(FTransform(FVector(5.0, 5.0, 5.0)));

	const FMantleCellPlan Plan = FMantleCellPlanner::Plan(Place, Groups, Input);
	TestEqual(TEXT("three cells: (0,0), (1,0) and (-1,0)"), Plan.Cells.Num(), 3);
	TestEqual(TEXT("four meshes folded"), Plan.Report.FoldedActors, 4);

	const FMantlePlannedCell* Home = FindCell(Plan, 0, 0, 0);
	if (!TestNotNull(TEXT("cell (0,0)"), Home))
	{
		return false;
	}
	// MeshA's four folded instances are one set; MeshB's two are another: two InstancedMesh entities and nothing else.
	TestEqual(TEXT("two instance sets, no separate meshes"), Home->Entities.Num(), 2);
	TestEqual(TEXT("six instances"), Home->NumInstances(), static_cast<int64>(6));
	for (const FMantleEntity& Entity : Home->Entities)
	{
		TestEqual(TEXT("an InstancedMesh"), static_cast<int32>(Entity.Type), static_cast<int32>(EMantleEntityType::InstancedMesh));
		TestTrue(TEXT("its id has the cell's hashed shape"), Entity.Id.StartsWith(TEXT("ent_ism_")) && Entity.Id.EndsWith(TEXT("_0")) && Entity.Id.Len() == 8 + 24 + 2);
		const FVector Anchor = Entity.Transform.GetLocation();
		TestTrue(TEXT("a whole-cm anchor"), FMath::IsNearlyEqual(Anchor.X, FMath::RoundToDouble(Anchor.X)) && FMath::IsNearlyEqual(Anchor.Y, FMath::RoundToDouble(Anchor.Y)));
		TestTrue(TEXT("with no rotation or scale"), Entity.Transform.GetRotation().IsIdentity() && Entity.Transform.GetScale3D().Equals(FVector::OneVector));
	}
	const FMantlePlannedCell* East = FindCell(Plan, 0, 1, 0);
	if (!TestNotNull(TEXT("cell (1,0)"), East))
	{
		return false;
	}
	TestEqual(TEXT("the three stay separate meshes beside MeshB's one instance"), East->Entities.Num(), 4);
	const FMantlePlannedCell* West = FindCell(Plan, 0, -1, 0);
	TestNotNull(TEXT("cell (-1,0) holds the instance at -10"), West);

	// The root keeps the instances nothing could move.
	TestEqual(TEXT("the group with a draw distance has only its root-only instance left"), Groups[0].WorldInstances.Num(), 0);
	TestEqual(TEXT("and still has it"), Groups[0].RootOnlyInstances.Num(), 1);
	TestEqual(TEXT("the group with no draw distance is untouched"), Groups[1].WorldInstances.Num(), 1);
	TestEqual(TEXT("it was counted"), Plan.Report.StayCounts[static_cast<int32>(EMantleCellStay::NoDrawDistance)], static_cast<int64>(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellDeterminismTest,
	"Mantle.Editor.Cells.CellsDoNotDependOnLoadOrder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellDeterminismTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellsTestPrivate;
	// 200 instances across a few cells, in two different orders, must write the same bytes: stage 5 republishes by hash.
	TArray<FVector> Locations;
	FRandomStream Random(7);
	for (int32 Index = 0; Index < 200; ++Index)
	{
		Locations.Add(FVector(Random.FRandRange(-9000.0, 9000.0), Random.FRandRange(-9000.0, 9000.0), Random.FRandRange(0.0, 500.0)));
	}
	// Two at one place, to be ordered by their written numbers, not by which came first.
	Locations.Add(FVector(100.0, 100.0, 0.0));
	Locations.Add(FVector(100.0, 100.0, 0.0));

	auto Run = [&Locations](const bool bReversed, int32& OutCells)
	{
		FMantlePlace Place;
		TArray<FMantleInstancedMeshGroup> Groups;
		Groups.Add(MakeGroupOf(MeshB, 9000.0, Locations));
		// The twins get different yaws, so what separates them is a number the file writes.
		Groups[0].WorldInstances[200].SetRotation(FQuat(FRotator(0.0, 10.0, 0.0)));
		Groups[0].WorldInstances[201].SetRotation(FQuat(FRotator(0.0, 80.0, 0.0)));
		if (bReversed)
		{
			Algo::Reverse(Groups[0].WorldInstances);
		}
		const FMantleCellPlan Plan = FMantleCellPlanner::Plan(Place, Groups, FMantleCellPlanInput());
		OutCells = Plan.Cells.Num();
		return AllDocuments(Plan);
	};
	int32 CellsA = 0;
	int32 CellsB = 0;
	const FString A = Run(false, CellsA);
	const FString B = Run(true, CellsB);
	TestTrue(TEXT("the scatter reaches more than one cell"), CellsA > 1);
	TestEqual(TEXT("the same cells"), CellsA, CellsB);
	TestFalse(TEXT("there is something to compare"), A.IsEmpty());
	TestEqual(TEXT("the same instances in the opposite order write the same bytes"), A, B);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellChunksTest,
	"Mantle.Editor.Cells.ChunksAreCutAtTheEntityLimitWithFixedIds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellChunksTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellsTestPrivate;
	FMantlePlace Place;
	TArray<FMantleInstancedMeshGroup> Groups;
	TArray<FVector> Locations;
	// 5,000 instances in one 64 m cell: 4,096 then 904.
	for (int32 Index = 0; Index < 5000; ++Index)
	{
		Locations.Add(FVector((Index % 50) * 100.0, (Index / 50) * 60.0, 0.0));
	}
	Groups.Add(MakeGroupOf(MeshB, 15000.0, Locations));
	const FString Key = FMantlePlaceExporter::InstancedGroupKey(Groups[0].Mesh);
	const FMantleCellPlan Plan = FMantleCellPlanner::Plan(Place, Groups, FMantleCellPlanInput());

	int64 Total = 0;
	int32 Entities = 0;
	for (const FMantlePlannedCell& Cell : Plan.Cells)
	{
		for (const FMantleEntity& Entity : Cell.Entities)
		{
			const FMantleInstancedMeshProps* Props = Entity.Props.TryGet<FMantleInstancedMeshProps>();
			if (TestNotNull(TEXT("an instance set"), Props))
			{
				TestTrue(TEXT("at most 4,096"), Props->Instances.Num() <= FMantlePlaceLimits::MaxInstancesPerEntity);
				Total += Props->Instances.Num();
			}
			++Entities;
		}
	}
	TestEqual(TEXT("every instance is in a cell"), Total, static_cast<int64>(5000));
	TestTrue(TEXT("cut into more than one entity"), Entities > 1);

	// The id is ent_ism_ + the first 24 hex digits of MD5(key|L<level>|<x>|<y>) + _ + the chunk number (5.5).
	const FMantlePlannedCell& First = Plan.Cells[0];
	const FString Expected = FMantleCellPlanner::InstancedEntityId(Key, First.Key, 0);
	TestEqual(TEXT("the first chunk's id is the formula's"), First.Entities[0].Id, Expected);
	TestEqual(TEXT("the same call twice agrees"), FMantleCellPlanner::InstancedEntityId(Key, First.Key, 0), Expected);
	TestNotEqual(TEXT("another chunk, another id"), FMantleCellPlanner::InstancedEntityId(Key, First.Key, 1), Expected);
	FMantleCellKey Other = First.Key;
	Other.X += 1;
	TestNotEqual(TEXT("another cell, another id"), FMantleCellPlanner::InstancedEntityId(Key, Other, 0), Expected);
	TestTrue(TEXT("at most 36 characters and inside ^ent_[a-z0-9_]{1,40}$"), Expected.Len() <= 36);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellFilesTest,
	"Mantle.Editor.Cells.FilesRoundTripAndAreDeterministic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellFilesTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellsTestPrivate;
	FMantlePlace Place;
	TArray<FMantleInstancedMeshGroup> Groups;
	for (int32 Index = 0; Index < 6; ++Index)
	{
		Place.Entities.Add(MakeMesh(FString::Printf(TEXT("ent_m%d"), Index), MeshA, FVector(Index * 150.0, 20.0, 0.0)));
	}
	Place.Entities.Add(MakeMesh(TEXT("ent_loner"), MeshB, FVector(500.0, 500.0, 0.0)));
	Place.Entities.Last().Props.Get<FMantleStaticMeshProps>().Collision = EMantleCollision::None;
	FMantleCellPlanInput Input;
	Input.MeshBounds.Add(MeshA, FBox(FVector(-50.0, -50.0, 0.0), FVector(50.0, 50.0, 200.0)));
	const FMantleCellPlan Plan = FMantleCellPlanner::Plan(Place, Groups, Input);
	if (!TestEqual(TEXT("one cell"), Plan.Cells.Num(), 1))
	{
		return false;
	}
	const FMantlePlannedCell& Cell = Plan.Cells[0];
	TestEqual(TEXT("two entities: the fold and the loner"), Cell.Entities.Num(), 2);
	TestEqual(TEXT("six instances"), Cell.NumInstances(), static_cast<int64>(6));
	// solid: the folded set collides (6 instances), the loner does not.
	TestEqual(TEXT("six solid"), Cell.NumSolid(), static_cast<int64>(6));
	TestTrue(TEXT("the bounds hold the meshes' boxes"), Cell.Bounds.IsValid && Cell.Bounds.Max.Z >= 200.0 && Cell.Bounds.Min.X <= -50.0);

	TArray<FMantleCellFileBlob> Files;
	FString Error;
	if (!TestTrue(TEXT("the cell is written"), FMantleCellFiles::WriteCell(Cell, Files, Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestEqual(TEXT("one file"), Files.Num(), 1))
	{
		return false;
	}
	const FMantleCellFileBlob& File = Files[0];
	TestTrue(TEXT("MCF1 magic"), File.Bytes.Num() > 8 && File.Bytes[0] == 'M' && File.Bytes[1] == 'C' && File.Bytes[2] == 'F' && File.Bytes[3] == '1');
	TestTrue(TEXT("a sha256 hash"), File.Hash.StartsWith(TEXT("sha256:")) && File.Hash.Len() == 71);
	TestEqual(TEXT("its counts"), File.Entities, 2);
	TestEqual(TEXT("instances"), File.Instances, static_cast<int64>(6));
	TestEqual(TEXT("solid"), File.Solid, static_cast<int64>(6));

	// Round trip: the inflated JSON is the document, and it parses as one with the version and the entities.
	FString Json;
	if (!TestTrue(TEXT("the container reads back"), FMantleCellFiles::ReadContainer(File.Bytes, FMantleCellFiles::CellMagic(), FMantleCellFiles::MaxCellRawBytes, Json, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("the document is what was compressed"), Json, FMantleCellFiles::CellDocument(Cell.Entities));
	const uint32 Raw = static_cast<uint32>(File.Bytes[4]) | (static_cast<uint32>(File.Bytes[5]) << 8) | (static_cast<uint32>(File.Bytes[6]) << 16) | (static_cast<uint32>(File.Bytes[7]) << 24);
	TestEqual(TEXT("rawBytes is the UTF-8 length"), static_cast<int32>(Raw), FTCHARToUTF8(*Json).Length());
	TSharedPtr<FJsonObject> Root;
	if (TestTrue(TEXT("valid JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root)) && Root.IsValid())
	{
		TestEqual(TEXT("version 1"), static_cast<int32>(Root->GetNumberField(TEXT("version"))), 1);
		const TArray<TSharedPtr<FJsonValue>>* Entities = nullptr;
		TestTrue(TEXT("entities"), Root->TryGetArrayField(TEXT("entities"), Entities) && Entities && Entities->Num() == 2);
		if (Entities && Entities->Num() == 2)
		{
			TestTrue(TEXT("sorted by id"), (*Entities)[0]->AsObject()->GetStringField(TEXT("id")) < (*Entities)[1]->AsObject()->GetStringField(TEXT("id")));
			TestFalse(TEXT("parent is null in a cell"), (*Entities)[1]->AsObject()->HasTypedField<EJson::String>(TEXT("parent")));
		}
	}

	// Refusals a reader makes.
	FString Ignored;
	// The cut stream makes the engine log that it could not inflate it, which is the refusal working.
	AddExpectedError(TEXT("appUncompressMemoryZLIB failed"), EAutomationExpectedErrorFlags::Contains, 0);
	AddExpectedError(TEXT("Failed to uncompress memory"), EAutomationExpectedErrorFlags::Contains, 0);
	TestFalse(TEXT("the wrong magic"), FMantleCellFiles::ReadContainer(File.Bytes, FMantleCellFiles::IndexMagic(), FMantleCellFiles::MaxCellRawBytes, Ignored, Error));
	TestFalse(TEXT("a raw length over the cap"), FMantleCellFiles::ReadContainer(File.Bytes, FMantleCellFiles::CellMagic(), 10, Ignored, Error));
	TArray<uint8> Cut = File.Bytes;
	Cut.SetNum(Cut.Num() - 6);
	TestFalse(TEXT("a cut stream"), FMantleCellFiles::ReadContainer(Cut, FMantleCellFiles::CellMagic(), FMantleCellFiles::MaxCellRawBytes, Ignored, Error));
	TestFalse(TEXT("too short"), FMantleCellFiles::ReadContainer(TArray<uint8>({ 'M', 'C' }), FMantleCellFiles::CellMagic(), 100, Ignored, Error));

	// Determinism: the same level loaded in another order writes the same bytes.
	{
		FMantlePlace Again;
		TArray<FMantleInstancedMeshGroup> NoGroups;
		Again.Entities.Add(MakeMesh(TEXT("ent_loner"), MeshB, FVector(500.0, 500.0, 0.0)));
		Again.Entities.Last().Props.Get<FMantleStaticMeshProps>().Collision = EMantleCollision::None;
		for (int32 Index = 5; Index >= 0; --Index)
		{
			Again.Entities.Add(MakeMesh(FString::Printf(TEXT("ent_m%d"), Index), MeshA, FVector(Index * 150.0, 20.0, 0.0)));
		}
		const FMantleCellPlan AgainPlan = FMantleCellPlanner::Plan(Again, NoGroups, Input);
		TArray<FMantleCellFileBlob> AgainFiles;
		if (TestEqual(TEXT("one cell again"), AgainPlan.Cells.Num(), 1))
		{
			FMantleCellFiles::WriteCell(AgainPlan.Cells[0], AgainFiles, Error);
		}
		TestTrue(TEXT("the same hash from the other load order"), AgainFiles.Num() == 1 && AgainFiles[0].Hash == File.Hash);
	}

	// A cell too big for one file gets more (3.2): parts are filled in id order, whole entities only.
	FMantlePlannedCell Big;
	Big.Key = Cell.Key;
	for (int32 Index = 0; Index < 40; ++Index)
	{
		Big.Entities.Add(MakeMesh(FString::Printf(TEXT("ent_%03d"), Index), MeshA, FVector(Index, 0.0, 0.0)));
	}
	const int32 EntityBytes = FTCHARToUTF8(*FMantlePlaceExporter::EntityToJson(Big.Entities[0])).Length();
	TArray<FMantleCellFileBlob> Parts;
	if (TestTrue(TEXT("a cell over the limit is written as parts"), FMantleCellFiles::WriteCell(Big, Parts, Error, EntityBytes * 10 + 200)))
	{
		TestTrue(TEXT("more than one file"), Parts.Num() > 1);
		int32 Sum = 0;
		for (int32 Part = 0; Part < Parts.Num(); ++Part)
		{
			TestEqual(TEXT("numbered from 0"), Parts[Part].Part, Part);
			Sum += Parts[Part].Entities;
		}
		TestEqual(TEXT("no entity is lost or split"), Sum, 40);
	}
	Parts.Reset();
	TestTrue(TEXT("a roomy limit gives one file"), FMantleCellFiles::WriteCell(Big, Parts, Error) && Parts.Num() == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellIndexTest,
	"Mantle.Editor.Cells.IndexAndRoot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellIndexTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellsTestPrivate;
	const FMantleCellGrid Grid = FMantleCellGrid::Default();
	FMantleCellIndexEntry Cell;
	Cell.Key = FMantleCellKey{ 0, 12, -3 };
	Cell.Bounds = FBox(FVector(76800.0, -19200.0, -350.0), FVector(83200.0, -12800.0, 2100.0));
	Cell.DrawDistance = 9000.0;
	Cell.Files.Add(FMantleCellFileRef{ TEXT("ast_u0123456789abcdef0123456789abcdef"), TEXT("sha256:") + FString::ChrN(64, TEXT('a')), 182340 });
	Cell.Entities = 310;
	Cell.Instances = 5120;
	Cell.Solid = 44;

	const FString Json = FMantleCellFiles::IndexDocument(Grid, { Cell });
	TSharedPtr<FJsonObject> Root;
	if (!TestTrue(TEXT("valid JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root)) || !Root.IsValid())
	{
		return false;
	}
	TestEqual(TEXT("version"), static_cast<int32>(Root->GetNumberField(TEXT("version"))), 1);
	const TArray<TSharedPtr<FJsonValue>>& Levels = Root->GetObjectField(TEXT("grid"))->GetArrayField(TEXT("levels"));
	if (TestEqual(TEXT("three levels"), Levels.Num(), 3))
	{
		TestEqual(TEXT("level 1 cell size"), static_cast<int64>(Levels[1]->AsObject()->GetNumberField(TEXT("cellSize"))), static_cast<int64>(25600));
		TestEqual(TEXT("level 2 draw distance"), static_cast<int64>(Levels[2]->AsObject()->GetNumberField(TEXT("maxDrawDistance"))), static_cast<int64>(250000));
	}
	const TArray<TSharedPtr<FJsonValue>>& Cells = Root->GetArrayField(TEXT("cells"));
	if (TestEqual(TEXT("one cell"), Cells.Num(), 1))
	{
		const TSharedPtr<FJsonObject> Entry = Cells[0]->AsObject();
		TestEqual(TEXT("x"), static_cast<int32>(Entry->GetNumberField(TEXT("x"))), 12);
		TestEqual(TEXT("y"), static_cast<int32>(Entry->GetNumberField(TEXT("y"))), -3);
		TestEqual(TEXT("drawDistance"), Entry->GetNumberField(TEXT("drawDistance")), 9000.0);
		const TSharedPtr<FJsonObject> Counts = Entry->GetObjectField(TEXT("counts"));
		TestEqual(TEXT("entities"), static_cast<int32>(Counts->GetNumberField(TEXT("entities"))), 310);
		TestEqual(TEXT("instances"), static_cast<int32>(Counts->GetNumberField(TEXT("instances"))), 5120);
		TestEqual(TEXT("solid"), static_cast<int32>(Counts->GetNumberField(TEXT("solid"))), 44);
		const TArray<TSharedPtr<FJsonValue>>& Min = Entry->GetObjectField(TEXT("bounds"))->GetArrayField(TEXT("min"));
		TestEqual(TEXT("bounds min z"), Min[2]->AsNumber(), -350.0);
		const TSharedPtr<FJsonObject> FileEntry = Entry->GetArrayField(TEXT("files"))[0]->AsObject();
		TestEqual(TEXT("file bytes"), static_cast<int32>(FileEntry->GetNumberField(TEXT("bytes"))), 182340);
	}

	TArray<uint8> Bytes;
	FString Error;
	if (TestTrue(TEXT("the index is written"), FMantleCellFiles::WriteIndex(Grid, { Cell }, Bytes, Error)))
	{
		TestTrue(TEXT("MCI1 magic"), Bytes[0] == 'M' && Bytes[1] == 'C' && Bytes[2] == 'I' && Bytes[3] == '1');
		FString Inflated;
		TestTrue(TEXT("reads back"), FMantleCellFiles::ReadContainer(Bytes, FMantleCellFiles::IndexMagic(), FMantleCellFiles::MaxIndexRawBytes, Inflated, Error));
		TestEqual(TEXT("as the document"), Inflated, Json);
	}
	FMantleCellGrid BadGrid;
	TestFalse(TEXT("an invalid grid is not written"), FMantleCellFiles::WriteIndex(BadGrid, { Cell }, Bytes, Error));

	// Provisional ids (an offline export): stable per content, different per kind.
	const FString Hash = TEXT("sha256:") + FString::ChrN(64, TEXT('b'));
	const FString CellId = FMantleCellFiles::ProvisionalCellId(Hash);
	TestTrue(TEXT("a schema-valid id"), CellId.StartsWith(TEXT("ast_u")) && CellId.Len() == 37);
	TestEqual(TEXT("stable"), FMantleCellFiles::ProvisionalCellId(Hash), CellId);
	TestNotEqual(TEXT("the index's differs"), FMantleCellFiles::ProvisionalIndexId(Hash), CellId);

	// The root names the index: its manifest entry sits in id order and the key is written once (2).
	FMantlePlace Place;
	Place.PlaceId = TEXT("plc_test0000000000001");
	FMantleAssetRef Before;
	Before.Id = TEXT("ast_u00000000000000000000000000000001");
	Before.Hash = TEXT("sha256:") + FString::ChrN(64, TEXT('1'));
	FMantleAssetRef After;
	After.Id = TEXT("ast_uffffffffffffffffffffffffffffffff");
	After.Hash = TEXT("sha256:") + FString::ChrN(64, TEXT('2'));
	Place.Assets = { Before, After };
	FMantleCellRoot RootRef;
	RootRef.IndexId = TEXT("ast_u8888888888888888888888888888888");
	RootRef.Hash = TEXT("sha256:") + FString::ChrN(64, TEXT('c'));
	RootRef.Bytes = 4321;
	const FString RootJson = FMantlePlaceExporter::ToJson(Place, &RootRef);
	TSharedPtr<FJsonObject> RootObject;
	if (TestTrue(TEXT("the root is valid JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(RootJson), RootObject)) && RootObject.IsValid())
	{
		const TSharedPtr<FJsonObject> CellsKey = RootObject->GetObjectField(TEXT("cells"));
		TestEqual(TEXT("cells.index"), CellsKey->GetStringField(TEXT("index")), RootRef.IndexId);
		const TArray<TSharedPtr<FJsonValue>>& Assets = RootObject->GetArrayField(TEXT("assets"));
		if (TestEqual(TEXT("the manifest has the index entry"), Assets.Num(), 3))
		{
			TestEqual(TEXT("in id order"), Assets[1]->AsObject()->GetStringField(TEXT("id")), RootRef.IndexId);
			TestEqual(TEXT("of kind PlaceCellIndex"), Assets[1]->AsObject()->GetStringField(TEXT("kind")), FString(TEXT("PlaceCellIndex")));
			TestEqual(TEXT("with its bytes"), static_cast<int32>(Assets[1]->AsObject()->GetNumberField(TEXT("bytes"))), 4321);
			TestEqual(TEXT("no other key"), Assets[1]->AsObject()->Values.Num(), 4);
		}
	}
	const TSharedPtr<FJsonObject> Whole = [&]
	{
		TSharedPtr<FJsonObject> Object;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FMantlePlaceExporter::ToJson(Place)), Object);
		return Object;
	}();
	TestTrue(TEXT("a root with no cells has no cells key and the two assets"), Whole.IsValid() && !Whole->HasField(TEXT("cells")) && Whole->GetArrayField(TEXT("assets")).Num() == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEntityJsonTest,
	"Mantle.Editor.Cells.EntityJsonIsTheRootsOwn",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEntityJsonTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellsTestPrivate;
	// A cell file's entity is exactly the object the root writes: the whole place's entity array holds the same text.
	FMantlePlace Place;
	Place.PlaceId = TEXT("plc_test0000000000001");
	const FMantleEntity Mesh = MakeMesh(TEXT("ent_one"), MeshA, FVector(1.5, 2.5, 3.5));
	Place.Entities.Add(Mesh);
	const FString Whole = FMantlePlaceExporter::ToJson(Place);
	const FString Single = FMantlePlaceExporter::EntityToJson(Mesh);
	TestTrue(TEXT("the root contains the entity's object byte for byte"), Whole.Contains(Single));
	TestTrue(TEXT("it is an object"), Single.StartsWith(TEXT("{")) && Single.EndsWith(TEXT("}")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleCellPlannerStayReasonsTest,
	"Mantle.Editor.Cells.InteractableAndOutOfGridStayInTheRoot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleCellPlannerStayReasonsTest::RunTest(const FString& Parameters)
{
	using namespace MantleCellsTestPrivate;
	FMantlePlace Place;
	TArray<FMantleInstancedMeshGroup> Groups;
	FMantleCellPlanInput Input;
	Input.bFoldStaticMeshes = false;

	Place.Entities.Add(MakeMesh(TEXT("ent_cell"), MeshA, FVector(100.0, 100.0, 0.0)));
	FMantleEntity Button = MakeMesh(TEXT("ent_button"), MeshA, FVector(200.0, 200.0, 0.0));
	Button.Interactable.Emplace();
	Place.Entities.Add(Button);
	// Past the largest map a grid covers: 7e9 cm is over a million level 0 cells away.
	Place.Entities.Add(MakeMesh(TEXT("ent_faraway"), MeshA, FVector(7.0e9, 0.0, 0.0)));
	Groups.Add(MakeGroupOf(MeshB, 9000.0, { FVector(7.0e9, 100.0, 0.0), FVector(300.0, 300.0, 0.0) }));

	const FMantleCellPlan Plan = FMantleCellPlanner::Plan(Place, Groups, Input);
	TestTrue(TEXT("the interactable stays"), HasEntity(Place, TEXT("ent_button")));
	TestTrue(TEXT("the one outside the grid stays"), HasEntity(Place, TEXT("ent_faraway")));
	TestFalse(TEXT("the plain one left"), HasEntity(Place, TEXT("ent_cell")));
	TestEqual(TEXT("one is interactable"), Plan.Report.StayCounts[static_cast<int32>(EMantleCellStay::Interactable)], static_cast<int64>(1));
	// The far mesh and the far instance: one each.
	TestEqual(TEXT("two are outside the grid"), Plan.Report.StayCounts[static_cast<int32>(EMantleCellStay::OutOfGrid)], static_cast<int64>(2));
	TestEqual(TEXT("the far instance stays with its group"), Groups[0].WorldInstances.Num(), 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
