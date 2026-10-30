// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceParser.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// #149: a static mesh's Lumen side file in the place manifest. A StaticMesh entry names a Lumen entry of the same
// manifest (lumen), and the Lumen entry says the engine it was baked by (engine). Mirrors validate.py and the backend.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantlePlaceLumenParseTestPrivate
{
	const TCHAR* const MeshId = TEXT("ast_u00000000000000000000000000000001");
	const TCHAR* const LumenId = TEXT("ast_u00000000000000000000000000000002");

	/** A place with no entities and Assets as its manifest (the text between the brackets). */
	FMantleParseResult ParseAssets(const FString& Assets)
	{
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_lumen001\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Lumen\",\"authorId\":\"usr_4b1d\"},\"settings\":{},\"assets\":[%s],\"entities\":[]}"),
			*Assets);
		return FMantlePlaceParser::ParseFromString(Json);
	}

	FString Mesh(const FString& Extra)
	{
		return FString::Printf(TEXT("{\"id\":\"%s\",\"kind\":\"StaticMesh\",\"hash\":\"sha256:%s\",\"bytes\":100%s}"), MeshId, *FString::ChrN(64, TEXT('a')), *Extra);
	}

	FString Lumen(const FString& Kind, const FString& Extra)
	{
		return FString::Printf(TEXT("{\"id\":\"%s\",\"kind\":\"%s\",\"hash\":\"sha256:%s\",\"bytes\":50%s}"), LumenId, *Kind, *FString::ChrN(64, TEXT('b')), *Extra);
	}

	bool HasErrorAt(const FMantleParseResult& Result, const FString& Path)
	{
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error) { return Error.Path == Path; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceLumenParseTest,
	"Mantle.Runtime.Place.Parser.LumenLink",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceLumenParseTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceLumenParseTestPrivate;

	const FString Linked = FString::Printf(TEXT(",\"lumen\":\"%s\""), LumenId);
	const FString Engine = TEXT(",\"engine\":\"5.8.0-1+++UE5+Release-5.8\"");
	const FMantleParseResult Good = ParseAssets(Mesh(Linked) + TEXT(",") + Lumen(TEXT("Lumen"), Engine));
	if (!TestTrue(TEXT("a mesh naming its Lumen entry parses"), Good.bSuccess))
	{
		AddError(Good.ErrorSummary());
		return false;
	}
	TestEqual(TEXT("the mesh keeps its link"), Good.Place.Assets[0].Lumen, FString(LumenId));
	TestTrue(TEXT("the Lumen entry is a Lumen"), Good.Place.Assets[1].Kind == EMantleAssetKind::Lumen);
	TestEqual(TEXT("the Lumen entry keeps its engine"), Good.Place.Assets[1].Engine, FString(TEXT("5.8.0-1+++UE5+Release-5.8")));

	TestTrue(TEXT("a mesh without one parses"), ParseAssets(Mesh(TEXT(""))).bSuccess);
	TestTrue(TEXT("a Lumen entry must say its engine"), HasErrorAt(ParseAssets(Mesh(Linked) + TEXT(",") + Lumen(TEXT("Lumen"), TEXT(""))), TEXT("/assets/1/engine")));
	TestTrue(TEXT("lumen must name a listed entry"), HasErrorAt(ParseAssets(Mesh(Linked)), TEXT("/assets/0/lumen")));
	TestTrue(TEXT("lumen must name a Lumen entry"), HasErrorAt(ParseAssets(Mesh(Linked) + TEXT(",") + Lumen(TEXT("Texture"), TEXT(""))), TEXT("/assets/0/lumen")));
	TestTrue(TEXT("only a StaticMesh names a Lumen file"), HasErrorAt(ParseAssets(Lumen(TEXT("Lumen"), Engine + Linked)), TEXT("/assets/0/lumen")));
	TestTrue(TEXT("only a Lumen entry says an engine"), HasErrorAt(ParseAssets(Mesh(Engine)), TEXT("/assets/0/engine")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
