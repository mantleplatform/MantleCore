// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePlaceParser.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// A static mesh's Nanite file in the place manifest. A StaticMesh entry names a Nanite entry of the same manifest (nanite), and
// the Nanite entry says the engine build it was baked for (engine). Mirrors validate.py, the backend (places/nanite.go) and the Lumen link.

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantlePlaceNaniteParseTestPrivate
{
	const TCHAR* const MeshId = TEXT("ast_u00000000000000000000000000000001");
	const TCHAR* const LumenId = TEXT("ast_u00000000000000000000000000000002");
	const TCHAR* const NaniteId = TEXT("ast_u00000000000000000000000000000003");
	const TCHAR* const Engine = TEXT("5.8.0-1+++UE5+Release-5.8");

	/** A place with no entities and Assets as its manifest (the text between the brackets). */
	FMantleParseResult ParseAssets(const FString& Assets)
	{
		const FString Json = FString::Printf(
			TEXT("{\"schemaVersion\":1,\"placeId\":\"plc_nanite001\",\"revision\":{\"number\":1,\"state\":\"draft\"},"
				 "\"metadata\":{\"name\":\"Nanite\",\"authorId\":\"usr_4b1d\"},\"settings\":{},\"assets\":[%s],\"entities\":[]}"),
			*Assets);
		return FMantlePlaceParser::ParseFromString(Json);
	}

	FString Mesh(const FString& Extra)
	{
		return FString::Printf(TEXT("{\"id\":\"%s\",\"kind\":\"StaticMesh\",\"hash\":\"sha256:%s\",\"bytes\":100%s}"), MeshId, *FString::ChrN(64, TEXT('a')), *Extra);
	}

	FString Side(const TCHAR* Id, const FString& Kind, const FString& Extra)
	{
		return FString::Printf(TEXT("{\"id\":\"%s\",\"kind\":\"%s\",\"hash\":\"sha256:%s\",\"bytes\":50%s}"), Id, *Kind, *FString::ChrN(64, TEXT('b')), *Extra);
	}

	bool HasErrorAt(const FMantleParseResult& Result, const FString& Path)
	{
		return Result.Errors.ContainsByPredicate([&Path](const FMantleParseError& Error) { return Error.Path == Path; });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePlaceNaniteParseTest,
	"Mantle.Runtime.Place.Parser.NaniteLink",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePlaceNaniteParseTest::RunTest(const FString& Parameters)
{
	using namespace MantlePlaceNaniteParseTestPrivate;

	const FString Linked = FString::Printf(TEXT(",\"nanite\":\"%s\""), NaniteId);
	const FString Engine_ = FString::Printf(TEXT(",\"engine\":\"%s\""), Engine);
	const FMantleParseResult Good = ParseAssets(Mesh(Linked) + TEXT(",") + Side(NaniteId, TEXT("Nanite"), Engine_));
	if (!TestTrue(TEXT("a mesh naming its Nanite entry parses"), Good.bSuccess))
	{
		AddError(Good.ErrorSummary());
		return false;
	}
	TestEqual(TEXT("the mesh keeps its link"), Good.Place.Assets[0].Nanite, FString(NaniteId));
	TestTrue(TEXT("the Nanite entry is a Nanite"), Good.Place.Assets[1].Kind == EMantleAssetKind::Nanite);
	TestEqual(TEXT("the Nanite entry keeps its engine"), Good.Place.Assets[1].Engine, FString(Engine));

	// A mesh may name both side files, each held to its own kind.
	const FString Both = FString::Printf(TEXT(",\"lumen\":\"%s\",\"nanite\":\"%s\""), LumenId, NaniteId);
	const FMantleParseResult WithBoth = ParseAssets(Mesh(Both) + TEXT(",") + Side(LumenId, TEXT("Lumen"), Engine_) + TEXT(",") + Side(NaniteId, TEXT("Nanite"), Engine_));
	TestTrue(TEXT("a mesh with a Lumen and a Nanite file parses"), WithBoth.bSuccess);
	if (WithBoth.bSuccess)
	{
		TestEqual(TEXT("its Lumen link"), WithBoth.Place.Assets[0].Lumen, FString(LumenId));
		TestEqual(TEXT("its Nanite link"), WithBoth.Place.Assets[0].Nanite, FString(NaniteId));
	}

	TestTrue(TEXT("a mesh without one parses"), ParseAssets(Mesh(TEXT(""))).bSuccess);
	TestTrue(TEXT("a Nanite entry must say its engine"), HasErrorAt(ParseAssets(Mesh(Linked) + TEXT(",") + Side(NaniteId, TEXT("Nanite"), TEXT(""))), TEXT("/assets/1/engine")));
	TestTrue(TEXT("nanite must name a listed entry"), HasErrorAt(ParseAssets(Mesh(Linked)), TEXT("/assets/0/nanite")));
	TestTrue(TEXT("nanite must name a Nanite entry (not a Lumen one)"),
		HasErrorAt(ParseAssets(Mesh(Linked) + TEXT(",") + Side(NaniteId, TEXT("Lumen"), Engine_)), TEXT("/assets/0/nanite")));
	TestTrue(TEXT("nanite must name an uploaded asset id"),
		HasErrorAt(ParseAssets(Mesh(TEXT(",\"nanite\":\"ast_cube\"")) + TEXT(",") + Side(TEXT("ast_cube"), TEXT("Nanite"), Engine_)), TEXT("/assets/0/nanite")));
	TestTrue(TEXT("only a StaticMesh names a Nanite file"), HasErrorAt(ParseAssets(Side(NaniteId, TEXT("Nanite"), Engine_ + Linked)), TEXT("/assets/0/nanite")));
	TestTrue(TEXT("only a Lumen or Nanite entry says an engine"), HasErrorAt(ParseAssets(Mesh(Engine_)), TEXT("/assets/0/engine")));
	TestFalse(TEXT("an unknown kind next to it is still refused"), ParseAssets(Side(NaniteId, TEXT("Nanites"), Engine_)).bSuccess);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
