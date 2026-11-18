// Copyright Joshua Eger. All Rights Reserved.
#include "Curves/CurveFloat.h"
#include "MantleEnvironment.h"
#include "MantleEnvironmentDefaults.h"
#include "MantleEnvironmentSubsystem.h"
#include "MantlePlaceBuilder.h"
#include "MantlePlaceParser.h"

#include "Components/LightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/Scene.h"
#include "HAL/IConsoleManager.h"
#include "Engine/SkyLight.h"
#include "Engine/World.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The place's exposure and fog (settings.environment.exposure and .fog): the parser's rules for both, and how an exposure
// becomes a post-process volume's settings. No network, no world.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleEnvironmentLookTestPrivate
{
	/** A complete place whose settings.environment is Environment (the text between the braces). */
	FString PlaceJson(const FString& Environment)
	{
		return FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_look_test\",\"revision\":{\"number\":1,\"state\":\"draft\"},")
				TEXT("\"metadata\":{\"name\":\"Look test\",\"authorId\":\"usr_test\"},\"settings\":{\"environment\":{%s}},\"assets\":[],\"entities\":[]}"),
			*Environment);
	}

	bool HasErrorAt(const FMantleParseResult& Result, const FString& Path)
	{
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error)
		{
			return Error.Path.Equals(Path, ESearchCase::CaseSensitive);
		});
	}

	/** Parses Environment and says whether it was refused at Path (and refused at all). */
	bool RefusedAt(const FString& Environment, const FString& Path)
	{
		const FMantleParseResult Result = FMantlePlaceParser::ParseFromString(PlaceJson(Environment));
		return !Result.bSuccess && HasErrorAt(Result, Path);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentExposureParserAcceptsTest,
	"Mantle.Runtime.Parser.EnvironmentLook.Accepts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentExposureParserAcceptsTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentLookTestPrivate;

	// Every key of both blocks.
	const FMantleParseResult Full = FMantlePlaceParser::ParseFromString(PlaceJson(
		TEXT("\"exposure\":{\"mode\":\"auto\",\"bias\":-1.5,\"minBrightness\":0.5,\"maxBrightness\":6},")
		TEXT("\"fog\":{\"density\":0.005,\"heightFalloff\":0.4,\"inscatteringColor\":[0.447,0.638,1],\"startDistance\":2500,")
		TEXT("\"directionalInscatteringColor\":[0.25,0.25,0.125]}")));
	if (!TestTrue(TEXT("a place with exposure and fog parses"), Full.bSuccess))
	{
		AddError(Full.ErrorSummary());
		return false;
	}
	const FMantleEnvironmentSettings& Environment = Full.Place.Settings.Environment;
	TestFalse(TEXT("the block is not empty"), Environment.IsEmpty());
	if (TestTrue(TEXT("the exposure is read"), Environment.Exposure.IsSet()))
	{
		TestTrue(TEXT("mode auto"), Environment.Exposure->Mode == EMantleExposureMode::Auto);
		TestEqual(TEXT("bias"), Environment.Exposure->Bias, -1.5);
		TestTrue(TEXT("min brightness"), Environment.Exposure->MinBrightness.IsSet() && *Environment.Exposure->MinBrightness == 0.5);
		TestTrue(TEXT("max brightness"), Environment.Exposure->MaxBrightness.IsSet() && *Environment.Exposure->MaxBrightness == 6.0);
	}
	if (TestTrue(TEXT("the fog is read"), Environment.Fog.IsSet()))
	{
		const FMantleFogSettings& Fog = *Environment.Fog;
		TestFalse(TEXT("and is not empty"), Fog.IsEmpty());
		TestTrue(TEXT("density"), Fog.Density.IsSet() && *Fog.Density == 0.005);
		TestTrue(TEXT("height falloff"), Fog.HeightFalloff.IsSet() && *Fog.HeightFalloff == 0.4);
		TestTrue(TEXT("start distance"), Fog.StartDistance.IsSet() && *Fog.StartDistance == 2500.0);
		if (TestTrue(TEXT("inscattering color"), Fog.InscatteringColor.IsSet()))
		{
			TestNearlyEqual(TEXT("its green"), Fog.InscatteringColor->G, 0.638f, 1.0e-4f);
			TestNearlyEqual(TEXT("its blue"), Fog.InscatteringColor->B, 1.0f, 1.0e-4f);
		}
		if (TestTrue(TEXT("directional inscattering color"), Fog.DirectionalInscatteringColor.IsSet()))
		{
			TestNearlyEqual(TEXT("its blue"), Fog.DirectionalInscatteringColor->B, 0.125f, 1.0e-4f);
		}
	}

	// Each at its edge: a color brighter than 1 is allowed, to 10 a channel.
	const FMantleParseResult Edges = FMantlePlaceParser::ParseFromString(PlaceJson(
		TEXT("\"exposure\":{\"mode\":\"auto\",\"bias\":4,\"minBrightness\":0.03,\"maxBrightness\":0.03},")
		TEXT("\"fog\":{\"density\":0.05,\"heightFalloff\":0.001,\"inscatteringColor\":[10,10,10],\"startDistance\":1000000,")
		TEXT("\"directionalInscatteringColor\":[0,0,0]}")));
	TestTrue(TEXT("the ranges' edges parse"), Edges.bSuccess);
	TestTrue(TEXT("a bias at the lower edge parses"), FMantlePlaceParser::ParseFromString(PlaceJson(TEXT("\"exposure\":{\"bias\":-4}"))).bSuccess);

	// Fixed is the default: a bias alone, or mode fixed spelled out.
	const FMantleParseResult Biased = FMantlePlaceParser::ParseFromString(PlaceJson(TEXT("\"exposure\":{\"bias\":0.5}")));
	if (TestTrue(TEXT("a bias alone parses"), Biased.bSuccess) && TestTrue(TEXT("and sets the exposure"), Biased.Place.Settings.Environment.Exposure.IsSet()))
	{
		TestTrue(TEXT("fixed, with no mode written"), Biased.Place.Settings.Environment.Exposure->Mode == EMantleExposureMode::Fixed);
		TestFalse(TEXT("with no range"), Biased.Place.Settings.Environment.Exposure->MinBrightness.IsSet());
	}
	TestTrue(TEXT("mode fixed parses"), FMantlePlaceParser::ParseFromString(PlaceJson(TEXT("\"exposure\":{\"mode\":\"fixed\"}"))).bSuccess);
	TestTrue(TEXT("an empty fog parses"), FMantlePlaceParser::ParseFromString(PlaceJson(TEXT("\"fog\":{}"))).bSuccess);

	// A place with neither is the place it always was.
	const FMantleParseResult Plain = FMantlePlaceParser::ParseFromString(PlaceJson(TEXT("\"saturation\":1.2")));
	TestTrue(TEXT("a place without them parses"), Plain.bSuccess);
	TestFalse(TEXT("with no exposure"), Plain.Place.Settings.Environment.Exposure.IsSet());
	TestFalse(TEXT("and no fog"), Plain.Place.Settings.Environment.Fog.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentExposureParserRefusesTest,
	"Mantle.Runtime.Parser.EnvironmentLook.Refuses",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentExposureParserRefusesTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentLookTestPrivate;
	const FString Exposure = TEXT("/settings/environment/exposure");
	const FString Fog = TEXT("/settings/environment/fog");

	TestTrue(TEXT("exposure not an object"), RefusedAt(TEXT("\"exposure\":\"auto\""), Exposure));
	TestTrue(TEXT("a mode outside the list"), RefusedAt(TEXT("\"exposure\":{\"mode\":\"manual\"}"), Exposure + TEXT("/mode")));
	TestTrue(TEXT("a mode in the wrong case"), RefusedAt(TEXT("\"exposure\":{\"mode\":\"Auto\"}"), Exposure + TEXT("/mode")));
	TestTrue(TEXT("an unknown exposure key"), RefusedAt(TEXT("\"exposure\":{\"compensation\":1}"), Exposure + TEXT("/compensation")));
	TestTrue(TEXT("a bias past 4"), RefusedAt(TEXT("\"exposure\":{\"bias\":4.5}"), Exposure + TEXT("/bias")));
	TestTrue(TEXT("a bias below -4"), RefusedAt(TEXT("\"exposure\":{\"bias\":-4.5}"), Exposure + TEXT("/bias")));
	TestTrue(TEXT("a bias that is a string"), RefusedAt(TEXT("\"exposure\":{\"bias\":\"1\"}"), Exposure + TEXT("/bias")));
	TestTrue(TEXT("a minimum below 0.03"),
		RefusedAt(TEXT("\"exposure\":{\"mode\":\"auto\",\"minBrightness\":0.01}"), Exposure + TEXT("/minBrightness")));
	TestTrue(TEXT("a maximum past 20"),
		RefusedAt(TEXT("\"exposure\":{\"mode\":\"auto\",\"maxBrightness\":21}"), Exposure + TEXT("/maxBrightness")));
	TestTrue(TEXT("a range without mode auto"), RefusedAt(TEXT("\"exposure\":{\"minBrightness\":0.5}"), Exposure + TEXT("/minBrightness")));
	TestTrue(TEXT("a range with mode fixed"),
		RefusedAt(TEXT("\"exposure\":{\"mode\":\"fixed\",\"maxBrightness\":4}"), Exposure + TEXT("/maxBrightness")));
	TestTrue(TEXT("a minimum above the maximum"),
		RefusedAt(TEXT("\"exposure\":{\"mode\":\"auto\",\"minBrightness\":4,\"maxBrightness\":2}"), Exposure + TEXT("/minBrightness")));

	TestTrue(TEXT("fog not an object"), RefusedAt(TEXT("\"fog\":0.01"), Fog));
	TestTrue(TEXT("an unknown fog key"), RefusedAt(TEXT("\"fog\":{\"maxOpacity\":0.5}"), Fog + TEXT("/maxOpacity")));
	TestTrue(TEXT("a density past 0.05"), RefusedAt(TEXT("\"fog\":{\"density\":0.06}"), Fog + TEXT("/density")));
	TestTrue(TEXT("a negative density"), RefusedAt(TEXT("\"fog\":{\"density\":-0.001}"), Fog + TEXT("/density")));
	TestTrue(TEXT("a falloff of 0"), RefusedAt(TEXT("\"fog\":{\"heightFalloff\":0}"), Fog + TEXT("/heightFalloff")));
	TestTrue(TEXT("a falloff past 2"), RefusedAt(TEXT("\"fog\":{\"heightFalloff\":2.5}"), Fog + TEXT("/heightFalloff")));
	TestTrue(TEXT("a color channel past 10"),
		RefusedAt(TEXT("\"fog\":{\"inscatteringColor\":[0.5,10.5,0.5]}"), Fog + TEXT("/inscatteringColor/1")));
	TestTrue(TEXT("a negative color channel"),
		RefusedAt(TEXT("\"fog\":{\"inscatteringColor\":[-0.1,0.5,0.5]}"), Fog + TEXT("/inscatteringColor/0")));
	TestTrue(TEXT("a color of two numbers"),
		RefusedAt(TEXT("\"fog\":{\"directionalInscatteringColor\":[0.2,0.2]}"), Fog + TEXT("/directionalInscatteringColor")));
	TestTrue(TEXT("a negative start distance"), RefusedAt(TEXT("\"fog\":{\"startDistance\":-1}"), Fog + TEXT("/startDistance")));
	TestTrue(TEXT("a start distance past 1e6"), RefusedAt(TEXT("\"fog\":{\"startDistance\":1000001}"), Fog + TEXT("/startDistance")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentExposureSettingsTest,
	"Mantle.Runtime.Environment.PlaceExposure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentExposureSettingsTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentDefaults;

	// Always Unreal's auto exposure as Ultra Dynamic Sky's physical mode sets it, whatever the place says.
	{
		FPostProcessSettings Settings;
		UMantleEnvironmentSubsystem::ApplyExposureSettings(Settings, 0.0f, TOptional<FMantleExposureSettings>(), NightExposureFloor);
		TestTrue(TEXT("histogram metering"), Settings.bOverride_AutoExposureMethod && Settings.AutoExposureMethod == EAutoExposureMethod::AEM_Histogram);
		TestTrue(TEXT("compensation 0, not the engine's +1"), Settings.bOverride_AutoExposureBias && Settings.AutoExposureBias == 0.0f);
		TestTrue(TEXT("the floor given"), Settings.bOverride_AutoExposureMinBrightness && Settings.AutoExposureMinBrightness == NightExposureFloor);
		TestTrue(TEXT("up to the pack's 20"), Settings.bOverride_AutoExposureMaxBrightness && Settings.AutoExposureMaxBrightness == MaxExposure);
		TestTrue(TEXT("the physical curve"), Settings.bOverride_AutoExposureBiasCurve
			&& Settings.AutoExposureBiasCurve == UMantleEnvironmentSubsystem::GetPhysicalExposureCurve());
		TestTrue(TEXT("twice the engine's speed into the dark"), Settings.bOverride_AutoExposureSpeedUp && Settings.AutoExposureSpeedUp == ExposureSpeedUp);
		TestTrue(TEXT("and into the light"), Settings.bOverride_AutoExposureSpeedDown && Settings.AutoExposureSpeedDown == ExposureSpeedDown);
	}
	// The place's bias applies, a script's replaces it; the place's old mode and range keys do not.
	{
		FMantleExposureSettings Place;
		Place.Mode = EMantleExposureMode::Auto;
		Place.Bias = -1.0;
		Place.MinBrightness = 0.5;
		Place.MaxBrightness = 4.0;
		FPostProcessSettings Settings;
		UMantleEnvironmentSubsystem::ApplyExposureSettings(Settings, 0.0f, Place, DayExposureFloor);
		TestEqual(TEXT("the place's bias"), Settings.AutoExposureBias, -1.0f);
		TestEqual(TEXT("not the place's minimum"), Settings.AutoExposureMinBrightness, DayExposureFloor);
		TestEqual(TEXT("nor its maximum"), Settings.AutoExposureMaxBrightness, MaxExposure);
		UMantleEnvironmentSubsystem::ApplyExposureSettings(Settings, 3.0f, Place, DayExposureFloor);
		TestEqual(TEXT("a script's bias replaces the place's"), Settings.AutoExposureBias, 3.0f);
	}
	// A floor above the top cannot empty the range.
	{
		FPostProcessSettings Settings;
		UMantleEnvironmentSubsystem::ApplyExposureSettings(Settings, 0.0f, TOptional<FMantleExposureSettings>(), 40.0f);
		TestTrue(TEXT("the floor is at most the top"), Settings.AutoExposureMinBrightness <= Settings.AutoExposureMaxBrightness);
	}
	// The floor by the sun: the night's in the dark, none (the pack's) by day, moving between.
	TestEqual(TEXT("the night's floor"), ExposureFloorFor(-30.0), NightExposureFloor);
	TestEqual(TEXT("from 6 degrees down"), ExposureFloorFor(ExposureFloorNightBelowDegrees), NightExposureFloor);
	TestEqual(TEXT("the day's from 10 up"), ExposureFloorFor(ExposureFloorDayAboveDegrees), DayExposureFloor);
	TestEqual(TEXT("and at noon"), ExposureFloorFor(60.0), DayExposureFloor);
	const float Dusk = ExposureFloorFor(2.0);
	TestTrue(TEXT("between at dusk"), Dusk < NightExposureFloor && Dusk > DayExposureFloor);
	TestTrue(TEXT("inside Lumen's cached range at night (exposure about floor + 1.1 >= -3 with CachedLightingPreExposure 9)"),
		NightExposureFloor + 1.1f >= -3.0f);

	// The physical curve is the pack's 41 points: -1.73 in the dark, +1 in daylight.
	const UCurveFloat* Curve = UMantleEnvironmentSubsystem::GetPhysicalExposureCurve();
	if (TestNotNull(TEXT("the curve"), Curve))
	{
		TestTrue(TEXT("dark end"), FMath::IsNearlyEqual(Curve->GetFloatValue(-9.163f), -1.7288f, 1.0e-4f));
		TestTrue(TEXT("daylight end"), FMath::IsNearlyEqual(Curve->GetFloatValue(13.6f), 1.0f, 1.0e-4f));
		TestTrue(TEXT("linear between points"), FMath::IsNearlyEqual(Curve->GetFloatValue((12.462f + 13.031f) * 0.5f), (0.1507f + 0.4806f) * 0.5f, 1.0e-4f));
	}

	// The sky glows: the sun's ratio by day, unscaled at night, as the pack's physical mode measured.
	TestEqual(TEXT("sky glow by day"), PhysicalSkyScale(30.0), LightUnitScale);
	TestEqual(TEXT("sky glow at night"), PhysicalSkyScale(-30.0), 1.0f);
	TestTrue(TEXT("the sun's own light lasts longer than the sky's after sunset"), PhysicalSunScale(-6.0) > PhysicalSkyScale(-6.0));
	TestTrue(TEXT("the default sun is a clear noon's lux"), FMath::IsNearlyEqual(DefaultSunIntensity * LightUnitScale, 114688.0f, 1.0f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentInteriorTest,
	"Mantle.Runtime.Environment.Interior",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentInteriorTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentLookTestPrivate;

	// The keys parse: outdoors false is an interior, the threshold is a number in range, and a wrong type is refused.
	const FMantleParseResult Interior = FMantlePlaceParser::ParseFromString(
		PlaceJson(TEXT("\"outdoors\":false,\"bloom\":2,\"bloomThreshold\":2.0")));
	if (!TestTrue(TEXT("an interior place with a bloom threshold parses"), Interior.bSuccess))
	{
		AddError(Interior.ErrorSummary());
		return false;
	}
	const FMantleEnvironmentSettings& Settings = Interior.Place.Settings.Environment;
	TestTrue(TEXT("outdoors false is read"), Settings.bNoOutdoors);
	TestTrue(TEXT("and the threshold"), Settings.BloomThreshold.IsSet() && *Settings.BloomThreshold == 2.0);
	TestFalse(TEXT("a block with those keys is not empty"), Settings.IsEmpty());
	TestFalse(TEXT("outdoors is true unless the file says false"),
		FMantlePlaceParser::ParseFromString(PlaceJson(TEXT("\"outdoors\":true"))).Place.Settings.Environment.bNoOutdoors);
	TestTrue(TEXT("outdoors must be a boolean"), RefusedAt(TEXT("\"outdoors\":\"no\""), TEXT("/settings/environment/outdoors")));
	TestTrue(TEXT("a threshold past 8 is refused"), RefusedAt(TEXT("\"bloomThreshold\":9"), TEXT("/settings/environment/bloomThreshold")));
	TestTrue(TEXT("and a negative one"), RefusedAt(TEXT("\"bloomThreshold\":-1"), TEXT("/settings/environment/bloomThreshold")));

	// The threshold reaches the volume's settings, and unset leaves the engine's own.
	{
		FPostProcessSettings Applied;
		UMantleEnvironmentSubsystem::ApplyBloomThresholdSettings(Applied, TOptional<double>(2.0));
		TestTrue(TEXT("a threshold is overridden"), Applied.bOverride_BloomThreshold);
		TestEqual(TEXT("to the place's value"), Applied.BloomThreshold, 2.0f);
		UMantleEnvironmentSubsystem::ApplyBloomThresholdSettings(Applied, TOptional<double>());
		TestFalse(TEXT("an unset threshold is not overridden"), Applied.bOverride_BloomThreshold);
	}

	// An interior place gets no default sun, sky light, atmosphere, clouds or fog; an outdoor one gets all of them.
	auto CountEnvironmentActors = [this](const FMantlePlace& Place, int32& OutSuns, int32& OutOthers)
	{
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("MantleEnvironmentInteriorTestWorld"));
		if (!TestNotNull(TEXT("a world"), World))
		{
			return;
		}
		FMantleBuildResult Result;
		FMantleEnvironment::Apply(World, Place, Result);
		// Without a sun or sky nothing is bound, and every subsystem path that reads them copes: the clock still runs, the
		// sun gives no light, no sky weather is driven, and a tick is a no-op.
		if (UMantleEnvironmentSubsystem* Subsystem = World->GetSubsystem<UMantleEnvironmentSubsystem>())
		{
			if (Place.Settings.Environment.bNoOutdoors)
			{
				TestTrue(TEXT("an interior is marked"), Subsystem->IsInterior());
				TestEqual(TEXT("it has no sun to give light"), Subsystem->GetSunIntensity(), 0.0f);
				TestFalse(TEXT("no sky weather is driven"), Subsystem->HasWeatherSky());
				TestFalse(TEXT("and no night sky"), Subsystem->HasNightSky());
				TestFalse(TEXT("so the subsystem has nothing to tick"), Subsystem->IsTickable());
				TestEqual(TEXT("its natural weather is held at Clear"), static_cast<int32>(Subsystem->GetWeather().State), static_cast<int32>(EMantleWeatherState::Clear));
				Subsystem->Tick(0.016f);
			}
			else
			{
				TestFalse(TEXT("an outdoor place is not an interior"), Subsystem->IsInterior());
			}
		}
		OutSuns = 0;
		OutOthers = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->IsA<ADirectionalLight>())
			{
				++OutSuns;
			}
			else if (It->IsA<ASkyLight>() || It->IsA<ASkyAtmosphere>() || It->IsA<AVolumetricCloud>() || It->IsA<AExponentialHeightFog>())
			{
				++OutOthers;
			}
		}
		World->DestroyWorld(/*bInformEngineOfWorld*/ false);
	};

	int32 Suns = 0, Others = 0;
	CountEnvironmentActors(Interior.Place, Suns, Others);
	TestEqual(TEXT("an interior place spawns no default sun"), Suns, 0);
	TestEqual(TEXT("nor any other sky actor"), Others, 0);

	FMantlePlace Outdoor = Interior.Place;
	Outdoor.Settings.Environment.bNoOutdoors = false;
	CountEnvironmentActors(Outdoor, Suns, Others);
	TestEqual(TEXT("an outdoor place with no sun of its own gets the default one"), Suns, 1);
	TestTrue(TEXT("and the rest of the sky"), Others > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentInteriorLampTest,
	"Mantle.Runtime.Environment.InteriorLamp",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentInteriorLampTest::RunTest(const FString& Parameters)
{
	// A directional light outdoors is a sun, in Mantle units (x LightUnitScale lux). An interior has no sun: its directional
	// light is the creator's lamp, published in lux as the level had it (Unreal's default 10), and built at that.
	auto BuiltLux = [this](const bool bOutdoors) -> float
	{
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_lamp_test\",\"revision\":{\"number\":1,\"state\":\"draft\"},")
				TEXT("\"metadata\":{\"name\":\"Lamp test\",\"authorId\":\"usr_test\"},\"settings\":{\"environment\":{\"outdoors\":%s}},\"assets\":[],")
				TEXT("\"entities\":[{\"id\":\"ent_lamp\",\"type\":\"DirectionalLight\",\"parent\":null,")
				TEXT("\"transform\":{\"location\":[0,0,0],\"rotation\":[0,-45,0],\"scale\":[1,1,1]},\"properties\":{\"intensity\":10,\"color\":[1,1,1]}}]}"),
			bOutdoors ? TEXT("true") : TEXT("false"));
		const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
		if (!TestTrue(TEXT("the place parses"), Parsed.bSuccess))
		{
			AddError(Parsed.ErrorSummary());
			return -1.0f;
		}
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("MantleInteriorLampTestWorld"));
		if (!TestNotNull(TEXT("a world"), World))
		{
			return -1.0f;
		}
		FMantleBuildOptions Options;
		Options.bRuntimeEnvironment = false;
		const FMantleBuildResult Built = FMantlePlaceBuilder::Build(World, Parsed.Place, Options);
		float Lux = -1.0f;
		AActor* const* Actor = Built.Actors.Find(TEXT("ent_lamp"));
		if (const ADirectionalLight* Light = Actor ? Cast<ADirectionalLight>(*Actor) : nullptr)
		{
			Lux = Light->GetLightComponent()->Intensity;
		}
		World->DestroyWorld(/*bInformEngineOfWorld*/ false);
		return Lux;
	};

	TestEqual(TEXT("an interior's directional light at 10 builds at 10 lux"), BuiltLux(false), 10.0f);
	TestEqual(TEXT("an outdoor one at 10 is a sun: 10 x LightUnitScale lux"), BuiltLux(true), 10.0f * MantleEnvironmentDefaults::LightUnitScale);
	TestEqual(TEXT("an interior lamp's lux reads back as authored"), MantleEnvironmentDefaults::DirectionalLightAuthored(10.0, true), 10.0);
	TestEqual(TEXT("a sun's lux reads back in Mantle units"),
		MantleEnvironmentDefaults::DirectionalLightAuthored(MantleEnvironmentDefaults::DirectionalLightLux(3.5, false), false), 3.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleSkyLightStateOrderTest,
	"Mantle.Runtime.Environment.SkyLightStateOrder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleSkyLightStateOrderTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentLookTestPrivate;

	// A settings change reaches the post-process volume (local exposure) whichever caller sees it first: the sky light update
	// from outside the tick, then the tick. The tick used to ask only "did the state change just now", so the first caller
	// used the change up and the volume kept its old state.
	struct FScopedVariables
	{
		TArray<TPair<IConsoleVariable*, int32>> Saved;
		void Set(const TCHAR* Name, const int32 Value)
		{
			if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
			{
				if (!Saved.ContainsByPredicate([Variable](const TPair<IConsoleVariable*, int32>& Entry) { return Entry.Key == Variable; }))
				{
					Saved.Emplace(Variable, Variable->GetInt());
				}
				Variable->Set(Value, ECVF_SetByConsole);
			}
		}
		~FScopedVariables()
		{
			for (const TPair<IConsoleVariable*, int32>& Entry : Saved)
			{
				Entry.Key->Set(Entry.Value, ECVF_SetByConsole);
			}
		}
	} Variables;
	// Start from full Lumen (local exposure on).
	Variables.Set(TEXT("r.DynamicGlobalIlluminationMethod"), 1);
	Variables.Set(TEXT("r.Lumen.DiffuseIndirect.Allow"), 1);
	Variables.Set(TEXT("r.DistanceFieldAO"), 1);
	Variables.Set(TEXT("r.AOQuality"), 2);

	const FMantleParseResult Outdoor = FMantlePlaceParser::ParseFromString(PlaceJson(TEXT("\"outdoors\":true")));
	if (!TestTrue(TEXT("an outdoor place parses"), Outdoor.bSuccess))
	{
		AddError(Outdoor.ErrorSummary());
		return false;
	}
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, TEXT("MantleSkyLightStateOrderTestWorld"));
	if (!TestNotNull(TEXT("a world"), World))
	{
		return false;
	}
	FMantleBuildResult Result;
	FMantleEnvironment::Apply(World, Outdoor.Place, Result);
	UMantleEnvironmentSubsystem* Subsystem = World->GetSubsystem<UMantleEnvironmentSubsystem>();
	if (!TestNotNull(TEXT("the environment subsystem"), Subsystem))
	{
		World->DestroyWorld(/*bInformEngineOfWorld*/ false);
		return false;
	}
	auto LocalExposureOverridden = [World]()
	{
		for (TActorIterator<APostProcessVolume> It(World); It; ++It)
		{
			const FPostProcessSettings& Settings = It->Settings;
			if (It->bEnabled && Settings.bOverride_LocalExposureHighlightContrastScale && Settings.bOverride_LocalExposureShadowContrastScale
				&& Settings.LocalExposureHighlightContrastScale == 1.0f && Settings.LocalExposureShadowContrastScale == 1.0f)
			{
				return true;
			}
		}
		return false;
	};

	Subsystem->Tick(0.016f);
	TestFalse(TEXT("full Lumen leaves local exposure alone"), LocalExposureOverridden());

	// Nothing shades the sky light any more; the sky light update from outside the tick sees it first.
	Variables.Set(TEXT("r.DistanceFieldAO"), 0);
	Variables.Set(TEXT("r.DynamicGlobalIlluminationMethod"), 0);
	Subsystem->UpdateWeatherSky();
	Subsystem->Tick(0.016f);
	TestTrue(TEXT("the tick still switches local exposure off outdoors"), LocalExposureOverridden());

	// And back the other way, with the post-process update from outside the tick first.
	Variables.Set(TEXT("r.DynamicGlobalIlluminationMethod"), 1);
	Variables.Set(TEXT("r.DistanceFieldAO"), 1);
	Subsystem->UpdatePostProcess();
	Subsystem->Tick(0.016f);
	TestFalse(TEXT("and switches it back on"), LocalExposureOverridden());

	World->DestroyWorld(/*bInformEngineOfWorld*/ false);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
