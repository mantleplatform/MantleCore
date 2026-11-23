// Copyright Joshua Eger. All Rights Reserved.
#include "MantleFarSetPlan.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// What far sets decide, worked out by hand: which cells have a twin and how far it reaches, when one is wanted, built and released, which instances a twin keeps, what a twin
// of an entity is made of, and when it is drawn.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleFarSetPlanTestPrivate
{
	FMantleCellInfo MakeCell(const FVector& Min, const FVector& Max, const double DrawDistance)
	{
		FMantleCellInfo Cell;
		Cell.Bounds = FBox(Min, Max);
		Cell.DrawDistance = DrawDistance;
		return Cell;
	}

	FMantleFarSetSettings On()
	{
		FMantleFarSetSettings Settings;
		Settings.bEnabled = true;
		// The default keeps every instance; these tests work the thinning out, so they ask for every second one.
		Settings.ThinnedStride = 2;
		return Settings;
	}

	FMantleEntity InstancedEntity(const int32 Count, const double DrawDistance)
	{
		FMantleEntity Entity;
		Entity.Id = TEXT("ent_ism_a");
		Entity.Type = EMantleEntityType::InstancedMesh;
		Entity.Name = TEXT("Pines (instanced 1/1)");
		Entity.Transform = FTransform(FVector(1000, 2000, 0));
		FMantleInstancedMeshProps Props;
		Props.Mesh.Mesh = TEXT("ast_u0001");
		Props.Mesh.Materials = { TEXT("ast_u0002") };
		Props.Mesh.Collision = EMantleCollision::Blocking;
		Props.Mesh.bCastShadow = true;
		Props.Mesh.DrawDistance = DrawDistance;
		for (int32 At = 0; At < Count; ++At)
		{
			// The instance's own translation is its number, so which ones are kept can be read back.
			Props.Instances.Add(FTransform(FVector(At, 0, 0)));
		}
		Entity.Props.Emplace<FMantleInstancedMeshProps>(MoveTemp(Props));
		return Entity;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleFarSetPlanReachTest,
	"Mantle.Runtime.FarSet.Plan.Reach",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFarSetPlanReachTest::RunTest(const FString& Parameters)
{
	using namespace MantleFarSetPlanTestPrivate;
	const FMantleFarSetSettings Settings = On();
	// 150 m is the least: a cell of 149.99 m has no twin, one of exactly 150 m has.
	TestFalse(TEXT("14,999 cm: no twin"), FMantleFarSetPlan::CellHasFarSet(MakeCell(FVector::ZeroVector, FVector(100), 14999.0), Settings));
	TestTrue(TEXT("15,000 cm: a twin"), FMantleFarSetPlan::CellHasFarSet(MakeCell(FVector::ZeroVector, FVector(100), 15000.0), Settings));
	TestFalse(TEXT("switched off: none"), FMantleFarSetPlan::CellHasFarSet(MakeCell(FVector::ZeroVector, FVector(100), 60000.0), FMantleFarSetSettings()));
	// 2.5 x the drawDistance, at most 1.5 km: 150 m reaches 375 m, 600 m reaches 1.5 km, a 2.5 km cell is held to 1.5 km.
	TestEqual(TEXT("150 m reaches 375 m"), FMantleFarSetPlan::ReachOf(MakeCell(FVector::ZeroVector, FVector(100), 15000.0), Settings), 37500.0);
	TestEqual(TEXT("600 m reaches 1,500 m"), FMantleFarSetPlan::ReachOf(MakeCell(FVector::ZeroVector, FVector(100), 60000.0), Settings), 150000.0);
	TestEqual(TEXT("a 1 km cell is held to 1,500 m"), FMantleFarSetPlan::ReachOf(MakeCell(FVector::ZeroVector, FVector(100), 100000.0), Settings), 150000.0);
	TestEqual(TEXT("2.5 km is held to 1,500 m"), FMantleFarSetPlan::ReachOf(MakeCell(FVector::ZeroVector, FVector(100), 250000.0), Settings), 150000.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleFarSetPlanWantedTest,
	"Mantle.Runtime.FarSet.Plan.WantedBuiltReleased",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFarSetPlanWantedTest::RunTest(const FString& Parameters)
{
	using namespace MantleFarSetPlanTestPrivate;
	const FMantleFarSetSettings Settings = On();
	// A 600 m cell, a 256 m square from the origin. A point on the x axis east of it, X cm from its right edge (25,600), is X away from its bounds.
	const FMantleCellInfo Cell = MakeCell(FVector(0, 0, 0), FVector(25600, 25600, 1000), 60000.0);
	auto PathAt = [](const double DistanceFromEdge)
	{
		FMantleCellStreamPlan::FPath Path;
		Path.Add(FVector(25600.0 + DistanceFromEdge, 100, 0));
		return Path;
	};
	// Wanted out to the reach (150,000): exactly there yes, a centimetre more no.
	TestTrue(TEXT("on the reach"), FMantleFarSetPlan::IsWanted(Cell, PathAt(150000.0), Settings));
	TestFalse(TEXT("1 cm past the reach"), FMantleFarSetPlan::IsWanted(Cell, PathAt(150001.0), Settings));
	// A cell of no twin is never wanted, however near.
	TestFalse(TEXT("a 120 m cell has none"), FMantleFarSetPlan::IsWanted(MakeCell(FVector::ZeroVector, FVector(6400, 6400, 500), 12000.0), PathAt(100.0), Settings));
	// Released past reach x 1.2 = 180,000 (more than reach + 2,500): on it no, past it yes.
	TestFalse(TEXT("on the release distance"), FMantleFarSetPlan::IsReleased(Cell, PathAt(180000.0), Settings));
	TestTrue(TEXT("1 cm past it"), FMantleFarSetPlan::IsReleased(Cell, PathAt(180001.0), Settings));
	// A 150 m cell reaches 37,500: x 1.2 is 45,000 and reach + 2,500 is 40,000, the larger rules, so it is released past 45,000.
	const FMantleCellInfo Small = MakeCell(FVector(0, 0, 0), FVector(100, 100, 100), 15000.0);
	FMantleCellStreamPlan::FPath SmallPath;
	SmallPath.Add(FVector(100 + 45000.0, 50, 50));
	TestFalse(TEXT("on 1.2 x 37,500"), FMantleFarSetPlan::IsReleased(Small, SmallPath, Settings));
	SmallPath[0].X = 100 + 45001.0;
	TestTrue(TEXT("past it"), FMantleFarSetPlan::IsReleased(Small, SmallPath, Settings));
	// Built only past half the drawDistance (30,000): on it no, past it yes.
	TestFalse(TEXT("on the half"), FMantleFarSetPlan::NeedsBuilding(Cell, PathAt(30000.0), Settings));
	TestTrue(TEXT("past the half"), FMantleFarSetPlan::NeedsBuilding(Cell, PathAt(30001.0), Settings));
	// With two points (a view and its look-ahead) the nearer counts.
	FMantleCellStreamPlan::FPath Two = PathAt(200000.0);
	Two.Add(FVector(25600.0 + 100000.0, 100, 0));
	TestTrue(TEXT("the nearer point is within reach"), FMantleFarSetPlan::IsWanted(Cell, Two, Settings));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleFarSetPlanNaniteLeftOutTest,
	"Mantle.Runtime.FarSet.Plan.NaniteLeftOut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFarSetPlanNaniteLeftOutTest::RunTest(const FString& Parameters)
{
	using Left = FMantleFarSetPlan::ENaniteLeftOut;
	// A mesh with its Nanite data is built, see-through material or not (a mesh is published with both only if the converter let it through).
	TestTrue(TEXT("Nanite data, opaque: kept"), FMantleFarSetPlan::ClassifyNaniteLeftOut(true, false) == Left::Kept);
	TestTrue(TEXT("Nanite data, see-through: kept"), FMantleFarSetPlan::ClassifyNaniteLeftOut(true, true) == Left::Kept);
	// Without it, a see-through material is the converter's own reason (road paint, decals); anything else is a mesh that should have had it.
	TestTrue(TEXT("no Nanite data, see-through: counted as see-through"), FMantleFarSetPlan::ClassifyNaniteLeftOut(false, true) == Left::SeeThrough);
	TestTrue(TEXT("no Nanite data, opaque: counted as no Nanite"), FMantleFarSetPlan::ClassifyNaniteLeftOut(false, false) == Left::NoNanite);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleFarSetPlanTwinTest,
	"Mantle.Runtime.FarSet.Plan.Twin",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFarSetPlanTwinTest::RunTest(const FString& Parameters)
{
	using namespace MantleFarSetPlanTestPrivate;
	const FMantleFarSetSettings Settings = On();

	// Stride: 250 m and up keep every instance, below it every second one.
	TestEqual(TEXT("600 m: all"), FMantleFarSetPlan::StrideFor(60000.0, Settings), 1);
	TestEqual(TEXT("250 m: all"), FMantleFarSetPlan::StrideFor(25000.0, Settings), 1);
	TestEqual(TEXT("164 m: every second"), FMantleFarSetPlan::StrideFor(16468.0, Settings), 2);
	TestEqual(TEXT("the default settings thin nothing: every instance, even at 150 m"), FMantleFarSetPlan::StrideFor(15000.0, FMantleFarSetSettings()), 1);

	// Ten instances of a 164 m entity: indices 0, 2, 4, 6, 8 survive; the twin carries the reach, no collision, no shadow, and the mesh and material of the entity.
	{
		FMantleEntity Twin;
		const FMantleEntity Source = InstancedEntity(10, 16468.0);
		if (TestTrue(TEXT("made"), FMantleFarSetPlan::MakeTwin(Source, 90000.0, Settings, Twin)))
		{
			const FMantleInstancedMeshProps* Props = Twin.Props.TryGet<FMantleInstancedMeshProps>();
			if (TestNotNull(TEXT("still an instanced mesh"), Props))
			{
				TestEqual(TEXT("five kept"), Props->Instances.Num(), 5);
				for (int32 At = 0; At < Props->Instances.Num() && At < 5; ++At)
				{
					TestEqual(*FString::Printf(TEXT("kept %d is source %d"), At, At * 2), Props->Instances[At].GetLocation().X, static_cast<double>(At * 2));
				}
				TestEqual(TEXT("no collision"), Props->Mesh.Collision, EMantleCollision::None);
				TestFalse(TEXT("no shadow"), Props->Mesh.bCastShadow);
				TestEqual(TEXT("the reach is its distance"), Props->Mesh.DrawDistance, 90000.0);
				TestEqual(TEXT("the mesh"), Props->Mesh.Mesh, FString(TEXT("ast_u0001")));
				TestEqual(TEXT("the materials"), Props->Mesh.Materials.Num(), 1);
			}
			TestEqual(TEXT("the id"), Twin.Id, FString(TEXT("ent_ism_a")));
			TestEqual(TEXT("the name"), Twin.Name, FString(TEXT("Pines (instanced 1/1) (far)")));
			TestTrue(TEXT("the same anchor"), Twin.Transform.GetLocation().Equals(FVector(1000, 2000, 0)));
		}
		// The source is untouched.
		TestEqual(TEXT("source keeps its ten"), Source.Props.Get<FMantleInstancedMeshProps>().Instances.Num(), 10);
		TestEqual(TEXT("source keeps its collision"), Source.Props.Get<FMantleInstancedMeshProps>().Mesh.Collision, EMantleCollision::Blocking);
	}

	// Seven instances at 250 m: every one. One instance at 164 m: that one (index 0 is always kept).
	{
		FMantleEntity Twin;
		TestTrue(TEXT("250 m made"), FMantleFarSetPlan::MakeTwin(InstancedEntity(7, 25000.0), 1.0, Settings, Twin));
		TestEqual(TEXT("seven kept"), Twin.Props.Get<FMantleInstancedMeshProps>().Instances.Num(), 7);
		TestTrue(TEXT("one of one made"), FMantleFarSetPlan::MakeTwin(InstancedEntity(1, 16468.0), 1.0, Settings, Twin));
		TestEqual(TEXT("one kept"), Twin.Props.Get<FMantleInstancedMeshProps>().Instances.Num(), 1);
		// Three instances at 164 m: indices 0 and 2.
		TestTrue(TEXT("three made"), FMantleFarSetPlan::MakeTwin(InstancedEntity(3, 16468.0), 1.0, Settings, Twin));
		TestEqual(TEXT("two kept"), Twin.Props.Get<FMantleInstancedMeshProps>().Instances.Num(), 2);
	}

	// Too small an entity, an empty list and an entity that is not a mesh have no twin.
	{
		FMantleEntity Twin;
		TestFalse(TEXT("14,999 cm: none"), FMantleFarSetPlan::MakeTwin(InstancedEntity(10, 14999.0), 1.0, Settings, Twin));
		TestFalse(TEXT("no drawDistance: none"), FMantleFarSetPlan::MakeTwin(InstancedEntity(10, 0.0), 1.0, Settings, Twin));
		TestFalse(TEXT("nothing in the list: none"), FMantleFarSetPlan::MakeTwin(InstancedEntity(0, 60000.0), 1.0, Settings, Twin));
		FMantleEntity Light;
		Light.Type = EMantleEntityType::PointLight;
		Light.Props.Emplace<FMantlePointLightProps>();
		TestFalse(TEXT("a light: none"), FMantleFarSetPlan::MakeTwin(Light, 1.0, Settings, Twin));
	}

	// A single model keeps its place, loses its collision and shadow.
	{
		FMantleEntity House;
		House.Id = TEXT("ent_house");
		House.Type = EMantleEntityType::StaticMesh;
		House.Name = TEXT("House");
		House.Transform = FTransform(FRotator(0, 90, 0), FVector(5, 6, 7));
		FMantleStaticMeshProps Props;
		Props.Mesh = TEXT("ast_u0009");
		Props.Collision = EMantleCollision::Blocking;
		Props.bCastShadow = true;
		Props.Surface = EMantleSurface::Water;
		Props.DrawDistance = 60000.0;
		House.Props.Emplace<FMantleStaticMeshProps>(Props);
		FMantleEntity Twin;
		if (TestTrue(TEXT("house made"), FMantleFarSetPlan::MakeTwin(House, 180000.0, Settings, Twin)))
		{
			const FMantleStaticMeshProps& Far = Twin.Props.Get<FMantleStaticMeshProps>();
			TestEqual(TEXT("no collision"), Far.Collision, EMantleCollision::None);
			TestFalse(TEXT("no shadow"), Far.bCastShadow);
			TestEqual(TEXT("no surface"), Far.Surface, EMantleSurface::Default);
			TestEqual(TEXT("reach"), Far.DrawDistance, 180000.0);
			TestTrue(TEXT("the transform"), Twin.Transform.Equals(House.Transform));
			TestEqual(TEXT("the name"), Twin.Name, FString(TEXT("House (far)")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleFarSetPlanShowTest,
	"Mantle.Runtime.FarSet.Plan.ShouldShow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFarSetPlanShowTest::RunTest(const FString& Parameters)
{
	// The twin draws unless the real entity does: the four cases of (cell wanted, real entity standing).
	TestTrue(TEXT("not wanted, not standing: the twin is all there is"), FMantleFarSetPlan::ShouldShow(false, false));
	TestTrue(TEXT("not wanted, still standing (between the draw distance and the release distance): the real entity draws nothing there"), FMantleFarSetPlan::ShouldShow(false, true));
	TestTrue(TEXT("wanted, not built yet: the twin covers until it is"), FMantleFarSetPlan::ShouldShow(true, false));
	TestFalse(TEXT("wanted and standing: the real entity draws"), FMantleFarSetPlan::ShouldShow(true, true));
	return true;
}

#endif
