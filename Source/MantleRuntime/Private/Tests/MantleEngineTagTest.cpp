// Copyright Joshua Eger. All Rights Reserved.
#include "MantleEngineTag.h"
#include "MantleNaniteSideFile.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// The Nanite and Lumen side files carry the engine release they were baked by. The editor from Epic's launcher says "5.8.3-58210709"
// and a source build (the release builder's engine) says "5.8.3-0": the same release, so the shipped game must take the files.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleEngineTagTest,
	"Mantle.Runtime.StaticMesh.EngineTag",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleEngineTagTest::RunTest(const FString& Parameters)
{
	// The two builds of 5.8.3 (the builder's log says 5.8.3-0, the launcher's Build.version says changelist 58210709).
	TestTrue(TEXT("the builder's engine takes the launcher's file"), MantleEngineTag::Matches(TEXT("5.8.3-58210709"), TEXT("5.8.3-0")));
	TestTrue(TEXT("the launcher's engine takes the builder's file"), MantleEngineTag::Matches(TEXT("5.8.3-0"), TEXT("5.8.3-58210709")));
	TestTrue(TEXT("a tag with no changelist takes the old tag"), MantleEngineTag::Matches(TEXT("5.8.3-58210709"), TEXT("5.8.3")));
	TestTrue(TEXT("the same tag"), MantleEngineTag::Matches(TEXT("5.8.3"), TEXT("5.8.3")));

	// Another release is refused, in each part.
	TestFalse(TEXT("an older patch"), MantleEngineTag::Matches(TEXT("5.8.2-58210709"), TEXT("5.8.3-0")));
	TestFalse(TEXT("a newer patch"), MantleEngineTag::Matches(TEXT("5.8.4-0"), TEXT("5.8.3-0")));
	TestFalse(TEXT("another minor"), MantleEngineTag::Matches(TEXT("5.9.0-0"), TEXT("5.8.3-0")));
	TestFalse(TEXT("another major"), MantleEngineTag::Matches(TEXT("4.8.3-0"), TEXT("5.8.3-0")));
	TestFalse(TEXT("parts are numbers, not text: 5.8.30 is not 5.8.3"), MantleEngineTag::Matches(TEXT("5.8.30"), TEXT("5.8.3")));
	TestFalse(TEXT("5.18.3 is not 5.8.3"), MantleEngineTag::Matches(TEXT("5.18.3"), TEXT("5.8.3")));

	// Anything that is not a tag is refused, on either side.
	const TCHAR* const Malformed[] = {
		TEXT(""), TEXT("-"), TEXT("5.8"), TEXT("5.8.3.1"), TEXT("5..3"), TEXT(".8.3"), TEXT("5.8."), TEXT("5.8.3-"), TEXT("5.8.3-x"), TEXT("5.8.3-12a"),
		TEXT("5.8.3x"), TEXT("5.8.3-other"), TEXT("5.8.3 "), TEXT("v5.8.3"), TEXT("5.-8.3"), TEXT("5.8.3-1-2"), TEXT("5.8.3-0+UE5"), TEXT("garbage"),
	};
	for (const TCHAR* Tag : Malformed)
	{
		int32 Major = 0;
		int32 Minor = 0;
		int32 Patch = 0;
		TestFalse(*FString::Printf(TEXT("'%s' is not a tag"), Tag), MantleEngineTag::Parse(Tag, Major, Minor, Patch));
		TestFalse(*FString::Printf(TEXT("'%s' as a file's tag"), Tag), MantleEngineTag::Matches(Tag, TEXT("5.8.3-0")));
		TestFalse(*FString::Printf(TEXT("'%s' as this engine's tag"), Tag), MantleEngineTag::Matches(TEXT("5.8.3-0"), Tag));
	}

	int32 Major = 0;
	int32 Minor = 0;
	int32 Patch = 0;
	if (TestTrue(TEXT("a tag parses"), MantleEngineTag::Parse(TEXT("5.8.3-58210709"), Major, Minor, Patch)))
	{
		TestEqual(TEXT("major"), Major, 5);
		TestEqual(TEXT("minor"), Minor, 8);
		TestEqual(TEXT("patch"), Patch, 3);
	}

	// This engine's own tag is a release, with no changelist, and matches itself and its own changelist form.
	const FString Current = MantleEngineTag::Current();
	TestTrue(TEXT("this engine's tag parses"), MantleEngineTag::Parse(Current, Major, Minor, Patch));
	TestFalse(TEXT("this engine's tag has no changelist"), Current.Contains(TEXT("-")));
	TestEqual(TEXT("the side files use it"), FMantleNaniteSideFile::EngineTag(), Current);
	TestTrue(TEXT("this engine matches itself"), MantleEngineTag::Matches(Current, Current));
	TestTrue(TEXT("this engine matches a file baked with any changelist"), MantleEngineTag::Matches(Current + TEXT("-58210709"), Current));
	TestTrue(TEXT("this engine matches a file baked with changelist 0"), MantleEngineTag::Matches(Current + TEXT("-0"), Current));

	// The manifest decides the download from the tags alone.
	TestTrue(TEXT("a file the launcher baked is fetched by the builder's client"), FMantleNaniteSideFile::ShouldFetch(true, TEXT("5.8.3-58210709"), TEXT("5.8.3")));
	TestFalse(TEXT("a machine that cannot run Nanite never fetches"), FMantleNaniteSideFile::ShouldFetch(false, TEXT("5.8.3-58210709"), TEXT("5.8.3")));
	TestFalse(TEXT("another release is never fetched"), FMantleNaniteSideFile::ShouldFetch(true, TEXT("5.8.2-58210709"), TEXT("5.8.3")));
	TestFalse(TEXT("an entry with no engine is never fetched"), FMantleNaniteSideFile::ShouldFetch(true, FString(), TEXT("5.8.3")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
