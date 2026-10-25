// Copyright Joshua Eger. All Rights Reserved.
#include "MantleHairRoute.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Which route a hair upload takes (#52) and the words for it. Pure inputs, no assets.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleHairRouteKindsTest,
	"Mantle.Editor.HairRoute.EachKindOfHairGetsItsRoute",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleHairRouteKindsTest::RunTest(const FString& Parameters)
{
	using EKind = FMantleHairRoute::EKind;
	FMantleHairRoute::FInput In;
	In.Budget = 14000;

	In.bGroom = true;
	TestTrue(TEXT("a groom"), FMantleHairRoute::Decide(In).Kind == EKind::Groom);
	In = FMantleHairRoute::FInput();
	In.Budget = 14000;
	In.bSkeletalMesh = true;
	TestTrue(TEXT("skeletal hair"), FMantleHairRoute::Decide(In).Kind == EKind::SkeletalHair);

	// man4-like: many cards, under the limit.
	In = FMantleHairRoute::FInput();
	In.Budget = 14000; In.Triangles = 9000; In.Pieces = 600; In.LargestPieceTriangles = 60;
	FMantleHairRoute::FResult R = FMantleHairRoute::Decide(In);
	TestTrue(TEXT("bought cards"), R.Kind == EKind::BoughtCards);
	TestTrue(TEXT("sized to the head"), R.bFitsToHead && !R.bThinned && R.bSupported);

	// man1-like: 356,344 triangles, 45 welded clumps, none holding 80%.
	In.Triangles = 356344; In.Pieces = 45; In.LargestPieceTriangles = 30000;
	R = FMantleHairRoute::Decide(In);
	TestTrue(TEXT("dense cards"), R.Kind == EKind::DenseCards);
	TestTrue(TEXT("thinned to about 4%"), R.bThinned && R.KeptPercent == 4);
	TestTrue(TEXT("the words give both numbers"), R.Words.Contains(TEXT("356344")) && R.Words.Contains(TEXT("14000")));
	In.bPlacedByHand = true;
	R = FMantleHairRoute::Decide(In);
	TestFalse(TEXT("placed by hand is not fitted"), R.bFitsToHead);
	In.bPlacedByHand = false;

	// One solid sculpted mesh: one piece, thousands of triangles.
	In.Triangles = 20000; In.Pieces = 1; In.LargestPieceTriangles = 20000;
	R = FMantleHairRoute::Decide(In);
	TestTrue(TEXT("solid sculpted hair"), R.Kind == EKind::SculptedSolid);
	TestFalse(TEXT("is not supported yet"), R.bSupported);
	TestTrue(TEXT("and says it needs a sample"), R.Words.Contains(TEXT("sample")));
	// A tiny single card is not a sculpted head of hair.
	In.Triangles = 40; In.LargestPieceTriangles = 40;
	TestTrue(TEXT("a handful of triangles stays cards"), FMantleHairRoute::Decide(In).Kind == EKind::BoughtCards);
	// MetaHuman cards are kept as they are, even as one piece.
	In.Triangles = 20000; In.LargestPieceTriangles = 20000; In.bMetaHumanOrigin = true;
	R = FMantleHairRoute::Decide(In);
	TestTrue(TEXT("MetaHuman cards"), R.Kind == EKind::MetaHumanCards && !R.bFitsToHead);
	return true;
}

#endif
