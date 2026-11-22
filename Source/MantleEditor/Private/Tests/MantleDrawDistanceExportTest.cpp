// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCells.h"
#include "MantleCheck.h"
#include "MantleClientFeatures.h"
#include "MantleDrawDistance.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceLimits.h"
#include "MantleTargetFeatures.h"

#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

// GhzGarage/mantle#630 stage 2: the draw distance a mesh or instance set publishes, from the source component's cull distance or
// a size-based default, only for a client that reads drawDistance. All on plain numbers and built places.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleDrawDistanceExportTestPrivate
{
	const TCHAR* const Mesh = TEXT("ast_u0123456789abcdef0123456789abcdef");

	FMantleDrawDistanceSource Source(const double Radius)
	{
		FMantleDrawDistanceSource Result;
		Result.SphereRadius = Radius;
		return Result;
	}

	FMantleStaticMeshProps Apply(const FMantleDrawDistanceSource& From, const FMantleTargetFeatures& Target = FMantleTargetFeatures::All())
	{
		FMantleStaticMeshProps Props;
		Props.Mesh = Mesh;
		FMantleDrawDistance::Apply(From, Target, Props);
		return Props;
	}

	/** The properties object of entity Index in the place file the exporter writes for Place. */
	TSharedPtr<FJsonObject> PropertiesOf(const FMantlePlace& Place, const int32 Index)
	{
		TSharedPtr<FJsonObject> Root;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FMantlePlaceExporter::ToJson(Place)), Root);
		if (!Root.IsValid())
		{
			return nullptr;
		}
		const TArray<TSharedPtr<FJsonValue>>* Entities = nullptr;
		if (!Root->TryGetArrayField(TEXT("entities"), Entities) || !Entities->IsValidIndex(Index))
		{
			return nullptr;
		}
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		(*Entities)[Index]->AsObject()->TryGetObjectField(TEXT("properties"), Properties);
		return Properties ? *Properties : nullptr;
	}

	FMantleEntity MeshEntity(const TCHAR* Id, const FMantleStaticMeshProps& Props)
	{
		FMantleEntity Entity;
		Entity.Id = Id;
		Entity.Type = EMantleEntityType::StaticMesh;
		Entity.Props.Set<FMantleStaticMeshProps>(Props);
		return Entity;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDrawDistanceSourceTest,
	"Mantle.Editor.DrawDistance.SourceCullDistanceIsCarried",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDrawDistanceSourceTest::RunTest(const FString& Parameters)
{
	using namespace MantleDrawDistanceExportTestPrivate;

	// A small mesh whose component sets Desired Max Draw Distance: the band's default does not replace it.
	FMantleDrawDistanceSource Desired = Source(20.0);
	Desired.LDMaxDrawDistance = 9000.0;
	Desired.CachedMaxDrawDistance = 4000.0;
	TestEqual(TEXT("the desired distance"), Apply(Desired).DrawDistance, 9000.0);

	// Only the current one (a cull distance volume): that one.
	Desired.LDMaxDrawDistance = 0.0;
	TestEqual(TEXT("the current distance"), Apply(Desired).DrawDistance, 4000.0);

	// A large mesh with a cull distance is culled there, though its size alone would never cull it.
	FMantleDrawDistanceSource Big = Source(FMantleDrawDistance::NeverCullRadius() * 2.0);
	Big.LDMaxDrawDistance = 150000.0;
	TestEqual(TEXT("a big mesh's own distance"), Apply(Big).DrawDistance, 150000.0);

	// Never Distance Cull wins over both the source and the default.
	FMantleDrawDistanceSource Never = Source(20.0);
	Never.LDMaxDrawDistance = 9000.0;
	Never.bNeverDistanceCull = true;
	TestEqual(TEXT("never cull publishes no draw distance"), Apply(Never).DrawDistance, 0.0);

	// Past the schema's largest: no cull, and never a 0 passed through as a distance.
	FMantleDrawDistanceSource Far = Source(20.0);
	Far.LDMaxDrawDistance = FMantlePlaceLimits::MaxDrawDistance * 2.0;
	TestEqual(TEXT("a distance past the limit is no cull"), Apply(Far).DrawDistance, 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDrawDistanceDefaultTest,
	"Mantle.Editor.DrawDistance.SizeBasedDefaultByBand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDrawDistanceDefaultTest::RunTest(const FString& Parameters)
{
	using namespace MantleDrawDistanceExportTestPrivate;

	// Each band: a radius inside it gets its draw distance, and a bigger mesh a longer one.
	double Previous = 0.0;
	for (const FMantleDrawDistance::FBand& Band : FMantleDrawDistance::Bands())
	{
		TestEqual(*FString::Printf(TEXT("draw distance below radius %g"), Band.MaxRadius), Apply(Source(Band.MaxRadius * 0.5)).DrawDistance, Band.Draw);
		TestTrue(TEXT("a bigger band culls farther"), Band.Draw > Previous);
		TestTrue(TEXT("and within the limit"), Band.Draw <= FMantlePlaceLimits::MaxDrawDistance);
		Previous = Band.Draw;
	}

	// The edge of a band belongs to the next one.
	TestEqual(TEXT("the edge goes to the bigger band"), Apply(Source(FMantleDrawDistance::Bands()[0].MaxRadius)).DrawDistance,
		FMantleDrawDistance::Bands()[1].Draw);

	// An unmeasured mesh (no radius) is the smallest band, never an unculled one by accident of 0.
	TestEqual(TEXT("a mesh with no bounds is treated as small"), Apply(Source(0.0)).DrawDistance, FMantleDrawDistance::Bands()[0].Draw);

	// Every band's distance fits a level of the default cell grid, so banded scenery streams in cells and never stays in the root
	// for being drawn too far (City Sample's 10 to 100 m ground, collision and roofs go to the farthest level).
	const FMantleCellGrid Grid = FMantleCellGrid::Default();
	for (const FMantleDrawDistance::FBand& Band : FMantleDrawDistance::Bands())
	{
		TestTrue(*FString::Printf(TEXT("band to %g cm fits a cell level"), Band.Draw), Grid.LevelFor(Band.Draw) != INDEX_NONE);
	}
	TestEqual(TEXT("a 50 m radius mesh draws to 2.5 km"), Apply(Source(5000.0)).DrawDistance, 250000.0);
	TestEqual(TEXT("which is the grid's farthest level"), Grid.LevelFor(250000.0), Grid.Levels.Num() - 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDrawDistanceHugeTest,
	"Mantle.Editor.DrawDistance.HugeMeshGetsNone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDrawDistanceHugeTest::RunTest(const FString& Parameters)
{
	using namespace MantleDrawDistanceExportTestPrivate;
	const double Radius = FMantleDrawDistance::NeverCullRadius();
	for (const double Size : { Radius, Radius * 2.0, 100000.0 })
	{
		TestEqual(*FString::Printf(TEXT("radius %g: no draw distance"), Size), Apply(Source(Size)).DrawDistance, 0.0);
	}
	TestTrue(TEXT("just below it, still culled"), Apply(Source(Radius - 1.0)).DrawDistance > 0.0);

	// And the place file has no key for it, not a 0.
	FMantlePlace Place;
	Place.SchemaVersion = MANTLE_PLACE_SCHEMA_VERSION;
	Place.Entities.Add(MeshEntity(TEXT("ent_big"), Apply(Source(FMantleDrawDistance::NeverCullRadius() * 2.0))));
	const TSharedPtr<FJsonObject> Properties = PropertiesOf(Place, 0);
	if (TestNotNull(TEXT("the properties"), Properties.Get()))
	{
		TestFalse(TEXT("no drawDistance key"), Properties->HasField(TEXT("drawDistance")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDrawDistanceFoliageTest,
	"Mantle.Editor.DrawDistance.InstanceEndCullDistance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDrawDistanceFoliageTest::RunTest(const FString& Parameters)
{
	using namespace MantleDrawDistanceExportTestPrivate;

	// Painted foliage: the foliage type's end cull distance, on the instanced component, is the draw distance.
	FMantleDrawDistanceSource Foliage = Source(150.0);
	Foliage.bInstanced = true;
	Foliage.InstanceEndCullDistance = 12000;
	TestEqual(TEXT("the end distance is the draw distance"), Apply(Foliage).DrawDistance, 12000.0);

	// With a plain Max Draw Distance as well, the end distance still wins.
	Foliage.LDMaxDrawDistance = 3000.0;
	TestEqual(TEXT("the end distance wins"), Apply(Foliage).DrawDistance, 12000.0);

	// No end set (0 is never passed through): the component's other distance, then the size default.
	Foliage.InstanceEndCullDistance = 0;
	TestEqual(TEXT("then the max draw distance"), Apply(Foliage).DrawDistance, 3000.0);
	Foliage.LDMaxDrawDistance = 0.0;
	TestEqual(TEXT("then the band"), Apply(Foliage).DrawDistance, FMantleDrawDistance::Bands()[2].Draw);

	// An end cull distance means nothing on a plain mesh component.
	FMantleDrawDistanceSource Plain = Source(150.0);
	Plain.InstanceEndCullDistance = 12000;
	TestEqual(TEXT("a plain mesh ignores it"), Apply(Plain).DrawDistance, FMantleDrawDistance::Bands()[2].Draw);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDrawDistanceGatedTest,
	"Mantle.Editor.DrawDistance.NothingWrittenWithoutTheFeature",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDrawDistanceGatedTest::RunTest(const FString& Parameters)
{
	using namespace MantleDrawDistanceExportTestPrivate;
	FMantleDrawDistanceSource Foliage = Source(20.0);
	Foliage.bInstanced = true;
	Foliage.InstanceEndCullDistance = 5000;
	Foliage.LDMaxDrawDistance = 3000.0;

	// A client that reads no features, and one that reads every feature but this one.
	TArray<FString> AllButThis = MantleClientFeatures::All();
	AllButThis.Remove(MantleClientFeatures::DrawDistance);
	for (const FMantleTargetFeatures& Target : { FMantleTargetFeatures::None(), FMantleTargetFeatures::FromNames(AllButThis) })
	{
		FMantleStaticMeshProps Props;
		Props.DrawDistance = 1000.0;
		TestFalse(TEXT("nothing set"), FMantleDrawDistance::Apply(Foliage, Target, Props));
		TestEqual(TEXT("no draw distance"), Props.DrawDistance, 0.0);
	}

	// The default target (a bare export, Publish) reads it, and so does a target that names it.
	TestTrue(TEXT("the default target reads it"), Apply(Foliage).DrawDistance > 0.0);
	TestTrue(TEXT("a named target reads it"), Apply(Foliage, FMantleTargetFeatures::FromNames({ MantleClientFeatures::DrawDistance })).DrawDistance > 0.0);

	// And the written place file has no key.
	FMantlePlace Place;
	Place.SchemaVersion = MANTLE_PLACE_SCHEMA_VERSION;
	Place.Entities.Add(MeshEntity(TEXT("ent_gated"), Apply(Foliage, FMantleTargetFeatures::None())));
	const TSharedPtr<FJsonObject> Properties = PropertiesOf(Place, 0);
	if (TestNotNull(TEXT("the entity's properties"), Properties.Get()))
	{
		TestFalse(TEXT("no drawDistance"), Properties->HasField(TEXT("drawDistance")));
	}
	TestEqual(TEXT("and the check counts none"), FMantleDrawDistance::CountEntities(Place), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDrawDistanceJsonTest,
	"Mantle.Editor.DrawDistance.KeyIsWrittenAndCounted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDrawDistanceJsonTest::RunTest(const FString& Parameters)
{
	using namespace MantleDrawDistanceExportTestPrivate;
	FMantlePlace Place;
	Place.SchemaVersion = MANTLE_PLACE_SCHEMA_VERSION;

	// A mesh, an instance set, and a big mesh with none.
	const FMantleStaticMeshProps Small = Apply(Source(20.0));
	Place.Entities.Add(MeshEntity(TEXT("ent_small"), Small));

	FMantleDrawDistanceSource Foliage = Source(20.0);
	Foliage.bInstanced = true;
	Foliage.InstanceEndCullDistance = 5000;
	FMantleInstancedMeshProps Set;
	Set.Mesh = Apply(Foliage);
	Set.Instances.Add(FTransform::Identity);
	FMantleEntity SetEntity;
	SetEntity.Id = TEXT("ent_set");
	SetEntity.Type = EMantleEntityType::InstancedMesh;
	SetEntity.Props.Set<FMantleInstancedMeshProps>(Set);
	Place.Entities.Add(MoveTemp(SetEntity));
	Place.Entities.Add(MeshEntity(TEXT("ent_big"), Apply(Source(FMantleDrawDistance::NeverCullRadius() * 2.0))));

	const TSharedPtr<FJsonObject> SmallJson = PropertiesOf(Place, 0);
	if (TestNotNull(TEXT("the mesh's properties"), SmallJson.Get()))
	{
		TestEqual(TEXT("drawDistance"), SmallJson->GetNumberField(TEXT("drawDistance")), FMantleDrawDistance::Bands()[0].Draw);
	}
	const TSharedPtr<FJsonObject> SetJson = PropertiesOf(Place, 1);
	if (TestNotNull(TEXT("the instance set's properties"), SetJson.Get()))
	{
		TestEqual(TEXT("drawDistance"), SetJson->GetNumberField(TEXT("drawDistance")), 5000.0);
	}
	const TSharedPtr<FJsonObject> BigJson = PropertiesOf(Place, 2);
	if (TestNotNull(TEXT("the big mesh's properties"), BigJson.Get()))
	{
		TestFalse(TEXT("no drawDistance"), BigJson->HasField(TEXT("drawDistance")));
	}

	TestEqual(TEXT("two entities got a draw distance"), FMantleDrawDistance::CountEntities(Place), 2);

	// Different distances are different instance sets; no distance keeps the key as it was.
	FMantleStaticMeshProps Other = Small;
	Other.DrawDistance = 7000.0;
	TestNotEqual(TEXT("another distance is another group"), FMantlePlaceExporter::InstancedGroupKey(Small), FMantlePlaceExporter::InstancedGroupKey(Other));
	FMantleStaticMeshProps None;
	None.Mesh = Mesh;
	TestFalse(TEXT("no distance adds nothing to the key"), FMantlePlaceExporter::InstancedGroupKey(None).Contains(TEXT("|d")));

	// The count reaches Mantle.Check's last line.
	FMantleCheckReport Report;
	Report.DrawDistances = FMantleDrawDistance::CountEntities(Place);
	TestTrue(TEXT("Mantle.Check reports it"), FMantleCheck::DoneLine(Report).Contains(TEXT(" drawDistances=2 ")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDrawDistanceStructuresTest,
	"Mantle.Editor.DrawDistance.StructureDrawsToOneDistance",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDrawDistanceStructuresTest::RunTest(const FString& Parameters)
{
	// City Sample's floating pieces (2026-11-22): a building's walls at 250 and 600 m under a roof at 2.5 km, a freeway deck at 2.5 km on
	// supports at 250 m. Each structure draws to its largest band, at most 600 m; the ground under them keeps its own 2.5 km.
	enum EOwner : int32 { Walls, Roof, Deck, Supports, Ground, Lamps, OtherBuilding, Tower, Clutter, Count };
	TArray<double> MinBand;
	TArray<double> MaxBand;
	MinBand.Init(0.0, Count);
	MaxBand.Init(0.0, Count);
	auto Bands = [&MinBand, &MaxBand](const int32 Owner, const double Min, const double Max)
	{
		MinBand[Owner] = Min;
		MaxBand[Owner] = Max;
	};
	Bands(Walls, 25000.0, 60000.0);
	Bands(Roof, 250000.0, 250000.0);
	Bands(Deck, 250000.0, 250000.0);
	Bands(Supports, 25000.0, 25000.0);
	Bands(Ground, 250000.0, 250000.0);
	Bands(Lamps, 12000.0, 12000.0);
	Bands(OtherBuilding, 25000.0, 25000.0);
	Bands(Tower, 60000.0, 60000.0);
	Bands(Clutter, 0.0, 0.0);

	auto Box = [](const double X0, const double Y0, const double Z0, const double X1, const double Y1, const double Z1)
	{
		return FBox(FVector(X0, Y0, Z0), FVector(X1, Y1, Z1));
	};
	TArray<FMantleStructurePiece> Links;
	// A building: walls 0 to 30 m over a 20 x 20 m footprint, its roof slab on top.
	Links.Add({ Walls, Box(0, 0, 0, 2000, 2000, 3000) });
	Links.Add({ Roof, Box(0, 0, 3000, 2000, 2000, 3100) });
	// A freeway 100 m away: a deck 8 to 9 m up, on supports that reach its underside.
	Links.Add({ Deck, Box(10000, 0, 800, 16000, 2000, 900) });
	Links.Add({ Supports, Box(11000, 900, 0, 11200, 1100, 800) });
	Links.Add({ Supports, Box(15000, 900, 0, 15200, 1100, 800) });
	// The ground under all of it: it reaches no raised piece's underside.
	Links.Add({ Ground, Box(-2000, -2000, -50, 20000, 4000, 20) });
	// Lamps are small: they never come in as links, so they join nothing (here: none listed, though they stand under the deck).
	// Another building beside the first, touching it, with nothing raised over it.
	Links.Add({ OtherBuilding, Box(2000, 0, 0, 4000, 2000, 2500) });
	// A tower whose pieces all share one band.
	Links.Add({ Tower, Box(30000, 0, 0, 32000, 2000, 10000) });
	// An owner whose every piece sets its own distance takes no part, even listed.
	Links.Add({ Clutter, Box(0, 0, 3000, 2000, 2000, 3200) });

	TArray<double> Distances;
	FMantleDrawDistance::PlanStructures(MinBand, MaxBand, Links, Distances);
	TestEqual(TEXT("one distance per owner"), Distances.Num(), static_cast<int32>(Count));
	TestEqual(TEXT("the walls draw to 600 m, the structure's largest band capped"), Distances[Walls], 60000.0);
	TestEqual(TEXT("and the roof on them comes down to 600 m with them"), Distances[Roof], 60000.0);
	TestEqual(TEXT("the deck comes down to 600 m"), Distances[Deck], 60000.0);
	TestEqual(TEXT("and its supports go up to 600 m with it"), Distances[Supports], 60000.0);
	TestEqual(TEXT("the ground keeps its 2.5 km"), Distances[Ground], 250000.0);
	TestEqual(TEXT("the lamps keep theirs"), Distances[Lamps], 12000.0);
	TestEqual(TEXT("the building beside, holding nothing up, keeps its 250 m"), Distances[OtherBuilding], 25000.0);
	TestEqual(TEXT("a structure of one band keeps it"), Distances[Tower], 60000.0);
	TestEqual(TEXT("an owner with no band gets none"), Distances[Clutter], 0.0);

	// A structure of small bands only is raised to its largest, under the cap.
	TArray<double> SmallMin = { 6000.0 };
	TArray<double> SmallMax = { 12000.0 };
	FMantleDrawDistance::PlanStructures(SmallMin, SmallMax, {}, Distances);
	TestEqual(TEXT("a stall's 60 m and 120 m pieces both draw to 120 m"), Distances[0], 12000.0);

	// Apply: the structure's distance replaces the size band, never a distance the component sets itself.
	FMantleDrawDistanceSource Banded;
	Banded.SphereRadius = 150.0;
	Banded.StructureDistance = 60000.0;
	FMantleStaticMeshProps Props;
	FMantleDrawDistance::Apply(Banded, FMantleTargetFeatures::All(), Props);
	TestEqual(TEXT("a 250 m piece of a 600 m structure draws to 600 m"), Props.DrawDistance, 60000.0);
	FMantleDrawDistanceSource Authored = Banded;
	Authored.LDMaxDrawDistance = 9000.0;
	FMantleDrawDistance::Apply(Authored, FMantleTargetFeatures::All(), Props);
	TestEqual(TEXT("its own Desired Max Draw Distance wins"), Props.DrawDistance, 9000.0);
	FMantleDrawDistanceSource Never = Banded;
	Never.bNeverDistanceCull = true;
	FMantleDrawDistance::Apply(Never, FMantleTargetFeatures::All(), Props);
	TestEqual(TEXT("and Never Distance Cull still never culls"), Props.DrawDistance, 0.0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
