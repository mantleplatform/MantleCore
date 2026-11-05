// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceParser.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Placing mode's place format. settings.placing (persistence, maxPieces) and a StaticMesh asset's sockets.
// Mirrors validate.py (check_mesh_sockets and the schema) and the backend's validate.go.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantlePlacePlacingParseTestPrivate
{
	const TCHAR* const MeshId = TEXT("ast_u00000000000000000000000000000001");

	/** A place with no entities, Settings between the settings braces and Assets as its manifest (the text between the brackets). */
	FMantleParseResult ParsePlace(const FString& Settings, const FString& Assets)
	{
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_placing01\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Placing\",\"authorId\":\"usr_4b1d\"},\"settings\":{%s},\"assets\":[%s],\"entities\":[]}"),
			*Settings, *Assets);
		return FMantlePlaceParser::ParseFromString(Json);
	}

	FMantleParseResult ParseSettings(const FString& Placing)
	{
		return ParsePlace(FString::Printf(TEXT("\"placing\":%s"), *Placing), FString());
	}

	/** An asset of Kind whose sockets are the given JSON (the text after "sockets":), or none when Sockets is empty. */
	FMantleParseResult ParseSockets(const FString& Sockets, const FString& Kind = TEXT("StaticMesh"))
	{
		const FString Extra = Sockets.IsEmpty() ? FString() : FString::Printf(TEXT(",\"sockets\":%s"), *Sockets);
		return ParsePlace(FString(), FString::Printf(
			TEXT("{\"id\":\"%s\",\"kind\":\"%s\",\"hash\":\"sha256:%s\",\"bytes\":100%s}"), MeshId, *Kind, *FString::ChrN(64, TEXT('a')), *Extra));
	}

	bool HasErrorAt(const FMantleParseResult& Result, const FString& Path)
	{
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error) { return Error.Path == Path; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlacingSettingsParseTest,
	"Mantle.Runtime.Place.Parser.PlacingSettings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlacingSettingsParseTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlacePlacingParseTestPrivate;

	const FMantleParseResult Default = ParsePlace(FString(), FString());
	if (!TestTrue(TEXT("a place without settings.placing parses"), Default.bSuccess))
	{
		AddError(Default.ErrorSummary());
		return false;
	}
	TestTrue(TEXT("no persistence by default"), Default.Place.Settings.Placing.Persistence == EMantlePlacingPersistence::None);
	TestEqual(TEXT("a cap of 2000 by default"), Default.Place.Settings.Placing.MaxPieces, 2000);

	const FMantleParseResult Set = ParseSettings(TEXT("{\"persistence\":\"server\",\"maxPieces\":500}"));
	if (!TestTrue(TEXT("persistence server and a lower cap parse"), Set.bSuccess))
	{
		AddError(Set.ErrorSummary());
		return false;
	}
	TestTrue(TEXT("persistence server is read"), Set.Place.Settings.Placing.Persistence == EMantlePlacingPersistence::Server);
	TestEqual(TEXT("maxPieces is read"), Set.Place.Settings.Placing.MaxPieces, 500);

	TestTrue(TEXT("an empty block parses"), ParseSettings(TEXT("{}")).bSuccess);
	TestTrue(TEXT("maxPieces 1 parses"), ParseSettings(TEXT("{\"maxPieces\":1}")).bSuccess);
	TestTrue(TEXT("maxPieces 2000 parses"), ParseSettings(TEXT("{\"maxPieces\":2000}")).bSuccess);

	TestTrue(TEXT("persistence place is refused"), HasErrorAt(ParseSettings(TEXT("{\"persistence\":\"place\"}")), TEXT("/settings/placing/persistence")));
	TestTrue(TEXT("persistence is case-exact"), HasErrorAt(ParseSettings(TEXT("{\"persistence\":\"Server\"}")), TEXT("/settings/placing/persistence")));
	TestTrue(TEXT("maxPieces 0 is refused"), HasErrorAt(ParseSettings(TEXT("{\"maxPieces\":0}")), TEXT("/settings/placing/maxPieces")));
	TestTrue(TEXT("maxPieces 2001 is refused"), HasErrorAt(ParseSettings(TEXT("{\"maxPieces\":2001}")), TEXT("/settings/placing/maxPieces")));
	TestTrue(TEXT("a fractional maxPieces is refused"), HasErrorAt(ParseSettings(TEXT("{\"maxPieces\":10.5}")), TEXT("/settings/placing/maxPieces")));
	TestTrue(TEXT("a string maxPieces is refused"), HasErrorAt(ParseSettings(TEXT("{\"maxPieces\":\"100\"}")), TEXT("/settings/placing/maxPieces")));
	TestTrue(TEXT("an unknown key is refused"), HasErrorAt(ParseSettings(TEXT("{\"fog\":true}")), TEXT("/settings/placing/fog")));
	TestTrue(TEXT("a block that is not an object is refused"), HasErrorAt(ParseSettings(TEXT("\"server\"")), TEXT("/settings/placing")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleMeshSocketsParseTest,
	"Mantle.Runtime.Place.Parser.MeshSockets",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleMeshSocketsParseTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlacePlacingParseTestPrivate;

	const FMantleParseResult Good = ParseSockets(TEXT(
		"[{\"name\":\"bottom\",\"kind\":\"wall_bottom\",\"position\":[0,0,0],\"accepts\":[\"floor_edge\"]},"
		"{\"name\":\"side_right\",\"kind\":\"wall_side\",\"position\":[200,0,150.5],\"rotation\":[10,90,-20],\"accepts\":[\"wall_side\",\"door_frame\"]},"
		"{\"name\":\"marker\",\"kind\":\"marker\",\"position\":[-100000,100000,0]}]"));
	if (!TestTrue(TEXT("a mesh with sockets parses"), Good.bSuccess))
	{
		AddError(Good.ErrorSummary());
		return false;
	}
	const TArray<FMantlePlaceSocket>& Sockets = Good.Place.Assets[0].Sockets;
	if (!TestEqual(TEXT("three sockets are read"), Sockets.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("name"), Sockets[0].Name, FString(TEXT("bottom")));
	TestEqual(TEXT("kind"), Sockets[0].Kind, FString(TEXT("wall_bottom")));
	TestEqual(TEXT("one accepted kind"), Sockets[0].Accepts.Num(), 1);
	TestTrue(TEXT("rotation defaults to zero"), Sockets[0].Rotation.IsNearlyZero());
	TestEqual(TEXT("position is in cm"), Sockets[1].Position, FVector(200.0, 0.0, 150.5));
	TestEqual(TEXT("rotation is [pitch, yaw, roll]"), Sockets[1].Rotation, FRotator(10.0, 90.0, -20.0));
	TestEqual(TEXT("two accepted kinds"), Sockets[1].Accepts.Num(), 2);
	TestEqual(TEXT("the second accepted kind"), Sockets[1].Accepts[1], FString(TEXT("door_frame")));
	TestEqual(TEXT("accepts is optional"), Sockets[2].Accepts.Num(), 0);

	TestTrue(TEXT("a mesh without sockets parses"), ParseSockets(FString()).bSuccess);
	TestTrue(TEXT("an empty list parses"), ParseSockets(TEXT("[]")).bSuccess);

	TArray<FString> ThirtyThree;
	for (int32 I = 0; I < 33; ++I)
	{
		ThirtyThree.Add(FString::Printf(TEXT("{\"name\":\"s%d\",\"kind\":\"peg\",\"position\":[0,0,0]}"), I));
	}
	TestTrue(TEXT("33 sockets are refused"), HasErrorAt(ParseSockets(TEXT("[") + FString::Join(ThirtyThree, TEXT(",")) + TEXT("]")), TEXT("/assets/0/sockets")));
	ThirtyThree.RemoveAt(32);
	TestTrue(TEXT("32 sockets parse"), ParseSockets(TEXT("[") + FString::Join(ThirtyThree, TEXT(",")) + TEXT("]")).bSuccess);

	TestTrue(TEXT("sockets on a Material are refused"), HasErrorAt(
		ParseSockets(TEXT("[{\"name\":\"a\",\"kind\":\"k\",\"position\":[0,0,0]}]"), TEXT("Material")), TEXT("/assets/0/sockets")));
	TestTrue(TEXT("sockets that are not an array are refused"), HasErrorAt(ParseSockets(TEXT("{}")), TEXT("/assets/0/sockets")));
	TestTrue(TEXT("a duplicate name is refused"), HasErrorAt(ParseSockets(
		TEXT("[{\"name\":\"a\",\"kind\":\"k\",\"position\":[0,0,0]},{\"name\":\"a\",\"kind\":\"j\",\"position\":[0,0,0]}]")), TEXT("/assets/0/sockets/1/name")));
	TestTrue(TEXT("a socket that is not an object is refused"), HasErrorAt(ParseSockets(TEXT("[\"a\"]")), TEXT("/assets/0/sockets/0")));
	TestTrue(TEXT("an unknown socket key is refused"), HasErrorAt(ParseSockets(
		TEXT("[{\"name\":\"a\",\"kind\":\"k\",\"position\":[0,0,0],\"scale\":1}]")), TEXT("/assets/0/sockets/0/scale")));
	TestTrue(TEXT("a missing name is refused"), HasErrorAt(ParseSockets(TEXT("[{\"kind\":\"k\",\"position\":[0,0,0]}]")), TEXT("/assets/0/sockets/0/name")));
	TestTrue(TEXT("a missing kind is refused"), HasErrorAt(ParseSockets(TEXT("[{\"name\":\"a\",\"position\":[0,0,0]}]")), TEXT("/assets/0/sockets/0/kind")));
	TestTrue(TEXT("a missing position is refused"), HasErrorAt(ParseSockets(TEXT("[{\"name\":\"a\",\"kind\":\"k\"}]")), TEXT("/assets/0/sockets/0/position")));
	TestTrue(TEXT("a name with a space is refused"), HasErrorAt(ParseSockets(
		TEXT("[{\"name\":\"wall bottom\",\"kind\":\"k\",\"position\":[0,0,0]}]")), TEXT("/assets/0/sockets/0/name")));
	TestTrue(TEXT("a name of 33 characters is refused"), HasErrorAt(ParseSockets(
		TEXT("[{\"name\":\"") + FString::ChrN(33, TEXT('n')) + TEXT("\",\"kind\":\"k\",\"position\":[0,0,0]}]")), TEXT("/assets/0/sockets/0/name")));
	TestTrue(TEXT("an empty kind is refused"), HasErrorAt(ParseSockets(TEXT("[{\"name\":\"a\",\"kind\":\"\",\"position\":[0,0,0]}]")), TEXT("/assets/0/sockets/0/kind")));
	TestTrue(TEXT("a position of two numbers is refused"), HasErrorAt(ParseSockets(
		TEXT("[{\"name\":\"a\",\"kind\":\"k\",\"position\":[0,0]}]")), TEXT("/assets/0/sockets/0/position")));
	TestTrue(TEXT("a position past 100000 is refused"), HasErrorAt(ParseSockets(
		TEXT("[{\"name\":\"a\",\"kind\":\"k\",\"position\":[0,100001,0]}]")), TEXT("/assets/0/sockets/0/position/1")));
	TestTrue(TEXT("a position that is not a number is refused"), HasErrorAt(ParseSockets(
		TEXT("[{\"name\":\"a\",\"kind\":\"k\",\"position\":[0,0,\"up\"]}]")), TEXT("/assets/0/sockets/0/position/2")));
	TestTrue(TEXT("a rotation of four numbers is refused"), HasErrorAt(ParseSockets(
		TEXT("[{\"name\":\"a\",\"kind\":\"k\",\"position\":[0,0,0],\"rotation\":[0,0,0,0]}]")), TEXT("/assets/0/sockets/0/rotation")));
	TestTrue(TEXT("accepts that is not an array is refused"), HasErrorAt(ParseSockets(
		TEXT("[{\"name\":\"a\",\"kind\":\"k\",\"position\":[0,0,0],\"accepts\":\"j\"}]")), TEXT("/assets/0/sockets/0/accepts")));
	TestTrue(TEXT("nine accepted kinds are refused"), HasErrorAt(ParseSockets(
		TEXT("[{\"name\":\"a\",\"kind\":\"k\",\"position\":[0,0,0],\"accepts\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\"]}]")),
		TEXT("/assets/0/sockets/0/accepts")));
	TestTrue(TEXT("an accepted kind with a space is refused"), HasErrorAt(ParseSockets(
		TEXT("[{\"name\":\"a\",\"kind\":\"k\",\"position\":[0,0,0],\"accepts\":[\"floor edge\"]}]")), TEXT("/assets/0/sockets/0/accepts/0")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
