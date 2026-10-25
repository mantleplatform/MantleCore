// Copyright Joshua Eger. All Rights Reserved.
#include "MantleClientFeatures.h"
#include "MantleCoverage.h"
#include "MantleEnvironment.h"
#include "MantleEnvironmentDefaults.h"
#include "MantleEnvironmentExport.h"
#include "MantlePlaceExporter.h"
#include "MantlePlaceParser.h"
#include "MantlePlaceSettingsActor.h"
#include "MantleSkyClock.h"
#include "MantleTargetFeatures.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextureCube.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

// The level's sun, sky, fog, clouds and post-process mapped onto the place: the sky clock's hour and heading for a
// level sun, what Place Settings sets winning, and what each actor is reported as. Actors are spawned into a transient
// editor world, as the coverage tests do.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleEnvironmentExportTestPrivate
{
	using EOutcome = EMantleCoverageOutcome;

	/** An empty editor world for one test, destroyed with it. */
	struct FTestWorld
	{
		UWorld* World = nullptr;

		FTestWorld()
		{
			World = UWorld::CreateWorld(EWorldType::Editor, /*bInformEngineOfWorld*/ false, TEXT("MantleEnvironmentExportTestWorld"));
		}

		~FTestWorld()
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

		template <typename T>
		T* Spawn(const TCHAR* Label, const FRotator& Rotation = FRotator::ZeroRotator)
		{
			T* Actor = World ? World->SpawnActor<T>(FVector::ZeroVector, Rotation) : nullptr;
			if (Actor)
			{
				Actor->SetActorLabel(Label);
				// A light's component carries its own default pitch (-46), which a spawn rotation is composed with
				// rather than replacing, so the rotation is set outright to be what the test says.
				if (!Rotation.IsZero())
				{
					Actor->SetActorRotation(Rotation);
				}
			}
			return Actor;
		}
	};

	/** A level with a sun the way an export sees it: the actor, and the place's entity for it. */
	struct FSunLevel
	{
		ADirectionalLight* Sun = nullptr;
		FMantlePlace Place;
		TMap<const AActor*, FString> IdByActor;

		bool Build(FTestWorld& Test, const FRotator& Rotation, const bool bTemperature)
		{
			Sun = Test.Spawn<ADirectionalLight>(TEXT("Sun"), Rotation);
			UDirectionalLightComponent* Light = Sun ? Cast<UDirectionalLightComponent>(Sun->GetLightComponent()) : nullptr;
			if (!Light)
			{
				return false;
			}
			Light->Intensity = 8.0f;
			Light->bUseTemperature = bTemperature;
			Light->Temperature = 4000.0f;

			FMantleEntity Entity;
			Entity.Id = TEXT("ent_sun");
			Entity.Type = EMantleEntityType::DirectionalLight;
			Entity.Name = TEXT("Sun");
			Entity.Transform = Sun->GetActorTransform();
			FMantleDirectionalLightProps Props;
			Props.Intensity = 8.0;
			Entity.Props.Set<FMantleDirectionalLightProps>(Props);
			Place.Entities.Add(MoveTemp(Entity));
			IdByActor.Add(Sun, TEXT("ent_sun"));
			return true;
		}
	};

	/** Every detail the bucket has, joined. */
	FString DetailsOf(const FMantleCoverage& Coverage, const TCHAR* Category, const EOutcome Outcome)
	{
		FString Out;
		if (const FMantleCoverageCategory* Found = Coverage.Find(Category))
		{
			for (const FMantleCoverageExample& Example : Found->Get(Outcome).Examples)
			{
				Out += Example.Name + TEXT(": ") + Example.Detail + TEXT("\n");
			}
		}
		return Out;
	}

	int64 CountOf(const FMantleCoverage& Coverage, const TCHAR* Category, const EOutcome Outcome)
	{
		return Coverage.Count(Category, Outcome);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentSunClockTest,
	"Mantle.Editor.EnvironmentExport.SunClock",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentSunClockTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		double Pitch;
		double Yaw;
	};
	// The demo level's sun, the same pitch turned each way round, a low sun and a high one.
	const FCase Cases[] = { { -46.0, 0.0 }, { -46.0, 135.0 }, { -46.0, -90.0 }, { -20.0, -70.0 }, { -55.0, 10.0 }, { -85.0, 40.0 } };
	for (const FCase& Case : Cases)
	{
		const FString Name = FString::Printf(TEXT("pitch %.0f yaw %.0f"), Case.Pitch, Case.Yaw);
		const FVector TowardSun = -FRotator(Case.Pitch, Case.Yaw, 0.0).Vector();
		FMantleSunClock Clock;
		if (!TestTrue(*(Name + TEXT(": a sun above the horizon has a clock")), FMantleEnvironmentExport::SolveSunClock(TowardSun, Case.Yaw, Clock)))
		{
			continue;
		}
		const double Elevation = -Case.Pitch;
		const double Peak = MantleEnvironmentDefaults::PeakElevationDegrees;
		TestTrue(*(Name + TEXT(": clamped only above the clock's peak")), Clock.bElevationClamped == (Elevation > Peak));
		TestNearlyEqual(*(Name + TEXT(": the clock's sun is at the level sun's elevation")),
			FMantleSkyClock::SunElevationDegrees(Clock.Hour), FMath::Min(Elevation, Peak), 0.05);
		TestTrue(*(Name + TEXT(": by day")), Clock.Hour > 6.0 && Clock.Hour < 18.0);
		TestTrue(*(Name + TEXT(": the heading is a yaw")), Clock.HeadingYaw > -180.0 - UE_KINDA_SMALL_NUMBER && Clock.HeadingYaw <= 180.0 + UE_KINDA_SMALL_NUMBER);
		if (!Clock.bElevationClamped)
		{
			// With the heading the clock's sun is in the level sun's direction exactly (the hour is rounded to a thousandth).
			const FVector Placed = FMantleSkyClock::SunDirection(Clock.Hour, Clock.HeadingYaw);
			TestTrue(*(Name + TEXT(": and on its side of the sky")), FVector::Distance(Placed, TowardSun) < 0.001);
		}
	}

	FMantleSunClock Clock;
	TestFalse(TEXT("a light shining upward has no day clock"), FMantleEnvironmentExport::SolveSunClock(-FRotator(15.0, 0.0, 0.0).Vector(), 0.0, Clock));
	TestFalse(TEXT("nor one at the horizon"), FMantleEnvironmentExport::SolveSunClock(-FRotator(-0.5, 0.0, 0.0).Vector(), 0.0, Clock));
	TestFalse(TEXT("nor a zero vector"), FMantleEnvironmentExport::SolveSunClock(FVector::ZeroVector, 0.0, Clock));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentHelpersTest,
	"Mantle.Editor.EnvironmentExport.Helpers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentHelpersTest::RunTest(const FString& Parameters)
{
	// The material's own coverage is the sky every place had: it leaves the natural weather alone.
	TestTrue(TEXT("the default cloud coverage is partly cloudy"), FMantleEnvironmentExport::WeatherForCloudCoverage(-0.2) == EMantleWeatherState::PartlyCloudy);
	TestTrue(TEXT("a thin layer is clear"), FMantleEnvironmentExport::WeatherForCloudCoverage(-0.7) == EMantleWeatherState::Clear);
	TestTrue(TEXT("a thick one is overcast"), FMantleEnvironmentExport::WeatherForCloudCoverage(0.4) == EMantleWeatherState::Overcast);
	TestTrue(TEXT("past the range is overcast"), FMantleEnvironmentExport::WeatherForCloudCoverage(5.0) == EMantleWeatherState::Overcast);

	TestEqual(TEXT("the neutral white balance changes nothing"), FMantleEnvironmentExport::TintForWhiteBalance(6500.0), 0xFFFFFFu);
	const uint32 Warm = FMantleEnvironmentExport::TintForWhiteBalance(9000.0);
	const uint32 Cool = FMantleEnvironmentExport::TintForWhiteBalance(4000.0);
	TestNotEqual(TEXT("a warm white balance tints"), Warm, 0xFFFFFFu);
	TestTrue(TEXT("a higher temperature warms the picture: red over blue"), ((Warm >> 16) & 0xFF) > (Warm & 0xFF));
	TestTrue(TEXT("a lower one cools it: blue over red"), (Cool & 0xFF) > ((Cool >> 16) & 0xFF));

	FPostProcessSettings Settings;
	TestEqual(TEXT("a fresh settings struct overrides nothing"), FMantleEnvironmentExport::OverriddenSettings(Settings).Num(), 0);
	Settings.bOverride_ColorSaturation = true;
	Settings.bOverride_WhiteTemp = true;
	const TArray<FString> Overridden = FMantleEnvironmentExport::OverriddenSettings(Settings);
	TestEqual(TEXT("two overrides are found"), Overridden.Num(), 2);
	TestTrue(TEXT("by their property names"), Overridden.Contains(TEXT("ColorSaturation")) && Overridden.Contains(TEXT("WhiteTemp")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentTunedPropertiesTest,
	"Mantle.Editor.EnvironmentExport.TunedProperties",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentTunedPropertiesTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentExportTestPrivate;
	FTestWorld Test;
	if (!TestNotNull(TEXT("a test world"), Test.World))
	{
		return false;
	}
	AExponentialHeightFog* Fog = Test.Spawn<AExponentialHeightFog>(TEXT("Mist"));
	ASkyLight* SkyLight = Test.Spawn<ASkyLight>(TEXT("Ambient"));
	if (!TestNotNull(TEXT("a fog"), Fog) || !TestNotNull(TEXT("a sky light"), SkyLight))
	{
		return false;
	}
	TestEqual(TEXT("a fog at its defaults has nothing tuned"), FMantleEnvironmentExport::TunedProperties(Fog->GetComponent()).Num(), 0);
	TestEqual(TEXT("nor a sky light"), FMantleEnvironmentExport::TunedProperties(SkyLight->GetLightComponent()).Num(), 0);

	Fog->GetComponent()->FogDensity = 0.5f;
	const TArray<FString> Tuned = FMantleEnvironmentExport::TunedProperties(Fog->GetComponent());
	TestEqual(TEXT("a changed fog density is the one thing tuned"), Tuned.Num(), 1);
	TestTrue(TEXT("named as the editor does"), Tuned.Num() == 1 && Tuned[0].Contains(TEXT("Density")));

	SkyLight->GetLightComponent()->bRealTimeCapture = true;
	TestEqual(TEXT("a property named in the ignore list is not counted"),
		FMantleEnvironmentExport::TunedProperties(SkyLight->GetLightComponent(), { FName(TEXT("bRealTimeCapture")) }).Num(), 0);
	SkyLight->GetLightComponent()->Intensity = 0.25f;
	TestEqual(TEXT("a changed sky light intensity is tuned"),
		FMantleEnvironmentExport::TunedProperties(SkyLight->GetLightComponent(), { FName(TEXT("bRealTimeCapture")) }).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentMapsTheLevelTest,
	"Mantle.Editor.EnvironmentExport.MapsTheLevel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentMapsTheLevelTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentExportTestPrivate;
	FTestWorld Test;
	if (!TestNotNull(TEXT("a test world"), Test.World))
	{
		return false;
	}
	FSunLevel Level;
	if (!TestTrue(TEXT("a sun"), Level.Build(Test, FRotator(-46.0, 30.0, 0.0), /*bTemperature*/ true)))
	{
		return false;
	}
	APostProcessVolume* Grade = Test.Spawn<APostProcessVolume>(TEXT("Grade"));
	ASkyAtmosphere* Atmosphere = Test.Spawn<ASkyAtmosphere>(TEXT("Atmosphere"));
	if (!TestNotNull(TEXT("a post-process volume"), Grade) || !TestNotNull(TEXT("a sky atmosphere"), Atmosphere))
	{
		return false;
	}
	Grade->bUnbound = true;
	Grade->Settings.bOverride_ColorSaturation = true;
	Grade->Settings.ColorSaturation = FVector4(1.4, 1.4, 1.4, 1.0);
	Grade->Settings.bOverride_BloomIntensity = true;
	Grade->Settings.BloomIntensity = 2.0f;
	Grade->Settings.bOverride_WhiteTemp = true;
	Grade->Settings.WhiteTemp = 8000.0f;
	// A compensation 1.5 stops over the project's own, whatever that is: the place's bias is relative to it.
	const FMantleExposureProject Project = FMantleExposureProject::Current();
	Grade->Settings.bOverride_AutoExposureBias = true;
	Grade->Settings.AutoExposureBias = static_cast<float>(Project.EngineBias + 1.5);

	FMantleCoverage Coverage;
	FMantleEnvironmentExport::Apply(Test.World, Level.IdByActor, Level.Place, Coverage);

	const FMantleEnvironmentSettings& Environment = Level.Place.Settings.Environment;
	const FMantleEntity& Sun = Level.Place.Entities[0];
	if (TestTrue(TEXT("the sun's angle is carried as the clock's hour"), Environment.ClockTime.IsSet()))
	{
		FMantleSunClock Clock;
		FMantleEnvironmentExport::SolveSunClock(-FRotator(-46.0, 30.0, 0.0).Vector(), 30.0, Clock);
		// A light whose yaw is the sun's azimuth is as near to the morning heading as to the afternoon one, for any yaw
		// (the headings differ from the yaw by the same amount each way), so the tie goes to the morning.
		TestTrue(TEXT("the morning one, on that tie"), Clock.Hour < 12.0 && *Environment.ClockTime < 12.0);
		TestNearlyEqual(TEXT("at the hour that puts the clock's sun there"), *Environment.ClockTime, Clock.Hour, 0.002);
		TestNearlyEqual(TEXT("with the heading turned in the entity's yaw"), Sun.Transform.Rotator().Yaw, Clock.HeadingYaw, 0.01);
	}
	TestTrue(TEXT("and the clock is stopped there"), Environment.TimeScale.IsSet() && *Environment.TimeScale == 0.0);
	if (const FMantleDirectionalLightProps* Props = Sun.Props.TryGet<FMantleDirectionalLightProps>())
	{
		TestTrue(TEXT("the light's 4000 K is baked into its colour: warm, red over blue"), Props->Color.R > Props->Color.B);
		TestNearlyEqual(TEXT("its intensity is untouched"), Props->Intensity, 8.0, 0.0001);
	}
	if (TestTrue(TEXT("the volume's saturation is carried"), Environment.Saturation.IsSet()))
	{
		TestNearlyEqual(TEXT("as it is"), *Environment.Saturation, 1.4, 0.001);
	}
	if (TestTrue(TEXT("and its bloom"), Environment.Bloom.IsSet()))
	{
		TestNearlyEqual(TEXT("as it is"), *Environment.Bloom, 2.0, 0.001);
	}
	TestTrue(TEXT("and its white balance as a colour tint"), Environment.ColorTint.IsSet() && *Environment.ColorTint != 0xFFFFFF);
	TestTrue(TEXT("a sky with no cloud holds the weather clear"), Environment.Weather.IsSet() && *Environment.Weather == EMantleWeatherState::Clear);

	TestEqual(TEXT("the sun is reported once, approximated"), CountOf(Coverage, MantleCoverageCategory::Lights, EOutcome::Approximated), static_cast<int64>(1));
	TestEqual(TEXT("and nothing of it dropped"), CountOf(Coverage, MantleCoverageCategory::Lights, EOutcome::Dropped), static_cast<int64>(0));
	TestEqual(TEXT("the volume is approximated: some of it was carried"), CountOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Approximated), static_cast<int64>(1));
	const FString Grading = DetailsOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Approximated);
	TestTrue(TEXT("saying what was carried"), Grading.Contains(TEXT("saturation")) && Grading.Contains(TEXT("bloom")) && Grading.Contains(TEXT("white balance")));
	TestTrue(TEXT("and the exposure"), Grading.Contains(TEXT("exposure (")) && Grading.Contains(TEXT("1.5 stops")));
	if (TestTrue(TEXT("the exposure is the place's"), Environment.Exposure.IsSet()))
	{
		TestNearlyEqual(TEXT("its bias is the volume's compensation less the project's own"), Environment.Exposure->Bias, 1.5, 0.001);
		TestTrue(TEXT("its mode is the project's metering, a volume that does not override it"),
			(Environment.Exposure->Mode == EMantleExposureMode::Auto) == Project.bAuto);
	}
	TestEqual(TEXT("the weather held for want of a cloud is its own approximated line"), CountOf(Coverage, MantleCoverageCategory::Sky, EOutcome::Approximated), static_cast<int64>(1));
	TestEqual(TEXT("and an atmosphere at its defaults is kept"), CountOf(Coverage, MantleCoverageCategory::Sky, EOutcome::Kept), static_cast<int64>(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentSettingsWinTest,
	"Mantle.Editor.EnvironmentExport.PlaceSettingsWin",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentSettingsWinTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentExportTestPrivate;
	FTestWorld Test;
	if (!TestNotNull(TEXT("a test world"), Test.World))
	{
		return false;
	}
	FSunLevel Level;
	if (!TestTrue(TEXT("a sun"), Level.Build(Test, FRotator(-46.0, 30.0, 0.0), /*bTemperature*/ false)))
	{
		return false;
	}
	APostProcessVolume* Grade = Test.Spawn<APostProcessVolume>(TEXT("Grade"));
	Test.Spawn<ASkyAtmosphere>(TEXT("Atmosphere"));
	if (!TestNotNull(TEXT("a post-process volume"), Grade))
	{
		return false;
	}
	Grade->Settings.bOverride_ColorSaturation = true;
	Grade->Settings.ColorSaturation = FVector4(1.4, 1.4, 1.4, 1.0);
	Grade->Settings.bOverride_BloomIntensity = true;
	Grade->Settings.BloomIntensity = 2.0f;

	// What Mantle Place Settings set, as AMantlePlaceSettingsActor::MakeEnvironmentSettings writes it.
	FMantleEnvironmentSettings& Environment = Level.Place.Settings.Environment;
	Environment.ClockTime = 21.0;
	Environment.Saturation = 0.5;
	Environment.Weather = EMantleWeatherState::Storm;

	FMantleCoverage Coverage;
	FMantleEnvironmentExport::Apply(Test.World, Level.IdByActor, Level.Place, Coverage);

	TestTrue(TEXT("the settings' clock wins"), Environment.ClockTime.IsSet() && *Environment.ClockTime == 21.0);
	TestFalse(TEXT("and the level's sun does not stop it"), Environment.TimeScale.IsSet());
	TestTrue(TEXT("the settings' saturation wins"), Environment.Saturation.IsSet() && *Environment.Saturation == 0.5);
	TestTrue(TEXT("the settings' weather wins over a sky with no cloud"), Environment.Weather.IsSet() && *Environment.Weather == EMantleWeatherState::Storm);
	if (TestTrue(TEXT("what the settings left alone is carried: the bloom"), Environment.Bloom.IsSet()))
	{
		TestNearlyEqual(TEXT("as it is"), *Environment.Bloom, 2.0, 0.001);
	}
	TestTrue(TEXT("the report says the settings' value won"), DetailsOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Approximated).Contains(TEXT("Mantle Place Settings")));
	TestTrue(TEXT("and about the sun's angle"), DetailsOf(Coverage, MantleCoverageCategory::Lights, EOutcome::Approximated).Contains(TEXT("Mantle Place Settings")));
	TestEqual(TEXT("the held weather is dropped, not written"), CountOf(Coverage, MantleCoverageCategory::Sky, EOutcome::Dropped), static_cast<int64>(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentOlderClientTest,
	"Mantle.Editor.EnvironmentExport.ClientThatCannotReadIt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentOlderClientTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentExportTestPrivate;
	FTestWorld Test;
	if (!TestNotNull(TEXT("a test world"), Test.World))
	{
		return false;
	}
	FSunLevel Level;
	if (!TestTrue(TEXT("a sun"), Level.Build(Test, FRotator(-46.0, 30.0, 0.0), /*bTemperature*/ true)))
	{
		return false;
	}
	APostProcessVolume* Grade = Test.Spawn<APostProcessVolume>(TEXT("Grade"));
	if (!TestNotNull(TEXT("a post-process volume"), Grade))
	{
		return false;
	}
	Grade->Settings.bOverride_ColorSaturation = true;
	Grade->Settings.ColorSaturation = FVector4(1.4, 1.4, 1.4, 1.0);

	// A client from before settings.environment refuses the key, so none is written for it.
	const FMantleTargetFeaturesScope Scope(FMantleTargetFeatures::None());
	FMantleCoverage Coverage;
	FMantleEnvironmentExport::Apply(Test.World, Level.IdByActor, Level.Place, Coverage);

	TestTrue(TEXT("the place has no environment block"), Level.Place.Settings.Environment.IsEmpty());
	if (const FMantleDirectionalLightProps* Props = Level.Place.Entities[0].Props.TryGet<FMantleDirectionalLightProps>())
	{
		TestTrue(TEXT("but the sun's temperature is still in its colour, which every client reads"), Props->Color.R > Props->Color.B);
	}
	TestTrue(TEXT("the sun's angle says it needs a newer client"),
		DetailsOf(Coverage, MantleCoverageCategory::Lights, EOutcome::Approximated).Contains(FMantleTargetFeatures::NeedsNewerClient));
	TestEqual(TEXT("the volume has nothing carried, so it is dropped"), CountOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Dropped), static_cast<int64>(1));
	TestTrue(TEXT("saying why"), DetailsOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Dropped).Contains(FMantleTargetFeatures::NeedsNewerClient));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentReportsActorsTest,
	"Mantle.Editor.EnvironmentExport.ReportsEachActor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentReportsActorsTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentExportTestPrivate;
	FTestWorld Test;
	if (!TestNotNull(TEXT("a test world"), Test.World))
	{
		return false;
	}
	AExponentialHeightFog* Plain = Test.Spawn<AExponentialHeightFog>(TEXT("PlainFog"));
	AExponentialHeightFog* Thick = Test.Spawn<AExponentialHeightFog>(TEXT("ThickFog"));
	ASkyLight* Bright = Test.Spawn<ASkyLight>(TEXT("BrightSkyLight"));
	AVolumetricCloud* Cloud = Test.Spawn<AVolumetricCloud>(TEXT("Clouds"));
	APostProcessVolume* Empty = Test.Spawn<APostProcessVolume>(TEXT("EmptyGrade"));
	if (!TestNotNull(TEXT("the fogs"), Plain) || !TestNotNull(TEXT("the sky light"), Bright) || !TestNotNull(TEXT("the cloud"), Cloud)
		|| !TestNotNull(TEXT("the volume"), Empty) || !TestNotNull(TEXT("the thick fog"), Thick))
	{
		return false;
	}
	Thick->GetComponent()->FogDensity = 0.3f;
	Bright->GetLightComponent()->Intensity = 4.0f;

	TestTrue(TEXT("a fog is one the export reports"), FMantleEnvironmentExport::IsReported(Plain));
	TestTrue(TEXT("a cloud too"), FMantleEnvironmentExport::IsReported(Cloud));
	TestTrue(TEXT("and a volume"), FMantleEnvironmentExport::IsReported(Empty));
	TestFalse(TEXT("a light is not"), FMantleEnvironmentExport::IsReported(Test.Spawn<ADirectionalLight>(TEXT("Lamp"))));

	FMantlePlace Place;
	TMap<const AActor*, FString> IdByActor;
	FMantleCoverage Coverage;
	FMantleEnvironmentExport::Apply(Test.World, IdByActor, Place, Coverage);

	TestEqual(TEXT("a fog at its defaults is kept"), CountOf(Coverage, MantleCoverageCategory::Fog, EOutcome::Kept), static_cast<int64>(1));
	TestEqual(TEXT("a tuned one is dropped"), CountOf(Coverage, MantleCoverageCategory::Fog, EOutcome::Dropped), static_cast<int64>(1));
	TestTrue(TEXT("naming what was tuned"), DetailsOf(Coverage, MantleCoverageCategory::Fog, EOutcome::Dropped).Contains(TEXT("Density")));
	TestEqual(TEXT("a sky light the creator tuned is dropped"), CountOf(Coverage, MantleCoverageCategory::Sky, EOutcome::Dropped), static_cast<int64>(1));
	TestEqual(TEXT("a cloud with the default coverage is approximated: its coverage is carried as a held weather"), CountOf(Coverage, MantleCoverageCategory::Sky, EOutcome::Approximated), static_cast<int64>(1));
	TestTrue(TEXT("held at partly cloudy, never left to change on its own"),
		Place.Settings.Environment.Weather.IsSet() && *Place.Settings.Environment.Weather == EMantleWeatherState::PartlyCloudy);
	TestTrue(TEXT("and the report says how to choose another"), DetailsOf(Coverage, MantleCoverageCategory::Sky, EOutcome::Approximated).Contains(TEXT("Mantle Place Settings")));
	// A project whose default is an auto exposure shows the level that way, so even a volume that overrides nothing carries it.
	const bool bAutoProject = FMantleExposureProject::Current().bAuto;
	TestEqual(TEXT("a volume that overrides nothing changes nothing, in a project with fixed exposure"),
		CountOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Info), static_cast<int64>(bAutoProject ? 0 : 1));
	TestEqual(TEXT("and carries the default auto exposure in one with auto"),
		CountOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Approximated), static_cast<int64>(bAutoProject ? 1 : 0));
	TestEqual(TEXT("the exposure is written exactly then"), Place.Settings.Environment.Exposure.IsSet(), bAutoProject);
	TestFalse(TEXT("and it writes no saturation, bloom or tint"),
		Place.Settings.Environment.Saturation.IsSet() || Place.Settings.Environment.Bloom.IsSet() || Place.Settings.Environment.ColorTint.IsSet());
	TestFalse(TEXT("and no fog: the first fog is at Mantle's own"), Place.Settings.Environment.Fog.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentExportTest,
	"Mantle.Editor.EnvironmentExport.ExportWritesIt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentExportTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentExportTestPrivate;
	FTestWorld Test;
	if (!TestNotNull(TEXT("a test world"), Test.World))
	{
		return false;
	}
	ADirectionalLight* Sun = Test.Spawn<ADirectionalLight>(TEXT("Sun"), FRotator(-46.0, 30.0, 0.0));
	APostProcessVolume* Grade = Test.Spawn<APostProcessVolume>(TEXT("Grade"));
	AExponentialHeightFog* Mist = Test.Spawn<AExponentialHeightFog>(TEXT("Mist"));
	if (!TestNotNull(TEXT("the sun"), Sun) || !TestNotNull(TEXT("the volume"), Grade) || !TestNotNull(TEXT("the fog"), Mist))
	{
		return false;
	}
	Grade->Settings.bOverride_ColorSaturation = true;
	Grade->Settings.ColorSaturation = FVector4(1.3, 1.3, 1.3, 1.0);
	Mist->GetComponent()->FogDensity = 0.2f;

	const FMantleExportResult Exported = FMantlePlaceExporter::Export(Test.World, FMantleExportOptions());
	const FMantleCoverage& Coverage = Exported.Coverage;
	TestEqual(TEXT("the sun is one approximated line, not two"), CountOf(Coverage, MantleCoverageCategory::Lights, EOutcome::Approximated), static_cast<int64>(1));
	TestEqual(TEXT("and none kept"), CountOf(Coverage, MantleCoverageCategory::Lights, EOutcome::Kept), static_cast<int64>(0));
	TestEqual(TEXT("the volume is one line, approximated"), CountOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Approximated), static_cast<int64>(1));
	TestEqual(TEXT("and not also dropped by the survey"), CountOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Dropped), static_cast<int64>(0));
	TestEqual(TEXT("the fog is one approximated line: its density is carried"), CountOf(Coverage, MantleCoverageCategory::Fog, EOutcome::Approximated), static_cast<int64>(1));
	TestEqual(TEXT("and not dropped"), CountOf(Coverage, MantleCoverageCategory::Fog, EOutcome::Dropped), static_cast<int64>(0));
	TestTrue(TEXT("the density is the place's largest, 0.2 being past it"),
		Exported.Place.Settings.Environment.Fog.IsSet() && Exported.Place.Settings.Environment.Fog->Density.IsSet()
		&& *Exported.Place.Settings.Environment.Fog->Density == MantleEnvironmentDefaults::MaxFogDensity);
	TestTrue(TEXT("and the report says it was clamped"), DetailsOf(Coverage, MantleCoverageCategory::Fog, EOutcome::Approximated).Contains(TEXT("past the place's largest")));
	TestTrue(TEXT("a level with a sun and no cloud holds the weather clear"),
		Exported.Place.Settings.Environment.Weather.IsSet() && *Exported.Place.Settings.Environment.Weather == EMantleWeatherState::Clear);
	TestTrue(TEXT("the place says when the clock starts and what saturation is"), Exported.Place.Settings.Environment.ClockTime.IsSet() && Exported.Place.Settings.Environment.Saturation.IsSet());

	const FString Json = FMantlePlaceExporter::ToJson(Exported.Place);
	TestTrue(TEXT("and writes them to the place file"), Json.Contains(TEXT("\"clockTime\"")) && Json.Contains(TEXT("\"timeScale\"")) && Json.Contains(TEXT("\"saturation\"")));
	TestTrue(TEXT("with the fog's density"), Json.Contains(TEXT("\"fog\"")) && Json.Contains(TEXT("\"density\"")));
	TestEqual(TEXT("and the exposure when the project's is auto"), Json.Contains(TEXT("\"exposure\"")), FMantleExposureProject::Current().bAuto);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentExposureForTest,
	"Mantle.Editor.EnvironmentExport.ExposureFor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentExposureForTest::RunTest(const FString& Parameters)
{
	// An auto-exposure project with the engine's own compensation of 1, and its legacy luminance range.
	FMantleExposureProject Auto;
	Auto.bAuto = true;
	Auto.bExtendedRange = false;
	Auto.EngineBias = 1.0;
	FMantleExposureProject Fixed = Auto;
	Fixed.bAuto = false;

	FMantleExposureSettings Out;
	TArray<FString> Carried;
	TArray<FString> Notes;

	// No volume: the project's default.
	TestTrue(TEXT("no volume in an auto project is an auto exposure"), FMantleEnvironmentExport::ExposureFor(nullptr, Auto, Out, Carried, Notes));
	TestTrue(TEXT("auto"), Out.Mode == EMantleExposureMode::Auto);
	TestEqual(TEXT("with no bias"), Out.Bias, 0.0);
	TestFalse(TEXT("and no range"), Out.MinBrightness.IsSet() || Out.MaxBrightness.IsSet());
	Carried.Reset();
	TestFalse(TEXT("none in a fixed project is the game's own: nothing to write"), FMantleEnvironmentExport::ExposureFor(nullptr, Fixed, Out, Carried, Notes));
	TestEqual(TEXT("and nothing carried"), Carried.Num(), 0);

	// The demo level's volume: a compensation of 0 and a minimum brightness of 0.5, nothing else about the metering.
	FPostProcessSettings Demo;
	Demo.bOverride_AutoExposureBias = true;
	Demo.AutoExposureBias = 0.0f;
	Demo.bOverride_AutoExposureMinBrightness = true;
	Demo.AutoExposureMinBrightness = 0.5f;
	Carried.Reset();
	TestTrue(TEXT("the demo's volume is carried"), FMantleEnvironmentExport::ExposureFor(&Demo, Auto, Out, Carried, Notes));
	TestTrue(TEXT("as auto, the project's default"), Out.Mode == EMantleExposureMode::Auto);
	TestEqual(TEXT("with a bias of 0 less the project's 1"), Out.Bias, -1.0);
	TestTrue(TEXT("its minimum brightness"), Out.MinBrightness.IsSet() && *Out.MinBrightness == 0.5);
	TestFalse(TEXT("and no maximum: it did not override one"), Out.MaxBrightness.IsSet());
	TestTrue(TEXT("saying so"), Carried.Num() == 1 && Carried[0].Contains(TEXT("auto")) && Carried[0].Contains(TEXT("bias -1")) && Carried[0].Contains(TEXT("min brightness 0.5")));

	// In a fixed project the same volume is a fixed exposure with a bias: the range is read only with auto.
	Carried.Reset();
	TestTrue(TEXT("in a fixed project the bias is still carried"), FMantleEnvironmentExport::ExposureFor(&Demo, Fixed, Out, Carried, Notes));
	TestTrue(TEXT("as fixed"), Out.Mode == EMantleExposureMode::Fixed);
	TestFalse(TEXT("with no range"), Out.MinBrightness.IsSet());

	// An explicit method: histogram is auto even in a fixed project, manual is fixed even in an auto one.
	FPostProcessSettings Metered;
	Metered.bOverride_AutoExposureMethod = true;
	Metered.AutoExposureMethod = EAutoExposureMethod::AEM_Histogram;
	Carried.Reset();
	TestTrue(TEXT("histogram metering in a fixed project is auto"), FMantleEnvironmentExport::ExposureFor(&Metered, Fixed, Out, Carried, Notes));
	TestTrue(TEXT("auto"), Out.Mode == EMantleExposureMode::Auto);
	Metered.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
	Carried.Reset();
	Notes.Reset();
	// Manual metering exposes by the camera: at the default f/4, 1/60 s and ISO 100 (EV100 9.9) the picture is about
	// 7.4 stops darker than the game's fixed exposure, so it is carried as fixed at the darkest bias, with a note.
	TestTrue(TEXT("manual metering is carried"), FMantleEnvironmentExport::ExposureFor(&Metered, Auto, Out, Carried, Notes));
	TestTrue(TEXT("as fixed"), Out.Mode == EMantleExposureMode::Fixed);
	TestEqual(TEXT("with the camera folded into the bias (clamped)"), Out.Bias, -MantleEnvironmentDefaults::MaxExposureBias);
	TestTrue(TEXT("saying so"), Carried.Num() == 1 && Carried[0].Contains(TEXT("EV100 9.9")));
	TestTrue(TEXT("and that it was clamped"), Notes.Num() == 1 && Notes[0].Contains(TEXT("clamped")));
	// Without the physical camera, the EV100 is 0 and only the white-point calibration remains: log2(1 / 0.18), 2.47 stops.
	Metered.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	Metered.AutoExposureApplyPhysicalCameraExposure = 0;
	Carried.Reset();
	Notes.Reset();
	TestTrue(TEXT("manual metering without the camera is carried"), FMantleEnvironmentExport::ExposureFor(&Metered, Auto, Out, Carried, Notes));
	TestEqual(TEXT("at the white-point calibration"), Out.Bias, FMath::Log2(1.0 / 0.18), 0.01);
	Metered.bOverride_AutoExposureApplyPhysicalCameraExposure = false;
	Metered.AutoExposureMethod = EAutoExposureMethod::AEM_Basic;
	Carried.Reset();
	TestTrue(TEXT("basic metering is auto"), FMantleEnvironmentExport::ExposureFor(&Metered, Auto, Out, Carried, Notes));
	TestTrue(TEXT("saying it was carried as histogram"), Carried.Num() == 1 && Carried[0].Contains(TEXT("histogram")));

	// A compensation past the place's range is clamped, and says so.
	FPostProcessSettings Bright;
	Bright.bOverride_AutoExposureBias = true;
	Bright.AutoExposureBias = 9.0f;
	Carried.Reset();
	Notes.Reset();
	TestTrue(TEXT("a very bright volume is carried"), FMantleEnvironmentExport::ExposureFor(&Bright, Fixed, Out, Carried, Notes));
	TestEqual(TEXT("at the largest bias"), Out.Bias, MantleEnvironmentDefaults::MaxExposureBias);
	TestTrue(TEXT("with a note"), Notes.Num() == 1 && Notes[0].Contains(TEXT("clamped")));

	// A range the wrong way round, and one in EV100.
	FPostProcessSettings Backwards;
	Backwards.bOverride_AutoExposureMinBrightness = true;
	Backwards.AutoExposureMinBrightness = 6.0f;
	Backwards.bOverride_AutoExposureMaxBrightness = true;
	Backwards.AutoExposureMaxBrightness = 2.0f;
	Carried.Reset();
	Notes.Reset();
	TestTrue(TEXT("a backwards range is carried"), FMantleEnvironmentExport::ExposureFor(&Backwards, Auto, Out, Carried, Notes));
	TestTrue(TEXT("with its maximum raised to its minimum"), Out.MinBrightness.IsSet() && Out.MaxBrightness.IsSet() && *Out.MaxBrightness == *Out.MinBrightness);
	FMantleExposureProject Extended = Auto;
	Extended.bExtendedRange = true;
	Carried.Reset();
	Notes.Reset();
	TestTrue(TEXT("a project counting in EV100 is still auto"), FMantleEnvironmentExport::ExposureFor(&Backwards, Extended, Out, Carried, Notes));
	TestFalse(TEXT("but the range, in other units, is not carried"), Out.MinBrightness.IsSet() || Out.MaxBrightness.IsSet());
	TestTrue(TEXT("and says so"), Notes.Num() == 1 && Notes[0].Contains(TEXT("EV100")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentFogMapsTest,
	"Mantle.Editor.EnvironmentExport.FogMapsTheLevel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentFogMapsTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentExportTestPrivate;
	FTestWorld Test;
	if (!TestNotNull(TEXT("a test world"), Test.World))
	{
		return false;
	}
	// The demo level's fog: thin, with a blue inscattering and a warm glow toward the sun; the falloff and start are the defaults.
	AExponentialHeightFog* Mist = Test.Spawn<AExponentialHeightFog>(TEXT("Mist"));
	if (!TestNotNull(TEXT("a fog"), Mist))
	{
		return false;
	}
	UExponentialHeightFogComponent* Fog = Mist->GetComponent();
	Fog->FogDensity = 0.005f;
	Fog->FogInscatteringLuminance = FLinearColor(0.447f, 0.638f, 1.0f);
	Fog->DirectionalInscatteringLuminance = FLinearColor(0.25f, 0.25f, 0.125f);
	Fog->FogMaxOpacity = 0.9f;

	FMantlePlace Place;
	TMap<const AActor*, FString> IdByActor;
	FMantleCoverage Coverage;
	FMantleEnvironmentExport::Apply(Test.World, IdByActor, Place, Coverage);

	const FMantleEnvironmentSettings& Environment = Place.Settings.Environment;
	if (!TestTrue(TEXT("the fog is the place's"), Environment.Fog.IsSet()))
	{
		return false;
	}
	const FMantleFogSettings& Mapped = *Environment.Fog;
	TestTrue(TEXT("its density"), Mapped.Density.IsSet() && FMath::IsNearlyEqual(*Mapped.Density, 0.005, 1.0e-7));
	TestFalse(TEXT("its height falloff is Mantle's own, so unwritten"), Mapped.HeightFalloff.IsSet());
	TestFalse(TEXT("and its start distance"), Mapped.StartDistance.IsSet());
	if (TestTrue(TEXT("its inscattering colour"), Mapped.InscatteringColor.IsSet()))
	{
		TestNearlyEqual(TEXT("blue"), Mapped.InscatteringColor->B, 1.0f, 1.0e-4f);
		TestNearlyEqual(TEXT("green, rounded"), Mapped.InscatteringColor->G, 0.638f, 1.0e-4f);
	}
	TestTrue(TEXT("its directional colour"), Mapped.DirectionalInscatteringColor.IsSet() && FMath::IsNearlyEqual(Mapped.DirectionalInscatteringColor->B, 0.125f, 1.0e-4f));
	TestEqual(TEXT("the fog is approximated"), CountOf(Coverage, MantleCoverageCategory::Fog, EOutcome::Approximated), static_cast<int64>(1));
	const FString Report = DetailsOf(Coverage, MantleCoverageCategory::Fog, EOutcome::Approximated);
	TestTrue(TEXT("saying what was carried"), Report.Contains(TEXT("density")) && Report.Contains(TEXT("inscattering colour")) && Report.Contains(TEXT("directional")));
	TestTrue(TEXT("and what was not, by name"), Report.Contains(TEXT("not carried")) && Report.Contains(TEXT("Max Opacity")));
	TestFalse(TEXT("what was carried is not listed as left out"), Report.Contains(TEXT("not carried: Fog Density")));

	// A client that cannot read the key gets none of it, and the report says why.
	FMantlePlace Older;
	FMantleCoverage OlderCoverage;
	{
		const FMantleTargetFeaturesScope Scope(FMantleTargetFeatures::None());
		FMantleEnvironmentExport::Apply(Test.World, IdByActor, Older, OlderCoverage);
	}
	TestFalse(TEXT("an older client's place has no fog"), Older.Settings.Environment.Fog.IsSet());
	TestEqual(TEXT("the fog is dropped"), CountOf(OlderCoverage, MantleCoverageCategory::Fog, EOutcome::Dropped), static_cast<int64>(1));
	TestTrue(TEXT("saying it needs a newer client"), DetailsOf(OlderCoverage, MantleCoverageCategory::Fog, EOutcome::Dropped).Contains(FMantleTargetFeatures::NeedsNewerClient));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentApplyFogTest,
	"Mantle.Editor.EnvironmentExport.FogAppliesToTheSpawnedFog",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentApplyFogTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentExportTestPrivate;
	FTestWorld Test;
	if (!TestNotNull(TEXT("a test world"), Test.World))
	{
		return false;
	}
	AExponentialHeightFog* Spawned = Test.Spawn<AExponentialHeightFog>(TEXT("MantleFog"));
	if (!TestNotNull(TEXT("a fog"), Spawned))
	{
		return false;
	}
	// Movable, as the environment spawns it: a static component refuses a change of its values.
	Spawned->GetRootComponent()->SetMobility(EComponentMobility::Movable);
	const UExponentialHeightFogComponent* Fog = Spawned->GetComponent();
	const float OwnFalloff = Fog->FogHeightFalloff;

	// Only the keys the place sets change: the others keep the fog's own values.
	FMantleFogSettings Settings;
	Settings.Density = 0.005;
	Settings.InscatteringColor = FLinearColor(0.447f, 0.638f, 1.0f);
	FMantleEnvironment::ApplyFogSettings(Spawned, Settings);
	TestNearlyEqual(TEXT("the density"), Fog->FogDensity, 0.005f, 1.0e-6f);
	TestNearlyEqual(TEXT("the colour"), Fog->FogInscatteringLuminance.G, 0.638f, 1.0e-4f);
	TestEqual(TEXT("the falloff keeps the fog's own"), Fog->FogHeightFalloff, OwnFalloff);
	TestEqual(TEXT("and the start distance"), Fog->StartDistance, 0.0f);

	FMantleFogSettings More;
	More.HeightFalloff = 0.7;
	More.StartDistance = 5000.0;
	More.DirectionalInscatteringColor = FLinearColor(0.25f, 0.25f, 0.125f);
	FMantleEnvironment::ApplyFogSettings(Spawned, More);
	TestNearlyEqual(TEXT("the falloff"), Fog->FogHeightFalloff, 0.7f, 1.0e-6f);
	TestEqual(TEXT("the start distance"), Fog->StartDistance, 5000.0f);
	TestNearlyEqual(TEXT("the directional colour"), Fog->DirectionalInscatteringLuminance.B, 0.125f, 1.0e-4f);
	TestNearlyEqual(TEXT("the earlier density stays"), Fog->FogDensity, 0.005f, 1.0e-6f);

	FMantleEnvironment::ApplyFogSettings(nullptr, Settings);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceWeatherChoiceTest,
	"Mantle.Editor.EnvironmentExport.PlaceSettingsWeatherChoice",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceWeatherChoiceTest::RunTest(const FString& Parameters)
{
	AMantlePlaceSettingsActor* Settings = NewObject<AMantlePlaceSettingsActor>(GetTransientPackage());
	if (!TestNotNull(TEXT("a settings actor"), Settings))
	{
		return false;
	}
	TestTrue(TEXT("an untouched actor matches the level's sky"), Settings->Weather == EMantlePlaceWeather::LevelSky);
	TestFalse(TEXT("which leaves the weather unset"), Settings->MakeEnvironmentSettings().Weather.IsSet());
	Settings->Weather = EMantlePlaceWeather::Natural;
	TestFalse(TEXT("Natural leaves it unset too"), Settings->MakeEnvironmentSettings().Weather.IsSet());
	Settings->Weather = EMantlePlaceWeather::Clear;
	const FMantleEnvironmentSettings Chosen = Settings->MakeEnvironmentSettings();
	TestTrue(TEXT("Clear maps to the Clear weather state"), Chosen.Weather.IsSet() && *Chosen.Weather == EMantleWeatherState::Clear);
	Settings->SetEnvironmentFrom(FMantleEnvironmentSettings());
	TestTrue(TEXT("a place file with no weather imports as Natural"), Settings->Weather == EMantlePlaceWeather::Natural);
	Settings->SetEnvironmentFrom(Chosen);
	TestTrue(TEXT("and one with a weather imports as that weather"), Settings->Weather == EMantlePlaceWeather::Clear);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentNaturalWeatherExportTest,
	"Mantle.Editor.EnvironmentExport.NaturalWeatherExport",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentNaturalWeatherExportTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentExportTestPrivate;
	// #718: a level with a sky of its own holds its weather by default, and Natural in Place Settings lets it change on its own.
	FTestWorld Test;
	if (!TestNotNull(TEXT("a test world"), Test.World) || !TestNotNull(TEXT("a cloud"), Test.Spawn<AVolumetricCloud>(TEXT("Clouds"))))
	{
		return false;
	}
	TMap<const AActor*, FString> IdByActor;
	{
		FMantlePlace Place;
		FMantleCoverage Coverage;
		FMantleEnvironmentExport::Apply(Test.World, IdByActor, Place, Coverage);
		TestTrue(TEXT("by default the weather is held"), Place.Settings.Environment.Weather.IsSet());
	}
	AMantlePlaceSettingsActor* Settings = Test.Spawn<AMantlePlaceSettingsActor>(TEXT("Settings"));
	if (!TestNotNull(TEXT("place settings"), Settings))
	{
		return false;
	}
	Settings->Weather = EMantlePlaceWeather::Natural;
	FMantlePlace Place;
	FMantleCoverage Coverage;
	FMantleEnvironmentExport::Apply(Test.World, IdByActor, Place, Coverage);
	TestFalse(TEXT("with Natural no weather is written"), Place.Settings.Environment.Weather.IsSet());
	TestTrue(TEXT("and the report says Natural was chosen"), DetailsOf(Coverage, MantleCoverageCategory::Sky, EOutcome::Approximated).Contains(TEXT("chooses Natural")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentSkyLightScaleTest,
	"Mantle.Editor.EnvironmentExport.SkyLightScale",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentSkyLightScaleTest::RunTest(const FString& Parameters)
{
	// #722: the scale on Mantle's sky light is the level's sky light's brightness over Mantle's sky at the clock's hour.
	const double Noon = FMantleEnvironmentExport::MantleSkyMeanLuminance(12.0);
	TestNearlyEqual(TEXT("Mantle's sky at noon is the client's measured 0.0761"), Noon, 0.0761, 1.0e-9);
	const double Midnight = FMantleEnvironmentExport::MantleSkyMeanLuminance(0.0);
	const double Morning = FMantleEnvironmentExport::MantleSkyMeanLuminance(9.0);
	TestTrue(TEXT("Mantle's sky is brightest at noon"), Noon > Morning && Morning > Midnight);
	TestTrue(TEXT("and never dark, the moon and stars are left"), Midnight > 0.0);

	TestEqual(TEXT("a sky light as bright as Mantle's sky is Mantle's own"), FMantleEnvironmentExport::SkyLightScaleFor(Noon, 12.0), 1.0);
	TestEqual(TEXT("a brighter one is held at 1"), FMantleEnvironmentExport::SkyLightScaleFor(Noon * 10.0, 12.0), 1.0);
	TestNearlyEqual(TEXT("a dim one is its share of Mantle's sky"), FMantleEnvironmentExport::SkyLightScaleFor(Noon * 0.05, 12.0), 0.05, 1.0e-9);
	TestTrue(TEXT("the same room is a bigger share of a darker sky"),
		FMantleEnvironmentExport::SkyLightScaleFor(Midnight * 0.5, 0.0) > FMantleEnvironmentExport::SkyLightScaleFor(Midnight * 0.5, 12.0));
	TestEqual(TEXT("a black capture is no sky light"), FMantleEnvironmentExport::SkyLightScaleFor(0.0, 12.0), 0.0);
	TestEqual(TEXT("and a bad number is none"), FMantleEnvironmentExport::SkyLightScaleFor(std::numeric_limits<double>::quiet_NaN(), 12.0), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentCubemapMeanTest,
	"Mantle.Editor.EnvironmentExport.CubemapMean",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentCubemapMeanTest::RunTest(const FString& Parameters)
{
	// #722: a specified cubemap's brightness, read from its source data. Small generated cubemaps, 4 texels a side.
	constexpr int32 Size = 4;
	constexpr int32 Texels = Size * Size * 6;
	TestTrue(TEXT("no cubemap is unreadable"), FMantleEnvironmentExport::CubemapMeanLuminance(nullptr, false) < 0.0);

	// A float cubemap of one grey: its mean is that grey, whatever the weights.
	UTextureCube* Float = NewObject<UTextureCube>(GetTransientPackage());
	if (!TestNotNull(TEXT("a cubemap"), Float))
	{
		return false;
	}
	TArray<float> FloatTexels;
	for (int32 Texel = 0; Texel < Texels; ++Texel)
	{
		FloatTexels.Append({ 0.5f, 0.5f, 0.5f, 1.0f });
	}
	Float->Source.Init(Size, Size, 6, 1, TSF_RGBA32F, reinterpret_cast<const uint8*>(FloatTexels.GetData()));
	TestNearlyEqual(TEXT("a float cubemap of 0.5 has mean 0.5"), FMantleEnvironmentExport::CubemapMeanLuminance(Float, false), 0.5, 1.0e-4);
	TestNearlyEqual(TEXT("with a black lower hemisphere it has a quarter of it"), FMantleEnvironmentExport::CubemapMeanLuminance(Float, true), 0.25, 1.0e-3);

	// An LDR cubemap: sRGB 188 is about 0.5 linear; read as linear it is 188 / 255.
	UTextureCube* Ldr = NewObject<UTextureCube>(GetTransientPackage());
	if (!TestNotNull(TEXT("an LDR cubemap"), Ldr))
	{
		return false;
	}
	TArray<uint8> LdrTexels;
	for (int32 Texel = 0; Texel < Texels; ++Texel)
	{
		LdrTexels.Append({ 188, 188, 188, 255 });
	}
	Ldr->Source.Init(Size, Size, 6, 1, TSF_BGRA8, LdrTexels.GetData());
	Ldr->SRGB = true;
	TestNearlyEqual(TEXT("an sRGB cubemap is converted to linear"), FMantleEnvironmentExport::CubemapMeanLuminance(Ldr, false), 0.503, 0.01);
	Ldr->SRGB = false;
	TestNearlyEqual(TEXT("a linear one is not"), FMantleEnvironmentExport::CubemapMeanLuminance(Ldr, false), 188.0 / 255.0, 0.01);

	// Where it was read from is said.
	FString ReadFrom;
	FMantleEnvironmentExport::CubemapMeanLuminance(Float, false, &ReadFrom);
	TestTrue(TEXT("a six-face source says so"), ReadFrom.Contains(TEXT("six")));

	// No source data at all: unreadable.
	UTextureCube* Empty = NewObject<UTextureCube>(GetTransientPackage());
	TestTrue(TEXT("a cubemap with no source data is unreadable"), FMantleEnvironmentExport::CubemapMeanLuminance(Empty, false) < 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentCubemapLongLatTest,
	"Mantle.Editor.EnvironmentExport.CubemapMeanLongLat",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentCubemapLongLatTest::RunTest(const FString& Parameters)
{
	// #722: an imported HDR panorama is a cubemap whose source is one long-lat image. 8 by 4 texels: the rows' latitudes are +-67.5 and +-22.5.
	constexpr int32 Width = 8;
	constexpr int32 Height = 4;
	auto MakeLongLat = [](const TFunctionRef<float(int32)>& RowValue)
	{
		UTextureCube* Cube = NewObject<UTextureCube>(GetTransientPackage());
		TArray<float> Texels;
		for (int32 Row = 0; Row < Height; ++Row)
		{
			for (int32 Column = 0; Column < Width; ++Column)
			{
				Texels.Append({ RowValue(Row), RowValue(Row), RowValue(Row), 1.0f });
			}
		}
		Cube->Source.Init(Width, Height, 1, 1, TSF_RGBA32F, reinterpret_cast<const uint8*>(Texels.GetData()));
		return Cube;
	};

	UTextureCube* Flat = MakeLongLat([](int32) { return 0.5f; });
	FString ReadFrom;
	TestNearlyEqual(TEXT("a long-lat image of 0.5 has mean 0.5"), FMantleEnvironmentExport::CubemapMeanLuminance(Flat, false, &ReadFrom), 0.5, 1.0e-4);
	TestTrue(TEXT("and says it was a long-lat image"), ReadFrom.Contains(TEXT("long-lat")));

	// Rows are weighted by cos(latitude): a bright top row of 4 over dark rows is (4 cos 67.5) / (2 (cos 67.5 + cos 22.5)), not 1.
	UTextureCube* Top = MakeLongLat([](int32 Row) { return Row == 0 ? 4.0f : 0.0f; });
	const double NearPole = FMath::Cos(FMath::DegreesToRadians(67.5));
	const double NearHorizon = FMath::Cos(FMath::DegreesToRadians(22.5));
	TestNearlyEqual(TEXT("a bright top row counts by the solid angle it covers"), FMantleEnvironmentExport::CubemapMeanLuminance(Top, false),
		4.0 * NearPole / (2.0 * (NearPole + NearHorizon)), 1.0e-4);

	// A black lower hemisphere: the lower two rows count as black, the upper keep their weight.
	UTextureCube* Halves = MakeLongLat([](int32 Row) { return Row < 2 ? 1.0f : 0.2f; });
	TestNearlyEqual(TEXT("two halves of 1 and 0.2 average 0.6"), FMantleEnvironmentExport::CubemapMeanLuminance(Halves, false), 0.6, 1.0e-4);
	TestNearlyEqual(TEXT("with the lower half black it is 0.5"), FMantleEnvironmentExport::CubemapMeanLuminance(Halves, true), 0.5, 1.0e-4);

	// A half-float long-lat image, as an EXR or HDR import may keep it.
	UTextureCube* Half = NewObject<UTextureCube>(GetTransientPackage());
	TArray<FFloat16> HalfTexels;
	for (int32 Texel = 0; Texel < Width * Height; ++Texel)
	{
		HalfTexels.Append({ FFloat16(2.0f), FFloat16(2.0f), FFloat16(2.0f), FFloat16(1.0f) });
	}
	Half->Source.Init(Width, Height, 1, 1, TSF_RGBA16F, reinterpret_cast<const uint8*>(HalfTexels.GetData()));
	TestNearlyEqual(TEXT("a half-float long-lat image of 2 has mean 2"), FMantleEnvironmentExport::CubemapMeanLuminance(Half, false), 2.0, 1.0e-3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentSkyLightExportTest,
	"Mantle.Editor.EnvironmentExport.SkyLightReport",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentSkyLightExportTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentExportTestPrivate;
	FTestWorld Test;
	ASkyLight* SkyLight = Test.World ? Test.Spawn<ASkyLight>(TEXT("Ambient")) : nullptr;
	if (!TestNotNull(TEXT("a sky light"), SkyLight))
	{
		return false;
	}
	TMap<const AActor*, FString> IdByActor;
	{
		FMantlePlace Place;
		FMantleCoverage Coverage;
		FMantleEnvironmentExport::Apply(Test.World, IdByActor, Place, Coverage);
		TestFalse(TEXT("a sky light that has not captured writes no strength"), Place.Settings.Environment.SkyLightIntensity.IsSet());
		const FString Dropped = DetailsOf(Coverage, MantleCoverageCategory::Sky, EOutcome::Dropped);
		TestTrue(TEXT("and the report says why"), Dropped.Contains(TEXT("could not be captured")));
		TestFalse(TEXT("and no longer that the place has no setting for it"), Dropped.Contains(TEXT("no setting")));
	}

	// A real-time-capture sky light has an empty map on the game thread: it is captured once from a transient copy or, where
	// nothing can render, reported as not captured. Either way the creator's light is not changed and no copy is left spawned.
	{
		SkyLight->GetLightComponent()->bRealTimeCapture = true;
		FMantlePlace Place;
		FMantleCoverage Coverage;
		FMantleEnvironmentExport::Apply(Test.World, IdByActor, Place, Coverage);
		TestTrue(TEXT("the creator's sky light is still real-time"), SkyLight->GetLightComponent()->bRealTimeCapture);
		int32 SkyLights = 0;
		for (TActorIterator<ASkyLight> It(Test.World); It; ++It)
		{
			++SkyLights;
		}
		TestEqual(TEXT("and no copy is left in the level"), SkyLights, 1);
		SkyLight->GetLightComponent()->bRealTimeCapture = false;
	}

	// A target client that does not read the lighting keys is told so, and nothing is written.
	FMantleTargetFeatures Old = FMantleTargetFeatures::All();
	Old.Names.Remove(MantleClientFeatures::EnvironmentLighting);
	const FMantleTargetFeaturesScope Scope(Old);
	FMantlePlace OldPlace;
	FMantleCoverage OldCoverage;
	FMantleEnvironmentExport::Apply(Test.World, IdByActor, OldPlace, OldCoverage);
	TestFalse(TEXT("an older client gets no sky light key"), OldPlace.Settings.Environment.SkyLightIntensity.IsSet());
	TestTrue(TEXT("and the report says it needs a newer one"),
		DetailsOf(OldCoverage, MantleCoverageCategory::Sky, EOutcome::Dropped).Contains(FMantleTargetFeatures::NeedsNewerClient));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentCastsShadowTest,
	"Mantle.Editor.EnvironmentExport.CastsShadowInPlace",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentCastsShadowTest::RunTest(const FString& Parameters)
{
	UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(GetTransientPackage());
	if (!TestNotNull(TEXT("a mesh component"), Mesh))
	{
		return false;
	}
	TestFalse(TEXT("no component casts nothing"), FMantleEnvironmentExport::CastsShadowInPlace(nullptr));
	TestTrue(TEXT("a mesh at the engine's defaults casts a shadow"), FMantleEnvironmentExport::CastsShadowInPlace(Mesh));
	Mesh->bCastDynamicShadow = false;
	TestTrue(TEXT("one that casts the baked light's static shadow alone still does in a place, which has no baked light"),
		FMantleEnvironmentExport::CastsShadowInPlace(Mesh));
	Mesh->bCastStaticShadow = false;
	TestFalse(TEXT("one with neither does not"), FMantleEnvironmentExport::CastsShadowInPlace(Mesh));
	Mesh->bCastDynamicShadow = true;
	Mesh->CastShadow = false;
	TestFalse(TEXT("and Cast Shadow off is none whatever the others say"), FMantleEnvironmentExport::CastsShadowInPlace(Mesh));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentSunShadeTest,
	"Mantle.Editor.EnvironmentExport.DirectSun",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentSunShadeTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentExportTestPrivate;
	FTestWorld Test;
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("a test world"), Test.World) || !TestNotNull(TEXT("the engine's cube"), Cube))
	{
		return false;
	}
	const FVector TowardSun = FVector(0.3, 0.2, 0.9).GetSafeNormal();
	const TArray<FVector> Points = { FVector::ZeroVector, FVector(300.0, 0.0, 0.0) };
	TestFalse(TEXT("under open sky the sun reaches the points"), FMantleEnvironmentExport::IsShadedFromSun(Test.World, TowardSun, Points));
	TestFalse(TEXT("and with no points there is nothing to shade"), FMantleEnvironmentExport::IsShadedFromSun(Test.World, TowardSun, TArray<FVector>()));

	// A flat roof, a hundred metres wide, six metres up.
	AStaticMeshActor* Roof = Test.World->SpawnActor<AStaticMeshActor>(FVector(0.0, 0.0, 600.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("a roof"), Roof))
	{
		return false;
	}
	Roof->SetMobility(EComponentMobility::Movable);
	UStaticMeshComponent* RoofMesh = Roof->GetStaticMeshComponent();
	RoofMesh->SetStaticMesh(Cube);
	Roof->SetActorScale3D(FVector(400.0, 400.0, 1.0));
	RoofMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	RoofMesh->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	TestTrue(TEXT("a shadow-casting roof shades them"), FMantleEnvironmentExport::IsShadedFromSun(Test.World, TowardSun, Points));
	TestFalse(TEXT("unless one of the points is outside it"),
		FMantleEnvironmentExport::IsShadedFromSun(Test.World, TowardSun, { FVector::ZeroVector, FVector(900000.0, 0.0, 0.0) }));

	RoofMesh->SetCastShadow(false);
	TestFalse(TEXT("a roof that casts no shadow in the place lets the sun in, so the sun is kept"),
		FMantleEnvironmentExport::IsShadedFromSun(Test.World, TowardSun, Points));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentLightingKeysTest,
	"Mantle.Editor.EnvironmentExport.LightingKeys",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentLightingKeysTest::RunTest(const FString& Parameters)
{
	// The place settings actor's choices (#722): the strength it sets, and Never for the sun.
	AMantlePlaceSettingsActor* Settings = NewObject<AMantlePlaceSettingsActor>(GetTransientPackage());
	if (!TestNotNull(TEXT("a settings actor"), Settings))
	{
		return false;
	}
	TestTrue(TEXT("an untouched actor leaves both unset"),
		!Settings->MakeEnvironmentSettings().SkyLightIntensity.IsSet() && !Settings->MakeEnvironmentSettings().bNoDirectSun);
	Settings->bSetSkyLightIntensity = true;
	Settings->SkyLightIntensity = 0.25;
	Settings->DirectSun = EMantlePlaceDirectSun::Never;
	const FMantleEnvironmentSettings Chosen = Settings->MakeEnvironmentSettings();
	TestTrue(TEXT("a strength is carried"), Chosen.SkyLightIntensity.IsSet() && *Chosen.SkyLightIntensity == 0.25);
	TestTrue(TEXT("and Never is no direct sun"), Chosen.bNoDirectSun);
	Settings->SkyLightIntensity = 5.0;
	TestEqual(TEXT("a strength past 1 is held to it"), *Settings->MakeEnvironmentSettings().SkyLightIntensity, 1.0);
	Settings->DirectSun = EMantlePlaceDirectSun::Always;
	TestFalse(TEXT("Always leaves the key out"), Settings->MakeEnvironmentSettings().bNoDirectSun);
	Settings->SetEnvironmentFrom(Chosen);
	TestTrue(TEXT("a place file's values import into the actor"),
		Settings->bSetSkyLightIntensity && Settings->SkyLightIntensity == 0.25 && Settings->DirectSun == EMantlePlaceDirectSun::Never);
	Settings->SetEnvironmentFrom(FMantleEnvironmentSettings());
	TestTrue(TEXT("and a file without them imports as unset and Automatic"),
		!Settings->bSetSkyLightIntensity && Settings->DirectSun == EMantlePlaceDirectSun::Automatic);

	// The keys written, and read back by the parser.
	FMantlePlace Place;
	Place.SchemaVersion = MANTLE_PLACE_SCHEMA_VERSION;
	Place.PlaceId = TEXT("plc_lighting_keys");
	Place.Revision.Number = 1;
	Place.Revision.State = EMantleRevisionState::Draft;
	Place.Metadata.Name = TEXT("Lighting keys");
	Place.Metadata.AuthorId = TEXT("usr_test");
	// The place file is read as JSON, not as text: how it is spaced is the writer's business.
	auto EnvironmentOf = [](const FString& Text) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Root;
		if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) && Root.IsValid())
		{
			const TSharedPtr<FJsonObject>* Settings = nullptr;
			const TSharedPtr<FJsonObject>* Environment = nullptr;
			if (Root->TryGetObjectField(TEXT("settings"), Settings) && (*Settings)->TryGetObjectField(TEXT("environment"), Environment))
			{
				return *Environment;
			}
		}
		return nullptr;
	};

	const FString Plain = FMantlePlaceExporter::ToJson(Place);
	const TSharedPtr<FJsonObject> PlainEnvironment = EnvironmentOf(Plain);
	TestFalse(TEXT("a place that sets neither writes neither"),
		PlainEnvironment.IsValid() && (PlainEnvironment->HasField(TEXT("skyLight")) || PlainEnvironment->HasField(TEXT("sun"))));

	Place.Settings.Environment = Chosen;
	const FString Json = FMantlePlaceExporter::ToJson(Place);
	const TSharedPtr<FJsonObject> Environment = EnvironmentOf(Json);
	if (!TestTrue(TEXT("a place that sets both writes an environment block"), Environment.IsValid()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* SkyLight = nullptr;
	if (TestTrue(TEXT("with skyLight"), Environment->TryGetObjectField(TEXT("skyLight"), SkyLight)))
	{
		TestEqual(TEXT("and its intensity"), (*SkyLight)->GetNumberField(TEXT("intensity")), 0.25);
	}
	const TSharedPtr<FJsonObject>* Sun = nullptr;
	if (TestTrue(TEXT("and sun"), Environment->TryGetObjectField(TEXT("sun"), Sun)))
	{
		TestTrue(TEXT("with direct false"), (*Sun)->HasTypedField<EJson::Boolean>(TEXT("direct")) && !(*Sun)->GetBoolField(TEXT("direct")));
	}
	auto InEnvironment = [](const FMantleParseError& Error) { return Error.Path.StartsWith(TEXT("/settings/environment")); };
	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
	TestFalse(TEXT("the parser has no complaint about the environment block"), Parsed.Errors.ContainsByPredicate(InEnvironment));
	TestTrue(TEXT("it reads the strength back"), Parsed.Place.Settings.Environment.SkyLightIntensity.IsSet() && *Parsed.Place.Settings.Environment.SkyLightIntensity == 0.25);
	TestTrue(TEXT("and the sun"), Parsed.Place.Settings.Environment.bNoDirectSun);

	// Refused: a scale past 1, a sun that is not a bool and a key the block does not know. Each edits the parsed file and writes it again.
	auto Refused = [&Json](const TFunctionRef<void(const TSharedPtr<FJsonObject>& Environment)>& Edit)
	{
		TSharedPtr<FJsonObject> Root;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root);
		const TSharedPtr<FJsonObject>* Settings = nullptr;
		const TSharedPtr<FJsonObject>* Env = nullptr;
		Root->TryGetObjectField(TEXT("settings"), Settings);
		(*Settings)->TryGetObjectField(TEXT("environment"), Env);
		Edit(*Env);
		FString Edited;
		FJsonSerializer::Serialize(Root.ToSharedRef(), TJsonWriterFactory<>::Create(&Edited));
		return FMantlePlaceParser::ParseFromString(Edited).Errors.ContainsByPredicate([](const FMantleParseError& Error)
		{
			return Error.Path.StartsWith(TEXT("/settings/environment"));
		});
	};
	TestTrue(TEXT("an intensity of 1.5 is refused"), Refused([](const TSharedPtr<FJsonObject>& Env)
	{
		Env->GetObjectField(TEXT("skyLight"))->SetNumberField(TEXT("intensity"), 1.5);
	}));
	TestTrue(TEXT("a direct of \"no\" is refused"), Refused([](const TSharedPtr<FJsonObject>& Env)
	{
		Env->GetObjectField(TEXT("sun"))->SetStringField(TEXT("direct"), TEXT("no"));
	}));
	TestTrue(TEXT("an unknown key is refused"), Refused([](const TSharedPtr<FJsonObject>& Env)
	{
		Env->GetObjectField(TEXT("skyLight"))->SetNumberField(TEXT("colour"), 1.0);
	}));
	TestFalse(TEXT("and the unedited file is not"), Refused([](const TSharedPtr<FJsonObject>&) {}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
