// Copyright Joshua Eger. All Rights Reserved.
#include "Graph/MantleGraphCatalogue.h"
#include "MantleApiManifest.h"
#include "MantleScriptApiUsage.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The editor's view of the script API comes from the plugin's committed manifest
// (Resources/ScriptApi/mantle_api.json), not MantleScript's live registry.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleApiManifestReadTest,
	"Mantle.Editor.ApiManifest.ReadsThePluginCopy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleApiManifestReadTest::RunTest(const FString& Parameters)
{
	const FMantleApiManifest& Api = FMantleApiManifest::Get();
	TestEqual(TEXT("script API major"), Api.Major, 1);
	TestTrue(TEXT("a minor past 1.0"), Api.Minor > 0);
	TestTrue(TEXT("classes are listed"), Api.Classes.Num() > 10);

	const FMantleApiManifestClass* Avatar = Api.FindClass(TEXT("avatar"));
	if (TestNotNull(TEXT("Avatar, found ignoring case"), Avatar))
	{
		TestTrue(TEXT("a service"), Avatar->bService);
		TestEqual(TEXT("added in 1.2"), Avatar->IntroducedMinor, 2);
	}
	const FMantleApiManifestClass* Entity = Api.FindClass(TEXT("Entity"));
	if (TestNotNull(TEXT("Entity"), Entity))
	{
		TestEqual(TEXT("a base member is 1.0"), Entity->MemberIntroducedMinor(TEXT("Destroy")), 0);
		TestEqual(TEXT("an unknown member"), Entity->MemberIntroducedMinor(TEXT("NoSuchMember")), INDEX_NONE);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleApiManifestUsersTest,
	"Mantle.Editor.ApiManifest.GraphCatalogueAndApiNeed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleApiManifestUsersTest::RunTest(const FString& Parameters)
{
	const TArray<FMantleGraphNodeDef>& Nodes = FMantleGraphCatalogue::All();
	TestTrue(TEXT("Entity.Destroy is a graph node"),
		Nodes.ContainsByPredicate([](const FMantleGraphNodeDef& Def) { return Def.Dotted == TEXT("Entity.Destroy"); }));
	TestTrue(TEXT("a settable property gets a Set node"),
		Nodes.ContainsByPredicate([](const FMantleGraphNodeDef& Def) { return Def.Category == EMantleGraphNodeCategory::Set; }));

	const FMantleScriptApiNeed Base = FMantleScriptApiUsage::ForSource(TEXT("local x = 1\nprint(x)\n"), EMantleScriptCatalogLanguage::Luau);
	TestEqual(TEXT("a base script needs 1.0"), Base.Minor, 0);
	const FMantleScriptApiNeed Newer = FMantleScriptApiUsage::ForSource(TEXT("local id = Server.PrivateServerId\n"), EMantleScriptCatalogLanguage::Luau);
	TestTrue(TEXT("the Server service needs 1.3 or later"), Newer.Minor >= 3);
	TestFalse(TEXT("naming what needed it"), Newer.NeededFor.IsEmpty());
	return true;
}

#endif
