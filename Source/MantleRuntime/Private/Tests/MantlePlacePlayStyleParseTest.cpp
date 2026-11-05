// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceParser.h"
#include "MantlePlayStyle.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The place's play style. settings.playStyle (style, options) and the option rules in MantlePlayStyle.
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

	TestTrue(TEXT("an unknown style is refused"), HasErrorAt(ParsePlayStyle(TEXT("{\"style\":\"Platformer\"}")), TEXT("/settings/playStyle/style")));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaySideScrollSettingsParseTest,
	"Mantle.Runtime.Place.Parser.PlayStyleSideScroll",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaySideScrollSettingsParseTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlacePlayStyleParseTestPrivate;

	const FMantleParseResult Plain = ParsePlayStyle(TEXT("{\"style\":\"SideScroll\"}"));
	if (!TestTrue(TEXT("SideScroll with no options parses"), Plain.bSuccess))
	{
		AddError(Plain.ErrorSummary());
		return false;
	}
	TestTrue(TEXT("the style is read"), Plain.Place.Settings.PlayStyle.Style == EMantlePlayStyle::SideScroll);
	TestTrue(TEXT("the options are the defaults"), Plain.Place.Settings.PlayStyle.SideScroll == FMantleSideScrollOptions());

	const FMantleParseResult Full = ParsePlayStyle(TEXT(
		"{\"style\":\"SideScroll\",\"options\":{\"Path\":[[0,0,0],[4000,0,90],[4000,3000,0]],\"Distance\":1200,\"Height\":50,"
		"\"LookAhead\":300,\"FieldOfView\":90,\"Bounds\":{\"Min\":0,\"Max\":7000}}}"));
	if (!TestTrue(TEXT("every option parses"), Full.bSuccess))
	{
		AddError(Full.ErrorSummary());
		return false;
	}
	const FMantleSideScrollOptions& Options = Full.Place.Settings.PlayStyle.SideScroll;
	TestEqual(TEXT("three path points"), Options.Path.Num(), 3);
	TestEqual(TEXT("the path is flat"), Options.Path[1], FVector2D(4000.0, 0.0));
	TestEqual(TEXT("Distance"), Options.Distance, 1200.0f);
	TestEqual(TEXT("Height"), Options.Height, 50.0f);
	TestEqual(TEXT("LookAhead"), Options.LookAhead, 300.0f);
	TestEqual(TEXT("FieldOfView"), Options.FieldOfView, 90.0f);
	TestTrue(TEXT("Bounds"), Options.bHasBounds && Options.BoundsMin == 0.0f && Options.BoundsMax == 7000.0f);

	TestTrue(TEXT("empty options parse"), ParsePlayStyle(TEXT("{\"style\":\"SideScroll\",\"options\":{}}")).bSuccess);
	TestTrue(TEXT("a TopDown option is refused"), HasErrorAt(ParsePlayStyle(TEXT("{\"style\":\"SideScroll\",\"options\":{\"Pitch\":60}}")), TEXT("/settings/playStyle/options/Pitch")));
	TestTrue(TEXT("Distance 299 is refused"), HasErrorAt(ParsePlayStyle(TEXT("{\"style\":\"SideScroll\",\"options\":{\"Distance\":299}}")), TEXT("/settings/playStyle/options/Distance")));
	TestTrue(TEXT("Height -501 is refused"), HasErrorAt(ParsePlayStyle(TEXT("{\"style\":\"SideScroll\",\"options\":{\"Height\":-501}}")), TEXT("/settings/playStyle/options/Height")));
	TestTrue(TEXT("LookAhead 1001 is refused"), HasErrorAt(ParsePlayStyle(TEXT("{\"style\":\"SideScroll\",\"options\":{\"LookAhead\":1001}}")), TEXT("/settings/playStyle/options/LookAhead")));
	TestTrue(TEXT("FieldOfView 121 is refused"), HasErrorAt(ParsePlayStyle(TEXT("{\"style\":\"SideScroll\",\"options\":{\"FieldOfView\":121}}")), TEXT("/settings/playStyle/options/FieldOfView")));
	TestTrue(TEXT("a path of one point is refused"), HasErrorAt(ParsePlayStyle(TEXT("{\"style\":\"SideScroll\",\"options\":{\"Path\":[[0,0,0]]}}")), TEXT("/settings/playStyle/options/Path")));
	TestTrue(TEXT("a path that is not an array is refused"), HasErrorAt(ParsePlayStyle(TEXT("{\"style\":\"SideScroll\",\"options\":{\"Path\":\"east\"}}")), TEXT("/settings/playStyle/options/Path")));
	TestTrue(TEXT("a point of two numbers is refused"), ParsePlayStyle(TEXT("{\"style\":\"SideScroll\",\"options\":{\"Path\":[[0,0,0],[100,0]]}}")).Errors.Num() > 0);
	TestTrue(TEXT("a segment of 49 cm is refused"), HasErrorAt(ParsePlayStyle(TEXT("{\"style\":\"SideScroll\",\"options\":{\"Path\":[[0,0,0],[49,0,0]]}}")), TEXT("/settings/playStyle/options/Path")));
	TestTrue(TEXT("Bounds with no path are refused"), HasErrorAt(ParsePlayStyle(TEXT("{\"style\":\"SideScroll\",\"options\":{\"Bounds\":{\"Min\":0,\"Max\":100}}}")), TEXT("/settings/playStyle/options/Bounds")));
	TestTrue(TEXT("Bounds past the end are refused"), HasErrorAt(ParsePlayStyle(TEXT(
		"{\"style\":\"SideScroll\",\"options\":{\"Path\":[[0,0,0],[1000,0,0]],\"Bounds\":{\"Min\":0,\"Max\":1001}}}")), TEXT("/settings/playStyle/options/Bounds")));
	TestTrue(TEXT("Bounds with Min at Max are refused"), HasErrorAt(ParsePlayStyle(TEXT(
		"{\"style\":\"SideScroll\",\"options\":{\"Path\":[[0,0,0],[1000,0,0]],\"Bounds\":{\"Min\":300,\"Max\":300}}}")), TEXT("/settings/playStyle/options/Bounds")));
	TestTrue(TEXT("an unknown option is refused"), HasErrorAt(ParsePlayStyle(TEXT("{\"style\":\"SideScroll\",\"options\":{\"Zoom\":3}}")), TEXT("/settings/playStyle/options/Zoom")));

	// The edges the Go and Python checkers agree on (Bounds compared in double, with 0.01 cm of slack).
	TestTrue(TEXT("Bounds exactly the path's length parse"), ParsePlayStyle(TEXT(
		"{\"style\":\"SideScroll\",\"options\":{\"Path\":[[0,0,0],[1000,0,0]],\"Bounds\":{\"Min\":0,\"Max\":1000}}}")).bSuccess);
	TestTrue(TEXT("Bounds the length of a diagonal path, to the centimeter and a bit, parse"), ParsePlayStyle(TEXT(
		"{\"style\":\"SideScroll\",\"options\":{\"Path\":[[0,0,0],[3000,4000,0]],\"Bounds\":{\"Min\":0,\"Max\":5000.005}}}")).bSuccess);
	TestTrue(TEXT("Bounds 0.02 cm past it do not"), HasErrorAt(ParsePlayStyle(TEXT(
		"{\"style\":\"SideScroll\",\"options\":{\"Path\":[[0,0,0],[3000,4000,0]],\"Bounds\":{\"Min\":0,\"Max\":5000.02}}}")), TEXT("/settings/playStyle/options/Bounds")));
	TestTrue(TEXT("a segment of exactly 50 cm parses"), ParsePlayStyle(TEXT("{\"style\":\"SideScroll\",\"options\":{\"Path\":[[0,0,0],[50,0,0]]}}")).bSuccess);
	const auto LinePath = [](const int32 Count)
	{
		FString Points;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Points += FString::Printf(TEXT("%s[%d,0,0]"), Index > 0 ? TEXT(",") : TEXT(""), Index * 100);
		}
		return FString::Printf(TEXT("{\"style\":\"SideScroll\",\"options\":{\"Path\":[%s]}}"), *Points);
	};
	TestTrue(TEXT("64 points parse"), ParsePlayStyle(LinePath(64)).bSuccess);
	TestTrue(TEXT("65 points do not"), HasErrorAt(ParsePlayStyle(LinePath(65)), TEXT("/settings/playStyle/options/Path")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
