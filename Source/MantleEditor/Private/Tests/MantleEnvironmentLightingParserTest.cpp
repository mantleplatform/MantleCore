// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceParser.h"
#include "MantlePlaceTypes.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// settings.environment.skyLight and settings.environment.sun (#722), as the parser reads them. Only the environment block's
// errors are looked at: the rest of the place is not the test's business.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleEnvironmentLightingParserTestPrivate
{
	FMantleParseResult ParseEnvironment(const FString& EnvironmentBody)
	{
		return FMantlePlaceParser::ParseFromString(FString::Printf(TEXT("{ \"schemaVersion\": %d, \"settings\": { \"environment\": { %s } } }"), MANTLE_PLACE_SCHEMA_VERSION, *EnvironmentBody));
	}

	bool HasEnvironmentError(const FMantleParseResult& Result, const TCHAR* PathSuffix = nullptr)
	{
		return Result.Errors.ContainsByPredicate([PathSuffix](const FMantleParseError& Error)
		{
			return Error.Path.StartsWith(TEXT("/settings/environment")) && (!PathSuffix || Error.Path.EndsWith(PathSuffix));
		});
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEnvironmentLightingParserTest,
	"Mantle.Editor.EnvironmentExport.LightingParser",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEnvironmentLightingParserTest::RunTest(const FString& Parameters)
{
	using namespace MantleEnvironmentLightingParserTestPrivate;
	const FMantleParseResult Both = ParseEnvironment(TEXT("\"skyLight\": { \"intensity\": 0.25 }, \"sun\": { \"direct\": false }"));
	TestFalse(TEXT("both keys are accepted"), HasEnvironmentError(Both));
	TestTrue(TEXT("the strength is read"), Both.Place.Settings.Environment.SkyLightIntensity.IsSet() && *Both.Place.Settings.Environment.SkyLightIntensity == 0.25);
	TestTrue(TEXT("and no direct sun"), Both.Place.Settings.Environment.bNoDirectSun);
	TestFalse(TEXT("the block is not empty"), Both.Place.Settings.Environment.IsEmpty());

	const FMantleParseResult Direct = ParseEnvironment(TEXT("\"sun\": { \"direct\": true }"));
	TestFalse(TEXT("a sun that is direct is accepted"), HasEnvironmentError(Direct));
	TestFalse(TEXT("and is Mantle's own"), Direct.Place.Settings.Environment.bNoDirectSun);

	const FMantleParseResult Edge = ParseEnvironment(TEXT("\"skyLight\": { \"intensity\": 0 }"));
	TestTrue(TEXT("a strength of 0 is read"), !HasEnvironmentError(Edge) && Edge.Place.Settings.Environment.SkyLightIntensity.IsSet() && *Edge.Place.Settings.Environment.SkyLightIntensity == 0.0);

	TestTrue(TEXT("a strength past 1 is refused"), HasEnvironmentError(ParseEnvironment(TEXT("\"skyLight\": { \"intensity\": 1.5 }")), TEXT("intensity")));
	TestTrue(TEXT("and a negative one"), HasEnvironmentError(ParseEnvironment(TEXT("\"skyLight\": { \"intensity\": -0.1 }")), TEXT("intensity")));
	TestTrue(TEXT("and a string"), HasEnvironmentError(ParseEnvironment(TEXT("\"skyLight\": { \"intensity\": \"0.5\" }")), TEXT("intensity")));
	TestTrue(TEXT("a sun that is not a boolean is refused"), HasEnvironmentError(ParseEnvironment(TEXT("\"sun\": { \"direct\": \"no\" }")), TEXT("direct")));
	TestTrue(TEXT("an unknown key in skyLight is refused"), HasEnvironmentError(ParseEnvironment(TEXT("\"skyLight\": { \"colour\": 1 }"))));
	TestTrue(TEXT("an unknown key in sun is refused"), HasEnvironmentError(ParseEnvironment(TEXT("\"sun\": { \"angle\": 1 }"))));
	TestTrue(TEXT("a skyLight that is not an object is refused"), HasEnvironmentError(ParseEnvironment(TEXT("\"skyLight\": 1")), TEXT("skyLight")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
