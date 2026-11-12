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

		/** A second directional light (a blue fill sun), after the first in the place's entities. */
		bool AddFill(FTestWorld& Test)
		{
			ADirectionalLight* Fill = Test.Spawn<ADirectionalLight>(TEXT("BlueFill"), FRotator(-30.0, 200.0, 0.0));
			if (!Fill)
			{
				return false;
			}
			FMantleEntity Entity;
			Entity.Id = TEXT("ent_fill");
			Entity.Type = EMantleEntityType::DirectionalLight;
			Entity.Name = TEXT("BlueFill");
			Entity.Transform = Fill->GetActorTransform();
			FMantleDirectionalLightProps Props;
			Props.Intensity = 2.0;
			Props.Color = FLinearColor(0.2f, 0.3f, 1.0f);
			Entity.Props.Set<FMantleDirectionalLightProps>(Props);
			Place.Entities.Add(MoveTemp(Entity));
			IdByActor.Add(Fill, TEXT("ent_fill"));
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
		// The level's sun (8, with a 4000 K temperature) was tuned to its own sky: the place gets the game's.
		TestNearlyEqual(TEXT("the sun is at the game's intensity, not the level's 8"), Props->Intensity, 3.5, 0.0001);
		TestTrue(TEXT("and the game's color, not the light's 4000 K"), Props->Color.Equals(FLinearColor(1.0f, 0.97f, 0.92f), 0.0001f));
	}
	TestFalse(TEXT("the volume's saturation is not carried"), Environment.Saturation.IsSet());
	TestFalse(TEXT("nor its bloom"), Environment.Bloom.IsSet());
	TestFalse(TEXT("nor its white balance as a color tint"), Environment.ColorTint.IsSet());
	TestFalse(TEXT("nor its exposure: this is an outdoor place"), Environment.Exposure.IsSet());
	TestTrue(TEXT("a sky with no cloud holds the weather clear"), Environment.Weather.IsSet() && *Environment.Weather == EMantleWeatherState::Clear);

	TestEqual(TEXT("the sun is reported once, approximated"), CountOf(Coverage, MantleCoverageCategory::Lights, EOutcome::Approximated), static_cast<int64>(1));
	TestEqual(TEXT("and nothing of it dropped"), CountOf(Coverage, MantleCoverageCategory::Lights, EOutcome::Dropped), static_cast<int64>(0));
	const FString SunLine = DetailsOf(Coverage, MantleCoverageCategory::Lights, EOutcome::Approximated);
	TestTrue(TEXT("the sun's line says its brightness is the game's, not the level's"), SunLine.Contains(TEXT("game's own (3.5)")) && SunLine.Contains(TEXT("level's (8)")));
	TestTrue(TEXT("and where to change it on purpose"), SunLine.Contains(TEXT("Sun Brightness in Mantle Place Settings")) && SunLine.Contains(TEXT("SunIntensityScale")));
	TestEqual(TEXT("the volume is dropped: nothing of it is carried"), CountOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Dropped), static_cast<int64>(1));
	TestEqual(TEXT("and not approximated"), CountOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Approximated), static_cast<int64>(0));
	const FString Grading = DetailsOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Dropped);
	TestTrue(TEXT("saying what was left out"), Grading.Contains(TEXT("Saturation 1.40")) && Grading.Contains(TEXT("Bloom 2.00"))
		&& Grading.Contains(TEXT("White balance 8000 K")) && Grading.Contains(TEXT("exposure (")) && Grading.Contains(TEXT("1.5 stops")));
	TestTrue(TEXT("and where to set them on purpose"), Grading.Contains(TEXT("Mantle Place Settings")));
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
	Environment.Bloom = 0.25;
	Environment.ColorTint = 0xFFE0C0;
	Environment.Weather = EMantleWeatherState::Storm;

	FMantleCoverage Coverage;
	FMantleEnvironmentExport::Apply(Test.World, Level.IdByActor, Level.Place, Coverage);

	TestTrue(TEXT("the settings' clock wins"), Environment.ClockTime.IsSet() && *Environment.ClockTime == 21.0);
	TestFalse(TEXT("and the level's sun does not stop it"), Environment.TimeScale.IsSet());
	TestTrue(TEXT("the settings' saturation wins"), Environment.Saturation.IsSet() && *Environment.Saturation == 0.5);
	TestTrue(TEXT("the settings' weather wins over a sky with no cloud"), Environment.Weather.IsSet() && *Environment.Weather == EMantleWeatherState::Storm);
	TestTrue(TEXT("the settings' bloom wins over the volume's 2"), Environment.Bloom.IsSet() && *Environment.Bloom == 0.25);
	TestTrue(TEXT("and its color tint"), Environment.ColorTint.IsSet() && *Environment.ColorTint == 0xFFE0C0);
	const FString Won = DetailsOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Dropped);
	TestTrue(TEXT("the report says the settings' values won"), Won.Contains(TEXT("Saturation (Mantle Place Settings sets its own, which wins)"))
		&& Won.Contains(TEXT("Bloom (Mantle Place Settings sets its own, which wins)")));
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
		TestNearlyEqual(TEXT("but the sun is still at the game's intensity, which every client reads"), Props->Intensity, 3.5, 0.0001);
	}
	TestTrue(TEXT("the sun's angle says it needs a newer client"),
		DetailsOf(Coverage, MantleCoverageCategory::Lights, EOutcome::Approximated).Contains(FMantleTargetFeatures::NeedsNewerClient));
	TestEqual(TEXT("the volume has nothing carried, so it is dropped"), CountOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Dropped), static_cast<int64>(1));
	TestTrue(TEXT("saying it was tuned for a different sky"), DetailsOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Dropped).Contains(TEXT("different sky")));
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
	// A project whose default is an auto exposure shows the level that way, but an outdoor place (this level has a cloud) does not
	// carry it: it is reported as dropped.
	const bool bAutoProject = FMantleExposureProject::Current().bAuto;
	TestEqual(TEXT("a volume that overrides nothing changes nothing, in a project with fixed exposure"),
		CountOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Info), static_cast<int64>(bAutoProject ? 0 : 1));
	TestEqual(TEXT("and drops the default auto exposure in one with auto"),
		CountOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Dropped), static_cast<int64>(bAutoProject ? 1 : 0));
	TestEqual(TEXT("nothing of the volume is carried"), CountOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Approximated), static_cast<int64>(0));
	TestFalse(TEXT("so no exposure is written for an outdoor place, whatever the project"), Place.Settings.Environment.Exposure.IsSet());
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
	TestEqual(TEXT("the volume is one line, dropped: its saturation is not carried"), CountOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Dropped), static_cast<int64>(1));
	TestEqual(TEXT("and not also approximated by the survey"), CountOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Approximated), static_cast<int64>(0));
	TestEqual(TEXT("the fog is one dropped line: Mantle's own is used"), CountOf(Coverage, MantleCoverageCategory::Fog, EOutcome::Dropped), static_cast<int64>(1));
	TestEqual(TEXT("and not approximated"), CountOf(Coverage, MantleCoverageCategory::Fog, EOutcome::Approximated), static_cast<int64>(0));
	TestFalse(TEXT("no fog is written"), Exported.Place.Settings.Environment.Fog.IsSet());
	TestTrue(TEXT("a level with a sun and no cloud holds the weather clear"),
		Exported.Place.Settings.Environment.Weather.IsSet() && *Exported.Place.Settings.Environment.Weather == EMantleWeatherState::Clear);
	TestTrue(TEXT("the place says when the clock starts"), Exported.Place.Settings.Environment.ClockTime.IsSet());
	TestFalse(TEXT("but nothing of the level's saturation"), Exported.Place.Settings.Environment.Saturation.IsSet());

	const FString Json = FMantlePlaceExporter::ToJson(Exported.Place);
	TestTrue(TEXT("the file writes the clock"), Json.Contains(TEXT("\"clockTime\"")) && Json.Contains(TEXT("\"timeScale\"")));
	TestFalse(TEXT("and none of the level's look: no saturation, fog, exposure, bloom or tint"),
		Json.Contains(TEXT("\"saturation\"")) || Json.Contains(TEXT("\"fog\"")) || Json.Contains(TEXT("\"exposure\""))
		|| Json.Contains(TEXT("\"bloom")) || Json.Contains(TEXT("\"colorTint\"")));
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
	FMantleEnvironmentFogLeftOutTest,
	"Mantle.Editor.EnvironmentExport.FogIsLeftOut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentFogLeftOutTest::RunTest(const FString& Parameters)
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

	TestFalse(TEXT("the level's fog is not the place's"), Place.Settings.Environment.Fog.IsSet());
	TestTrue(TEXT("so the place writes no environment block for it"), Place.Settings.Environment.IsEmpty());
	TestEqual(TEXT("the fog is one dropped line"), CountOf(Coverage, MantleCoverageCategory::Fog, EOutcome::Dropped), static_cast<int64>(1));
	TestEqual(TEXT("and nothing of it approximated"), CountOf(Coverage, MantleCoverageCategory::Fog, EOutcome::Approximated), static_cast<int64>(0));
	const FString Report = DetailsOf(Coverage, MantleCoverageCategory::Fog, EOutcome::Dropped);
	TestTrue(TEXT("saying what was left out"), Report.Contains(TEXT("density (0.005)")) && Report.Contains(TEXT("inscattering color")) && Report.Contains(TEXT("directional")));
	TestTrue(TEXT("and what else was tuned, by name"), Report.Contains(TEXT("Max Opacity")));
	TestTrue(TEXT("saying Mantle supplies its own, and where to set one on purpose"),
		Report.Contains(TEXT("Mantle supplies its own fog")) && Report.Contains(TEXT("Environment.FogDensity")) && Report.Contains(TEXT("Environment.FogColor")));

	// A second fog is not used by the engine: reported once, as such.
	AExponentialHeightFog* Second = Test.Spawn<AExponentialHeightFog>(TEXT("SecondMist"));
	if (TestNotNull(TEXT("a second fog"), Second))
	{
		FMantlePlace Both;
		FMantleCoverage BothCoverage;
		FMantleEnvironmentExport::Apply(Test.World, IdByActor, Both, BothCoverage);
		TestFalse(TEXT("two fogs still write none"), Both.Settings.Environment.Fog.IsSet());
		TestEqual(TEXT("and report both, one way or the other"),
			CountOf(BothCoverage, MantleCoverageCategory::Fog, EOutcome::Dropped) + CountOf(BothCoverage, MantleCoverageCategory::Fog, EOutcome::Kept), static_cast<int64>(2));
	}
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
	TestNearlyEqual(TEXT("the color"), Fog->FogInscatteringLuminance.G, 0.638f, 1.0e-4f);
	TestEqual(TEXT("the falloff keeps the fog's own"), Fog->FogHeightFalloff, OwnFalloff);
	TestEqual(TEXT("and the start distance"), Fog->StartDistance, 0.0f);

	FMantleFogSettings More;
	More.HeightFalloff = 0.7;
	More.StartDistance = 5000.0;
	More.DirectionalInscatteringColor = FLinearColor(0.25f, 0.25f, 0.125f);
	FMantleEnvironment::ApplyFogSettings(Spawned, More);
	TestNearlyEqual(TEXT("the falloff"), Fog->FogHeightFalloff, 0.7f, 1.0e-6f);
	TestEqual(TEXT("the start distance"), Fog->StartDistance, 5000.0f);
	TestNearlyEqual(TEXT("the directional color"), Fog->DirectionalInscatteringLuminance.B, 0.125f, 1.0e-4f);
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

	// A float cubemap of one gray: its mean is that gray, whatever the weights.
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

	// A hill far off in the sun's way, and a tree-sized block near it with the sky open above: neither encloses the points.
	AStaticMeshActor* Hill = Test.World->SpawnActor<AStaticMeshActor>(TowardSun * 20000.0, FRotator::ZeroRotator);
	AStaticMeshActor* Tree = Test.World->SpawnActor<AStaticMeshActor>(TowardSun * 1000.0, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("a hill"), Hill) || !TestNotNull(TEXT("a tree"), Tree))
	{
		return false;
	}
	for (AStaticMeshActor* Blocker : { Hill, Tree })
	{
		Blocker->SetMobility(EComponentMobility::Movable);
		UStaticMeshComponent* BlockerMesh = Blocker->GetStaticMeshComponent();
		BlockerMesh->SetStaticMesh(Cube);
		BlockerMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		BlockerMesh->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	}
	Hill->SetActorScale3D(FVector(100.0));
	Tree->SetActorScale3D(FVector(3.0));
	TestFalse(TEXT("a hill far off in the sun's way does not shade the points"), FMantleEnvironmentExport::IsShadedFromSun(Test.World, TowardSun, Points));
	Hill->Destroy();
	TestFalse(TEXT("nor does a tree near them with open sky above"), FMantleEnvironmentExport::IsShadedFromSun(Test.World, TowardSun, Points));
	Tree->Destroy();

	// A closed box, 80 m wide and 10 m tall, under a low sun: the sun's way crosses more than 30 m of the hall before it meets the roof or a wall.
	{
		const FVector LowSun = FVector(FMath::Cos(FMath::DegreesToRadians(10.0)), 0.0, FMath::Sin(FMath::DegreesToRadians(10.0)));
		TArray<AStaticMeshActor*> Box;
		const auto AddSlab = [&](const FVector& Where, const FVector& Scale)
		{
			AStaticMeshActor* Slab = Test.World->SpawnActor<AStaticMeshActor>(Where, FRotator::ZeroRotator);
			if (Slab)
			{
				Slab->SetMobility(EComponentMobility::Movable);
				UStaticMeshComponent* SlabMesh = Slab->GetStaticMeshComponent();
				SlabMesh->SetStaticMesh(Cube);
				Slab->SetActorScale3D(Scale);
				SlabMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
				SlabMesh->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
				Box.Add(Slab);
			}
		};
		AddSlab(FVector(0.0, 0.0, 1000.0), FVector(80.0, 80.0, 1.0));
		AddSlab(FVector(4000.0, 0.0, 500.0), FVector(1.0, 80.0, 10.0));
		AddSlab(FVector(-4000.0, 0.0, 500.0), FVector(1.0, 80.0, 10.0));
		AddSlab(FVector(0.0, 4000.0, 500.0), FVector(80.0, 1.0, 10.0));
		AddSlab(FVector(0.0, -4000.0, 500.0), FVector(80.0, 1.0, 10.0));
		TestEqual(TEXT("a box's five slabs"), Box.Num(), 5);
		TestTrue(TEXT("a closed hall shades its spawn point from a low sun, however far the wall is"),
			FMantleEnvironmentExport::IsShadedFromSun(Test.World, LowSun, { FVector::ZeroVector }));
		for (AStaticMeshActor* Slab : Box)
		{
			Slab->Destroy();
		}
	}

	// A flat roof, a hundred meters wide, six meters up.
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
	FMantleEnvironmentInteriorExportTest,
	"Mantle.Editor.EnvironmentExport.Interior",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentInteriorExportTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentExportTestPrivate;
	FTestWorld Test;
	if (!TestNotNull(TEXT("a test world"), Test.World))
	{
		return false;
	}
	// A level with a post-process volume and nothing else: no sun, sky light, atmosphere, clouds or fog (Room A's shape).
	APostProcessVolume* Grade = Test.Spawn<APostProcessVolume>(TEXT("Grade"));
	if (!TestNotNull(TEXT("a post-process volume"), Grade))
	{
		return false;
	}
	Grade->bUnbound = true;
	Grade->Settings.bOverride_BloomIntensity = true;
	Grade->Settings.BloomIntensity = 2.0f;
	Grade->Settings.bOverride_BloomThreshold = true;
	Grade->Settings.BloomThreshold = 2.0f;

	FMantlePlace Place;
	TMap<const AActor*, FString> IdByActor;
	FMantleCoverage Coverage;
	FMantleEnvironmentExport::Apply(Test.World, IdByActor, Place, Coverage);
	const FMantleEnvironmentSettings& Environment = Place.Settings.Environment;
	if (!FMantleTargetFeatures::Current().Reads(MantleClientFeatures::EnvironmentLighting))
	{
		AddInfo(TEXT("the target client does not read the lighting keys; the interior key is dropped"));
		return true;
	}
	TestTrue(TEXT("a level with no environment of its own is an interior"), Environment.bNoOutdoors);
	TestFalse(TEXT("the level's bloom threshold is not carried, even for an interior"), Environment.BloomThreshold.IsSet());
	TestFalse(TEXT("nor its bloom"), Environment.Bloom.IsSet());
	TestFalse(TEXT("the weather is not written for it: the client holds it"), Environment.Weather.IsSet());
	const FString Json = FMantlePlaceExporter::ToJson(Place);
	TestTrue(TEXT("outdoors false is written"), Json.Replace(TEXT(" "), TEXT("")).Contains(TEXT("\"outdoors\":false")));
	TestFalse(TEXT("and no bloom threshold"), Json.Contains(TEXT("bloomThreshold")));
	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
	TestTrue(TEXT("the parser reads it back"), Parsed.Place.Settings.Environment.bNoOutdoors);

	// A level with an atmosphere is not an interior.
	Grade->Settings.BloomThreshold = -1.0f;
	ASkyAtmosphere* Atmosphere = Test.Spawn<ASkyAtmosphere>(TEXT("Atmosphere"));
	if (!TestNotNull(TEXT("a sky atmosphere"), Atmosphere))
	{
		return false;
	}
	FMantlePlace Outdoor;
	FMantleCoverage OutdoorCoverage;
	FMantleEnvironmentExport::Apply(Test.World, IdByActor, Outdoor, OutdoorCoverage);
	TestFalse(TEXT("a level with its own sky is outdoors"), Outdoor.Settings.Environment.bNoOutdoors);

	// A sky Blueprint holds its sky light as a component on an ordinary actor: that is an outdoor environment too.
	Atmosphere->Destroy();
	AActor* SkyBlueprint = Test.Spawn<AActor>(TEXT("SkyBlueprint"));
	if (!TestNotNull(TEXT("a Blueprint-style actor"), SkyBlueprint))
	{
		return false;
	}
	USkyLightComponent* SkyPart = NewObject<USkyLightComponent>(SkyBlueprint, TEXT("SkyLightPart"));
	SkyBlueprint->SetRootComponent(SkyPart);
	SkyPart->RegisterComponent();
	FMantlePlace ComponentSky;
	FMantleCoverage ComponentCoverage;
	FMantleEnvironmentExport::Apply(Test.World, IdByActor, ComponentSky, ComponentCoverage);
	TestFalse(TEXT("a sky light inside a Blueprint actor makes the level outdoors"), ComponentSky.Settings.Environment.bNoOutdoors);
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
		Env->GetObjectField(TEXT("skyLight"))->SetNumberField(TEXT("color"), 1.0);
	}));
	TestFalse(TEXT("and the unedited file is not"), Refused([](const TSharedPtr<FJsonObject>&) {}));
	return true;
}

