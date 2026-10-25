// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCoverage.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceParser.h"
#include "MantleTargetFeatures.h"
#include "MantleTerrainExport.h"
#include "MantleWaterBodyExport.h"

#include "Components/BoxComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

// The Water plugin reader (GhzGarage/mantle#997, stage 2). The Water plugin is Experimental and off by default, so every test that spawns
// a body skips itself when the plugin's classes are not loaded; the data, summary and landscape-carve tests need no plugin. The merged
// heightfield itself is read by FMantleTerrainExport::ReadLandscape from the landscape's final textures (see the note in
// FMantleTerrainCarveCoverageTest): rebuilding a real landscape's edit layers needs the GPU, which an automation run may not have.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleWaterBodyExportTestPrivate
{
	/** An empty editor world for one test, destroyed with it. */
	struct FWaterTestWorld
	{
		UWorld* World = nullptr;

		FWaterTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleWaterBodyExportTestWorld"));
		}

		~FWaterTestWorld()
		{
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

	AActor* SpawnWaterActor(UWorld* World, const TCHAR* ClassPath, const FVector& Location)
	{
		UClass* Class = FindObject<UClass>(nullptr, ClassPath);
		return Class ? World->SpawnActor(Class, &Location) : nullptr;
	}

	const FMantleWaterBodyData* FindBody(const FMantleWaterBodies& Water, const EMantleWaterBodyKind Type)
	{
		return Water.Bodies.FindByPredicate([Type](const FMantleWaterBodyData& Body) { return Body.Type == Type; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterBodyReadSpawnedBodiesTest,
	"Mantle.Editor.Water.ReadsSpawnedBodies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterBodyReadSpawnedBodiesTest::RunTest(const FString& Parameters)
{
	using namespace MantleWaterBodyExportTestPrivate;

	if (!FMantleWaterBodyExport::IsWaterPluginLoaded())
	{
		AddInfo(TEXT("The Water plugin is not loaded in this editor, so there are no water bodies to spawn; skipped."));
		return true;
	}
	FWaterTestWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}

	AActor* Lake = SpawnWaterActor(Fixture.World, TEXT("/Script/Water.WaterBodyLake"), FVector(0.0, 0.0, 100.0));
	AActor* River = SpawnWaterActor(Fixture.World, TEXT("/Script/Water.WaterBodyRiver"), FVector(10000.0, 0.0, 100.0));
	AActor* Ocean = SpawnWaterActor(Fixture.World, TEXT("/Script/Water.WaterBodyOcean"), FVector(0.0, 50000.0, 0.0));
	AActor* Custom = SpawnWaterActor(Fixture.World, TEXT("/Script/Water.WaterBodyCustom"), FVector(-10000.0, 0.0, 0.0));
	AActor* Zone = SpawnWaterActor(Fixture.World, TEXT("/Script/Water.WaterZone"), FVector(0.0, 0.0, 0.0));
	if (!TestTrue(TEXT("the bodies and the zone spawn"), Lake && River && Ocean && Custom && Zone))
	{
		return false;
	}

	// A custom body names the mesh it is: set on its property, as the details panel does.
	UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(), TEXT("MantleWaterTestPool"));
	UActorComponent* CustomBody = Custom->FindComponentByClass(FindObject<UClass>(nullptr, TEXT("/Script/Water.WaterBodyCustomComponent")));
	if (!TestNotNull(TEXT("the custom body's component"), CustomBody))
	{
		return false;
	}
	if (const FObjectPropertyBase* MeshProperty = CastField<FObjectPropertyBase>(CustomBody->GetClass()->FindPropertyByName(TEXT("WaterMeshOverride"))))
	{
		MeshProperty->SetObjectPropertyValue(MeshProperty->ContainerPtrToValuePtr<void>(CustomBody), Mesh);
	}

	const FMantleWaterBodies Water = FMantleWaterBodyExport::Read(Fixture.World);
	TestTrue(TEXT("the plugin is seen as loaded"), Water.bWaterPluginLoaded);
	TestEqual(TEXT("one lake"), Water.Count(EMantleWaterBodyKind::Lake), 1);
	TestEqual(TEXT("one river"), Water.Count(EMantleWaterBodyKind::River), 1);
	TestEqual(TEXT("one ocean"), Water.Count(EMantleWaterBodyKind::Ocean), 1);
	TestEqual(TEXT("one custom body"), Water.Count(EMantleWaterBodyKind::Custom), 1);
	// The Water plugin makes a zone of its own for a body that has none, so the level can have more than the one spawned here.
	TestTrue(TEXT("the water zone is read"), Water.Zones.Num() >= 1);
	TestTrue(TEXT("the check line counts them"), Water.SummaryLine().Contains(TEXT("ocean=1 lake=1 river=1 custom=1")) && Water.SummaryLine().Contains(TEXT("plugin=1")));

	if (const FMantleWaterBodyData* Body = FindBody(Water, EMantleWaterBodyKind::Lake))
	{
		TestTrue(TEXT("the lake is where it was put"), Body->Transform.GetLocation().Equals(FVector(0.0, 0.0, 100.0), 1.0));
		TestTrue(TEXT("a lake's spline is closed"), Body->bClosedLoop);
		TestTrue(TEXT("with points"), Body->Points.Num() >= 3);
		TestTrue(TEXT("a lake carves the landscape by default"), Body->bAffectsLandscape);
		TestFalse(TEXT("and has a collision setting"), Body->Collision.Enabled.IsEmpty());
	}
	if (const FMantleWaterBodyData* Body = FindBody(Water, EMantleWaterBodyKind::River))
	{
		TestFalse(TEXT("a river's spline is open"), Body->bClosedLoop);
		TestTrue(TEXT("with at least two points"), Body->Points.Num() >= 2);
		TestTrue(TEXT("and the Water plugin's metadata"), Body->bHasMetadata);
		if (Body->Points.Num() > 0)
		{
			TestTrue(TEXT("each point has a width"), Body->Points[0].Width > 0.f);
			TestTrue(TEXT("and a depth"), Body->Points[0].Depth > 0.f);
		}
	}
	if (const FMantleWaterBodyData* Body = FindBody(Water, EMantleWaterBodyKind::Custom))
	{
		TestTrue(TEXT("a custom body names its mesh"), Body->MeshPath.Contains(TEXT("MantleWaterTestPool")));
		TestFalse(TEXT("and never carves the landscape"), Body->bAffectsLandscape);
	}
	if (const FMantleWaterBodyData* Body = FindBody(Water, EMantleWaterBodyKind::Ocean))
	{
		TestTrue(TEXT("an ocean has an extent"), Body->OceanExtents.X > 0.0 && Body->OceanExtents.Y > 0.0);
	}
	TestTrue(TEXT("a zone has an extent"), Water.Zones.ContainsByPredicate([](const FMantleWaterZoneData& Zone) { return Zone.Extent.X > 0.0 && Zone.Extent.Y > 0.0; }));

	// The writer-to-come's input is plain JSON.
	const FString Json = FMantleWaterBodyExport::ToJsonString(Water);
	TestTrue(TEXT("the JSON lists the bodies"), Json.Contains(TEXT("\"bodyType\"")) && Json.Contains(TEXT("\"zones\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterBodyEmptyWorldTest,
	"Mantle.Editor.Water.EmptyWorldHasNoBodies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterBodyEmptyWorldTest::RunTest(const FString& Parameters)
{
	using namespace MantleWaterBodyExportTestPrivate;

	// With the plugin loaded or not, a level with no water reads as empty and does not fail.
	FWaterTestWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	const FMantleWaterBodies Water = FMantleWaterBodyExport::Read(Fixture.World);
	TestEqual(TEXT("no bodies"), Water.Bodies.Num(), 0);
	TestEqual(TEXT("no zones"), Water.Zones.Num(), 0);
	TestEqual(TEXT("the plugin flag follows the classes"), Water.bWaterPluginLoaded, FMantleWaterBodyExport::IsWaterPluginLoaded());
	const FMantleWaterBodies Nothing = FMantleWaterBodyExport::Read(nullptr);
	TestEqual(TEXT("no world, no bodies"), Nothing.Bodies.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterBodyJsonTest,
	"Mantle.Editor.Water.JsonCarriesTheData",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterBodyJsonTest::RunTest(const FString& Parameters)
{
	// Built by hand, so it runs without the Water plugin.
	FMantleWaterBodies Water;
	Water.bWaterPluginLoaded = true;

	FMantleWaterBodyData& River = Water.Bodies.AddDefaulted_GetRef();
	River.Type = EMantleWaterBodyKind::River;
	River.Name = TEXT("Brook");
	River.bHasMetadata = true;
	FMantleWaterSplineSample& Start = River.Points.AddDefaulted_GetRef();
	Start.Location = FVector(0.0, 0.0, 10.0);
	Start.Width = 300.f;
	Start.Depth = 120.f;
	Start.Velocity = 64.f;
	Start.AudioIntensity = 0.5f;
	River.Points.AddDefaulted();
	River.Waves.bHasWaves = true;
	River.Waves.WavesClass = TEXT("GerstnerWaterWaves");
	FMantleWaterWaveSample& Wave = River.Waves.Waves.AddDefaulted_GetRef();
	Wave.WaveLength = 800.f;
	Wave.Amplitude = 12.f;
	Water.Bodies.AddDefaulted_GetRef().Type = EMantleWaterBodyKind::Ocean;
	Water.Zones.AddDefaulted();

	TestEqual(TEXT("the check line"), Water.SummaryLine(), FString(TEXT("MANTLE_CHECK_WATER ocean=1 lake=0 river=1 custom=0 zones=1 plugin=1")));

	const TSharedRef<FJsonObject> Json = FMantleWaterBodyExport::ToJson(Water);
	const TArray<TSharedPtr<FJsonValue>>& Bodies = Json->GetArrayField(TEXT("bodies"));
	if (!TestEqual(TEXT("both bodies are written"), Bodies.Num(), 2))
	{
		return false;
	}
	// Sorted order is the reader's job; here the bodies are in the order they were added.
	const TSharedPtr<FJsonObject> First = Bodies[0]->AsObject();
	TestEqual(TEXT("the type"), First->GetStringField(TEXT("bodyType")), FString(TEXT("river")));
	const TArray<TSharedPtr<FJsonValue>>& Points = First->GetObjectField(TEXT("spline"))->GetArrayField(TEXT("points"));
	TestEqual(TEXT("both spline points"), Points.Num(), 2);
	TestEqual(TEXT("a river point has its width"), Points[0]->AsObject()->GetNumberField(TEXT("width")), 300.0);
	TestEqual(TEXT("its depth"), Points[0]->AsObject()->GetNumberField(TEXT("depth")), 120.0);
	TestEqual(TEXT("its velocity"), Points[0]->AsObject()->GetNumberField(TEXT("velocity")), 64.0);
	TestEqual(TEXT("and one wave"), First->GetObjectField(TEXT("waves"))->GetArrayField(TEXT("waves")).Num(), 1);
	TestFalse(TEXT("an ocean without waves writes none"), Bodies[1]->AsObject()->HasField(TEXT("waves")));
	TestEqual(TEXT("and no spline points of its own"), Bodies[1]->AsObject()->GetObjectField(TEXT("spline"))->GetArrayField(TEXT("points")).Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterBodyCarvingCountTest,
	"Mantle.Editor.Water.CountsBodiesThatCarveALandscape",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterBodyCarvingCountTest::RunTest(const FString& Parameters)
{
	using namespace MantleWaterBodyExportTestPrivate;

	if (!FMantleWaterBodyExport::IsWaterPluginLoaded())
	{
		AddInfo(TEXT("The Water plugin is not loaded in this editor, so there are no water bodies to spawn; skipped."));
		return true;
	}
	FWaterTestWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}

	// A box stands in for the landscape: the count reads only its bounds on the ground plane. A plain AActor has no root component when
	// it spawns, so the spawn location is dropped (AActor::PostSpawnInitialize places only a root); the box is put at the centre itself.
	auto MakeGround = [&Fixture](const FVector& Centre)
	{
		AActor* Ground = Fixture.World->SpawnActor<AActor>(Centre, FRotator::ZeroRotator);
		UBoxComponent* Box = NewObject<UBoxComponent>(Ground, TEXT("Ground"));
		Box->SetBoxExtent(FVector(5000.0, 5000.0, 100.0));
		Ground->SetRootComponent(Box);
		Box->SetWorldLocation(Centre);
		Box->RegisterComponent();
		return Ground;
	};

	AActor* Lake = SpawnWaterActor(Fixture.World, TEXT("/Script/Water.WaterBodyLake"), FVector(0.0, 0.0, 0.0));
	AActor* Custom = SpawnWaterActor(Fixture.World, TEXT("/Script/Water.WaterBodyCustom"), FVector(0.0, 0.0, 0.0));
	if (!TestTrue(TEXT("the bodies spawn"), Lake && Custom))
	{
		return false;
	}
	AActor* Under = MakeGround(FVector(0.0, 0.0, 0.0));
	AActor* Far = MakeGround(FVector(5000000.0, 5000000.0, 0.0));
	// The numbers the count compares, so a failure shows them.
	const FBox LakeBounds = Lake->GetComponentsBoundingBox(/*bNonColliding*/ true);
	const FBox UnderBounds = Under->GetComponentsBoundingBox();
	const FBox FarBounds = Far->GetComponentsBoundingBox();
	AddInfo(FString::Printf(TEXT("lake %s; under %s; far %s; lake to far %.0f"), *LakeBounds.ToString(), *UnderBounds.ToString(), *FarBounds.ToString(),
		FVector::Dist2D(LakeBounds.GetCenter(), FarBounds.GetCenter())));
	TestTrue(TEXT("the far ground is where it was put"), FarBounds.GetCenter().Equals(FVector(5000000.0, 5000000.0, 0.0), 1.0));
	TestEqual(TEXT("the lake carves the landscape it overlaps, and the custom body does not"), FMantleWaterBodyExport::CountBodiesCarving(Under), 1);
	TestEqual(TEXT("a landscape far from the water is not carved"), FMantleWaterBodyExport::CountBodiesCarving(Far), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleTerrainCarveCoverageTest,
	"Mantle.Editor.Water.TerrainCarveCoverage",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleTerrainCarveCoverageTest::RunTest(const FString& Parameters)
{
	// FMantleTerrainExport::ReadLandscape reads the landscape's final textures (FLandscapeEditDataInterface with no edit layer guid),
	// which hold every edit layer merged, water brushes included. It rebuilds the layers first when the landscape is out of date and
	// water carves it. A landscape's layer merge renders on the GPU, so this test covers what ChooseLayers says about it from a
	// source built in code, not the render.
	FMantleTerrainSource Source;
	Source.Width = 4;
	Source.Height = 4;
	Source.Heights.Init(32768, 16);
	Source.LayerNames.Add(TEXT("Grass"));
	Source.LayerWeights.Emplace();
	Source.LayerWeights.Last().Init(255, 16);
	Source.bMaterialRead = true;
	FMantleLandscapeLayerLook Look;
	Look.Layer = TEXT("Grass");
	Look.bHasFlatColour = true;
	Look.FlatColour = FLinearColor(0.1f, 0.5f, 0.1f);
	Look.bHasAverage = true;
	Look.Average = Look.FlatColour;
	Source.MaterialRead.Layers.Add(Look);
	Source.MaterialRead.bFoundLayers = true;

	auto Choose = [](const FMantleTerrainSource& Read, FMantleCoverage& Coverage, TArray<FMantleValidationIssue>& Issues)
	{
		TArray<FMantleTerrainLayerChoice> Layers;
		return FMantleTerrainExport::ChooseLayers(nullptr, Read, TEXT("Landscape"), Layers, Issues, &Coverage);
	};

	// No water, no line.
	{
		FMantleCoverage Coverage;
		TArray<FMantleValidationIssue> Issues;
		TestTrue(TEXT("a plain landscape is chosen"), Choose(Source, Coverage, Issues));
		TestEqual(TEXT("with no water carve line"), static_cast<int64>(Coverage.Count(MantleCoverageCategory::WaterCarve, EMantleCoverageOutcome::Kept)), static_cast<int64>(0));
	}

	// Carved, up to date: a kept line, no warning.
	Source.WaterBodiesCarving = 2;
	{
		FMantleCoverage Coverage;
		TArray<FMantleValidationIssue> Issues;
		TestTrue(TEXT("a carved landscape is chosen"), Choose(Source, Coverage, Issues));
		TestEqual(TEXT("with a kept water carve line"), static_cast<int64>(Coverage.Count(MantleCoverageCategory::WaterCarve, EMantleCoverageOutcome::Kept)), static_cast<int64>(1));
		TestEqual(TEXT("and no approximated one"), static_cast<int64>(Coverage.Count(MantleCoverageCategory::WaterCarve, EMantleCoverageOutcome::Approximated)), static_cast<int64>(0));
		TestEqual(TEXT("and no warning"), Issues.Num(), 0);
	}

	// Carved, layers still out of date after the rebuild: approximated, with a warning that says what to do.
	Source.bCarveRefreshed = true;
	Source.bCarveStale = true;
	{
		FMantleCoverage Coverage;
		TArray<FMantleValidationIssue> Issues;
		TestTrue(TEXT("a stale carved landscape still publishes"), Choose(Source, Coverage, Issues));
		TestEqual(TEXT("with an approximated water carve line"), static_cast<int64>(Coverage.Count(MantleCoverageCategory::WaterCarve, EMantleCoverageOutcome::Approximated)), static_cast<int64>(1));
		TestEqual(TEXT("and one warning"), Issues.Num(), 1);
		TestTrue(TEXT("which is a warning, not an error"), Issues.Num() == 1 && Issues[0].Severity == EMantleIssueSeverity::Warning);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterBodyBuildPropsTest,
	"Mantle.Editor.Water.BuildsThePlaceFormatsProperties",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterBodyBuildPropsTest::RunTest(const FString& Parameters)
{
	// Built by hand, so it runs without the Water plugin.
	auto Point = [](const FVector& Location, const float Depth, const float Width = 0.f, const float Velocity = 0.f)
	{
		FMantleWaterSplineSample Sample;
		Sample.Location = Location;
		Sample.ArriveTangent = FVector(100.0, 0.0, 0.0);
		Sample.LeaveTangent = FVector(100.0, 0.0, 0.0);
		Sample.Type = TEXT("Curve");
		Sample.Depth = Depth;
		Sample.Width = Width;
		Sample.Velocity = Velocity;
		return Sample;
	};

	// A river: widths and velocity kept, waves never carried, a clamped point written as curveClamped.
	{
		FMantleWaterBodyData River;
		River.Type = EMantleWaterBodyKind::River;
		River.bHasMetadata = true;
		River.Points = { Point(FVector(0.0, 0.0, 0.0), 300.f, 600.f, 120.f), Point(FVector(1000.0, 0.0, 0.0), 400.f, 800.f, 200000.f) };
		River.Points[1].Type = TEXT("CurveClamped");
		River.Waves.bHasWaves = true;
		River.Waves.Waves.AddDefaulted();
		FMantleWaterBodyProps Props;
		FString Why;
		TArray<FString> Notes;
		TestTrue(TEXT("a river builds"), FMantleWaterBodyExport::BuildProps(River, Props, Why, Notes));
		TestEqual(TEXT("as a river"), Props.BodyType, EMantleWaterBodyType::River);
		TestEqual(TEXT("with both points"), Props.SplinePoints.Num(), 2);
		TestEqual(TEXT("the width is kept"), Props.SplinePoints[0].Width, 600.0);
		TestEqual(TEXT("a velocity past the format's limit is held to it"), Props.SplinePoints[1].Velocity, FMantlePlaceLimits::MaxWaterRiverVelocity);
		TestEqual(TEXT("the interpolation is carried"), Props.SplinePoints[1].Interp, EMantleWaterSplinePointType::CurveClamped);
		TestEqual(TEXT("a river has no waves"), Props.Waves.Num(), 0);
		TestTrue(TEXT("and says values were held"), Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("held")); }));
	}

	// A lake whose spline sits offset from the actor: the points are in the actor's own space. Waves go out as world-space degrees.
	{
		FMantleWaterBodyData Lake;
		Lake.Type = EMantleWaterBodyKind::Lake;
		Lake.bHasMetadata = true;
		Lake.Transform = FTransform(FVector(1000.0, 0.0, 0.0));
		Lake.SplineTransform = FTransform(FVector(1500.0, 0.0, 0.0));
		Lake.Points = { Point(FVector(0.0, 0.0, 0.0), 500.f), Point(FVector(100.0, 0.0, 0.0), 500.f), Point(FVector(0.0, 100.0, 0.0), 500.f) };
		Lake.Waves.bHasWaves = true;
		FMantleWaterWaveSample& Wave = Lake.Waves.Waves.AddDefaulted_GetRef();
		Wave.WaveLength = 800.f;
		Wave.Amplitude = 20.f;
		Wave.Steepness = 2.f;
		Wave.Direction = FVector(0.0, 1.0, 0.0);
		Lake.Look.Albedo = FLinearColor(0.1f, 0.2f, 1.5f);
		Lake.Collision.Enabled = TEXT("NoCollision");
		FMantleWaterBodyProps Props;
		FString Why;
		TArray<FString> Notes;
		TestTrue(TEXT("a lake builds"), FMantleWaterBodyExport::BuildProps(Lake, Props, Why, Notes));
		TestTrue(TEXT("its points are in the actor's space"), Props.SplinePoints[0].Location.Equals(FVector(500.0, 0.0, 0.0), 0.01));
		TestEqual(TEXT("one wave"), Props.Waves.Num(), 1);
		TestEqual(TEXT("travelling along +Y is 90 degrees"), Props.Waves[0].DirectionDeg, 90.0);
		TestEqual(TEXT("a steepness past one is held to one"), Props.Waves[0].Steepness, 1.0);
		TestTrue(TEXT("a look colour is held to 0 to 1"), Props.Albedo.IsSet() && Props.Albedo->B == 1.f);
		TestFalse(TEXT("no collision is written as off"), Props.bCollision);
	}

	// What cannot be carried says why.
	{
		FMantleWaterBodyData Custom;
		Custom.Type = EMantleWaterBodyKind::Custom;
		FMantleWaterBodyProps Props;
		FString Why;
		TArray<FString> Notes;
		TestFalse(TEXT("a custom body without a mesh is refused"), FMantleWaterBodyExport::BuildProps(Custom, Props, Why, Notes));
		TestFalse(TEXT("with a reason"), Why.IsEmpty());
		FMantleWaterBodyData Lake;
		Lake.Type = EMantleWaterBodyKind::Lake;
		Lake.Points = { Point(FVector::ZeroVector, 100.f), Point(FVector(10.0, 0.0, 0.0), 100.f) };
		TestFalse(TEXT("a lake of two points is refused"), FMantleWaterBodyExport::BuildProps(Lake, Props, Why, Notes));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterBodyPlaceFileTest,
	"Mantle.Editor.Water.PropertiesAndZoneRoundTripThePlaceFile",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterBodyPlaceFileTest::RunTest(const FString& Parameters)
{
	// A lake, a river and an ocean written as the exporter writes them, with the zone, and read back by the place parser.
	auto Square = [](const double Half, const double Depth)
	{
		TArray<FMantleWaterSplineSample> Points;
		const FVector2D Corners[] = { FVector2D(Half, 0.0), FVector2D(0.0, Half), FVector2D(-Half, 0.0), FVector2D(0.0, -Half) };
		for (const FVector2D& Corner : Corners)
		{
			FMantleWaterSplineSample& Sample = Points.AddDefaulted_GetRef();
			Sample.Location = FVector(Corner.X, Corner.Y, 0.0);
			Sample.Type = TEXT("Curve");
			Sample.Depth = Depth;
		}
		return Points;
	};
	FMantleWaterWaveSample Wave;
	Wave.WaveLength = 1500.f;
	Wave.Amplitude = 40.f;
	Wave.Steepness = 0.5f;
	Wave.Direction = FVector(1.0, 0.0, 0.0);
	Wave.PhaseOffset = 2.5f;

	FMantleWaterBodyData Lake;
	Lake.Type = EMantleWaterBodyKind::Lake;
	Lake.bHasMetadata = true;
	Lake.Points = Square(2000.0, 500.0);
	Lake.Waves.bHasWaves = true;
	Lake.Waves.Waves.Add(Wave);
	Lake.Look.Albedo = FLinearColor(0.02f, 0.1f, 0.2f);
	Lake.Collision.Enabled = TEXT("QueryAndPhysics");
	Lake.Collision.HeightOffset = -20.f;

	FMantleWaterBodyData River = Lake;
	River.Type = EMantleWaterBodyKind::River;
	River.Waves = FMantleWaterWaves();
	River.Look = FMantleWaterLook();
	River.Points.SetNum(3);
	for (FMantleWaterSplineSample& Sample : River.Points)
	{
		Sample.Width = 600.f;
		Sample.Velocity = 120.f;
	}

	FMantleWaterBodyData Ocean = Lake;
	Ocean.Type = EMantleWaterBodyKind::Ocean;
	Ocean.Points = Square(100000.0, 500.0);
	Ocean.OceanExtents = FVector2D(409600.0, 409600.0);
	Ocean.Collision.OceanCollisionExtents = FVector(200000.0, 200000.0, 10000.0);
	Ocean.bOceanCentersOnZone = true;

	FMantlePlace Place;
	Place.SchemaVersion = MANTLE_PLACE_SCHEMA_VERSION;
	Place.PlaceId = TEXT("plc_water001");
	Place.Revision.Number = 1;
	Place.Metadata.Name = TEXT("Waterworks");
	Place.Metadata.AuthorId = TEXT("usr_4b1d");
	int32 Index = 0;
	const FMantleWaterBodyData* const Bodies[] = { &Lake, &River, &Ocean };
	for (const FMantleWaterBodyData* Body : Bodies)
	{
		FMantleEntity& Entity = Place.Entities.AddDefaulted_GetRef();
		Entity.Id = FString::Printf(TEXT("ent_water%d"), Index++);
		Entity.Name = TEXT("Water");
		Entity.Type = EMantleEntityType::WaterBody;
		FMantleWaterBodyProps Props;
		FString Why;
		TArray<FString> Notes;
		if (!TestTrue(TEXT("each body builds"), FMantleWaterBodyExport::BuildProps(*Body, Props, Why, Notes)))
		{
			return false;
		}
		Entity.Props.Set<FMantleWaterBodyProps>(MoveTemp(Props));
	}
	FMantleWaterZoneData Zone;
	Zone.Extent = FVector2D(102400.0, 102400.0);
	Zone.Transform = FTransform(FRotator(0.0, 45.0, 0.0), FVector(10.0, 20.0, 30.0));
	Place.Settings.Water = FMantleWaterBodyExport::BuildZone(Zone);

	const FString Json = FMantlePlaceExporter::ToJson(Place);
	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
	if (!TestTrue(TEXT("the file parses as a place"), Parsed.bSuccess))
	{
		AddError(Parsed.ErrorSummary());
		return false;
	}
	TestEqual(TEXT("all three bodies are read back"), Parsed.Place.Entities.Num(), 3);
	TestTrue(TEXT("and the zone"), Parsed.Place.Settings.Water.IsSet());
	if (Parsed.Place.Settings.Water.IsSet())
	{
		TestEqual(TEXT("with its yaw"), Parsed.Place.Settings.Water->ZoneYawDeg, 45.0);
		TestEqual(TEXT("and extent"), Parsed.Place.Settings.Water->ZoneExtent.X, 102400.0);
	}
	if (const FMantleWaterBodyProps* Back = Parsed.Place.Entities.Num() == 3 ? Parsed.Place.Entities[2].Props.TryGet<FMantleWaterBodyProps>() : nullptr)
	{
		TestTrue(TEXT("the ocean keeps its centring"), Back->CenterOnZone.IsSet() && *Back->CenterOnZone);
		TestEqual(TEXT("and its waves"), Back->Waves.Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterBodyExportsEntityTest,
	"Mantle.Editor.Water.ExportsABodyAsAWaterBodyEntity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterBodyExportsEntityTest::RunTest(const FString& Parameters)
{
	using namespace MantleWaterBodyExportTestPrivate;

	if (!FMantleWaterBodyExport::IsWaterPluginLoaded())
	{
		AddInfo(TEXT("The Water plugin is not loaded in this editor, so there is no water body to spawn; skipped."));
		return true;
	}
	FWaterTestWorld Fixture;
	if (!TestNotNull(TEXT("a world"), Fixture.World))
	{
		return false;
	}
	AActor* Lake = SpawnWaterActor(Fixture.World, TEXT("/Script/Water.WaterBodyLake"), FVector(0.0, 0.0, 100.0));
	if (!TestNotNull(TEXT("a lake"), Lake))
	{
		return false;
	}
	EMantleEntityType Type = EMantleEntityType::Group;
	TestTrue(TEXT("a lake classifies"), FMantlePlaceExporter::ClassifyActor(Lake, Type));
	TestEqual(TEXT("as a WaterBody"), Type, EMantleEntityType::WaterBody);

	// The plugin's own default lake spline is enough.
	const FMantleExportResult Result = FMantlePlaceExporter::Export(Fixture.World, FMantleExportOptions());
	int32 Bodies = 0;
	for (const FMantleEntity& Entity : Result.Place.Entities)
	{
		Bodies += Entity.Type == EMantleEntityType::WaterBody ? 1 : 0;
	}
	TestEqual(TEXT("the place has one water body"), Bodies, 1);
	TestTrue(TEXT("and the zone"), Result.Place.Settings.Water.IsSet());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
