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
	// supports at 250 m. The pieces that could hang in the air draw to their structure's largest band, at most 600 m; small props and flat
	// ground keep their own (the ground's road tiles their 2.5 km).
	enum EOwner : int32 { Walls, Roof, Deck, Supports, Ground, Park, OtherBuilding, Tower };
	TArray<FMantleStructureMember> Members;
	auto Member = [&Members](const int32 Owner, const double Band, const double Radius, const double Height, const double Bottom)
	{
		FMantleStructureMember& Added = Members.AddDefaulted_GetRef();
		Added.Owner = Owner;
		Added.Band = Band;
		Added.Radius = Radius;
		Added.Height = Height;
		Added.Bottom = Bottom;
		return Members.Num() - 1;
	};
	const int32 GroundFloor = Member(Walls, 25000.0, 150.0, 300.0, 0.0);
	const int32 UpperFloors = Member(Walls, 60000.0, 400.0, 300.0, 300.0);
	const int32 Decal = Member(Walls, 6000.0, 20.0, 50.0, 100.0);
	const int32 RoofSlab = Member(Roof, 250000.0, 1500.0, 100.0, 3000.0);
	const int32 DeckSlab = Member(Deck, 250000.0, 3000.0, 100.0, 800.0);
	const int32 Support = Member(Supports, 25000.0, 400.0, 800.0, 0.0);
	const int32 RoadTile = Member(Ground, 250000.0, 2000.0, 70.0, -50.0);
	const int32 Sidewalk = Member(Ground, 25000.0, 200.0, 30.0, 0.0);
	const int32 Lamp = Member(Ground, 12000.0, 80.0, 600.0, 0.0);
	const int32 SmallTree = Member(Park, 25000.0, 300.0, 900.0, 0.0);
	const int32 BigTree = Member(Park, 60000.0, 600.0, 1500.0, 0.0);
	const int32 Shop = Member(OtherBuilding, 25000.0, 900.0, 2500.0, 0.0);
	const int32 TowerBase = Member(Tower, 60000.0, 800.0, 3000.0, 0.0);
	const int32 TowerTop = Member(Tower, 60000.0, 800.0, 3000.0, 3000.0);

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
	// A park of trees, 10 to 15 m tall, with nothing on them.
	Links.Add({ Park, Box(40000, 0, 0, 44000, 4000, 1500) });
	// Another building beside the first, touching it, with nothing raised over it.
	Links.Add({ OtherBuilding, Box(2000, 0, 0, 4000, 2000, 2500) });
	// A tower whose pieces all share one band.
	Links.Add({ Tower, Box(30000, 0, 0, 32000, 2000, 6000) });

	TArray<double> Distances;
	TArray<int32> Structures;
	FMantleDrawDistance::PlanStructures(Members, Links, Distances, &Structures);
	TestEqual(TEXT("one distance per member"), Distances.Num(), Members.Num());
	TestEqual(TEXT("the ground floor's 250 m walls draw to 600 m"), Distances[GroundFloor], 60000.0);
	TestEqual(TEXT("the upper floors' 600 m walls too"), Distances[UpperFloors], 60000.0);
	TestEqual(TEXT("and the roof on them comes down to 600 m with them"), Distances[RoofSlab], 60000.0);
	TestEqual(TEXT("a small decal on the walls keeps its 60 m"), Distances[Decal], 6000.0);
	TestEqual(TEXT("the roof and walls are one structure"), Structures[RoofSlab], Structures[GroundFloor]);
	TestEqual(TEXT("the deck comes down to 600 m"), Distances[DeckSlab], 60000.0);
	TestEqual(TEXT("and its supports go up to 600 m with it"), Distances[Support], 60000.0);
	TestEqual(TEXT("the road tiles keep their 2.5 km"), Distances[RoadTile], 250000.0);
	TestEqual(TEXT("the sidewalk keeps its 250 m"), Distances[Sidewalk], 25000.0);
	TestEqual(TEXT("the lamp keeps its 120 m"), Distances[Lamp], 12000.0);
	TestTrue(TEXT("the ground is not part of the building"), Structures[RoadTile] != Structures[GroundFloor]);
	TestEqual(TEXT("trees with nothing raised keep their 250 m"), Distances[SmallTree], 25000.0);
	TestEqual(TEXT("and their 600 m"), Distances[BigTree], 60000.0);
	TestEqual(TEXT("the building beside, holding nothing up, keeps its 250 m"), Distances[Shop], 25000.0);
	TestEqual(TEXT("a structure of one band keeps it"), Distances[TowerBase], 60000.0);
	TestEqual(TEXT("at the top too"), Distances[TowerTop], 60000.0);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDrawDistancePullDownTest,
	"Mantle.Editor.DrawDistance.RaisedPieceEndsWithItsSupports",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDrawDistancePullDownTest::RunTest(const FString& Parameters)
{
	// City Sample's tower roofs (2026-11-22, Joshua: "A: pull down"): a 600 m roof slab on walls of 120 m modules hung in the sky past
	// 120 m. A raised piece draws no farther than what holds it up, and what stands on it comes down with it.
	enum EComponent : int32
	{
		Modules, Roof, Rooftop, BigWalls, SecondRoof, Ground, Lamp, Curb, Pedestal, Statue, Sign, Pipe, Walls, Floors, ThirdRoof, Planes, Post,
		Count
	};
	TArray<double> Distances;
	Distances.SetNumZeroed(Count);
	Distances[Modules] = 12000.0;
	Distances[Roof] = 60000.0;
	Distances[Rooftop] = 60000.0;
	Distances[BigWalls] = 25000.0;
	Distances[SecondRoof] = 60000.0;
	Distances[Ground] = 250000.0;
	Distances[Lamp] = 12000.0;
	Distances[Curb] = 6000.0;
	Distances[Pedestal] = 0.0;
	Distances[Statue] = 60000.0;
	Distances[Sign] = 90000.0;
	Distances[Pipe] = 6000.0;
	Distances[Walls] = 60000.0;
	Distances[Floors] = 60000.0;
	Distances[ThirdRoof] = 60000.0;
	Distances[Planes] = 25000.0;
	Distances[Post] = 6000.0;
	TArray<bool> Movable;
	Movable.Init(true, Count);
	// The pedestal is never culled and the sign sets its own distance: they hold others up but keep theirs.
	Movable[Pedestal] = false;
	Movable[Sign] = false;

	TArray<FMantleHeldPiece> Pieces;
	auto Piece = [&Pieces](const int32 Component, const float X0, const float Y0, const float Z0, const float X1, const float Y1, const float Z1)
	{
		Pieces.Add(FMantleHeldPiece{ Component, FBox3f(FVector3f(X0, Y0, Z0), FVector3f(X1, Y1, Z1)) });
		return Pieces.Num() - 1;
	};
	// The ground under everything, 50 cm deep.
	const int32 GroundTile = Piece(Ground, -2000, -2000, -50, 20000, 4000, 0);
	// A tower: a wall of 120 m modules, 3 m a floor, the top one with a parapet 1 m over the 600 m roof slab; a 600 m rooftop unit on the slab.
	for (int32 Floor = 0; Floor < 9; ++Floor)
	{
		Piece(Modules, 0, 0, Floor * 300.0f, 2000, 100, (Floor + 1) * 300.0f);
	}
	Piece(Modules, 0, 0, 2700, 2000, 100, 3100);
	const int32 RoofSlab = Piece(Roof, 0, 0, 3000, 2000, 2000, 3020);
	Piece(Rooftop, 500, 500, 3020, 1500, 1500, 3400);
	// A second tower: one side 250 m walls, the other 120 m modules; its roof keeps the larger of the two.
	Piece(BigWalls, 5000, 0, 0, 7000, 100, 3000);
	Piece(Modules, 5000, 1900, 0, 7000, 2000, 3000);
	Piece(SecondRoof, 5000, 0, 3000, 7000, 2000, 3020);
	// A lamp on the street beside a curb: on the ground, so nothing holds it up.
	Piece(Curb, 100, 3000, -30, 300, 3100, 15);
	Piece(Lamp, 150, 3000, 0, 250, 3100, 600);
	// A statue on a never-culled pedestal, and a sign with its own distance on a 60 m pipe.
	Piece(Pedestal, 10000, 0, 0, 12000, 2000, 3000);
	Piece(Statue, 10500, 500, 3000, 11500, 1500, 3500);
	Piece(Pipe, 14000, 0, 0, 14100, 100, 400);
	Piece(Sign, 14000, 0, 400, 14100, 100, 800);
	// A tower of 600 m walls and floors on each other in turn, its roof on the top walls.
	for (int32 Floor = 0; Floor < 3; ++Floor)
	{
		Piece(Walls, 16000, 0, Floor * 320.0f, 18000, 2000, Floor * 320.0f + 300.0f);
		Piece(Floors, 16000, 0, Floor * 320.0f + 300.0f, 18000, 2000, Floor * 320.0f + 320.0f);
	}
	Piece(ThirdRoof, 16000, 0, 960, 18000, 2000, 980);
	// One set of planes: one on the ground, one up on a 60 m post. The whole set comes down to the post's.
	Piece(Planes, 18000, 0, 0, 19000, 1000, 10);
	Piece(Post, 18400, 2400, 0, 18600, 2600, 500);
	Piece(Planes, 18000, 2000, 500, 19000, 3000, 510);

	TArray<double> Limits;
	const int32 Lowered = FMantleDrawDistance::PullDownToSupports(Pieces, Distances, Movable, &Limits);
	TestEqual(TEXT("the roof slab on 120 m modules ends at the modules' 120 m"), Distances[Roof], 12000.0);
	TestEqual(TEXT("and the rooftop unit on the slab comes down with it"), Distances[Rooftop], 12000.0);
	TestEqual(TEXT("the modules keep their 120 m"), Distances[Modules], 12000.0);
	TestEqual(TEXT("a roof on 250 m walls and 120 m modules ends at 250 m"), Distances[SecondRoof], 25000.0);
	TestEqual(TEXT("the walls on the ground keep their 250 m"), Distances[BigWalls], 25000.0);
	TestEqual(TEXT("the ground keeps its 2.5 km"), Distances[Ground], 250000.0);
	TestEqual(TEXT("a lamp on the street keeps its 120 m (the curb does not hold it up)"), Distances[Lamp], 12000.0);
	TestEqual(TEXT("a statue on a never-culled pedestal keeps its 600 m"), Distances[Statue], 60000.0);
	TestEqual(TEXT("the pedestal stays never culled"), Distances[Pedestal], 0.0);
	TestEqual(TEXT("a sign's own distance stays"), Distances[Sign], 90000.0);
	TestEqual(TEXT("walls and floors holding each other up keep their 600 m"), Distances[Walls], 60000.0);
	TestEqual(TEXT("both of them"), Distances[Floors], 60000.0);
	TestEqual(TEXT("and so does their roof"), Distances[ThirdRoof], 60000.0);
	TestEqual(TEXT("a set with one piece up on a 60 m post comes down to 60 m"), Distances[Planes], 6000.0);
	TestEqual(TEXT("four components come down"), Lowered, 4);
	TestEqual(TEXT("one limit per piece"), Limits.Num(), Pieces.Num());
	TestEqual(TEXT("the roof slab's limit is its modules'"), Limits[RoofSlab], 12000.0);
	TestEqual(TEXT("nothing holds the ground up"), Limits[GroundTile], 0.0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
