// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceLimits.h"
#include "MantlePlaceParser.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Water bodies and the Water Zone (GhzGarage/mantle#997): what the parser reads and refuses. Places carry them and
// the game builds them; the runtime tests are in MantlePlaceWaterTest.cpp.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleWaterBodyTestPrivate
{
	/** A place whose settings are Settings and whose entities are Entities (the text between the brackets). */
	FMantleParseResult ParsePlace(const FString& Settings, const FString& Entities)
	{
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_water001\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Water\",\"authorId\":\"usr_4b1d\"},\"settings\":{%s},\"assets\":[],\"entities\":[%s]}"),
			*Settings, *Entities);
		return FMantlePlaceParser::ParseFromString(Json);
	}

	/** One WaterBody entity (id ent_water_<Number>) with Properties (the text between the braces). */
	FString WaterEntity(int32 Number, const FString& Properties)
	{
		return FString::Printf(
			TEXT("{\"id\":\"ent_water_%d\",\"type\":\"WaterBody\",\"parent\":null,"
				 "\"transform\":{\"location\":[%d,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{%s}}"),
			Number, Number * 100, *Properties);
	}

	FMantleParseResult ParseWater(const FString& Properties)
	{
		return ParsePlace(TEXT(""), WaterEntity(0, Properties));
	}

	/** A spline point; Extra is more keys after depth, such as ",\"width\":600". */
	FString Point(int32 X, const FString& Extra = TEXT(""))
	{
		return FString::Printf(
			TEXT("{\"location\":[%d,0,0],\"arriveTangent\":[100,0,0],\"leaveTangent\":[100,0,0],\"depth\":500%s}"), X, *Extra);
	}

	/** A spline of Count points, each carrying Extra. */
	FString Spline(int32 Count, const FString& Extra = TEXT(""))
	{
		FString Points;
		for (int32 i = 0; i < Count; ++i)
		{
			Points += (i ? TEXT(",") : TEXT("")) + Point(i * 100, Extra);
		}
		return FString::Printf(TEXT("\"spline\":{\"points\":[%s]}"), *Points);
	}

	const FMantleWaterBodyProps* WaterOf(const FMantleParseResult& Result, int32 Index = 0)
	{
		return Result.Place.Entities.IsValidIndex(Index) ? Result.Place.Entities[Index].Props.TryGet<FMantleWaterBodyProps>() : nullptr;
	}

	bool HasErrorAt(const FMantleParseResult& Result, const FString& Path)
	{
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error) { return Error.Path == Path; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterBodyParseTest,
	"Mantle.Runtime.WaterBody.Parser.ReadsEveryBodyType",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterBodyParseTest::RunTest(const FString& Parameters)
{
	using namespace MantleWaterBodyTestPrivate;

	const FMantleParseResult Lake = ParseWater(TEXT("\"bodyType\":\"lake\",") + Spline(4));
	if (!TestTrue(TEXT("a lake with four points parses"), Lake.bSuccess))
	{
		AddError(Lake.ErrorSummary());
		return false;
	}
	const FMantleWaterBodyProps* LakeProps = WaterOf(Lake);
	if (!TestNotNull(TEXT("a water body"), LakeProps))
	{
		return false;
	}
	TestTrue(TEXT("a lake"), LakeProps->BodyType == EMantleWaterBodyType::Lake);
	TestEqual(TEXT("four points"), LakeProps->SplinePoints.Num(), 4);
	TestTrue(TEXT("collision defaults on"), LakeProps->bCollision);
	TestEqual(TEXT("flat: no waves"), LakeProps->Waves.Num(), 0);
	TestFalse(TEXT("no albedo"), LakeProps->Albedo.IsSet());
	TestTrue(TEXT("interp defaults to curve"), LakeProps->SplinePoints[0].Interp == EMantleWaterSplinePointType::Curve);
	TestEqual(TEXT("depth"), LakeProps->SplinePoints[1].Depth, 500.0);
	TestTrue(TEXT("a point's location"), LakeProps->SplinePoints[2].Location.Equals(FVector(200.0, 0.0, 0.0)));

	// A river: open, width required, velocity optional, interp read.
	const FMantleParseResult River = ParseWater(
		TEXT("\"bodyType\":\"river\",\"spline\":{\"points\":[") + Point(0, TEXT(",\"width\":600,\"velocity\":120,\"interp\":\"linear\"")) + TEXT(",")
		+ Point(100, TEXT(",\"width\":800")) + TEXT("]}"));
	if (TestTrue(TEXT("a river with two points parses"), River.bSuccess))
	{
		const FMantleWaterBodyProps* Props = WaterOf(River);
		TestEqual(TEXT("two points"), Props->SplinePoints.Num(), 2);
		TestEqual(TEXT("width"), Props->SplinePoints[0].Width, 600.0);
		TestEqual(TEXT("velocity"), Props->SplinePoints[0].Velocity, 120.0);
		TestEqual(TEXT("velocity defaults to 0"), Props->SplinePoints[1].Velocity, 0.0);
		TestTrue(TEXT("interp linear"), Props->SplinePoints[0].Interp == EMantleWaterSplinePointType::Linear);
	}
	else
	{
		AddError(River.ErrorSummary());
	}

	// An ocean with every ocean-only key and waves.
	const FMantleParseResult Ocean = ParseWater(
		TEXT("\"bodyType\":\"ocean\",") + Spline(3)
		+ TEXT(",\"collisionExtent\":[200000,200000,10000],\"oceanExtent\":[409600,409600],\"centerOnZone\":true,")
		  TEXT("\"waves\":{\"list\":[{\"wavelength\":1500,\"amplitude\":40,\"steepness\":0.5,\"directionDeg\":-180},")
		  TEXT("{\"wavelength\":700,\"amplitude\":0,\"steepness\":1,\"directionDeg\":180,\"phaseOffset\":2.5}]},")
		  TEXT("\"look\":{\"albedo\":[0.1,0.2,0.3],\"absorption\":[1,0,0]},\"collision\":false,\"collisionHeightOffset\":-20"));
	if (TestTrue(TEXT("an ocean with every key parses"), Ocean.bSuccess))
	{
		const FMantleWaterBodyProps* Props = WaterOf(Ocean);
		TestTrue(TEXT("collision extent"), Props->CollisionExtent.IsSet() && Props->CollisionExtent->Equals(FVector(200000.0, 200000.0, 10000.0)));
		TestTrue(TEXT("ocean extent"), Props->OceanExtent.IsSet() && Props->OceanExtent->X == 409600.0);
		TestTrue(TEXT("centred on the zone"), Props->CenterOnZone.IsSet() && Props->CenterOnZone.GetValue());
		TestEqual(TEXT("two waves"), Props->Waves.Num(), 2);
		TestEqual(TEXT("phase offset"), Props->Waves[1].PhaseOffset, 2.5);
		TestEqual(TEXT("phase offset defaults to 0"), Props->Waves[0].PhaseOffset, 0.0);
		TestTrue(TEXT("albedo set, scattering not"), Props->Albedo.IsSet() && !Props->Scattering.IsSet() && Props->Absorption.IsSet());
		TestFalse(TEXT("collision off"), Props->bCollision);
		TestTrue(TEXT("collision height offset"), Props->CollisionHeightOffset.IsSet() && Props->CollisionHeightOffset.GetValue() == -20.0);
	}
	else
	{
		AddError(Ocean.ErrorSummary());
	}

	// A custom body names a mesh, and no spline.
	const FMantleParseResult Custom = ParseWater(TEXT("\"bodyType\":\"custom\",\"mesh\":\"ast_u00000000000000000000000000000000\""));
	if (TestTrue(TEXT("a custom body with a mesh parses"), Custom.bSuccess))
	{
		TestEqual(TEXT("the mesh"), WaterOf(Custom)->MeshId, FString(TEXT("ast_u00000000000000000000000000000000")));
		TestEqual(TEXT("no spline"), WaterOf(Custom)->SplinePoints.Num(), 0);
	}
	else
	{
		AddError(Custom.ErrorSummary());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterBodyRefusalsTest,
	"Mantle.Runtime.WaterBody.Parser.RefusesWhatTheTypeDoesNotTake",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterBodyRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace MantleWaterBodyTestPrivate;

	struct FCase
	{
		const TCHAR* What;
		FString Properties;
		FString Path;
	};
	const FString Mesh = TEXT("\"mesh\":\"ast_u00000000000000000000000000000000\"");
	const FString Waves = TEXT("\"waves\":{\"list\":[{\"wavelength\":1500,\"amplitude\":40,\"steepness\":0.5,\"directionDeg\":30}]}");
	const TArray<FCase> Cases = {
		{ TEXT("an unknown body type"), TEXT("\"bodyType\":\"pond\",") + Spline(4), TEXT("/entities/0/properties/bodyType") },
		{ TEXT("no body type"), Spline(4), TEXT("/entities/0/properties/bodyType") },
		{ TEXT("a lake with no spline"), TEXT("\"bodyType\":\"lake\""), TEXT("/entities/0/properties/spline") },
		{ TEXT("a lake with a mesh"), TEXT("\"bodyType\":\"lake\",") + Spline(4) + TEXT(",") + Mesh, TEXT("/entities/0/properties/mesh") },
		{ TEXT("a custom body with no mesh"), TEXT("\"bodyType\":\"custom\""), TEXT("/entities/0/properties/mesh") },
		{ TEXT("a custom body with a spline"), TEXT("\"bodyType\":\"custom\",") + Mesh + TEXT(",") + Spline(4), TEXT("/entities/0/properties/spline") },
		{ TEXT("a lake with two points"), TEXT("\"bodyType\":\"lake\",") + Spline(2), TEXT("/entities/0/properties/spline/points") },
		{ TEXT("a river with one point"), TEXT("\"bodyType\":\"river\",") + Spline(1, TEXT(",\"width\":5")), TEXT("/entities/0/properties/spline/points") },
		{ TEXT("a spline of 257 points"), TEXT("\"bodyType\":\"lake\",") + Spline(257), TEXT("/entities/0/properties/spline/points") },
		{ TEXT("a width on a lake"), TEXT("\"bodyType\":\"lake\",") + Spline(4, TEXT(",\"width\":5")), TEXT("/entities/0/properties/spline/points/0/width") },
		{ TEXT("a velocity on a lake"), TEXT("\"bodyType\":\"lake\",") + Spline(4, TEXT(",\"velocity\":5")), TEXT("/entities/0/properties/spline/points/0/velocity") },
		{ TEXT("a river with no width"), TEXT("\"bodyType\":\"river\",") + Spline(2), TEXT("/entities/0/properties/spline/points/0/width") },
		{ TEXT("waves on a river"), TEXT("\"bodyType\":\"river\",") + Spline(2, TEXT(",\"width\":5")) + TEXT(",") + Waves, TEXT("/entities/0/properties/waves") },
		{ TEXT("an ocean extent on a lake"), TEXT("\"bodyType\":\"lake\",") + Spline(4) + TEXT(",\"oceanExtent\":[1000,1000]"), TEXT("/entities/0/properties/oceanExtent") },
		{ TEXT("a collision extent on a lake"), TEXT("\"bodyType\":\"lake\",") + Spline(4) + TEXT(",\"collisionExtent\":[1,1,1]"), TEXT("/entities/0/properties/collisionExtent") },
		{ TEXT("centerOnZone on a lake"), TEXT("\"bodyType\":\"lake\",") + Spline(4) + TEXT(",\"centerOnZone\":true"), TEXT("/entities/0/properties/centerOnZone") },
		{ TEXT("a point with no depth"),
			TEXT("\"bodyType\":\"lake\",\"spline\":{\"points\":[{\"location\":[0,0,0],\"arriveTangent\":[0,0,0],\"leaveTangent\":[0,0,0]},")
				+ Point(100) + TEXT(",") + Point(200) + TEXT("]}"),
			TEXT("/entities/0/properties/spline/points/0/depth") },
		{ TEXT("an unknown interp"), TEXT("\"bodyType\":\"lake\",") + Spline(4, TEXT(",\"interp\":\"bezier\"")), TEXT("/entities/0/properties/spline/points/0/interp") },
		{ TEXT("a steepness above 1"),
			TEXT("\"bodyType\":\"lake\",") + Spline(4)
				+ TEXT(",\"waves\":{\"list\":[{\"wavelength\":1500,\"amplitude\":40,\"steepness\":1.5,\"directionDeg\":30}]}"),
			TEXT("/entities/0/properties/waves/list/0/steepness") },
		{ TEXT("a direction past 180"),
			TEXT("\"bodyType\":\"lake\",") + Spline(4)
				+ TEXT(",\"waves\":{\"list\":[{\"wavelength\":1500,\"amplitude\":40,\"steepness\":0.5,\"directionDeg\":181}]}"),
			TEXT("/entities/0/properties/waves/list/0/directionDeg") },
		{ TEXT("an empty wave list"), TEXT("\"bodyType\":\"lake\",") + Spline(4) + TEXT(",\"waves\":{\"list\":[]}"), TEXT("/entities/0/properties/waves/list") },
		{ TEXT("an ocean extent past the limit"),
			TEXT("\"bodyType\":\"ocean\",") + Spline(4) + TEXT(",\"oceanExtent\":[409601,1000]"), TEXT("/entities/0/properties/oceanExtent/0") },
		{ TEXT("a look colour past 1"), TEXT("\"bodyType\":\"lake\",") + Spline(4) + TEXT(",\"look\":{\"albedo\":[0,2,0]}"),
			TEXT("/entities/0/properties/look/albedo/1") },
		{ TEXT("a collision that is not a boolean"), TEXT("\"bodyType\":\"lake\",") + Spline(4) + TEXT(",\"collision\":\"yes\""),
			TEXT("/entities/0/properties/collision") },
		{ TEXT("an unknown key"), TEXT("\"bodyType\":\"lake\",") + Spline(4) + TEXT(",\"depth\":5"), TEXT("/entities/0/properties/depth") },
	};
	for (const FCase& Case : Cases)
	{
		const FMantleParseResult Result = ParseWater(Case.Properties);
		TestFalse(FString::Printf(TEXT("%s is refused"), Case.What), Result.bSuccess);
		TestTrue(FString::Printf(TEXT("%s is reported at %s"), Case.What, *Case.Path), HasErrorAt(Result, Case.Path));
	}

	// 65 waves.
	FString ManyWaves;
	for (int32 i = 0; i < FMantlePlaceLimits::MaxWaterWavesPerBody + 1; ++i)
	{
		ManyWaves += (i ? TEXT(",") : TEXT("")) + FString(TEXT("{\"wavelength\":1500,\"amplitude\":40,\"steepness\":0.5,\"directionDeg\":30}"));
	}
	const FMantleParseResult TooManyWaves = ParseWater(TEXT("\"bodyType\":\"lake\",") + Spline(4) + TEXT(",\"waves\":{\"list\":[") + ManyWaves + TEXT("]}"));
	TestTrue(TEXT("65 waves are refused at the list"), HasErrorAt(TooManyWaves, TEXT("/entities/0/properties/waves/list")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterBodyPlaceLimitsTest,
	"Mantle.Runtime.WaterBody.Parser.HoldsThePlaceToItsLimits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterBodyPlaceLimitsTest::RunTest(const FString& Parameters)
{
	using namespace MantleWaterBodyTestPrivate;

	const FString Lake = TEXT("\"bodyType\":\"lake\",") + Spline(3);
	FString Bodies;
	for (int32 i = 0; i < FMantlePlaceLimits::MaxWaterBodiesPerPlace; ++i)
	{
		Bodies += (i ? TEXT(",") : TEXT("")) + WaterEntity(i, Lake);
	}
	TestTrue(TEXT("64 bodies are accepted"), ParsePlace(TEXT(""), Bodies).bSuccess);
	const FMantleParseResult Over = ParsePlace(TEXT(""), Bodies + TEXT(",") + WaterEntity(FMantlePlaceLimits::MaxWaterBodiesPerPlace, Lake));
	TestFalse(TEXT("65 bodies are refused"), Over.bSuccess);
	TestTrue(TEXT("at the type of the 65th"), HasErrorAt(Over, FString::Printf(TEXT("/entities/%d/type"), FMantlePlaceLimits::MaxWaterBodiesPerPlace)));

	// 16 bodies of 256 points are exactly the place's spline points; one more body with 3 crosses them.
	const int32 FullBodies = FMantlePlaceLimits::MaxWaterSplinePointsPerPlace / FMantlePlaceLimits::MaxWaterSplinePointsPerBody;
	const FString BigLake = TEXT("\"bodyType\":\"lake\",") + Spline(FMantlePlaceLimits::MaxWaterSplinePointsPerBody);
	FString Big;
	for (int32 i = 0; i < FullBodies; ++i)
	{
		Big += (i ? TEXT(",") : TEXT("")) + WaterEntity(i, BigLake);
	}
	TestTrue(TEXT("4096 spline points are accepted"), ParsePlace(TEXT(""), Big).bSuccess);
	const FMantleParseResult OverPoints = ParsePlace(TEXT(""), Big + TEXT(",") + WaterEntity(FullBodies, Lake));
	TestFalse(TEXT("4099 spline points are refused"), OverPoints.bSuccess);
	TestTrue(TEXT("at the type of the body that crosses"), HasErrorAt(OverPoints, FString::Printf(TEXT("/entities/%d/type"), FullBodies)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleWaterZoneParseTest,
	"Mantle.Runtime.WaterBody.Parser.ReadsTheWaterZone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleWaterZoneParseTest::RunTest(const FString& Parameters)
{
	using namespace MantleWaterBodyTestPrivate;

	TestFalse(TEXT("no settings.water: no zone"), ParsePlace(TEXT(""), TEXT("")).Place.Settings.Water.IsSet());

	const FMantleParseResult Empty = ParsePlace(TEXT("\"water\":{}"), TEXT(""));
	if (TestTrue(TEXT("an empty settings.water parses"), Empty.bSuccess) && TestTrue(TEXT("and is a zone"), Empty.Place.Settings.Water.IsSet()))
	{
		TestEqual(TEXT("extent X defaults to 51200"), Empty.Place.Settings.Water->ZoneExtent.X, 51200.0);
		TestEqual(TEXT("extent Y defaults to 51200"), Empty.Place.Settings.Water->ZoneExtent.Y, 51200.0);
		TestTrue(TEXT("location defaults to the origin"), Empty.Place.Settings.Water->ZoneLocation.IsZero());
		TestEqual(TEXT("yaw defaults to 0"), Empty.Place.Settings.Water->ZoneYawDeg, 0.0);
	}

	const FMantleParseResult Full = ParsePlace(TEXT("\"water\":{\"zoneExtent\":[409600,1],\"zoneLocation\":[10,-20,30],\"zoneYawDeg\":-180}"), TEXT(""));
	if (TestTrue(TEXT("every key at an edge parses"), Full.bSuccess) && Full.Place.Settings.Water.IsSet())
	{
		TestEqual(TEXT("extent"), Full.Place.Settings.Water->ZoneExtent.X, 409600.0);
		TestTrue(TEXT("location"), Full.Place.Settings.Water->ZoneLocation.Equals(FVector(10.0, -20.0, 30.0)));
		TestEqual(TEXT("yaw"), Full.Place.Settings.Water->ZoneYawDeg, -180.0);
	}

	const TCHAR* const Refused[][2] = {
		{ TEXT("\"water\":{\"zoneExtent\":[409601,1000]}"), TEXT("/settings/water/zoneExtent/0") },
		{ TEXT("\"water\":{\"zoneExtent\":[1000]}"), TEXT("/settings/water/zoneExtent") },
		{ TEXT("\"water\":{\"zoneYawDeg\":181}"), TEXT("/settings/water/zoneYawDeg") },
		{ TEXT("\"water\":{\"zoneLocation\":[0,0]}"), TEXT("/settings/water/zoneLocation") },
		{ TEXT("\"water\":{\"zoneHeight\":100}"), TEXT("/settings/water/zoneHeight") },
	};
	for (const TCHAR* const (&Case)[2] : Refused)
	{
		const FMantleParseResult Result = ParsePlace(Case[0], TEXT(""));
		TestFalse(FString::Printf(TEXT("%s is refused"), Case[0]), Result.bSuccess);
		TestTrue(FString::Printf(TEXT("%s is reported at %s"), Case[0], Case[1]), HasErrorAt(Result, Case[1]));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
