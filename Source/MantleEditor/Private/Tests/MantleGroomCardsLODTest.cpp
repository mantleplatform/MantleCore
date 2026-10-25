// Copyright Joshua Eger. All Rights Reserved.
#include "MantleCosmeticExport.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Native groom hair: the finest cards LOD that fits the budget, groups summed. Pure counts, no assets.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleGroomCardsLODTest,
	"Mantle.Editor.Hair.GroomCardsLOD",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleGroomCardsLODTest::RunTest(const FString& Parameters)
{
	// Bob Messy shape: LOD0 34k, LOD1 22k, LOD2 14,580 (one group), LOD3 6k.
	TestEqual(TEXT("finest that fits"), FMantleCosmeticExport::ChooseGroomCardsLOD({ {34000}, {22000}, {14580}, {6000} }, 14600), 2);
	// Hair_M_BobMessy's real counts against the 14,000 budget: LOD2 is 4% over, so it is picked and trimmed, not LOD3.
	TestEqual(TEXT("Bob: LOD2 trimmed"), FMantleCosmeticExport::ChooseGroomCardsLOD({ {34093}, {18738}, {14580}, {5328} }, 14000), 2);
	// 15% over is the limit (16,100 for 14,000); one triangle more goes to the next LOD.
	TestEqual(TEXT("exactly 15% over"), FMantleCosmeticExport::ChooseGroomCardsLOD({ {30000}, {16100}, {5000} }, 14000), 1);
	TestEqual(TEXT("just over 15% goes to the next LOD"), FMantleCosmeticExport::ChooseGroomCardsLOD({ {30000}, {16101}, {5000} }, 14000), 2);
	TestEqual(TEXT("nothing within 15%, coarsest"), FMantleCosmeticExport::ChooseGroomCardsLOD({ {34093}, {18738}, {16500} }, 14000), 2);
	TestEqual(TEXT("exact budget fits"), FMantleCosmeticExport::ChooseGroomCardsLOD({ {34000}, {14000}, {6000} }, 14000), 1);
	TestEqual(TEXT("LOD0 fits, so LOD0"), FMantleCosmeticExport::ChooseGroomCardsLOD({ {9000}, {4000} }, 14000), 0);
	// Two groups count together: 10k + 8k = 18k is over, the next LOD 6k + 4k fits.
	TestEqual(TEXT("groups are summed"), FMantleCosmeticExport::ChooseGroomCardsLOD({ {10000, 8000}, {6000, 4000}, {2000, 1000} }, 14000), 1);
	TestEqual(TEXT("each group alone fits but not together"), FMantleCosmeticExport::ChooseGroomCardsLOD({ {9000, 9000}, {3000, 3000} }, 14000), 1);
	// Nothing fits: the coarsest LOD, to be trimmed.
	TestEqual(TEXT("none fits, coarsest"), FMantleCosmeticExport::ChooseGroomCardsLOD({ {34000}, {22000}, {16000} }, 14000), 2);
	// A LOD that draws nothing is never chosen.
	TestEqual(TEXT("empty LOD skipped"), FMantleCosmeticExport::ChooseGroomCardsLOD({ {20000}, {0, 0}, {10000} }, 14000), 2);
	TestEqual(TEXT("all empty"), FMantleCosmeticExport::ChooseGroomCardsLOD({ {0}, {} }, 14000), INDEX_NONE);
	TestEqual(TEXT("no LODs"), FMantleCosmeticExport::ChooseGroomCardsLOD({}, 14000), INDEX_NONE);
	TestEqual(TEXT("no budget takes the finest"), FMantleCosmeticExport::ChooseGroomCardsLOD({ {34000}, {6000} }, 0), 0);
	return true;
}

#endif
