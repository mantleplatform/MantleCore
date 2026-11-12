// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceParser.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// settings.health and settings.combat (#332): both off when left out, on with true, refused when not a boolean.
// Mirrors validate.py (the schema) and the backend's validate.go.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantlePlaceHealthCombatParseTestPrivate
{
	FMantleParseResult ParseSettings(const FString& Settings)
	{
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_healthcombat1\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Health and combat\",\"authorId\":\"usr_4b1d\"},\"settings\":%s,\"assets\":[],\"entities\":[]}"),
			*Settings);
		return FMantlePlaceParser::ParseFromString(Json);
	}

	bool HasErrorAt(const FMantleParseResult& Result, const FString& Path)
	{
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error) { return Error.Path == Path; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceHealthCombatParseTest,
	"Mantle.Runtime.Place.Parser.HealthAndCombatSettings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceHealthCombatParseTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceHealthCombatParseTestPrivate;

	// Left out: both off.
	const FMantleParseResult None = ParseSettings(TEXT("{}"));
	if (!TestTrue(TEXT("a place without health or combat parses"), None.bSuccess))
	{
		AddError(None.ErrorSummary());
		return false;
	}
	TestFalse(TEXT("health is off when left out"), None.Place.Settings.bHealth);
	TestFalse(TEXT("combat is off when left out"), None.Place.Settings.bCombat);

	// True: on, each on its own.
	const FMantleParseResult Both = ParseSettings(TEXT("{\"health\":true,\"combat\":true}"));
	if (!TestTrue(TEXT("health and combat true parse"), Both.bSuccess))
	{
		AddError(Both.ErrorSummary());
		return false;
	}
	TestTrue(TEXT("health true is on"), Both.Place.Settings.bHealth);
	TestTrue(TEXT("combat true is on"), Both.Place.Settings.bCombat);

	const FMantleParseResult HealthOnly = ParseSettings(TEXT("{\"health\":true}"));
	TestTrue(TEXT("health alone parses"), HealthOnly.bSuccess);
	TestTrue(TEXT("health alone is on"), HealthOnly.Place.Settings.bHealth);
	TestFalse(TEXT("combat stays off beside health"), HealthOnly.Place.Settings.bCombat);

	const FMantleParseResult CombatOnly = ParseSettings(TEXT("{\"combat\":true}"));
	TestTrue(TEXT("combat alone parses"), CombatOnly.bSuccess);
	TestTrue(TEXT("combat alone is on"), CombatOnly.Place.Settings.bCombat);
	TestFalse(TEXT("health stays off beside combat"), CombatOnly.Place.Settings.bHealth);

	// False: off, written out.
	const FMantleParseResult Off = ParseSettings(TEXT("{\"health\":false,\"combat\":false}"));
	TestTrue(TEXT("health and combat false parse"), Off.bSuccess);
	TestFalse(TEXT("health false is off"), Off.Place.Settings.bHealth);
	TestFalse(TEXT("combat false is off"), Off.Place.Settings.bCombat);

	// Not a boolean: refused at its own path.
	const FMantleParseResult BadHealth = ParseSettings(TEXT("{\"health\":\"on\"}"));
	TestFalse(TEXT("a string health is refused"), BadHealth.bSuccess);
	TestTrue(TEXT("the error is at /settings/health"), HasErrorAt(BadHealth, TEXT("/settings/health")));

	const FMantleParseResult BadCombat = ParseSettings(TEXT("{\"combat\":1}"));
	TestFalse(TEXT("a number combat is refused"), BadCombat.bSuccess);
	TestTrue(TEXT("the error is at /settings/combat"), HasErrorAt(BadCombat, TEXT("/settings/combat")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
