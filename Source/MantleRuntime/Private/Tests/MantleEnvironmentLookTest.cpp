// Copyright Joshua Eger. All Rights Reserved.
#include "MantleEnvironmentDefaults.h"
#include "MantleEnvironmentSubsystem.h"
#include "MantlePlaceParser.h"

#include "Engine/Scene.h"
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
		if (TestTrue(TEXT("inscattering colour"), Fog.InscatteringColor.IsSet()))
		{
			TestNearlyEqual(TEXT("its green"), Fog.InscatteringColor->G, 0.638f, 1.0e-4f);
			TestNearlyEqual(TEXT("its blue"), Fog.InscatteringColor->B, 1.0f, 1.0e-4f);
		}
		if (TestTrue(TEXT("directional inscattering colour"), Fog.DirectionalInscatteringColor.IsSet()))
		{
			TestNearlyEqual(TEXT("its blue"), Fog.DirectionalInscatteringColor->B, 0.125f, 1.0e-4f);
		}
	}

	// Each at its edge: a colour brighter than 1 is allowed, to 10 a channel.
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
	TestTrue(TEXT("a colour channel past 10"),
		RefusedAt(TEXT("\"fog\":{\"inscatteringColor\":[0.5,10.5,0.5]}"), Fog + TEXT("/inscatteringColor/1")));
	TestTrue(TEXT("a negative colour channel"),
		RefusedAt(TEXT("\"fog\":{\"inscatteringColor\":[-0.1,0.5,0.5]}"), Fog + TEXT("/inscatteringColor/0")));
	TestTrue(TEXT("a colour of two numbers"),
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
	constexpr float EngineBias = 1.0f;

	TestTrue(TEXT("no exposure changes nothing"), UMantleEnvironmentSubsystem::IsPlaceExposureNeutral(TOptional<FMantleExposureSettings>()));
	FMantleExposureSettings FixedPlain;
	TestTrue(TEXT("nor does fixed with no bias"), UMantleEnvironmentSubsystem::IsPlaceExposureNeutral(FixedPlain));
	FMantleExposureSettings FixedBiased;
	FixedBiased.Bias = 0.5;
	TestFalse(TEXT("fixed with a bias does"), UMantleEnvironmentSubsystem::IsPlaceExposureNeutral(FixedBiased));
	FMantleExposureSettings Auto;
	Auto.Mode = EMantleExposureMode::Auto;
	TestFalse(TEXT("and auto does"), UMantleEnvironmentSubsystem::IsPlaceExposureNeutral(Auto));

	// Fixed, no bias: nothing overridden, which is the game's own exposure as before.
	{
		FPostProcessSettings Settings;
		UMantleEnvironmentSubsystem::ApplyExposureSettings(Settings, 0.0f, TOptional<FMantleExposureSettings>(), EngineBias);
		TestFalse(TEXT("no place exposure overrides no compensation"), Settings.bOverride_AutoExposureBias);
		TestFalse(TEXT("nor the method"), Settings.bOverride_AutoExposureMethod);
		TestFalse(TEXT("nor the minimum"), Settings.bOverride_AutoExposureMinBrightness);
		TestFalse(TEXT("nor the maximum"), Settings.bOverride_AutoExposureMaxBrightness);
	}
	// A script's bias adds to the game's own exposure.
	{
		FPostProcessSettings Settings;
		UMantleEnvironmentSubsystem::ApplyExposureSettings(Settings, 0.5f, TOptional<FMantleExposureSettings>(), EngineBias);
		TestTrue(TEXT("a script's bias is overridden"), Settings.bOverride_AutoExposureBias);
		TestEqual(TEXT("the game's own plus the script's"), Settings.AutoExposureBias, EngineBias + 0.5f);
		TestFalse(TEXT("with the metering left alone"), Settings.bOverride_AutoExposureMethod);
	}
	// Fixed with a bias: the compensation only, the game's own plus the bias.
	{
		FPostProcessSettings Settings;
		UMantleEnvironmentSubsystem::ApplyExposureSettings(Settings, 0.0f, FixedBiased, EngineBias);
		TestTrue(TEXT("a fixed bias overrides the compensation"), Settings.bOverride_AutoExposureBias);
		TestEqual(TEXT("the game's own plus the bias"), Settings.AutoExposureBias, EngineBias + 0.5f);
		TestFalse(TEXT("and leaves the metering and range to the fixed base"),
			Settings.bOverride_AutoExposureMethod || Settings.bOverride_AutoExposureMinBrightness || Settings.bOverride_AutoExposureMaxBrightness);
	}
	// Auto with no range: the engine's default one, with histogram metering.
	{
		FPostProcessSettings Settings;
		UMantleEnvironmentSubsystem::ApplyExposureSettings(Settings, 0.0f, Auto, EngineBias);
		TestTrue(TEXT("auto overrides the metering"), Settings.bOverride_AutoExposureMethod);
		TestTrue(TEXT("as the histogram"), Settings.AutoExposureMethod == EAutoExposureMethod::AEM_Histogram);
		TestTrue(TEXT("and the range"), Settings.bOverride_AutoExposureMinBrightness && Settings.bOverride_AutoExposureMaxBrightness);
		TestEqual(TEXT("the default minimum"), static_cast<double>(Settings.AutoExposureMinBrightness), DefaultAutoExposureMin);
		TestEqual(TEXT("the default maximum"), static_cast<double>(Settings.AutoExposureMaxBrightness), DefaultAutoExposureMax);
		TestTrue(TEXT("and the compensation, the game's own with no bias"), Settings.bOverride_AutoExposureBias && Settings.AutoExposureBias == EngineBias);
	}
	// Auto with the place's range and bias; a script's bias replaces the place's.
	{
		FMantleExposureSettings Ranged = Auto;
		Ranged.Bias = -1.0;
		Ranged.MinBrightness = 0.5;
		FPostProcessSettings Settings;
		UMantleEnvironmentSubsystem::ApplyExposureSettings(Settings, 0.0f, Ranged, EngineBias);
		TestEqual(TEXT("the place's minimum"), Settings.AutoExposureMinBrightness, 0.5f);
		TestEqual(TEXT("an unset end is the default"), static_cast<double>(Settings.AutoExposureMaxBrightness), DefaultAutoExposureMax);
		TestEqual(TEXT("the bias is relative to the game's own"), Settings.AutoExposureBias, EngineBias - 1.0f);
		UMantleEnvironmentSubsystem::ApplyExposureSettings(Settings, 3.0f, Ranged, EngineBias);
		TestEqual(TEXT("a script's bias replaces the place's, on the game's own"), Settings.AutoExposureBias, EngineBias + 3.0f);
		TestTrue(TEXT("the range stays"), Settings.bOverride_AutoExposureMinBrightness && Settings.AutoExposureMinBrightness == 0.5f);
	}
	// The client's lift adds to a fixed exposure, whoever set it, and leaves an auto one alone.
	{
		FPostProcessSettings Settings;
		UMantleEnvironmentSubsystem::ApplyExposureSettings(Settings, 0.0f, TOptional<FMantleExposureSettings>(), EngineBias, 1.5f);
		TestTrue(TEXT("a lift overrides the game's own compensation"), Settings.bOverride_AutoExposureBias);
		TestEqual(TEXT("by the lift"), Settings.AutoExposureBias, EngineBias + 1.5f);
		TestFalse(TEXT("leaving the metering fixed"), Settings.bOverride_AutoExposureMethod);
		UMantleEnvironmentSubsystem::ApplyExposureSettings(Settings, 0.0f, FixedBiased, EngineBias, 1.5f);
		TestEqual(TEXT("on top of the place's bias"), Settings.AutoExposureBias, EngineBias + 2.0f);
		UMantleEnvironmentSubsystem::ApplyExposureSettings(Settings, 2.0f, TOptional<FMantleExposureSettings>(), EngineBias, 1.5f);
		TestEqual(TEXT("and of a script's"), Settings.AutoExposureBias, EngineBias + 3.5f);
		UMantleEnvironmentSubsystem::ApplyExposureSettings(Settings, 0.0f, Auto, EngineBias, 1.5f);
		TestEqual(TEXT("an auto exposure takes none"), Settings.AutoExposureBias, EngineBias);
	}
	// A minimum above the default maximum cannot make the range empty: that would be a fixed exposure.
	{
		FMantleExposureSettings High = Auto;
		High.MinBrightness = 12.0;
		FPostProcessSettings Settings;
		UMantleEnvironmentSubsystem::ApplyExposureSettings(Settings, 0.0f, High, EngineBias);
		TestTrue(TEXT("the maximum is at least the minimum"), Settings.AutoExposureMaxBrightness >= Settings.AutoExposureMinBrightness);
	}
	// A fixed exposure after an auto one hands the volume's overrides back.
	{
		FPostProcessSettings Settings;
		UMantleEnvironmentSubsystem::ApplyExposureSettings(Settings, 0.0f, Auto, EngineBias);
		UMantleEnvironmentSubsystem::ApplyExposureSettings(Settings, 0.0f, TOptional<FMantleExposureSettings>(), EngineBias);
		TestFalse(TEXT("the metering is no longer overridden"), Settings.bOverride_AutoExposureMethod);
		TestFalse(TEXT("nor the range"), Settings.bOverride_AutoExposureMinBrightness || Settings.bOverride_AutoExposureMaxBrightness);
		TestFalse(TEXT("nor the compensation"), Settings.bOverride_AutoExposureBias);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
