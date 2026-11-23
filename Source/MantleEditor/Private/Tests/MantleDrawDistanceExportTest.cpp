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

	// Exactly the schema's largest is no cull too: that number only ever means "no cut-off of its own" (the cell decides).
	Far.LDMaxDrawDistance = FMantlePlaceLimits::MaxDrawDistance;
	TestEqual(TEXT("the largest distance a creator sets is no cull"), Apply(Far).DrawDistance, 0.0);

	// A creator's own distance on a big mesh is kept as it is, below the ring's reach or past it.
	FMantleDrawDistanceSource Wall = Source(500.0);
	Wall.LDMaxDrawDistance = 20000.0;
	TestEqual(TEXT("a big mesh's own short distance"), Apply(Wall).DrawDistance, 20000.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleDrawDistanceDefaultTest,
	"Mantle.Editor.DrawDistance.SizeBasedDefaultByBand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDrawDistanceDefaultTest::RunTest(const FString& Parameters)
{
	using namespace MantleDrawDistanceExportTestPrivate;

	// The big-map plan, step 3: one band is left, small clutter under 30 cm radius at 60 m.
	TestEqual(TEXT("one size band"), FMantleDrawDistance::Bands().Num(), 1);
	TestEqual(TEXT("under 30 cm"), FMantleDrawDistance::NoCutOffRadius(), 30.0);
	TestEqual(TEXT("a 10 cm prop culls at 60 m"), Apply(Source(10.0)).DrawDistance, 6000.0);
	TestEqual(TEXT("just under 30 cm too"), Apply(Source(29.9)).DrawDistance, 6000.0);

	// An unmeasured mesh (no radius) is the small band, never an unculled one by accident of 0.
	TestEqual(TEXT("a mesh with no bounds is treated as small"), Apply(Source(0.0)).DrawDistance, FMantleDrawDistance::Bands()[0].Draw);

	// Every piece of 30 cm or more, a wall module, a window, a tower floor or a 50 m roof, has no cut-off of its own: no size picks a
	// distance any more, so no building is split by the size of its pieces.
	for (const double Radius : { 30.0, 99.0, 150.0, 375.0, 999.0, 1000.0, 5000.0, 9999.0 })
	{
		TestEqual(*FString::Printf(TEXT("radius %g cm has no cut-off of its own"), Radius), Apply(Source(Radius)).DrawDistance, FMantleDrawDistance::NoCutOff);
	}
	TestTrue(TEXT("which is the schema's largest"), FMantleDrawDistance::NoCutOff == FMantlePlaceLimits::MaxDrawDistance);
	TestTrue(TEXT("and it reads as no cut-off"), FMantleDrawDistance::IsNoCutOff(Apply(Source(150.0)).DrawDistance));
	TestFalse(TEXT("the small band does not"), FMantleDrawDistance::IsNoCutOff(Apply(Source(10.0)).DrawDistance));

	// In the default grid the small band streams in the 64 m cells as before, and no cut-off in the 256 m cells with one 600 m reach.
	const FMantleCellGrid Grid = FMantleCellGrid::Default();
	TestEqual(TEXT("the small band is level 0"), Grid.LevelFor(FMantleDrawDistance::Bands()[0].Draw), 0);
	const int32 Ring = Grid.LevelFor(FMantleDrawDistance::NoCutOff);
	if (TestEqual(TEXT("no cut-off is level 1"), Ring, 1))
	{
		TestEqual(TEXT("its cells are 256 m"), Grid.Levels[Ring].CellSize, static_cast<int64>(25600));
		TestEqual(TEXT("and load out to 600 m"), Grid.CellDrawDistanceFor(Ring, FMantleDrawDistance::NoCutOff), 60000.0);
	}
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
	FMantleDrawDistanceTallPieceTest,
	"Mantle.Editor.DrawDistance.TallNarrowPieceGoesToItsCell",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleDrawDistanceTallPieceTest::RunTest(const FString& Parameters)
{
	using namespace MantleDrawDistanceExportTestPrivate;

	// City Sample's SM_arbitrary_roof_694: bounds extent (5695, 2941, 20891) cm. Its sphere is 218 m round (so the old rule left it in the root, over a tower
	// that was not loaded), its footprint 64 m across: one building's piece, no cut-off of its own, in its cell.
	FMantleDrawDistanceSource Roof = Source(21852.0);
	Roof.FootprintRadius = FMath::Sqrt(FMath::Square(5695.0) + FMath::Square(2941.0));
	TestTrue(TEXT("its sphere is past the never-cull radius"), Roof.SphereRadius > FMantleDrawDistance::NeverCullRadius());
	TestTrue(TEXT("its footprint is not"), Roof.FootprintRadius < FMantleDrawDistance::NeverCullRadius());
	TestEqual(TEXT("a tall narrow piece has no cut-off of its own"), Apply(Roof).DrawDistance, FMantleDrawDistance::NoCutOff);

	// A mast or a tower far taller than wide is the same.
	FMantleDrawDistanceSource Mast = Source(60000.0);
	Mast.FootprintRadius = 500.0;
	TestEqual(TEXT("a 600 m mast too"), Apply(Mast).DrawDistance, FMantleDrawDistance::NoCutOff);

	// Wide and low stays always loaded: a district's ground tile, a freeway deck.
	FMantleDrawDistanceSource Ground = Source(50000.0);
	Ground.FootprintRadius = 49990.0;
	TestEqual(TEXT("a ground tile has no draw distance"), Apply(Ground).DrawDistance, 0.0);
	FMantleDrawDistanceSource Deck = Source(30000.0);
	Deck.FootprintRadius = 29990.0;
	TestEqual(TEXT("a freeway deck has none"), Apply(Deck).DrawDistance, 0.0);

	// The footprint decides at the edge, whatever the sphere.
	FMantleDrawDistanceSource Edge = Source(FMantleDrawDistance::NeverCullRadius() * 3.0);
	Edge.FootprintRadius = FMantleDrawDistance::NeverCullRadius();
	TestEqual(TEXT("a footprint of exactly the radius is wide"), Apply(Edge).DrawDistance, 0.0);
	Edge.FootprintRadius = FMantleDrawDistance::NeverCullRadius() - 1.0;
	TestEqual(TEXT("one cm under is a piece of a cell"), Apply(Edge).DrawDistance, FMantleDrawDistance::NoCutOff);

	// A piece that was not measured sideways is judged by its sphere, as before.
	TestEqual(TEXT("unmeasured and big: none"), Apply(Source(21852.0)).DrawDistance, 0.0);

	// The component's own distance and Never Distance Cull still come first.
	Roof.LDMaxDrawDistance = 90000.0;
	TestEqual(TEXT("its own distance wins"), Apply(Roof).DrawDistance, 90000.0);
	Roof.bNeverDistanceCull = true;
	TestEqual(TEXT("and never cull wins over that"), Apply(Roof).DrawDistance, 0.0);
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
	TestEqual(TEXT("then by size: no cut-off of its own"), Apply(Foliage).DrawDistance, FMantleDrawDistance::NoCutOff);

	// An end cull distance means nothing on a plain mesh component.
	FMantleDrawDistanceSource Plain = Source(150.0);
	Plain.InstanceEndCullDistance = 12000;
	TestEqual(TEXT("a plain mesh ignores it"), Apply(Plain).DrawDistance, FMantleDrawDistance::NoCutOff);
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

#endif // WITH_DEV_AUTOMATION_TESTS
