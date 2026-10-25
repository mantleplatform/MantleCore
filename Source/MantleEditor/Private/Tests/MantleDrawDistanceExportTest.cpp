// Copyright Joshua Eger. All Rights Reserved.
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
	FMantleDrawDistanceSource Big = Source(5000.0);
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
	Place.Entities.Add(MeshEntity(TEXT("ent_big"), Apply(Source(5000.0))));
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
	Place.Entities.Add(MeshEntity(TEXT("ent_big"), Apply(Source(5000.0))));

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
	TestTrue(TEXT("Mantle.Check reports it"), FMantleCheck::DoneLine(Report).EndsWith(TEXT(" drawDistances=2")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