// ---- Upload no longer copies the level's look ---------------------------------------------------------------------
// A level's sun brightness, exposure, fog, bloom, tint and saturation were tuned for its own sky and wash the picture out
// under Mantle's. Outdoor places get the game's own; an interior keeps its exposure; what Mantle Place Settings sets still wins.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentOutdoorDropsExposureTest,
	"Mantle.Editor.EnvironmentExport.OutdoorPlaceDropsTheLevelsExposure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentOutdoorDropsExposureTest::RunTest(const FString& Parameters)
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
	const FMantleExposureProject Project = FMantleExposureProject::Current();

	// No volume: in a project with automatic exposure the editor shows the level that way, and an outdoor place does not carry it.
	{
		FMantleCoverage Coverage;
		FMantleEnvironmentExport::Apply(Test.World, Level.IdByActor, Level.Place, Coverage);
		TestFalse(TEXT("an outdoor level with no volume writes no exposure"), Level.Place.Settings.Environment.Exposure.IsSet());
		const FString Dropped = DetailsOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Dropped);
		TestEqual(TEXT("and says so once if the project's exposure is automatic"), CountOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Dropped), static_cast<int64>(Project.bAuto ? 1 : 0));
		if (Project.bAuto)
		{
			TestTrue(TEXT("saying Mantle supplies its own and where to set one"),
				Dropped.Contains(TEXT("Mantle supplies its own exposure")) && Dropped.Contains(TEXT("Mantle Place Settings")) && Dropped.Contains(TEXT("Environment.ExposureBias")));
		}
	}

	// A volume with an exposure of its own, 1.5 stops over the project's: left out as well.
	APostProcessVolume* Grade = Test.Spawn<APostProcessVolume>(TEXT("Grade"));
	if (!TestNotNull(TEXT("a post-process volume"), Grade))
	{
		return false;
	}
	Grade->bUnbound = true;
	Grade->Settings.bOverride_AutoExposureBias = true;
	Grade->Settings.AutoExposureBias = static_cast<float>(Project.EngineBias + 1.5);
	Grade->Settings.bOverride_AutoExposureMinBrightness = true;
	Grade->Settings.AutoExposureMinBrightness = 5.0f;
	Grade->Settings.bOverride_AutoExposureMaxBrightness = true;
	Grade->Settings.AutoExposureMaxBrightness = 5.0f;
	FMantleCoverage Coverage;
	FMantleEnvironmentExport::Apply(Test.World, Level.IdByActor, Level.Place, Coverage);
	TestFalse(TEXT("an outdoor level's volume exposure is not carried"), Level.Place.Settings.Environment.Exposure.IsSet());
	const FString Dropped = DetailsOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Dropped);
	TestTrue(TEXT("the report says what was left out"), Dropped.Contains(TEXT("exposure (")) && Dropped.Contains(TEXT("1.5 stops")));
	TestTrue(TEXT("and where to set an exposure on purpose"), Dropped.Contains(TEXT("Exposure")) && Dropped.Contains(TEXT("Mantle Place Settings")));
	const FString Json = FMantlePlaceExporter::ToJson(Level.Place);
	TestFalse(TEXT("so the file has no exposure"), Json.Contains(TEXT("\"exposure\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentInteriorKeepsExposureTest,
	"Mantle.Editor.EnvironmentExport.InteriorKeepsTheLevelsExposure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentInteriorKeepsExposureTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentExportTestPrivate;
	if (!FMantleTargetFeatures::Current().Reads(MantleClientFeatures::EnvironmentLighting))
	{
		AddInfo(TEXT("the target client does not read the lighting keys, so there is no interior"));
		return true;
	}
	const FMantleExposureProject Project = FMantleExposureProject::Current();
	const auto AddGrade = [&Project](FTestWorld& Test)
	{
		APostProcessVolume* Grade = Test.Spawn<APostProcessVolume>(TEXT("Grade"));
		if (Grade)
		{
			Grade->bUnbound = true;
			Grade->Settings.bOverride_AutoExposureBias = true;
			Grade->Settings.AutoExposureBias = static_cast<float>(Project.EngineBias + 1.5);
			Grade->Settings.bOverride_BloomIntensity = true;
			Grade->Settings.BloomIntensity = 2.0f;
		}
		return Grade;
	};

	// A level with a volume and nothing else is an interior: its exposure travels as it did, its bloom does not.
	{
		FTestWorld Test;
		if (!TestNotNull(TEXT("a test world"), Test.World) || !TestNotNull(TEXT("a volume"), AddGrade(Test)))
		{
			return false;
		}
		FMantlePlace Place;
		TMap<const AActor*, FString> IdByActor;
		FMantleCoverage Coverage;
		FMantleEnvironmentExport::Apply(Test.World, IdByActor, Place, Coverage);
		const FMantleEnvironmentSettings& Environment = Place.Settings.Environment;
		TestTrue(TEXT("it is an interior"), Environment.bNoOutdoors);
		if (TestTrue(TEXT("its exposure is the place's"), Environment.Exposure.IsSet()))
		{
			TestNearlyEqual(TEXT("with the volume's bias over the project's"), Environment.Exposure->Bias, 1.5, 0.001);
		}
		TestFalse(TEXT("its bloom is not"), Environment.Bloom.IsSet());
		TestEqual(TEXT("the volume is approximated: its exposure was carried"), CountOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Approximated), static_cast<int64>(1));
		const FString Json = FMantlePlaceExporter::ToJson(Place);
		TestTrue(TEXT("the file writes the exposure"), Json.Contains(TEXT("\"exposure\"")));
	}

	// A level with a sun that Mantle Place Settings makes an interior keeps its exposure too.
	{
		FTestWorld Test;
		if (!TestNotNull(TEXT("a test world"), Test.World) || !TestNotNull(TEXT("a volume"), AddGrade(Test)))
		{
			return false;
		}
		FSunLevel Level;
		AMantlePlaceSettingsActor* Settings = Test.Spawn<AMantlePlaceSettingsActor>(TEXT("Settings"));
		if (!TestTrue(TEXT("a sun"), Level.Build(Test, FRotator(-46.0, 30.0, 0.0), /*bTemperature*/ false)) || !TestNotNull(TEXT("place settings"), Settings))
		{
			return false;
		}
		Settings->Outdoors = EMantlePlaceOutdoors::Interior;
		Level.Place.Settings.Environment = Settings->MakeEnvironmentSettings();
		FMantleCoverage Coverage;
		FMantleEnvironmentExport::Apply(Test.World, Level.IdByActor, Level.Place, Coverage);
		TestTrue(TEXT("the settings make it an interior"), Level.Place.Settings.Environment.bNoOutdoors);
		TestTrue(TEXT("and its exposure is carried"), Level.Place.Settings.Environment.Exposure.IsSet());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentSunAtGameBrightnessTest,
	"Mantle.Editor.EnvironmentExport.SunIsTheGamesAndExtraSunsAreLeftOut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentSunAtGameBrightnessTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentExportTestPrivate;
	FTestWorld Test;
	if (!TestNotNull(TEXT("a test world"), Test.World))
	{
		return false;
	}
	// The level's sun is 8 with a 4000 K temperature; a blue fill sun is 2.
	FSunLevel Level;
	if (!TestTrue(TEXT("a sun"), Level.Build(Test, FRotator(-46.0, 30.0, 0.0), /*bTemperature*/ true)) || !TestTrue(TEXT("a fill sun"), Level.AddFill(Test)))
	{
		return false;
	}
	FMantleCoverage Coverage;
	FMantleEnvironmentExport::Apply(Test.World, Level.IdByActor, Level.Place, Coverage);

	if (!TestEqual(TEXT("only the first sun is published"), Level.Place.Entities.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("and it is the first one"), Level.Place.Entities[0].Name, FString(TEXT("Sun")));
	const FMantleDirectionalLightProps* Props = Level.Place.Entities[0].Props.TryGet<FMantleDirectionalLightProps>();
	if (TestNotNull(TEXT("with sun props"), Props))
	{
		TestEqual(TEXT("at the game's own intensity, 3.5"), Props->Intensity, 3.5);
		TestTrue(TEXT("and color, (1, 0.97, 0.92)"), Props->Color.Equals(FLinearColor(1.0f, 0.97f, 0.92f), 0.0001f));
	}
	TestTrue(TEXT("the sun's angle is still carried as the clock"), Level.Place.Settings.Environment.ClockTime.IsSet());
	TestEqual(TEXT("the fill sun is one dropped line"), CountOf(Coverage, MantleCoverageCategory::Lights, EOutcome::Dropped), static_cast<int64>(1));
	const FString Dropped = DetailsOf(Coverage, MantleCoverageCategory::Lights, EOutcome::Dropped);
	TestTrue(TEXT("naming the light"), Dropped.Contains(TEXT("BlueFill")));
	TestTrue(TEXT("saying Mantle supplies its own and where to change the sun on purpose"),
		Dropped.Contains(TEXT("Mantle supplies its own sun")) && Dropped.Contains(TEXT("Sun Brightness")) && Dropped.Contains(TEXT("SunIntensityScale")));

	// A fill sun other parts hang from stays, dark, so they keep their parent; it is reported all the same.
	FSunLevel Parented;
	if (!TestTrue(TEXT("another sun"), Parented.Build(Test, FRotator(-46.0, 30.0, 0.0), false)) || !TestTrue(TEXT("another fill"), Parented.AddFill(Test)))
	{
		return false;
	}
	FMantleEntity Child;
	Child.Id = TEXT("ent_child");
	Child.Type = EMantleEntityType::Group;
	Child.Name = TEXT("Child");
	Child.Parent = TEXT("ent_fill");
	Parented.Place.Entities.Add(MoveTemp(Child));
	FMantleCoverage ParentedCoverage;
	FMantleEnvironmentExport::Apply(Test.World, Parented.IdByActor, Parented.Place, ParentedCoverage);
	TestEqual(TEXT("with a child the fill stays, with the child"), Parented.Place.Entities.Num(), 3);
	if (const FMantleDirectionalLightProps* FillProps = Parented.Place.Entities[1].Props.TryGet<FMantleDirectionalLightProps>())
	{
		TestEqual(TEXT("but dark"), FillProps->Intensity, 0.0);
	}
	TestEqual(TEXT("and is still reported as dropped"), CountOf(ParentedCoverage, MantleCoverageCategory::Lights, EOutcome::Dropped), static_cast<int64>(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentPlaceSettingsStillWriteTest,
	"Mantle.Editor.EnvironmentExport.PlaceSettingsStillWriteTheirLook",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentPlaceSettingsStillWriteTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentExportTestPrivate;
	FTestWorld Test;
	if (!TestNotNull(TEXT("a test world"), Test.World))
	{
		return false;
	}
	FSunLevel Level;
	APostProcessVolume* Grade = Test.Spawn<APostProcessVolume>(TEXT("Grade"));
	AMantlePlaceSettingsActor* Settings = Test.Spawn<AMantlePlaceSettingsActor>(TEXT("Settings"));
	if (!TestTrue(TEXT("a sun"), Level.Build(Test, FRotator(-46.0, 30.0, 0.0), /*bTemperature*/ false)) || !TestNotNull(TEXT("a volume"), Grade)
		|| !TestNotNull(TEXT("place settings"), Settings))
	{
		return false;
	}
	// The level's look: all of it left out. Place Settings' look: all of it written.
	const FMantleExposureProject Project = FMantleExposureProject::Current();
	Grade->bUnbound = true;
	Grade->Settings.bOverride_ColorSaturation = true;
	Grade->Settings.ColorSaturation = FVector4(1.4, 1.4, 1.4, 1.0);
	Grade->Settings.bOverride_BloomIntensity = true;
	Grade->Settings.BloomIntensity = 2.0f;
	Grade->Settings.bOverride_WhiteTemp = true;
	Grade->Settings.WhiteTemp = 8000.0f;
	Grade->Settings.bOverride_AutoExposureBias = true;
	Grade->Settings.AutoExposureBias = static_cast<float>(Project.EngineBias + 1.5);

	Settings->ColorTint = FColor(255, 224, 192);
	Settings->Saturation = 0.5;
	Settings->bSetBloom = true;
	Settings->Bloom = 0.25;
	Settings->ColorGrading = EMantlePlaceColorGrading::Warm;
	Settings->ColorGradingIntensity = 0.5;
	Settings->bSetSkyLightIntensity = true;
	Settings->SkyLightIntensity = 0.5;
	Settings->bSetExposure = true;
	Settings->Exposure = -1.5;
	Settings->bSetSunBrightness = true;
	Settings->SunBrightness = 2.0;
	Level.Place.Settings.Environment = Settings->MakeEnvironmentSettings();

	FMantleCoverage Coverage;
	FMantleEnvironmentExport::Apply(Test.World, Level.IdByActor, Level.Place, Coverage);
	const FMantleEnvironmentSettings& Environment = Level.Place.Settings.Environment;
	TestTrue(TEXT("Color Tint is written"), Environment.ColorTint.IsSet() && *Environment.ColorTint == 0xFFE0C0);
	TestTrue(TEXT("Saturation"), Environment.Saturation.IsSet() && *Environment.Saturation == 0.5);
	TestTrue(TEXT("Bloom"), Environment.Bloom.IsSet() && *Environment.Bloom == 0.25);
	TestTrue(TEXT("Color Grading and its strength"), Environment.ColorGrading.IsSet() && Environment.ColorGradingIntensity.IsSet() && *Environment.ColorGradingIntensity == 0.5);
	TestTrue(TEXT("Sky Light Intensity"), Environment.SkyLightIntensity.IsSet() && *Environment.SkyLightIntensity == 0.5);
	if (TestTrue(TEXT("the exposure is the settings' own, not the volume's +1.5"), Environment.Exposure.IsSet()))
	{
		TestEqual(TEXT("a bias of -1.5"), Environment.Exposure->Bias, -1.5);
		TestTrue(TEXT("on the game's fixed exposure"), Environment.Exposure->Mode == EMantleExposureMode::Fixed);
	}
	if (const FMantleDirectionalLightProps* Props = Level.Place.Entities[0].Props.TryGet<FMantleDirectionalLightProps>())
	{
		TestEqual(TEXT("the sun is 2 times the game's, 7"), Props->Intensity, 7.0);
		TestTrue(TEXT("in the game's color"), Props->Color.Equals(FLinearColor(1.0f, 0.97f, 0.92f), 0.0001f));
	}
	const FString Won = DetailsOf(Coverage, MantleCoverageCategory::PostProcess, EOutcome::Dropped);
	TestTrue(TEXT("the report says the settings won, for each"), Won.Contains(TEXT("Exposure (Mantle Place Settings sets its own, which wins)"))
		&& Won.Contains(TEXT("Saturation (Mantle Place Settings sets its own, which wins)")) && Won.Contains(TEXT("Bloom (Mantle Place Settings sets its own, which wins)"))
		&& Won.Contains(TEXT("Color Tint (Mantle Place Settings sets its own, which wins)")));
	TestTrue(TEXT("and says the sun is set there"), DetailsOf(Coverage, MantleCoverageCategory::Lights, EOutcome::Approximated).Contains(TEXT("2 times the game's sun")));

	// The file holds them, with the exposure as a bias alone, and the parser accepts it.
	const FString Json = FMantlePlaceExporter::ToJson(Level.Place);
	TSharedPtr<FJsonObject> Root;
	if (!TestTrue(TEXT("the file is JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) && Root.IsValid()))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* SettingsJson = nullptr;
	const TSharedPtr<FJsonObject>* EnvironmentJson = nullptr;
	if (!TestTrue(TEXT("with an environment block"),
		Root->TryGetObjectField(TEXT("settings"), SettingsJson) && (*SettingsJson)->TryGetObjectField(TEXT("environment"), EnvironmentJson)))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* ExposureJson = nullptr;
	if (TestTrue(TEXT("with an exposure"), (*EnvironmentJson)->TryGetObjectField(TEXT("exposure"), ExposureJson)))
	{
		TestEqual(TEXT("whose bias is -1.5"), (*ExposureJson)->GetNumberField(TEXT("bias")), -1.5);
		TestFalse(TEXT("and which names no mode"), (*ExposureJson)->HasField(TEXT("mode")));
	}
	TestEqual(TEXT("the saturation is the settings'"), (*EnvironmentJson)->GetNumberField(TEXT("saturation")), 0.5);
	TestEqual(TEXT("and the bloom"), (*EnvironmentJson)->GetNumberField(TEXT("bloom")), 0.25);
	const FMantleParseResult Parsed = FMantlePlaceParser::ParseFromString(Json);
	TestFalse(TEXT("the parser has no complaint about the environment block"),
		Parsed.Errors.ContainsByPredicate([](const FMantleParseError& Error) { return Error.Path.StartsWith(TEXT("/settings/environment")); }));
	TestTrue(TEXT("it reads the exposure back"), Parsed.Place.Settings.Environment.Exposure.IsSet() && Parsed.Place.Settings.Environment.Exposure->Bias == -1.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentExposureAndSunFieldsTest,
	"Mantle.Editor.EnvironmentExport.ExposureAndSunBrightnessFields",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentExposureAndSunFieldsTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentExportTestPrivate;
	AMantlePlaceSettingsActor* Settings = NewObject<AMantlePlaceSettingsActor>(GetTransientPackage());
	if (!TestNotNull(TEXT("a settings actor"), Settings))
	{
		return false;
	}
	// Both fields are off by default, and write nothing.
	TestFalse(TEXT("Exposure is off by default"), Settings->bSetExposure);
	TestFalse(TEXT("Sun Brightness is off by default"), Settings->bSetSunBrightness);
	TestFalse(TEXT("so no exposure is written"), Settings->MakeEnvironmentSettings().Exposure.IsSet());
	TestTrue(TEXT("and the settings are empty"), Settings->MakeEnvironmentSettings().IsEmpty());

	// Exposure: stops, -4 to +4. Zero is the game's own, so it writes nothing.
	Settings->bSetExposure = true;
	Settings->Exposure = 0.0;
	TestFalse(TEXT("an exposure of 0 writes nothing"), Settings->MakeEnvironmentSettings().Exposure.IsSet());
	Settings->Exposure = 1.25;
	TestEqual(TEXT("1.25 stops brighter is a bias of 1.25"), Settings->MakeEnvironmentSettings().Exposure->Bias, 1.25);
	TestTrue(TEXT("on the fixed exposure"), Settings->MakeEnvironmentSettings().Exposure->Mode == EMantleExposureMode::Fixed);
	Settings->Exposure = 9.0;
	TestEqual(TEXT("past +4 is held to it"), Settings->MakeEnvironmentSettings().Exposure->Bias, 4.0);
	Settings->Exposure = -9.0;
	TestEqual(TEXT("and past -4"), Settings->MakeEnvironmentSettings().Exposure->Bias, -4.0);
	Settings->bSetExposure = false;
	TestFalse(TEXT("turned off, a value is not written"), Settings->MakeEnvironmentSettings().Exposure.IsSet());

	// Importing a place: a fixed bias fills the field; an automatic exposure, which the field cannot say, leaves it off.
	FMantleEnvironmentSettings FromFile;
	FMantleExposureSettings Fixed;
	Fixed.Bias = -2.0;
	FromFile.Exposure = Fixed;
	Settings->SetEnvironmentFrom(FromFile);
	TestTrue(TEXT("a fixed bias imports into the field"), Settings->bSetExposure && Settings->Exposure == -2.0);
	FMantleExposureSettings Auto;
	Auto.Mode = EMantleExposureMode::Auto;
	FromFile.Exposure = Auto;
	Settings->SetEnvironmentFrom(FromFile);
	TestFalse(TEXT("an automatic exposure does not"), Settings->bSetExposure);

	// The fields' ranges and tooltips, in plain words.
	const FProperty* Exposure = AMantlePlaceSettingsActor::StaticClass()->FindPropertyByName(TEXT("Exposure"));
	const FProperty* SunBrightness = AMantlePlaceSettingsActor::StaticClass()->FindPropertyByName(TEXT("SunBrightness"));
	if (TestNotNull(TEXT("an Exposure property"), Exposure) && TestNotNull(TEXT("a Sun Brightness property"), SunBrightness))
	{
		TestEqual(TEXT("Exposure runs from -4"), Exposure->GetMetaData(TEXT("ClampMin")), FString(TEXT("-4")));
		TestEqual(TEXT("to 4"), Exposure->GetMetaData(TEXT("ClampMax")), FString(TEXT("4")));
		TestEqual(TEXT("Sun Brightness runs from 0"), SunBrightness->GetMetaData(TEXT("ClampMin")), FString(TEXT("0")));
		TestEqual(TEXT("to 4"), SunBrightness->GetMetaData(TEXT("ClampMax")), FString(TEXT("4")));
		TestTrue(TEXT("Exposure has a tooltip that says stops"), Exposure->GetToolTipText().ToString().Contains(TEXT("stops")));
		TestTrue(TEXT("Sun Brightness has one that says it is a multiple of the game's sun"), SunBrightness->GetToolTipText().ToString().Contains(TEXT("multiple of the game's own sun")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentSunBrightnessTest,
	"Mantle.Editor.EnvironmentExport.SunBrightnessScalesTheGamesSun",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentSunBrightnessTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentExportTestPrivate;
	FTestWorld Test;
	AMantlePlaceSettingsActor* Settings = Test.World ? Test.Spawn<AMantlePlaceSettingsActor>(TEXT("Settings")) : nullptr;
	FSunLevel Level;
	if (!TestNotNull(TEXT("place settings"), Settings) || !TestTrue(TEXT("a sun"), Level.Build(Test, FRotator(-46.0, 30.0, 0.0), /*bTemperature*/ false)))
	{
		return false;
	}
	const auto SunIntensityFor = [&](const double Multiple)
	{
		FMantleCoverage Coverage;
		Settings->bSetSunBrightness = true;
		Settings->SunBrightness = Multiple;
		FMantleEnvironmentExport::Apply(Test.World, Level.IdByActor, Level.Place, Coverage);
		return Level.Place.Entities[0].Props.TryGet<FMantleDirectionalLightProps>()->Intensity;
	};
	TestEqual(TEXT("1 is the game's sun"), SunIntensityFor(1.0), 3.5);
	TestEqual(TEXT("half is 1.75"), SunIntensityFor(0.5), 1.75);
	TestEqual(TEXT("4 times is 14"), SunIntensityFor(4.0), 14.0);
	TestEqual(TEXT("past 4 is held to it"), SunIntensityFor(9.0), 14.0);
	TestEqual(TEXT("0 is no sun"), SunIntensityFor(0.0), 0.0);
	Settings->bSetSunBrightness = false;
	Settings->SunBrightness = 3.0;
	FMantleCoverage OffCoverage;
	FMantleEnvironmentExport::Apply(Test.World, Level.IdByActor, Level.Place, OffCoverage);
	TestEqual(TEXT("turned off, the value is not used"), Level.Place.Entities[0].Props.TryGet<FMantleDirectionalLightProps>()->Intensity, 3.5);

	// With no sun in the level there is nothing to scale, and the report says so.
	FMantlePlace NoSun;
	TMap<const AActor*, FString> NoIds;
	FMantleCoverage NoSunCoverage;
	Settings->bSetSunBrightness = true;
	FMantleEnvironmentExport::Apply(Test.World, NoIds, NoSun, NoSunCoverage);
	TestEqual(TEXT("no sun entity is made up"), NoSun.Entities.Num(), 0);
	const FString Dropped = DetailsOf(NoSunCoverage, MantleCoverageCategory::Lights, EOutcome::Dropped);
	TestTrue(TEXT("and the report says Sun Brightness had no sun to apply to"), Dropped.Contains(TEXT("Sun Brightness")) && Dropped.Contains(TEXT("no Directional Light")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
