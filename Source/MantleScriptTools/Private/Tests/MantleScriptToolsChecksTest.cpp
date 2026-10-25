// Copyright Joshua Eger. All Rights Reserved.
#include "MantleLuauRuntime.h"
#include "MantlePocketPySourceCheck.h"
#include "MantleQuickJSRuntime.h"
#include "MantleRmlDocumentRules.h"

#include "Misc/AutomationTest.h"

THIRD_PARTY_INCLUDES_START
#include "MantlePocketPyHooks.h"
THIRD_PARTY_INCLUDES_END

#if WITH_DEV_AUTOMATION_TESTS

// The editor's compile-only checks, in MantleScriptTools so the plugin runs them without
// MantleScript or MantleRml. Each accepts good source and names the problem in bad source.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScriptToolsLuauCheckTest,
	"Mantle.ScriptTools.Checks.Luau",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScriptToolsLuauCheckTest::RunTest(const FString& Parameters)
{
	FMantleScriptError Error;
	TestTrue(TEXT("good Luau passes"), FMantleLuauRuntime::ValidateSource(TEXT("local x = 1\nreturn x"), Error));
	TestFalse(TEXT("broken Luau fails"), FMantleLuauRuntime::ValidateSource(TEXT("local = 1"), Error));
	TestTrue(TEXT("a compile error"), Error.Kind == EMantleScriptErrorKind::Compile);
	TestFalse(TEXT("with a message"), Error.Message.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScriptToolsJavaScriptCheckTest,
	"Mantle.ScriptTools.Checks.JavaScript",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScriptToolsJavaScriptCheckTest::RunTest(const FString& Parameters)
{
	FMantleScriptError Error;
	TestTrue(TEXT("a good module passes"), FMantleQuickJSRuntime::ValidateSource(TEXT("export const answer = 42;"), Error));
	TestFalse(TEXT("a broken module fails"), FMantleQuickJSRuntime::ValidateSource(TEXT("export const = ;"), Error));
	TestTrue(TEXT("a compile error"), Error.Kind == EMantleScriptErrorKind::Compile);
	TestFalse(TEXT("with a message"), Error.Message.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScriptToolsPythonCheckTest,
	"Mantle.ScriptTools.Checks.Python",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScriptToolsPythonCheckTest::RunTest(const FString& Parameters)
{
	int32 InUseBefore = 0;
	for (int32 Slot = 0; Slot < MANTLE_POCKETPY_MAX_VM; ++Slot)
	{
		InUseBefore += MantlePocketPy_SlotInUse[Slot] ? 1 : 0;
	}

	FString Error;
	TestTrue(TEXT("good Python passes"), MantlePocketPyCheckSource(TEXT("Scripts/Good.py"), TEXT("x = 1\nprint(x)\n"), Error));
	TestFalse(TEXT("broken Python fails"), MantlePocketPyCheckSource(TEXT("Scripts/Bad.py"), TEXT("def (:\n"), Error));
	TestFalse(TEXT("with a message"), Error.IsEmpty());
	// A second check after a failure still works: the scratch slot was reset and given back.
	TestTrue(TEXT("a later check passes"), MantlePocketPyCheckSource(TEXT("Scripts/Good.py"), TEXT("y = 2\n"), Error));

	int32 InUseAfter = 0;
	for (int32 Slot = 0; Slot < MANTLE_POCKETPY_MAX_VM; ++Slot)
	{
		InUseAfter += MantlePocketPy_SlotInUse[Slot] ? 1 : 0;
	}
	TestEqual(TEXT("checks leave no VM slot claimed"), InUseAfter, InUseBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleScriptToolsRmlTextRulesTest,
	"Mantle.ScriptTools.Checks.RmlTextRules",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleScriptToolsRmlTextRulesTest::RunTest(const FString& Parameters)
{
	FString Error;
	TMap<FString, FString> Good;
	Good.Add(TEXT("UI/Shop.rml"), TEXT("<rml><head></head><body><div>Shop</div></body></rml>"));
	TestTrue(TEXT("a plain document passes"), MantleRmlDocumentRules::CheckFileSet(Good, Error));

	TMap<FString, FString> Script;
	Script.Add(TEXT("UI/Shop.rml"), TEXT("<rml><body><script>x</script></body></rml>"));
	TestFalse(TEXT("<script> is refused"), MantleRmlDocumentRules::CheckFileSet(Script, Error));
	TestTrue(TEXT("naming the file"), Error.Contains(TEXT("UI/Shop.rml")));

	FString Deep = TEXT("<rml><body>");
	for (int32 Level = 0; Level <= MantleRmlDocumentRules::MaxNestingDepth; ++Level)
	{
		Deep += TEXT("<div>");
	}
	TestTrue(TEXT("nesting depth is measured"), MantleRmlDocumentRules::MaxTagDepth(Deep) > MantleRmlDocumentRules::MaxNestingDepth);
	return true;
}

#endif
