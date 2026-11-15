// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePreviewLaunch.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Where a preview may run and where a client's place comes from. A preview is the creator's own test of a place: it runs standalone, on a dedicated server, and on a client
// connected to one (which plays the manifest's place, so a place that streams its cells can be tested the way players get it). Nothing about a normal player changes.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePreviewLaunchNetModeTest,
	"Mantle.Runtime.Preview.NetModes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePreviewLaunchNetModeTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("a standalone world may open a preview"), MantlePreviewLaunch::AllowsNetMode(NM_Standalone));
	TestTrue(TEXT("a dedicated server may"), MantlePreviewLaunch::AllowsNetMode(NM_DedicatedServer));
	TestTrue(TEXT("a client of one may"), MantlePreviewLaunch::AllowsNetMode(NM_Client));
	TestFalse(TEXT("a listen server may not: a preview never hosts other people's machines"), MantlePreviewLaunch::AllowsNetMode(NM_ListenServer));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantlePreviewLaunchClientSourceTest,
	"Mantle.Runtime.Preview.ClientPlaceSource",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantlePreviewLaunchClientSourceTest::RunTest(const FString& Parameters)
{
	using MantlePreviewLaunch::EClientPlaceSource;
	// A preview client plays the manifest's place whatever its join says: it never reaches the backend.
	TestTrue(TEXT("a preview whose join names a place still reads the manifest"), MantlePreviewLaunch::ClientPlaceSource(true, true) == EClientPlaceSource::PreviewManifest);
	TestTrue(TEXT("a preview whose join names nothing reads the manifest"), MantlePreviewLaunch::ClientPlaceSource(true, false) == EClientPlaceSource::PreviewManifest);
	// Every other client is as before: the backend when its join names a place and revision, and nothing otherwise.
	TestTrue(TEXT("a player who joined a place reads the backend"), MantlePreviewLaunch::ClientPlaceSource(false, true) == EClientPlaceSource::Backend);
	TestTrue(TEXT("a client that connected straight to an address has nothing to read"), MantlePreviewLaunch::ClientPlaceSource(false, false) == EClientPlaceSource::None);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
