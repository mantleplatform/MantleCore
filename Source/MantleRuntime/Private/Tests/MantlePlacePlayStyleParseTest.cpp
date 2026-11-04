// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceParser.h"
#include "MantlePlayStyle.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// #232: the place's play style. settings.playStyle (style, options) and the option rules in MantlePlayStyle.
// Mirrors validate.py (check_play_style and the schema) and the backend's validate.go.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantlePlacePlayStyleParseTestPrivate
{
	FMantleParseResult ParsePlayStyle(const FString& PlayStyle)
	{
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_playstyle1\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Play style\",\"authorId\":\"usr_4b1d\"},\"settings\":{\"playStyle\":%s},\"assets\":[],\"entities\":[]}"),
			*PlayStyle);
		return FMantlePlaceParser::ParseFromString(Json);
	}

	bool HasErrorAt(const FMantleParseResult& Result, const FString& Path)
	{
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error) { return Error.Path == Path; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlayStyleSettingsParseTest,
	"Mantle.Runtime.Place.Parser.PlayStyleSettings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlayStyleSettingsParseTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlacePlayStyleParseTestPrivate;

	// A place without the key stays as it was: Default.
	const FMantleParseResult None = FMantlePlaceParser::ParseFromString(
		TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_playstyle1\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
			 "\"metadata\":{\"name\":\"Play style\",\"authorId\":\"usr_4b1d\"},\"settings\":{},\"assets\":[],\"entities\":[]}"));
	if (!TestTrue(TEXT("a place without settings.playStyle parses"), None.bSuccess))
	{
		AddError(None.ErrorSummary());
		return false;
	}
	TestTrue(TEXT("no play style means Default"), None.Place.Settings.PlayStyle.Style == EMantlePlayStyle::Default);

	const FMantleParseResult Top = ParsePlayStyle(TEXT("{\"style\":\"TopDown\"}"));
	if (!TestTrue(TEXT("TopDown with no options parses"), Top.bSuccess))
	{
		AddError(Top.ErrorSummary());
		return false;
	}
	TestTrue(TEXT("the style is read"), Top.Place.Settings.PlayStyle.Style == EMantlePlayStyle::TopDown);
	TestTrue(TEXT("the options are the defaults"), Top.Place.Settings.PlayStyle.TopDown == FMantleTopDownOptions());

	const FMantleParseResult Set = ParsePlayStyle(TEXT(
		"{\"style\":\"TopDown\",\"options\":{\"Distance\":2000,\"MinDistance\":1000,\"MaxDistance\":3000,\"Pitch\":60,\"Yaw\":450,"
		"\"FieldOfView\":70,\"ClickToMove\":true,\"HideWalls\":false}}"));
	if (!TestTrue(TEXT("every option parses"), Set.bSuccess))
	{
		AddError(Set.ErrorSummary());
		return false;
	}
	const FMantleTopDownOptions& Options = Set.Place.Settings.PlayStyle.TopDown;
	TestEqual(TEXT("Distance"), Options.Distance, 2000.0f);
	TestEqual(TEXT("MinDistance"), Options.MinDistance, 1000.0f);
	TestEqual(TEXT("MaxDistance"), Options.MaxDistance, 3000.0f);
	TestEqual(TEXT("Pitch"), Options.Pitch, 60.0f);
	TestEqual(TEXT("Yaw is wrapped"), Options.Yaw, 90.0f);
	TestEqual(TEXT("FieldOfView"), Options.FieldOfView, 70.0f);
	TestTrue(TEXT("ClickToMove"), Options.bClickToMove);
	TestFalse(TEXT("HideWalls"), Options.bHideWalls);

	TestTrue(TEXT("Default parses"), ParsePlayStyle(TEXT("{\"style\":\"Default\"}")).bSuccess);
	TestTrue(TEXT("an empty block parses"), ParsePlayStyle(TEXT("{}")).bSuccess);
	TestTrue(TEXT("a locked zoom parses"), ParsePlayStyle(TEXT("{\"style\":\"TopDown\",\"options\":{\"Distance\":900,\"MinDistance\":900,\"MaxDistance\":900}}")).bSuccess);

	TestTrue(TEXT("an unknown style is refused"), HasErrorAt(ParsePlayStyle(TEXT("{\"style\":\"SideScroll\"}")), TEXT("/settings/playStyle/style")));
	TestTrue(TEXT("a style is case-exact"), HasErrorAt(ParsePlayStyle(TEXT("{\"style\":\"topdown\"}")), TEXT("/settings/playStyle/style")));
	TestTrue(TEXT("options on Default are refused"), HasErrorAt(ParsePlayStyle(TEXT("{\"options\":{\"Pitch\":60}}")), TEXT("/settings/playStyle/options")));
	TestTrue(TEXT("an unknown option is refused"), HasErrorAt(ParsePlayStyle(TEXT("{\"style\":\"TopDown\",\"options\":{\"Zoom\":3}}")), TEXT("/settings/playStyle/options/Zoom")));
	TestTrue(TEXT("Pitch 90 is refused"), HasErrorAt(ParsePlayStyle(TEXT("{\"style\":\"TopDown\",\"options\":{\"Pitch\":90}}")), TEXT("/settings/playStyle/options/Pitch")));
	TestTrue(TEXT("Pitch 10 is refused"), HasErrorAt(ParsePlayStyle(TEXT("{\"style\":\"TopDown\",\"options\":{\"Pitch\":10}}")), TEXT("/settings/playStyle/options/Pitch")));
	TestTrue(TEXT("FieldOfView 120 is refused"), HasErrorAt(ParsePlayStyle(TEXT("{\"style\":\"TopDown\",\"options\":{\"FieldOfView\":120}}")), TEXT("/settings/playStyle/options/FieldOfView")));
	TestTrue(TEXT("a string Yaw is refused"), HasErrorAt(ParsePlayStyle(TEXT("{\"style\":\"TopDown\",\"options\":{\"Yaw\":\"north\"}}")), TEXT("/settings/playStyle/options/Yaw")));
	TestTrue(TEXT("a number ClickToMove is refused"), HasErrorAt(ParsePlayStyle(TEXT("{\"style\":\"TopDown\",\"options\":{\"ClickToMove\":1}}")), TEXT("/settings/playStyle/options/ClickToMove")));
	TestTrue(TEXT("Distance above MaxDistance is refused"), HasErrorAt(ParsePlayStyle(TEXT("{\"style\":\"TopDown\",\"options\":{\"Distance\":3000}}")), TEXT("/settings/playStyle/options/Distance")));
	TestTrue(TEXT("MinDistance above MaxDistance is refused"),
		HasErrorAt(ParsePlayStyle(TEXT("{\"style\":\"TopDown\",\"options\":{\"MinDistance\":3000,\"MaxDistance\":2000,\"Distance\":2500}}")), TEXT("/settings/playStyle/options/MinDistance")));
	TestTrue(TEXT("an unknown key is refused"), HasErrorAt(ParsePlayStyle(TEXT("{\"fog\":true}")), TEXT("/settings/playStyle/fog")));
	TestTrue(TEXT("a block that is not an object is refused"), HasErrorAt(ParsePlayStyle(TEXT("\"TopDown\"")), TEXT("/settings/playStyle")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
